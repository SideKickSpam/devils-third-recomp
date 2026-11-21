// The game's menu fonts, read from the player's UI_Fonts.xxx (the fonts_en movie) for the overlay.
#pragma once

#include "swf_font.h"

#include <string>
#include <vector>

namespace assets
{
// Load the fonts (once; later calls return the same list). Empty if the package cannot be read.
const std::vector<SwfFont>& GameFonts();
// The font named `name` (as in the movie, e.g. "Eurostile Next LT Pro"), or null.
const SwfFont* GameFont(const std::string& name);
// Debugging (--font-sample DIR): a PGM image per font with a sample line, to see which font is which.
void WriteFontSamples(const std::string& dir);
} // namespace assets
