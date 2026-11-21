#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "exclusive.h"

#include "../log.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <new>

namespace gpu::exclusive
{
#if defined(_WIN32)
namespace
{
struct Structs
{
    VkSurfaceFullScreenExclusiveInfoEXT info;
    VkSurfaceFullScreenExclusiveWin32InfoEXT win32;
};
static_assert(sizeof(Structs) <= 96);
} // namespace

bool InstanceExtension(const char*& name)
{
    name = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    return true;
}

bool DeviceExtension(const char*& name)
{
    name = VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME;
    return true;
}

const void* Swapchain::Chain(SDL_Window* window, bool enabled, bool exclusive, const void* next)
{
    applicationControlled = false;
    if (!enabled || !exclusive)
        return next;
    HWND hwnd = HWND(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        return next;
    Structs* s = new (storage) Structs{};
    s->info.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    s->info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    s->info.pNext = &s->win32;
    s->win32.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
    s->win32.hmonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    s->win32.pNext = const_cast<void*>(next);
    applicationControlled = true;
    return &s->info;
}

bool Swapchain::Acquire(VkDevice device, VkSwapchainKHR swapchain)
{
    if (!applicationControlled)
        return true;
    const VkResult r = vkAcquireFullScreenExclusiveModeEXT(device, swapchain);
    if (r != VK_SUCCESS)
        dtlog::Important("[dt-port] exclusive fullscreen not acquired (%s); presenting through the desktop\n", VkResultName(r));
    else
        dtlog::Important("[dt-port] exclusive fullscreen acquired\n");
    return r == VK_SUCCESS;
}
#else
bool InstanceExtension(const char*&) { return false; }
bool DeviceExtension(const char*&) { return false; }
const void* Swapchain::Chain(SDL_Window*, bool, bool, const void* next) { return next; }
bool Swapchain::Acquire(VkDevice, VkSwapchainKHR) { return true; }
#endif
} // namespace gpu::exclusive
