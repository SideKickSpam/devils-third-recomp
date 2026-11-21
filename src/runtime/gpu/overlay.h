// The overlay: Dear ImGui drawn on top of the presented image, at the window's resolution. It shows the settings menu
// (host/menu.*) when open, and the frame-time readout (display.show_fps) at any time. It runs on the thread that
// presents (the GPU thread); window events reach it through a queue.
#pragma once

#include "vulkan.h"

union SDL_Event;

namespace gpu
{
class Device;
}

namespace gpu::overlay
{
// The fonts the menu draws with: the game's own (from the player's UI_Fonts.xxx), or Roboto when they cannot be read.
struct Fonts
{
    void* label = nullptr;  // ImFont*: row labels and descriptions (NewCezanne Pro B)
    void* value = nullptr;  // values, titles and buttons (Mechanical NewRodin UB)
    void* tab = nullptr;    // tab captions (OptiB NewRodin B85)
};
const Fonts& MenuFonts();

// Window thread: an event for the overlay (keyboard, mouse, text) while the menu is open.
void PushEvent(const SDL_Event& e);

// Presenting thread.
bool WantsToDraw();
// Draw into `target` (in COLOR_ATTACHMENT_OPTIMAL; left in it) with dynamic rendering.
void Record(Device& device, VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);
} // namespace gpu::overlay
