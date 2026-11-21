// What the game's own UI is doing, as the GFx and Bink hooks see it (gfx_hooks.cpp). The settings page uses it to open
// the way the game expects: on top of the game's pause menu, never during a movie or loading.
#pragma once

namespace game_ui
{
enum class State
{
    Gameplay,   // in a level with only the HUD's movies running: Plus opens the pause menu
    PauseMenu,  // the game's pause menu is open (it pauses the game itself in single-player, not in multiplayer)
    FrontEnd,   // title and front-end menus: nothing to pause
    Busy,       // a Bink movie, an in-engine cutscene (cinematic mode), loading, or another of the game's menus or prompts
};
// Any thread.
State Current();
// A menu wants the mouse pointer: the pause menu, the front end, or another of the game's menus or prompts (the score
// screen, dialogs). Not during Bink movies, in-engine cutscenes, loading or gameplay.
bool PointerWanted();
const char* Name(State s);
// The game's own Options menu, when the PC page was opened from its Options entry (pause or title menu): run that
// entry after all (on the Game Thread, next frame).
bool CanOpenOriginalOptions();
void OpenOriginalOptions();
// The page closed (any thread): Options the page was opened over close too, as the game closes them.
void PageClosed();
// Log where the player stands on the next Game Thread tick (F12, with its screenshot).
void RequestPositionLog();
// An in-engine cutscene (cinematic mode) or a full-screen Bink movie is playing. Any thread.
bool CutsceneOrMovie();
} // namespace game_ui
