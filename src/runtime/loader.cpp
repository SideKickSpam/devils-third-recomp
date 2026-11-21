#include "loader.h"

#include "guest.h"
#include "trace.h"

#include <fstream>
#include <vector>

namespace
{
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

constexpr uint32_t SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_NOBITS = 8, SHT_RPL_EXPORTS = 0x80000001,
                   SHT_RPL_FILEINFO = 0x80000004;
constexpr uint32_t SHF_ALLOC = 2, SHF_RPL_ZLIB = 0x08000000;
} // namespace

bool LoadRpx(const std::string& path, RpxInfo& info, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    if (f.size() < 0x34 || Be16(f.data() + 0x12) != 20)
    {
        error = path + " is not a PowerPC ELF";
        return false;
    }
    info.entry = Be32(f.data() + 0x18);
    const uint32_t shoff = Be32(f.data() + 0x20);
    const uint16_t shentsize = Be16(f.data() + 0x2E), shnum = Be16(f.data() + 0x30);
    auto sec = [&](uint32_t i) { return f.data() + shoff + i * shentsize; };

    for (uint32_t i = 0; i < shnum; i++)
    {
        const uint8_t* s = sec(i);
        const uint32_t type = Be32(s + 4), flags = Be32(s + 8), addr = Be32(s + 12), off = Be32(s + 16), size = Be32(s + 20);
        if (flags & SHF_RPL_ZLIB)
        {
            error = path + " has compressed sections; decompress it with wiiurpxtool -d";
            return false;
        }
        if ((flags & SHF_ALLOC) && (type == SHT_PROGBITS || type == SHT_NOBITS) && size)
        {
            if (type == SHT_PROGBITS)
                memcpy(guest::Ptr(addr), f.data() + off, size);
            else
                memset(guest::Ptr(addr), 0, size);
            if (addr >= guest::DataBase && addr + size > info.dataEnd)
                info.dataEnd = addr + size;
        }
        else if (type == SHT_RPL_FILEINFO && size >= 0x48)
        {
            const uint8_t* fi = f.data() + off;
            info.sdaBase = Be32(fi + 0x24);
            info.sda2Base = Be32(fi + 0x28);
            info.stackSize = Be32(fi + 0x2C);
            info.heapSize = Be32(fi + 0x38);
        }
        else if (type == SHT_RPL_EXPORTS && size >= 8)
        {
            // u32 count, u32 signature, then {u32 value, u32 name offset (relative to the section)}.
            const uint8_t* e = f.data() + off;
            const uint32_t count = Be32(e);
            for (uint32_t k = 0; k < count && 8 + k * 8 + 8 <= size; k++)
                info.exports[reinterpret_cast<const char*>(e + (Be32(e + 12 + k * 8) & 0x7FFFFFFF))] = Be32(e + 8 + k * 8);
        }
        else if (type == SHT_SYMTAB)
        {
            const uint8_t* str = f.data() + Be32(sec(Be32(s + 24)) + 16);
            for (uint32_t o = 0; o + 16 <= size; o += 16)
            {
                const uint8_t* sym = f.data() + off + o;
                const uint32_t value = Be32(sym + 4), symSize = Be32(sym + 8);
                if ((sym[12] & 0xF) == 2 && value >= guest::CodeBase && value < guest::DataBase)
                    trace::AddSymbol(value, symSize, reinterpret_cast<const char*>(str + Be32(sym)));
            }
        }
    }
    return true;
}
