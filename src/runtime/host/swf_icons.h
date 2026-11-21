// Pictures of the game's Scaleform sprites for the overlay: the weapon pictures of the game's weapon library
// (UI_Common.WeaponLibrary, the art the loadout slots show), rasterized from its vector shapes as the game loaded it.
// Used by the Score Attack loadout picker (host/menu.cpp). Nothing is written anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host::swf_icons
{
// The weapon library movie, as the game loads it (gfx_hooks.cpp). Any thread.
void SetLibrary(const uint8_t* movie, size_t size);
bool HaveLibrary();

struct Picture
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight alpha
};
// The first frame of sprite `exportName`, fitted into w x h pixels and centred. With `parts`, a child placed in the
// sprite itself is drawn only if its instance name ends with one of them (a gun's set sprite holds every attachment:
// "base", "mag_default", "under_grenade"...). Empty if the sprite is unknown. Any thread.
Picture Render(const std::string& exportName, const std::vector<std::string>& parts, int w, int h);
} // namespace host::swf_icons
