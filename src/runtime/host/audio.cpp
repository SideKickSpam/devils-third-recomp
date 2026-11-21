#include "audio.h"

#include "../log.h"
#include "../settings.h"
#include "window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace host::audio
{
namespace
{
std::mutex gMutex;
bool gMuted = false;
std::string gDumpPath;
std::atomic<int> gRate{ 0 };
SDL_AudioStream* gStream = nullptr;
std::atomic<bool> gOpen{ false };

uint64_t gPushed = 0, gUnderruns = 0, gReportedUnderruns = 0;  // frames pushed; times the queue had run dry
FILE* gDump = nullptr;
uint64_t gDumpFrames = 0;

void WriteWavHeader(FILE* f, int rate, uint64_t frames)
{
    const uint32_t dataBytes = uint32_t(std::min<uint64_t>(frames * Channels * 2, 0xFFFFFFF0u));
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };  // WAV is little-endian, as are our hosts
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f), u32(36 + dataBytes), fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(Channels), u32(uint32_t(rate)), u32(uint32_t(rate) * Channels * 2), u16(Channels * 2), u16(16);
    fwrite("data", 1, 4, f), u32(dataBytes);
}
} // namespace

void SetMuted(bool muted) { gMuted = muted; }
void SetDumpPath(const std::string& path) { gDumpPath = path; }

void SetSourceRate(int rate)
{
    std::lock_guard lock(gMutex);
    gRate = rate;
    if (!gDumpPath.empty() && !gDump)
    {
        gDump = fopen(gDumpPath.c_str(), "wb");
        if (gDump)
            WriteWavHeader(gDump, rate, 0);
        else
            dtlog::Important("[audio] cannot write %s\n", gDumpPath.c_str());
    }
}

void OpenDevice()
{
    // The mixer may not have started yet; it produces 32 kHz (AX's default renderer rate) unless told otherwise.
    const int rate = gRate ? gRate.load() : 32000;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        dtlog::Important("[audio] no sound: %s\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{ SDL_AUDIO_F32, Channels, rate };
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s)
    {
        dtlog::Important("[audio] no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec device{};
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s), &device, nullptr);
    dtlog::Important("[audio] playing %d Hz stereo on %s (%d Hz, %d channels, driver %s)\n", rate,
        SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)), device.freq, device.channels, SDL_GetCurrentAudioDriver());
    if (gMuted)
        dtlog::Important("[audio] muted\n");  // played silently: the device still paces the mixer, as in normal play
    SDL_ResumeAudioStreamDevice(s);
    {
        std::lock_guard lock(gMutex);
        gStream = s;
        gOpen = true;
    }
    UpdateVolume();
}

void UpdateVolume()
{
    if (!gOpen)
        return;
    // audio.mute_unfocused (when the window is in the background) and --mute; volumes are the game's own options.
    float gain = 1.0f;
    if (gMuted || (settings::GetBool("audio.mute_unfocused", false) && !host::HasFocus()))
        gain = 0;
    SDL_SetAudioStreamGain(gStream, gain);
}

bool DeviceOpen() { return gOpen; }

size_t QueuedFrames()
{
    if (!gOpen)
        return 0;
    const int bytes = SDL_GetAudioStreamQueued(gStream);
    return bytes > 0 ? size_t(bytes) / (sizeof(float) * Channels) : 0;
}

void Push(const float* samples, size_t frames)
{
    if (gOpen)
    {
        // An empty queue once playback is under way means the device ran dry (an audible gap).
        if (gPushed > uint64_t(gRate) && SDL_GetAudioStreamQueued(gStream) == 0 && ++gUnderruns >= gReportedUnderruns * 2 + 1)
        {
            gReportedUnderruns = gUnderruns;
            dtlog::Important("[audio] %llu underruns so far (%.1f s in)\n", (unsigned long long)gUnderruns,
                double(gPushed) / gRate);
        }
        gPushed += frames;
        SDL_PutAudioStreamData(gStream, samples, int(frames * Channels * sizeof(float)));
    }
    std::lock_guard lock(gMutex);
    if (gDump)
    {
        std::vector<int16_t> pcm(frames * Channels);
        for (size_t i = 0; i < pcm.size(); i++)
            pcm[i] = int16_t(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        fwrite(pcm.data(), 2, pcm.size(), gDump);
        gDumpFrames += frames;
    }
}

void Close()
{
    std::lock_guard lock(gMutex);
    if (gOpen && gRate)
        dtlog::Important("[audio] played %.1f s with %llu underruns\n", double(gPushed) / gRate, (unsigned long long)gUnderruns);
    if (gDump)
    {
        fflush(gDump);
        fseek(gDump, 0, SEEK_SET);
        WriteWavHeader(gDump, gRate, gDumpFrames);
        fclose(gDump);
        gDump = nullptr;
        dtlog::Important("[audio] wrote %s (%.1f s)\n", gDumpPath.c_str(), gRate ? double(gDumpFrames) / gRate : 0.0);
    }
}
} // namespace host::audio
