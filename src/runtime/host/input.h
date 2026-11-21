// Host input mapped onto the Wii U GamePad (VPAD): keyboard and SDL gamepads, plus scripted presses
// (--press) so unattended runs can get past "press START" screens.
#pragma once

#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;

namespace host::input
{
// VPAD button bits.
enum : uint32_t
{
    VpadSync = 0x1, VpadHome = 0x2, VpadMinus = 0x4, VpadPlus = 0x8, VpadR = 0x10, VpadL = 0x20, VpadZR = 0x40,
    VpadZL = 0x80, VpadDown = 0x100, VpadUp = 0x200, VpadRight = 0x400, VpadLeft = 0x800, VpadY = 0x1000,
    VpadX = 0x2000, VpadB = 0x4000, VpadA = 0x8000, VpadStickR = 0x20000, VpadStickL = 0x40000,
};

struct GamePad
{
    uint32_t buttons = 0;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;  // -1..1, y up
};

// Called on the window thread for every SDL event.
void HandleEvent(const SDL_Event& e);
// Window thread: leave relative mouse mode (the settings menu opened).
void ReleaseMouse(SDL_Window* window);
// Window thread, on every pass of its loop: leaves relative mouse mode when the game is no longer in gameplay (a
// mission's score screen, a cutscene, a menu the game opened itself), so the menus' pointer and cursor work there.
void ReleaseMouseOutsideGameplay(SDL_Window* window);

// The controllers' state for navigating the settings menu (keyboard and mouse reach it as events).
struct MenuPad
{
    bool up = false, down = false, left = false, right = false, confirm = false, back = false, reset = false,
         prevPage = false, nextPage = false;
    float stickX = 0, stickY = 0;  // left stick, y up
};
MenuPad ReadMenuPad();

// The kind of device last used, for prompts and conventions: keyboard and mouse, or a controller family by its layout
// (SDL's gamepad type; anything not PlayStation or Nintendo counts as Xbox-style).
enum class Device
{
    Keyboard,
    Xbox,
    PlayStation,
    Nintendo,
};
// Any thread.
Device ActiveDevice();
// Mouse look: in gameplay with the keyboard and mouse, the game's turn speeds (DevilsThirdPlayerInput.LookRightScale and
// LookUpScale) are this many times larger (gfx_hooks.cpp) and the mouse's right-stick values this many times smaller.
// The game caps the stick at full tilt (about 132 degrees a second), so a fast mouse move hit the cap; scaled down, it
// stays below it. Slow motion turns as before.
constexpr float MouseLookBoost = 8.0f;
// DT_TEST_STICK (testing): the right stick value held now.
float TestStickValue();
// The controller family last used (Xbox with none yet): what pad prompts show while the keyboard is active.
Device PadDevice();
Device DeviceOfType(int sdlGamepadType);
// --prompts NAME (keyboard, xbox, playstation, nintendo): use that device's prompts and conventions (tests).
bool ForceDevice(const std::string& name);
const char* DeviceName(Device d);
// Window thread: every SDL event, to follow the device last touched (hot-swapping mid-game).
void NoteEvent(const SDL_Event& e);
// The SDL_GamepadType of the controller last used (SDL_GAMEPAD_TYPE_STANDARD with none), for button names.
int ConnectedPadType();

// Rumble every connected controller at `strength` (0-1) for `ms` milliseconds (0 stops it).
void Rumble(float strength, uint32_t ms);

// The GamePad state at this frame: live input plus any scripted presses.
GamePad Read(uint64_t frame);

// Any thread: hold `buttons` on the game's GamePad for `frames` frames from now, as if pressed (the settings page uses it
// to open and close the game's own pause menu).
void Inject(uint32_t buttons, uint32_t frames);
// The title's "press START" screen is showing (gfx_hooks.cpp, from D3FrontEnd_TitleMenu.NowMenuStage): any key, mouse
// button or controller button presses A there.
void SetPressStartScreen(bool showing);
// The GamePad buttons held at the last read (host::input::Vpad*).
uint32_t HeldButtons();
// The buttons the game was given at its last read, injected ones (menu clicks, Inject) included. Any thread.
uint32_t GameButtons();
bool PressStartScreen();
// The menus' mouse pointer (not captured: outside gameplay), in the game's 1280x720 layout. `valid` once the mouse has
// moved over the window since the keyboard and mouse became the active device; `serial` counts moves, `down` is the left
// button. DT_TEST_POINTER="frame:x,y[:down];..." (testing) scripts it.
struct Pointer
{
    bool valid = false, down = false;
    float x = 0, y = 0;
    uint64_t serial = 0;
    int wheel = 0;  // wheel notches turned since the start (up positive)
};
Pointer GetPointer();
// Game Thread: the player is escaping a throw (gfx_hooks.cpp, the pawn's PlayerThrowBeaten* states). The game counts
// every tick either stick moved more than 0.05; a key jumps the stick to full at once (two counts a tap, against a
// wiggled stick's count every frame), so while escaping the keyboard's left stick sweeps to each A/D press over frames.
void SetThrowEscape(bool escaping);
// Window thread: the mouse moved to (x, y) (window pixels) while the PC page had it.
void NotePointer(float x, float y);
// Any thread: hide the buttons and keys held now from the game until each is released (the press that closed the
// settings page must not reach the game's menu underneath).
void MaskHeld();

// --press FRAME:BUTTONS[:FRAMES]  e.g. "1000:plus" or "1200:a:10". BUTTONS is a '+'-separated list of names
// (a b x y l r zl zr plus minus home up down left right) and stick directions at full deflection (lup ldown lleft
// lright, rup rdown rleft rright). Returns false on a malformed spec.
bool AddScriptedPress(const std::string& spec);
// --pad-press FRAME:BUTTONS[:FRAMES] (tests): press buttons of a virtual Xbox-style controller, so they go through
// the bindings as a real controller's do. BUTTONS is a '+'-separated list of SDL gamepad button names (a = south,
// b = east, x = west, y = north, start, back, dpup, dpdown, dpleft, dpright, leftshoulder, rightshoulder...).
bool AddPadPress(const std::string& spec);
} // namespace host::input
