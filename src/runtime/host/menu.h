// The in-game settings menu (Dear ImGui, drawn by gpu/overlay.*). F1, Escape (to close), the controller's Guide button,
// or Back + right-stick click open and close it. While it is open the game sees an idle GamePad and keyboard and mouse
// go to the menu. Every change is applied at once where possible and saved to settings.ini.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace host::menu
{
// Any thread.
bool IsOpen();
// F1, Escape, the Guide button: close the page, or open it the way the game expects (`why` names the input, for the log).
// In a level the game's own pause is triggered first (a Plus press) and the page opens on top of its pause menu, which
// pauses single-player and not multiplayer, as the game decides; closing the page then closes that pause menu too.
// Over the game's pause menu or in the front end it opens at once; during a movie, loading or another of the game's
// menus it does not open.
void Toggle(const char* why);
void SetOpen(bool open, const char* why);
// Any thread: the game's Yes / No dialog over the game (the page opens showing only it, and closes when it is answered;
// No is the default). `yes` runs on the presenting thread.
void AskOverGame(std::string title, std::vector<std::string> lines, std::function<void()> yes);
// The Score Attack loadout picker (gfx_hooks.cpp opens it from a focused slot): one entry per selectable weapon, by its
// index in the game's list. Primary and Secondary (slot 0, 1) list weapon types (category 0-7: pistols, assault rifles,
// SMGs, shotguns, machine guns, sniper rifles, launchers, X-gear), then weapons, then a weapon's variants; Melee (2)
// lists the weapons. Any thread; the pick is taken with TakeLoadoutPick.
struct LoadoutEntry
{
    int index = 0;
    std::string weapon, variant;
    int category = 0;
    bool xgear = false;
    std::string gfx, part;  // its picture: the weapon library's sprite, and the underbarrel part shown with it
};
void OpenLoadoutPicker(int slot, std::vector<LoadoutEntry> entries, int current);
bool TakeLoadoutPick(int& slot, int& index);
bool LoadoutPickerOpen();
// Closes the picker if it is up (or about to be), without a pick. Any thread.
void CloseLoadoutPicker(const char* why);
// Game Thread (gfx_hooks.cpp): "PC Settings" chosen in the game's own menu (the page opens over it and closing it
// returns there); our entry focused there (the game's cursor sound).
void OpenFromGameMenu();
// The game's own Options entry was chosen (gfx_hooks.cpp): the page opens instead, with a row to its original menu.
void OpenFromGameOptions();
void PlayCursorSound();
// Game Thread (gfx_hooks.cpp): the game's pause menu opened or closed; once per frame.
void OnPauseMenu(bool open);
void GameTick();
// The overlay has something to show with the menu closed (the frame-time readout).
bool WantsOverlay();

// The overlay's thread, inside an ImGui frame.
void Draw();
// The cutscenes' "SKIP" prompt drawn this frame (gfx_hooks.cpp, from the game's canvas text): where its button glyph sits
// (1280x720 layout units) and its height. On PlayStation and the keyboard the game's Wii U A is blanked and the overlay
// draws the active device's confirm button there.
void NoteSkipPrompt(float x, float y, float height);
bool SkipGlyphReplaced();
// The overlay has the skip prompt's button to draw this frame.
bool SkipGlyphActive();
// Subtitles (the game's SubtitleFont, a 720p bitmap font that blurs or blocks up when scaled): drawn by the overlay at the
// window's resolution in the game's NewCezanne instead (gameplay.subtitles "sharp", the default). gfx_hooks.cpp passes
// each line the game draws: its position, cell height and the width the game measured (1280x720 units), its colour
// (0xRRGGBBAA, sRGB) and text (UTF-8); the game's own draw is made invisible. `tick` is the Game Thread's engine tick the
// line was drawn in (the HUD draws on the Game Thread: one tick's lines are one frame of subtitles).
bool SharpSubtitles();
void NoteSubtitle(uint64_t tick, float x, float y, float height, float gameWidth, uint32_t rgba, std::string text);
// The overlay has subtitle lines to draw.
bool SubtitlesActive();
// Game Thread: the menus' mouse pointer is in use (keyboard and mouse, outside gameplay): the overlay draws the cursor
// (DrawGameCursor), always above the game's menus and without the game picture's latency.
void SetMenuPointer(bool active);
bool MenuPointerActive();

// The game's own menu sounds, which the page asks for and the Game Thread plays (gfx_hooks.cpp).
enum Sound : uint32_t
{
    SoundCursor = 1,  // PLAY_UI_CURSOR: a move between rows, tabs or values
    SoundOk = 2,      // PLAY_UI_OK: opening the page, an action, a confirmation
    SoundCancel = 4,  // PLAY_UI_CANCEL: closing the page or a dialog
};
// Game Thread: the sounds asked for since the last call (a mask of Sound).
uint32_t TakeSounds();
} // namespace host::menu
