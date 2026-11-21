// GX2 on Vulkan: the state and resources behind the gx2 imports. Called from cafe/gx2.cpp when a Vulkan
// device is available; every function takes guest addresses and GX2 enum values.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::renderer
{
// GX2CompareFunction and three GX2StencilFunction (z-pass, z-fail, fail) for one face. Both enums match Vulkan's.
struct StencilFace
{
    uint32_t func = 7, zPass = 0, zFail = 0, fail = 0;
};
// GX2SetStencilMask for one face: compare mask, write mask, reference.
struct StencilMask
{
    uint8_t compare = 0xFF, write = 0xFF, ref = 0;
};

void Init(const std::string& shaderCacheDir);
bool Active();
// Log a GX2 feature the renderer does not implement, once per distinct message.
void Report(const std::string& message);
// Wait for the GPU to go idle before the process exits. Leaves the renderer locked.
void Shutdown();

// Context state (GX2ContextState): state changes go to the active context and are restored on switch.
void SetupContextState(uint32_t context);
void SetContextState(uint32_t context);

// Render targets, clears, viewport.
void SetColorBuffer(uint32_t colorBuffer, uint32_t target);
void SetDepthBuffer(uint32_t depthBuffer);
void ClearColor(uint32_t colorBuffer, float r, float g, float b, float a);
// GX2ClearFlags: 1 depth, 2 stencil.
void ClearDepthStencil(uint32_t depthBuffer, float depth, uint8_t stencil, uint32_t flags);
// GX2CopySurface: one level and slice of a surface into another's.
void CopySurface(uint32_t src, uint32_t srcLevel, uint32_t srcSlice, uint32_t dst, uint32_t dstLevel, uint32_t dstSlice);
// GX2ConvertDepthBufferToTextureSurface: copy depth into a texture surface's level and slice.
void ConvertDepthBuffer(uint32_t depthBuffer, uint32_t surface, uint32_t level, uint32_t slice);
void SetViewport(float x, float y, float w, float h, float zNear, float zFar);
void SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Shaders and inputs.
void InitFetchShader(uint32_t fetchShader, uint32_t attribCount, uint32_t attribs);
// Guest memory [src, src + bytes) was moved to dst (UE3's texture pool defragmenter, FGX2TexturePool::PlatformRelocate,
// with DMAECopyMem): GPU-written surfaces in it hold data guest memory never received, so they move with it.
void RelocateSurfaces(uint32_t dst, uint32_t src, uint32_t bytes);
void SetFetchShader(uint32_t fetchShader);
void SetVertexShader(uint32_t shader);
void SetPixelShader(uint32_t shader);
void SetUniforms(bool pixel, uint32_t offset, const uint32_t* guestWords, uint32_t count);  // big-endian words
void SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer);

// Textures and samplers.
void SetTexture(bool pixel, uint32_t texture, uint32_t unit);
void SetSampler(bool pixel, uint32_t sampler, uint32_t unit);
void InitSampler(uint32_t sampler, uint32_t clamp, uint32_t filter);
void InitSamplerClamping(uint32_t sampler, uint32_t x, uint32_t y, uint32_t z);
void InitSamplerXYFilter(uint32_t sampler, uint32_t mag, uint32_t min, uint32_t aniso);
void InitSamplerZMFilter(uint32_t sampler, uint32_t z, uint32_t mip);
void InitSamplerLOD(uint32_t sampler, float minLod, float maxLod, float bias);
void Invalidate(uint32_t mode, uint32_t addr, uint32_t size);

// Occlusion queries (GX2QueryBegin/End; `issue` numbers each begin of a query object). QueryResult is the samples of
// that issue once the GPU has counted them; ResolveQueries (GPU thread) submits pending queries and reads them all.
void BeginQuery(uint32_t addr, uint64_t issue);
void EndQuery(uint32_t addr);
void ResolveQueries();
// (waits up to waitMs for it to arrive)
bool QueryResult(uint32_t addr, uint64_t issue, uint64_t& samples, double waitMs = 0);

// Fixed-function state.
void SetBlendControl(uint32_t target, uint32_t colorSrc, uint32_t colorDst, uint32_t colorOp, bool separateAlpha,
    uint32_t alphaSrc, uint32_t alphaDst, uint32_t alphaOp);
void SetColorControl(uint32_t logicOp, uint32_t blendMask, bool multiWrite, bool colorWrite);
void SetTargetChannelMasks(const uint32_t masks[8]);
void SetPolygonControl(uint32_t frontFace, bool cullFront, bool cullBack, bool polygonOffset);
void SetPolygonOffset(float offset, float scale, float clamp);
void SetRasterizerClip(bool rasterizer, bool zClip, bool halfZ);
void SetBlendConstant(float r, float g, float b, float a);
void SetLineWidth(float width);
void SetDepthStencilControl(bool depthTest, bool depthWrite, uint32_t depthFunc, bool stencilTest, bool backfaceStencil,
    StencilFace front, StencilFace back);
void SetStencilMask(StencilMask front, StencilMask back);
void SetAlphaTest(bool enable, uint32_t func, float ref);

// Draws.
void Draw(uint32_t mode, uint32_t count, uint32_t firstVertex, uint32_t instances);
void DrawIndexed(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices, uint32_t firstVertex, uint32_t instances);

// Scan-out.
void CopyColorBufferToScanBuffer(uint32_t colorBuffer, uint32_t target);
void Swap(uint64_t frame);

// --gpu-stats: log draw, pass, upload and pipeline counts and the frame rate every 300 frames.
void EnableStats();
// --frame-csv: write one row per frame (its time and where it went) to `path`. Implies the stats counters.
void SetFrameCsv(const std::string& path);
// --capture-frame N: save every pass's render targets in frame N as PNGs to dir, and log its draws.
void SetCapture(const std::string& dir, uint64_t frame);
// --no-cull: ignore GX2's face culling (debugging).
void SetNoCull(bool noCull);

// Screenshots of the TV image (PNG) at the given frame numbers, and every `every` frames if non-zero.
// window: the presented image with the overlay (--screenshot-window) instead of the TV image.
// Any thread: save the next frame presented (to the --screenshot-dir).
void RequestScreenshot();
// F12: the next frame's TV image into src/notes/screenshots/ (the frame number is logged).
void RequestUserScreenshot();
void SetScreenshots(const std::string& dir, const std::vector<uint64_t>& frames, uint64_t every, bool window);
} // namespace gpu::renderer
