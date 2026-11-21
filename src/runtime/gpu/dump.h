// --gx2-dump DIR: decode GX2 objects the first time each is bound, and save shader programs to DIR.
// A development aid for the renderer and shader translator.
#pragma once

#include <cstdint>
#include <string>

namespace gpu::dump
{
void Enable(const std::string& dir);
bool Enabled();

void VertexShader(uint32_t addr);
void PixelShader(uint32_t addr);
void FetchShaderInit(uint32_t fs, uint32_t attribCount, uint32_t attribs);
void Texture(uint32_t addr, uint32_t unit);
// After GX2CalcSurfaceSizeAndAlignment: the computed layout (every call, not just the first per address).
void SurfaceLayout(uint32_t addr);
void Sampler(uint32_t addr, uint32_t unit);
void ColorBuffer(uint32_t addr, uint32_t target);
void AttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer);
void Uniforms(bool pixel, uint32_t offset, uint32_t count, uint32_t data);
void Draw(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices);

// Texture layout check: the game copies each cooked (pre-tiled) mip into texture memory and then invalidates it.
// The copy's size is the size the offline GX2 tools computed; when the texture is bound, compare it with our
// layout for each level.
void Copy(uint32_t dst, uint32_t size);     // memcpy / OSBlockMove from guest code
void Invalidate(uint32_t addr, uint32_t size);
} // namespace gpu::dump
