#include "window.h"
#include "../gpu/renderer.h"
#include "../game_ui.h"

#include "../cafe/gx2.h"
#include "../log.h"
#include "../platform.h"
#include "../settings.h"
#include "audio.h"
#include "bindings.h"
#include "input.h"
#include "menu.h"
#include "../gpu/overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

namespace host
{
namespace
{
std::atomic<SDL_Window*> gWindow{ nullptr };
std::atomic<bool> gQuit{ false };
bool gEnabled = true;
bool gHidden = false;
bool gStarted = false;
void (*gQuitHandler)(const char* reason) = nullptr;

std::mutex gReadyMutex;
std::condition_variable gReady;
bool gReadyFlag = false;
bool gOpened = false;

std::atomic<DisplayMode> gMode{ DisplayMode::Windowed };
std::atomic<uint64_t> gGeneration{ 0 };
std::atomic<bool> gFocus{ true };
uint32_t gModeEvent = 0;  // SDL user event: switch to the display mode in event.user.code
uint32_t gMenuEvent = 0;  // SDL user event: the settings menu opened or closed

// Events for the settings menu while it is open: keyboard, mouse and text input.
bool IsMenuInput(const SDL_Event& e)
{
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// The exclusive fullscreen mode: display.exclusive_width/height/refresh, 0 = the desktop's.
bool ExclusiveMode(SDL_Window* w, SDL_DisplayMode& mode)
{
    const SDL_DisplayID display = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop)
        return false;
    const int width = settings::GetInt("display.exclusive_width", 0), height = settings::GetInt("display.exclusive_height", 0);
    float refresh = settings::GetFloat("display.exclusive_refresh", 0);
    if (refresh <= 0)
    {
        // The highest refresh rate the display offers at that size (VRR monitors report their maximum).
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
        for (int i = 0; i < count; i++)
            if (modes[i]->w == (width ? width : desktop->w) && modes[i]->h == (height ? height : desktop->h))
                refresh = std::max(refresh, modes[i]->refresh_rate);
        SDL_free(modes);
    }
    return SDL_GetClosestFullscreenDisplayMode(display, width ? width : desktop->w, height ? height : desktop->h,
        refresh, false, &mode);
}

void ApplyMode(SDL_Window* w, DisplayMode mode)
{
    bool ok = true;
    if (mode == DisplayMode::Windowed)
        ok = SDL_SetWindowFullscreen(w, false);
    else if (mode == DisplayMode::Borderless)
        ok = SDL_SetWindowFullscreenMode(w, nullptr) && SDL_SetWindowFullscreen(w, true);
    else
    {
        SDL_DisplayMode dm{};
        ok = ExclusiveMode(w, dm) && SDL_SetWindowFullscreenMode(w, &dm) && SDL_SetWindowFullscreen(w, true);
        if (ok)
            dtlog::Important("[dt-port] exclusive fullscreen %dx%d at %.2f Hz\n", dm.w, dm.h, dm.refresh_rate);
    }
    if (!ok)
    {
        dtlog::Important("[dt-port] cannot switch to %s: %s\n", DisplayModeName(mode), SDL_GetError());
        return;
    }
    SDL_SyncWindow(w);
    gMode = mode;
    gGeneration++;
    dtlog::Important("[dt-port] display mode %s\n", DisplayModeName(mode));
}

// Alt+Enter and F11: windowed <-> the configured fullscreen kind. Returns true if the key was consumed.
bool HandleToggle(SDL_Window* w, const SDL_Event& e)
{
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP)
        return false;
    const bool altEnter = (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT);
    if (!altEnter && e.key.key != SDLK_F11)
        return false;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !gHidden)
    {
        const DisplayMode full = ParseDisplayMode(settings::Get("display.fullscreen", "borderless"), DisplayMode::Borderless);
        const DisplayMode next = gMode == DisplayMode::Windowed ? (full == DisplayMode::Windowed ? DisplayMode::Borderless : full)
                                                                : DisplayMode::Windowed;
        ApplyMode(w, next);
        settings::Set("display.mode", DisplayModeName(gMode));
        settings::Save();
    }
    return true;
}

void WindowThread(const char* title)
{
    // Signals and console events are handled in platform::InstallQuitSignals, which says what caused a quit.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // SDL keeps the screen on while video is initialised; a hidden window (unattended runs) should not.
    if (gHidden)
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    bool ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    SDL_Window* w = nullptr;
    const int width = std::max(320, settings::GetInt("display.window_width", 1280));
    const int height = std::max(180, settings::GetInt("display.window_height", 720));
    if (!ok)
        dtlog::Important("[dt-port] no window: SDL_Init failed: %s\n", SDL_GetError());
    else
    {
        gModeEvent = SDL_RegisterEvents(2);
        gMenuEvent = gModeEvent + 1;
        w = SDL_CreateWindow(title, width, height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | (gHidden ? SDL_WINDOW_HIDDEN : 0));
        if (!w)
            dtlog::Important("[dt-port] no window: SDL_CreateWindow failed: %s\n", SDL_GetError());
        else
        {
            SDL_SetWindowMinimumSize(w, 320, 180);
            dtlog::Important("[dt-port] opened %dx%d window (SDL %d.%d.%d, video driver %s)\n", width, height,
                SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
            const DisplayMode mode = gHidden ? DisplayMode::Windowed
                                             : ParseDisplayMode(settings::Get("display.mode", "windowed"), DisplayMode::Windowed);
            if (mode != DisplayMode::Windowed)
                ApplyMode(w, mode);
        }
    }
    gWindow = w;
    if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS))
        SDL_HideCursor();  // (as on focus: see SDL_EVENT_WINDOW_FOCUS_GAINED)
    {
        std::lock_guard lock(gReadyMutex);
        gReadyFlag = true;
        gOpened = w != nullptr;
    }
    gReady.notify_all();
    if (!w)
        return;
    audio::OpenDevice();
    // DT_TEST_KEYS="frame:key[:frames];..." (testing): presses an SDL key (by name, e.g. Escape, Return) at that frame
    // for that many frames (default 4), as real key events through this loop. "WheelUp" and "WheelDown" send a notch of
    // the mouse wheel instead.
    struct TestKey
    {
        uint64_t frame, until;
        SDL_Keycode key;
        int state = 0;  // 0 not yet, 1 down, 2 done
        int wheel = 0;  // +1 up, -1 down: a wheel notch, not a key
    };
    std::vector<TestKey> testKeys;
    if (const char* spec = getenv("DT_TEST_KEYS"))
        for (std::string rest = spec; !rest.empty();)
        {
            const size_t end = std::min(rest.find(';'), rest.size());
            const std::string item = rest.substr(0, end);
            rest = end < rest.size() ? rest.substr(end + 1) : "";
            unsigned long long f = 0, n = 4;
            char name[32] = {};
            if (sscanf(item.c_str(), "%llu:%31[^:]:%llu", &f, name, &n) >= 2)
            {
                const std::string n2 = name;
                testKeys.push_back({ f, f + n, SDL_GetKeyFromName(name), 0, n2 == "WheelUp" ? 1 : n2 == "WheelDown" ? -1 : 0 });
            }
        }
    while (!gQuit)
    {
        for (TestKey& k : testKeys)
        {
            const uint64_t now = cafe::gx2::FrameCount();
            if (k.wheel && k.state == 0 && now >= k.frame)
            {
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_WHEEL;
                ev.wheel.windowID = SDL_GetWindowID(w), ev.wheel.y = float(k.wheel), ev.wheel.integer_y = k.wheel;
                float mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                ev.wheel.mouse_x = mx, ev.wheel.mouse_y = my;
                SDL_PushEvent(&ev);
                k.state = 2;
            }
            if (!k.wheel && k.state < 2 && now >= (k.state ? k.until : k.frame))
            {
                SDL_Event ev{};
                ev.type = k.state ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
                ev.key.windowID = SDL_GetWindowID(w), ev.key.key = k.key, ev.key.down = !k.state;
                ev.key.scancode = SDL_GetScancodeFromKey(k.key, nullptr);
                SDL_PushEvent(&ev);
                k.state++;
            }
        }
        input::ReleaseMouseOutsideGameplay(w);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, testKeys.empty() ? 100 : 5))
            continue;
        switch (e.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            gQuit = true;
            if (gQuitHandler)
                gQuitHandler(e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ? "window closed" : "SDL quit event");
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            gGeneration++;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            // Remember the size of the plain window (not fullscreen or maximised) for the next start.
            if (gMode == DisplayMode::Windowed && !(SDL_GetWindowFlags(w) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)))
            {
                settings::SetInt("display.window_width", e.window.data1), settings::SetInt("display.window_height", e.window.data2);
                settings::Save();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            gFocus = true;
            // The Windows cursor never shows over the focused game: menus show the game's own cursor (gfx_hooks.cpp,
            // the menus' mouse pointer) and the PC page draws its own (overlay).
            SDL_HideCursor();
            // Exclusive fullscreen is lost with focus; the next swapchain takes it again. Other modes keep theirs: a
            // rebuild stalls a present 45-110 ms, and a popup taking focus (a notification, an overlay) had caused
            // bursts of them in borderless play.
            if (gMode == DisplayMode::Exclusive)
                gGeneration++;
            dtlog::Important("[dt-port] window focus gained\n");
            audio::UpdateVolume();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            gFocus = false;
            dtlog::Important("[dt-port] window focus lost\n");
            SDL_ShowCursor();
            audio::UpdateVolume();
            break;
        default:
            if (gModeEvent && e.type == gModeEvent)
            {
                ApplyMode(w, DisplayMode(e.user.code));
                break;
            }
            if (gMenuEvent && e.type == gMenuEvent && menu::IsOpen())
                input::ReleaseMouse(w);  // the menu takes the mouse
            break;
        }
        // (leaving relative mouse mode or another window can show the Windows cursor again: hidden while focused)
        if (gFocus && SDL_CursorVisible())
            SDL_HideCursor();
        input::NoteEvent(e);  // the device last touched (prompts, conventions)
        // F12 saves a screenshot (the game picture at full internal resolution) into src/notes/screenshots/.
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12)
        {
            gpu::renderer::RequestUserScreenshot();
            game_ui::RequestPositionLog();
            continue;
        }
        // The Controls tab waiting for a key or button to bind takes it first.
        if (bindings::CaptureEvent(e))
            continue;
        // F1 opens and closes the settings menu; Escape closes it.
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.key == SDLK_F1 || (e.key.key == SDLK_ESCAPE && menu::IsOpen())))
        {
            menu::Toggle(e.key.key == SDLK_F1 ? "F1" : "Escape");
            continue;
        }
        if (HandleToggle(w, e))
            continue;
        if (menu::IsOpen() && IsMenuInput(e))
        {
            gpu::overlay::PushEvent(e);
            // (the menus' pointer follows the mouse under the page too: closing it shows the game's cursor there)
            if (e.type == SDL_EVENT_MOUSE_MOTION)
                input::NotePointer(e.motion.x, e.motion.y);
        }
        else
            input::HandleEvent(e);
    }
}
} // namespace

const char* DisplayModeName(DisplayMode mode)
{
    switch (mode)
    {
    case DisplayMode::Borderless: return "borderless";
    case DisplayMode::Exclusive: return "exclusive";
    default: return "windowed";
    }
}

DisplayMode ParseDisplayMode(const std::string& name, DisplayMode fallback)
{
    if (name == "windowed")
        return DisplayMode::Windowed;
    if (name == "borderless")
        return DisplayMode::Borderless;
    if (name == "exclusive" || name == "fullscreen")
        return DisplayMode::Exclusive;
    return fallback;
}

void SetWindowEnabled(bool enabled) { gEnabled = enabled; }
void SetWindowHidden(bool hidden) { gHidden = hidden; }

bool OpenWindow(const char* title)
{
    if (!gEnabled)
        return false;
    if (!gStarted)
    {
        gStarted = true;
        platform::StartThread([title] { WindowThread(title); }, 1 << 20);
    }
    std::unique_lock lock(gReadyMutex);
    gReady.wait(lock, [] { return gReadyFlag; });
    return gOpened;
}

void RequestDisplayMode(DisplayMode mode)
{
    if (!gWindow || !gModeEvent || gHidden)
        return;
    settings::Set("display.mode", DisplayModeName(mode));
    SDL_Event e{};
    e.type = gModeEvent;
    e.user.code = int32_t(mode);
    SDL_PushEvent(&e);
}

SDL_Window* Window() { return gWindow; }
bool QuitRequested() { return gQuit; }
void SetQuitHandler(void (*handler)(const char* reason)) { gQuitHandler = handler; }
DisplayMode CurrentDisplayMode() { return gMode; }
uint64_t DisplayGeneration() { return gGeneration; }
void NoteDisplayChanged() { gGeneration++; }

void NoteMenuChanged()
{
    if (!gMenuEvent)
        return;
    SDL_Event e{};
    e.type = gMenuEvent;
    SDL_PushEvent(&e);
}
bool HasFocus() { return gFocus; }
} // namespace host
