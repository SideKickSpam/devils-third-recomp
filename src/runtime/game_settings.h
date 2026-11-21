// The game's own options (its Options menu: D3SystemSettings), read and changed for the PC page. Values are read with
// the game's getters and changed the way its option pages change them (D3UIWidget_OptionMenu*: the same setters,
// apply functions and side effects), on the Game Thread; the page reads a snapshot and queues changes from any thread.
#pragma once

#include <cstdint>

namespace game_settings
{
// Where the player is, which decides what the game's Options offer (as D3UIWidget_OptionMenuSolo / Subtitle do).
enum class Mode
{
    Unknown,      // not read yet
    FrontEnd,     // title and front-end menus: difficulty and score display
    Story,        // a story mission: difficulty
    ScoreAttack,  // mission mode: score display
    Multiplayer,  // no single-player page; only the keyboard map on the language page
};

struct Values
{
    bool valid = false;
    Mode mode = Mode::Unknown;
    bool canChangeLanguage = false;  // D3Engine.GetGameRegion() != 0, and not in multiplayer
    int region = 0;                  // D3Engine.GetGameRegion(): 0 is the Japanese version
    // Gore: D3GRI.ShouldShowGore shows it always outside region 0; in region 0 only once bCanUnlockGore is set, which
    // the online multiplayer menu does when the game's server sends can_unlock_gore = 1 (D3FrontEnd_DsrMultiplayMenu).
    bool canUnlockGore = false;
    bool aimAssistAllowed = false;   // the game's Controls page shows Aim Assist only if D3GRI.bAllowAimAssist
    // Controls page
    int controllerLayout = 0;  // 0 Default, 1 Shooter, 2 Acrobat, 3 Ninja
    bool vibration = true, coverAssist = true, aimAssist = true, gore = false;
    // Camera page: sensitivity in the menu's steps, 1-20 (10 = 1.0)
    int sensitivityX = 10, sensitivityY = 10;
    bool invertX = false, invertY = false;
    // Video page: 0-100
    int brightness = 50, contrast = 50, gamma = 50, horizontalArea = 0, verticalArea = 0;
    // Audio page: 0-100
    int voiceVolume = 100, sfxVolume = 100, musicVolume = 100;
    // Language page
    bool subtitles = true;
    int textLanguage = 0, voiceLanguage = 0;  // text: English, French, Japanese; voice: English, Japanese
    int keyboardMap = 0;                      // index into the USB keyboard maps
    // Single-player page
    int difficulty = 1;  // Casual, Standard, Hardcore
    bool scoreDisplay = true;
};

enum class Field
{
    ControllerLayout, Vibration, CoverAssist, AimAssist, Gore,
    SensitivityX, SensitivityY, InvertX, InvertY,
    Brightness, Contrast, Gamma, HorizontalArea, VerticalArea,
    VoiceVolume, SfxVolume, MusicVolume,
    Subtitles, KeyboardMap,
    Difficulty, ScoreDisplay,
};

// Any thread.
Values Current();
Values Defaults();  // the game's initial values (D3SystemSettings.GetOption*InitialValue*)
void Set(Field field, int value);
// Text and voice language: applied as the game's Options do it, which saves and reloads to the title screen.
void ChangeLanguage(int textLanguage, int voiceLanguage);
// Save to the system save if anything changed since the last save (the page calls it when it closes).
void SaveIfChanged();
// Keep the snapshot fresh (the page calls it while open).
void RequestRefresh();
// Game Thread, once per frame (gfx_hooks.cpp, from UGameEngine::Tick).
void Tick(uint32_t engine);
} // namespace game_settings
