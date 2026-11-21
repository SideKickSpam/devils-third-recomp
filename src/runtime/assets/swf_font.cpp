#include "swf_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace assets
{
namespace
{
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// SWF bit fields: most significant bit first.
struct Bits
{
    const uint8_t* p;
    size_t size, bit = 0;
    uint32_t U(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, bit++)
            v = v << 1 | (bit / 8 < size ? (p[bit / 8] >> (7 - bit % 8)) & 1 : 0);
        return v;
    }
    int32_t S(int n)
    {
        if (n == 0)
            return 0;
        const uint32_t v = U(n);
        return (v & (1u << (n - 1))) ? int32_t(v | ~((1u << n) - 1)) : int32_t(v);
    }
};

// One glyph's SHAPE: edges into closed polylines, quadratic curves flattened.
SwfFont::Glyph ParseShape(const uint8_t* p, size_t size)
{
    SwfFont::Glyph g;
    Bits b{ p, size };
    const int fillBits = int(b.U(4)), lineBits = int(b.U(4));
    float x = 0, y = 0;
    std::vector<SwfFont::Point>* contour = nullptr;
    auto start = [&] {
        g.contours.emplace_back();
        contour = &g.contours.back();
        contour->push_back({ x, y });
    };
    for (;;)
    {
        if (b.bit / 8 >= size)
            break;
        if (b.U(1) == 0)
        {
            const uint32_t flags = b.U(5);
            if (flags == 0)
                break;  // end of shape
            if (flags & 1)  // move to (absolute)
            {
                const int n = int(b.U(5));
                x = float(b.S(n)), y = float(b.S(n));
                start();
            }
            if (flags & 2)
                b.U(fillBits);
            if (flags & 4)
                b.U(fillBits);
            if (flags & 8)
                b.U(lineBits);
            if (flags & 16)
                break;  // new styles: not used in font glyphs
            continue;
        }
        if (!contour)
            start();
        const bool straight = b.U(1);
        const int n = int(b.U(4)) + 2;
        if (straight)
        {
            int32_t dx = 0, dy = 0;
            if (b.U(1))
                dx = b.S(n), dy = b.S(n);
            else if (b.U(1))
                dy = b.S(n);
            else
                dx = b.S(n);
            x += float(dx), y += float(dy);
            contour->push_back({ x, y });
        }
        else  // quadratic curve: control then anchor
        {
            const float cx = x + float(b.S(n)), cy = y + float(b.S(n));
            const float ax = cx + float(b.S(n)), ay = cy + float(b.S(n));
            constexpr int Steps = 8;
            for (int i = 1; i <= Steps; i++)
            {
                const float t = float(i) / Steps, u = 1 - t;
                contour->push_back({ u * u * x + 2 * u * t * cx + t * t * ax, u * u * y + 2 * u * t * cy + t * t * ay });
            }
            x = ax, y = ay;
        }
    }
    bool first = true;
    for (auto& c : g.contours)
        for (auto& pt : c)
        {
            if (first)
                g.minX = g.maxX = pt.x, g.minY = g.maxY = pt.y, first = false;
            g.minX = std::min(g.minX, pt.x), g.maxX = std::max(g.maxX, pt.x);
            g.minY = std::min(g.minY, pt.y), g.maxY = std::max(g.maxY, pt.y);
        }
    return g;
}

bool ParseDefineFont3(const uint8_t* body, size_t size, SwfFont& f)
{
    if (size < 7)
        return false;
    const uint8_t flags = body[2];
    const bool hasLayout = flags & 0x80, wideOffsets = flags & 0x08, wideCodes = flags & 0x04;
    f.italic = flags & 0x02, f.bold = flags & 0x01;
    const size_t nameLen = body[4];
    if (5 + nameLen + 2 > size)
        return false;
    f.name.assign(reinterpret_cast<const char*>(body + 5), nameLen);
    while (!f.name.empty() && f.name.back() == '\0')
        f.name.pop_back();
    size_t o = 5 + nameLen;
    const uint32_t count = U16(body + o);
    o += 2;
    const size_t tableAt = o, offsetSize = wideOffsets ? 4 : 2;
    if (tableAt + (count + 1) * offsetSize > size)
        return false;
    auto offset = [&](uint32_t i) { return wideOffsets ? U32(body + tableAt + i * 4) : U16(body + tableAt + i * 2); };
    const size_t codeTable = tableAt + offset(count);
    f.glyphs.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        const size_t a = tableAt + offset(i), e = i + 1 < count ? tableAt + offset(i + 1) : codeTable;
        if (a <= e && e <= size)
            f.glyphs[i] = ParseShape(body + a, e - a);
    }
    o = codeTable;
    for (uint32_t i = 0; i < count && o + (wideCodes ? 2 : 1) <= size; i++)
    {
        const uint32_t code = wideCodes ? U16(body + o) : body[o];
        o += wideCodes ? 2 : 1;
        f.glyphOf.emplace(code, i);
    }
    if (hasLayout && o + 6 + count * 2 <= size)
    {
        f.ascent = float(int16_t(U16(body + o))), f.descent = float(int16_t(U16(body + o + 2)));
        o += 6;  // ascent, descent, leading
        for (uint32_t i = 0; i < count; i++)
            f.glyphs[i].advance = float(int16_t(U16(body + o + i * 2)));
    }
    else
    {
        f.ascent = 20480 * 0.8f, f.descent = 20480 * 0.2f;
        for (auto& g : f.glyphs)
            g.advance = g.maxX + 20480 * 0.05f;
    }
    return true;
}

void ParseTags(const uint8_t* p, size_t size, std::vector<SwfFont>& out)
{
    size_t o = 0;
    while (o + 2 <= size)
    {
        const uint16_t codeLen = U16(p + o);
        o += 2;
        const uint32_t code = codeLen >> 6;
        size_t len = codeLen & 0x3F;
        if (len == 0x3F)
        {
            if (o + 4 > size)
                break;
            len = U32(p + o);
            o += 4;
        }
        if (o + len > size)
            break;
        if (code == 75)  // DefineFont3
        {
            SwfFont f;
            if (ParseDefineFont3(p + o, len, f))
                out.push_back(std::move(f));
        }
        else if (code == 39 && len > 4)  // DefineSprite: its own tags
            ParseTags(p + o + 4, len - 4, out);
        o += len;
        if (code == 0)
            break;
    }
}

// Signed-area coverage accumulation (as in font-rs): each edge adds its covered area to the cells it crosses; a
// running sum over the buffer then gives every pixel's coverage.
void Line(std::vector<float>& acc, int w, int h, SwfFont::Point a, SwfFont::Point b)
{
    if (a.y == b.y)
        return;
    float dir = 1;
    if (a.y > b.y)
        std::swap(a, b), dir = -1;
    const float dxdy = (b.x - a.x) / (b.y - a.y);
    float x = a.x;
    if (a.y < 0)
        x -= a.y * dxdy;
    const int yStart = std::max(0, int(a.y)), yEnd = std::min(h, int(std::ceil(b.y)));
    for (int y = yStart; y < yEnd; y++)
    {
        const size_t row = size_t(y) * size_t(w);
        const float dy = std::min(float(y + 1), b.y) - std::max(float(y), a.y);
        const float xnext = x + dxdy * dy, d = dy * dir;
        const float x0 = std::min(x, xnext), x1 = std::max(x, xnext);
        const float x0floor = std::floor(x0), x1ceil = std::ceil(x1);
        const int x0i = int(x0floor), x1i = int(x1ceil);
        auto add = [&](int xi, float v) {
            if (xi >= 0 && xi <= w)
                acc[row + size_t(xi)] += v;
        };
        if (x1i <= x0i + 1)
        {
            const float xmf = 0.5f * (x + xnext) - x0floor;
            add(x0i, d - d * xmf);
            add(x0i + 1, d * xmf);
        }
        else
        {
            const float s = 1.0f / (x1 - x0), x0f = x0 - x0floor, a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            const float x1f = x1 - x1ceil + 1, am = 0.5f * s * x1f * x1f;
            add(x0i, d * a0);
            if (x1i == x0i + 2)
                add(x0i + 1, d * (1 - a0 - am));
            else
            {
                const float a1 = s * (1.5f - x0f);
                add(x0i + 1, d * (a1 - a0));
                for (int xi = x0i + 2; xi < x1i - 1; xi++)
                    add(xi, d * s);
                const float a2 = a1 + float(x1i - x0i - 3) * s;
                add(x1i - 1, d * (1 - a2 - am));
            }
            add(x1i, d * am);
        }
        x = xnext;
    }
}
} // namespace

std::vector<SwfFont> ParseFonts(const std::vector<uint8_t>& movie)
{
    std::vector<SwfFont> fonts;
    if (movie.size() < 12)
        return fonts;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;  // header, stage rectangle, frame rate and count
    if (start < movie.size())
        ParseTags(movie.data() + start, movie.size() - start, fonts);
    return fonts;
}

GlyphBitmap Rasterize(const SwfFont::Glyph& glyph, float scale)
{
    GlyphBitmap bm;
    if (glyph.contours.empty() || glyph.maxX <= glyph.minX || glyph.maxY <= glyph.minY)
        return bm;
    bm.left = int(std::floor(glyph.minX * scale));
    bm.top = int(std::floor(glyph.minY * scale));
    bm.width = int(std::ceil(glyph.maxX * scale)) - bm.left + 1;
    bm.height = int(std::ceil(glyph.maxY * scale)) - bm.top + 1;
    std::vector<float> acc(size_t(bm.width) * size_t(bm.height) + size_t(bm.width) + 4, 0.0f);
    for (const auto& c : glyph.contours)
        for (size_t i = 0; i < c.size(); i++)
        {
            const SwfFont::Point& p0 = c[i];
            const SwfFont::Point& p1 = c[(i + 1) % c.size()];  // contours close back to their start
            Line(acc, bm.width, bm.height, { p0.x * scale - float(bm.left), p0.y * scale - float(bm.top) },
                { p1.x * scale - float(bm.left), p1.y * scale - float(bm.top) });
        }
    bm.alpha.resize(size_t(bm.width) * size_t(bm.height));
    float sum = 0;
    for (size_t i = 0; i < bm.alpha.size(); i++)
    {
        sum += acc[i];
        bm.alpha[i] = uint8_t(std::min(1.0f, std::abs(sum)) * 255.0f + 0.5f);
    }
    return bm;
}
} // namespace assets
