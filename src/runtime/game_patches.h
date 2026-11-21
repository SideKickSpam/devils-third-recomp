// Patches to the game's own data (game_patches.cpp), applied once after the RPX is loaded.
#pragma once

namespace game_patches
{
void Apply();
// Set UE3's frame limiter to `fps` (50 or 60) now, while the game runs (the settings menu).
void SetFrameRateCap(int fps);
}
