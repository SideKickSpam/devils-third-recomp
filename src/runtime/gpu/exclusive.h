// Exclusive fullscreen on Windows (VK_EXT_full_screen_exclusive). Elsewhere these do nothing: the window system
// decides how a fullscreen window is presented.
#pragma once

#include "vulkan.h"

#include <vector>

struct SDL_Window;

namespace gpu::exclusive
{
// Extensions to enable when available: instance (VK_KHR_get_surface_capabilities2) and device
// (VK_EXT_full_screen_exclusive). Each returns false when the platform has none.
bool InstanceExtension(const char*& name);
bool DeviceExtension(const char*& name);

// State for one swapchain, chained into VkSwapchainCreateInfoKHR::pNext.
class Swapchain
{
public:
    // `enabled`: the extension is on; `exclusive`: take the monitor (application-controlled), otherwise leave
    // the decision to the driver.
    const void* Chain(SDL_Window* window, bool enabled, bool exclusive, const void* next);
    // After creating the swapchain: acquire exclusive mode if it was asked for. Returns false if it failed
    // (presentation still works, just not exclusively).
    bool Acquire(VkDevice device, VkSwapchainKHR swapchain);

private:
    [[maybe_unused]] alignas(8) unsigned char storage[96] = {};
    [[maybe_unused]] bool applicationControlled = false;
};
} // namespace gpu::exclusive
