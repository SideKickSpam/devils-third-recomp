// Button prompts: what to show for an input on the device in use, for the PC page (drawn by host/menu.cpp) and the
// game's own prompts. One description, so both show the same icons.
#pragma once

#include "input.h"

#include <cstdint>
#include <string>
#include <vector>

namespace host::prompts
{
struct Glyph
{
    enum Shape
    {
        None,
        Face,     // a round face button: `text` (A, B...) or a PlayStation `symbol`
        Cap,      // a rounded cap with `text`: shoulders, triggers, stick clicks, system buttons
        Dpad,     // the D-pad with direction `dir` (1 up, 2 down, 3 left, 4 right) lit
        Key,      // a key cap with `text`
        Mouse,    // a mouse with `button` lit (1 left, 2 middle, 3 right, 4 and 5 the side buttons)
        Wheel,    // a mouse wheel turned `dir` (1 up, 2 down)
    } shape = None;
    std::string text;
    int symbol = 0;  // PlayStation: 1 cross, 2 circle, 3 square, 4 triangle
    int button = 0, dir = 0;
};

// The glyph for a bound input (bindings::Make) on a device family (controllers; keys and mouse ignore it).
Glyph For(uint32_t input, input::Device device);
// The glyph for a GamePad button as the player presses it now: on keyboard and mouse their first bound key, on a
// controller its family's button for that GamePad button (through the bindings).
Glyph ForGamePad(uint32_t vpadButton, input::Device device);
// The game's prompt library (assets/swf_patch AddPromptVariants): per GamePad button, which picture its icons show.
struct GameIcon
{
    const char* button;  // A B X Y L R ZL ZR Plus Minus; Up Down Left Right Dpad LClick RClick LStick RStick
    int kind = 0;        // 0 the Wii U art, 1 cap with text, 2 face with a letter, 3-6 PlayStation symbols, 7-11 mouse,
                         // 12-15 (face buttons) the game's own A, B, X, Y art
    std::string text;
    int capWidth = 100, textScale = 100;  // percent
};
std::vector<GameIcon> GameIcons(input::Device device);
} // namespace host::prompts
