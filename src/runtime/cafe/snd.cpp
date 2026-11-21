// snd_core (AX) and snd_user (MIX). A host thread runs the AX frame: every 3 ms it mixes the playing voices
// into 96 stereo samples at 32 kHz (AX's default renderer rate, which voice sample-rate ratios are relative to)
// and hands them to host::audio, then runs the registered app frame callbacks on the "AX" guest context.
// Wwise and Bink pace their mixing on those callbacks and on the voices' play positions. With a sound device
// the frames follow the device's consumption; without one, the wall clock.
//
// Voices are mono PCM (16- or 8-bit, big-endian). Each has a volume (AXSetVoiceVe) and per-device mixes
// (AXSetVoiceDeviceMix); the TV mix's main bus gives its six channels (front L/R, surround L/R, centre, LFE),
// which are folded down to stereo. The GamePad and remote mixes and the auxiliary (effect) buses are not played.
// The game never calls AXSetVoiceAdpcm, so ADPCM voices cannot occur.
#include "../host/audio.h"
#include "../imports.h"
#include "../log.h"
#include "../platform.h"
#include "../trace.h"
#include "coreinit.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace cafe::snd
{
namespace
{
constexpr uint32_t SampleRate = 32000;
constexpr uint32_t FrameUs = 3000;                             // one AX frame
constexpr uint32_t SamplesPerFrame = SampleRate * FrameUs / 1000000;  // 96
// With a device: keep about this much mixed audio queued (latency against underruns).
constexpr size_t TargetQueuedFrames = SampleRate / 25;  // 40 ms
// AXVoiceOffsets.dataType
constexpr uint16_t FormatAdpcm = 0x00, FormatPcm16 = 0x0A, FormatPcm8 = 0x19;
constexpr uint32_t TvChannels = 6;
constexpr uint32_t AXVoiceSize = 0x5C;

// AXVoice fields (guest): index, state, volume, renderer, link, cbNext, priority, callback, userContext.
constexpr uint32_t OffIndex = 0x00, OffState = 0x04, OffPriority = 0x1C, OffCallback = 0x20, OffUserContext = 0x24;

struct Voice
{
    uint32_t guest = 0;
    bool playing = false;
    double ratio = 1.0;
    double position = 0;  // in samples
    // AXVoiceOffsets: dataType (u16), loopingEnabled (u16), loopOffset, endOffset, currentOffset, data.
    uint16_t dataType = 0, looping = 0;
    uint32_t loopOffset = 0, endOffset = 0, data = 0;
    uint32_t srcType = 0;  // 0 none (nearest sample), otherwise interpolated
    float volume = 0;      // AXSetVoiceVe, 1.0 = 0x8000
    float tv[TvChannels] = {};  // TV device mix, main bus
};

// One sample of a voice at integer offset i (absolute, in samples from data).
float Sample(const Voice& v, uint64_t i)
{
    if (v.dataType == FormatPcm16)
        return int16_t(guest::Read16(uint32_t(v.data + i * 2))) / 32768.0f;
    if (v.dataType == FormatPcm8)
        return int8_t(guest::Read8(uint32_t(v.data + i))) / 128.0f;
    return 0;
}

std::mutex gMutex;
std::map<uint32_t, Voice> gVoices;  // by guest AXVoice address
std::vector<uint32_t> gFrameCallbacks;
int gConfigLogs = 0;  // voice set-ups logged so far (the first few, to show formats and mixes)
std::atomic<int> gProtect{ 0 };
bool gInit = false;
uint32_t gNextVoiceIndex = 0;
uint32_t gDrcVsMode = 0;
bool gThreadStarted = false;

// Mixes one frame of a playing voice into `out` (interleaved stereo) and advances it, looping or stopping at
// its end as AX does.
void MixVoice(Voice& v, float* out)
{
    constexpr float Half = 0.70710678f;
    const float left = v.volume * (v.tv[0] + Half * (v.tv[2] + v.tv[4]));
    const float right = v.volume * (v.tv[1] + Half * (v.tv[3] + v.tv[4]));
    const bool audible = (left != 0 || right != 0) && (v.dataType == FormatPcm16 || v.dataType == FormatPcm8);
    for (uint32_t i = 0; i < SamplesPerFrame; i++)
    {
        if (audible)
        {
            const uint64_t at = uint64_t(v.position);
            float s = Sample(v, at);
            if (v.srcType != 0)
            {
                // Linear interpolation towards the next sample (the loop start after the end, if looping).
                const uint64_t next = at + 1 <= v.endOffset ? at + 1 : v.looping ? v.loopOffset : at;
                s += (Sample(v, next) - s) * float(v.position - double(at));
            }
            out[i * 2] += s * left;
            out[i * 2 + 1] += s * right;
        }
        v.position += v.ratio;
        if (v.position >= double(v.endOffset) + 1)
        {
            if (v.looping && v.endOffset >= v.loopOffset)
                v.position = v.loopOffset + std::fmod(v.position - double(v.endOffset) - 1, double(v.endOffset - v.loopOffset + 1));
            else
            {
                v.position = v.endOffset;
                v.playing = false;
                guest::Write32(v.guest + OffState, 0);
                return;
            }
        }
    }
}

void FrameThread()
{
    static PPCContext ctx{};
    const uint32_t stackSize = 256 * 1024;
    const uint32_t stack = guest::SystemAlloc(stackSize, 16);
    ctx.r[1].u32 = stack + stackSize - 16;
    guest::SetCurrentContext(&ctx);
    trace::RegisterThread("AX", &ctx);
    host::audio::SetSourceRate(int(SampleRate));
    auto next = std::chrono::steady_clock::now();
    float mix[SamplesPerFrame * 2];
    for (;;)
    {
        if (host::audio::DeviceOpen())
        {
            // The device's clock paces the frames: mix the next one once the queue runs low.
            while (host::audio::QueuedFrames() > TargetQueuedFrames)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            next = std::chrono::steady_clock::now();
        }
        else
        {
            next += std::chrono::microseconds(FrameUs);
            std::this_thread::sleep_until(next);
        }
        std::vector<uint32_t> callbacks;
        {
            std::lock_guard lock(gMutex);
            std::fill(std::begin(mix), std::end(mix), 0.0f);
            for (auto& [addr, v] : gVoices)
                if (v.playing)
                    MixVoice(v, mix);
            callbacks = gFrameCallbacks;
        }
        host::audio::Push(mix, SamplesPerFrame);
        for (uint32_t cb : callbacks)
            guest::Call(cb);
    }
}

void StartFrameThread()
{
    if (!gThreadStarted)
    {
        gThreadStarted = true;
        platform::StartThread(FrameThread);
    }
}

// ---- library ----
void AXInit()
{
    std::lock_guard lock(gMutex);
    gInit = true;
}
bool AXIsInit() { return gInit; }
void AXQuit()
{
    std::lock_guard lock(gMutex);
    gInit = false;
}
void AXUserBegin() { gProtect++; }
void AXUserEnd() { gProtect--; }
bool AXUserIsProtected() { return gProtect > 0; }
void AXSetDefaultMixerSelect(uint32_t mixer) { (void)mixer; }
int32_t AXSetDRCVSMode(uint32_t mode) { gDrcVsMode = mode; return 0; }
int32_t AXGetDRCVSMode(uint32_t out)
{
    if (out)
        guest::Write32(out, gDrcVsMode);
    return 0;
}
// AXGetDeviceMode(device, u32* mode): 0 = stereo.
int32_t AXGetDeviceMode(uint32_t device, uint32_t out)
{
    (void)device;
    if (out)
        guest::Write32(out, 0);
    return 0;
}

int32_t AXRegisterAppFrameCallback(uint32_t callback)
{
    {
        std::lock_guard lock(gMutex);
        gFrameCallbacks.push_back(callback);
    }
    StartFrameThread();
    return 0;
}
int32_t AXDeregisterAppFrameCallback(uint32_t callback)
{
    std::lock_guard lock(gMutex);
    std::erase(gFrameCallbacks, callback);
    return 0;
}

// ---- voices ----
uint32_t Acquire(uint32_t priority, uint32_t callback, uint32_t userContext)
{
    const uint32_t v = guest::SystemAlloc(AXVoiceSize, 32);
    std::lock_guard lock(gMutex);
    guest::Write32(v + OffIndex, gNextVoiceIndex++);
    guest::Write32(v + OffPriority, priority);
    guest::Write32(v + OffCallback, callback);
    guest::Write32(v + OffUserContext, userContext);
    gVoices[v].guest = v;
    return v;
}
uint32_t AXAcquireVoice(uint32_t priority, uint32_t callback, uint32_t userContext) { return Acquire(priority, callback, userContext); }
uint32_t AXAcquireVoiceEx(uint32_t priority, uint32_t callbackEx, uint32_t userContext) { return Acquire(priority, callbackEx, userContext); }
void AXFreeVoice(uint32_t voice)
{
    std::lock_guard lock(gMutex);
    gVoices.erase(voice);
}

void AXSetVoiceState(uint32_t voice, uint32_t state)
{
    std::lock_guard lock(gMutex);
    if (auto it = gVoices.find(voice); it != gVoices.end())
    {
        it->second.playing = state != 0;
        guest::Write32(voice + OffState, state);
    }
}

void AXSetVoiceOffsets(uint32_t voice, uint32_t offsets)
{
    std::lock_guard lock(gMutex);
    auto it = gVoices.find(voice);
    if (it == gVoices.end())
        return;
    Voice& v = it->second;
    v.dataType = guest::Read16(offsets + 0x00);
    v.looping = guest::Read16(offsets + 0x02);
    v.loopOffset = guest::Read32(offsets + 0x04);
    v.endOffset = guest::Read32(offsets + 0x08);
    v.position = guest::Read32(offsets + 0x0C);
    v.data = guest::Read32(offsets + 0x10);
    if (v.dataType != FormatPcm16 && v.dataType != FormatPcm8)
        dtlog::Important("[ax] voice 0x%08X uses data format 0x%X, which is not played\n", voice, v.dataType);
    if (gConfigLogs < 12)
    {
        gConfigLogs++;
        dtlog::Write("[ax] voice 0x%08X offsets: format 0x%X loop %u [%u, %u] current %u data 0x%08X ratio %g src %u\n",
            voice, v.dataType, v.looping, v.loopOffset, v.endOffset, uint32_t(v.position), v.data, v.ratio, v.srcType);
    }
}

void AXGetVoiceOffsets(uint32_t voice, uint32_t out)
{
    std::lock_guard lock(gMutex);
    auto it = gVoices.find(voice);
    if (it == gVoices.end())
        return;
    const Voice& v = it->second;
    guest::Write16(out + 0x00, v.dataType);
    guest::Write16(out + 0x02, v.looping);
    guest::Write32(out + 0x04, v.loopOffset);
    guest::Write32(out + 0x08, v.endOffset);
    guest::Write32(out + 0x0C, uint32_t(v.position));
    guest::Write32(out + 0x10, v.data);
}

// AXSetVoiceSrcRatio(voice, float ratio)
void AXSetVoiceSrcRatio(uint32_t voice, float ratio)
{
    std::lock_guard lock(gMutex);
    if (auto it = gVoices.find(voice); it != gVoices.end())
        it->second.ratio = ratio > 0 ? ratio : 1.0;
}

// AXSetVoiceType: streaming or not, which only matters to ADPCM loop context.
void AXSetVoiceType(uint32_t voice, uint32_t type) { (void)voice, (void)type; }
// AXSetVoiceSrcType(voice, type): 0 none, 1 linear, 2-4 4-tap filters (played as linear).
void AXSetVoiceSrcType(uint32_t voice, uint32_t type)
{
    std::lock_guard lock(gMutex);
    if (auto it = gVoices.find(voice); it != gVoices.end())
        it->second.srcType = type;
}
// AXSetVoiceVe(voice, AXVoiceVeData*): u16 volume (0x8000 = 1.0), s16 per-sample delta (ramps; not modelled).
void AXSetVoiceVe(uint32_t voice, uint32_t ve)
{
    std::lock_guard lock(gMutex);
    if (auto it = gVoices.find(voice); it != gVoices.end())
        it->second.volume = guest::Read16(ve) / 32768.0f;
}
// AXSetVoiceDeviceMix(voice, device, id, AXVoiceDeviceMixData*): per channel, four buses of (u16 volume, s16
// delta), 16 bytes a channel; device 0 is the TV (6 channels), 1 the GamePad, 2 the remotes.
int32_t AXSetVoiceDeviceMix(uint32_t voice, uint32_t device, uint32_t id, uint32_t mix)
{
    std::lock_guard lock(gMutex);
    auto it = gVoices.find(voice);
    if (it == gVoices.end() || device != 0 || id != 0)
        return 0;
    for (uint32_t c = 0; c < TvChannels; c++)
        it->second.tv[c] = guest::Read16(mix + c * 16) / 32768.0f;
    if (gConfigLogs < 12)
    {
        gConfigLogs++;
        std::string raw;
        for (uint32_t i = 0; i < TvChannels * 8; i++)
            raw += (i % 8 ? " " : " | ") + std::to_string(guest::Read16(mix + i * 2));
        dtlog::Write("[ax] voice 0x%08X TV mix:%s\n", voice, raw.c_str());
    }
    return 0;
}
void AXSetVoiceRmtOn(uint32_t voice, uint16_t on) { (void)voice, (void)on; }

// ---- snd_user ----
void MIXInit() {}
void MIXQuit() {}
} // namespace

CAFE_IMPORT(snd_core, "AXInit", AXInit);
CAFE_IMPORT(snd_core, "AXIsInit", AXIsInit);
CAFE_IMPORT(snd_core, "AXQuit", AXQuit);
CAFE_IMPORT(snd_core, "AXUserBegin", AXUserBegin);
CAFE_IMPORT(snd_core, "AXUserEnd", AXUserEnd);
CAFE_IMPORT(snd_core, "AXUserIsProtected", AXUserIsProtected);
CAFE_IMPORT(snd_core, "AXSetDefaultMixerSelect", AXSetDefaultMixerSelect);
CAFE_IMPORT(snd_core, "AXSetDRCVSMode", AXSetDRCVSMode);
CAFE_IMPORT(snd_core, "AXGetDRCVSMode", AXGetDRCVSMode);
CAFE_IMPORT(snd_core, "AXGetDeviceMode", AXGetDeviceMode);
CAFE_IMPORT(snd_core, "AXRegisterAppFrameCallback", AXRegisterAppFrameCallback);
CAFE_IMPORT(snd_core, "AXDeregisterAppFrameCallback", AXDeregisterAppFrameCallback);
CAFE_IMPORT(snd_core, "AXAcquireVoice", AXAcquireVoice);
CAFE_IMPORT(snd_core, "AXAcquireVoiceEx", AXAcquireVoiceEx);
CAFE_IMPORT(snd_core, "AXFreeVoice", AXFreeVoice);
CAFE_IMPORT(snd_core, "AXSetVoiceState", AXSetVoiceState);
CAFE_IMPORT(snd_core, "AXSetVoiceOffsets", AXSetVoiceOffsets);
CAFE_IMPORT(snd_core, "AXGetVoiceOffsets", AXGetVoiceOffsets);
CAFE_IMPORT(snd_core, "AXSetVoiceSrcRatio", AXSetVoiceSrcRatio);
CAFE_IMPORT(snd_core, "AXSetVoiceType", AXSetVoiceType);
CAFE_IMPORT(snd_core, "AXSetVoiceSrcType", AXSetVoiceSrcType);
CAFE_IMPORT(snd_core, "AXSetVoiceVe", AXSetVoiceVe);
CAFE_IMPORT(snd_core, "AXSetVoiceDeviceMix", AXSetVoiceDeviceMix);
CAFE_IMPORT(snd_core, "AXSetVoiceRmtOn", AXSetVoiceRmtOn);
CAFE_IMPORT(snd_user, "MIXInit", MIXInit);
CAFE_IMPORT(snd_user, "MIXQuit", MIXQuit);
} // namespace cafe::snd
