// GX2 surface layout through AMD's addrlib (decaf-emu's R600/R700 fork): sizes, alignments, mip chains and
// detiling. This is what GX2CalcSurfaceSizeAndAlignment computes on the console, and the layout the GPU
// (and the game's pre-tiled texture data) uses.
#pragma once

#include "gx2_types.h"

#include <cstdint>
#include <vector>

namespace gpu::surface
{
// GX2TileMode values (identical to addrlib's AddrTileMode).
enum TileMode : uint32_t
{
    TileDefault = 0,
    TileLinearAligned = 1,
    Tile1DThin1 = 2,
    Tile1DThick = 3,
    Tile2DThin1 = 4,
    TileLinearSpecial = 16,
};

// GX2SurfaceUse bits.
constexpr uint32_t UseTexture = 1, UseColorBuffer = 2, UseDepthBuffer = 4, UseScanBuffer = 8;

// GX2SurfaceDim.
constexpr uint32_t Dim1D = 0, Dim2D = 1, Dim3D = 2, DimCube = 3, Dim1DArray = 4, Dim2DArray = 5, Dim2DMsaa = 6,
                   Dim2DMsaaArray = 7;

// One mip level as addrlib lays it out. Pitch and height are in elements (pixels, or 4x4 blocks for BCn).
struct Level
{
    uint32_t tileMode = 0;  // may be demoted from the surface's (small mips fall back to 1D tiling)
    uint32_t width = 0, height = 0, depth = 0;  // element counts of the real image
    uint32_t pitch = 0, paddedHeight = 0, paddedDepth = 0;
    uint32_t bpp = 0;  // bits per element
    uint32_t baseAlign = 0;
    uint64_t size = 0;
};

bool IsCompressed(uint32_t format);
uint32_t BitsPerElement(uint32_t format);
// Pixels per element along x and y: 4 for BCn, else 1.
uint32_t BlockSize(uint32_t format);

Level ComputeLevel(const Surface& s, uint32_t level);

// GX2CalcSurfaceSizeAndAlignment on a guest GX2Surface: resolves the default tile mode, then fills imageSize,
// alignment, pitch, mipmapSize, mipLevelOffset[] and the swizzle's tile-mode switch level.
void CalcSurfaceSizeAndAlignment(uint32_t surfaceAddr);

// Guest address of a mip level (level 0 = image, levels >= 1 in the mipmaps buffer).
uint32_t LevelAddress(const Surface& s, uint32_t level);

// Copy one mip level (all slices) out of guest memory into a linear buffer of `width x height x depth`
// elements, rows `width` elements apart (tightly packed), slices one after another.
void Detile(const Surface& s, uint32_t level, std::vector<uint8_t>& out);
// The reverse: write a linear copy of a level (as Detile makes it) into guest memory in the surface's layout.
void Tile(const Surface& s, uint32_t level, const std::vector<uint8_t>& in);

// Check Detile against addrlib's per-element addresses for many formats, tile modes and sizes, using scratch
// guest memory. Logs each case and returns the number of failures (--selftest).
int SelfTest();
} // namespace gpu::surface
