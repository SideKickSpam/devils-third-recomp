#include "prompts.h"

#include "bindings.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace host::prompts
{
namespace
{
Glyph Cap(const char* text) { return { Glyph::Cap, text }; }

Glyph PadGlyph(SDL_GamepadButton b, input::Device d)
{
    const bool ps = d == input::Device::PlayStation, nin = d == input::Device::Nintendo;
    switch (b)
    {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? Glyph{ Glyph::Face, "", 1 } : Glyph{ Glyph::Face, nin ? "B" : "A" };
    case SDL_GAMEPAD_BUTTON_EAST: return ps ? Glyph{ Glyph::Face, "", 2 } : Glyph{ Glyph::Face, nin ? "A" : "B" };
    case SDL_GAMEPAD_BUTTON_WEST: return ps ? Glyph{ Glyph::Face, "", 3 } : Glyph{ Glyph::Face, nin ? "Y" : "X" };
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? Glyph{ Glyph::Face, "", 4 } : Glyph{ Glyph::Face, nin ? "X" : "Y" };
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Cap(ps ? "L1" : nin ? "L" : "LB");
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Cap(ps ? "R1" : nin ? "R" : "RB");
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return Cap(ps ? "L3" : "LS");
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return Cap(ps ? "R3" : "RS");
    case SDL_GAMEPAD_BUTTON_START: return Cap(ps ? "OPTIONS" : nin ? "+" : "MENU");
    case SDL_GAMEPAD_BUTTON_BACK: return Cap(ps ? "SHARE" : nin ? "-" : "VIEW");
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return { Glyph::Dpad, "", 0, 0, 1 };
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return { Glyph::Dpad, "", 0, 0, 2 };
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return { Glyph::Dpad, "", 0, 0, 3 };
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return { Glyph::Dpad, "", 0, 0, 4 };
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return Cap("PAD");
    case SDL_GAMEPAD_BUTTON_MISC1: return Cap(ps ? "MIC" : "SHARE");
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return Cap("P1");
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return Cap("P2");
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return Cap("P3");
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return Cap("P4");
    default: return Cap("?");
    }
}
} // namespace

Glyph For(uint32_t input, input::Device device)
{
    using namespace bindings;
    const uint32_t code = CodeOf(input);
    switch (KindOf(input))
    {
    case Key: return { Glyph::Key, Name(input) };
    case Mouse: return { Glyph::Mouse, "", 0, int(code) };
    case Wheel: return { Glyph::Wheel, "", 0, 0, int(code) };
    case PadButton: return PadGlyph(SDL_GamepadButton(code), device == input::Device::Keyboard ? input::PadDevice() : device);
    case PadTrigger:
    {
        const input::Device d = device == input::Device::Keyboard ? input::PadDevice() : device;
        const bool left = code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        return Cap(d == input::Device::PlayStation ? (left ? "L2" : "R2") : d == input::Device::Nintendo ? (left ? "ZL" : "ZR") : (left ? "LT" : "RT"));
    }
    default: return {};
    }
}

Glyph ForGamePad(uint32_t vpad, input::Device device)
{
    const auto table = bindings::Effective();  // in menus the platform's convention, in gameplay the player's keys
    const auto& targets = bindings::Targets();
    for (size_t i = 0; i < targets.size(); i++)
    {
        if (targets[i].vpad != vpad)
            continue;
        if (device == input::Device::Keyboard)
        {
            for (uint32_t k : (*table)[i].keys)
                if (k)
                    return For(k, device);
            // In menus + may have no key of its own (Enter confirms there): the game takes A where it asks for +.
            if (vpad == input::VpadPlus && !bindings::InGameplay())
                return ForGamePad(input::VpadA, device);
            return {};
        }
        return For(bindings::PadInput(*table, i, device), device);
    }
    return {};
}
// In-game key caps use short labels, so each fits its button's place ("Left Shift" in a cap read "eft Shif").
std::string ShortKey(const std::string& name)
{
    static const std::pair<const char*, const char*> names[] = { { "Left Shift", "Shift" }, { "Right Shift", "Shift" },
        { "Left Ctrl", "Ctrl" }, { "Right Ctrl", "Ctrl" }, { "Left Alt", "Alt" }, { "Right Alt", "Alt" }, { "Left GUI", "Win" },
        { "Right GUI", "Win" }, { "Return", "Enter" }, { "Keypad Enter", "Enter" }, { "Escape", "Esc" }, { "Backspace", "Bksp" },
        { "CapsLock", "Caps" }, { "Caps Lock", "Caps" }, { "Delete", "Del" }, { "Insert", "Ins" }, { "PageUp", "PgUp" },
        { "Page Up", "PgUp" }, { "PageDown", "PgDn" }, { "Page Down", "PgDn" }, { "Left Click", "LMB" },
        { "Right Click", "RMB" }, { "Middle Click", "MMB" }, { "Wheel Up", "Wheel" }, { "Wheel Down", "Wheel" } };
    for (auto& [from, to] : names)
        if (name == from)
            return to;
    if (name.rfind("Keypad ", 0) == 0)
        return "Num " + name.substr(7);
    return name;
}

std::vector<GameIcon> GameIcons(input::Device device)
{
    using namespace input;
    // The GamePad buttons the library draws, what its Wii U art says, and the art's width (pixels).
    static const struct
    {
        const char* name;
        uint32_t vpad;
        const char* art;
        float width;
    } buttons[] = {
        { "A", VpadA, "A", 40 }, { "B", VpadB, "B", 40 }, { "X", VpadX, "X", 40 }, { "Y", VpadY, "Y", 40 },
        { "L", VpadL, "L", 54 }, { "R", VpadR, "R", 54 }, { "ZL", VpadZL, "ZL", 53 }, { "ZR", VpadZR, "ZR", 53 },
        { "Plus", VpadPlus, "+", 40 }, { "Minus", VpadMinus, "-", 40 },
    };
    std::vector<GameIcon> out;
    const auto table = bindings::Effective();
    const auto& targets = bindings::Targets();
    for (const auto& b : buttons)
    {
        GameIcon icon{ b.name };
        // Xbox-style and Nintendo controllers: the game's own art wherever it fits. A face button bound to a face button
        // shows that button's letter in the game's art (kinds 12-15); the other buttons keep theirs when bound to their
        // own place (L on the left shoulder...). Drawn icons only where the game has none (an unusual rebind).
        if (device == Device::Xbox || device == Device::Nintendo)
        {
            size_t i = 0;
            while (i < targets.size() && targets[i].vpad != b.vpad)
                i++;
            const uint32_t in = bindings::PadInput(*table, i, device);
            const Glyph g = For(in, device);
            static const std::string faces[] = { "A", "B", "X", "Y" };
            const auto face = std::find(std::begin(faces), std::end(faces), g.text);
            if (g.shape == Glyph::Face && face != std::end(faces))
            {
                icon.kind = g.text == b.art ? 0 : 12 + int(face - std::begin(faces));
                out.push_back(icon);
                continue;
            }
            if (std::find(std::begin(faces), std::end(faces), b.art) == std::end(faces) && in == bindings::Defaults()[i].pad)
            {
                out.push_back(icon);  // kind 0
                continue;
            }
        }
        const Glyph g = ForGamePad(b.vpad, device);
        switch (g.shape)
        {
        case Glyph::Face:
            if (g.symbol)
                icon.kind = 2 + g.symbol;
            else if (g.text != b.art)
                icon.kind = 2, icon.text = g.text;
            break;
        case Glyph::Cap:
        case Glyph::Key:
            if (g.text != b.art || g.shape == Glyph::Key)
                icon.kind = 1, icon.text = g.shape == Glyph::Key ? ShortKey(g.text) : g.text;
            break;
        case Glyph::Dpad:
            icon.kind = 1, icon.text = g.dir == 1 ? "UP" : g.dir == 2 ? "DOWN" : g.dir == 3 ? "LEFT" : "RIGHT";
            break;
        case Glyph::Mouse:
            if (g.button <= 3)
                icon.kind = g.button == 1 ? 7 : g.button == 2 ? 8 : 9;
            else
                icon.kind = 1, icon.text = "M" + std::to_string(g.button);
            break;
        case Glyph::Wheel:
            icon.kind = g.dir == 1 ? 10 : 11;
            break;
        default:
            icon.kind = 1, icon.text = "?";  // not bound
        }
        if (icon.kind == 1)
        {
            // Wider caps for longer names; smaller text for more than two letters.
            const size_t n = icon.text.size();
            icon.textScale = n <= 2 ? 100 : n <= 4 ? 78 : 62;
            const float need = float(n) * 11.0f * float(icon.textScale) / 100.0f + 16.0f;
            icon.capWidth = std::max(100, int(need / std::max(b.width, 48.0f) * 100.0f));
        }
        out.push_back(icon);
    }
    // Sticks and the D-pad: the library's own art on controllers; on the keyboard the keys bound to them.
    auto keyName = [&](uint32_t vpad, int stick) {
        for (size_t i = 0; i < targets.size(); i++)
            if ((vpad && targets[i].vpad == vpad) || (stick && targets[i].stick == stick))
                for (uint32_t k : (*table)[i].keys)
                    if (k)
                        return ShortKey(bindings::Name(k));
        return std::string("?");
    };
    static const struct
    {
        const char* name;
        uint32_t vpad;
        int stick;
    } extra[] = { { "Up", VpadUp, 0 }, { "Down", VpadDown, 0 }, { "Left", VpadLeft, 0 }, { "Right", VpadRight, 0 },
        { "LClick", VpadStickL, 0 }, { "RClick", VpadStickR, 0 }, { "Dpad", 0, 0 }, { "LStick", 0, 0 }, { "RStick", 0, 0 },
        { "Gyro", 0, 0 }, { "Mash", 0, 0 } };  // Mash: "move any stick" (throw escape: wiggle; A and D on the keyboard)
    for (const auto& e : extra)
    {
        GameIcon icon{ e.name };
        if (std::string(e.name) == "Gyro")
        {
            // Tilting the Wii U GamePad to aim (the FGM148): the mouse or the right stick aims instead.
            icon.kind = 1, icon.text = device == Device::Keyboard ? "Mouse" : "R Stick";
            icon.textScale = 62, icon.capWidth = 150;
            out.push_back(icon);
            continue;
        }
        if (device == Device::Keyboard)
        {
            // A stick click or D-pad direction bound to a mouse button or the wheel: the mouse picture, as for the other
            // buttons ("Middle Click" in a key cap did not fit: "iddle Clic").
            if (e.vpad)
            {
                uint32_t bound = 0;
                for (size_t i = 0; i < targets.size() && !bound; i++)
                    if (targets[i].vpad == e.vpad)
                        for (uint32_t k : (*table)[i].keys)
                            if (k && !bound)
                                bound = k;
                const uint32_t code = bindings::CodeOf(bound);
                if (bound && bindings::KindOf(bound) == bindings::Mouse && code <= 3)
                {
                    icon.kind = code == 1 ? 7 : code == 2 ? 8 : 9;
                    out.push_back(icon);
                    continue;
                }
                if (bound && bindings::KindOf(bound) == bindings::Wheel)
                {
                    icon.kind = code == 1 ? 10 : 11;
                    out.push_back(icon);
                    continue;
                }
            }
            icon.kind = 1;
            if (e.vpad)
                icon.text = keyName(e.vpad, 0);
            else if (std::string(e.name) == "LStick")
                icon.text = keyName(0, 1) + keyName(0, 3) + keyName(0, 2) + keyName(0, 4);  // forward, left, back, right
            else if (std::string(e.name) == "RStick")
                icon.text = "Mouse";
            else if (std::string(e.name) == "Mash")
                icon.text = keyName(0, 3) + "\t" + keyName(0, 4);  // left, right: two caps pressed in turn (tab-separated)
            else
                icon.text = "Arrows";
            // (sized for the longer of a Mash prompt's two keys)
            const size_t tab = icon.text.find('\t');
            const size_t n = tab == std::string::npos ? icon.text.size() : std::max(tab, icon.text.size() - tab - 1);
            icon.textScale = n <= 2 ? 100 : n <= 4 ? 78 : 62;
            icon.capWidth = std::max(100, int((float(n) * 11.0f * float(icon.textScale) / 100.0f + 16.0f) / 48.0f * 100.0f));
        }
        out.push_back(icon);
    }
    return out;
}
} // namespace host::prompts
