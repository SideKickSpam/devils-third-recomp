#include "rpx_decompress.h"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace rpx
{
namespace
{
constexpr uint32_t SHF_RPL_ZLIB = 0x08000000;
constexpr uint32_t SHT_NOBITS = 8;
constexpr uint32_t ShdrSize = 40;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutBe32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

bool Inflate(const uint8_t* src, size_t srcSize, uint8_t* dst, uint32_t dstSize)
{
    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        return false;
    z.next_in = const_cast<Bytef*>(src);
    z.avail_in = uInt(srcSize);
    z.next_out = dst;
    z.avail_out = dstSize;
    const int r = inflate(&z, Z_FINISH);
    const bool ok = r == Z_STREAM_END && z.total_out == dstSize;
    inflateEnd(&z);
    return ok;
}
} // namespace

bool Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& error)
{
    const uint8_t* d = in.data();
    if (in.size() < 0x34 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 2 || Be16(d + 0x10) != 0xFE01)
    {
        error = "not a Wii U RPX file";
        return false;
    }
    const uint32_t shoff = Be32(d + 0x20);
    const uint16_t shentsize = Be16(d + 0x2E), shnum = Be16(d + 0x30);
    if (shentsize < ShdrSize || shoff + uint64_t(shnum) * shentsize > in.size())
    {
        error = "the RPX's section table is damaged";
        return false;
    }
    struct Sec
    {
        uint32_t index, offset, size, flags, type;
    };
    std::vector<Sec> secs;
    for (uint32_t i = 0; i < shnum; i++)
    {
        const uint8_t* s = d + shoff + i * shentsize;
        const Sec sec{ i, Be32(s + 16), Be32(s + 20), Be32(s + 8), Be32(s + 4) };
        if (sec.offset && sec.type != SHT_NOBITS)
        {
            if (uint64_t(sec.offset) + sec.size > in.size())
            {
                error = "section " + std::to_string(i) + " lies outside the RPX file";
                return false;
            }
            secs.push_back(sec);
        }
    }
    std::sort(secs.begin(), secs.end(), [](const Sec& a, const Sec& b) { return a.offset < b.offset; });

    // Layout: the ELF header, the section table, then each stored section in its original order, 64-byte aligned.
    out.assign(0x40, 0);
    memcpy(out.data(), d, 0x34);
    PutBe32(out.data() + 0x20, 0x40);
    out.resize(0x40 + size_t(shnum) * shentsize, 0);
    memcpy(out.data() + 0x40, d + shoff, size_t(shnum) * shentsize);
    for (const Sec& sec : secs)
    {
        out.resize((out.size() + 0x3F) & ~size_t(0x3F), 0);
        const uint32_t newOffset = uint32_t(out.size());
        uint32_t newSize = sec.size;
        if (sec.flags & SHF_RPL_ZLIB)
        {
            if (sec.size < 4)
            {
                error = "compressed section " + std::to_string(sec.index) + " is truncated";
                return false;
            }
            newSize = Be32(d + sec.offset);
            out.resize(size_t(newOffset) + newSize);
            if (!Inflate(d + sec.offset + 4, sec.size - 4, out.data() + newOffset, newSize))
            {
                error = "compressed section " + std::to_string(sec.index) + " is damaged";
                return false;
            }
        }
        else
            out.insert(out.end(), d + sec.offset, d + sec.offset + sec.size);
        uint8_t* s = out.data() + 0x40 + sec.index * shentsize;
        PutBe32(s + 8, sec.flags & ~SHF_RPL_ZLIB);
        PutBe32(s + 16, newOffset);
        PutBe32(s + 20, newSize);
    }
    // Clear the flag on sections with no stored data too (none in practice).
    for (uint32_t i = 0; i < shnum; i++)
    {
        uint8_t* s = out.data() + 0x40 + i * shentsize;
        PutBe32(s + 8, Be32(s + 8) & ~SHF_RPL_ZLIB);
    }
    return true;
}
} // namespace rpx
