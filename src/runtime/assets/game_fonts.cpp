#include "game_fonts.h"

#include "../log.h"
#include "../vfs.h"
#include "ue3_package.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace assets
{
const std::vector<SwfFont>& GameFonts()
{
    static std::vector<SwfFont> fonts;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto path = vfs::Resolve("/vol/content/DevilsThirdGame/CookedWiiU/UI_Fonts.xxx");
        Package package;
        std::string error;
        if (!path || !package.Load(*path, error))
        {
            dtlog::Important("[fonts] cannot read the game's fonts: %s\n", path ? error.c_str() : "UI_Fonts.xxx not found");
            return;
        }
        const auto movieObject = package.Find("SwfMovie", "fonts_en");
        const auto movie = movieObject ? MovieBytes(*movieObject) : std::nullopt;
        if (!movie)
        {
            dtlog::Important("[fonts] no fonts_en movie in UI_Fonts.xxx\n");
            return;
        }
        fonts = ParseFonts(*movie);
        for (const SwfFont& f : fonts)
            dtlog::Write("[fonts] %s%s: %zu glyphs\n", f.name.c_str(), f.bold ? " (bold)" : "", f.glyphs.size());
        dtlog::Important("[fonts] %zu fonts from the game's UI_Fonts.xxx\n", fonts.size());
    });
    return fonts;
}

const SwfFont* GameFont(const std::string& name)
{
    for (const SwfFont& f : GameFonts())
        if (f.name == name)
            return &f;
    return nullptr;
}

void WriteFontSamples(const std::string& dir)
{
    std::filesystem::create_directories(dir);
    const char* sample = "Horizontal Sensitivity  Normal  On  Default  Controls  PC Settings 0123456789";
    int index = 0;
    for (const SwfFont& f : GameFonts())
    {
        if (getenv("DT_FONT_DEBUG"))
            for (size_t i = 0; i < f.glyphs.size() && i < 400; i++)
                if (f.glyphs[i].maxX - f.glyphs[i].minX > 30000 || f.glyphs[i].maxY - f.glyphs[i].minY > 30000)
                    dtlog::Important("  %s glyph %zu: %g..%g x %g..%g, %zu contours\n", f.name.c_str(), i, f.glyphs[i].minX,
                        f.glyphs[i].maxX, f.glyphs[i].minY, f.glyphs[i].maxY, f.glyphs[i].contours.size());
        const float scale = 40.0f / (f.ascent + f.descent);
        const int height = 56, width = 1800;
        std::vector<uint8_t> image(size_t(width) * height, 0);
        float pen = 8;
        const int baseline = int(f.ascent * scale) + 6;
        for (const char* c = sample; *c; c++)
        {
            const SwfFont::Glyph* g = f.Find(uint8_t(*c));
            if (!g)
                continue;
            const GlyphBitmap b = Rasterize(*g, scale);
            for (int y = 0; y < b.height; y++)
                for (int x = 0; x < b.width; x++)
                {
                    const int px = int(pen) + b.left + x, py = baseline + b.top + y;
                    if (px >= 0 && px < width && py >= 0 && py < height)
                        image[size_t(py) * width + px] = std::max(image[size_t(py) * width + px], b.alpha[size_t(y) * b.width + x]);
                }
            pen += g->advance * scale;
        }
        char name[64];
        snprintf(name, sizeof(name), "/font%02d.pgm", index++);
        if (FILE* out = fopen((dir + name).c_str(), "wb"))
        {
            fprintf(out, "P5\n%d %d\n255\n", width, height);
            fwrite(image.data(), 1, image.size(), out);
            fclose(out);
        }
        dtlog::Important("[fonts] %s%s -> %s\n", f.name.c_str(), f.bold ? " (bold)" : "", (dir + name).c_str());
    }
}
} // namespace assets
