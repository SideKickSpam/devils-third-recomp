#include "button_text.h"

#include "bindings.h"
#include "input.h"

#include <SDL3/SDL_gamepad.h>

#include <regex>
#include <set>
#include <utility>

namespace host::button_text
{
namespace
{
using input::Device;

std::string Narrow(const std::u16string& s)
{
    std::string out;
    for (char16_t c : s)
    {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
            out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

std::u16string Widen(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = s[i];
        if (c < 0x80)
            out += char16_t(c), i++;
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size())
            out += char16_t((c & 0x1F) << 6 | (s[i + 1] & 0x3F)), i += 2;
        else if (i + 2 < s.size())
            out += char16_t((c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F)), i += 3;
        else
            i++;
    }
    return out;
}

// A controller input's name on a device family (not the connected pad's: the text follows the device in use).
std::string PadName(uint32_t input, Device d)
{
    const bool ps = d == Device::PlayStation, nin = d == Device::Nintendo;
    const uint32_t code = bindings::CodeOf(input);
    if (bindings::KindOf(input) == bindings::PadTrigger)
        return code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? (ps ? "L2" : nin ? "ZL" : "LT") : (ps ? "R2" : nin ? "ZR" : "RT");
    switch (SDL_GamepadButton(code))
    {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? "Cross" : nin ? "B" : "A";
    case SDL_GAMEPAD_BUTTON_EAST: return ps ? "Circle" : nin ? "A" : "B";
    case SDL_GAMEPAD_BUTTON_WEST: return ps ? "Square" : nin ? "Y" : "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? "Triangle" : nin ? "X" : "Y";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : "RB";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
    case SDL_GAMEPAD_BUTTON_BACK: return ps ? "Share" : nin ? "-" : "View";
    case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nin ? "+" : "Menu";
    default: return bindings::Name(input);
    }
}

// The name of what presses a GamePad button in gameplay, on the device in use: "Space key", "Left Click", "A Button",
// "Triangle Button", "LB".
std::string ButtonName(uint32_t vpad, Device device)
{
    const auto table = bindings::Current();
    const auto& targets = bindings::Targets();
    for (size_t i = 0; i < targets.size(); i++)
    {
        if (targets[i].vpad != vpad || targets[i].stick || targets[i].enbaku)
            continue;
        if (device == Device::Keyboard)
        {
            for (uint32_t k : (*table)[i].keys)
                if (k)
                    return bindings::KindOf(k) == bindings::Key ? bindings::Name(k) + " key" : bindings::Name(k);
            return "(unbound)";
        }
        const uint32_t in = bindings::PadInput(*table, i, device);
        const std::string name = PadName(in, device);
        const bool face = bindings::KindOf(in) == bindings::PadButton && bindings::CodeOf(in) <= 3;  // south..north
        return face ? name + " Button" : name;
    }
    return "?";
}

// A stick moved (`tilt`) or clicked.
std::string StickName(bool left, bool tilt, Device device)
{
    if (!tilt && device != Device::Keyboard)
    {
        // Pressing the stick is still "the Left Stick", unless the player moved that action to another button.
        const auto table = bindings::Current();
        const auto& targets = bindings::Targets();
        for (size_t i = 0; i < targets.size(); i++)
            if (targets[i].vpad == (left ? input::VpadStickL : input::VpadStickR))
            {
                const uint32_t in = bindings::PadInput(*table, i, device);
                if (in != bindings::Make(bindings::PadButton, left ? SDL_GAMEPAD_BUTTON_LEFT_STICK : SDL_GAMEPAD_BUTTON_RIGHT_STICK))
                    return ButtonName(targets[i].vpad, device);
            }
        tilt = true;  // named as the stick itself, below
    }
    if (!tilt)
        return ButtonName(left ? input::VpadStickL : input::VpadStickR, device);
    if (device == Device::Keyboard)
    {
        if (!left)
            return "mouse";
        const auto table = bindings::Current();
        std::string keys;
        for (int dir : { 1, 3, 2, 4 })  // forward, left, back, right: W/A/S/D
            for (size_t i = 0; i < bindings::Targets().size(); i++)
                if (bindings::Targets()[i].stick == dir && (*table)[i].keys[0])
                    keys += (keys.empty() ? "" : "/") + bindings::Name((*table)[i].keys[0]);
        return keys.empty() ? "the movement keys" : keys;
    }
    if (device == Device::Nintendo)
        return left ? "L Stick" : "R Stick";
    return left ? "Left Stick" : "Right Stick";
}

// Strings whose stick mentions mean clicking it (dash, slide, crouch); the others mean tilting it.
bool StickClicked(const std::string& key)
{
    static const std::set<std::string> clicks = { "Id_1124", "Id_1125", "Id_1129", "Action_13", "Action_15" };
    return clicks.count(key) > 0;
}

uint32_t VpadOf(const std::string& letter)
{
    using namespace input;
    static const std::pair<const char*, uint32_t> names[] = { { "A", VpadA }, { "B", VpadB }, { "X", VpadX }, { "Y", VpadY },
        { "L", VpadL }, { "R", VpadR }, { "ZL", VpadZL }, { "ZR", VpadZR } };
    for (auto& [n, v] : names)
        if (letter == n)
            return v;
    return 0;
}

// Replaces each match of `re` with fn(match).
template <typename Fn>
std::string ReplaceAll(const std::string& s, const std::regex& re, Fn fn)
{
    std::string out;
    auto last = s.cbegin();
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
    {
        out.append(last, (*it)[0].first);
        out += fn(*it);
        last = (*it)[0].second;
    }
    out.append(last, s.cend());
    return out;
}
} // namespace

std::optional<std::u16string> Rewrite(const std::u16string& sectionW, const std::u16string& keyW, const std::u16string& textW)
{
    // The title's "START" (press START): any input starts there (input::PressStartScreen).
    if (keyW == u"Start" && sectionW == u"MainTitleMenus")
        return input::ActiveDevice() == input::Device::Keyboard ? u"Press any key" : u"Press any button";
    // Quick test first: Localize runs often.
    static const std::u16string marks[] = { u"Button", u"button", u"Stick", u"GamePad", u"Gamepad", u"touch screen", u"touchscreen" };
    bool any = false;
    for (const auto& m : marks)
        any |= textW.find(m) != std::u16string::npos;
    if (!any)
        return std::nullopt;
    const std::string section = Narrow(sectionW), key = Narrow(keyW), text = Narrow(textW);
    const Device device = input::ActiveDevice();

    // The Wii U GamePad's own features.
    if (section == "Notice")
    {
        static const std::set<std::string> screen = { "WatchDRC", "TouchOnDRC", "TouchOffDRC" };
        static const std::set<std::string> offTv = { "UseGamePadOnly", "UseGamePad", "ChangeTV", "ChangeTV_URCC", "ChangeDRC",
            "ChangeDRC_URCC" };
        if (screen.count(key))
            return u"This was shown on the Wii U GamePad's screen, which the PC version does not have.";
        if (offTv.count(key))
            return u"Off-TV play (on the Wii U GamePad's screen) is not available on PC.";
    }
    if (section == "Multi_Single" && key == "System_02")
        return u"Off-TV play (on the Wii U GamePad's screen) is not available on PC.";
    if (text == "Aim with the Wii U GamePad!")  // the FGM148 launcher (gyro aiming on the Wii U)
        return Widen(std::string("Aim with the ") + (device == Device::Keyboard ? "mouse" : StickName(false, true, device)) + "!");

    std::string out = text;
    static const std::regex pair(R"(\bL/R Buttons\b)");
    out = ReplaceAll(out, pair, [&](const std::smatch&) {
        return ButtonName(input::VpadL, device) + " / " + ButtonName(input::VpadR, device);
    });
    static const std::regex button(R"(\b(ZL|ZR|A|B|X|Y|L|R) Button\b)");
    out = ReplaceAll(out, button, [&](const std::smatch& m) { return ButtonName(VpadOf(m[1].str()), device); });
    static const std::regex stick(R"(\b(L|R|Left|Right) Stick\b)");
    const bool clicked = StickClicked(key);
    out = ReplaceAll(out, stick, [&](const std::smatch& m) {
        return StickName(m[1].str()[0] == 'L', !clicked, device);
    });
    static const std::regex select(R"(\bselect button\b)");
    out = ReplaceAll(out, select, [&](const std::smatch&) { return ButtonName(input::VpadMinus, device); });
    // "the Left Click" reads oddly: mouse buttons and the wheel take no article.
    static const std::regex article(R"(\b[Tt]he ((Left|Right|Middle) Click|Wheel (Up|Down)|Mouse \d|[^ /]+/[^ /]+/[^ /]+/[^ /]+))");
    out = std::regex_replace(out, article, "$1");
    if (out == text)
        return std::nullopt;
    return Widen(out);
}
} // namespace host::button_text
