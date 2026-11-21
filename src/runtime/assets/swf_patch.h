// Patches for the game's Scaleform movies, applied in memory as the game loads them (gfx_hooks.cpp), never to files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace assets::swf_patch
{
// The pause menu (UI_InGame.PauseMenu_new): a "PC Settings" button between Options and Back to Title on the
// single-player frame, a clone of the Options button whose label reads `label`. Pressing it calls
// ExternalInterface.call("DtPortSettings"); focusing it calls "DtPortFocus". False (movie unchanged) if the movie is not
// laid out as expected.
bool AddPauseMenuButton(std::vector<uint8_t>& movie, const std::string& label);
// The title menu (UI_FrontEnd.TitleMenu): a "PC Settings" tile after Options, a clone of the Options tile, moving with it.
bool AddTitleMenuButton(std::vector<uint8_t>& movie, const std::string& label);
// The prompt library (UI_Common.ButtonLibrary): each Wii U button picture becomes a sprite holding it and variants
// (caps with text, faces with letters, PlayStation symbols, mouse pictures); a script shows the one named by the
// movie's _global.dtK_<button> (with dtT_ text as HTML, dtW_ cap width and dtS_ text scale, re-read when _global.dtPG
// changes). Buttons: A B X Y L R ZL ZR Plus Minus.
bool AddPromptVariants(std::vector<uint8_t>& movie);
// A movie's own drawings of button prompts: the art inside each `holder` (an exported sprite, or a sprite placed with
// that instance name) becomes the prompt library's container `container`, imported, so it follows the device like the
// shared prompts. The game's Options footer: "+ Save" (save_label) and "Y Default" (default_settings).
struct LocalPrompt
{
    std::string holder, container;
};
bool UseLibraryPrompts(std::vector<uint8_t>& movie, const std::vector<LocalPrompt>& prompts);
// The text field `child` of the exported sprite `spriteExport`: its box grown by `left`/`right` pixels, its font scaled.
// (`down` moves the box down by that many pixels.)
bool ResizeEditText(std::vector<uint8_t>& movie, const std::string& spriteExport, const std::string& child, float left,
    float right, float fontScale, float down = 0);
// The menus' mouse pointer: a script on the movie's first frame focuses the CLIK button under the pointer (as the game's
// D3Button roll-over does) and reports a click on it (ExternalInterface DtPortClick).
bool AddMenuMouse(std::vector<uint8_t>& movie);
// The Score Attack loadout's Primary and Secondary slots turn light blue while _global.dtXGear has their bit (1, 2).
bool AddXGearTint(std::vector<uint8_t>& movie);
// A sprite (by exported name) drawn through a colour transform (percent and offset per channel).
bool TintSprite(std::vector<uint8_t>& movie, const std::string& spriteExport, int ra, int rb, int ga, int gb, int ba, int bb);
// Removes the placements at `depth` from the exported sprite (testing, and pictures a patch makes obsolete).
bool RemoveDepth(std::vector<uint8_t>& movie, const std::string& spriteExport, uint16_t depth);
} // namespace assets::swf_patch
