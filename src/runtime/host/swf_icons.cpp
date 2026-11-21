#include "swf_icons.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace host::swf_icons
{
namespace
{
std::mutex gMutex;
std::vector<uint8_t> gMovie;

struct Bits
{
    const uint8_t* p;
    size_t size, byte = 0;
    int bit = 0;
    bool ok = true;
    uint32_t U(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
        {
            if (byte >= size)
            {
                ok = false;
                return v;
            }
            v = (v << 1) | ((p[byte] >> (7 - bit)) & 1);
            if (++bit == 8)
                bit = 0, byte++;
        }
        return v;
    }
    int32_t S(int n)
    {
        if (n == 0)
            return 0;
        uint32_t v = U(n);
        if (v & (1u << (n - 1)))
            v |= ~0u << n;
        return int32_t(v);
    }
    void Align()
    {
        if (bit)
            bit = 0, byte++;
    }
    uint8_t U8()
    {
        Align();
        return byte < size ? p[byte++] : (ok = false, 0);
    }
    uint16_t U16()
    {
        const uint16_t lo = U8();
        return uint16_t(lo | (U8() << 8));
    }
};

struct Matrix
{
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;  // x' = a x + c y + tx, y' = b x + d y + ty
    Matrix operator*(const Matrix& m) const  // this after m
    {
        return { a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.tx + c * m.ty + tx,
            b * m.tx + d * m.ty + ty };
    }
    void Apply(double x, double y, double& ox, double& oy) const { ox = a * x + c * y + tx, oy = b * x + d * y + ty; }
};

struct ColorX
{
    double mul[4] = { 1, 1, 1, 1 }, add[4] = { 0, 0, 0, 0 };
    ColorX operator*(const ColorX& inner) const  // this applied after inner
    {
        ColorX r;
        for (int i = 0; i < 4; i++)
            r.mul[i] = mul[i] * inner.mul[i], r.add[i] = mul[i] * inner.add[i] + add[i];
        return r;
    }
};

Matrix ReadMatrix(Bits& b)
{
    Matrix m;
    b.Align();
    if (b.U(1))
    {
        const int n = int(b.U(5));
        m.a = b.S(n) / 65536.0, m.d = b.S(n) / 65536.0;
    }
    if (b.U(1))
    {
        const int n = int(b.U(5));
        m.b = b.S(n) / 65536.0, m.c = b.S(n) / 65536.0;
    }
    const int n = int(b.U(5));
    m.tx = b.S(n), m.ty = b.S(n);
    b.Align();
    return m;
}

ColorX ReadCxform(Bits& b)  // CXFORMWITHALPHA
{
    ColorX x;
    b.Align();
    const bool add = b.U(1), mul = b.U(1);
    const int n = int(b.U(4));
    if (mul)
        for (int i = 0; i < 4; i++)
            x.mul[i] = b.S(n) / 256.0;
    if (add)
        for (int i = 0; i < 4; i++)
            x.add[i] = b.S(n);
    b.Align();
    return x;
}

struct Rgba
{
    double c[4] = { 0, 0, 0, 0 };
    bool bitmap = false;  // a bitmap fill (its image is not in the library): see Render
};

struct Segment
{
    double x0, y0, x1, y1;
    int fill0, fill1, line;  // global style indices, 0 = none
};

struct Shape
{
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds (twips)
    std::vector<Rgba> fills;                 // index 1.. (0 unused)
    std::vector<std::pair<double, Rgba>> lines;  // width (twips), colour
    std::vector<Segment> segments;
};

struct Placement
{
    uint16_t depth = 0, character = 0;
    Matrix m;
    ColorX cx;
    std::string name;
    bool mask = false;
};

struct Sprite
{
    std::vector<Placement> frame;  // frame 1's display list, by depth
};

struct Library
{
    std::map<uint16_t, Shape> shapes;
    std::map<uint16_t, Sprite> sprites;
    std::map<std::string, uint16_t> exports;
};

Rgba ReadColor(Bits& b, bool alpha)
{
    Rgba c;
    c.c[0] = b.U8(), c.c[1] = b.U8(), c.c[2] = b.U8();
    c.c[3] = alpha ? b.U8() : 255;
    return c;
}

// FILLSTYLE: solid colours; gradients as their first colour; bitmaps marked (see Render).
Rgba ReadFill(Bits& b, int version)
{
    const uint8_t type = b.U8();
    if (type == 0x00)
        return ReadColor(b, version >= 3);
    if (type == 0x10 || type == 0x12 || type == 0x13)
    {
        ReadMatrix(b);
        const uint8_t g = b.U8();
        Rgba first;
        for (int i = 0; i < (g & 15); i++)
        {
            b.U8();
            const Rgba c = ReadColor(b, version >= 3);
            if (i == 0)
                first = c;
        }
        if (type == 0x13)
            b.U16();
        return first;
    }
    if (type >= 0x40 && type <= 0x43)
    {
        // A bitmap fill: its image is an external texture, not in the library. The picture that uses one (the katana's
        // blade) colours it with its placement's colour transform (white: multiply 0, add 255), and its outline encloses
        // it: Render fills the outline's inside with that colour instead.
        b.U16();
        ReadMatrix(b);
        Rgba body;
        body.c[0] = body.c[1] = body.c[2] = 160, body.c[3] = 255, body.bitmap = true;
        return body;
    }
    b.ok = false;
    return Rgba{};
}

void ReadStyles(Bits& b, int version, Shape& s, int& fillBase, int& lineBase)
{
    fillBase = int(s.fills.size()) - 1, lineBase = int(s.lines.size()) - 1;
    int n = b.U8();
    if (n == 0xFF && version >= 2)
        n = b.U16();
    for (int i = 0; i < n && b.ok; i++)
        s.fills.push_back(ReadFill(b, version));
    n = b.U8();
    if (n == 0xFF && version >= 2)
        n = b.U16();
    for (int i = 0; i < n && b.ok; i++)
    {
        const double width = b.U16();
        if (version >= 4)
        {
            const uint8_t f1 = b.U8();
            b.U8();
            if (((f1 >> 4) & 3) == 2)
                b.U16();  // miter limit
            s.lines.push_back({ width, (f1 & 0x08) ? ReadFill(b, version) : ReadColor(b, true) });
        }
        else
            s.lines.push_back({ width, ReadColor(b, version >= 3) });
    }
}

std::optional<Shape> ReadShape(const uint8_t* p, size_t size, int version)
{
    Bits b{ p, size };
    Shape s;
    s.fills.resize(1), s.lines.resize(1);
    b.U16();  // id
    const int n = int(b.U(5));
    s.x0 = b.S(n), s.x1 = b.S(n), s.y0 = b.S(n), s.y1 = b.S(n);
    b.Align();
    if (version >= 4)
    {
        const int m = int(b.U(5));
        b.S(m), b.S(m), b.S(m), b.S(m);
        b.Align();
        b.U8();
    }
    int fillBase = 0, lineBase = 0;
    ReadStyles(b, version, s, fillBase, lineBase);
    uint8_t counts = b.U8();
    int fillBits = counts >> 4, lineBits = counts & 15;
    double x = 0, y = 0;
    int fill0 = 0, fill1 = 0, line = 0;
    auto add = [&](double nx, double ny) {
        s.segments.push_back({ x, y, nx, ny, fill0, fill1, line });
        x = nx, y = ny;
    };
    while (b.ok)
    {
        if (b.U(1) == 0)
        {
            const uint32_t flags = b.U(5);
            if (!flags)
                break;
            if (flags & 1)  // move to
            {
                const int m = int(b.U(5));
                x = b.S(m), y = b.S(m);
            }
            if (flags & 2)
            {
                const int v = int(b.U(fillBits));
                fill0 = v ? v + fillBase : 0;
            }
            if (flags & 4)
            {
                const int v = int(b.U(fillBits));
                fill1 = v ? v + fillBase : 0;
            }
            if (flags & 8)
            {
                const int v = int(b.U(lineBits));
                line = v ? v + lineBase : 0;
            }
            if ((flags & 16) && version >= 2)
            {
                ReadStyles(b, version, s, fillBase, lineBase);
                counts = b.U8();
                fillBits = counts >> 4, lineBits = counts & 15;
                fill0 = fill1 = line = 0;
            }
        }
        else if (b.U(1))  // straight
        {
            const int m = int(b.U(4)) + 2;
            double dx = 0, dy = 0;
            if (b.U(1))
                dx = b.S(m), dy = b.S(m);
            else if (b.U(1))
                dy = b.S(m);
            else
                dx = b.S(m);
            add(x + dx, y + dy);
        }
        else  // quadratic curve, flattened
        {
            const int m = int(b.U(4)) + 2;
            const double cx = x + b.S(m), cy = y + b.S(m);
            const double ax = cx + b.S(m), ay = cy + b.S(m);
            const double sx = x, sy = y;
            for (int k = 1; k <= 8; k++)
            {
                const double t = k / 8.0, u = 1 - t;
                add(u * u * sx + 2 * u * t * cx + t * t * ax, u * u * sy + 2 * u * t * cy + t * t * ay);
            }
        }
    }
    if (!b.ok)
        return std::nullopt;
    return s;
}

Placement ReadPlacement(const uint8_t* p, size_t size, bool three)
{
    Bits b{ p, size };
    Placement pl;
    const uint8_t f = b.U8();
    const uint8_t f2 = three ? b.U8() : 0;
    pl.depth = b.U16();
    if (three && ((f2 & 0x08) || ((f2 & 0x10) && (f & 0x02))))
        while (b.byte < size && p[b.byte++])
        {
        }
    if (f & 0x02)
        pl.character = b.U16();
    if (f & 0x04)
        pl.m = ReadMatrix(b);
    if (f & 0x08)
        pl.cx = ReadCxform(b);
    if (f & 0x10)
        b.U16();
    if (f & 0x20)
    {
        b.Align();
        while (b.byte < size && p[b.byte])
            pl.name += char(p[b.byte++]);
        b.byte++;
    }
    if (f & 0x40)
        pl.mask = true;
    return pl;
}

void ReadTags(const uint8_t* p, size_t size, Library& lib, Sprite* sprite)
{
    size_t o = 0;
    bool firstFrame = true;
    while (o + 2 <= size)
    {
        const uint16_t h = uint16_t(p[o] | (p[o + 1] << 8));
        o += 2;
        const uint16_t code = h >> 6;
        size_t len = h & 0x3F;
        if (len == 0x3F)
        {
            if (o + 4 > size)
                return;
            len = size_t(p[o]) | size_t(p[o + 1]) << 8 | size_t(p[o + 2]) << 16 | size_t(p[o + 3]) << 24;
            o += 4;
        }
        if (o + len > size)
            return;
        const uint8_t* body = p + o;
        o += len;
        if (code == 0)
            return;
        if (code == 1)
        {
            firstFrame = false;
            if (sprite)
                return;
        }
        else if ((code == 2 || code == 22 || code == 32 || code == 83) && len >= 2)
        {
            const int version = code == 2 ? 1 : code == 22 ? 2 : code == 32 ? 3 : 4;
            if (auto s = ReadShape(body, len, version))
                lib.shapes[uint16_t(body[0] | (body[1] << 8))] = std::move(*s);
        }
        else if (code == 39 && len >= 4)
        {
            Sprite s;
            ReadTags(body + 4, len - 4, lib, &s);
            lib.sprites[uint16_t(body[0] | (body[1] << 8))] = std::move(s);
        }
        else if (code == 56 && len >= 2)
        {
            const size_t n = body[0] | (body[1] << 8);
            size_t q = 2;
            for (size_t i = 0; i < n && q + 2 < len; i++)
            {
                const uint16_t id = uint16_t(body[q] | (body[q + 1] << 8));
                q += 2;
                std::string name;
                while (q < len && body[q])
                    name += char(body[q++]);
                q++;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                lib.exports[name] = id;  // (by lower-case name: the game's names differ in case, gun_MP443_set / gun_mp443_set)
            }
        }
        else if (sprite && firstFrame && (code == 26 || code == 70))
        {
            const Placement pl = ReadPlacement(body, len, code == 70);
            auto it = std::find_if(sprite->frame.begin(), sprite->frame.end(), [&](const Placement& q) { return q.depth == pl.depth; });
            if (it == sprite->frame.end())
                sprite->frame.push_back(pl);
            else
            {
                // A move: changes what it says (here the character, matrix, colour and name of the first placement).
                if (pl.character)
                    it->character = pl.character;
                it->m = pl.m, it->cx = pl.cx;
                if (!pl.name.empty())
                    it->name = pl.name;
            }
        }
        else if (sprite && firstFrame && code == 28 && len >= 2)
        {
            const uint16_t depth = uint16_t(body[0] | (body[1] << 8));
            sprite->frame.erase(std::remove_if(sprite->frame.begin(), sprite->frame.end(),
                                    [&](const Placement& q) { return q.depth == depth; }),
                sprite->frame.end());
        }
    }
}

const Library* Parsed()
{
    static std::optional<Library> lib;
    static size_t parsedSize = 0;
    if (gMovie.size() < 32)
        return nullptr;
    if (!lib || parsedSize != gMovie.size())
    {
        lib.emplace();
        parsedSize = gMovie.size();
        const size_t rectBits = 5 + 4 * size_t(gMovie[8] >> 3);
        const size_t start = 8 + (rectBits + 7) / 8 + 4;
        if (start < gMovie.size())
            ReadTags(gMovie.data() + start, gMovie.size() - start, *lib, nullptr);
        for (auto& [id, s] : lib->sprites)
            std::sort(s.frame.begin(), s.frame.end(), [](const Placement& a, const Placement& b) { return a.depth < b.depth; });
    }
    return &*lib;
}

bool EndsWith(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Walks a character's shapes with their transforms (twips -> parent space) and colour transforms.
template <typename F>
void Walk(const Library& lib, uint16_t id, const Matrix& m, const ColorX& cx, const std::vector<std::string>* parts, int level, F&& f)
{
    if (level > 16)
        return;
    if (auto s = lib.shapes.find(id); s != lib.shapes.end())
    {
        f(s->second, m, cx);
        return;
    }
    auto sp = lib.sprites.find(id);
    if (sp == lib.sprites.end())
        return;
    for (const Placement& pl : sp->second.frame)
    {
        if (pl.mask || !pl.character)
            continue;
        if (parts && !parts->empty())
        {
            bool wanted = false;
            for (const std::string& part : *parts)
                wanted |= EndsWith(pl.name, part);
            if (!wanted)
                continue;
        }
        Walk(lib, pl.character, m * pl.m, cx * pl.cx, nullptr, level + 1, f);
    }
}

// Even-odd scanline fill of a set of segments (pixel coordinates) into an RGBA canvas.
void FillPolygon(std::vector<uint8_t>& canvas, int w, int h, const std::vector<std::array<double, 4>>& segs, const Rgba& col)
{
    if (segs.empty() || col.c[3] <= 0)
        return;
    double ymin = 1e30, ymax = -1e30;
    for (const auto& s : segs)
        ymin = std::min({ ymin, s[1], s[3] }), ymax = std::max({ ymax, s[1], s[3] });
    const int y0 = std::max(0, int(std::floor(ymin))), y1 = std::min(h - 1, int(std::ceil(ymax)));
    std::vector<double> xs;
    const double a = std::clamp(col.c[3] / 255.0, 0.0, 1.0);
    for (int y = y0; y <= y1; y++)
    {
        const double cy = y + 0.5;
        xs.clear();
        for (const auto& s : segs)
        {
            const double ya = s[1], yb = s[3];
            if ((ya <= cy && yb > cy) || (yb <= cy && ya > cy))
                xs.push_back(s[0] + (cy - ya) * (s[2] - s[0]) / (yb - ya));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
        {
            const int xa = std::max(0, int(std::ceil(xs[i] - 0.5))), xb = std::min(w - 1, int(std::floor(xs[i + 1] - 0.5)));
            for (int x = xa; x <= xb; x++)
            {
                uint8_t* p = &canvas[(size_t(y) * w + x) * 4];
                const double da = p[3] / 255.0, oa = a + da * (1 - a);
                for (int k = 0; k < 3; k++)
                {
                    const double c = std::clamp(col.c[k], 0.0, 255.0);
                    p[k] = uint8_t(oa > 0 ? (c * a + p[k] * da * (1 - a)) / oa : 0);
                }
                p[3] = uint8_t(oa * 255);
            }
        }
    }
}

Rgba Transform(const Rgba& c, const ColorX& x)
{
    Rgba r;
    for (int i = 0; i < 4; i++)
        r.c[i] = std::clamp(c.c[i] * x.mul[i] + x.add[i], 0.0, 255.0);
    return r;
}
} // namespace

void SetLibrary(const uint8_t* movie, size_t size)
{
    std::lock_guard lock(gMutex);
    gMovie.assign(movie, movie + size);
}

bool HaveLibrary()
{
    std::lock_guard lock(gMutex);
    return !gMovie.empty();
}

Picture Render(const std::string& exportName, const std::vector<std::string>& parts, int w, int h)
{
    std::lock_guard lock(gMutex);
    Picture out;
    const Library* lib = Parsed();
    if (!lib || w <= 0 || h <= 0)
        return out;
    std::string key = exportName;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const auto e = lib->exports.find(key);
    if (e == lib->exports.end())
        return out;
    // Bounds of what is drawn, in the sprite's space.
    double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
    Walk(*lib, e->second, Matrix{}, ColorX{}, &parts, 0, [&](const Shape& s, const Matrix& m, const ColorX&) {
        for (const auto& [x, y] : { std::pair{ s.x0, s.y0 }, std::pair{ s.x1, s.y0 }, std::pair{ s.x0, s.y1 }, std::pair{ s.x1, s.y1 } })
        {
            double tx, ty;
            m.Apply(x, y, tx, ty);
            bx0 = std::min(bx0, tx), by0 = std::min(by0, ty), bx1 = std::max(bx1, tx), by1 = std::max(by1, ty);
        }
    });
    if (bx1 <= bx0 || by1 <= by0)
        return out;
    // Drawn at 4x and averaged down (anti-aliasing).
    constexpr int SS = 4;
    const int W = w * SS, H = h * SS;
    const double scale = std::min(W / (bx1 - bx0), H / (by1 - by0));
    Matrix fit;
    fit.a = fit.d = scale;
    fit.tx = (W - (bx1 - bx0) * scale) / 2 - bx0 * scale, fit.ty = (H - (by1 - by0) * scale) / 2 - by0 * scale;
    std::vector<uint8_t> canvas(size_t(W) * H * 4, 0);
    std::optional<Rgba> bitmapBody;  // the colour of a bitmap-filled body, drawn by filling the outline's inside
    Walk(*lib, e->second, fit, ColorX{}, &parts, 0, [&](const Shape& s, const Matrix& m, const ColorX& cx) {
        // Fills: for each style, every edge with it on one side (an edge with it on both cancels under even-odd).
        for (size_t f = 1; f < s.fills.size(); f++)
        {
            if (s.fills[f].bitmap)
            {
                bitmapBody = Transform(s.fills[f], cx);
                continue;
            }
            std::vector<std::array<double, 4>> segs;
            for (const Segment& g : s.segments)
                if ((g.fill0 == int(f)) != (g.fill1 == int(f)))
                {
                    std::array<double, 4> t;
                    m.Apply(g.x0, g.y0, t[0], t[1]);
                    m.Apply(g.x1, g.y1, t[2], t[3]);
                    segs.push_back(t);
                }
            FillPolygon(canvas, W, H, segs, Transform(s.fills[f], cx));
        }
        // Lines: each segment a quad of the line's width (at least a pixel).
        const double unit = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
        for (const Segment& g : s.segments)
        {
            if (g.line <= 0 || size_t(g.line) >= s.lines.size())
                continue;
            const auto& [width, col] = s.lines[size_t(g.line)];
            double x0, y0, x1, y1;
            m.Apply(g.x0, g.y0, x0, y0);
            m.Apply(g.x1, g.y1, x1, y1);
            const double len = std::hypot(x1 - x0, y1 - y0);
            if (len <= 0)
                continue;
            const double half = std::max(width * unit, double(SS)) / 2;
            const double nx = -(y1 - y0) / len * half, ny = (x1 - x0) / len * half;
            const double ex = (x1 - x0) / len * half, ey = (y1 - y0) / len * half;  // (round-ish ends: extended)
            const double ax = x0 - ex, ay = y0 - ey, bx = x1 + ex, by = y1 + ey;
            const std::vector<std::array<double, 4>> quad = { { ax + nx, ay + ny, bx + nx, by + ny }, { bx + nx, by + ny, bx - nx, by - ny },
                { bx - nx, by - ny, ax - nx, ay - ny }, { ax - nx, ay - ny, ax + nx, ay + ny } };
            FillPolygon(canvas, W, H, quad, Transform(col, cx));
        }
    });
    if (bitmapBody)
    {
        // Everything the outline encloses: the transparent pixels a flood fill from the picture's edges cannot reach.
        Rgba body = *bitmapBody;
        for (int k = 0; k < 3; k++)
            body.c[k] = std::min(body.c[k], 205.0);  // (as light as the other pictures' bodies)
        std::vector<uint8_t> outside(size_t(W) * H, 0);
        std::vector<int> stack;
        auto push = [&](int x, int y) {
            const size_t i = size_t(y) * W + x;
            if (!outside[i] && canvas[i * 4 + 3] < 128)
                outside[i] = 1, stack.push_back(int(i));
        };
        for (int x = 0; x < W; x++)
            push(x, 0), push(x, H - 1);
        for (int y = 0; y < H; y++)
            push(0, y), push(W - 1, y);
        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % W, y = i / W;
            if (x > 0)
                push(x - 1, y);
            if (x + 1 < W)
                push(x + 1, y);
            if (y > 0)
                push(x, y - 1);
            if (y + 1 < H)
                push(x, y + 1);
        }
        for (size_t i = 0; i < outside.size(); i++)
            if (!outside[i] && canvas[i * 4 + 3] < 128)
            {
                for (int k = 0; k < 3; k++)
                    canvas[i * 4 + k] = uint8_t(body.c[k]);
                canvas[i * 4 + 3] = 255;
            }
    }
    out.w = w, out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            double sum[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++)
                {
                    const uint8_t* p = &canvas[(size_t(y * SS + j) * W + x * SS + i) * 4];
                    const double a = p[3];
                    sum[0] += p[0] * a, sum[1] += p[1] * a, sum[2] += p[2] * a, sum[3] += a;
                }
            uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
            if (sum[3] > 0)
                for (int k = 0; k < 3; k++)
                    o[k] = uint8_t(sum[k] / sum[3]);
            o[3] = uint8_t(sum[3] / (SS * SS));
        }
    return out;
}
} // namespace host::swf_icons
