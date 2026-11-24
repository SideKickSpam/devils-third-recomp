// gx2: every function logs its arguments (through the import dispatcher) and returns a sensible value. With a
// Vulkan device, state and draws go to gpu::renderer; without one (--no-window) nothing is drawn.
// GX2SwapScanBuffers counts frames, paces to the swap interval and keeps the host window responsive.
#include "../gpu/device.h"
#include "../gpu/dump.h"
#include "../gpu/queue.h"
#include "../gpu/renderer.h"
#include "../gpu/surface.h"
#include "../host/window.h"
#include "../imports.h"
#include "../log.h"
#include "../settings.h"
#include "../trace.h"
#include "coreinit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>

namespace cafe::gx2
{
namespace
{
using namespace std::chrono;

// ---- frame state ----
std::atomic<uint32_t> gSwapInterval{ 1 };
uint64_t gFrames = 0;
uint64_t gFrameLimit = 0;
std::string gShaderCacheDir = "shadercache";
steady_clock::time_point gLastSwap;
steady_clock::time_point gNextSwap;  // when the next swap is due (GX2SwapScanBuffers pacing)
steady_clock::time_point gFirstSwap;

std::string Hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

// Forward a call to the renderer when it is running: queued for the GPU thread (gpu/queue.h). Arguments are captured
// by value; anything the renderer would read from guest memory later must be copied before (see SetUniforms).
std::atomic<uint64_t> gQueryBegins{ 0 };  // GX2QueryBegin calls (logged every 300 frames)

#define RENDER(call)                                                   \
    do                                                                 \
    {                                                                  \
        if (gpu::renderer::Active())                                   \
            gpu::queue::Run([=] { gpu::renderer::call; });             \
    } while (0)

// GX2 copies a structure's registers into its command buffer when the call is made, so the game may reuse or free it
// as soon as the call returns (a GX2Surface on the stack for a copy, say). Commands on the GPU thread run later, so
// they get a copy, made now, in a ring in system memory. With at most one frame queued (GX2SwapScanBuffers), the
// ring holds many frames' worth; the renderer copies bound structures again into its own state (StateCopy).
constexpr uint32_t CopyRingSize = 64 << 20;
uint32_t gCopyRing = 0;
std::atomic<uint64_t> gCopyRingPos{ 0 };
uint32_t CallCopy(uint32_t addr, uint32_t size)
{
    if (!addr || !gpu::queue::Async())
        return addr;
    static std::once_flag once;
    std::call_once(once, [] { gCopyRing = guest::SystemAlloc(CopyRingSize, 64); });
    const uint32_t aligned = (size + 63) & ~63u;
    for (;;)
    {
        const uint64_t pos = gCopyRingPos.fetch_add(aligned);
        const uint32_t off = uint32_t(pos % CopyRingSize);
        if (off + aligned > CopyRingSize)
            continue;  // would straddle the end: take the next slot
        memcpy(guest::Ptr(gCopyRing + off), guest::Ptr(addr), size);
        return gCopyRing + off;
    }
}
constexpr uint32_t SurfaceSize = 0x74, TextureSize = 0x9C, ColorBufferSize = 0x9C, DepthBufferSize = 0xAC,
                   VertexShaderSize = 0x134, PixelShaderSize = 0xE8;

// GX2Surface (0x74 bytes): dim, width, height, depth, mipLevels, format, aa, use, imageSize, image, mipmapSize,
// mipmaps, tileMode, swizzle, alignment, pitch, mipLevelOffset[13]. Layouts come from addrlib (gpu/surface.*).
enum : uint32_t
{
    SWidth = 0x04, SHeight = 0x08, SImage = 0x24,
};
using gpu::surface::BitsPerElement;

// ---- initialisation and display ----
void GX2Init(uint32_t attribs)
{
    (void)attribs;
    if (host::OpenWindow("Devil's Third (dt-port)"))
        if (gpu::GetDevice().Init(host::Window()))
        {
            gpu::renderer::Init(gShaderCacheDir);
            gpu::queue::Start();
        }
}

void GX2SetupContextStateEx(uint32_t state, bool profiling)
{
    (void)profiling;
    RENDER(SetupContextState(state));
}
void GX2SetContextState(uint32_t state) { RENDER(SetContextState(state)); }

// GX2TVRenderMode: 1 480p 4:3, 2 480p 16:9, 3 720p, 4 1080i (unused), 5 1080p. Buffering: 1, 2 or 4 buffers.
void TvSize(uint32_t mode, uint32_t& w, uint32_t& h)
{
    switch (mode)
    {
    case 1: w = 640, h = 480; break;
    case 2: w = 854, h = 480; break;
    case 5: w = 1920, h = 1080; break;
    default: w = 1280, h = 720; break;
    }
}

void ScanBufferSize(uint32_t w, uint32_t h, uint32_t format, uint32_t buffering, uint32_t outSize, uint32_t outScale)
{
    const uint32_t buffers = std::max(1u, buffering);
    if (outSize)
        guest::Write32(outSize, guest::AlignUp(w * h * BitsPerElement(format) / 8, 0x1000) * buffers);
    if (outScale)
        guest::Write32(outScale, 0);
}

void GX2CalcTVSize(uint32_t mode, uint32_t format, uint32_t buffering, uint32_t outSize, uint32_t outScale)
{
    uint32_t w, h;
    TvSize(mode, w, h);
    ScanBufferSize(w, h, format, buffering, outSize, outScale);
}
void GX2CalcDRCSize(uint32_t mode, uint32_t format, uint32_t buffering, uint32_t outSize, uint32_t outScale)
{
    (void)mode;
    ScanBufferSize(854, 480, format, buffering, outSize, outScale);
}
void GX2SetTVBuffer(uint32_t buf, uint32_t size, uint32_t mode, uint32_t format, uint32_t buffering)
{
    (void)buf, (void)size, (void)mode, (void)format, (void)buffering;
}
void GX2SetDRCBuffer(uint32_t buf, uint32_t size, uint32_t mode, uint32_t format, uint32_t buffering)
{
    (void)buf, (void)size, (void)mode, (void)format, (void)buffering;
    RENDER(Report("the GamePad (DRC) screen is not shown"));
}
void GX2SetTVScale(uint32_t x, uint32_t y) { (void)x, (void)y; }
void GX2SetDRCScale(uint32_t x, uint32_t y) { (void)x, (void)y; }
void GX2SetTVEnable(bool enable) { (void)enable; }
void GX2SetDRCEnable(bool enable) { (void)enable; }
void GX2GetTVGamma(uint32_t out) { guest::Write32(out, std::bit_cast<uint32_t>(1.0f)); }
void GX2GetDRCGamma(uint32_t out) { guest::Write32(out, std::bit_cast<uint32_t>(1.0f)); }
void GX2SetTVGamma(float gamma) { (void)gamma; }
void GX2SetDRCGamma(float gamma) { (void)gamma; }
void GX2SetSwapInterval(uint32_t interval)
{
    if (interval != gSwapInterval)
    {
        const PPCContext* ctx = guest::CurrentContext();
        static const auto since = steady_clock::now();
        dtlog::Important("[dt-port] swap interval %u at frame %llu, %.2f s (from %s)\n", interval, (unsigned long long)gFrames,
            std::chrono::duration<double>(steady_clock::now() - since).count(), ctx ? trace::Describe(ctx->lr).c_str() : "?");
    }
    gSwapInterval = interval;
}
void GX2CopyColorBufferToScanBuffer(uint32_t colorBuffer, uint32_t target)
{
    const uint32_t cb = CallCopy(colorBuffer, ColorBufferSize);
    RENDER(CopyColorBufferToScanBuffer(cb, target));
}

// The swap grid's period: one frame per display.fps_cap (default 50 fps, the game's own UE3 cap), so presents are
// evenly spaced and a VRR display shows every frame for the same time. Movies (swap interval 2 or more) keep the
// Wii U's grid of that many 59.94 Hz refreshes. Zero: no pacing.
nanoseconds SwapPeriod()
{
    static uint64_t generation = ~0ull;
    static int cap = 50;
    if (settings::Generation() != generation)
    {
        generation = settings::Generation();
        int c = settings::GetInt("display.fps_cap", 50);
        if (c != 0 && (c < 30 || c > 1000))
            c = 50;  // as the frame limiter patch (game_patches.cpp); 0: unlimited, no pacing
        static bool logged = false;
        if (c != cap || !logged)
            dtlog::Important("[dt-port] frame pacing: %s fps (display.fps_cap)\n", c ? std::to_string(c).c_str() : "unlimited");
        cap = c, logged = true;
    }
    if (gSwapInterval >= 2)
        return nanoseconds(16683350ll * gSwapInterval);
    if (gSwapInterval == 0 || cap <= 0)
        return nanoseconds(0);
    return nanoseconds(1000000000ll / cap);
}

// Pace on a fixed schedule of SwapPeriod() (sleeping a period from the previous swap would add each frame's
// oversleep). A late frame swaps at once and restarts the schedule, so no frame is ever shorter than the period.
// Sleep until shortly before the slot and spin the rest: even with 1 ms timers a sleep can overshoot. Runs where the
// frame is presented: on the GPU thread when it is on.
void PaceSwap()
{
    static uint64_t paced = 0;
    const auto now = steady_clock::now();
    const nanoseconds period = SwapPeriod();
    if (period.count() && paced++ > 0)
    {
        gNextSwap += period;
        if (gNextSwap <= now)
            gNextSwap = now;
        else
        {
            if (gNextSwap - now > milliseconds(2))
                std::this_thread::sleep_until(gNextSwap - milliseconds(2));
            while (steady_clock::now() < gNextSwap)
                std::this_thread::yield();
        }
    }
    else
        gNextSwap = now;
}

void GX2SwapScanBuffers()
{
    const auto now = steady_clock::now();
    if (gFrames == 0)
        gFirstSwap = now;
    gFrames++;
    trace::NoteFrame();
    if (gFrames % 300 == 0 && gQueryBegins.load(std::memory_order_relaxed))
        dtlog::Important("[gx2] %.1f occlusion queries a frame\n", gQueryBegins.exchange(0) / 300.0);
    gLastSwap = now;
    if (gFrames == 1 || gFrames % 60 == 0)
        dtlog::Important("[dt-port] frame %llu (%.1f s since first swap)\n", (unsigned long long)gFrames,
            duration<double>(gLastSwap - gFirstSwap).count());
    if (gpu::renderer::Active())
    {
        const uint64_t frame = gFrames;
        gpu::queue::Run([frame] {
            PaceSwap();
            gpu::renderer::Swap(frame);
        });
        // The game may run one frame ahead of the GPU thread, no more (a deeper queue only adds latency).
        static uint64_t previousSwap = 0;
        const uint64_t thisSwap = gpu::queue::Fence();
        gpu::queue::WaitRetired(previousSwap);
        previousSwap = thisSwap;
        if (gFrameLimit && gFrames >= gFrameLimit)
            gpu::queue::WaitRetired(thisSwap);  // the last frame's screenshots and stats
    }
    else
    {
        PaceSwap();
        gpu::queue::Fence();
        if (gpu::GetDevice().Ready())
            gpu::GetDevice().Present(nullptr);
    }
    if (gFrameLimit && gFrames >= gFrameLimit)
        trace::Stop(0, "reached frame %llu (--frames limit)", (unsigned long long)gFrames);
}

// ---- command submission and synchronisation ----
// Timestamps follow the GPU thread: work is retired once it has executed the commands before the marker.
void GX2Flush() { gpu::queue::Fence(); }
bool GX2DrawDone()
{
    gpu::queue::WaitRetired(gpu::queue::Fence());
    return true;
}
uint64_t GX2GetLastSubmittedTimeStamp() { return gpu::queue::Submitted(); }
uint64_t GX2GetRetiredTimeStamp() { return gpu::queue::Retired(); }
bool GX2WaitTimeStamp(uint64_t ts)
{
    gpu::queue::WaitRetired(ts);
    return true;
}
void GX2Invalidate(uint32_t mode, uint32_t ptr, uint32_t size)
{
    gpu::dump::Invalidate(ptr, size);
    RENDER(Invalidate(mode, ptr, size));
}

// GX2SetSemaphore(uint64_t* sem, action): action 0 = wait (GPU blocks until > 0, then decrements), 1 = signal.
void GX2SetSemaphore(uint32_t sem, uint32_t action)
{
    // A GPU operation: it happens when the GPU thread reaches it.
    auto op = [sem, action] {
        const uint64_t v = guest::Read64(sem);
        guest::Write64(sem, action == 1 ? v + 1 : (v ? v - 1 : 0));
    };
    if (gpu::renderer::Active())
        gpu::queue::Run(op);
    else
        op();
}

// ---- surfaces, buffers and textures ----
void GX2CalcSurfaceSizeAndAlignment(uint32_t surface)
{
    gpu::surface::CalcSurfaceSizeAndAlignment(surface);
    gpu::dump::SurfaceLayout(surface);
}

void GX2CalcDepthBufferHiZInfo(uint32_t depthBuffer, uint32_t outSize, uint32_t outAlign)
{
    const uint32_t w = guest::Read32(depthBuffer + SWidth), h = guest::Read32(depthBuffer + SHeight);
    if (outSize)
        guest::Write32(outSize, guest::AlignUp((guest::AlignUp(w, 64) / 8) * (guest::AlignUp(h, 64) / 8), 0x1000));
    if (outAlign)
        guest::Write32(outAlign, 0x800);
}

void GX2InitColorBufferRegs(uint32_t buffer) { (void)buffer; }
void GX2InitDepthBufferRegs(uint32_t buffer) { (void)buffer; }
void GX2InitDepthBufferHiZEnable(uint32_t buffer, bool enable) { (void)buffer, (void)enable; }
void GX2InitTextureRegs(uint32_t texture) { (void)texture; }
uint32_t GX2GetSurfaceFormatBits(uint32_t format) { return BitsPerElement(format); }

// GX2AllocateTilingApertureEx gives the CPU a linear view of a tiled surface. Headless surfaces are not
// re-tiled, so the "aperture" is the surface memory itself.
uint32_t gNextAperture = 1;
void GX2AllocateTilingApertureEx(uint32_t surface, uint32_t level, uint32_t slice, uint32_t endian, uint32_t outHandle,
    uint32_t outAddress)
{
    (void)level, (void)slice, (void)endian;
    if (outHandle)
        guest::Write32(outHandle, gNextAperture++);
    if (outAddress)
        guest::Write32(outAddress, guest::Read32(surface + SImage));
}
void GX2FreeTilingAperture(uint32_t handle) { (void)handle; }

void GX2CopySurface(uint32_t src, uint32_t srcLevel, uint32_t srcSlice, uint32_t dst, uint32_t dstLevel, uint32_t dstSlice)
{
    const uint32_t s = CallCopy(src, SurfaceSize), d = CallCopy(dst, SurfaceSize);
    RENDER(CopySurface(s, srcLevel, srcSlice, d, dstLevel, dstSlice));
    // The game loads texture levels into reused staging memory and copies them into the texture with this call: wait
    // until the GPU thread has read the source, so the next load into that memory cannot reach this copy.
    if (gpu::renderer::Active())
        gpu::queue::WaitRetired(gpu::queue::Fence());
}
void GX2ConvertDepthBufferToTextureSurface(uint32_t depth, uint32_t dst, uint32_t level, uint32_t slice)
{
    const uint32_t db = CallCopy(depth, DepthBufferSize), d = CallCopy(dst, SurfaceSize);
    RENDER(ConvertDepthBuffer(db, d, level, slice));
}

// ---- shaders ----
// Fetch shaders: a small program (one vertex fetch per attribute plus header/footer). Generous size so the
// game's allocation always fits whatever we might write later.
uint32_t GX2CalcFetchShaderSizeEx(uint32_t attribs, uint32_t type, uint32_t tess)
{
    (void)type, (void)tess;
    return guest::AlignUp(0x20 + attribs * 0x20, 0x100);
}

// GX2FetchShader: type, regs (sq_pgm_resources_fs), size, program, attribCount, numDivisors, divisors[2].
void GX2InitFetchShaderEx(uint32_t fs, uint32_t program, uint32_t attribCount, uint32_t attribs, uint32_t type, uint32_t tess)
{
    (void)tess;
    gpu::dump::FetchShaderInit(fs, attribCount, attribs);
    // The attribute list is copied now and the definition queued in order with the draws, so draws already queued keep
    // the previous one. (It used to drain the GPU queue: UE3 creates vertex declarations for every skinned mesh spawned
    // at a level's start, each waiting for the frames in flight, 20-30 ms per frame there.)
    if (gpu::renderer::Active() && attribCount)
    {
        const uint32_t list = CallCopy(attribs, attribCount * 0x20);
        RENDER(InitFetchShader(fs, attribCount, list));
    }
    else if (gpu::renderer::Active())
        RENDER(InitFetchShader(fs, 0, 0));
    guest::Write32(fs + 0x00, type);
    guest::Write32(fs + 0x08, GX2CalcFetchShaderSizeEx(attribCount, type, tess));
    guest::Write32(fs + 0x0C, program);
    guest::Write32(fs + 0x10, attribCount);
}
void GX2SetFetchShader(uint32_t fs) { RENDER(SetFetchShader(fs)); }
void GX2SetVertexShader(uint32_t shader)
{
    gpu::dump::VertexShader(shader);
    const uint32_t vs = CallCopy(shader, VertexShaderSize);
    RENDER(SetVertexShader(vs));
}
void GX2SetPixelShader(uint32_t shader)
{
    gpu::dump::PixelShader(shader);
    const uint32_t ps = CallCopy(shader, PixelShaderSize);
    RENDER(SetPixelShader(ps));
}
void GX2SetShaderModeEx(uint32_t mode, uint32_t vsGprs, uint32_t vsStack, uint32_t gsGprs, uint32_t gsStack, uint32_t psGprs,
    uint32_t psStack)
{
    (void)vsGprs, (void)vsStack, (void)gsGprs, (void)gsStack, (void)psGprs, (void)psStack;
    // GX2ShaderMode: 0 uniform registers, 1 uniform blocks, 2 geometry shader, 3 compute shader.
    if (mode >= 2)
        RENDER(Report(mode == 2 ? "geometry shader mode is not supported" : "compute shader mode is not supported"));
}
// Guest words copied when the call is made: the game reuses the buffer as soon as the call returns.
// Offsets and counts are in 32-bit words (offset 4 is c1.x).
void GX2SetVertexUniformReg(uint32_t offset, uint32_t count, uint32_t data)
{
    gpu::dump::Uniforms(false, offset, count, data);
    if (gpu::renderer::Active())
        gpu::queue::RunWords(gpu::renderer::SetUniforms, false, offset,
            reinterpret_cast<const uint32_t*>(guest::Ptr(data)), std::min(count, 4096u));
}
void GX2SetPixelUniformReg(uint32_t offset, uint32_t count, uint32_t data)
{
    gpu::dump::Uniforms(true, offset, count, data);
    if (gpu::renderer::Active())
        gpu::queue::RunWords(gpu::renderer::SetUniforms, true, offset,
            reinterpret_cast<const uint32_t*>(guest::Ptr(data)), std::min(count, 4096u));
}

// ---- render targets, viewport, clears ----
void GX2SetColorBuffer(uint32_t buffer, uint32_t target)
{
    gpu::dump::ColorBuffer(buffer, target);
    const uint32_t cb = CallCopy(buffer, ColorBufferSize);
    RENDER(SetColorBuffer(cb, target));
}
void GX2SetDepthBuffer(uint32_t buffer)
{
    const uint32_t db = CallCopy(buffer, DepthBufferSize);
    RENDER(SetDepthBuffer(db));
}
void GX2SetViewport(float x, float y, float w, float h, float zNear, float zFar) { RENDER(SetViewport(x, y, w, h, zNear, zFar)); }
void GX2SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { RENDER(SetScissor(x, y, w, h)); }
void GX2ClearColor(uint32_t buffer, float r, float g, float b, float a)
{
    const uint32_t cb = CallCopy(buffer, ColorBufferSize);
    RENDER(ClearColor(cb, r, g, b, a));
}
// GX2ClearFlags: 1 depth, 2 stencil, 4 HiZ (no HiZ here).
void GX2ClearDepthStencilEx(uint32_t buffer, float depth, uint8_t stencil, uint32_t flags)
{
    const uint32_t db = CallCopy(buffer, DepthBufferSize);
    RENDER(ClearDepthStencil(db, depth, stencil, flags));
}

// ---- draws and vertex input ----
void GX2SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer)
{
    gpu::dump::AttribBuffer(index, size, stride, buffer);
    RENDER(SetAttribBuffer(index, size, stride, buffer));
}
void GX2DrawEx(uint32_t mode, uint32_t count, uint32_t offset, uint32_t instances)
{
    gpu::dump::Draw(mode, count, 0, 0);
    RENDER(Draw(mode, count, offset, instances));
}
void GX2DrawIndexedEx(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices, uint32_t offset, uint32_t instances)
{
    gpu::dump::Draw(mode, count, indexType, indices);
    RENDER(DrawIndexed(mode, count, indexType, indices, offset, instances));
}
void GX2SetPrimitiveRestartIndex(uint32_t index)
{
    if (index != 0xFFFF && index != 0xFFFFFFFF)
        RENDER(Report("primitive restart index " + Hex(index) + " is not supported"));
}
void GX2SetStreamOutEnable(bool enable)
{
    if (enable)
        RENDER(Report("stream out is not supported"));
}

// ---- textures and samplers ----
void GX2SetPixelTexture(uint32_t texture, uint32_t unit)
{
    gpu::dump::Texture(texture, unit);
    const uint32_t t = CallCopy(texture, TextureSize);
    RENDER(SetTexture(true, t, unit));
}
void GX2SetVertexTexture(uint32_t texture, uint32_t unit)
{
    const uint32_t t = CallCopy(texture, TextureSize);
    RENDER(SetTexture(false, t, unit));
}
void GX2SetPixelSampler(uint32_t sampler, uint32_t unit)
{
    gpu::dump::Sampler(sampler, unit);
    RENDER(SetSampler(true, sampler, unit));
}
// Sampler state is kept host-side, keyed by the GX2Sampler's address (its registers are not filled in).
void GX2InitSampler(uint32_t sampler, uint32_t clamp, uint32_t filter) { RENDER(InitSampler(sampler, clamp, filter)); }
void GX2InitSamplerClamping(uint32_t sampler, uint32_t x, uint32_t y, uint32_t z) { RENDER(InitSamplerClamping(sampler, x, y, z)); }
void GX2InitSamplerLOD(uint32_t sampler, float minLod, float maxLod, float bias) { RENDER(InitSamplerLOD(sampler, minLod, maxLod, bias)); }
void GX2InitSamplerXYFilter(uint32_t sampler, uint32_t mag, uint32_t min, uint32_t aniso) { RENDER(InitSamplerXYFilter(sampler, mag, min, aniso)); }
void GX2InitSamplerZMFilter(uint32_t sampler, uint32_t z, uint32_t mip) { RENDER(InitSamplerZMFilter(sampler, z, mip)); }

// ---- fixed-function state ----
void GX2SetAlphaTest(bool enable, uint32_t func, float ref) { RENDER(SetAlphaTest(enable, func, ref)); }
void GX2SetAlphaToMask(bool enable, uint32_t mode)
{
    (void)mode;
    if (enable)
        RENDER(Report("alpha-to-mask is not supported"));
}
void GX2SetBlendConstantColor(float r, float g, float b, float a) { RENDER(SetBlendConstant(r, g, b, a)); }
void GX2SetBlendControl(uint32_t target, uint32_t colorSrc, uint32_t colorDst, uint32_t colorOp, bool separateAlpha,
    uint32_t alphaSrc, uint32_t alphaDst, uint32_t alphaOp)
{
    RENDER(SetBlendControl(target, colorSrc, colorDst, colorOp, separateAlpha, alphaSrc, alphaDst, alphaOp));
}
void GX2SetColorControl(uint32_t logicOp, uint8_t blendMask, bool multiWrite, bool colorWrite)
{
    // GX2LogicOp 0xCC is COPY (source); anything else combines with the target.
    if (logicOp != 0xCC)
        RENDER(Report("logic op " + Hex(logicOp) + " is not supported (drawn as COPY)"));
    if (multiWrite)
        RENDER(Report("multi-write (render target 0 output to every target) is not supported"));
    RENDER(SetColorControl(logicOp, blendMask, multiWrite, colorWrite));
}
void GX2SetDepthOnlyControl(bool test, bool write, uint32_t func)
{
    RENDER(SetDepthStencilControl(test, write, func, false, false, { 7, 0, 0, 0 }, { 7, 0, 0, 0 }));
}
// GX2SetDepthStencilControl(depthTest, depthWrite, depthFunc, stencilTest, backfaceStencil, then GX2CompareFunction
// and three GX2StencilFunction (z-pass, z-fail, fail) for the front face and again for the back face).
void GX2SetDepthStencilControl(bool depthTest, bool depthWrite, uint32_t depthFunc, bool stencilTest, bool backfaceStencil,
    uint32_t frontFunc, uint32_t frontZPass, uint32_t frontZFail, uint32_t frontFail, uint32_t backFunc,
    uint32_t backZPass, uint32_t backZFail, uint32_t backFail)
{
    RENDER(SetDepthStencilControl(depthTest, depthWrite, depthFunc, stencilTest, backfaceStencil,
        { frontFunc, frontZPass, frontZFail, frontFail }, { backFunc, backZPass, backZFail, backFail }));
}
void GX2SetStencilMask(uint8_t frontPreMask, uint8_t frontWriteMask, uint8_t frontRef, uint8_t backPreMask,
    uint8_t backWriteMask, uint8_t backRef)
{
    RENDER(SetStencilMask({ frontPreMask, frontWriteMask, frontRef }, { backPreMask, backWriteMask, backRef }));
}
void GX2SetTargetChannelMasks(uint32_t m0, uint32_t m1, uint32_t m2, uint32_t m3, uint32_t m4, uint32_t m5, uint32_t m6,
    uint32_t m7)
{
    const uint32_t masks[8] = { m0, m1, m2, m3, m4, m5, m6, m7 };
    RENDER(SetTargetChannelMasks(masks));
}
void GX2SetLineWidth(float width) { RENDER(SetLineWidth(width)); }
void GX2SetPointSize(float w, float h)
{
    if (w != 1.0f || h != 1.0f)
        RENDER(Report("point sizes other than 1 are not supported"));
}
void GX2SetPointLimits(float min, float max) { (void)min, (void)max; }
// GX2SetPolygonControl(frontFace, cullFront, cullBack, polyMode, frontMode, backMode, offsetFront, offsetBack,
// offsetPointLine): polyMode enables the front/back GX2PolygonMode (0 point, 1 line, 2 triangle fill).
void GX2SetPolygonControl(uint32_t frontFace, bool cullFront, bool cullBack, bool polyMode, uint32_t frontMode,
    uint32_t backMode, bool offsetFront, bool offsetBack, bool offsetPointLine)
{
    if (polyMode && (frontMode != 2 || backMode != 2))
        RENDER(Report("point/line polygon modes (wireframe) are not supported"));
    RENDER(SetPolygonControl(frontFace, cullFront, cullBack, offsetFront || offsetBack || offsetPointLine));
}
void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clamp)
{
    (void)backOffset, (void)backScale;
    RENDER(SetPolygonOffset(frontOffset, frontScale, clamp));
}
void GX2SetRasterizerClipControl(bool rasterizer, bool zClip) { RENDER(SetRasterizerClip(rasterizer, zClip, false)); }
void GX2SetRasterizerClipControlEx(bool rasterizer, bool zClip, bool halfZ) { RENDER(SetRasterizerClip(rasterizer, zClip, halfZ)); }
// UE3 sets discrete tessellation with a maximum level of 1 (off) for every draw.
void GX2SetTessellation(uint32_t mode, uint32_t prim, uint32_t indexType) { (void)mode, (void)prim, (void)indexType; }
void GX2SetMaxTessellationLevel(float level)
{
    if (level > 1.0f)
        RENDER(Report("tessellation is not supported"));
}
void GX2SetMinTessellationLevel(float level) { (void)level; }

// ---- queries ----
// Occlusion queries (gpu.occlusion_queries = on; off by default): run on the GPU (renderer::BeginQuery...), and the
// game culls what they report hidden. Each begin of a query object gets an issue number here, at call time, so a
// result is only taken for the begin it belongs to (UE3 pools its query objects). Off, every query reads as visible.
// Measured in level 2's town (1,900 draws): about 190 queries a frame, 55% hidden, but only 12-16% fewer draws, and
// the game reads every result in the next frame, waiting for it: with the GPU thread up to a frame behind in heavy
// scenes it waited about 5 ms a frame, so frames got slower (10.85 -> 11.95 ms). DT_TEST_OCCLUDE_ALL (testing)
// reports every query hidden.
std::mutex gQueryMutex;
std::unordered_map<uint32_t, uint64_t> gQueryIssue;  // query -> its latest begin's issue number

bool OcclusionOn()
{
    static const bool on = settings::Get("gpu.occlusion_queries", "off") == "on" && getenv("DT_TEST_OCCLUDE_ALL") == nullptr;
    return on && gpu::renderer::Active() && gpu::queue::Async();
}

void GX2QueryBegin(uint32_t type, uint32_t data)
{
    (void)type;
    gQueryBegins.fetch_add(1, std::memory_order_relaxed);
    if (!OcclusionOn())
        return;
    uint64_t issue;
    {
        std::lock_guard lock(gQueryMutex);
        issue = ++gQueryIssue[data];
    }
    RENDER(BeginQuery(data, issue));
}
void GX2QueryEnd(uint32_t type, uint32_t data)
{
    (void)type;
    if (OcclusionOn())
        RENDER(EndQuery(data));
}
bool GX2QueryGetOcclusionResult(uint32_t data, uint32_t outResult)
{
    static const bool hideAll = getenv("DT_TEST_OCCLUDE_ALL") != nullptr;
    uint64_t samples = hideAll ? 0 : 0xFFFFFFFFull;
    if (OcclusionOn())
    {
        uint64_t issue = 0;
        {
            std::lock_guard lock(gQueryMutex);
            if (auto it = gQueryIssue.find(data); it != gQueryIssue.end())
                issue = it->second;
        }
        if (issue && !gpu::renderer::QueryResult(data, issue, samples))
        {
            // Not counted yet (the game always waits for last frame's results). The GPU thread hands results over as
            // the GPU finishes them (renderer::DoDraw); wait for this one, and queue a resolve in case the GPU thread
            // gets there first (it submits and reads every pending query). A query that still has no result (never
            // ended) reads as visible.
            gpu::queue::Run([] { gpu::renderer::ResolveQueries(); });
            gpu::queue::Flush();
            if (!gpu::renderer::QueryResult(data, issue, samples, 100.0))
                samples = 0xFFFFFFFFull, dtlog::Important("[gx2] occlusion query 0x%08X had no result after 100 ms\n", data);
        }
    }
    if (outResult)
        guest::Write64(outResult, samples);
    return true;
}

// ---- debugging and performance counters ----
void GX2DebugTagUserString(PPCContext& ctx, uint8_t*) { (void)ctx; }  // variadic
void GX2DebugTagUserStringVA(uint32_t type, GuestString fmt, uint32_t args) { (void)type, (void)fmt, (void)args; }
void GX2InitPerfMetric(uint32_t data, uint32_t type, uint32_t id) { (void)data, (void)type, (void)id; }
bool GX2GetPerfMetricF32(uint32_t data, uint32_t index, uint32_t out)
{
    (void)data, (void)index;
    if (out)
        guest::Write32(out, 0);
    return true;
}
bool GX2GetPerfMetricU64(uint32_t data, uint32_t index, uint32_t out)
{
    (void)data, (void)index;
    if (out)
        guest::Write64(out, 0);
    return true;
}
void CounterNoop(PPCContext& ctx, uint8_t*) { (void)ctx; }  // _GX2*Counters: internal perf-counter helpers
} // namespace

void SetFrameLimit(uint64_t frames) { gFrameLimit = frames; }
uint64_t FrameCount() { return gFrames; }
uint64_t TimeStamp() { return gpu::queue::Retired(); }
void SetShaderCacheDir(const std::string& dir) { gShaderCacheDir = dir; }

CAFE_IMPORT(gx2, "GX2Init", GX2Init);
CAFE_IMPORT(gx2, "GX2SetupContextStateEx", GX2SetupContextStateEx);
CAFE_IMPORT(gx2, "GX2SetContextState", GX2SetContextState);
CAFE_IMPORT(gx2, "GX2CalcTVSize", GX2CalcTVSize);
CAFE_IMPORT(gx2, "GX2CalcDRCSize", GX2CalcDRCSize);
CAFE_IMPORT(gx2, "GX2SetTVBuffer", GX2SetTVBuffer);
CAFE_IMPORT(gx2, "GX2SetDRCBuffer", GX2SetDRCBuffer);
CAFE_IMPORT(gx2, "GX2SetTVScale", GX2SetTVScale);
CAFE_IMPORT(gx2, "GX2SetDRCScale", GX2SetDRCScale);
CAFE_IMPORT(gx2, "GX2SetTVEnable", GX2SetTVEnable);
CAFE_IMPORT(gx2, "GX2SetDRCEnable", GX2SetDRCEnable);
CAFE_IMPORT(gx2, "GX2GetTVGamma", GX2GetTVGamma);
CAFE_IMPORT(gx2, "GX2GetDRCGamma", GX2GetDRCGamma);
CAFE_IMPORT(gx2, "GX2SetTVGamma", GX2SetTVGamma);
CAFE_IMPORT(gx2, "GX2SetDRCGamma", GX2SetDRCGamma);
CAFE_IMPORT(gx2, "GX2SetSwapInterval", GX2SetSwapInterval);
CAFE_IMPORT(gx2, "GX2CopyColorBufferToScanBuffer", GX2CopyColorBufferToScanBuffer);
CAFE_IMPORT(gx2, "GX2SwapScanBuffers", GX2SwapScanBuffers);
CAFE_IMPORT(gx2, "GX2Flush", GX2Flush);
CAFE_IMPORT(gx2, "GX2DrawDone", GX2DrawDone);
CAFE_IMPORT(gx2, "GX2GetLastSubmittedTimeStamp", GX2GetLastSubmittedTimeStamp);
CAFE_IMPORT(gx2, "GX2GetRetiredTimeStamp", GX2GetRetiredTimeStamp);
CAFE_IMPORT(gx2, "GX2WaitTimeStamp", GX2WaitTimeStamp);
CAFE_IMPORT(gx2, "GX2Invalidate", GX2Invalidate);
CAFE_IMPORT(gx2, "GX2SetSemaphore", GX2SetSemaphore);
CAFE_IMPORT(gx2, "GX2CalcSurfaceSizeAndAlignment", GX2CalcSurfaceSizeAndAlignment);
CAFE_IMPORT(gx2, "GX2CalcDepthBufferHiZInfo", GX2CalcDepthBufferHiZInfo);
CAFE_IMPORT(gx2, "GX2InitColorBufferRegs", GX2InitColorBufferRegs);
CAFE_IMPORT(gx2, "GX2InitDepthBufferRegs", GX2InitDepthBufferRegs);
CAFE_IMPORT(gx2, "GX2InitDepthBufferHiZEnable", GX2InitDepthBufferHiZEnable);
CAFE_IMPORT(gx2, "GX2InitTextureRegs", GX2InitTextureRegs);
CAFE_IMPORT(gx2, "GX2GetSurfaceFormatBits", GX2GetSurfaceFormatBits);
CAFE_IMPORT(gx2, "GX2AllocateTilingApertureEx", GX2AllocateTilingApertureEx);
CAFE_IMPORT(gx2, "GX2FreeTilingAperture", GX2FreeTilingAperture);
CAFE_IMPORT(gx2, "GX2CopySurface", GX2CopySurface);
CAFE_IMPORT(gx2, "GX2ConvertDepthBufferToTextureSurface", GX2ConvertDepthBufferToTextureSurface);
CAFE_IMPORT(gx2, "GX2CalcFetchShaderSizeEx", GX2CalcFetchShaderSizeEx);
CAFE_IMPORT(gx2, "GX2InitFetchShaderEx", GX2InitFetchShaderEx);
CAFE_IMPORT(gx2, "GX2SetFetchShader", GX2SetFetchShader);
CAFE_IMPORT(gx2, "GX2SetVertexShader", GX2SetVertexShader);
CAFE_IMPORT(gx2, "GX2SetPixelShader", GX2SetPixelShader);
CAFE_IMPORT(gx2, "GX2SetShaderModeEx", GX2SetShaderModeEx);
CAFE_IMPORT(gx2, "GX2SetVertexUniformReg", GX2SetVertexUniformReg);
CAFE_IMPORT(gx2, "GX2SetPixelUniformReg", GX2SetPixelUniformReg);
CAFE_IMPORT(gx2, "GX2SetColorBuffer", GX2SetColorBuffer);
CAFE_IMPORT(gx2, "GX2SetDepthBuffer", GX2SetDepthBuffer);
CAFE_IMPORT(gx2, "GX2SetViewport", GX2SetViewport);
CAFE_IMPORT(gx2, "GX2SetScissor", GX2SetScissor);
CAFE_IMPORT(gx2, "GX2ClearColor", GX2ClearColor);
CAFE_IMPORT(gx2, "GX2ClearDepthStencilEx", GX2ClearDepthStencilEx);
CAFE_IMPORT(gx2, "GX2SetAttribBuffer", GX2SetAttribBuffer);
CAFE_IMPORT(gx2, "GX2DrawEx", GX2DrawEx);
CAFE_IMPORT(gx2, "GX2DrawIndexedEx", GX2DrawIndexedEx);
CAFE_IMPORT(gx2, "GX2SetPrimitiveRestartIndex", GX2SetPrimitiveRestartIndex);
CAFE_IMPORT(gx2, "GX2SetStreamOutEnable", GX2SetStreamOutEnable);
CAFE_IMPORT(gx2, "GX2SetPixelTexture", GX2SetPixelTexture);
CAFE_IMPORT(gx2, "GX2SetVertexTexture", GX2SetVertexTexture);
CAFE_IMPORT(gx2, "GX2SetPixelSampler", GX2SetPixelSampler);
CAFE_IMPORT(gx2, "GX2InitSampler", GX2InitSampler);
CAFE_IMPORT(gx2, "GX2InitSamplerClamping", GX2InitSamplerClamping);
CAFE_IMPORT(gx2, "GX2InitSamplerLOD", GX2InitSamplerLOD);
CAFE_IMPORT(gx2, "GX2InitSamplerXYFilter", GX2InitSamplerXYFilter);
CAFE_IMPORT(gx2, "GX2InitSamplerZMFilter", GX2InitSamplerZMFilter);
CAFE_IMPORT(gx2, "GX2SetAlphaTest", GX2SetAlphaTest);
CAFE_IMPORT(gx2, "GX2SetAlphaToMask", GX2SetAlphaToMask);
CAFE_IMPORT(gx2, "GX2SetBlendConstantColor", GX2SetBlendConstantColor);
CAFE_IMPORT(gx2, "GX2SetBlendControl", GX2SetBlendControl);
CAFE_IMPORT(gx2, "GX2SetColorControl", GX2SetColorControl);
CAFE_IMPORT(gx2, "GX2SetDepthOnlyControl", GX2SetDepthOnlyControl);
CAFE_IMPORT(gx2, "GX2SetDepthStencilControl", GX2SetDepthStencilControl);
CAFE_IMPORT(gx2, "GX2SetStencilMask", GX2SetStencilMask);
CAFE_IMPORT(gx2, "GX2SetTargetChannelMasks", GX2SetTargetChannelMasks);
CAFE_IMPORT(gx2, "GX2SetLineWidth", GX2SetLineWidth);
CAFE_IMPORT(gx2, "GX2SetPointSize", GX2SetPointSize);
CAFE_IMPORT(gx2, "GX2SetPointLimits", GX2SetPointLimits);
CAFE_IMPORT(gx2, "GX2SetPolygonControl", GX2SetPolygonControl);
CAFE_IMPORT(gx2, "GX2SetPolygonOffset", GX2SetPolygonOffset);
CAFE_IMPORT(gx2, "GX2SetRasterizerClipControl", GX2SetRasterizerClipControl);
CAFE_IMPORT(gx2, "GX2SetRasterizerClipControlEx", GX2SetRasterizerClipControlEx);
CAFE_IMPORT(gx2, "GX2SetTessellation", GX2SetTessellation);
CAFE_IMPORT(gx2, "GX2SetMaxTessellationLevel", GX2SetMaxTessellationLevel);
CAFE_IMPORT(gx2, "GX2SetMinTessellationLevel", GX2SetMinTessellationLevel);
CAFE_IMPORT(gx2, "GX2QueryBegin", GX2QueryBegin);
CAFE_IMPORT(gx2, "GX2QueryEnd", GX2QueryEnd);
CAFE_IMPORT(gx2, "GX2QueryGetOcclusionResult", GX2QueryGetOcclusionResult);
CAFE_IMPORT_RAW(gx2, "GX2DebugTagUserString", GX2DebugTagUserString);
CAFE_IMPORT(gx2, "GX2DebugTagUserStringVA", GX2DebugTagUserStringVA);
CAFE_IMPORT(gx2, "GX2InitPerfMetric", GX2InitPerfMetric);
CAFE_IMPORT(gx2, "GX2GetPerfMetricF32", GX2GetPerfMetricF32);
CAFE_IMPORT(gx2, "GX2GetPerfMetricU64", GX2GetPerfMetricU64);
CAFE_IMPORT_RAW(gx2, "_GX2ResetCounters", CounterNoop);
CAFE_IMPORT_RAW(gx2, "_GX2SampleCounters", CounterNoop);
CAFE_IMPORT_RAW(gx2, "_GX2SetCounterInfo", CounterNoop);
CAFE_IMPORT_RAW(gx2, "_GX2StartCounters", CounterNoop);
CAFE_IMPORT_RAW(gx2, "_GX2StopCounters", CounterNoop);
} // namespace cafe::gx2
