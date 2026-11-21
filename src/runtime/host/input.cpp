#include "input.h"
#include "../game_ui.h"

#include "../cafe/gx2.h"
#include "../log.h"
#include "../settings.h"
#include "bindings.h"
#include "menu.h"
#include "window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>
#include <string>
#include <cstdio>

namespace host::input
{
namespace
{
std::mutex gMutex;
std::set<uint32_t> gHeldInputs;                      // keyboard keys and mouse buttons held (bindings::Make)
std::map<uint32_t, uint64_t> gWheelUntil;            // a wheel notch holds its binding until this frame
std::atomic<bool> gPressStart{ false };              // the title's press-START screen (SetPressStartScreen)
float gMouseX = 0, gMouseY = 0;                       // mouse motion since the last read, in pixels
bool gCaptured = false;                               // relative mouse mode: the mouse drives the right stick
float gPointerWX = 0, gPointerWY = 0;                 // the menus' pointer, in window pixels (GetPointer)
bool gPointerSeen = false, gPointerDown = false;
uint64_t gPointerSerial = 0;
std::atomic<bool> gThrowEscape{ false };  // SetThrowEscape
int gPointerWheel = 0;
// Mouse motion -> right stick: pixels per frame for full deflection.
constexpr float MousePixelsForFullTilt = 12.0f;
// Gameplay mouse look: rotation units (65536 a turn) per pixel at controls.mouse_sensitivity 1.0 (0.08 degrees).
constexpr float MouseUnitsPerPixel = 15.0f;

// The game's right-stick curve, measured (DT_TEST_STICK=sweep, at its own LookRightScale 240): the horizontal turn rate
// in rotation units per second for a stick value. A dead zone to 0.2, slow and nearly linear to 0.7 (2,400 units/s,
// 13 degrees a second), then fast to 1.0 (24,000 units/s, 132 degrees a second): fine for a thumb, mud for a mouse.
constexpr std::pair<float, float> StickCurve[] = { { 0.2f, 0 }, { 0.25f, 360 }, { 0.3f, 840 }, { 0.4f, 1200 }, { 0.5f, 1560 },
    { 0.6f, 1920 }, { 0.7f, 2400 }, { 0.8f, 7200 }, { 0.9f, 12000 }, { 1.0f, 24000 } };

// The stick value whose turn rate on that curve is `rate` (signed; beyond the curve's top it is full tilt). Rates below
// the slowest the game turns (360 units/s at 0.25; less falls in its dead zone) are dithered over frames: that slowest
// turn on some frames and none on the others, so the average is right (`carry` keeps the remainder, per axis).
float StickForTurnRate(float rate, float& carry)
{
    const float r = std::abs(rate);
    const float slowest = StickCurve[1].second;
    if (r < slowest)
    {
        carry += rate / slowest;
        if (std::abs(carry) >= 1.0f)
        {
            const float sign = carry > 0 ? 1.0f : -1.0f;
            carry -= sign;
            return sign * StickCurve[1].first;
        }
        return 0;
    }
    carry = 0;
    float stick = 1.0f;
    for (size_t i = 1; i < std::size(StickCurve); i++)
        if (r <= StickCurve[i].second)
        {
            const auto [s0, r0] = StickCurve[i - 1];
            const auto [s1, r1] = StickCurve[i];
            stick = s0 + (s1 - s0) * (r - r0) / (r1 - r0);
            break;
        }
    return std::copysign(stick, rate);
}
std::map<SDL_JoystickID, SDL_Gamepad*> gPads;

struct Press
{
    uint64_t frame, frames;
    uint32_t buttons;
    float leftX, leftY, rightX, rightY;  // stick deflection held with the buttons
};
std::vector<Press> gScript;
struct PadPress
{
    uint64_t frame, frames;
    std::vector<SDL_GamepadButton> buttons;
};
std::vector<PadPress> gPadScript;  // --pad-press, on a virtual controller
SDL_Joystick* gVirtualPad = nullptr;
std::atomic<uint32_t> gInjected{ 0 };       // Inject: buttons and the frame they stop at
std::atomic<uint64_t> gInjectedUntil{ 0 };
std::atomic<uint32_t> gGameButtons{ 0 };  // what the game was given at the last Read, injected presses included
std::atomic<uint32_t> gHeld{ 0 };           // the buttons held at the last Read, before masking
std::atomic<uint32_t> gMasked{ 0 };         // MaskHeld: hidden from the game until released
uint32_t gScriptedNow = 0;
std::atomic<int> gActive{ int(Device::Keyboard) };
std::atomic<int> gLastPadType{ int(SDL_GAMEPAD_TYPE_STANDARD) };
std::atomic<bool> gPadUsed{ false };
bool gWasGameplay = false;
std::set<std::pair<SDL_JoystickID, uint32_t>> gSuppressed;  // held across a gameplay/menu switch: (pad or 0, input)

void SetActive(Device d)
{
    if (gActive.exchange(int(d)) != int(d))
        dtlog::Important("[input] active device: %s\n", DeviceName(d));
}  // the --press buttons held at the last Read (they drive the settings menu too, for tests)

bool PadInputDown(SDL_Gamepad* pad, uint32_t input)
{
    if (bindings::KindOf(input) == bindings::PadButton)
        return SDL_GetGamepadButton(pad, SDL_GamepadButton(bindings::CodeOf(input)));
    if (bindings::KindOf(input) == bindings::PadTrigger)
        return SDL_GetGamepadAxis(pad, SDL_GamepadAxis(bindings::CodeOf(input))) > 16000;
    return false;
}

float Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool invert, float deadZone)
{
    float v = SDL_GetGamepadAxis(pad, axis) / 32767.0f;
    // Dead zone (controls.dead_zone), rescaled so the stick still reaches full deflection.
    v = std::abs(v) < deadZone ? 0 : (v - std::copysign(deadZone, v)) / (1.0f - deadZone);
    return std::clamp(invert ? -v : v, -1.0f, 1.0f);
}

// What a keyboard or mouse input presses in a table: GamePad buttons, and (high bits) keyboard stick directions.
uint64_t KeyEffect(const bindings::Table& table, uint32_t input)
{
    const auto& targets = bindings::Targets();
    uint64_t effect = 0;
    for (size_t i = 0; i < targets.size(); i++)
        for (uint32_t k : table[i].keys)
            if (k == input)
                effect |= targets[i].enbaku ? 1ull << 40 : targets[i].vpad | uint64_t(targets[i].stick) << 32;
    return effect;
}

uint32_t PadEffect(const bindings::Table& table, uint32_t input, Device device)
{
    uint32_t effect = 0;
    for (size_t i = 0; i < bindings::Targets().size(); i++)
        if (bindings::PadInput(table, i, device) == input)
            effect |= bindings::Targets()[i].enbaku ? 0x80000000u : bindings::Targets()[i].vpad;
    return effect;
}

// Every controller button and trigger, as bindings inputs.
const std::vector<uint32_t>& AllPadInputs()
{
    static const std::vector<uint32_t> all = [] {
        std::vector<uint32_t> v;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            v.push_back(bindings::Make(bindings::PadButton, uint32_t(b)));
        v.push_back(bindings::Make(bindings::PadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        v.push_back(bindings::Make(bindings::PadTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        return v;
    }();
    return all;
}

// --pad-press: a virtual Xbox-style controller, attached on first use, its buttons set for this frame (the window
// thread's event pump applies them).
void DriveVirtualPad(uint64_t frame)
{
    if (!gVirtualPad)
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x045E, desc.product_id = 0x028E;  // an Xbox 360 controller
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT, desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "dt-port virtual controller";
        if (const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc))
            gVirtualPad = SDL_OpenJoystick(id);
        if (!gVirtualPad)
        {
            dtlog::Important("[input] no virtual controller: %s\n", SDL_GetError());
            gPadScript.clear();
            return;
        }
    }
    bool down[SDL_GAMEPAD_BUTTON_COUNT] = {};
    for (const PadPress& s : gPadScript)
        if (frame >= s.frame && frame < s.frame + s.frames)
            for (SDL_GamepadButton b : s.buttons)
                down[b] = true;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        SDL_SetJoystickVirtualButton(gVirtualPad, b, down[b]);
}

// The settings menu from a controller: the Guide (Home) button, or holding Back and clicking the right stick.
void CheckMenuCombo(uint32_t buttons)
{
    static bool wasCombo = false;
    const bool combo = (buttons & VpadHome) || ((buttons & VpadMinus) && (buttons & VpadStickR));
    if (combo && !wasCombo)
        menu::Toggle((buttons & VpadHome) ? "Guide button" : "Back + right stick");
    wasCombo = combo;
}
} // namespace

void HandleEvent(const SDL_Event& e)
{
    std::lock_guard lock(gMutex);
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        // Escape in gameplay opens the game's pause menu (START), whatever START is bound to, and frees the mouse; in
        // menus it stays Back (bindings::Menus). It is not held as an input, so the pause menu does not see it as Back.
        if (e.key.key == SDLK_ESCAPE && bindings::InGameplay())
        {
            if (down && !e.key.repeat)
            {
                Inject(VpadPlus, 4);
                if (gCaptured)
                    SDL_SetWindowRelativeMouseMode(SDL_GetWindowFromID(e.key.windowID), false), gCaptured = false;
            }
            break;
        }
        if (down && e.key.key == SDLK_ESCAPE && gCaptured)
        {
            SDL_SetWindowRelativeMouseMode(SDL_GetWindowFromID(e.key.windowID), false);
            gCaptured = false;
            break;
        }
        const uint32_t input = bindings::Make(bindings::Key, e.key.scancode);
        if (down)
            gHeldInputs.insert(input);
        else
            gHeldInputs.erase(input);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down && !gCaptured && bindings::InGameplay())
        {
            // In gameplay the first click captures the mouse (Escape releases it) and is not passed to the game; menus
            // keep the pointer.
            SDL_SetWindowRelativeMouseMode(SDL_GetWindowFromID(e.button.windowID), true);
            gCaptured = true;
            break;
        }
        if (!gCaptured && e.button.button == SDL_BUTTON_LEFT)
            gPointerDown = down, gPointerWX = e.button.x, gPointerWY = e.button.y, gPointerSeen = true;
        // Right click in the game's menus: Back (as B), the PC convention.
        if (!gCaptured && down && e.button.button == SDL_BUTTON_RIGHT && !bindings::InGameplay() && !menu::IsOpen())
            Inject(VpadB, 3);
        const uint32_t input = bindings::Make(bindings::Mouse, e.button.button);
        if (down)
            gHeldInputs.insert(input);
        else
            gHeldInputs.erase(input);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!gCaptured && e.wheel.y != 0)
            gPointerWheel += e.wheel.y > 0 ? 1 : -1;
        if (gCaptured && e.wheel.y != 0)
            gWheelUntil[bindings::Make(bindings::Wheel, e.wheel.y > 0 ? 1 : 2)] = cafe::gx2::FrameCount() + 3;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (gCaptured)
            gMouseX += e.motion.xrel, gMouseY += e.motion.yrel;
        else
            gPointerWX = e.motion.x, gPointerWY = e.motion.y, gPointerSeen = true, gPointerSerial++;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        gHeldInputs.clear(), gWheelUntil.clear(), gMouseX = gMouseY = 0;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which))
        {
            gPads[e.gdevice.which] = pad;
            dtlog::Important("[dt-port] gamepad connected: %s\n", SDL_GetGamepadName(pad));
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (auto it = gPads.find(e.gdevice.which); it != gPads.end())
        {
            SDL_CloseGamepad(it->second);
            gPads.erase(it);
        }
        break;
    default: break;
    }
}

GamePad Read(uint64_t frame)
{
    std::lock_guard lock(gMutex);
    GamePad p;
    // Gameplay uses the player's bindings; menus, dialogs, movies and loading each platform's fixed convention
    // (bindings::Effective). When that changes, an input held across it that now means something else is ignored until
    // released, so a held confirm never turns into a gameplay action (or the reverse).
    const bool gameplay = bindings::InGameplay();
    const auto table = gameplay ? bindings::Current() : bindings::Menus();
    if (gameplay != gWasGameplay)
    {
        const auto before = gameplay ? bindings::Menus() : bindings::Current();
        for (uint32_t input : gHeldInputs)
            if (KeyEffect(*before, input) != KeyEffect(*table, input))
                gSuppressed.insert({ 0, input });
        for (auto& [id, pad] : gPads)
        {
            const Device d = DeviceOfType(SDL_GetGamepadType(pad));
            for (uint32_t input : AllPadInputs())
                if (PadInputDown(pad, input) && PadEffect(*before, input, d) != PadEffect(*table, input, d))
                    gSuppressed.insert({ id, input });
        }
        gWasGameplay = gameplay;
    }
    if (!gPadScript.empty())
        DriveVirtualPad(frame);
    std::erase_if(gSuppressed, [&](const std::pair<SDL_JoystickID, uint32_t>& s) {
        if (!s.first)
            return !gHeldInputs.count(s.second);
        auto it = gPads.find(s.first);
        return it == gPads.end() || !PadInputDown(it->second, s.second);
    });
    const auto& targets = bindings::Targets();
    bool stick[5] = {};  // keyboard left stick: up, down, left, right
    uint32_t padButtons = 0;
    for (size_t i = 0; i < targets.size(); i++)
    {
        bool down = false;
        for (uint32_t input : (*table)[i].keys)
            if (input && ((gHeldInputs.count(input) && !gSuppressed.count({ 0, input })) ||
                             (bindings::KindOf(input) == bindings::Wheel && frame < gWheelUntil[input])))
                down = true;
        const uint32_t vpad = targets[i].enbaku ? bindings::EnbakuButtons() : targets[i].vpad;
        if (down)
        {
            p.buttons |= vpad;
            stick[targets[i].stick] = true;
        }
        for (auto& [id, pad] : gPads)
        {
            const uint32_t input = bindings::PadInput(*table, i, DeviceOfType(SDL_GetGamepadType(pad)));
            if (PadInputDown(pad, input) && !gSuppressed.count({ id, input }))
                padButtons |= vpad;
        }
    }
    // Keyboard left stick, normalised so diagonals are not faster.
    float kx = float(stick[4]) - float(stick[3]), ky = float(stick[1]) - float(stick[2]);
    if (kx && ky)
        kx *= 0.7071f, ky *= 0.7071f;
    p.leftX = kx, p.leftY = ky;
    // Escaping a throw with the keyboard: the stick sweeps toward the keys' direction a fifth of the way a frame, so
    // each A or D press moves it for several frames, as wiggling a stick does (SetThrowEscape).
    {
        static float sweptX = 0, sweptY = 0;
        if (gThrowEscape && ActiveDevice() == Device::Keyboard)
        {
            sweptX += std::clamp(kx - sweptX, -0.2f, 0.2f), sweptY += std::clamp(ky - sweptY, -0.2f, 0.2f);
            p.leftX = sweptX, p.leftY = sweptY;
        }
        else
            sweptX = kx, sweptY = ky;
    }
    // Mouse right stick: the motion since the last read, scaled so a brisk move tilts fully (controls.mouse_sensitivity:
    // the mouse's speed on top of the game's own camera sensitivity; inverting is the game's camera option).
    const float sensitivity = std::clamp(settings::GetFloat("controls.mouse_sensitivity", 1.0f), 0.1f, 10.0f);
    if (const char* t = getenv("DT_TEST_MOUSE"); t && bindings::InGameplay())
        gMouseX += float(atof(t));  // testing: a steady sideways motion, pixels per frame
    if (bindings::InGameplay())
    {
        // Gameplay: a linear mouse. The camera should turn MouseUnitsPerPixel * sensitivity rotation units per pixel;
        // at the frame rate that is a turn rate, and the stick value whose rate (on the game's curve, with the turn
        // speeds MouseLookBoost times larger) is exactly that is what the game gets.
        // (frames per second as measured between reads: the cap may be any rate, or none)
        static auto lastRead = std::chrono::steady_clock::now();
        const auto nowRead = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(nowRead - lastRead).count(), 1.0f / 1000.0f, 1.0f / 20.0f);
        lastRead = nowRead;
        const float fps = 1.0f / dt;
        const float rateX = gMouseX * MouseUnitsPerPixel * sensitivity * fps;   // units per second
        const float rateY = -gMouseY * MouseUnitsPerPixel * sensitivity * fps;
        static float carryX = 0, carryY = 0;
        p.rightX = StickForTurnRate(rateX / MouseLookBoost, carryX);
        p.rightY = StickForTurnRate(rateY / MouseLookBoost * (240.0f / 200.0f), carryY);  // LookUpScale 200, LookRightScale 240
    }
    else
    {
        p.rightX = std::clamp(gMouseX * sensitivity / MousePixelsForFullTilt, -1.0f, 1.0f);
        p.rightY = std::clamp(-gMouseY * sensitivity / MousePixelsForFullTilt, -1.0f, 1.0f);
    }
    // DT_TEST_STICK=<x> (environment, testing): hold the right stick at x (may exceed 1) in gameplay; "sweep" steps
    // through 0.05 .. 1.0, 3 s each (TestStickValue).
    if (getenv("DT_TEST_STICK") && bindings::InGameplay())
        p.rightX = TestStickValue();
    gMouseX = gMouseY = 0;
    const float deadZone = std::clamp(settings::GetFloat("controls.dead_zone", 0.15f), 0.0f, 0.6f);
    for (auto& [id, pad] : gPads)
    {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE))
            padButtons |= VpadHome;
        p.leftX += Axis(pad, SDL_GAMEPAD_AXIS_LEFTX, false, deadZone);
        p.leftY += Axis(pad, SDL_GAMEPAD_AXIS_LEFTY, true, deadZone);
        p.rightX += Axis(pad, SDL_GAMEPAD_AXIS_RIGHTX, false, deadZone);
        p.rightY += Axis(pad, SDL_GAMEPAD_AXIS_RIGHTY, true, deadZone);
    }
    p.buttons |= padButtons & ~VpadHome;  // the Guide button opens the settings menu instead
    uint32_t scripted = 0;
    for (const Press& s : gScript)
        if (frame >= s.frame && frame < s.frame + s.frames)
        {
            scripted |= s.buttons;
            p.leftX += s.leftX, p.leftY += s.leftY, p.rightX += s.rightX, p.rightY += s.rightY;
        }
    p.buttons |= scripted & ~VpadHome;
    gScriptedNow = scripted;
    // The title's "press START": the first frame anything is newly down (a key, a mouse button, any controller button,
    // real or scripted) presses A, which the screen takes. Read here, not from events: scripted pads send none.
    {
        static bool wasDown = false;
        bool down = !gHeldInputs.empty();
        for (auto& [id, pad] : gPads)
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !down; b++)
                down = SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
        if (gPressStart && down && !wasDown)
        {
            Inject(VpadA, 4);
            dtlog::Important("[ui] press-START: input at frame %llu, pressing A\n", (unsigned long long)frame);
        }
        wasDown = down;
    }
    gHeld = p.buttons;
    const uint32_t masked = gMasked.fetch_and(p.buttons) & p.buttons;  // released buttons are no longer masked
    p.buttons &= ~masked;
    if (frame < gInjectedUntil)
        p.buttons |= gInjected;
    gGameButtons = p.buttons;
    // DT_TEST_SKIP_CUTSCENES (testing): A for 4 frames in every 40 while a cutscene or a Bink movie plays, which skips
    // it (read here: the mission's briefing movie plays while the level loads, when the Game Thread does not tick).
    static const bool skipCutscenes = getenv("DT_TEST_SKIP_CUTSCENES") != nullptr;
    if (skipCutscenes && frame % 40 < 4 && game_ui::CutsceneOrMovie())
        p.buttons |= VpadA;
    CheckMenuCombo(padButtons | scripted);  // (--press F:home opens the menu, for tests)
    // While the settings menu is open the game sees an idle GamePad.
    if (menu::IsOpen())
        p = GamePad{};
    p.leftX = std::clamp(p.leftX, -1.0f, 1.0f), p.leftY = std::clamp(p.leftY, -1.0f, 1.0f);
    p.rightX = std::clamp(p.rightX, -1.0f, 1.0f), p.rightY = std::clamp(p.rightY, -1.0f, 1.0f);
    return p;
}

void Inject(uint32_t buttons, uint32_t frames)
{
    gInjected = buttons;
    gInjectedUntil = cafe::gx2::FrameCount() + frames;
}

void MaskHeld()
{
    gMasked = gHeld.load();  // (the scripted presses' share)
    // The physical inputs held now, until released: the page may see a press before the game's last Read did, so the
    // GamePad bits read last are not enough.
    std::lock_guard lock(gMutex);
    for (uint32_t input : gHeldInputs)
        gSuppressed.insert({ 0, input });
    for (auto& [id, pad] : gPads)
        for (uint32_t input : AllPadInputs())
            if (PadInputDown(pad, input))
                gSuppressed.insert({ id, input });
}

float TestStickValue()
{
    static const char* v = getenv("DT_TEST_STICK");
    if (!v)
        return 0;
    if (std::string(v) != "sweep")
        return float(atof(v));
    static const float steps[] = { 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f };
    static uint64_t start = 0;
    const uint64_t frame = cafe::gx2::FrameCount();
    if (!start)
        start = frame;
    return steps[((frame - start) / 180) % std::size(steps)];
}

uint32_t HeldButtons() { return gHeld.load(); }
uint32_t GameButtons() { return gGameButtons.load(); }

void SetThrowEscape(bool escaping)
{
    if (gThrowEscape.exchange(escaping) != escaping)
        dtlog::Important("[input] throw escape %s\n", escaping ? "started" : "ended");
}

void NotePointer(float x, float y)
{
    std::lock_guard lock(gMutex);
    if (!gCaptured)
        gPointerWX = x, gPointerWY = y, gPointerSeen = true, gPointerSerial++;
}

Pointer GetPointer()
{
    Pointer p;
    // DT_TEST_POINTER (testing): "frame:x,y[:down];...", each step holding from its frame on.
    static const std::vector<std::tuple<uint64_t, float, float, bool>> script = [] {
        std::vector<std::tuple<uint64_t, float, float, bool>> v;
        const char* e = getenv("DT_TEST_POINTER");
        for (std::string spec = e ? e : ""; !spec.empty();)
        {
            const size_t end = std::min(spec.find(';'), spec.size());
            unsigned long long f = 0;
            float x = 0, y = 0;
            char down[8] = {};
            if (sscanf(spec.substr(0, end).c_str(), "%llu:%f,%f:%7s", &f, &x, &y, down) >= 3)
                v.push_back({ f, x, y, down[0] == 'd' });
            spec = end < spec.size() ? spec.substr(end + 1) : "";
        }
        return v;
    }();
    if (!script.empty())
    {
        const uint64_t now = cafe::gx2::FrameCount();
        for (size_t i = 0; i < script.size(); i++)
            if (std::get<0>(script[i]) <= now)
                p.valid = true, p.x = std::get<1>(script[i]), p.y = std::get<2>(script[i]), p.down = std::get<3>(script[i]),
                p.serial = i + 1;
        return p;
    }
    std::lock_guard lock(gMutex);
    if (!gPointerSeen || gCaptured || ActiveDevice() != Device::Keyboard)
        return p;
    SDL_Window* w = Window();
    int ww = 0, wh = 0;
    if (!w || !SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0)
        return p;
    // (the picture is letterboxed to 16:9 in the window)
    const float s = std::min(float(ww) / 1280.0f, float(wh) / 720.0f), ox = (float(ww) - 1280 * s) / 2, oy = (float(wh) - 720 * s) / 2;
    p.x = (gPointerWX - ox) / s, p.y = (gPointerWY - oy) / s;
    p.valid = p.x >= 0 && p.y >= 0 && p.x < 1280 && p.y < 720;
    p.down = gPointerDown;
    p.serial = gPointerSerial;
    p.wheel = gPointerWheel;
    return p;
}

void ReleaseMouse(SDL_Window* window)
{
    std::lock_guard lock(gMutex);
    if (gCaptured && window)
        SDL_SetWindowRelativeMouseMode(window, false);
    gCaptured = false;
    gHeldInputs.clear(), gWheelUntil.clear(), gMouseX = gMouseY = 0;
}

void ReleaseMouseOutsideGameplay(SDL_Window* window)
{
    std::lock_guard lock(gMutex);
    static bool autoReleased = false;  // released here, so taken back when gameplay resumes (cutscenes)
    if (!window)
        return;
    if (!gCaptured && autoReleased && bindings::InGameplay())
    {
        autoReleased = false;
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) && !menu::IsOpen())
        {
            SDL_SetWindowRelativeMouseMode(window, true);
            gCaptured = true, gMouseX = gMouseY = 0;
            dtlog::Important("[input] mouse captured again: gameplay resumed\n");
        }
        return;
    }
    if (!gCaptured || bindings::InGameplay())
        return;
    autoReleased = true;
    SDL_SetWindowRelativeMouseMode(window, false);
    gCaptured = false, gMouseX = gMouseY = 0;
    // (the pointer starts where the mouse is now)
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    gPointerWX = x, gPointerWY = y, gPointerSeen = true, gPointerSerial++;
    dtlog::Important("[input] mouse released: the game left gameplay\n");
}

MenuPad ReadMenuPad()
{
    std::lock_guard lock(gMutex);
    MenuPad m;
    const uint32_t s = gScriptedNow;
    m.up = s & VpadUp, m.down = s & VpadDown, m.left = s & VpadLeft, m.right = s & VpadRight;
    m.confirm = s & VpadA, m.back = s & VpadB, m.reset = s & VpadX, m.prevPage = s & VpadL, m.nextPage = s & VpadR;
    for (auto& [id, pad] : gPads)
    {
        // Confirm and back by the face button style: positional (Nintendo pads by default) confirms with the east
        // button (A) as the Wii U, by label (the others) with the south one (A, cross); "Default" is Y (triangle).
        const bool nintendo = !bindings::FaceByLabel(DeviceOfType(SDL_GetGamepadType(pad)));  // positional
        m.confirm |= SDL_GetGamepadButton(pad, nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH);
        m.back |= SDL_GetGamepadButton(pad, nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST);
        m.reset |= SDL_GetGamepadButton(pad, nintendo ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH);
        m.up |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        m.down |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        m.left |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        m.right |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        m.prevPage |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        m.nextPage |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        m.stickX += Axis(pad, SDL_GAMEPAD_AXIS_LEFTX, false, 0.2f);
        m.stickY += Axis(pad, SDL_GAMEPAD_AXIS_LEFTY, true, 0.2f);
    }
    return m;  // (the open/close combo is checked in Read: the game keeps reading the GamePad while the menu is open)
}

int ConnectedPadType() { return gLastPadType; }

Device DeviceOfType(int type)
{
    switch (SDL_GamepadType(type))
    {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return Device::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return Device::Nintendo;
    default: return Device::Xbox;
    }
}

const char* DeviceName(Device d)
{
    switch (d)
    {
    case Device::Keyboard: return "keyboard and mouse";
    case Device::Xbox: return "Xbox-style controller";
    case Device::PlayStation: return "PlayStation controller";
    case Device::Nintendo: return "Nintendo controller";
    }
    return "?";
}

// --prompts DEVICE (tests): prompts and conventions as if that device were in use.
std::atomic<int> gForced{ -1 };

Device ActiveDevice() { return gForced >= 0 ? Device(gForced.load()) : Device(gActive.load()); }
Device PadDevice()
{
    if (gForced > 0)
        return Device(gForced.load());
    return DeviceOfType(gLastPadType);
}

bool ForceDevice(const std::string& name)
{
    static const std::pair<const char*, Device> names[] = { { "keyboard", Device::Keyboard }, { "xbox", Device::Xbox },
        { "playstation", Device::PlayStation }, { "nintendo", Device::Nintendo } };
    for (auto& [n, d] : names)
        if (name == n)
            return gForced = int(d), true;
    return false;
}

void SetPressStartScreen(bool showing)
{
    if (gPressStart.exchange(showing) != showing)
        dtlog::Important("[ui] title press-START screen %s\n", showing ? "shown" : "left");
}
bool PressStartScreen() { return gPressStart; }

void NoteEvent(const SDL_Event& e)
{
    auto pad = [](SDL_JoystickID id) {
        gLastPadType = int(SDL_GetGamepadTypeForID(id));
        gPadUsed = true;
        SetActive(DeviceOfType(gLastPadType));
    };
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        SetActive(Device::Keyboard);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 6)  // not the odd jitter
            SetActive(Device::Keyboard);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        pad(e.gbutton.which);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(int(e.gaxis.value)) > 16000)
            pad(e.gaxis.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gPadUsed)  // a controller just plugged in counts once something is pressed on it; its type shows meanwhile
            gLastPadType = int(SDL_GetGamepadTypeForID(e.gdevice.which));
        break;
    default:
        break;
    }
}

void Rumble(float strength, uint32_t ms)
{
    std::lock_guard lock(gMutex);
    const uint16_t level = uint16_t(std::clamp(strength, 0.0f, 1.0f) * 0xFFFF);
    for (auto& [id, pad] : gPads)
        SDL_RumbleGamepad(pad, level, level, level ? std::max(ms, 30u) : 0);
}

bool AddPadPress(const std::string& spec)
{
    const size_t c1 = spec.find(':');
    if (c1 == std::string::npos)
        return false;
    const size_t c2 = spec.find(':', c1 + 1);
    PadPress p{};
    try
    {
        p.frame = std::stoull(spec.substr(0, c1));
        p.frames = c2 == std::string::npos ? 6 : std::stoull(spec.substr(c2 + 1));
    }
    catch (...)
    {
        return false;
    }
    const std::string list = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
    for (size_t start = 0; start <= list.size();)
    {
        const size_t end = std::min(list.find('+', start), list.size());
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(list.substr(start, end - start).c_str());
        if (b == SDL_GAMEPAD_BUTTON_INVALID)
            return false;
        p.buttons.push_back(b);
        start = end + 1;
    }
    std::lock_guard lock(gMutex);
    gPadScript.push_back(p);
    return true;
}

bool AddScriptedPress(const std::string& spec)
{
    static const std::map<std::string, uint32_t> names = { { "a", VpadA }, { "b", VpadB }, { "x", VpadX },
        { "y", VpadY }, { "l", VpadL }, { "r", VpadR }, { "zl", VpadZL }, { "zr", VpadZR }, { "plus", VpadPlus },
        { "start", VpadPlus }, { "minus", VpadMinus }, { "home", VpadHome }, { "up", VpadUp }, { "down", VpadDown },
        { "left", VpadLeft }, { "right", VpadRight } };
    // Stick directions at full deflection: l/r stick, then up/down/left/right.
    static const std::map<std::string, std::pair<int, std::pair<float, float>>> sticks = {
        { "lup", { 0, { 0, 1 } } }, { "ldown", { 0, { 0, -1 } } }, { "lleft", { 0, { -1, 0 } } }, { "lright", { 0, { 1, 0 } } },
        { "rup", { 1, { 0, 1 } } }, { "rdown", { 1, { 0, -1 } } }, { "rleft", { 1, { -1, 0 } } }, { "rright", { 1, { 1, 0 } } } };
    const size_t c1 = spec.find(':');
    if (c1 == std::string::npos)
        return false;
    const size_t c2 = spec.find(':', c1 + 1);
    Press p{};
    try
    {
        p.frame = std::stoull(spec.substr(0, c1));
        p.frames = c2 == std::string::npos ? 6 : std::stoull(spec.substr(c2 + 1));
    }
    catch (...)
    {
        return false;
    }
    std::string list = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
    size_t start = 0;
    while (start <= list.size())
    {
        const size_t end = std::min(list.find('+', start), list.size());
        const std::string name = list.substr(start, end - start);
        if (auto it = names.find(name); it != names.end())
            p.buttons |= it->second;
        else if (auto st = sticks.find(name); st != sticks.end())
        {
            auto [x, y] = st->second.second;
            (st->second.first ? p.rightX : p.leftX) += x, (st->second.first ? p.rightY : p.leftY) += y;
        }
        else
            return false;
        start = end + 1;
    }
    std::lock_guard lock(gMutex);
    gScript.push_back(p);
    return true;
}
} // namespace host::input
