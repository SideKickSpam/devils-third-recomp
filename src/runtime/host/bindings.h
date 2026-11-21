// Key bindings: which keys, mouse buttons and controller buttons press which Wii U GamePad buttons. The game's own
// controller layout (Options > Controls: Default, Shooter, Acrobat, Ninja) then decides what each GamePad button does,
// so bindings sit on top of it: rebinding Fire moves it to another key whatever the layout, and the Controls tab names
// each button by the action the player's layout gives it. Saved in settings.ini as bindings.<target>.keys / .pad.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "input.h"

union SDL_Event;

namespace host::bindings
{
// A physical input. Encoded as kind << 16 | code.
enum Kind : uint32_t
{
    None = 0,
    Key = 1,         // SDL_Scancode
    Mouse = 2,       // SDL mouse button (1 left, 2 middle, 3 right, 4 and 5 the side buttons)
    Wheel = 3,       // 1 up, 2 down: a short press per notch
    PadButton = 4,   // SDL_GamepadButton
    PadTrigger = 5,  // SDL_GamepadAxis (the left or right trigger)
};
constexpr uint32_t Make(Kind kind, uint32_t code) { return uint32_t(kind) << 16 | code; }
constexpr Kind KindOf(uint32_t input) { return Kind(input >> 16); }
constexpr uint32_t CodeOf(uint32_t input) { return input & 0xFFFF; }

// What a binding drives: a GamePad button, or (keyboard only) a direction of the left stick.
struct Target
{
    const char* id;       // settings name
    uint32_t vpad;        // GamePad button bits (host::input::Vpad*), or 0 for a stick direction
    int stick;            // 1 up, 2 down, 3 left, 4 right of the left stick (keyboard only)
    const char* button;   // the GamePad button's name, shown beside the action ("ZR")
    const char* layoutKey;  // its field in the game's layout table (BtnZR), or null
    bool enbaku = false;    // Enbaku: the layout's Jump and Fast Melee buttons together (EnbakuButtons)
};
const std::vector<Target>& Targets();

constexpr int KeySlots = 2;  // keyboard and mouse inputs per target; one controller input
struct Binding
{
    uint32_t keys[KeySlots] = {};
    uint32_t pad = 0;     // when padSet: the controller input the player chose
    bool padSet = false;  // otherwise the default of the controller's family (DefaultPad)
};
using Table = std::vector<Binding>;  // indexed like Targets()

// Any thread: the current table (replaced, never changed in place, when a binding changes).
std::shared_ptr<const Table> Current();
const Table& Defaults();  // keyboard defaults (pads: DefaultPad)
// The bindings outside gameplay (the game's menus and dialogs, movies, loading): each platform's fixed convention,
// whatever the player rebound. Controllers use their family's defaults (DefaultPad); the keyboard the default keys,
// with Enter and Space confirming (A) and Escape and Backspace backing out (B); + keeps the player's own keys that are
// not among those. No Enbaku.
std::shared_ptr<const Table> Menus();
// Any thread: the table in effect: Menus() outside gameplay, Current() in it (game_ui).
std::shared_ptr<const Table> Effective();
bool InGameplay();
// The face button style (controls.face_buttons): true when a controller's A/B/X/Y map by label ("xbox": the button
// labelled A is the Wii U's A; PlayStation by the same positions), false by position as on the Wii U ("positional",
// east = A). "auto" (the default): positional for Nintendo controllers, by label for the others.
bool FaceByLabel(input::Device device);
// A controller's default for a target, by its family's convention: Xbox and PlayStation pads by label (south = A, the
// confirm button there), Nintendo pads by position as on the Wii U (east = A).
uint32_t DefaultPad(size_t target, input::Device device);
// The controller input that presses a target on a pad of that family.
uint32_t PadInput(const Table& table, size_t target, input::Device device);
// Back to the family default for one target's controller slot.
void ResetPad(size_t target);
// Settings thread (the menu's): set one slot (0..KeySlots-1 keyboard, KeySlots the controller) and save.
void Set(size_t target, int slot, uint32_t input);
// Restore the defaults of one device: keyboard and mouse, or the controller.
void ResetKeyboard();
void ResetController();
// The target and slot already using `input` on the same device, other than (target, slot); false if none.
bool FindUse(uint32_t input, size_t target, int slot, size_t& usedTarget, int& usedSlot);

// A short name for an input, e.g. "Space", "Left Click", "RB", "Cross" (controller names follow the connected pad).
std::string Name(uint32_t input);
// The action the player's game layout gives a target ("Fire"), from the game's own text; the button name if unknown.
std::string ActionName(size_t target);
// The target holds the layout's Interact action, which the game names "Reload / Interact": one GamePad button that
// interacts when something is in reach and reloads otherwise (DevilsThirdPlayerController), in every layout.
bool IsReloadInteract(size_t target);
// The player's game layout's name ("Acrobat"), read from the save.
std::string LayoutName();
// The GamePad button(s) a target presses, as shown beside its action: "ZR", or "B + Y" for Enbaku.
std::string ButtonName(size_t target);
// Any thread: the GamePad buttons Enbaku needs in the player's layout, its Jump and Fast Melee Attack buttons
// (DevilsThirdPlayerController.CheckSpecialMode starts it when both are held and each went down no more than
// 0.099 s before: the binding presses both on the same frame).
uint32_t EnbakuButtons();

// Capturing the next input for a slot (the Controls tab's "rebind").
void BeginCapture(size_t target, int slot);
void CancelCapture();
bool Capturing();
// Window thread: true when the event was taken by a capture.
bool CaptureEvent(const SDL_Event& e);
// Settings thread: the captured input (Make(None, 0) for a cancel), once; false while still waiting.
bool TakeCaptured(uint32_t& input);
} // namespace host::bindings
