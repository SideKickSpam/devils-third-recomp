#include "menu.h"

#include "../cafe/gx2.h"
#include "../game_patches.h"
#include "../assets/coalesced.h"
#include "../game_settings.h"
#include "../game_ui.h"
#include "../gpu/overlay.h"
#include "../log.h"
#include "../settings.h"
#include "audio.h"
#include "bindings.h"
#include "input.h"
#include "prompts.h"
#include "swf_icons.h"
#include "window.h"

#include <imgui.h>
#include <imgui_internal.h>  // (ImGui::RegisterUserTexture: the weapon pictures)
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace host::menu
{
namespace
{
std::atomic<bool> gOpen{ false };
std::atomic<uint32_t> gSounds{ 0 };
// Toggle in a level: the Plus press sent to open the game's pause menu, and the frame it gives up at (0: none).
std::atomic<uint64_t> gPauseRequestUntil{ 0 };
const char* gPauseRequestWhy = "";
std::atomic<bool> gWePaused{ false };  // the page opened the game's pause menu, so closing the page closes it

void Play(Sound sound) { gSounds.fetch_or(sound); }

// ---- the pages: rows of settings ----
struct Row
{
    enum Kind
    {
        Choice,  // a value from a list, changed with left/right
        Slider,  // a number, changed with left/right or by dragging
        Action,   // runs something on confirm
        Info,     // text only
        Binding,  // the keys and controller button bound to a GamePad button (Controls tab)
    } kind = Info;
    std::string label, help;
    std::vector<std::string> choices;
    std::function<int()> get;  // Choice: the current index
    std::function<void(int)> set;
    int fallback = 0;
    float lo = 0, hi = 1, step = 0.1f, fallbackValue = 0;
    std::function<float()> getValue;  // Slider
    std::function<void(float)> setValue;
    const char* format = "%.1f";
    std::function<void()> action;
    size_t target = 0;  // Binding: index into bindings::Targets()
    bool preview = false;  // a picture setting: the page shows the game picture below while it is selected
};

// A Choice row over a string setting.
Row StringChoice(std::string label, std::string help, const char* key, const char* fallback,
    std::vector<std::pair<std::string, std::string>> options, std::function<void(const std::string&)> applied = nullptr)
{
    Row r;
    r.kind = Row::Choice;
    r.label = std::move(label), r.help = std::move(help);
    for (auto& [value, name] : options)
        r.choices.push_back(name);
    for (size_t i = 0; i < options.size(); i++)
        if (options[i].first == fallback)
            r.fallback = int(i);
    r.get = [key, fallback, options] {
        const std::string v = settings::Get(key, fallback);
        for (size_t i = 0; i < options.size(); i++)
            if (options[i].first == v)
                return int(i);
        return 0;
    };
    r.set = [key, options, applied](int i) {
        settings::Set(key, options[size_t(i)].first);
        settings::Save();
        if (applied)
            applied(options[size_t(i)].first);
    };
    return r;
}

Row BoolChoice(std::string label, std::string help, const char* key, bool fallback, std::function<void()> applied = nullptr)
{
    Row r;
    r.kind = Row::Choice;
    r.label = std::move(label), r.help = std::move(help);
    r.choices = { "Off", "On" };
    r.fallback = fallback ? 1 : 0;
    r.get = [key, fallback] { return settings::GetBool(key, fallback) ? 1 : 0; };
    r.set = [key, applied](int i) {
        settings::SetBool(key, i == 1);
        settings::Save();
        if (applied)
            applied();
    };
    return r;
}

Row FloatSlider(std::string label, std::string help, const char* key, float fallback, float lo, float hi, float step,
    const char* format)
{
    Row r;
    r.kind = Row::Slider;
    r.label = std::move(label), r.help = std::move(help);
    r.lo = lo, r.hi = hi, r.step = step, r.fallbackValue = fallback, r.format = format;
    r.getValue = [key, fallback] { return settings::GetFloat(key, fallback); };
    r.setValue = [key](float v) {
        settings::SetFloat(key, v);
        settings::Save();
    };
    return r;
}

// The game's Yes / No dialog, for Quit, the binding resets and binding conflicts.
struct Dialog
{
    bool open = false;
    std::string line1, line2;
    std::function<void()> yes;
    int choice = 1;  // 0 Yes, 1 No (the game's dialogs default to No)
    std::string title = "Confirm";
    std::vector<std::string> more;  // lines after line2
};
// A dialog over the game with the page closed (AskOverGame): the page opens showing only it, and closes when answered.
std::mutex gNoticeMutex;
std::optional<Dialog> gPendingNotice;
std::atomic<bool> gNoticeOnly{ false };
Dialog gDialog;
std::atomic<bool> gCloseModals{ false };  // SetOpen(false) from any thread: drop the dialog and any capture

void Ask(std::string line1, std::string line2, std::function<void()> yes)
{
    gDialog = { true, std::move(line1), std::move(line2), std::move(yes), 1 };
    Play(SoundOk);
}

// Controls tab: the binding being captured (its row's target and slot) and when the capture gives up.
size_t gCaptureTarget = 0;
int gCaptureSlot = 0;
double gCaptureUntil = 0;
bool gWaitPadRelease = false;  // after a capture, until the controller's buttons are up (they reach ImGui too)
// Set on opening: the controller is read only while the page is open, so a button still held from opening it (the
// game's Options entry, whatever it is bound to) would read as a new press; wait for its release instead.
std::atomic<bool> gOpenedWithPad{ false };
int gColumn = 0;               // the selected binding cell: 0, 1 keyboard and mouse, 2 controller

struct Page
{
    std::string name;
    std::vector<Row> rows;
    bool compact = false;  // short rows in columns, scrolled (the Controls tab)
};

Row BindingRow(size_t target)
{
    Row r;
    r.kind = Row::Binding;
    r.target = target;
    const bindings::Target& t = bindings::Targets()[target];
    r.help = t.stick    ? "Enter or A to change, Delete or Y for the default. Controllers move with the left stick."
             : t.enbaku ? "Presses Jump and Fast Melee Attack together (your layout's buttons). Enter or A to change."
                        : "Enter or A to change, Delete or Y for the default.";
    return r;
}

// The game's own text (DT_UI_Pause.INT, in the player's language), looked up once.
std::string Loc(const char* key, const char* fallback, const char* section = "Menus")
{
    static std::map<std::string, std::string> cache;
    const std::string id = std::string(section) + "." + key;
    auto it = cache.find(id);
    if (it == cache.end())
    {
        if (!assets::coalesced::Loaded())
            return fallback;
        it = cache.emplace(id, assets::coalesced::Value("DT_UI_Pause.INT", section, key, fallback)).first;
    }
    return it->second;
}

using game_settings::Field;
using game_settings::Values;

// A row over one of the game's own options: shown from the game's values, changed through game_settings.
Row GameChoice(std::string label, std::string help, Field field, std::vector<std::string> choices,
    int (*get)(const Values&))
{
    Row r;
    r.kind = Row::Choice;
    r.label = std::move(label), r.help = std::move(help), r.choices = std::move(choices);
    r.get = [get] { return get(game_settings::Current()); };
    r.set = [field](int i) { game_settings::Set(field, i); };
    const Values d = game_settings::Defaults();
    r.fallback = d.valid ? get(d) : 0;
    return r;
}

Row GameSlider(std::string label, std::string help, Field field, float lo, float hi, int (*get)(const Values&))
{
    Row r;
    r.kind = Row::Slider;
    r.label = std::move(label), r.help = std::move(help);
    r.lo = lo, r.hi = hi, r.step = 1, r.format = "%.0f";
    r.getValue = [get] { return float(get(game_settings::Current())); };
    r.setValue = [field](float v) { game_settings::Set(field, int(std::lround(v))); };
    const Values d = game_settings::Defaults();
    r.fallbackValue = d.valid ? float(get(d)) : lo;
    return r;
}

// An option the game does not offer right now: shown greyed with the reason.
Row Unavailable(std::string label, std::string why)
{
    Row r;
    r.kind = Row::Info;
    r.label = std::move(label), r.help = std::move(why);
    r.choices = { "-" };
    return r;
}

bool gFromGameOptions = false;  // opened from the game's Options entry: its own menu is one row away

std::vector<Page> BuildPages()
{
    const Values v = game_settings::Current();
    const std::vector<std::string> offOn = { Loc("Disabled", "Off"), Loc("Enabled", "On") };
    auto notYet = [&](std::vector<Row>& rows, const char* label) {
        rows.push_back(Unavailable(label, "The game's settings can be read once it has started."));
    };

    std::vector<Page> pages;
    Page display{ "Display", {} };
    {
        Row mode;
        mode.kind = Row::Choice;
        mode.label = "Window Mode";
        mode.help = "Show the game in a window, a borderless fullscreen window, or exclusive fullscreen.";
        mode.choices = { "Windowed", "Borderless", "Exclusive" };
        mode.fallback = 0;
        mode.get = [] { return int(host::CurrentDisplayMode()); };
        mode.set = [](int i) {
            host::RequestDisplayMode(DisplayMode(i));
            settings::Save();
        };
        display.rows.push_back(mode);
    }
    display.rows.push_back(StringChoice("Alt+Enter / F11", "The fullscreen mode Alt+Enter and F11 switch to.",
        "display.fullscreen", "borderless", { { "borderless", "Borderless" }, { "exclusive", "Exclusive" } }));
    display.rows.push_back(StringChoice("Vsync",
        "On waits for the display (best with G-Sync or FreeSync). Fast never waits and does not tear. Off tears.",
        "display.vsync", "fifo", { { "fifo", "On" }, { "mailbox", "Fast" }, { "immediate", "Off" } }));
    display.rows.push_back(StringChoice("Frame Rate",
        "50 fps is the original. Higher rates are smoother where the PC keeps up; Unlimited goes as fast as it can "
        "(with Vsync On and G-Sync, up to the display's refresh).",
        "display.fps_cap", "50",
        { { "50", "50 fps" }, { "60", "60 fps" }, { "120", "120 fps" }, { "144", "144 fps" }, { "240", "240 fps" }, { "0", "Unlimited" } },
        [](const std::string& v) { game_patches::SetFrameRateCap(std::stoi(v)); }));
    display.rows.push_back(BoolChoice("Frame Time", "Show the frame rate and frame time in the corner.", "display.show_fps", false));
    if (gFromGameOptions && game_ui::CanOpenOriginalOptions())
    {
        Row original;
        original.kind = Row::Action;
        original.label = "Original Options Menu";
        original.help = "Open the game's own Options menu instead. Both show the same settings.";
        original.action = [] {
            Play(SoundOk);
            gFromGameOptions = false;
            game_ui::OpenOriginalOptions();
            SetOpen(false, "original Options");
        };
        display.rows.push_back(original);
    }
    {
        Row quit;
        quit.kind = Row::Action;
        quit.label = "Quit Game";
        quit.help = "Close Devil's Third. Progress since the last checkpoint is lost.";
        quit.action = [] {
            Ask("Quit Devil's Third?", "Progress since the last checkpoint is lost.", [] {
                SDL_Event e{};
                e.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&e);
            });
        };
        display.rows.push_back(quit);
    }
    pages.push_back(std::move(display));

    // The game's Video page: brightness, contrast and gamma (with a preview of the game picture), screen areas.
    Page graphics{ "Graphics", {}, true };
    graphics.rows.push_back(StringChoice("Resolution",
        "The internal rendering resolution. 1280x720 is the Wii U's. Takes effect when the game is next started.",
        "graphics.resolution", "720p",
        { { "720p", "1280x720" }, { "1080p", "1920x1080" }, { "1440p", "2560x1440" }, { "2160p", "3840x2160" },
            { "custom", "Custom scale" } }));
    graphics.rows.push_back(StringChoice("Shadow Quality",
        "The resolution of shadows: the game's own (1024x512), or 2 or 4 times as sharp. Takes effect when the game is next "
        "started.",
        "graphics.shadow_quality", "1x", { { "1x", "Original" }, { "2x", "High" }, { "4x", "Ultra" } }));
    graphics.rows.push_back(StringChoice("Asynchronous Shaders",
        "Compiles new shaders in the background: something may be missing for a moment the first time it appears, "
        "instead of the game stuttering.",
        "graphics.async_shaders", "on", { { "off", "Off" }, { "on", "On" } }));
    graphics.rows.push_back(StringChoice("Keep GPU Clocks Up",
        "NVIDIA graphics cards: keeps the GPU at full speed during movies, loading and menus, so the first moments of "
        "play after them do not stutter while it speeds up again. Uses a little more power.",
        "graphics.gpu_boost", "on", { { "off", "Off" }, { "on", "On" } }));
    graphics.rows.push_back(FloatSlider("Custom Scale",
        "With Resolution set to Custom scale: the factor over 1280x720 (2.0 is 2560x1440). Takes effect at the next start.",
        "graphics.render_scale", 2.0f, 1.0f, 4.0f, 0.25f, "%.2fx"));
    if (v.valid)
    {
        Row b = GameSlider(Loc("Brightness", "Brightness"),
            "Adjust until dark areas in the picture below are just visible (the game's own brightness).", Field::Brightness, 0,
            100, [](const Values& x) { return x.brightness; });
        b.preview = true;
        graphics.rows.push_back(b);
        Row c = GameSlider(Loc("contrast", "Contrast"), Loc("Info_Contrast", "Adjust contrast.", "Descriptions"), Field::Contrast, 0,
            100, [](const Values& x) { return x.contrast; });
        c.preview = true;
        graphics.rows.push_back(c);
        Row g = GameSlider(Loc("Gamma", "Gamma"), Loc("Info_Gamma", "Adjust gamma correction.", "Descriptions"), Field::Gamma, 0,
            100, [](const Values& x) { return x.gamma; });
        g.preview = true;
        graphics.rows.push_back(g);
        graphics.rows.push_back(GameSlider(Loc("H_Margin", "Horizontal Area"),
            Loc("Info_H_Margin", "Adjust HUD horizontal margins.", "Descriptions"), Field::HorizontalArea, 0, 100,
            [](const Values& x) { return x.horizontalArea; }));
        graphics.rows.push_back(GameSlider(Loc("V_Margin", "Vertical Area"),
            Loc("Info_V_Margin", "Adjust HUD vertical margins.", "Descriptions"), Field::VerticalArea, 0, 100,
            [](const Values& x) { return x.verticalArea; }));
    }
    else
        notYet(graphics.rows, "Brightness");
    pages.push_back(std::move(graphics));

    Page audio{ Loc("Audio_title", "Audio"), {}, true };
    if (v.valid)
    {
        audio.rows.push_back(GameSlider(Loc("VoiceVolume", "Voice/Movie Volume"),
            Loc("Info_VoiceVolume", "Adjust voice/movie volume.", "Descriptions"), Field::VoiceVolume, 0, 100,
            [](const Values& x) { return x.voiceVolume; }));
        audio.rows.push_back(GameSlider(Loc("SeVolume", "SFX Volume"), Loc("Info_SE_Volume", "Adjust sound effect volume.", "Descriptions"),
            Field::SfxVolume, 0, 100, [](const Values& x) { return x.sfxVolume; }));
        audio.rows.push_back(GameSlider(Loc("BgmVolume", "Music Volume"), Loc("Info_BGM_Volume", "Adjust music volume.", "Descriptions"),
            Field::MusicVolume, 0, 100, [](const Values& x) { return x.musicVolume; }));
    }
    else
        notYet(audio.rows, "Volume");
    audio.rows.push_back(BoolChoice("Mute in Background", "Silence the game while its window is in the background.",
        "audio.mute_unfocused", false, [] { audio::UpdateVolume(); }));
    pages.push_back(std::move(audio));

    Page controls{ Loc("Game_title", "Controls"), {}, true };
    if (v.valid)
    {
        controls.rows.push_back(GameChoice(Loc("CtrLayout", "Controller Layout"),
            Loc("Info_ControllerLayout", "Adjust controller layout.", "Descriptions") +
                " The layout decides what each button does; the bindings below choose the keys.",
            Field::ControllerLayout,
            { Loc("Default", "Default"), Loc("Custom", "Shooter"), Loc("Acrobat", "Acrobat"), Loc("Ninja", "Ninja") },
            [](const Values& x) { return x.controllerLayout; }));
        controls.rows.push_back(GameChoice(Loc("vibration", "Rumble"), Loc("Info_Vibration", "Toggle rumble.", "Descriptions"),
            Field::Vibration, offOn, [](const Values& x) { return int(x.vibration); }));
    }
    else
        notYet(controls.rows, "Controller Layout");
    controls.rows.push_back(FloatSlider("Mouse Speed",
        "How fast the mouse turns the camera, on top of the game's own Camera sensitivity.", "controls.mouse_sensitivity",
        1.0f, 0.1f, 5.0f, 0.1f, "%.1f"));
    controls.rows.push_back(StringChoice("Face Buttons",
        "Xbox-style: the button labelled A does what the Wii U's A does (confirm, interact, skip), B/X/Y by their letters. "
        "Positional: by position, as on the Wii U (east is A). Your bindings apply on top.",
        "controls.face_buttons", "auto",
        { { "auto", "Auto" }, { "xbox", "Xbox-style" }, { "positional", "Positional" } }));
    controls.rows.push_back(FloatSlider("Stick Dead Zone", "How far a stick must move before the game sees it.",
        "controls.dead_zone", 0.15f, 0.0f, 0.5f, 0.01f, "%.2f"));
    for (size_t i = 0; i < bindings::Targets().size(); i++)
        controls.rows.push_back(BindingRow(i));
    {
        Row reset;
        reset.kind = Row::Action;
        reset.label = "Reset Keyboard and Mouse";
        reset.help = "Restore every keyboard and mouse binding.";
        reset.action = [] { Ask("Reset the keyboard and mouse bindings?", "", [] { bindings::ResetKeyboard(); }); };
        controls.rows.push_back(reset);
        reset.label = "Reset Controller";
        reset.help = "Restore every controller binding.";
        reset.action = [] { Ask("Reset the controller bindings?", "", [] { bindings::ResetController(); }); };
        controls.rows.push_back(reset);
    }
    pages.push_back(std::move(controls));

    Page camera{ Loc("Camera_title", "Camera"), {}, true };
    if (v.valid)
    {
        camera.rows.push_back(GameSlider(Loc("SensitivityX", "Horizontal Sensitivity"),
            Loc("Info_SensitivityX", "Adjust horizontal stick sensitivity.", "Descriptions") + " (Mouse Speed on Controls adds to it.)",
            Field::SensitivityX, 1, 20, [](const Values& x) { return x.sensitivityX; }));
        camera.rows.push_back(GameSlider(Loc("SensitivityY", "Vertical Sensitivity"),
            Loc("Info_SensitivityY", "Adjust vertical stick sensitivity.", "Descriptions"), Field::SensitivityY, 1, 20,
            [](const Values& x) { return x.sensitivityY; }));
        const std::vector<std::string> normalReverse = { Loc("SensNormal", "Normal"), Loc("SensReverse", "Reverse") };
        camera.rows.push_back(GameChoice(Loc("H_Inversion", "Horizontal Inversion"),
            Loc("Info_H_Inversion", "Toggle horizontal inversion.", "Descriptions"), Field::InvertX, normalReverse,
            [](const Values& x) { return int(x.invertX); }));
        camera.rows.push_back(GameChoice(Loc("V_Inversion", "Vertical Inversion"),
            Loc("Info_V_Inversion", "Toggle vertical inversion.", "Descriptions"), Field::InvertY, normalReverse,
            [](const Values& x) { return int(x.invertY); }));
    }
    else
        notYet(camera.rows, "Sensitivity");
    pages.push_back(std::move(camera));

    // The game's Single-player page (difficulty, score) and the assists of its Controls page.
    Page gameplay{ "Gameplay", {}, true };
    gameplay.rows.push_back(StringChoice("Crosshair",
        "Always shows the crosshair, as the game does (for hip fire). When Aiming shows it only while aiming or firing (and a moment after).",
        "gameplay.crosshair", "always", { { "always", "Always" }, { "aiming", "When Aiming" } }));
    gameplay.rows.push_back(StringChoice("Subtitles",
        "Sharp draws subtitles at the screen's resolution in the game's typeface; Original keeps the game's 720p "
        "lettering, which blurs when scaled up.",
        "gameplay.subtitles", "sharp", { { "sharp", "Sharp" }, { "original", "Original" } }));
    gameplay.rows.push_back(StringChoice("Subtitle Size", "The size of sharp subtitles.", "gameplay.subtitle_size", "100",
        { { "100", "100%" }, { "125", "125%" }, { "150", "150%" }, { "175", "175%" }, { "200", "200%" } }));
    if (v.valid)
    {
        const bool multi = v.mode == game_settings::Mode::Multiplayer;
        const std::string difficulty = Loc("Difficulty", "Difficulty");
        if (v.mode == game_settings::Mode::Story || v.mode == game_settings::Mode::FrontEnd)
            gameplay.rows.push_back(GameChoice(difficulty,
                Loc("Difficulty", "Change game difficulty.", "Descriptions") +
                    (v.mode == game_settings::Mode::Story ? " Applies to this mission at once." : ""),
                Field::Difficulty, { Loc("Easy", "Casual", "Items"), Loc("Normal", "Standard", "Items"), Loc("Hard", "Hardcore", "Items") },
                [](const Values& x) { return x.difficulty; }));
        else
            gameplay.rows.push_back(Unavailable(difficulty, multi ? "Not used in multiplayer." : "Fixed in Mission mode."));
        const std::string score = Loc("Score", "Score");
        if (v.mode == game_settings::Mode::ScoreAttack || v.mode == game_settings::Mode::FrontEnd)
            gameplay.rows.push_back(GameChoice(score, Loc("Score", "Toggle score visibility in Mission mode.", "Descriptions"),
                Field::ScoreDisplay, offOn, [](const Values& x) { return int(x.scoreDisplay); }));
        else
            gameplay.rows.push_back(Unavailable(score, "Shown in Mission mode only. Change it from the title or in a mission."));
        if (v.aimAssistAllowed)
            gameplay.rows.push_back(GameChoice(Loc("AimAssist", "Aim Assist"),
                Loc("Info_AimAssist", "Toggle aim assist.", "Descriptions"), Field::AimAssist, offOn,
                [](const Values& x) { return int(x.aimAssist); }));
        else
            gameplay.rows.push_back(Unavailable(Loc("AimAssist", "Aim Assist"),
                "The game offers it only where the game mode allows it (for example in a story mission)."));
        gameplay.rows.push_back(GameChoice(Loc("CoverAssist", "Cover Assist"),
            Loc("Info_CoverAssist", "Assist option to automatically enter any nearby cover.", "Descriptions"), Field::CoverAssist, offOn,
            [](const Values& x) { return int(x.coverAssist); }));
        // Violence: the game's own Options never show it in this build; outside the Japanese version (region 0) gore is
        // always on, so the row is left out there as in the game.
        if (v.region == 0)
        {
            if (v.canUnlockGore)
                gameplay.rows.push_back(GameChoice("Violence", "Show or hide gore.", Field::Gore, offOn,
                    [](const Values& x) { return int(x.gore); }));
            else
                gameplay.rows.push_back(Unavailable("Violence", "Locked until the game's online service unlocks it."));
        }
    }
    else
        notYet(gameplay.rows, "Difficulty");
    pages.push_back(std::move(gameplay));

    Page language{ Loc("Subtitle_title", "Language"), {}, true };
    if (v.valid)
    {
        const bool multi = v.mode == game_settings::Mode::Multiplayer;
        if (!multi)
            language.rows.push_back(GameChoice(Loc("Subtitles", "Subtitles"), Loc("Info_Subtitle", "Toggle subtitles.", "Descriptions"),
                Field::Subtitles, offOn, [](const Values& x) { return int(x.subtitles); }));
        const std::vector<std::string> texts = { Loc("English", "English"), Loc("French", "French"), Loc("Japanese", "Japanese") };
        const std::vector<std::string> voices = { Loc("English", "English"), Loc("Japanese", "Japanese") };
        if (v.canChangeLanguage)
        {
            // A language change saves and returns to the title screen, as in the game's Options: asked first.
            auto languageRow = [&](std::string label, std::string help, std::vector<std::string> choices, bool text) {
                Row r;
                r.kind = Row::Choice;
                r.label = std::move(label), r.help = std::move(help) + " Changing it returns to the title screen.";
                r.choices = std::move(choices);
                r.get = [text] { const Values x = game_settings::Current(); return text ? x.textLanguage : x.voiceLanguage; };
                r.set = [text](int i) {
                    const Values x = game_settings::Current();
                    const int t = text ? i : x.textLanguage, vo = text ? x.voiceLanguage : i;
                    Ask("Change the language and return to the title screen?", "Progress since the last checkpoint is lost.", [t, vo] {
                        game_settings::ChangeLanguage(t, vo);
                        gWePaused = false;  // the reload leaves the pause menu itself
                        SetOpen(false, "language change");
                    });
                };
                const Values d = game_settings::Defaults();
                r.fallback = d.valid ? (text ? d.textLanguage : d.voiceLanguage) : 0;
                return r;
            };
            language.rows.push_back(languageRow(Loc("Language", "Language"), Loc("Info_Language", "Change text language.", "Descriptions"),
                texts, true));
            language.rows.push_back(languageRow(Loc("Voice", "Voice"), Loc("Info_Voice", "Change speech language.", "Descriptions"),
                voices, false));
        }
        else
        {
            const char* why = multi ? "Change it from the title screen, not in multiplayer." : "Fixed by this version of the game.";
            language.rows.push_back(Unavailable(Loc("Language", "Language"), why));
            language.rows.push_back(Unavailable(Loc("Voice", "Voice"), why));
        }
        static const char* const maps[] = { "KBD_US", "KBD_CA_BI", "KBD_CA_FR", "KBD_LA", "KBD_UK", "KBD_FR", "KBD_DE", "KBD_ES",
            "KBD_IT", "KBD_NL", "KBD_PG", "KBD_SW", "KBD_FI", "KBD_NO", "KBD_DA", "KBD_GR", "KBD_JA" };
        std::vector<std::string> mapNames;
        for (const char* m : maps)
            mapNames.push_back(Loc(m, m) + (std::string(m) == "KBD_CA_BI" ? " (bilingual)" : std::string(m) == "KBD_CA_FR" ? " (French)" : ""));
        language.rows.push_back(GameChoice(Loc("KBDlayouts", "USB Keyboard Map"),
            Loc("Info_KeyMap", "Select the keyboard map for your connected USB keyboard.", "Descriptions") +
                " (Text entry in the game's own keyboard.)",
            Field::KeyboardMap, mapNames, [](const Values& x) { return x.keyboardMap; }));
    }
    else
        notYet(language.rows, "Language");
    pages.push_back(std::move(language));
    return pages;
}

// ---- drawing in the game's style, in its 1280x720 layout scaled to the window ----
const ImU32 Amber = IM_COL32(247, 171, 52, 255), Label = IM_COL32(222, 222, 222, 255), Value = IM_COL32(196, 196, 196, 255),
            Dim = IM_COL32(150, 150, 150, 255), Shadow = IM_COL32(0, 0, 0, 170);

struct Canvas
{
    ImDrawList* dl;
    float s, ox, oy;  // scale and offset of the 1280x720 layout
    ImVec2 P(float x, float y) const { return { ox + x * s, oy + y * s }; }
    ImFont* F(void* f) const { return static_cast<ImFont*>(f); }
    ImVec2 Size(void* font, float size, const char* text) const { return F(font)->CalcTextSizeA(size * s, FLT_MAX, 0, text); }
    void Text(void* font, float size, float x, float y, ImU32 col, const char* text, bool shadow = true) const
    {
        if (shadow)
            dl->AddText(F(font), size * s, P(x + 1.5f, y + 2), Shadow, text);
        dl->AddText(F(font), size * s, P(x, y), col, text);
    }
    void TextCentered(void* font, float size, float cx, float y, ImU32 col, const char* text, bool shadow = true) const
    {
        Text(font, size, cx - Size(font, size, text).x / s / 2, y, col, text, shadow);
    }
    // A soft amber glow: a horizontal band fading out at both ends.
    void Glow(float x0, float x1, float y0, float y1, int alpha) const
    {
        const float mid = (x0 + x1) / 2;
        const ImU32 col = IM_COL32(247, 150, 40, alpha), clear = IM_COL32(247, 150, 40, 0);
        dl->AddRectFilledMultiColor(P(x0, y0), P(mid, y1), clear, col, col, clear);
        dl->AddRectFilledMultiColor(P(mid, y0), P(x1, y1), col, clear, clear, col);
    }
    bool Hover(float x0, float y0, float x1, float y1) const { return ImGui::IsMouseHoveringRect(P(x0, y0), P(x1, y1), false); }
};

// ---- button prompts (host/prompts), in the game's style: white face buttons and caps with dark text ----
using prompts::Glyph;
constexpr ImU32 GlyphFill = IM_COL32(245, 245, 245, 255), GlyphInk = IM_COL32(12, 12, 12, 255);

float GlyphWidth(const Canvas& c, float h, const Glyph& g)
{
    const auto& f = gpu::overlay::MenuFonts();
    switch (g.shape)
    {
    case Glyph::Face: case Glyph::Dpad: return h;
    case Glyph::Mouse: case Glyph::Wheel: return h * 0.72f;
    case Glyph::Cap: case Glyph::Key:
        return std::max(h * (g.shape == Glyph::Cap ? 1.25f : 1.0f), c.Size(f.value, h * 0.52f, g.text.c_str()).x / c.s + h * 0.5f);
    default: return 0;
    }
}

// Centred on (cx, cy), `h` tall, in the 1280x720 layout.
void DrawGlyph(const Canvas& c, float cx, float cy, float h, const Glyph& g)
{
    const auto& f = gpu::overlay::MenuFonts();
    const float w = GlyphWidth(c, h, g), r = h / 2;
    auto text = [&](float size) {
        c.dl->AddText(c.F(f.value), size * c.s, c.P(cx - c.Size(f.value, size, g.text.c_str()).x / c.s / 2, cy - size * 0.56f), GlyphInk,
            g.text.c_str());
    };
    switch (g.shape)
    {
    case Glyph::Face:
        c.dl->AddCircleFilled(c.P(cx, cy), r * c.s, GlyphFill, 24);
        if (g.symbol)  // PlayStation's shapes
        {
            const float k = r * 0.48f, t = 2.2f * c.s;
            if (g.symbol == 1)
                c.dl->AddLine(c.P(cx - k, cy - k), c.P(cx + k, cy + k), GlyphInk, t), c.dl->AddLine(c.P(cx + k, cy - k), c.P(cx - k, cy + k), GlyphInk, t);
            else if (g.symbol == 2)
                c.dl->AddCircle(c.P(cx, cy), k * 1.1f * c.s, GlyphInk, 20, t);
            else if (g.symbol == 3)
                c.dl->AddRect(c.P(cx - k, cy - k), c.P(cx + k, cy + k), GlyphInk, 0, t);
            else
                c.dl->AddTriangle(c.P(cx, cy - k * 1.15f), c.P(cx + k * 1.1f, cy + k * 0.8f), c.P(cx - k * 1.1f, cy + k * 0.8f), GlyphInk, t);
        }
        else
            text(h * 0.62f);
        break;
    case Glyph::Cap:
        c.dl->AddRectFilled(c.P(cx - w / 2, cy - r), c.P(cx + w / 2, cy + r), GlyphFill, r * 0.55f * c.s);
        text(g.text.size() > 3 ? h * 0.42f : h * 0.56f);
        break;
    case Glyph::Key:
        c.dl->AddRectFilled(c.P(cx - w / 2, cy - r), c.P(cx + w / 2, cy + r), GlyphFill, 3 * c.s);
        c.dl->AddRect(c.P(cx - w / 2 + 2, cy - r + 2), c.P(cx + w / 2 - 2, cy + r - 4), IM_COL32(170, 170, 170, 255), 2 * c.s, c.s);
        text(h * 0.5f);
        break;
    case Glyph::Dpad:
    {
        const float a = h * 0.18f;
        for (int d = 1; d <= 4; d++)
        {
            const float dx = d == 3 ? -1.f : d == 4 ? 1.f : 0.f, dy = d == 1 ? -1.f : d == 2 ? 1.f : 0.f;
            const float ax = cx + dx * r * 0.62f, ay = cy + dy * r * 0.62f;
            c.dl->AddRectFilled(c.P(ax - a, ay - a), c.P(ax + a, ay + a), d == g.dir ? Amber : GlyphFill, 1.5f * c.s);
        }
        c.dl->AddRectFilled(c.P(cx - a, cy - a), c.P(cx + a, cy + a), GlyphFill);
        break;
    }
    case Glyph::Mouse: case Glyph::Wheel:
    {
        const float x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - r, y1 = cy + r, mid = y0 + h * 0.42f;
        c.dl->AddRectFilled(c.P(x0, y0), c.P(x1, y1), GlyphFill, w * 0.45f * c.s);
        if (g.shape == Glyph::Mouse && (g.button == 1 || g.button == 3))
        {
            const bool leftSide = g.button == 1;
            c.dl->AddRectFilled(c.P(leftSide ? x0 : cx, y0), c.P(leftSide ? cx : x1, mid), Amber,
                w * 0.45f * c.s, leftSide ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTopRight);
        }
        c.dl->AddLine(c.P(x0, mid), c.P(x1, mid), GlyphInk, 1.2f * c.s);
        c.dl->AddLine(c.P(cx, y0), c.P(cx, mid), GlyphInk, 1.2f * c.s);
        const bool wheel = g.shape == Glyph::Wheel || g.button == 2;
        c.dl->AddRectFilled(c.P(cx - w * 0.09f, y0 + h * 0.1f), c.P(cx + w * 0.09f, y0 + h * 0.32f), wheel ? Amber : GlyphInk, 2 * c.s);
        if (g.shape == Glyph::Wheel)
        {
            const float ty = g.dir == 1 ? y0 - h * 0.12f : y1 + h * 0.12f, s2 = h * 0.12f * (g.dir == 1 ? 1.f : -1.f);
            c.dl->AddTriangleFilled(c.P(cx, ty - s2), c.P(cx + h * 0.12f, ty + s2), c.P(cx - h * 0.12f, ty + s2), Amber);
        }
        if (g.shape == Glyph::Mouse && g.button >= 4)
            text(h * 0.4f);  // (side buttons: shown by number below)
        break;
    }
    default:
        break;
    }
    if (g.shape == Glyph::Mouse && g.button >= 4)
        c.Text(f.value, h * 0.45f, cx + w / 2 + 2, cy - h * 0.3f, GlyphFill, std::to_string(g.button).c_str(), false);
}

input::Device Device() { return input::ActiveDevice(); }
bool UsingPad() { return Device() != input::Device::Keyboard; }

// The page's own buttons as prompts: on a controller its convention's buttons (MenuPad), on the keyboard its keys.
Glyph PromptFor(SDL_GamepadButton padButton, const char* key)
{
    if (!UsingPad())
        return { Glyph::Key, key };
    return prompts::For(bindings::Make(bindings::PadButton, padButton), Device());
}
Glyph ConfirmPrompt() { return PromptFor(!bindings::FaceByLabel(Device()) ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, "Enter"); }
Glyph BackPrompt() { return PromptFor(!bindings::FaceByLabel(Device()) ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, "Backspace"); }
Glyph DefaultPrompt() { return PromptFor(!bindings::FaceByLabel(Device()) ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH, "Del"); }

int gPage = 0;
std::vector<int> gRow, gScroll;
int gDragging = -1;

// Keys and controller buttons arrive through ImGui's IO (the overlay feeds the controller in).
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true)
{
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k, repeat))
            return true;
    return false;
}

bool MouseMoved() { return ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0; }

// Binding cells: keyboard slots 0 and 1, the controller 2 (none for the keyboard-only stick directions).
int Columns(const Row& r) { return r.kind == Row::Binding && !bindings::Targets()[r.target].stick ? 3 : 2; }

uint32_t CellInput(const Row& r, int column)
{
    const auto t = bindings::Current();
    const bindings::Binding& b = (*t)[r.target];
    return column < bindings::KeySlots ? b.keys[column] : bindings::PadInput(*t, r.target, input::PadDevice());
}

// Bind `input` to the cell, asking first when another action already uses it on that device.
void Assign(size_t target, int slot, uint32_t input)
{
    const auto t = bindings::Current();
    if ((slot < bindings::KeySlots ? (*t)[target].keys[slot] : bindings::PadInput(*t, target, input::PadDevice())) == input)
        return;
    size_t other;
    int otherSlot;
    if (bindings::FindUse(input, target, slot, other, otherSlot))
    {
        if (other == target)  // the row's other keyboard slot: just swap them
        {
            const uint32_t mine = (*t)[target].keys[slot];
            bindings::Set(target, otherSlot, mine);
            bindings::Set(target, slot, input);
            Play(SoundOk);
            return;
        }
        Ask(bindings::Name(input) + " is already used for " + bindings::ActionName(other) + ".",
            "Use it for " + bindings::ActionName(target) + " instead?", [=] {
                bindings::Set(other, otherSlot, 0);
                bindings::Set(target, slot, input);
            });
        return;
    }
    bindings::Set(target, slot, input);
    Play(SoundOk);
}

void Change(Row& r, int delta)
{
    if (r.kind == Row::Choice && r.choices.size() > 1)
        r.set((r.get() + delta + int(r.choices.size())) % int(r.choices.size())), Play(SoundCursor);
    else if (r.kind == Row::Slider)
    {
        const float v = std::clamp(std::round((r.getValue() + float(delta) * r.step) / r.step) * r.step, r.lo, r.hi);
        if (v != r.getValue())
            r.setValue(v), Play(SoundCursor);
    }
    else if (r.kind == Row::Binding)
    {
        const int column = std::clamp(gColumn + delta, 0, Columns(r) - 1);
        if (column != gColumn)
            gColumn = column, Play(SoundCursor);
    }
}

void Reset(Row& r)
{
    if (r.kind == Row::Choice && r.get() != r.fallback)
        r.set(r.fallback), Play(SoundOk);
    else if (r.kind == Row::Slider && r.getValue() != r.fallbackValue)
        r.setValue(r.fallbackValue), Play(SoundOk);
    else if (r.kind == Row::Binding)
    {
        if (gColumn >= bindings::KeySlots)
        {
            if ((*bindings::Current())[r.target].padSet)
                bindings::ResetPad(r.target), Play(SoundOk);
            return;
        }
        const bindings::Binding& d = bindings::Defaults()[r.target];
        const uint32_t input = d.keys[gColumn];
        if (input)
            Assign(r.target, gColumn, input);
        else if (CellInput(r, gColumn))
            bindings::Set(r.target, gColumn, 0), Play(SoundOk);
    }
}

void Confirm(Row& r)
{
    if (r.kind == Row::Action)
        r.action();
    else if (r.kind == Row::Binding)
    {
        gCaptureTarget = r.target, gCaptureSlot = gColumn;
        gCaptureUntil = ImGui::GetTime() + 8;
        bindings::BeginCapture(r.target, gColumn);
        Play(SoundOk);
    }
    else
        Change(r, +1);
}

// A white panel with gold accents, as the game's dialogs.
void DrawPanel(const Canvas& c, const char* title)
{
    const auto& f = gpu::overlay::MenuFonts();
    c.dl->AddRectFilled(c.P(0, 0), c.P(1280, 720), IM_COL32(0, 0, 0, 110));
    c.dl->AddRectFilled(c.P(280, 220), c.P(1000, 500), IM_COL32(243, 243, 243, 250), 8 * c.s);
    c.dl->AddLine(c.P(280, 254), c.P(1000, 254), IM_COL32(150, 150, 150, 120), c.s);
    c.dl->AddLine(c.P(280, 467), c.P(1000, 467), IM_COL32(150, 150, 150, 120), c.s);
    c.dl->AddQuadFilled(c.P(297, 237), c.P(308, 226), c.P(319, 237), c.P(308, 248), IM_COL32(184, 134, 11, 255));
    c.Text(f.value, 22, 326, 225, IM_COL32(30, 30, 30, 255), title, false);
}

// The game's confirmation dialog: Yes / No pills.
void DrawDialog(const Canvas& c)
{
    const auto& f = gpu::overlay::MenuFonts();
    DrawPanel(c, gDialog.title.c_str());
    const ImU32 text = IM_COL32(110, 110, 110, 255);
    std::vector<const std::string*> lines{ &gDialog.line1 };
    if (!gDialog.line2.empty())
        lines.push_back(&gDialog.line2);
    for (const std::string& l : gDialog.more)
        lines.push_back(&l);
    const float spacing = lines.size() > 2 ? 26.0f : 33.0f;
    const float top = 333.0f - spacing * float(lines.size() - 1) / 2 - 17;
    for (size_t i = 0; i < lines.size(); i++)
        c.TextCentered(f.label, 22, 640, top + spacing * float(i), text, lines[i]->c_str(), false);
    const char* names[] = { "Yes", "No" };
    for (int i = 0; i < 2; i++)
    {
        const float x0 = i == 0 ? 442.0f : 654.0f, x1 = x0 + 176;
        if (c.Hover(x0, 410, x1, 450) && MouseMoved() && gDialog.choice != i)
            gDialog.choice = i, Play(SoundCursor);
        const bool sel = gDialog.choice == i;
        if (sel)
        {
            c.dl->AddRect(c.P(x0 - 4, 406), c.P(x1 + 4, 454), IM_COL32(230, 200, 110, 255), 24 * c.s, 2 * c.s);
            c.dl->AddRectFilled(c.P(x0, 410), c.P(x1, 450), IM_COL32(186, 134, 6, 255), 20 * c.s);
        }
        else
            c.dl->AddRect(c.P(x0, 410), c.P(x1, 450), IM_COL32(170, 170, 170, 255), 20 * c.s, 3 * c.s);
        c.TextCentered(f.value, 24, (x0 + x1) / 2, 416, sel ? IM_COL32(255, 220, 0, 255) : IM_COL32(150, 150, 150, 255), names[i],
            false);
    }
    if (Pressed({ ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_A, ImGuiKey_D, ImGuiKey_GamepadDpadLeft,
            ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight }))
        gDialog.choice ^= 1, Play(SoundCursor);
    const bool clicked = ImGui::IsMouseClicked(0) && (c.Hover(442, 410, 618, 450) || c.Hover(654, 410, 830, 450));
    if (Pressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown }, false) || clicked)
    {
        Dialog d = std::move(gDialog);
        gDialog = {};
        Play(d.choice == 0 ? SoundOk : SoundCancel);
        if (d.choice == 0 && d.yes)
            d.yes();
    }
    else if (Pressed({ ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight }, false))
        gDialog = {}, Play(SoundCancel);
}

// Waiting for the key or button to bind.
void DrawCapture(const Canvas& c)
{
    const auto& f = gpu::overlay::MenuFonts();
    DrawPanel(c, "Change Binding");
    const ImU32 text = IM_COL32(110, 110, 110, 255);
    const std::string action = bindings::ActionName(gCaptureTarget);
    c.TextCentered(f.value, 26, 640, 290, IM_COL32(186, 134, 6, 255), action.c_str(), false);
    c.TextCentered(f.label, 22, 640, 345, text,
        gCaptureSlot < bindings::KeySlots ? "Press a key or mouse button." : "Press a controller button or trigger.", false);
    c.TextCentered(f.label, 20, 640, 420, Dim, "Escape cancels.", false);
}

void HandleCapture()
{
    uint32_t input;
    if (bindings::TakeCaptured(input))
    {
        gWaitPadRelease = true;
        if (input)
            Assign(gCaptureTarget, gCaptureSlot, input);
        else
            Play(SoundCancel);
    }
    else if (bindings::Capturing() && ImGui::GetTime() > gCaptureUntil)
        bindings::CancelCapture();
}

// The value side of a row in the standard (two-line) layout.
void DrawStandardRows(const Canvas& c, Page& page, int& row, bool modal)
{
    const auto& f = gpu::overlay::MenuFonts();
    const ImGuiIO& io = ImGui::GetIO();
    float y = 182;
    for (size_t i = 0; i < page.rows.size(); i++, y += 82)
    {
        Row& r = page.rows[i];
        const bool sel = int(i) == row;
        if (!modal && c.Hover(60, y, 1180, y + 74) && MouseMoved())
            row = int(i);
        if (sel)
            c.Glow(20, r.kind == Row::Slider ? 1180.0f : 520.0f, y + 4, y + 30, 60);
        c.Text(f.label, 24, 82, y, sel ? Amber : (r.kind == Row::Info ? Dim : Label), r.label.c_str());
        if (r.kind == Row::Choice || r.kind == Row::Info)
        {
            const std::string v = r.kind == Row::Choice ? r.choices[size_t(std::clamp(r.get(), 0, int(r.choices.size()) - 1))]
                                                        : (r.choices.empty() ? std::string() : r.choices[0]);
            if (r.kind == Row::Choice)
            {
                const ImU32 arrow = sel ? IM_COL32(230, 230, 230, 255) : Dim;
                c.Text(f.tab, 18, 72, y + 33, arrow, "<", false);
                c.Text(f.tab, 18, 250, y + 33, arrow, ">", false);
                if (!modal && sel && ImGui::IsMouseClicked(0))
                {
                    if (c.Hover(60, y + 28, 110, y + 60))
                        Change(r, -1);
                    else if (c.Hover(220, y + 28, 280, y + 60))
                        Change(r, +1);
                }
            }
            c.TextCentered(f.value, 20, 165, y + 32, sel ? IM_COL32(245, 245, 245, 255) : Value, v.c_str());
        }
        else if (r.kind == Row::Slider)
        {
            const float x0 = 410, x1 = 1100, ly = y + 13;
            const ImU32 line = IM_COL32(200, 200, 200, 200);
            c.dl->AddLine(c.P(x0, ly), c.P(x1, ly), line, 1.2f * c.s);
            c.dl->AddLine(c.P(x0, ly - 6), c.P(x0, ly + 6), line, 1.2f * c.s);
            c.dl->AddLine(c.P(x1, ly - 6), c.P(x1, ly + 6), line, 1.2f * c.s);
            const float t = std::clamp((r.getValue() - r.lo) / (r.hi - r.lo), 0.0f, 1.0f);
            const float tx = x0 + (x1 - x0) * t;
            c.dl->AddRectFilled(c.P(tx - 4, ly - 12), c.P(tx + 4, ly + 12), IM_COL32(245, 245, 245, 255), 1.5f * c.s);
            char buf[32];
            snprintf(buf, sizeof(buf), r.format, r.getValue());
            c.Text(f.tab, 20, 1112, y, IM_COL32(240, 240, 240, 255), buf);
            if (!modal && ImGui::IsMouseClicked(0) && c.Hover(x0 - 10, ly - 16, x1 + 10, ly + 16))
                gDragging = int(i), row = int(i);
            if (gDragging == int(i))
            {
                if (ImGui::IsMouseDown(0))
                {
                    const float mx = (io.MousePos.x - c.ox) / c.s;
                    r.setValue(std::round((r.lo + (r.hi - r.lo) * std::clamp((mx - x0) / (x1 - x0), 0.0f, 1.0f)) / r.step) * r.step);
                }
                else
                    gDragging = -1;
            }
        }
        else if (r.kind == Row::Action && !modal && ImGui::IsMouseClicked(0) && c.Hover(60, y, 520, y + 34))
            r.action();
    }
}

// The Controls tab: one line per row, scrolled to keep the selection in view. Binding rows show the action (from the
// player's game layout), the GamePad button, two keyboard and mouse cells and a controller cell.
constexpr float CompactTop = 196, CompactRow = 39;
constexpr int CompactVisible = 11;
const float CellX[3] = { 560, 760, 990 }, CellW[3] = { 185, 185, 190 };

void DrawCompactRows(const Canvas& c, Page& page, int& row, int& scroll, bool modal)
{
    const auto& f = gpu::overlay::MenuFonts();
    const ImGuiIO& io = ImGui::GetIO();
    const int count = int(page.rows.size());
    scroll = std::clamp(scroll, std::max(0, row - CompactVisible + 1), std::min(row, std::max(0, count - CompactVisible)));
    if (!modal && ImGui::IsMouseHoveringRect(c.P(0, CompactTop), c.P(1280, CompactTop + CompactRow * CompactVisible)) && io.MouseWheel != 0)
    {
        scroll = std::clamp(scroll - int(io.MouseWheel), 0, std::max(0, count - CompactVisible));
        row = std::clamp(row, scroll, scroll + CompactVisible - 1);
    }
    if (std::any_of(page.rows.begin(), page.rows.end(), [](const Row& r) { return r.kind == Row::Binding; }))
    {
        c.TextCentered(f.label, 18, (CellX[0] + CellX[1] + CellW[1]) / 2, 170, Dim, "Keyboard / Mouse");
        c.TextCentered(f.label, 18, CellX[2] + CellW[2] / 2, 170, Dim, "Controller");
    }
    if (scroll > 0)
        c.TextCentered(f.tab, 16, 640, 176, Dim, "^");
    if (scroll + CompactVisible < count)
        c.TextCentered(f.tab, 16, 640, CompactTop + CompactRow * CompactVisible - 2, Dim, "v");

    for (int i = scroll; i < std::min(count, scroll + CompactVisible); i++)
    {
        Row& r = page.rows[size_t(i)];
        const float y = CompactTop + CompactRow * float(i - scroll);
        const bool sel = i == row;
        if (!modal && c.Hover(60, y, 1200, y + CompactRow) && MouseMoved())
        {
            row = i;
            if (r.kind == Row::Binding)
                for (int k = 0; k < Columns(r); k++)
                    if (c.Hover(CellX[k], y, CellX[k] + CellW[k], y + CompactRow - 6) && gColumn != k)
                        gColumn = k, Play(SoundCursor);
        }
        if (sel)
            c.Glow(20, r.kind == Row::Binding || r.kind == Row::Slider ? 1200.0f : 640.0f, y + 2, y + CompactRow - 6, 50);
        const ImU32 labelCol = sel ? Amber : (r.kind == Row::Info ? Dim : Label);
        if (r.kind == Row::Binding)
        {
            const bindings::Target& t = bindings::Targets()[r.target];
            c.Text(f.label, 22, 82, y + 4, labelCol, bindings::ActionName(r.target).c_str());
            c.Text(f.value, 16, 400, y + 9, Dim, bindings::ButtonName(r.target).c_str());
            for (int k = 0; k < 3; k++)
            {
                const float x0 = CellX[k], x1 = x0 + CellW[k];
                const bool cell = sel && k == gColumn;
                if (k == 2 && t.stick)
                {
                    c.TextCentered(f.value, 17, (x0 + x1) / 2, y + 8, IM_COL32(110, 110, 110, 255), "Left Stick");
                    continue;
                }
                const uint32_t input = CellInput(r, k);
                if (cell)
                {
                    c.dl->AddRectFilled(c.P(x0, y + 2), c.P(x1, y + CompactRow - 6), IM_COL32(186, 134, 6, 230), 4 * c.s);
                    c.dl->AddRect(c.P(x0 - 2, y), c.P(x1 + 2, y + CompactRow - 4), IM_COL32(230, 200, 110, 255), 5 * c.s, 1.5f * c.s);
                }
                else
                    c.dl->AddRect(c.P(x0, y + 2), c.P(x1, y + CompactRow - 6), IM_COL32(255, 255, 255, 40), 4 * c.s, c.s);
                if (input)
                {
                    // The bound input as its prompt; a cap too long for the cell is shown as text.
                    Glyph g = prompts::For(input, k < bindings::KeySlots ? input::Device::Keyboard : input::PadDevice());
                    if (GlyphWidth(c, 24, g) < CellW[k] - 12)
                        DrawGlyph(c, (x0 + x1) / 2, y + 16, 24, g);
                    else
                        c.TextCentered(f.value, 17, (x0 + x1) / 2, y + 8, cell ? IM_COL32(255, 230, 120, 255) : Value, bindings::Name(input).c_str());
                }
                else
                    c.TextCentered(f.value, 17, (x0 + x1) / 2, y + 8, IM_COL32(110, 110, 110, 255), "-");
                if (!modal && ImGui::IsMouseClicked(0) && c.Hover(x0, y, x1, y + CompactRow - 6))
                    row = i, gColumn = k, Confirm(r);
            }
        }
        else
        {
            c.Text(f.label, 22, 82, y + 4, labelCol, r.label.c_str());
            if (r.kind == Row::Info || r.kind == Row::Choice)
            {
                const std::string v = r.kind == Row::Choice ? r.choices[size_t(std::clamp(r.get(), 0, int(r.choices.size()) - 1))]
                                                            : (r.choices.empty() ? std::string() : r.choices[0]);
                c.Text(f.value, 19, CellX[0], y + 7, sel ? IM_COL32(245, 245, 245, 255) : Value, v.c_str());
            }
            else if (r.kind == Row::Slider)
            {
                const float x0 = CellX[0], x1 = 1100, ly = y + 17;
                const ImU32 line = IM_COL32(200, 200, 200, 200);
                c.dl->AddLine(c.P(x0, ly), c.P(x1, ly), line, 1.2f * c.s);
                const float t = std::clamp((r.getValue() - r.lo) / (r.hi - r.lo), 0.0f, 1.0f);
                const float tx = x0 + (x1 - x0) * t;
                c.dl->AddRectFilled(c.P(tx - 4, ly - 10), c.P(tx + 4, ly + 10), IM_COL32(245, 245, 245, 255), 1.5f * c.s);
                char buf[32];
                snprintf(buf, sizeof(buf), r.format, r.getValue());
                c.Text(f.tab, 19, 1112, y + 6, IM_COL32(240, 240, 240, 255), buf);
                if (!modal && ImGui::IsMouseClicked(0) && c.Hover(x0 - 10, ly - 14, x1 + 10, ly + 14))
                    gDragging = i, row = i;
                if (gDragging == i)
                {
                    if (ImGui::IsMouseDown(0))
                    {
                        const float mx = (io.MousePos.x - c.ox) / c.s;
                        r.setValue(std::round((r.lo + (r.hi - r.lo) * std::clamp((mx - x0) / (x1 - x0), 0.0f, 1.0f)) / r.step) * r.step);
                    }
                    else
                        gDragging = -1;
                }
            }
            else if (r.kind == Row::Action && !modal && ImGui::IsMouseClicked(0) && c.Hover(60, y, 640, y + CompactRow))
                r.action();
        }
    }
}

// ---- the Score Attack loadout picker (OpenLoadoutPicker): a vertical list over the game's loadout screen ----
// Primary and Secondary: weapon types, then that type's weapons (each once), then a weapon's variants in a dialog.
// Melee: the weapons. X-gear rows are light blue, as the HUD shows those weapons.
const char* const LoadoutCategories[] = { "Pistols", "Assault Rifles", "SMGs", "Shotguns", "Machine Guns", "Sniper Rifles",
    "Launchers", "X-Gear" };
constexpr ImU32 XGearBlue = IM_COL32(110, 205, 235, 255), XGearBright = IM_COL32(170, 235, 255, 255);

struct Picker
{
    int slot = 0;
    std::vector<LoadoutEntry> entries;
    int current = -1;            // the slot's entry index now
    int level = 0;               // 0 types, 1 weapons, 2 variants
    int category = 0;            // the type shown at level 1
    std::string weapon;          // the weapon whose variants show at level 2
    int cursor[3] = { 0, 0, 0 };
    int scroll[3] = { 0, 0, 0 };
    int frames = 0;  // drawn so far: input is ignored on the first ones (the press that opened it may still count)
};
Picker gPicker;
std::optional<Picker> gPendingPicker;  // (under gNoticeMutex)
std::atomic<bool> gPickerOnly{ false };
std::mutex gPickMutex;
std::optional<std::pair<int, int>> gPick;  // slot, entry index

std::vector<int> PickerCategories(const Picker& p)
{
    std::vector<int> out;
    for (int c = 0; c < int(std::size(LoadoutCategories)); c++)
        for (const LoadoutEntry& e : p.entries)
            if (e.category == c)
            {
                out.push_back(c);
                break;
            }
    return out;
}

// The weapons (one row per weapon) of a type, or all of them for Melee (category -1).
std::vector<std::string> PickerWeapons(const Picker& p, int category)
{
    std::vector<std::string> out;
    for (const LoadoutEntry& e : p.entries)
        if ((category < 0 || e.category == category) && std::find(out.begin(), out.end(), e.weapon) == out.end())
            out.push_back(e.weapon);
    return out;
}

std::vector<const LoadoutEntry*> PickerVariants(const Picker& p, const std::string& weapon, int category)
{
    std::vector<const LoadoutEntry*> out;
    for (const LoadoutEntry& e : p.entries)
        if (e.weapon == weapon && (category < 0 || e.category == category))
            out.push_back(&e);
    return out;
}

void PickerPick(int index)
{
    {
        std::lock_guard lock(gPickMutex);
        gPick = std::pair{ gPicker.slot, index };
    }
    Play(SoundOk);
    SetOpen(false, "loadout picked");
}

// A weapon's picture (the weapon library's art, as the loadout slots show it: the default parts and the variant's
// underbarrel part), made into an overlay texture once (null if the library or the sprite is missing).
ImTextureData* WeaponPicture(const std::string& gfx, const std::string& part)
{
    static std::map<std::string, ImTextureData*> cache;
    if (gfx.empty() || !swf_icons::HaveLibrary())
        return nullptr;
    const std::string key = gfx + "|" + part;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    const swf_icons::Picture pic =
        swf_icons::Render(gfx, { "base", "mag_default", "muzzle_default", "sight_default", part }, 300, 100);
    ImTextureData* tex = nullptr;
    if (pic.w > 0)
    {
        tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, pic.w, pic.h);
        memcpy(tex->GetPixels(), pic.rgba.data(), pic.rgba.size());
        ImGui::RegisterUserTexture(tex);
    }
    cache[key] = tex;
    return tex;
}

void DrawPicker(const Canvas& c)
{
    const auto& f = gpu::overlay::MenuFonts();
    Picker& p = gPicker;
    const bool melee = p.slot == 2;
    const int top = melee ? 1 : 0;  // (Melee has no types)
    const int category = melee ? -1 : p.category;
    const std::vector<int> categories = PickerCategories(p);
    const std::vector<std::string> weapons = p.level >= 1 ? PickerWeapons(p, category) : std::vector<std::string>{};
    const std::vector<const LoadoutEntry*> variants =
        p.level == 2 ? PickerVariants(p, p.weapon, category) : std::vector<const LoadoutEntry*>{};
    const LoadoutEntry* now = nullptr;
    for (const LoadoutEntry& e : p.entries)
        if (e.index == p.current)
            now = &e;

    // The panel, at the right of the game's slots.
    constexpr float X0 = 838, X1 = 1250, Y0 = 36, Y1 = 690, RowTop = 136;
    const float rowH = p.level == 0 ? 46.0f : 62.0f;  // (weapon rows hold a picture)
    const int visible = int((Y1 - 70 - RowTop) / rowH);
    c.dl->AddRectFilled(c.P(X0, Y0), c.P(X1, Y1), IM_COL32(10, 9, 8, 236), 6 * c.s);
    c.dl->AddRect(c.P(X0, Y0), c.P(X1, Y1), IM_COL32(186, 134, 6, 160), 6 * c.s, 1.5f * c.s);
    const char* slotName = p.slot == 0 ? "Primary" : p.slot == 1 ? "Secondary" : "Melee";
    c.Text(f.value, 36, X0 + 28, Y0 + 12, IM_COL32(222, 212, 196, 255), slotName);  // (the game's heading face, as "Solo-play")
    const std::string crumb = p.level == 0 ? "Choose a weapon type"
        : p.level == 2                     ? p.weapon + ": choose a variant"
        : melee                            ? "Choose a weapon"
                                           : LoadoutCategories[p.category];
    c.Text(f.label, 22, X0 + 30, Y0 + 62, Dim, crumb.c_str());
    c.dl->AddLine(c.P(X0 + 20, RowTop - 10), c.P(X1 - 20, RowTop - 10), IM_COL32(186, 134, 6, 140), 1.2f * c.s);

    const int count = p.level == 0 ? int(categories.size()) : p.level == 1 ? int(weapons.size()) : int(variants.size());
    int& cursor = p.cursor[p.level];
    int& scroll = p.scroll[p.level];
    cursor = std::clamp(cursor, 0, std::max(0, count - 1));
    if (Pressed({ ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp }) && count)
        cursor = (cursor + count - 1) % count, Play(SoundCursor);
    if (Pressed({ ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown }) && count)
        cursor = (cursor + 1) % count, Play(SoundCursor);
    // The mouse wheel moves the selection a row per notch (the list scrolls with it).
    static float wheelRest = 0;
    wheelRest += ImGui::GetIO().MouseWheel;
    const int notches = int(wheelRest);
    wheelRest -= float(notches);
    if (notches && count)
    {
        const int moved = std::clamp(cursor - notches, 0, count - 1);
        if (moved != cursor)
            cursor = moved, Play(SoundCursor);
    }
    scroll = std::clamp(scroll, cursor - visible + 1, cursor);
    scroll = std::clamp(scroll, 0, std::max(0, count - visible));
    bool activate = false;
    for (int i = scroll; i < std::min(count, scroll + visible); i++)
    {
        const float y = RowTop + rowH * float(i - scroll);
        const bool sel = i == cursor;
        if (c.Hover(X0 + 10, y, X1 - 10, y + rowH - 4) && MouseMoved() && cursor != i)
            cursor = i, Play(SoundCursor);
        if (ImGui::IsMouseClicked(0) && c.Hover(X0 + 10, y, X1 - 10, y + rowH - 4))
            cursor = i, activate = true;
        std::string label, right;
        bool xgear = false, equipped = false;
        const LoadoutEntry* picture = nullptr;  // the entry whose picture the row shows
        if (p.level == 0)
        {
            const int cat = categories[size_t(i)];
            label = LoadoutCategories[cat];
            xgear = cat == 7;
            right = std::to_string(PickerWeapons(p, cat).size());
            equipped = now && now->category == cat;
        }
        else if (p.level == 1)
        {
            label = weapons[size_t(i)];
            const auto list = PickerVariants(p, label, category);
            xgear = !list.empty() && list[0]->xgear;
            if (list.size() > 1)
                right = std::to_string(list.size()) + " variants";
            equipped = now && now->weapon == label && (category < 0 || now->category == category);
            // (the weapon's equipped variant if it is this slot's weapon, else its standard one)
            for (const LoadoutEntry* e : list)
                if ((equipped && e == now) || (!picture && e->variant == "Standard"))
                    picture = e;
            if (!picture && !list.empty())
                picture = list[0];
            if (equipped)
                picture = now;
        }
        else
        {
            picture = variants[size_t(i)];
            label = picture->variant;
            xgear = picture->xgear;
            equipped = picture->index == p.current;
        }
        if (sel)
            c.Glow(X0 + 6, X1 - 6, y + 4, y + rowH - 8, 70);
        if (equipped)
            c.dl->AddQuadFilled(c.P(X0 + 18, y + rowH / 2 - 4), c.P(X0 + 24, y + rowH / 2 - 10), c.P(X0 + 30, y + rowH / 2 - 4),
                c.P(X0 + 24, y + rowH / 2 + 2), IM_COL32(184, 134, 11, 255));
        const ImU32 col = xgear ? (sel ? XGearBright : XGearBlue) : sel ? Amber : Label;
        float textX = X0 + 42;
        if (p.level > 0)
        {
            // The picture: 150 x 50, as the slots show it (X-gear in their light blue).
            if (ImTextureData* tex = picture ? WeaponPicture(picture->gfx, picture->part) : nullptr)
            {
                const ImU32 tint = xgear ? IM_COL32(120, 215, 240, 255) : sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 205, 205, 255);
                c.dl->AddImage(tex->GetTexRef(), c.P(X0 + 40, y + 5), c.P(X0 + 190, y + 55), ImVec2(0, 0), ImVec2(1, 1), tint);
            }
            textX = X0 + 202;
        }
        const float textSize = p.level == 0 ? 24.0f : 22.0f;
        c.Text(f.label, textSize, textX, y + (rowH - textSize) / 2 - 4, col, label.c_str());
        if (!right.empty())
            c.Text(f.value, 16, X1 - 24 - c.Size(f.value, 16, right.c_str()).x / c.s, y + rowH - 26, xgear ? XGearBlue : Dim,
                right.c_str());
    }
    if (count > visible)  // (more above or below)
    {
        if (scroll > 0)
            c.TextCentered(f.tab, 18, (X0 + X1) / 2, RowTop - 34, Dim, "^", false);
        if (scroll + visible < count)
            c.TextCentered(f.tab, 18, (X0 + X1) / 2, RowTop + rowH * float(visible) - 4, Dim, "v", false);
    }

    // Select and Back, as the device in use has them.
    {
        float x = X1 - 30;
        for (auto [glyph, label] : { std::pair{ BackPrompt(), "Back" }, std::pair{ ConfirmPrompt(), "Select" } })
        {
            x -= c.Size(f.value, 22, label).x / c.s;
            c.Text(f.value, 22, x, Y1 - 44, IM_COL32(240, 240, 240, 255), label);
            const float gw = GlyphWidth(c, 26, glyph);
            DrawGlyph(c, x - 8 - gw / 2, Y1 - 32, 26, glyph);
            x -= gw + 30;
        }
    }

    const bool settled = ++p.frames > 8;
    const bool confirm = settled && Pressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown }, false);
    const bool back = settled && (Pressed({ ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight }, false) || ImGui::IsMouseClicked(1));
    activate = activate && settled;
    if ((confirm || activate) && count)
    {
        if (p.level == 0)
        {
            p.category = categories[size_t(cursor)];
            p.level = 1, p.cursor[1] = 0, p.scroll[1] = 0;
            const auto list = PickerWeapons(p, p.category);
            for (size_t i = 0; i < list.size(); i++)
                if (now && now->weapon == list[i] && now->category == p.category)
                    p.cursor[1] = int(i);
            Play(SoundOk);
        }
        else if (p.level == 1)
        {
            const auto list = PickerVariants(p, weapons[size_t(cursor)], category);
            if (list.size() == 1)
                PickerPick(list[0]->index);
            else if (!list.empty())
            {
                // The variants, in this list.
                p.weapon = weapons[size_t(cursor)];
                p.level = 2, p.cursor[2] = 0, p.scroll[2] = 0;
                for (size_t i = 0; i < list.size(); i++)
                    if (list[i]->index == p.current || (!(now && now->weapon == p.weapon) && list[i]->variant == "Standard"))
                        p.cursor[2] = int(i);
                Play(SoundOk);
            }
        }
        else
            PickerPick(variants[size_t(cursor)]->index);
    }
    else if (back)
    {
        if (p.level > top)
            p.level--, Play(SoundCancel);
        else
            SetOpen(false, "loadout picker closed");
    }
}

void DrawMenu()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Canvas c;
    c.dl = ImGui::GetForegroundDrawList();
    c.s = std::min(vp->Size.x / 1280.0f, vp->Size.y / 720.0f);
    c.ox = vp->Pos.x + (vp->Size.x - 1280 * c.s) / 2, c.oy = vp->Pos.y + (vp->Size.y - 720 * c.s) / 2;
    const auto& f = gpu::overlay::MenuFonts();
    if (gCloseModals.exchange(false))
        gDialog = {}, bindings::CancelCapture();
    if (gPickerOnly)
    {
        // Only the loadout picker (OpenLoadoutPicker); a pick or Back from its top closes the page.
        bool padDown = false;
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k++)
            if (ImGui::IsKeyDown(ImGuiKey(k)))
                padDown = true;
        if (gOpenedWithPad.exchange(false))
            gWaitPadRelease = true;
        if (gWaitPadRelease && !padDown)
            gWaitPadRelease = false;
        if (!gWaitPadRelease)
            DrawPicker(c);
        return;
    }
    if (gNoticeOnly)
    {
        // Only the dialog (AskOverGame); answering it closes the page.
        bool padDown = false;
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k++)
            if (ImGui::IsKeyDown(ImGuiKey(k)))
                padDown = true;
        if (gOpenedWithPad.exchange(false))
            gWaitPadRelease = true;
        if (gWaitPadRelease && !padDown)
            gWaitPadRelease = false;
        if (gDialog.open)
        {
            if (gWaitPadRelease)
                DrawPanel(c, gDialog.title.c_str());
            else
                DrawDialog(c);
        }
        if (!gDialog.open)
            SetOpen(false, "notice answered");
        return;
    }
    HandleCapture();
    std::vector<Page> pages = BuildPages();
    gRow.resize(pages.size(), 0);
    gScroll.resize(pages.size(), 0);
    gPage = std::clamp(gPage, 0, int(pages.size()) - 1);
    Page& page = pages[size_t(gPage)];
    int& row = gRow[size_t(gPage)];
    row = std::clamp(row, 0, int(page.rows.size()) - 1);
    // A move to another row or tab, by any input, plays the cursor sound.
    struct MoveSound
    {
        int page, row;
        ~MoveSound()
        {
            if (gPage != page || gRow[size_t(gPage)] != row)
                Play(SoundCursor);
        }
    } moveSound{ gPage, row };
    const bool capturing = bindings::Capturing();
    const bool modal = gDialog.open || capturing;

    // (The prompts follow the device last touched: input::ActiveDevice.)
    bool padDown = false;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k++)
        if (ImGui::IsKeyDown(ImGuiKey(k)))
            padDown = true;
    if (gOpenedWithPad.exchange(false))
        gWaitPadRelease = true;
    if (gWaitPadRelease && !padDown)
        gWaitPadRelease = false;

    // Backdrop and the tab bar.
    const float left = vp->Pos.x, right = vp->Pos.x + vp->Size.x;
    const ImU32 backdrop = IM_COL32(8, 8, 8, 232);  // over the game's pause menu
    if (page.compact && page.rows[size_t(row)].preview)
    {
        // Picture settings: a window onto the game's own picture, which the game corrects as it would anything else.
        const ImVec2 p0 = c.P(82, 452), p1 = c.P(1198, 628);
        const float top = vp->Pos.y, bottom = vp->Pos.y + vp->Size.y;
        c.dl->AddRectFilled({ left, top }, { right, p0.y }, backdrop);
        c.dl->AddRectFilled({ left, p1.y }, { right, bottom }, backdrop);
        c.dl->AddRectFilled({ left, p0.y }, { p0.x, p1.y }, backdrop);
        c.dl->AddRectFilled({ p1.x, p0.y }, { right, p1.y }, backdrop);
        c.dl->AddRect(p0, p1, IM_COL32(230, 200, 110, 200), 0, 1.5f * c.s);
        c.Text(f.label, 18, 88, 430, Dim, "Preview: the game picture");
    }
    else
        c.dl->AddRectFilled({ left, vp->Pos.y }, { right, vp->Pos.y + vp->Size.y }, backdrop);
    c.dl->AddRectFilled({ left, c.P(0, 65).y }, { right, c.P(0, 165).y }, IM_COL32(0, 0, 0, 120));
    c.dl->AddLine({ left, c.P(0, 65).y }, { right, c.P(0, 65).y }, IM_COL32(255, 255, 255, 28), c.s);
    c.dl->AddLine({ left, c.P(0, 165).y }, { right, c.P(0, 165).y }, IM_COL32(255, 255, 255, 28), c.s);

    c.Text(f.value, 54, 76, 82, IM_COL32(150, 150, 150, 255), "<", false);
    c.Text(f.value, 54, 1176, 82, IM_COL32(150, 150, 150, 255), ">", false);
    DrawGlyph(c, 140, 114, 30, PromptFor(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "Q"));
    DrawGlyph(c, 1142, 114, 30, PromptFor(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "E"));
    const float tabX0 = 250, tabX1 = 1032;
    for (size_t i = 0; i < pages.size(); i++)
    {
        const float cx = tabX0 + (tabX1 - tabX0) * (float(i) + 0.5f) / float(pages.size());
        const bool sel = int(i) == gPage;
        if (sel)
            c.Glow(cx - 70, cx + 70, 90, 150, 70);
        c.TextCentered(f.tab, pages.size() > 5 ? 19.0f : 24.0f, cx, pages.size() > 5 ? 105.0f : 102.0f,
            sel ? Amber : IM_COL32(235, 235, 235, 255), pages[i].name.c_str());
        if (!modal && c.Hover(cx - 55, 70, cx + 55, 160) && ImGui::IsMouseClicked(0))
            gPage = int(i);
    }

    Row& selected = page.rows[size_t(row)];
    if (selected.kind == Row::Binding)
        gColumn = std::clamp(gColumn, 0, Columns(selected) - 1);
    if (page.compact)
        DrawCompactRows(c, page, row, gScroll[size_t(gPage)], modal);
    else
        DrawStandardRows(c, page, row, modal);

    // Footer: the selected row's description and the Default button.
    Row& r = page.rows[size_t(row)];
    const std::string help = r.kind == Row::Binding && bindings::IsReloadInteract(r.target)
                                 ? "One game button for both: it interacts when something is in reach, otherwise it reloads."
                                 : r.help;
    c.Text(f.label, 22, 82, 650, IM_COL32(240, 240, 240, 255), help.c_str());
    // Select and Back, as the device in use has them, where the description leaves room.
    if (c.Size(f.label, 22, help.c_str()).x / c.s < 600)
    {
        float x = 1030;
        for (auto [glyph, label] : { std::pair{ BackPrompt(), "Back" }, std::pair{ ConfirmPrompt(), "Select" } })
        {
            x -= c.Size(f.value, 22, label).x / c.s;
            c.Text(f.value, 22, x, 650, IM_COL32(240, 240, 240, 255), label);
            const float gw = GlyphWidth(c, 26, glyph);
            DrawGlyph(c, x - 8 - gw / 2, 662, 26, glyph);
            x -= gw + 30;
        }
    }
    if (r.kind != Row::Action && r.kind != Row::Info)
    {
        c.dl->AddRect(c.P(1045, 638), c.P(1227, 686), IM_COL32(220, 220, 220, 220), 6 * c.s, 1.5f * c.s);
        DrawGlyph(c, 1070, 662, 28, DefaultPrompt());
        c.Text(f.value, 24, 1104, 648, IM_COL32(240, 240, 240, 255), "Default");
        if (!modal && ImGui::IsMouseClicked(0) && c.Hover(1045, 638, 1227, 686))
            Reset(r);
    }

    if (capturing)
    {
        DrawCapture(c);
        return;
    }
    if (gDialog.open)
    {
        if (!gWaitPadRelease)
            DrawDialog(c);
        else
            DrawPanel(c, "Confirm");
        return;
    }
    if (gWaitPadRelease)
        return;  // the controller button just bound is still down

    // Keyboard and controller.
    const int rows = int(page.rows.size());
    if (Pressed({ ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp }))
        row = (row + rows - 1) % rows;
    if (Pressed({ ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown }))
        row = (row + 1) % rows;
    Row& cur = page.rows[size_t(row)];
    if (Pressed({ ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft }))
        Change(cur, -1);
    if (Pressed({ ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight }))
        Change(cur, +1);
    if (Pressed({ ImGuiKey_Delete, ImGuiKey_GamepadFaceUp }, false))
        Reset(cur);
    if (Pressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown }, false))
        Confirm(cur);
    const int pageCount = int(pages.size());
    if (Pressed({ ImGuiKey_Q, ImGuiKey_PageUp, ImGuiKey_GamepadL1 }, false))
        gPage = (gPage + pageCount - 1) % pageCount;
    if (Pressed({ ImGuiKey_E, ImGuiKey_PageDown, ImGuiKey_Tab, ImGuiKey_GamepadR1 }, false))
        gPage = (gPage + 1) % pageCount;
    if (Pressed({ ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight }, false))
        SetOpen(false, ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ? "Backspace" : "controller B");
}

void FrameReadout()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({ 10 * ImGui::GetStyle().FontScaleMain, 10 * ImGui::GetStyle().FontScaleMain });
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##fps", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings))
        ImGui::Text("%.0f fps  %.1f ms", io.Framerate, 1000.0f / std::max(io.Framerate, 1.0f));
    ImGui::End();
}
} // namespace

bool IsOpen() { return gOpen; }

void SetOpen(bool open, const char* why)
{
    if (gOpen.exchange(open) != open)
    {
        dtlog::Important("[menu] %s (%s)\n", open ? "opened" : "closed", why);
        Play(open ? SoundOk : SoundCancel);
        if (open)
            game_settings::RequestRefresh(), gOpenedWithPad = true;
        if (!open)
        {
            gNoticeOnly = false, gPickerOnly = false;
            game_settings::SaveIfChanged();  // as the game's Options save on leaving
            game_ui::PageClosed();
            gFromGameOptions = false;
            input::MaskHeld();
            // Opened by pausing: close the game's pause menu too, back to play (B, as on its top level).
            if (gWePaused.exchange(false) && game_ui::Current() == game_ui::State::PauseMenu)
                input::Inject(input::VpadB, 3);
        }
        gCloseModals = true;
        host::NoteMenuChanged();
    }
}

void Toggle(const char* why)
{
    if (gOpen)
    {
        SetOpen(false, why);
        return;
    }
    if (gPauseRequestUntil)
        return;  // already waiting for the pause menu
    const game_ui::State state = game_ui::Current();
    switch (state)
    {
    case game_ui::State::PauseMenu:
    case game_ui::State::FrontEnd:
        SetOpen(true, why);
        break;
    case game_ui::State::Gameplay:
        dtlog::Important("[menu] pausing the game (%s)\n", why);
        gPauseRequestWhy = why;
        gPauseRequestUntil = cafe::gx2::FrameCount() + 60;
        input::Inject(input::VpadPlus, 3);
        break;
    case game_ui::State::Busy:
        dtlog::Important("[menu] not opened: the game is busy (a movie, loading or one of its menus) (%s)\n", why);
        Play(SoundCancel);
        break;
    }
}

void OnPauseMenu(bool open)
{
    dtlog::Important("[menu] the game's pause menu %s\n", open ? "opened" : "closed");
    if (open && gPauseRequestUntil.exchange(0))
    {
        gWePaused = true;
        SetOpen(true, gPauseRequestWhy);
    }
    else if (!open)
        gWePaused = false;
}

void OpenFromGameMenu()
{
    gWePaused = false;
    SetOpen(true, "the game's menu");
}

void OpenFromGameOptions()
{
    gWePaused = false;
    gFromGameOptions = true;
    SetOpen(true, "the game's Options");
}

void PlayCursorSound() { Play(SoundCursor); }

void GameTick()
{
    const uint64_t until = gPauseRequestUntil;
    if (until && cafe::gx2::FrameCount() >= until && gPauseRequestUntil.exchange(0))
    {
        dtlog::Important("[menu] not opened: the game's pause menu did not open (%s)\n", gPauseRequestWhy);
        Play(SoundCancel);
    }
}

uint32_t TakeSounds() { return gSounds.exchange(0); }

bool WantsOverlay() { return settings::GetBool("display.show_fps", false); }

std::atomic<uint64_t> gSkipFrame{ 0 };
std::atomic<float> gSkipX{ 0 }, gSkipY{ 0 }, gSkipH{ 0 };

bool SkipGlyphReplaced()
{
    const input::Device d = input::ActiveDevice();
    return d == input::Device::Keyboard || d == input::Device::PlayStation;
}

void NoteSkipPrompt(float x, float y, float height)
{
    gSkipX = x, gSkipY = y, gSkipH = height;
    gSkipFrame = cafe::gx2::FrameCount();
}

bool SkipGlyphActive() { return gSkipFrame && cafe::gx2::FrameCount() <= gSkipFrame + 2 && SkipGlyphReplaced(); }

// The skip prompt's button: the active device's confirm (the menus' convention) where the game's Wii U A was.
void DrawSkipGlyph()
{
    if (!SkipGlyphActive())
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Canvas c;
    c.dl = ImGui::GetBackgroundDrawList();
    c.s = std::min(vp->Size.x / 1280.0f, vp->Size.y / 720.0f);
    c.ox = vp->Pos.x + (vp->Size.x - 1280 * c.s) / 2, c.oy = vp->Pos.y + (vp->Size.y - 720 * c.s) / 2;
    const Glyph g = prompts::ForGamePad(input::VpadA, input::ActiveDevice());
    const float h = gSkipH, w = GlyphWidth(c, h, g);
    DrawGlyph(c, gSkipX + h - w / 2, gSkipY + h / 2, h, g);  // right edge where the A ended, before "SKIP"
}

// ---- subtitles at the window's resolution (see menu.h) ----
struct SubtitleLine
{
    float x, y, h, w;
    uint32_t rgba;
    std::string text;
};
std::mutex gSubMutex;
std::vector<SubtitleLine> gSubBuilding, gSubReady;  // the frame being drawn, the last complete one
uint64_t gSubBuildFrame = 0, gSubReadyFrame = 0;
// When lines were last drawn by the game: they stay up for 100 ms after (by time, not frames: uncapped, the game ran at
// hundreds of frames a second and a line missing from a frame or two blinked).
std::chrono::steady_clock::time_point gSubSeen;
// (250 ms: in Bink movies the game draws the lines only on some frames, gaps of up to about 110 ms at movie pacing; a
// 100 ms hold ran out between them and the line blinked)
constexpr auto SubtitleHold = std::chrono::milliseconds(250);

bool SharpSubtitles()
{
    return settings::Get("gameplay.subtitles", "sharp") == "sharp" && host::Window() != nullptr;
}

void NoteSubtitle(uint64_t tick, float x, float y, float height, float gameWidth, uint32_t rgba, std::string text)
{
    // (frames by the Game Thread's tick, where the HUD draws: GX2's swap count, which the rendering thread advances,
    // changed in the middle of a frame's lines, and the half collected sets shown (1, 2 or 5 of 10 lines) made the text
    // blink for a frame at a time, which read as the picture behind it flickering)
    const uint64_t frame = tick;
    std::lock_guard lock(gSubMutex);
    // A new frame, or a new pass over the same frame (the game can draw twice between presents: during movies), which
    // starts again with a line already drawn at that place in that colour.
    const bool again = std::any_of(gSubBuilding.begin(), gSubBuilding.end(), [&](const SubtitleLine& l) {
        return l.x == x && l.y == y && (l.rgba >> 8) == (rgba >> 8);
    });
    if (frame != gSubBuildFrame || again)
    {
        if (!gSubBuilding.empty())
            gSubReady = std::move(gSubBuilding), gSubReadyFrame = gSubBuildFrame;
        gSubBuilding.clear(), gSubBuildFrame = frame;
    }
    static const bool logSubs = getenv("DT_LOG_SUBS") != nullptr;  // (testing)
    if (logSubs)
        dtlog::Important("[test] note frame %llu at %.0f,%.0f rgba %08X%s (building %zu, ready %zu)\n", (unsigned long long)frame, x, y,
            rgba, again ? " again" : "", gSubBuilding.size(), gSubReady.size());
    gSubBuilding.push_back({ x, y, height, gameWidth, rgba, std::move(text) });
    gSubSeen = std::chrono::steady_clock::now();
}

// Lines of the last frame the game finished (or of the one before, for a frame or two: the overlay may run ahead).
std::vector<SubtitleLine> CurrentSubtitles()
{
    // (only complete frames of lines: the game draws them on its rendering thread while this runs on the presenting
    // thread, and a frame taken half collected (outline copies without the text) blinked. The lines being collected
    // become current when the next frame's first line arrives, in NoteSubtitle.)
    std::lock_guard lock(gSubMutex);
    // (lines nothing has followed for 20 ms are complete: a line drawn only briefly had never shown)
    if (!gSubBuilding.empty() && std::chrono::steady_clock::now() - gSubSeen > std::chrono::milliseconds(20))
        gSubReady = std::move(gSubBuilding), gSubReadyFrame = gSubBuildFrame, gSubBuilding.clear();
    static const bool logSubs = getenv("DT_LOG_SUBS") != nullptr;  // (testing)
    if (logSubs && (gSubReady.empty() || std::chrono::steady_clock::now() - gSubSeen > SubtitleHold))
        dtlog::Important("[test] no lines: ready %zu, building %zu, %.1f ms since the last line\n", gSubReady.size(), gSubBuilding.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gSubSeen).count());
    if (gSubReady.empty() || std::chrono::steady_clock::now() - gSubSeen > SubtitleHold)
        return {};
    (void)gSubReadyFrame;
    return gSubReady;
}

bool SubtitlesActive()
{
    const uint64_t now = cafe::gx2::FrameCount();
    std::lock_guard lock(gSubMutex);
    (void)now;
    return (!gSubReady.empty() || !gSubBuilding.empty()) && std::chrono::steady_clock::now() - gSubSeen <= SubtitleHold;
}

void DrawSubtitles()
{
    const std::vector<SubtitleLine> lines = CurrentSubtitles();
    static const bool logSubs = getenv("DT_LOG_SUBS") != nullptr;  // (testing)
    if (logSubs)
        dtlog::Important("[test] draw at frame %llu: %zu lines\n", (unsigned long long)cafe::gx2::FrameCount(), lines.size());
    if (lines.empty())
        return;
    const auto& f = gpu::overlay::MenuFonts();
    if (!f.label)
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Canvas c;
    c.dl = ImGui::GetBackgroundDrawList();
    c.s = std::min(vp->Size.x / 1280.0f, vp->Size.y / 720.0f);
    c.ox = vp->Pos.x + (vp->Size.x - 1280 * c.s) / 2, c.oy = vp->Pos.y + (vp->Size.y - 720 * c.s) / 2;
    static const float fontScale = [] {
        const char* e = getenv("DT_SUBTITLE_FONT");  // testing: the font size per cell height
        return e ? float(atof(e)) : 0.80f;
    }();
    const float size = std::clamp(float(settings::GetInt("gameplay.subtitle_size", 100)), 50.0f, 250.0f) / 100.0f;
    static const float lower = [] {
        const char* e = getenv("DT_SUBTITLE_LOWER");  // testing: how far below the cell's centre, per cell height
        return e ? float(atof(e)) : 0.07f;
    }();
    for (const SubtitleLine& l : lines)
    {
        const float px = l.h * fontScale * size, base = l.h * fontScale;
        // The game's lettering is spaced wider than the typeface's own: letters spread to the width the game measured
        // (at 100%; scaled with the size), its line centred where the game centred it.
        std::vector<std::string> chars;
        for (size_t k = 0; k < l.text.size();)
        {
            size_t n = 1;
            const unsigned char b = static_cast<unsigned char>(l.text[k]);
            if (b >= 0xF0)
                n = 4;
            else if (b >= 0xE0)
                n = 3;
            else if (b >= 0xC0)
                n = 2;
            chars.push_back(l.text.substr(k, n));
            k += n;
        }
        const float natural = c.Size(f.label, px, l.text.c_str()).x / c.s;
        const float target = l.w > 0 ? l.w * size : natural;
        const float spacing = chars.size() > 1 ? std::clamp((target - natural) / float(chars.size() - 1), 0.0f, px * 0.15f) : 0.0f;
        const float width = natural + spacing * float(chars.size() > 1 ? chars.size() - 1 : 0);
        const float cx = l.x + l.w / 2, bottom = l.y + l.h;
        float x = cx - width / 2;
        const float y0 = bottom - (l.h - base) / 2 - px + l.h * lower;
        const ImU32 col = IM_COL32(l.rgba >> 24, (l.rgba >> 16) & 255, (l.rgba >> 8) & 255, l.rgba & 255);
        static const bool logSubs = getenv("DT_LOG_SUBS") != nullptr;  // (testing)
        if (logSubs && (l.rgba >> 8) == 0xFFFFFF)
            dtlog::Important("[test]   white line x %.1f y0 %.1f px %.1f w %.1f h %.1f width %.1f s %.3f alpha %u text %.12s\n", x, y0, px,
                l.w, l.h, width, c.s, l.rgba & 255, l.text.c_str());
        // (The game's outline comes as lines of its own: four black copies a pixel off each way.)
        for (const std::string& ch : chars)
        {
            c.dl->AddText(c.F(f.label), px * c.s, c.P(x, y0), col, ch.c_str());
            x += c.Size(f.label, px, ch.c_str()).x / c.s + spacing;
        }
    }
}

// The game's mouse cursor (UI_Common.MouseCursor: a dart, its left half dark grey and its right half white, with a dark
// edge) drawn over the PC page at the pointer, in the page's 1280x720 scale: the game's own cursor is part of the game
// picture, under the page.
std::atomic<bool> gMenuPointer{ false };
void SetMenuPointer(bool active) { gMenuPointer = active; }
bool MenuPointerActive() { return gMenuPointer.load(); }

void DrawGameCursor()
{
    if ((!gOpen && !gMenuPointer) || input::ActiveDevice() != input::Device::Keyboard)
        return;
    // (the pointer as the menus see it: it follows the mouse under the page too, in the 1280x720 layout)
    const input::Pointer p = input::GetPointer();
    if (!p.valid)
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = std::min(vp->Size.x / 1280.0f, vp->Size.y / 720.0f);
    const ImVec2 m(vp->Pos.x + (vp->Size.x - 1280 * s) / 2 + p.x * s, vp->Pos.y + (vp->Size.y - 720 * s) / 2 + p.y * s);
    auto P = [&](float x, float y) { return ImVec2(m.x + x * s, m.y + y * s); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 tip = P(0, 0), wing = P(21, 15), notch = P(8, 15.5f), tail = P(5, 23);
    const ImVec2 outline[] = { tip, wing, notch, tail };
    dl->AddTriangleFilled(tip, notch, tail, IM_COL32(112, 112, 118, 255));
    dl->AddTriangleFilled(tip, wing, notch, IM_COL32(238, 238, 240, 255));
    dl->AddPolyline(outline, 4, IM_COL32(20, 20, 24, 230), std::max(1.0f, 1.2f * s), ImDrawFlags_Closed);
}

void OpenLoadoutPicker(int slot, std::vector<LoadoutEntry> entries, int current)
{
    Picker p;
    p.slot = slot, p.entries = std::move(entries), p.current = current;
    p.level = slot == 2 ? 1 : 0;
    // Start on the slot's weapon: its type, and the weapon in the list.
    for (const LoadoutEntry& e : p.entries)
        if (e.index == current)
        {
            const auto cats = PickerCategories(p);
            for (size_t i = 0; i < cats.size(); i++)
                if (cats[i] == e.category)
                    p.cursor[0] = int(i);
            if (slot == 2)
            {
                const auto list = PickerWeapons(p, -1);
                for (size_t i = 0; i < list.size(); i++)
                    if (list[i] == e.weapon)
                        p.cursor[1] = int(i);
            }
        }
    std::lock_guard lock(gNoticeMutex);
    gPendingPicker = std::move(p);
}

bool TakeLoadoutPick(int& slot, int& index)
{
    std::lock_guard lock(gPickMutex);
    if (!gPick)
        return false;
    slot = gPick->first, index = gPick->second;
    gPick.reset();
    return true;
}

bool LoadoutPickerOpen() { return gPickerOnly || gPendingPicker; }

void CloseLoadoutPicker(const char* why)
{
    {
        std::lock_guard lock(gNoticeMutex);
        gPendingPicker.reset();
    }
    if (gPickerOnly)
        SetOpen(false, why);
}

void AskOverGame(std::string title, std::vector<std::string> lines, std::function<void()> yes)
{
    Dialog d;
    d.open = true, d.title = std::move(title), d.yes = std::move(yes);
    if (!lines.empty())
        d.line1 = lines[0];
    if (lines.size() > 1)
        d.line2 = lines[1];
    for (size_t i = 2; i < lines.size(); i++)
        d.more.push_back(lines[i]);
    std::lock_guard lock(gNoticeMutex);
    gPendingNotice = std::move(d);
}

void Draw()
{
    // Keys released while the page was closed went to the game, not to ImGui, which still counts them as held: a
    // Backspace that closed the loadout picker made the next picker's Backspace no new press. Forgotten as it opens.
    struct ClearOnOpen
    {
        ~ClearOnOpen()
        {
            static bool wasOpen = false;
            if (gOpen && !wasOpen)
                ImGui::GetIO().ClearInputKeys();
            wasOpen = gOpen;
        }
    } clearOnOpen;
    {
        std::lock_guard lock(gNoticeMutex);
        if (gPendingPicker && !gOpen)
        {
            gPickerOnly = true;
            SetOpen(true, "loadout picker");
            gCloseModals = false;
            gPicker = std::move(*gPendingPicker);
            gPendingPicker.reset();
        }
        if (gPendingNotice && !gOpen)
        {
            gNoticeOnly = true;
            SetOpen(true, "notice");
            gCloseModals = false;  // (opening asks to drop any old dialog: this one is new)
            gDialog = std::move(*gPendingNotice);
            gPendingNotice.reset();
        }
    }
    struct CursorLast  // (drawn after everything else in the frame: the page and the skip glyph lie under it)
    {
        ~CursorLast() { DrawGameCursor(); }
    } cursorLast;
    DrawSubtitles();
    DrawSkipGlyph();
    if (WantsOverlay())
        FrameReadout();
    if (gOpen)
    {
        // The game's settings can change under the page (its own menu, a mission starting): read them twice a second.
        static double nextRefresh = 0;
        if (ImGui::GetTime() > nextRefresh)
            game_settings::RequestRefresh(), nextRefresh = ImGui::GetTime() + 0.5;
        DrawMenu();
    }
}
} // namespace host::menu
