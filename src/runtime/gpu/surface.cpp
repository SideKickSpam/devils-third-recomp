#include "surface.h"

#include "../guest.h"
#include "../log.h"

#include <addrlib/addrinterface.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace gpu::surface
{
namespace
{
std::mutex gMutex;
ADDR_HANDLE gAddr = nullptr;

void* AllocSysMem(const ADDR_ALLOCSYSMEM_INPUT* in) { return malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE FreeSysMem(const ADDR_FREESYSMEM_INPUT* in)
{
    free(in->pVirtAddr);
    return ADDR_OK;
}

// The Wii U's Latte GPU: an R7xx-family part (RV710 family ID) with 2 pipes, 4 banks, 256-byte pipe
// interleave, 2 KB rows, 256-byte bank swaps and 2 KB sample split (GB_TILING_CONFIG 0x44902).
ADDR_HANDLE Addr()
{
    if (gAddr)
        return gAddr;
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = CIASICIDGFXENGINE_R600;
    in.chipFamily = 0x51;
    in.chipRevision = 71;
    in.createFlags.fillSizeFields = 1;
    in.regValue.gbAddrConfig = 0x44902;
    in.callbacks.allocSysMem = AllocSysMem;
    in.callbacks.freeSysMem = FreeSysMem;
    if (AddrCreate(&in, &out) != ADDR_OK)
    {
        dtlog::Important("[gpu] AddrCreate failed\n");
        abort();
    }
    gAddr = out.hLib;
    return gAddr;
}

bool IsMacroTiled(uint32_t mode) { return mode >= Tile2DThin1 && mode != TileLinearSpecial && mode <= 15; }
bool IsLinear(uint32_t mode) { return mode == 0 || mode == TileLinearAligned || mode == TileLinearSpecial; }
uint32_t Thickness(uint32_t mode)
{
    switch (mode)
    {
    case 3: case 7: case 11: case 13: case 15: return 4;  // *_THICK
    case 17: return 8;                                   // 3D_TILED_XTHICK
    default: return 1;
    }
}

uint32_t SliceCount(const Surface& s, uint32_t level)
{
    const uint32_t depth = std::max(1u, s.depth);
    return s.dim == Dim3D ? std::max(1u, depth >> level) : depth;
}

ADDR_COMPUTE_SURFACE_INFO_OUTPUT SurfaceInfo(const Surface& s, uint32_t level, uint32_t tileMode)
{
    ADDR_COMPUTE_SURFACE_INFO_INPUT in{};
    ADDR_COMPUTE_SURFACE_INFO_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    // GX2's LINEAR_SPECIAL (16) is laid out like LINEAR_GENERAL; addrlib's enum reuses 16 for 2D_TILED_XTHICK.
    in.tileMode = tileMode == TileLinearSpecial ? ADDR_TM_LINEAR_GENERAL : AddrTileMode(tileMode);
    in.format = AddrFormat(s.format & 0x3F);
    in.bpp = BitsPerElement(s.format);
    in.numSamples = 1u << s.aa;
    in.numFrags = in.numSamples;
    in.width = std::max(1u, s.width >> level);
    in.height = s.dim == Dim1D || s.dim == Dim1DArray ? 1 : std::max(1u, s.height >> level);
    in.numSlices = SliceCount(s, level);
    in.mipLevel = level;
    in.flags.inputBaseMap = level == 0;
    in.flags.depth = (s.use & UseDepthBuffer) != 0;
    in.flags.color = (s.use & UseColorBuffer) != 0 && !in.flags.depth;
    in.flags.texture = (s.use & UseTexture) != 0;
    in.flags.display = (s.use & UseScanBuffer) != 0;
    in.flags.cube = s.dim == DimCube;
    in.flags.volume = s.dim == Dim3D;
    if (AddrComputeSurfaceInfo(Addr(), &in, &out) != ADDR_OK)
        dtlog::Important("[gpu] AddrComputeSurfaceInfo failed (format 0x%x, %ux%u, tile mode %u)\n", s.format,
            in.width, in.height, tileMode);
    return out;
}

uint32_t ResolveTileMode(const Surface& s)
{
    if (s.tileMode != TileDefault)
        return s.tileMode;
    // GX2_TILE_MODE_DEFAULT: linear for unsampled 1D surfaces, otherwise 2D tiling.
    return s.dim == Dim1D && s.aa == 0 ? TileLinearAligned : Tile2DThin1;
}

// Byte offset of each element within a micro tile (8 x 8 x thickness), indexed [z][y][x], read from addrlib's
// 1D-tiled layout, where micro tiles are stored whole and in order.
struct MicroTile
{
    std::vector<uint32_t> offset;  // [(z * 8 + y) * 8 + x]
};

const MicroTile& MicroTileLayout(uint32_t bpp, uint32_t thickness, bool depth)
{
    static std::map<uint32_t, MicroTile> cache;
    const uint32_t key = bpp | thickness << 16 | uint32_t(depth) << 24;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    MicroTile& t = cache[key];
    t.offset.resize(64 * thickness);
    for (uint32_t z = 0; z < thickness; z++)
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++)
            {
                ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
                ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};
                in.size = sizeof(in);
                out.size = sizeof(out);
                in.x = x, in.y = y, in.slice = z;
                in.bpp = bpp;
                in.pitch = 8, in.height = 8, in.numSlices = thickness;
                in.numSamples = 1, in.numFrags = 1;
                in.tileMode = thickness > 1 ? ADDR_TM_1D_TILED_THICK : ADDR_TM_1D_TILED_THIN1;
                in.isDepth = depth;
                AddrComputeSurfaceAddrFromCoord(Addr(), &in, &out);
                t.offset[(z * 8 + y) * 8 + x] = uint32_t(out.addr);
            }
    return t;
}
} // namespace

bool IsCompressed(uint32_t format)
{
    const uint32_t f = format & 0x3F;
    return f >= 0x31 && f <= 0x35;
}

uint32_t BlockSize(uint32_t format) { return IsCompressed(format) ? 4 : 1; }

uint32_t BitsPerElement(uint32_t format)
{
    switch (format & 0x3F)
    {
    case 0x01: case 0x02: case 0x03: return 8;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: return 16;
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: return 32;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: return 64;
    case 0x22: case 0x23: return 128;
    case 0x31: case 0x34: return 64;              // BC1, BC4: 8 bytes per 4x4 block
    case 0x32: case 0x33: case 0x35: return 128;  // BC2, BC3, BC5: 16 bytes per block
    default: return 32;
    }
}

Level ComputeLevel(const Surface& s, uint32_t level)
{
    std::lock_guard lock(gMutex);
    const auto out = SurfaceInfo(s, level, ResolveTileMode(s));
    const uint32_t block = BlockSize(s.format);
    Level l;
    l.tileMode = ResolveTileMode(s) == TileLinearSpecial ? uint32_t(TileLinearSpecial) : uint32_t(out.tileMode);
    l.width = (std::max(1u, s.width >> level) + block - 1) / block;
    l.height = s.dim == Dim1D || s.dim == Dim1DArray ? 1 : (std::max(1u, s.height >> level) + block - 1) / block;
    l.depth = SliceCount(s, level);
    l.pitch = out.pitch;
    l.paddedHeight = out.height;
    l.paddedDepth = out.depth;
    l.bpp = out.bpp ? out.bpp : BitsPerElement(s.format);
    l.baseAlign = out.baseAlign;
    l.size = out.surfSize;
    return l;
}

void CalcSurfaceSizeAndAlignment(uint32_t addr)
{
    // GX2Surface offsets.
    constexpr uint32_t ImageSize = 0x20, MipmapSize = 0x28, TileModeOff = 0x30, Swizzle = 0x34, Alignment = 0x38,
                       Pitch = 0x3C, MipOffsets = 0x40;
    Surface s = ReadSurface(addr);
    s.tileMode = ResolveTileMode(s);
    const uint32_t levels = std::clamp(s.mipLevels, 1u, 14u);

    const Level base = ComputeLevel(s, 0);
    guest::Write32(addr + TileModeOff, base.tileMode);
    s.tileMode = base.tileMode;
    guest::Write32(addr + ImageSize, uint32_t(base.size));
    guest::Write32(addr + Alignment, base.baseAlign);
    guest::Write32(addr + Pitch, base.pitch);

    // Mip chain: level 1 starts the mipmaps buffer (mipLevelOffset[0] is its offset from the image when the two
    // are allocated together), and mipLevelOffset[n] (n >= 1) is level n+1's offset within the mipmaps buffer.
    // Bits 16-23 of the swizzle record the first level that drops from macro (2D) to micro (1D) tiling.
    uint32_t swizzle = s.swizzle & 0xFF00FFFF;
    uint32_t offsets[13] = {};
    uint64_t mipSize = 0;
    bool switched = !IsMacroTiled(base.tileMode);
    for (uint32_t level = 1; level < levels; level++)
    {
        const Level l = ComputeLevel(s, level);
        if (!switched && !IsMacroTiled(l.tileMode))
        {
            swizzle |= level << 16;
            switched = true;
        }
        mipSize = (mipSize + l.baseAlign - 1) / l.baseAlign * l.baseAlign;
        if (level == 1)
            offsets[0] = uint32_t((base.size + l.baseAlign - 1) / l.baseAlign * l.baseAlign);
        else
            offsets[level - 1] = uint32_t(mipSize);
        mipSize += l.size;
    }
    if (!switched)
        swizzle |= std::min(levels, 13u) << 16;
    guest::Write32(addr + Swizzle, swizzle);
    guest::Write32(addr + MipmapSize, uint32_t(mipSize));
    for (uint32_t i = 0; i < 13; i++)
        guest::Write32(addr + MipOffsets + i * 4, offsets[i]);
}

uint32_t LevelAddress(const Surface& s, uint32_t level)
{
    if (level == 0)
        return s.image;
    const uint32_t mips = s.mipmaps ? s.mipmaps : s.image + s.mipLevelOffset[0];
    return level == 1 ? mips : mips + s.mipLevelOffset[level - 1];
}

namespace
{
// Moves a level between guest memory (tiled) and a tightly packed linear copy, in either direction.
void Transfer(const Surface& s, uint32_t level, uint8_t* linear, bool toGuest)
{
    const Level l = ComputeLevel(s, level);
    const uint32_t bpe = l.bpp / 8;
    uint8_t* src = guest::Ptr(LevelAddress(s, level));
    auto move = [toGuest](uint8_t* guestBytes, uint8_t* linearBytes, size_t n) {
        if (toGuest)
            memcpy(guestBytes, linearBytes, n);
        else
            memcpy(linearBytes, guestBytes, n);
    };
    const size_t rowBytes = size_t(l.width) * bpe;

    if (IsLinear(l.tileMode))
    {
        for (uint32_t z = 0; z < l.depth; z++)
            for (uint32_t y = 0; y < l.height; y++)
                move(src + (size_t(z) * l.paddedHeight + y) * l.pitch * bpe, &linear[(size_t(z) * l.height + y) * rowBytes],
                    rowBytes);
        return;
    }

    std::lock_guard lock(gMutex);
    const bool depth = (s.use & UseDepthBuffer) != 0;
    const uint32_t thickness = Thickness(l.tileMode);
    const MicroTile& tile = MicroTileLayout(l.bpp, thickness, depth);
    const uint32_t tileBytes = 64 * thickness * bpe;
    // Addresses are contiguous within each pipe-interleave group (256 bytes) of a micro tile; between groups
    // the pipe and bank bits are inserted, so ask addrlib once per group.
    const uint32_t group = std::min(256u, tileBytes);
    // Element of the micro tile at each byte offset that starts a group.
    std::vector<uint32_t> groupLead(tileBytes / group);
    for (uint32_t i = 0; i < tile.offset.size(); i++)
        if (tile.offset[i] % group == 0)
            groupLead[tile.offset[i] / group] = i;

    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT res{};
    in.size = sizeof(in);
    res.size = sizeof(res);
    in.bpp = l.bpp;
    in.pitch = l.pitch;
    in.height = l.paddedHeight;
    in.numSlices = std::max(l.paddedDepth, l.depth);
    in.numSamples = 1, in.numFrags = 1;
    in.tileMode = AddrTileMode(l.tileMode);
    in.isDepth = depth;
    in.pipeSwizzle = (s.swizzle >> 8) & 1;
    in.bankSwizzle = (s.swizzle >> 9) & 3;
    std::vector<uint32_t> groupAddr(groupLead.size());
    for (uint32_t z0 = 0; z0 < l.depth; z0 += thickness)
        for (uint32_t ty = 0; ty < l.height; ty += 8)
            for (uint32_t tx = 0; tx < l.width; tx += 8)
            {
                for (size_t g = 0; g < groupLead.size(); g++)
                {
                    const uint32_t e = groupLead[g];
                    in.x = tx + e % 8, in.y = ty + e / 8 % 8, in.slice = z0 + e / 64;
                    AddrComputeSurfaceAddrFromCoord(Addr(), &in, &res);
                    groupAddr[g] = uint32_t(res.addr);
                }
                for (uint32_t z = 0; z < thickness && z0 + z < l.depth; z++)
                    for (uint32_t y = 0; y < 8 && ty + y < l.height; y++)
                        for (uint32_t x = 0; x < 8 && tx + x < l.width; x++)
                        {
                            const uint32_t off = tile.offset[(z * 8 + y) * 8 + x];
                            move(src + groupAddr[off / group] + off % group,
                                &linear[((size_t(z0 + z) * l.height + ty + y) * l.width + tx + x) * bpe], bpe);
                        }
            }
}
} // namespace

void Detile(const Surface& s, uint32_t level, std::vector<uint8_t>& out)
{
    const Level l = ComputeLevel(s, level);
    out.resize(size_t(l.width) * l.height * l.depth * (l.bpp / 8));
    Transfer(s, level, out.data(), false);
}

void Tile(const Surface& s, uint32_t level, const std::vector<uint8_t>& in)
{
    const Level l = ComputeLevel(s, level);
    if (in.size() < size_t(l.width) * l.height * l.depth * (l.bpp / 8))
        return;
    Transfer(s, level, const_cast<uint8_t*>(in.data()), true);
}

int SelfTest()
{
    constexpr uint32_t Scratch = 0xE0000000;  // guest memory nothing uses before the game boots
    struct Case
    {
        uint32_t format, tileMode, width, height, depth, dim, swizzle;
    };
    std::vector<Case> cases;
    for (uint32_t format : { 0x01u, 0x07u, 0x1Au, 0x1Fu, 0x23u, 0x31u, 0x33u })
        for (uint32_t tile : { 1u, 2u, 4u })
            for (auto [w, h] : { std::pair{ 64u, 64u }, { 200u, 75u }, { 1280u, 720u }, { 16u, 8u } })
                for (uint32_t swizzle : { 0u, 0x300u, 0x500u })
                    cases.push_back({ format, tile, w, h, 1, Dim2D, swizzle });
    cases.push_back({ 0x1A, 3, 64, 64, 8, Dim3D, 0 });
    cases.push_back({ 0x1A, 7, 128, 64, 8, Dim3D, 0x200 });
    cases.push_back({ 0x31, 4, 256, 256, 6, DimCube, 0x100 });

    int failures = 0;
    for (const Case& c : cases)
    {
        Surface s{};
        s.dim = c.dim, s.width = c.width, s.height = c.height, s.depth = c.depth, s.mipLevels = 1;
        s.format = c.format, s.use = UseTexture, s.tileMode = c.tileMode, s.swizzle = c.swizzle;
        s.image = Scratch;
        const Level l = ComputeLevel(s, 0);
        const uint32_t bpe = l.bpp / 8;
        memset(guest::Ptr(Scratch), 0, size_t(l.size));
        // Reference: write each element's index at the address addrlib gives for it.
        std::vector<uint32_t> expected;
        {
            std::lock_guard lock(gMutex);
            ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
            ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};
            in.size = sizeof(in);
            out.size = sizeof(out);
            in.bpp = l.bpp, in.pitch = l.pitch, in.height = l.paddedHeight;
            in.numSlices = std::max(l.paddedDepth, l.depth), in.numSamples = 1, in.numFrags = 1;
            in.tileMode = AddrTileMode(l.tileMode);
            in.pipeSwizzle = (s.swizzle >> 8) & 1, in.bankSwizzle = (s.swizzle >> 9) & 3;
            for (uint32_t z = 0; z < l.depth; z++)
                for (uint32_t y = 0; y < l.height; y++)
                    for (uint32_t x = 0; x < l.width; x++)
                    {
                        in.x = x, in.y = y, in.slice = z;
                        if (IsLinear(l.tileMode))
                            out.addr = ((uint64_t(z) * l.paddedHeight + y) * l.pitch + x) * bpe;
                        else
                            AddrComputeSurfaceAddrFromCoord(Addr(), &in, &out);
                        const uint32_t id = uint32_t(expected.size()) + 1;
                        if (out.addr + bpe > l.size)
                        {
                            failures++;
                            dtlog::Important("[selftest] address out of range\n");
                            break;
                        }
                        memcpy(guest::Ptr(Scratch + uint32_t(out.addr)), &id, std::min(bpe, 4u));
                        expected.push_back(id);
                    }
        }
        std::vector<uint8_t> linear;
        Detile(s, 0, linear);
        uint32_t bad = 0;
        for (size_t i = 0; i < expected.size(); i++)
        {
            uint32_t got = 0;
            memcpy(&got, &linear[i * bpe], std::min(bpe, 4u));
            const uint32_t want = bpe >= 4 ? expected[i] : expected[i] & ((1u << (8 * bpe)) - 1);
            bad += got != want;
        }
        // Tile must put the linear copy back exactly where it came from.
        {
            std::vector<uint8_t> before(guest::Ptr(Scratch), guest::Ptr(Scratch) + l.size);
            memset(guest::Ptr(Scratch), 0, size_t(l.size));
            Tile(s, 0, linear);
            std::vector<uint8_t> back;
            Detile(s, 0, back);
            if (back != linear)
            {
                bad++;
                dtlog::Important("[selftest] Tile does not round-trip\n");
            }
            memcpy(guest::Ptr(Scratch), before.data(), before.size());
        }
        // Elements narrower than 4 bytes cannot hold unique ids, so a collision check would need them; the
        // wider formats in the list cover the address mapping itself.
        dtlog::Important("[selftest] format 0x%02x tile %u %ux%ux%u dim %u swizzle 0x%03x -> %s, pitch %u, size 0x%llx, "
                         "align 0x%x: %s\n",
            c.format, c.tileMode, c.width, c.height, c.depth, c.dim, c.swizzle,
            l.tileMode == c.tileMode ? "same mode" : ("mode " + std::to_string(l.tileMode)).c_str(), l.pitch,
            (unsigned long long)l.size, l.baseAlign, bad ? "MISMATCH" : "ok");
        failures += bad != 0;
    }
    dtlog::Important("[selftest] surface: %zu cases, %d failures\n", cases.size(), failures);
    return failures;
}
} // namespace gpu::surface
