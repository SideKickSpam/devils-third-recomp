// Fonts embedded in the game's Scaleform movies (SWF DefineFont3: glyph outlines of straight and quadratic edges,
// 20480 units per em), parsed from the player's own files and rasterized with anti-aliasing for the overlay.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets
{
struct SwfFont
{
    struct Point
    {
        float x, y;  // font units; y grows downward, the baseline is y = 0
    };
    struct Glyph
    {
        std::vector<std::vector<Point>> contours;  // closed polylines (curves flattened)
        float advance = 0;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::string name;
    bool bold = false, italic = false;
    float ascent = 0, descent = 0;  // font units, both positive
    std::vector<Glyph> glyphs;
    std::unordered_map<uint32_t, uint32_t> glyphOf;  // character code -> glyph index

    const Glyph* Find(uint32_t codepoint) const
    {
        auto it = glyphOf.find(codepoint);
        return it == glyphOf.end() ? nullptr : &glyphs[it->second];
    }
};

// Every DefineFont3 in a movie (top level and inside sprites).
std::vector<SwfFont> ParseFonts(const std::vector<uint8_t>& movie);

// A glyph rasterized at `scale` pixels per font unit: an alpha bitmap whose top-left pixel sits at (left, top)
// relative to the glyph's origin on the baseline.
struct GlyphBitmap
{
    int left = 0, top = 0, width = 0, height = 0;
    std::vector<uint8_t> alpha;
};
GlyphBitmap Rasterize(const SwfFont::Glyph& glyph, float scale);
} // namespace assets
