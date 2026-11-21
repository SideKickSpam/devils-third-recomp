#include "png.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>

namespace gpu
{
namespace
{
void Be32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    Be32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Be32(out, uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}
} // namespace

bool WritePng(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t stride)
{
    // Raw image data: each row is prefixed with filter type 0 (none).
    std::vector<uint8_t> raw;
    raw.reserve(size_t(width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; y++)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * stride, rgba + size_t(y) * stride + width * 4);
    }
    uLongf zsize = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zsize);
    if (compress2(z.data(), &zsize, raw.data(), uLong(raw.size()), 6) != Z_OK)
        return false;
    z.resize(zsize);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<uint8_t> ihdr;
    Be32(ihdr, width);
    Be32(ihdr, height);
    ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });  // 8-bit, RGBA, deflate, adaptive filtering, no interlace
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}
} // namespace gpu
