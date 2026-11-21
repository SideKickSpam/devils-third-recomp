#include "game_settings.h"

#include "log.h"
#include "ue3.h"

#include <cmath>
#include <deque>
#include <functional>
#include <mutex>

namespace game_settings
{
namespace
{
std::mutex gMutex;
Values gValues, gDefaults;
std::deque<std::function<void(uint32_t)>> gQueue;  // run on the Game Thread with the engine object
bool gRefresh = true, gChanged = false;

// D3UIWidget_OptionMenuCamera: SensitiveScale 3, SensitiveNotch 10 (WiiU-DevilsThirdUI.ini); steps 1-20.
constexpr float SensitiveScale = 3.0f;
constexpr int SensitiveNotch = 10;
float SensitivityToFloat(int step)
{
    if (step <= SensitiveNotch)
        return 1.0f / SensitiveScale + float(step) * (1.0f - 1.0f / SensitiveScale) / float(SensitiveNotch);
    return 1.0f + float(step - SensitiveNotch) * (SensitiveScale - 1.0f) / float(SensitiveNotch);
}
int SensitivityToStep(float value)
{
    int best = SensitiveNotch;
    for (int s = 1; s <= 2 * SensitiveNotch; s++)
        if (std::fabs(SensitivityToFloat(s) - value) < std::fabs(SensitivityToFloat(best) - value))
            best = s;
    return best;
}

void Queue(std::function<void(uint32_t)> f)
{
    std::lock_guard lock(gMutex);
    gQueue.push_back(std::move(f));
    gRefresh = true;
}

// The objects the game's option pages work with.
struct Context
{
    uint32_t engine = 0, settings = 0, world = 0, pc = 0, input = 0, hud = 0, game = 0;
};

Context Find(uint32_t engine)
{
    Context c;
    c.engine = engine;
    c.settings = ue3::Call(engine, "GetSystemSettings").value_or(0);
    c.world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    c.pc = ue3::Call(c.world, "GetALocalPlayerController").value_or(0);
    c.input = ue3::GetObject(c.pc, "PlayerInput").value_or(0);
    c.hud = ue3::GetObject(c.pc, "myHUD").value_or(0);
    c.game = ue3::GetObject(c.world, "Game").value_or(0);
    return c;
}

int Int(uint32_t obj, const char* fn) { return int(ue3::Call(obj, fn).value_or(0)); }
float Float(uint32_t obj, const char* fn) { return ue3::CallFloat(obj, fn).value_or(0); }

Values Read(const Context& c)
{
    Values v;
    if (!c.settings)
        return v;
    const uint32_t s = c.settings;
    v.valid = true;
    if (ue3::IsA(c.game, "D3GameInfoSingleScoreAttack"))
        v.mode = Mode::ScoreAttack;
    else if (ue3::IsA(c.game, "D3GameInfoSingle"))
        v.mode = Mode::Story;
    else if (Int(c.engine, "GetGameMode") == 2)
        v.mode = Mode::Multiplayer;
    else
        v.mode = Mode::FrontEnd;
    v.region = Int(c.engine, "GetGameRegion");
    v.canChangeLanguage = v.region != 0 && v.mode != Mode::Multiplayer;
    v.aimAssistAllowed = ue3::GetBool(ue3::GetObject(c.game, "GameReplicationInfo").value_or(0), "bAllowAimAssist").value_or(false);
    v.canUnlockGore = ue3::GetBool(s, "bCanUnlockGore").value_or(false);
    v.controllerLayout = Int(s, "GetControllerLayout");
    v.vibration = Int(s, "GetVibration"), v.coverAssist = Int(s, "GetCoverAssist"), v.aimAssist = Int(s, "GetAimAssist");
    v.gore = Int(s, "GetEnableGore");
    v.sensitivityX = SensitivityToStep(Float(s, "GetCameraSensitivityX"));
    v.sensitivityY = SensitivityToStep(Float(s, "GetCameraSensitivityY"));
    v.invertX = Int(s, "GetCameraInvertX"), v.invertY = Int(s, "GetCameraInvertY");
    v.brightness = int(std::lround(Float(s, "GetVideoBrightness")));
    v.contrast = int(std::lround(Float(s, "GetVideoContrast")));
    v.gamma = int(std::lround(Float(s, "GetVideoGamma")));
    v.horizontalArea = Int(s, "GetVideoHorizontalArea"), v.verticalArea = Int(s, "GetVideoVerticalArea");
    v.voiceVolume = Int(s, "GetAudioVoiceVolume"), v.sfxVolume = Int(s, "GetAudioSEVolume");
    v.musicVolume = Int(s, "GetAudioBGMVolume");
    v.subtitles = Int(s, "GetSubtitleEnabled");
    v.textLanguage = Int(s, "GetSubtitleLanguage"), v.voiceLanguage = Int(s, "GetVoiceLanguage");
    v.keyboardMap = Int(s, "GetKeyboardCC");
    v.difficulty = Int(s, "GetDifficulty"), v.scoreDisplay = Int(s, "GetScoreDisp");
    return v;
}

Values ReadDefaults(const Context& c)
{
    Values v;
    const uint32_t s = c.settings;
    if (!s)
        return v;
    v.valid = true;
    v.controllerLayout = Int(s, "GetOptionGameInitialValueControllerLayout");
    v.vibration = Int(s, "GetOptionGameInitialValueVibration");
    v.aimAssist = Int(s, "GetOptionGameInitialValueAimAssist"), v.coverAssist = Int(s, "GetOptionGameInitialValueCoverAssist");
    v.scoreDisplay = Int(s, "GetOptionGameInitialValueScoreDisp"), v.difficulty = Int(s, "GetOptionGameInitialValueDifficulty");
    v.sensitivityX = SensitivityToStep(Float(s, "GetOptionCameraInitialValueSensitivityX"));
    v.sensitivityY = SensitivityToStep(Float(s, "GetOptionCameraInitialValueSensitivityY"));
    v.invertX = Int(s, "GetOptionCameraInitialValueInvertX"), v.invertY = Int(s, "GetOptionCameraInitialValueInvertY");
    v.horizontalArea = Int(s, "GetOptionVideoInitialValueHorizontalArea");
    v.verticalArea = Int(s, "GetOptionVideoInitialValueVerticalArea");
    v.brightness = int(std::lround(Float(s, "GetOptionVideoInitialValueBrightness")));
    v.contrast = int(std::lround(Float(s, "GetOptionVideoInitialValueContrast")));
    v.gamma = int(std::lround(Float(s, "GetOptionVideoInitialValueGamma")));
    v.voiceVolume = Int(s, "GetOptionAudioInitialValueVoiceVolume"), v.sfxVolume = Int(s, "GetOptionAudioInitialValueSEVolume");
    v.musicVolume = Int(s, "GetOptionAudioInitialValueBGMVolume");
    v.subtitles = Int(s, "GetOptionLanguageInitialValueSubtitleEnabled");
    v.textLanguage = Int(s, "GetOptionLanguageInitialValueSubtitle"), v.voiceLanguage = Int(s, "GetOptionLanguageInitialValueVoice");
    v.keyboardMap = Int(s, "GetOptionLanguageInitialValueKeyboardCC");
    return v;
}

// The side effects of the game's option pages after their setters.
void RefreshPlayer(const Context& c, bool bindings)
{
    if (bindings)
        ue3::Call(c.input, "RefreshBindings");  // D3UIWidget_OptionMenuGame / Solo.SetGameSettings
    ue3::Call(c.hud, "ApplySubHudVisibility");
}

void Apply(const Context& c, Field field, int value)
{
    const uint32_t s = c.settings;
    if (!s)
        return;
    const Values v = Read(c);
    switch (field)
    {
    // D3UIWidget_OptionMenuGame.SetGameSettings
    case Field::ControllerLayout: ue3::Call(s, "SetControllerLayout", { value }); RefreshPlayer(c, true); break;
    case Field::Vibration:
        ue3::SetBool(ue3::GetObject(c.pc, "ForceFeedbackManager").value_or(0), "bAllowsForceFeedback", value != 0);
        ue3::Call(s, "SetVibration", { value != 0 });
        RefreshPlayer(c, true);
        break;
    case Field::CoverAssist: ue3::Call(s, "SetCoverAssist", { value != 0 }); RefreshPlayer(c, true); break;
    case Field::AimAssist: ue3::Call(s, "SetAimAssist", { value != 0 }); RefreshPlayer(c, true); break;
    case Field::Gore: ue3::Call(s, "SetEnableGore", { value != 0 }); RefreshPlayer(c, true); break;
    // D3UIWidget_OptionMenuCamera.SetCameraSettings
    case Field::InvertX:
        ue3::SetBool(c.input, "bInvertTurn", value != 0);
        ue3::Call(s, "SetCameraInvertX", { value != 0 });
        RefreshPlayer(c, false);
        break;
    case Field::InvertY:
        ue3::SetBool(c.input, "bInvertMouse", value != 0);
        ue3::Call(s, "SetCameraInvertY", { value != 0 });
        RefreshPlayer(c, false);
        break;
    case Field::SensitivityX: ue3::Call(s, "SetCameraSensitivityX", { SensitivityToFloat(value) }); RefreshPlayer(c, false); break;
    case Field::SensitivityY: ue3::Call(s, "SetCameraSensitivityY", { SensitivityToFloat(value) }); RefreshPlayer(c, false); break;
    // D3UIWidget_OptionMenuVideo.SetVideoSettings (live, as its sliders move)
    case Field::Brightness: case Field::Contrast: case Field::Gamma: case Field::HorizontalArea: case Field::VerticalArea:
    {
        Values n = v;
        (field == Field::Brightness ? n.brightness : field == Field::Contrast ? n.contrast : field == Field::Gamma ? n.gamma
            : field == Field::HorizontalArea ? n.horizontalArea : n.verticalArea) = value;
        ue3::Call(s, "SetVideoParam", { float(n.brightness), float(n.contrast), float(n.gamma) });
        ue3::Call(s, "SetVideoArea", { n.horizontalArea, n.verticalArea });
        ue3::Call(s, "SetVideoBrightness", { float(n.brightness) });
        ue3::Call(s, "SetVideoContrast", { float(n.contrast) });
        ue3::Call(s, "SetVideoGamma", { float(n.gamma) });
        ue3::Call(s, "SetVideoHorizontalArea", { n.horizontalArea });
        ue3::Call(s, "SetVideoVerticalArea", { n.verticalArea });
        break;
    }
    // D3UIWidget_OptionMenuAudio.SetAudioSettings (live)
    case Field::VoiceVolume: case Field::SfxVolume: case Field::MusicVolume:
    {
        Values n = v;
        (field == Field::VoiceVolume ? n.voiceVolume : field == Field::SfxVolume ? n.sfxVolume : n.musicVolume) = value;
        ue3::Call(s, "SetAudioParam", { n.voiceVolume, n.musicVolume, n.sfxVolume });
        ue3::Call(s, "SetAudioVoiceVolume", { n.voiceVolume });
        ue3::Call(s, "SetAudioBGMVolume", { n.musicVolume });
        ue3::Call(s, "SetAudioSEVolume", { n.sfxVolume });
        break;
    }
    // D3UIWidget_OptionMenuSubtitle.SetSubtitleSettings
    case Field::Subtitles: ue3::Call(s, "SetSubtitleEnabled", { value != 0 }); ue3::Call(s, "ChangeSubtitleDisplay"); break;
    case Field::KeyboardMap: ue3::Call(s, "SetKeyboardCC", { value }); ue3::Call(s, "ChangeKeyboardCountryCode"); break;
    // D3UIWidget_OptionMenuSolo.SetGameSettings
    case Field::Difficulty:
        ue3::Call(c.game, "SetDifficulty", { value });  // the running mission's GameInfo, when it has one
        ue3::Call(s, "SetDifficulty", { value });
        RefreshPlayer(c, true);
        break;
    case Field::ScoreDisplay: ue3::Call(s, "SetScoreDisp", { value != 0 }); RefreshPlayer(c, true); break;
    }
}
} // namespace

Values Current()
{
    std::lock_guard lock(gMutex);
    return gValues;
}

Values Defaults()
{
    std::lock_guard lock(gMutex);
    return gDefaults;
}

void Set(Field field, int value)
{
    Queue([field, value](uint32_t engine) {
        Apply(Find(engine), field, value);
        gChanged = true;
    });
    // Show the new value at once; the next refresh reads back what the game stored.
    std::lock_guard lock(gMutex);
    Values& v = gValues;
    switch (field)
    {
    case Field::ControllerLayout: v.controllerLayout = value; break;
    case Field::Vibration: v.vibration = value; break;
    case Field::CoverAssist: v.coverAssist = value; break;
    case Field::AimAssist: v.aimAssist = value; break;
    case Field::Gore: v.gore = value; break;
    case Field::SensitivityX: v.sensitivityX = value; break;
    case Field::SensitivityY: v.sensitivityY = value; break;
    case Field::InvertX: v.invertX = value; break;
    case Field::InvertY: v.invertY = value; break;
    case Field::Brightness: v.brightness = value; break;
    case Field::Contrast: v.contrast = value; break;
    case Field::Gamma: v.gamma = value; break;
    case Field::HorizontalArea: v.horizontalArea = value; break;
    case Field::VerticalArea: v.verticalArea = value; break;
    case Field::VoiceVolume: v.voiceVolume = value; break;
    case Field::SfxVolume: v.sfxVolume = value; break;
    case Field::MusicVolume: v.musicVolume = value; break;
    case Field::Subtitles: v.subtitles = value; break;
    case Field::KeyboardMap: v.keyboardMap = value; break;
    case Field::Difficulty: v.difficulty = value; break;
    case Field::ScoreDisplay: v.scoreDisplay = value; break;
    }
}

void ChangeLanguage(int textLanguage, int voiceLanguage)
{
    // D3UIWidget_OptionMenuSubtitle.AcceptChangedLanguage, then D3UIWidget_OptionMenu.OnTryChangedLanguage: save, and
    // travel back to the front end behind the loading movie, which reloads everything in the new language.
    Queue([textLanguage, voiceLanguage](uint32_t engine) {
        const Context c = Find(engine);
        dtlog::Important("[options] language: text %d, voice %d; reloading to the title\n", textLanguage, voiceLanguage);
        ue3::Call(c.settings, "SetSubtitleLanguage", { textLanguage });
        ue3::Call(c.settings, "SetVoiceLanguage", { voiceLanguage });
        ue3::Call(c.settings, "ChangeGameLanguage");
        ue3::Call(engine, "SaveSystemSettings", { true });
        gChanged = false;
        ue3::Call(engine, "EnabledAssignedLoadingMovie", { true });
        ue3::Call(engine, "SetAssignedLoadingMovieName", { ue3::Arg::MakeName("Single_ivn_A") });
        ue3::Call(engine, "SetTipsReDetermination", { true });
        ue3::Call(engine, "ClearBattleFinalBoss");
        ue3::Call(ue3::UIManager(), "SetNextFrontEndMenu", { 2 });  // the title menu
        ue3::Call(c.pc, "ClientTravel", { ue3::Arg::MakeString("?Closed"), 0 });
    });
}

void SaveIfChanged()
{
    Queue([](uint32_t engine) {
        if (!gChanged)
            return;
        gChanged = false;
        dtlog::Important("[options] saving the game's settings\n");
        ue3::Call(engine, "SaveSystemSettings", { false });  // as D3UIWidget_OptionMenu does after its pages' changes
    });
}

void RequestRefresh()
{
    std::lock_guard lock(gMutex);
    gRefresh = true;
}

void Tick(uint32_t engine)
{
    std::deque<std::function<void(uint32_t)>> queue;
    bool refresh;
    {
        std::lock_guard lock(gMutex);
        queue.swap(gQueue);
        refresh = gRefresh;
        gRefresh = false;
    }
    for (auto& f : queue)
        f(engine);
    // Also every few seconds with the page closed: the game's own Options can change them (bindings show the layout).
    static int ticks = 0;
    if (++ticks >= 300)
        refresh = true;
    if (!refresh)
        return;
    ticks = 0;
    const Context c = Find(engine);
    Values v = Read(c);
    Values d = gDefaults.valid ? gDefaults : ReadDefaults(c);
    std::lock_guard lock(gMutex);
    if (gQueue.empty())  // (a change queued meanwhile keeps its value until the next refresh)
        gValues = v;
    gDefaults = d;
}
} // namespace game_settings
