// Host window (SDL3). The window lives on its own thread, which creates it and pumps its events, because on
// Windows a window's messages must be processed by the thread that created it, and the game calls GX2 from
// several threads. The renderer creates its Vulkan surface from the window handle.
#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace host
{
// How the window is shown (setting display.mode). Alt+Enter and F11 switch between windowed and the fullscreen
// kind in display.fullscreen.
enum class DisplayMode
{
    Windowed,
    Borderless,  // a desktop-mode window covering the monitor
    Exclusive,   // the display switches to the chosen mode; the swapchain takes the monitor (Windows)
};
const char* DisplayModeName(DisplayMode mode);
DisplayMode ParseDisplayMode(const std::string& name, DisplayMode fallback);

// Start the window thread and wait until the window exists. Returns false if no display is available; the
// game then keeps running without a window (headless).
bool OpenWindow(const char* title);
SDL_Window* Window();          // null when headless
bool QuitRequested();          // the user closed the window (or Ctrl+C / SIGTERM)
// Called on the window thread as soon as a quit is requested, whatever the game is doing. Expected to exit.
void SetQuitHandler(void (*handler)(const char* reason));
void SetWindowEnabled(bool enabled);  // --no-window
void SetWindowHidden(bool hidden);    // --hidden: render as usual but keep the window off screen

// Switch the window to `mode` (from any thread; applied on the window thread) and save it as display.mode.
void RequestDisplayMode(DisplayMode mode);
DisplayMode CurrentDisplayMode();
// Changes whenever the swapchain should be recreated: the window was resized, changed mode or regained focus.
uint64_t DisplayGeneration();
// Ask for a swapchain rebuild (a presentation setting changed).
void NoteDisplayChanged();
bool HasFocus();
// The settings menu opened or closed (host/menu.*): the window thread releases the mouse. Any thread.
void NoteMenuChanged();
} // namespace host
