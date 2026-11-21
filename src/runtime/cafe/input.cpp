// Input libraries: vpad (GamePad), padscore (Wii Remote / Pro Controller via WPAD/KPAD), nsyskbd (USB keyboard).
// The GamePad reports the host keyboard and gamepads (host/input.*); nothing else is ever connected.
#include "../host/input.h"
#include "../imports.h"
#include "gx2.h"

#include <bit>
#include <cstring>

namespace cafe::input
{
namespace
{
// padscore
void WPADEnableURCC(int32_t enable) { (void)enable; }
void WPADEnableWiiRemote(int32_t enable) { (void)enable; }
void KPADInit() {}
void KPADReset() {}

// vpad: VPADSetButtonProcMode(channel, mode) -> success flag
int32_t VPADSetButtonProcMode(int32_t channel, uint8_t mode) { (void)channel, (void)mode; return 1; }

// nsyskbd: KBDInit(void* memory, connectCb, disconnectCb, keyCb) -> 0 on success
int32_t KBDInit(uint32_t memory, uint32_t connectCb, uint32_t disconnectCb, uint32_t keyCb)
{
    (void)memory, (void)connectCb, (void)disconnectCb, (void)keyCb;
    return 0;  // no keyboard will ever connect
}

// ---- vpad: the GamePad is always connected and reports host input ----
// VPADStatus (0xAC bytes): hold, trigger (newly pressed), release (u32 each), leftStick and rightStick (float x, y),
// then motion, touch and battery data, which stay zero.
constexpr uint32_t VPADStatusSize = 0xAC;
uint32_t gLastHold = 0;
int32_t VPADRead(int32_t channel, uint32_t buffers, uint32_t count, uint32_t outError)
{
    if (channel != 0 || count == 0)
    {
        if (outError)
            guest::Write32(outError, uint32_t(-2));  // VPAD_READ_INVALID_CONTROLLER
        return 0;
    }
    const host::input::GamePad pad = host::input::Read(cafe::gx2::FrameCount());
    memset(guest::Ptr(buffers), 0, VPADStatusSize);
    guest::Write32(buffers + 0x00, pad.buttons);
    guest::Write32(buffers + 0x04, pad.buttons & ~gLastHold);
    guest::Write32(buffers + 0x08, gLastHold & ~pad.buttons);
    guest::Write32(buffers + 0x0C, std::bit_cast<uint32_t>(pad.leftX));
    guest::Write32(buffers + 0x10, std::bit_cast<uint32_t>(pad.leftY));
    guest::Write32(buffers + 0x14, std::bit_cast<uint32_t>(pad.rightX));
    guest::Write32(buffers + 0x18, std::bit_cast<uint32_t>(pad.rightY));
    gLastHold = pad.buttons;
    if (outError)
        guest::Write32(outError, 0);  // VPAD_READ_SUCCESS
    return 1;
}
void VPADGetTPCalibratedPoint(int32_t channel, uint32_t out, uint32_t in)
{
    (void)channel;
    memmove(guest::Ptr(out), guest::Ptr(in), 8);  // VPADTouchData: x, y, touched, validity (u16 each)
}
void VPADInitGyroDirReviseParam(int32_t channel) { (void)channel; }
// The GamePad's rumble: `pattern` is `length` bits (most significant first), each a 1/120 s slice of motor on or off.
// PC controllers get an SDL rumble as strong as the share of set bits, for the pattern's duration. The game calls this
// only when its own vibration option is on.
int32_t VPADControlMotor(int32_t channel, uint32_t pattern, uint8_t length)
{
    (void)channel;
    uint32_t on = 0;
    for (uint32_t i = 0; i < length; i++)
        on += (guest::Read8(pattern + i / 8) >> (7 - i % 8)) & 1;
    const float strength = length ? float(on) / float(length) : 0.0f;
    host::input::Rumble(strength, length * 1000u / 120u);
    return 0;
}

// ---- padscore: no Wii Remotes or Pro Controllers are connected ----
constexpr int32_t WPADNoController = -1, KPADInvalidController = -2;
int32_t KPADReadEx(int32_t channel, uint32_t data, uint32_t size, uint32_t outError)
{
    (void)channel, (void)data, (void)size;
    if (outError)
        guest::Write32(outError, uint32_t(KPADInvalidController));
    return 0;
}
int32_t WPADGetStatus() { return 1; }  // library initialised
uint8_t WPADGetBatteryLevel(int32_t channel) { (void)channel; return 0; }
bool WPADIsMotorEnabled() { return false; }
void WPADControlMotor(int32_t channel, uint32_t command) { (void)channel, (void)command; }
int32_t WPADControlSpeaker(int32_t channel, uint32_t command, uint32_t callback)
{
    (void)channel, (void)command, (void)callback;
    return WPADNoController;
}
void WPADSetAutoSleepTime(uint8_t minutes) { (void)minutes; }
void WPADResetAutoSleepTimeCount(int32_t channel) { (void)channel; }

// ---- nsyskbd: no keyboard attached ----
int32_t KBDGetChannelStatus(int32_t channel, uint32_t outStatus)
{
    (void)channel;
    if (outStatus)
        guest::Write32(outStatus, 0);  // KBD_CHANNEL_STATUS_DISCONNECTED
    return 0;
}
int32_t KBDGetKey(int32_t channel, uint32_t outEvent) { (void)channel, (void)outEvent; return -1; }
int32_t KBDGetModState(int32_t channel, uint32_t outState)
{
    (void)channel;
    if (outState)
        guest::Write32(outState, 0);
    return 0;
}
int32_t KBDSetModState(int32_t channel, uint32_t state) { (void)channel, (void)state; return 0; }
int32_t KBDSetLedsRetry(int32_t channel, uint8_t leds) { (void)channel, (void)leds; return 0; }
int32_t KBDSetCountry(int32_t channel, uint32_t country) { (void)channel, (void)country; return 0; }
} // namespace

CAFE_IMPORT(vpad, "VPADRead", VPADRead);
CAFE_IMPORT(vpad, "VPADGetTPCalibratedPoint", VPADGetTPCalibratedPoint);
CAFE_IMPORT(vpad, "VPADInitGyroDirReviseParam", VPADInitGyroDirReviseParam);
CAFE_IMPORT(vpad, "VPADControlMotor", VPADControlMotor);
CAFE_IMPORT(padscore, "KPADReadEx", KPADReadEx);
CAFE_IMPORT(padscore, "WPADGetStatus", WPADGetStatus);
CAFE_IMPORT(padscore, "WPADGetBatteryLevel", WPADGetBatteryLevel);
CAFE_IMPORT(padscore, "WPADIsMotorEnabled", WPADIsMotorEnabled);
CAFE_IMPORT(padscore, "WPADControlMotor", WPADControlMotor);
CAFE_IMPORT(padscore, "WPADControlSpeaker", WPADControlSpeaker);
CAFE_IMPORT(padscore, "WPADSetAutoSleepTime", WPADSetAutoSleepTime);
CAFE_IMPORT(padscore, "WPADResetAutoSleepTimeCount", WPADResetAutoSleepTimeCount);
CAFE_IMPORT(nsyskbd, "KBDGetChannelStatus", KBDGetChannelStatus);
CAFE_IMPORT(nsyskbd, "KBDGetKey", KBDGetKey);
CAFE_IMPORT(nsyskbd, "KBDGetModState", KBDGetModState);
CAFE_IMPORT(nsyskbd, "KBDSetModState", KBDSetModState);
CAFE_IMPORT(nsyskbd, "KBDSetLedsRetry", KBDSetLedsRetry);
CAFE_IMPORT(nsyskbd, "KBDSetCountry", KBDSetCountry);
CAFE_IMPORT(padscore, "WPADEnableURCC", WPADEnableURCC);
CAFE_IMPORT(padscore, "WPADEnableWiiRemote", WPADEnableWiiRemote);
CAFE_IMPORT(padscore, "KPADInit", KPADInit);
CAFE_IMPORT(padscore, "KPADReset", KPADReset);
CAFE_IMPORT(vpad, "VPADSetButtonProcMode", VPADSetButtonProcMode);
CAFE_IMPORT(nsyskbd, "KBDInit", KBDInit);
} // namespace cafe::input
