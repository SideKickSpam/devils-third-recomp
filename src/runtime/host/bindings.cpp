#include "bindings.h"

#include "../assets/coalesced.h"
#include "../game_settings.h"
#include "../game_ui.h"
#include "../log.h"
#include "../settings.h"
#include "../vfs.h"
#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>

namespace host::bindings
{
namespace
{
using namespace host::input;

const std::vector<Target> gTargets = {
    { "zr", VpadZR, 0, "ZR", "BtnZR" },
    { "zl", VpadZL, 0, "ZL", "BtnZL" },
    { "r", VpadR, 0, "R", "BtnR" },
    { "l", VpadL, 0, "L", "BtnL" },
    { "a", VpadA, 0, "A", "BtnA" },
    { "b", VpadB, 0, "B", "BtnB" },
    { "x", VpadX, 0, "X", "BtnX" },
    { "y", VpadY, 0, "Y", "BtnY" },
    { "lclick", VpadStickL, 0, "L Stick", "StkL_Push" },
    { "rclick", VpadStickR, 0, "R Stick", "StkR_Push" },
    { "up", VpadUp, 0, "D-pad Up", "Cross_Up" },
    { "down", VpadDown, 0, "D-pad Down", "Cross_Down" },
    { "left", VpadLeft, 0, "D-pad Left", "Cross_Left" },
    { "right", VpadRight, 0, "D-pad Right", "Cross_Right" },
    { "plus", VpadPlus, 0, "+", nullptr },
    { "minus", VpadMinus, 0, "-", nullptr },
    { "enbaku", 0, 0, "", nullptr, true },
    { "forward", 0, 1, "L Stick Up", nullptr },
    { "back", 0, 2, "L Stick Down", nullptr },
    { "strafe_left", 0, 3, "L Stick Left", nullptr },
    { "strafe_right", 0, 4, "L Stick Right", nullptr },
};

constexpr uint32_t K(SDL_Scancode s) { return Make(Key, uint32_t(s)); }
constexpr uint32_t P(SDL_GamepadButton b) { return Make(PadButton, uint32_t(b)); }

// Keyboard defaults (the mapping the port always had). Controller defaults depend on the pad (DefaultPad); the ones
// written here are the Nintendo-style positions the port always used, which settings saved before were full of.
Table MakeDefaults()
{
    Table t(gTargets.size());
    auto set = [&](const char* id, uint32_t k0, uint32_t k1, uint32_t pad) {
        for (size_t i = 0; i < gTargets.size(); i++)
            if (std::string(gTargets[i].id) == id)
                t[i] = { { k0, k1 }, pad };
    };
    set("zr", Make(Mouse, SDL_BUTTON_LEFT), K(SDL_SCANCODE_3), Make(PadTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    set("zl", Make(Mouse, SDL_BUTTON_RIGHT), K(SDL_SCANCODE_1), Make(PadTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    set("r", K(SDL_SCANCODE_E), 0, P(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    set("l", K(SDL_SCANCODE_Q), 0, P(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    set("a", K(SDL_SCANCODE_SPACE), K(SDL_SCANCODE_Z), P(SDL_GAMEPAD_BUTTON_EAST));
    set("b", K(SDL_SCANCODE_X), K(SDL_SCANCODE_BACKSPACE), P(SDL_GAMEPAD_BUTTON_SOUTH));
    set("x", K(SDL_SCANCODE_C), 0, P(SDL_GAMEPAD_BUTTON_NORTH));
    set("y", K(SDL_SCANCODE_V), 0, P(SDL_GAMEPAD_BUTTON_WEST));
    set("lclick", K(SDL_SCANCODE_LSHIFT), K(SDL_SCANCODE_RSHIFT), P(SDL_GAMEPAD_BUTTON_LEFT_STICK));
    set("rclick", Make(Mouse, SDL_BUTTON_MIDDLE), 0, P(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    set("up", K(SDL_SCANCODE_UP), 0, P(SDL_GAMEPAD_BUTTON_DPAD_UP));
    set("down", K(SDL_SCANCODE_DOWN), 0, P(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    set("left", K(SDL_SCANCODE_LEFT), 0, P(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    set("right", K(SDL_SCANCODE_RIGHT), 0, P(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    set("plus", K(SDL_SCANCODE_ESCAPE), 0, P(SDL_GAMEPAD_BUTTON_START));  // Escape pauses (and is Back in menus)
    set("minus", K(SDL_SCANCODE_TAB), 0, P(SDL_GAMEPAD_BUTTON_BACK));
    set("enbaku", K(SDL_SCANCODE_F), 0, 0);  // controllers press the two buttons, as on the Wii U
    set("forward", K(SDL_SCANCODE_W), 0, 0);
    set("back", K(SDL_SCANCODE_S), 0, 0);
    set("strafe_left", K(SDL_SCANCODE_A), 0, 0);
    set("strafe_right", K(SDL_SCANCODE_D), 0, 0);
    return t;
}

// ---- settings: names that stay the same across SDL versions and keyboard layouts ----
std::string StoreName(uint32_t input)
{
    const uint32_t code = CodeOf(input);
    switch (KindOf(input))
    {
    case Key: return SDL_GetScancodeName(SDL_Scancode(code));
    case Mouse: return "Mouse" + std::to_string(code);
    case Wheel: return code == 1 ? "WheelUp" : "WheelDown";
    case PadButton: return SDL_GetGamepadStringForButton(SDL_GamepadButton(code));
    case PadTrigger: return SDL_GetGamepadStringForAxis(SDL_GamepadAxis(code));
    default: return "none";
    }
}

uint32_t ParseKeyName(const std::string& s)
{
    if (s.rfind("Mouse", 0) == 0 && s.size() == 6 && s[5] >= '1' && s[5] <= '5')
        return Make(Mouse, uint32_t(s[5] - '0'));
    if (s == "WheelUp" || s == "WheelDown")
        return Make(Wheel, s == "WheelUp" ? 1 : 2);
    const SDL_Scancode sc = SDL_GetScancodeFromName(s.c_str());
    return sc == SDL_SCANCODE_UNKNOWN ? 0 : K(sc);
}

uint32_t ParsePadName(const std::string& s)
{
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(s.c_str());
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return Make(PadTrigger, axis);
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.c_str());
    return b == SDL_GAMEPAD_BUTTON_INVALID || b == SDL_GAMEPAD_BUTTON_GUIDE ? 0 : P(b);
}

std::mutex gMutex;
std::shared_ptr<const Table> gTable;

std::shared_ptr<const Table> Load()
{
    auto t = std::make_shared<Table>(Defaults());
    // Files of earlier versions saved every slot, so the old universal defaults there are not choices; files that
    // write "default" for unset slots (this version) keep every value they name.
    bool current = false;
    for (const Target& target : gTargets)
        if (settings::Get(std::string("bindings.") + target.id + ".pad", "") == "default")
            current = true;
    for (size_t i = 0; i < gTargets.size(); i++)
    {
        const std::string prefix = std::string("bindings.") + gTargets[i].id;
        const std::string keys = settings::Get(prefix + ".keys", "");
        if (!keys.empty())
        {
            Binding& b = (*t)[i];
            std::istringstream in(keys);
            std::string name;
            for (int slot = 0; slot < KeySlots; slot++)
                b.keys[slot] = std::getline(in, name, ',') ? ParseKeyName(name) : 0;
            // The backtick/tilde key never pauses: a START bound to it (a stray key while rebinding) is dropped, and a
            // START left with no key gets the default (Escape).
            if (gTargets[i].vpad == VpadPlus)
            {
                for (uint32_t& k : b.keys)
                    if (k == K(SDL_SCANCODE_GRAVE))
                        k = 0;
                if (!b.keys[0] && !b.keys[1])
                    std::copy(std::begin(Defaults()[i].keys), std::end(Defaults()[i].keys), b.keys);
                else if (!b.keys[0])
                    std::swap(b.keys[0], b.keys[1]);
            }
        }
        const std::string pad = settings::Get(prefix + ".pad", "");
        if (!pad.empty() && pad != "default" && !gTargets[i].stick)
        {
            (*t)[i].pad = ParsePadName(pad);
            (*t)[i].padSet = current || (*t)[i].pad != Defaults()[i].pad;
        }
    }
    return t;
}

void Store(const Table& t)
{
    for (size_t i = 0; i < gTargets.size(); i++)
    {
        const std::string prefix = std::string("bindings.") + gTargets[i].id;
        settings::Set(prefix + ".keys", StoreName(t[i].keys[0]) + "," + StoreName(t[i].keys[1]));
        if (!gTargets[i].stick)
            settings::Set(prefix + ".pad", t[i].padSet ? StoreName(t[i].pad) : "default");
    }
    settings::Save();
}

void Replace(std::shared_ptr<const Table> t)
{
    Store(*t);
    std::lock_guard lock(gMutex);
    gTable = std::move(t);
}

// ---- the player's game layout ----
std::string Localized(const std::string& key, const std::string& fallback)
{
    return assets::coalesced::Value("DT_UI_Pause.INT", "Menus", key, fallback);
}

// The player's layout: the game's own value once it can be read (game_settings), else from the save's tagged
// properties: the name "ControllerLayout", its type's name ("IntProperty"), size 4, array index, the value. Absent until
// the player first changes it (0, Default).
int LayoutIndex()
{
    if (const game_settings::Values v = game_settings::Current(); v.valid)
        return v.controllerLayout;
    static std::mutex m;
    static int index = 0;
    static auto checked = std::chrono::steady_clock::time_point{};
    std::lock_guard lock(m);
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::seconds(1))
        return index;
    checked = now;
    const auto saveDir = vfs::Resolve("/vol/save");
    if (!saveDir)
        return index;
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (auto& entry : std::filesystem::directory_iterator(*saveDir, ec))
    {
        const auto file = entry.path() / "d3sys.sav";
        const auto t = std::filesystem::last_write_time(file, ec);
        if (!ec && (newest.empty() || t > newestTime))
            newest = file, newestTime = t;
    }
    if (newest.empty())
        return index;
    std::ifstream in(newest, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::string tag("ControllerLayout\0", 17);
    const size_t at = data.find(tag);
    index = 0;
    if (at != std::string::npos)
    {
        auto u32 = [&](size_t o) {
            return o + 4 <= data.size() ? uint32_t(uint8_t(data[o])) << 24 | uint32_t(uint8_t(data[o + 1])) << 16 |
                    uint32_t(uint8_t(data[o + 2])) << 8 | uint8_t(data[o + 3])
                                        : 0u;
        };
        size_t o = at + tag.size();
        o += 4 + u32(o);  // the type's name
        if (u32(o) == 4)  // size, array index, value
            index = int(u32(o + 8));
    }
    return index;
}

// The layout table: "(BtnA=E_OPTION_MENU_GA_INTERACT, BtnB=E_OPTION_MENU_GA_JUMP, ...)" per layout.
std::map<std::string, std::string> LayoutActions(int index)
{
    std::map<std::string, std::string> out;
    const auto layouts = assets::coalesced::Values("", "DevilsThirdGame.D3UIWidget_OptionMenuGame", "OptionGameCtrLayoutList");
    if (index < 0 || size_t(index) >= layouts.size())
        return out;
    std::string s = layouts[size_t(index)];
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '(' || c == ')' || c == ' '; }), s.end());
    std::istringstream in(s);
    std::string pair;
    while (std::getline(in, pair, ','))
        if (const size_t eq = pair.find('='); eq != std::string::npos)
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
    return out;
}
} // namespace

const std::vector<Target>& Targets() { return gTargets; }

const Table& Defaults()
{
    static const Table t = MakeDefaults();
    return t;
}

std::shared_ptr<const Table> Current()
{
    std::lock_guard lock(gMutex);
    if (!gTable)
        gTable = Load();
    return gTable;
}

std::shared_ptr<const Table> Menus()
{
    static std::mutex mutex;
    static std::shared_ptr<const Table> from, menus;  // the player's table the menu table was made from
    const auto current = Current();
    std::lock_guard lock(mutex);
    if (current == from)
        return menus;
    auto t = std::make_shared<Table>(Defaults());
    const uint32_t confirm[] = { K(SDL_SCANCODE_RETURN), K(SDL_SCANCODE_SPACE) }, back[] = { K(SDL_SCANCODE_ESCAPE), K(SDL_SCANCODE_BACKSPACE) };
    auto reserved = [&](uint32_t k) {
        return k == K(SDL_SCANCODE_RETURN) || k == K(SDL_SCANCODE_KP_ENTER) || k == K(SDL_SCANCODE_SPACE) ||
               k == K(SDL_SCANCODE_ESCAPE) || k == K(SDL_SCANCODE_BACKSPACE);
    };
    for (size_t i = 0; i < t->size(); i++)
    {
        Binding& b = (*t)[i];
        b.pad = 0, b.padSet = false;
        if (gTargets[i].vpad == VpadA)
            std::copy(std::begin(confirm), std::end(confirm), b.keys);
        else if (gTargets[i].vpad == VpadB)
            std::copy(std::begin(back), std::end(back), b.keys);
        else if (gTargets[i].vpad == VpadPlus)
        {
            int n = 0;
            b.keys[0] = b.keys[1] = 0;
            for (uint32_t k : (*current)[i].keys)
                if (k && !reserved(k))
                    b.keys[n++] = k;
        }
        else if (gTargets[i].enbaku)
            b = {};
        else
            for (uint32_t& k : b.keys)
                if (reserved(k))
                    k = 0;
        // Mouse buttons are the pointer's in menus (host::input::GetPointer): bound to nothing there.
        for (uint32_t& k : b.keys)
            if (Kind(k >> 16) == Mouse)
                k = 0;
    }
    from = current, menus = std::move(t);
    return menus;
}

bool InGameplay() { return game_ui::Current() == game_ui::State::Gameplay; }

std::shared_ptr<const Table> Effective() { return InGameplay() ? Current() : Menus(); }

void Set(size_t target, int slot, uint32_t input)
{
    auto t = std::make_shared<Table>(*Current());
    if (slot < KeySlots)
        (*t)[target].keys[slot] = input;
    else
        (*t)[target].pad = input, (*t)[target].padSet = true;
    dtlog::Important("[bindings] %s %s = %s\n", gTargets[target].id, slot < KeySlots ? "key" : "pad", StoreName(input).c_str());
    Replace(std::move(t));
}

void ResetKeyboard()
{
    auto t = std::make_shared<Table>(*Current());
    for (size_t i = 0; i < t->size(); i++)
        std::copy(std::begin(Defaults()[i].keys), std::end(Defaults()[i].keys), (*t)[i].keys);
    Replace(std::move(t));
}

void ResetController()
{
    auto t = std::make_shared<Table>(*Current());
    for (size_t i = 0; i < t->size(); i++)
        (*t)[i].pad = 0, (*t)[i].padSet = false;
    Replace(std::move(t));
}

bool FaceByLabel(input::Device device)
{
    const std::string style = settings::Get("controls.face_buttons", "auto");
    if (style == "xbox")
        return true;
    if (style == "positional")
        return false;
    return device != input::Device::Nintendo;
}

uint32_t DefaultPad(size_t target, input::Device device)
{
    const bool byLabel = FaceByLabel(device);
    switch (gTargets[target].vpad)
    {
    case VpadA: return P(byLabel ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST);
    case VpadB: return P(byLabel ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH);
    case VpadX: return P(byLabel ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH);
    case VpadY: return P(byLabel ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST);
    default: return Defaults()[target].pad;
    }
}

uint32_t PadInput(const Table& table, size_t target, input::Device device)
{
    return table[target].padSet ? table[target].pad : DefaultPad(target, device);
}

void ResetPad(size_t target)
{
    auto t = std::make_shared<Table>(*Current());
    (*t)[target].pad = 0, (*t)[target].padSet = false;
    Replace(std::move(t));
}

bool FindUse(uint32_t input, size_t target, int slot, size_t& usedTarget, int& usedSlot)
{
    if (!input)
        return false;
    const auto t = Current();
    for (size_t i = 0; i < t->size(); i++)
    {
        if (slot < KeySlots)
        {
            for (int k = 0; k < KeySlots; k++)
                if ((*t)[i].keys[k] == input && !(i == target && k == slot))
                    return usedTarget = i, usedSlot = k, true;
        }
        else if (PadInput(*t, i, input::PadDevice()) == input && i != target)
            return usedTarget = i, usedSlot = KeySlots, true;
    }
    return false;
}

std::string Name(uint32_t input)
{
    const uint32_t code = CodeOf(input);
    switch (KindOf(input))
    {
    case Key:
    {
        if (code == SDL_SCANCODE_RETURN)
            return "Enter";  // (SDL calls it Return)
        if (code == SDL_SCANCODE_KP_ENTER)
            return "Num Enter";
        const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(code), SDL_KMOD_NONE, false));
        return name && *name ? name : SDL_GetScancodeName(SDL_Scancode(code));
    }
    case Mouse:
        switch (code)
        {
        case SDL_BUTTON_LEFT: return "Left Click";
        case SDL_BUTTON_RIGHT: return "Right Click";
        case SDL_BUTTON_MIDDLE: return "Middle Click";
        default: return "Mouse " + std::to_string(code);
        }
    case Wheel: return code == 1 ? "Wheel Up" : "Wheel Down";
    case PadButton:
    case PadTrigger:
    {
        const SDL_GamepadType type = SDL_GamepadType(input::ConnectedPadType());
        const bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
        const bool nintendo = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
        if (KindOf(input) == PadTrigger)
            return code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? (ps ? "L2" : nintendo ? "ZL" : "LT") : (ps ? "R2" : nintendo ? "ZR" : "RT");
        switch (SDL_GamepadButton(code))
        {
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_WEST:
        case SDL_GAMEPAD_BUTTON_NORTH:
            switch (SDL_GetGamepadButtonLabelForType(type, SDL_GamepadButton(code)))
            {
            case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
            case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
            case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
            case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
            default: break;
            }
            return code == SDL_GAMEPAD_BUTTON_SOUTH ? "A" : code == SDL_GAMEPAD_BUTTON_EAST ? "B" : code == SDL_GAMEPAD_BUTTON_WEST ? "X" : "Y";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nintendo ? "L" : "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nintendo ? "R" : "RB";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
        case SDL_GAMEPAD_BUTTON_BACK: return ps ? "Share" : nintendo ? "-" : "View";
        case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nintendo ? "+" : "Menu";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
        case SDL_GAMEPAD_BUTTON_MISC1: return ps ? "Mic" : "Share";
        default: return std::string("Paddle ") + std::to_string(code - SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1 + 1);
        }
    }
    default: return "";
    }
}

namespace
{
// Per layout, each target's action name and the layout's own name: the coalesced lookups scan the whole file.
struct LayoutNames
{
    std::vector<std::string> actions;
    std::vector<bool> interact;  // the target holds the layout's Interact action (GA_INTERACT, "Reload / Interact")
    std::string name;
    uint32_t enbaku = VpadB | VpadY;  // the Default layout's Jump and Fast Melee
    std::string enbakuButtons = "B + Y";
};

// A layout table field ("BtnB", "StkL_Push", "Cross_Up") as GamePad button bits and their name.
std::pair<uint32_t, std::string> LayoutButton(const std::string& field)
{
    for (const Target& t : gTargets)
        if (t.layoutKey && field == t.layoutKey)
            return { t.vpad, t.button };
    return { 0, field };
}

LayoutNames MakeLayoutNames(int index)
{
    LayoutNames out;
    const auto actions = LayoutActions(index);
    for (const Target& t : gTargets)
    {
        std::string name = t.button;
        switch (t.stick)
        {
        case 1: name = "Move Forward"; break;
        case 2: name = "Move Back"; break;
        case 3: name = "Move Left"; break;
        case 4: name = "Move Right"; break;
        default:
            if (t.enbaku)
                name = Localized("Enbaku", "Enbaku");
            else if (t.vpad == VpadPlus)
                name = "Pause Menu";
            else if (t.vpad == VpadMinus)
                name = "Scores (multiplayer)";
            else if (auto it = actions.find(t.layoutKey ? t.layoutKey : ""); it != actions.end())
            {
                // E_OPTION_MENU_GA_UNDER_BARREL -> the text under "Underbarrel" in DT_UI_Pause [Menus]
                std::string key = it->second.substr(std::min(it->second.size(), sizeof("E_OPTION_MENU_GA_") - 1));
                key.erase(std::remove(key.begin(), key.end(), '_'), key.end());
                name = Localized(key, t.button);
            }
        }
        out.actions.push_back(name);
        auto it = actions.find(t.layoutKey ? t.layoutKey : "");
        out.interact.push_back(!t.stick && !t.enbaku && it != actions.end() && it->second == "E_OPTION_MENU_GA_INTERACT");
    }
    // Enbaku: the buttons holding the Jump and Fast Melee Attack actions (GA_JUMP, GA_MELEE), which the game's
    // input bindings give bSpecialModeStartA and B in every layout.
    std::pair<uint32_t, std::string> jump{ 0, "" }, melee{ 0, "" };
    for (auto& [field, action] : actions)
        if (action == "E_OPTION_MENU_GA_JUMP")
            jump = LayoutButton(field);
        else if (action == "E_OPTION_MENU_GA_MELEE")
            melee = LayoutButton(field);
    if (jump.first && melee.first)
        out.enbaku = jump.first | melee.first, out.enbakuButtons = jump.second + " + " + melee.second;
    const auto items = assets::coalesced::Values("", "DevilsThirdGame.D3UIWidget_OptionMenuGame", "OptionGameCtrLayoutItemList");
    out.name = "Default";
    if (index >= 0 && size_t(index) < items.size())
    {
        const std::string& s = items[size_t(index)];  // (locName="Custom")
        const size_t q0 = s.find('"'), q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
        if (q1 != std::string::npos)
            out.name = Localized(s.substr(q0 + 1, q1 - q0 - 1), s.substr(q0 + 1, q1 - q0 - 1));
    }
    return out;
}

const LayoutNames& CurrentLayoutNames()
{
    static std::mutex m;
    static std::map<int, LayoutNames> cache;
    static bool loaded = false;
    const int index = LayoutIndex();
    std::lock_guard lock(m);
    if (!loaded && assets::coalesced::Loaded())
        cache.clear(), loaded = true;  // names made before the game's text was there
    auto it = cache.find(index);
    if (it == cache.end())
        it = cache.emplace(index, MakeLayoutNames(index)).first;
    return it->second;
}
} // namespace

std::string ActionName(size_t target) { return CurrentLayoutNames().actions[target]; }
bool IsReloadInteract(size_t target) { return CurrentLayoutNames().interact[target]; }

std::string ButtonName(size_t target)
{
    return gTargets[target].enbaku ? CurrentLayoutNames().enbakuButtons : std::string(gTargets[target].button);
}

uint32_t EnbakuButtons() { return CurrentLayoutNames().enbaku; }

std::string LayoutName() { return CurrentLayoutNames().name; }

// ---- capture ----
namespace
{
std::atomic<int> gCaptureSlot{ -1 };  // -1: not capturing
std::atomic<uint32_t> gCaptured{ 0 };
std::atomic<bool> gCaptureDone{ false };

void Finish(uint32_t input)
{
    gCaptured = input;
    gCaptureDone = true;
    gCaptureSlot = -1;
}
} // namespace

void BeginCapture(size_t, int slot)
{
    gCaptureDone = false;
    gCaptureSlot = slot;
}

void CancelCapture()
{
    if (gCaptureSlot >= 0)
        Finish(0);
}

bool Capturing() { return gCaptureSlot >= 0; }

bool CaptureEvent(const SDL_Event& e)
{
    const int slot = gCaptureSlot;
    if (slot < 0)
        return false;
    const bool keyboard = slot < KeySlots;
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat)
            return true;
        if (e.key.scancode == SDL_SCANCODE_ESCAPE)
            Finish(0);  // cancel
        else if (keyboard && e.key.scancode != SDL_SCANCODE_F1 && e.key.scancode != SDL_SCANCODE_F11)
            Finish(K(e.key.scancode));
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (keyboard)
            Finish(Make(Mouse, e.button.button));
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (keyboard && e.wheel.y != 0)
            Finish(Make(Wheel, e.wheel.y > 0 ? 1 : 2));
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
            return true;
        Finish(keyboard ? 0 : P(SDL_GamepadButton(e.gbutton.button)));  // a pad button cancels a keyboard capture
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!keyboard && (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            e.gaxis.value > 20000)
            Finish(Make(PadTrigger, e.gaxis.axis));
        return false;
    default:
        return false;
    }
}

bool TakeCaptured(uint32_t& input)
{
    if (!gCaptureDone.exchange(false))
        return false;
    input = gCaptured;
    return true;
}
} // namespace host::bindings
