#include "renderer.h"

#include "../guest.h"
#include "../log.h"
#include "../game_ui.h"
#include "../host/menu.h"
#include "../platform.h"
#include "../imports.h"
#include "../trace.h"
#include "../settings.h"
#include "cache_version.h"
#include "device.h"
#include "gx2_types.h"
#include "png.h"
#include "programs.h"
#include "shader/r700.h"
#include "shader/translator.h"
#include "surface.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include <ctime>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gpu::renderer
{
namespace
{
// ---------------------------------------------------------------------------------------------------------
// GX2 enums
// ---------------------------------------------------------------------------------------------------------
constexpr uint32_t InvalidateTexture = 0x02, InvalidateCpu = 0x40;
constexpr uint32_t ScanTargetTV = 1;

struct FormatInfo
{
    VkFormat format;
    uint32_t bytesPerElement;
    uint32_t blockSize;  // 1, or 4 for BCn
};

// GX2SurfaceFormat -> Vulkan. A GX2 format is a hardware format (low 6 bits) plus a number format: 0x100 uint,
// 0x200 snorm, 0x300 sint, 0x400 sRGB, 0x800 float (for example UINT_R8 0x101, SNORM_R8 0x201, BC4_SNORM 0x234).
// The sRGB bit is handled separately (SrgbVariant / a second view on render targets). Packed formats store their
// first component in the low bits, which is Vulkan's reversed (B..R / A..R) _PACK naming. Texture data is
// little-endian, as the GPU reads it.
std::optional<FormatInfo> SurfaceFormat(uint32_t format)
{
    switch (format & ~0x400u)
    {
    case 0x001: return FormatInfo{ VK_FORMAT_R8_UNORM, 1, 1 };
    case 0x101: return FormatInfo{ VK_FORMAT_R8_UINT, 1, 1 };
    case 0x201: return FormatInfo{ VK_FORMAT_R8_SNORM, 1, 1 };
    case 0x301: return FormatInfo{ VK_FORMAT_R8_SINT, 1, 1 };
    case 0x005: return FormatInfo{ VK_FORMAT_R16_UNORM, 2, 1 };
    case 0x105: return FormatInfo{ VK_FORMAT_R16_UINT, 2, 1 };
    case 0x205: return FormatInfo{ VK_FORMAT_R16_SNORM, 2, 1 };
    case 0x305: return FormatInfo{ VK_FORMAT_R16_SINT, 2, 1 };
    case 0x806: return FormatInfo{ VK_FORMAT_R16_SFLOAT, 2, 1 };
    case 0x007: return FormatInfo{ VK_FORMAT_R8G8_UNORM, 2, 1 };
    case 0x107: return FormatInfo{ VK_FORMAT_R8G8_UINT, 2, 1 };
    case 0x207: return FormatInfo{ VK_FORMAT_R8G8_SNORM, 2, 1 };
    case 0x307: return FormatInfo{ VK_FORMAT_R8G8_SINT, 2, 1 };
    case 0x008: return FormatInfo{ VK_FORMAT_B5G6R5_UNORM_PACK16, 2, 1 };
    case 0x10D: return FormatInfo{ VK_FORMAT_R32_UINT, 4, 1 };
    case 0x30D: return FormatInfo{ VK_FORMAT_R32_SINT, 4, 1 };
    case 0x80E: return FormatInfo{ VK_FORMAT_R32_SFLOAT, 4, 1 };
    case 0x00F: return FormatInfo{ VK_FORMAT_R16G16_UNORM, 4, 1 };
    case 0x10F: return FormatInfo{ VK_FORMAT_R16G16_UINT, 4, 1 };
    case 0x20F: return FormatInfo{ VK_FORMAT_R16G16_SNORM, 4, 1 };
    case 0x30F: return FormatInfo{ VK_FORMAT_R16G16_SINT, 4, 1 };
    case 0x810: return FormatInfo{ VK_FORMAT_R16G16_SFLOAT, 4, 1 };
    case 0x816: return FormatInfo{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1 };
    case 0x019: return FormatInfo{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1 };
    case 0x119: return FormatInfo{ VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1 };
    case 0x219: return FormatInfo{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, 4, 1 };
    case 0x01A: return FormatInfo{ VK_FORMAT_R8G8B8A8_UNORM, 4, 1 };
    case 0x11A: return FormatInfo{ VK_FORMAT_R8G8B8A8_UINT, 4, 1 };
    case 0x21A: return FormatInfo{ VK_FORMAT_R8G8B8A8_SNORM, 4, 1 };
    case 0x31A: return FormatInfo{ VK_FORMAT_R8G8B8A8_SINT, 4, 1 };
    case 0x11D: return FormatInfo{ VK_FORMAT_R32G32_UINT, 8, 1 };
    case 0x31D: return FormatInfo{ VK_FORMAT_R32G32_SINT, 8, 1 };
    case 0x81E: return FormatInfo{ VK_FORMAT_R32G32_SFLOAT, 8, 1 };
    case 0x01F: return FormatInfo{ VK_FORMAT_R16G16B16A16_UNORM, 8, 1 };
    case 0x11F: return FormatInfo{ VK_FORMAT_R16G16B16A16_UINT, 8, 1 };
    case 0x21F: return FormatInfo{ VK_FORMAT_R16G16B16A16_SNORM, 8, 1 };
    case 0x31F: return FormatInfo{ VK_FORMAT_R16G16B16A16_SINT, 8, 1 };
    case 0x820: return FormatInfo{ VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1 };
    case 0x122: return FormatInfo{ VK_FORMAT_R32G32B32A32_UINT, 16, 1 };
    case 0x322: return FormatInfo{ VK_FORMAT_R32G32B32A32_SINT, 16, 1 };
    case 0x823: return FormatInfo{ VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1 };
    // Depth formats used as textures hold depth copied from a depth buffer (GX2ConvertDepthBufferToTextureSurface),
    // which the renderer stores as 32-bit float.
    case 0x011: case 0x811: case 0x81C: return FormatInfo{ VK_FORMAT_R32_SFLOAT, 4, 1 };
    case 0x031: return FormatInfo{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4 };
    case 0x032: return FormatInfo{ VK_FORMAT_BC2_UNORM_BLOCK, 16, 4 };
    case 0x033: return FormatInfo{ VK_FORMAT_BC3_UNORM_BLOCK, 16, 4 };
    case 0x034: return FormatInfo{ VK_FORMAT_BC4_UNORM_BLOCK, 8, 4 };
    case 0x234: return FormatInfo{ VK_FORMAT_BC4_SNORM_BLOCK, 8, 4 };
    case 0x035: return FormatInfo{ VK_FORMAT_BC5_UNORM_BLOCK, 16, 4 };
    case 0x235: return FormatInfo{ VK_FORMAT_BC5_SNORM_BLOCK, 16, 4 };
    default: return std::nullopt;
    }
}

// The shader-side type of a Vulkan format's data: 0 float, 1 unsigned integer, 2 signed integer.
uint32_t VkNumberType(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}

// The shader-side type of a GX2 format's data: 0 float (unorm/snorm/float), 1 unsigned integer, 2 signed integer.
uint32_t NumberType(uint32_t format)
{
    switch (format & 0xF00)
    {
    case 0x100: return 1;
    case 0x300: return 2;
    default: return 0;
    }
}

VkFormat SrgbVariant(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    default: return f;
    }
}

// GX2AttribFormat = component layout (low 6 bits) | flags: 0x100 integer (not normalised), 0x200 signed,
// 0x400 degamma, 0x800 scaled (integer converted to float without normalising; float layouts carry it too).
// For example 0x00A UNORM 8_8_8_8, 0x20A SNORM 8_8_8_8, 0x10A UINT 8_8_8_8, 0x80A USCALED 8_8_8_8,
// 0xA07 SSCALED 16_16, 0x80D FLOAT 32_32.
struct AttribFormat
{
    VkFormat format;
    uint32_t componentSize, components;  // the endian-swap unit and how many there are (packed layouts: one)
};

std::optional<AttribFormat> VertexFormat(uint32_t f)
{
    enum { Unorm, Snorm, Uint, Sint, Uscaled, Sscaled };
    const bool integer = f & 0x100, sign = f & 0x200, scaled = f & 0x800;
    const int numeric = integer ? (sign ? Sint : Uint) : scaled ? (sign ? Sscaled : Uscaled) : (sign ? Snorm : Unorm);
    // Vulkan formats per numeric kind: UNORM, SNORM, UINT, SINT, USCALED, SSCALED.
    using F = std::array<VkFormat, 6>;
    auto pick = [&](const F& t, uint32_t size, uint32_t count) -> std::optional<AttribFormat> {
        if (t[numeric] == VK_FORMAT_UNDEFINED)
            return std::nullopt;
        return AttribFormat{ t[numeric], size, count };
    };
    switch (f & 0x3F)
    {
    case 0x00: return pick(F{ VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
                               VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED }, 1, 1);
    case 0x02: return pick(F{ VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
                               VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED }, 2, 1);
    case 0x03: return AttribFormat{ VK_FORMAT_R16_SFLOAT, 2, 1 };
    case 0x04: return pick(F{ VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
                               VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED }, 1, 2);
    case 0x05: return pick(F{ VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
                               VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED }, 4, 1);
    case 0x06: return AttribFormat{ VK_FORMAT_R32_SFLOAT, 4, 1 };
    case 0x07: return pick(F{ VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT,
                               VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED }, 2, 2);
    case 0x08: return AttribFormat{ VK_FORMAT_R16G16_SFLOAT, 2, 2 };
    case 0x09: return AttribFormat{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1 };
    case 0x0A: return pick(F{ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT,
                               VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED }, 1, 4);
    case 0x0B: return pick(F{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                               VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32,
                               VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32 }, 4, 1);
    case 0x0C: return pick(F{ VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
                               VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED }, 4, 2);
    case 0x0D: return AttribFormat{ VK_FORMAT_R32G32_SFLOAT, 4, 2 };
    case 0x0E: return pick(F{ VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
                               VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_USCALED,
                               VK_FORMAT_R16G16B16A16_SSCALED }, 2, 4);
    case 0x0F: return AttribFormat{ VK_FORMAT_R16G16B16A16_SFLOAT, 2, 4 };
    case 0x10: return pick(F{ VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_UINT,
                               VK_FORMAT_R32G32B32_SINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED }, 4, 3);
    case 0x11: return AttribFormat{ VK_FORMAT_R32G32B32_SFLOAT, 4, 3 };
    case 0x12: return pick(F{ VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_UINT,
                               VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED }, 4, 4);
    case 0x13: return AttribFormat{ VK_FORMAT_R32G32B32A32_SFLOAT, 4, 4 };
    default: return std::nullopt;  // 0x01 4_4: no matching Vulkan vertex format
    }
}

VkBlendFactor BlendFactor(uint32_t m)
{
    static const VkBlendFactor t[] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA };
    return m < std::size(t) ? t[m] : VK_BLEND_FACTOR_ONE;
}

VkBlendOp BlendOp(uint32_t m)
{
    static const VkBlendOp t[] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
        VK_BLEND_OP_REVERSE_SUBTRACT };
    return m < std::size(t) ? t[m] : VK_BLEND_OP_ADD;
}

VkSamplerAddressMode AddressMode(uint32_t clamp)
{
    switch (clamp)
    {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// ---------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------
struct SamplerState
{
    uint32_t clamp[3] = { 2, 2, 2 }, mag = 1, min = 1, mip = 1, aniso = 0;
    float minLod = 0, maxLod = 16, bias = 0;
    bool operator<(const SamplerState& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

struct Blend
{
    uint32_t colorSrc = 1, colorDst = 0, colorOp = 0, alphaSrc = 1, alphaDst = 0, alphaOp = 0;
    bool separate = false;
};

struct AttribBuffer
{
    uint32_t addr = 0, size = 0, stride = 0;
};

struct State
{
    uint32_t colorBuffers[8] = {};
    uint32_t depthBuffer = 0;
    float viewport[6] = { 0, 0, 0, 0, 0, 1 };
    uint32_t scissor[4] = {};
    bool viewportSet = false, scissorSet = false;
    uint32_t fetchShader = 0, vertexShader = 0, pixelShader = 0;
    Blend blend[8];
    uint32_t blendMask = 0, channelMasks[8] = { 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF };
    bool colorWrite = true;
    uint32_t frontFace = 0;
    bool cullFront = false, cullBack = false;
    bool depthTest = false, depthWrite = false;
    uint32_t depthFunc = 1;
    bool stencilTest = false, backfaceStencil = false;
    StencilFace stencilFront, stencilBack;
    StencilMask maskFront, maskBack;
    bool polygonOffset = false;
    float polyOffset = 0, polyScale = 0, polyClamp = 0;
    bool rasterizer = true, zClip = true, halfZ = false;
    float blendConstant[4] = {};
    float lineWidth = 1;
    bool alphaTest = false;
    uint32_t alphaFunc = 7;
    float alphaRef = 0;
    uint32_t textures[2][16] = {}, samplers[2][16] = {};  // [0] vertex, [1] pixel
    float constants[2][1024] = {};
    AttribBuffer attribs[16];
    uint32_t copies = 0;  // guest memory for the bound structures' copies (StateCopy)
};

// A bound structure (texture, color/depth buffer, shader) is copied into memory owned by the state when it is set,
// as GX2 copies its registers into the command buffer: the game may change or free the structure afterwards while
// the binding stays in effect. Slots: textures 0-31 (vertex, pixel), color buffers 32-39, depth 40, shaders 41-42.
constexpr uint32_t CopySlotSize = 0x140, CopySlots = 43;
constexpr uint32_t SlotTexture = 0, SlotColor = 32, SlotDepth = 40, SlotVertexShader = 41, SlotPixelShader = 42;
uint32_t StateCopy(State& st, uint32_t slot, uint32_t src, uint32_t size)
{
    if (!src)
        return 0;
    if (!st.copies)
        st.copies = guest::SystemAlloc(CopySlotSize * CopySlots, 64);
    const uint32_t dst = st.copies + slot * CopySlotSize;
    if (dst != src)
        memcpy(guest::Ptr(dst), guest::Ptr(src), size);
    return dst;
}

// A guest surface on the GPU: one Vulkan image covering all of a GX2 surface's mip levels and slices, keyed by its
// guest address. Textures, color buffers and depth buffers that share memory share the image, so rendering to a
// surface and sampling it later, or rendering into one mip or cube face, all see the same data. Views are made
// on demand.
// An image's views by their parameters, with the last two used in front (DoDraw asks for one per texture per draw; the
// hash map lookup was 3% of the GPU thread). Views are only added, never removed, until the image is retired.
struct ViewCache
{
    std::unordered_map<uint64_t, VkImageView> map;
    uint64_t keys[2] = {};
    VkImageView views[2] = {};
};

struct GpuSurface
{
    // The description the image was made for. width and height are the image's own: the game's size times `scale`
    // for render targets drawn at a higher internal resolution (guestWidth/guestHeight the game's).
    uint32_t addr = 0, width = 0, height = 0, depth = 1, levels = 1, dim = 1;
    uint32_t guestWidth = 0, guestHeight = 0;
    float scale = 1.0f;
    bool shadow = false;  // a shadow map made larger by graphics.shadow_quality (gShadowScale)
    Image upload;  // scaled surfaces: CPU data is uploaded at the game's size here, then scaled into the image
    VkFormat format = VK_FORMAT_UNDEFINED;  // the unorm (non-sRGB) base format, or a depth format
    bool isDepth = false, cube = false, is3D = false;
    Image image;
    ViewCache views;
    // The guest memory behind it, for uploads of CPU-written data.
    uint32_t imageSize = 0, mipAddr = 0, mipSize = 0;
    bool cpuDirty = true;   // guest memory changed: upload before the next use
    bool gpuOwned = false;  // rendered or copied into on the GPU: the image is newer than guest memory
    // While gpuOwned: the levels the GPU wrote (all for render targets; single levels for GX2CopySurface, which
    // texture streaming uses to move a texture's lower mips into its new allocation) with each level's guest memory
    // fingerprint at the time. When the CPU then writes the memory (streaming loads the new top level from disk), the
    // levels whose memory is unchanged keep the GPU's data and the others (cpuLevels) upload from guest memory
    // (KeepGpuLevels): that memory never received the copies, and holds whatever was there before.
    uint32_t gpuLevels = 0, cpuLevels = 0;
    bool mirrored = false;  // gpuOwned, but every GPU write since was also made in guest memory (CopyInGuest)
    uint32_t levelAddr[14] = {}, levelBytes[14] = {};
    uint64_t levelSample[14] = {};
    Surface desc{};  // the latest description it was looked up with (uploads of cpuLevels)
    uint64_t sample = 0, checkedFrame = ~0ull;
    // Guest memory as it was when the GPU wrote the image (GPU writes never reach guest memory): if it changes
    // later, the CPU or DMA overwrote the memory (a texture streamed into a freed render target, say).
    uint64_t ownedSample = 0, ownedCheckedFrame = ~0ull;
    uint64_t lastChange = 0;  // the last frame its data changed (CPU upload or GPU write): see CheckDue
    uint32_t indexedImage = 0, indexedImageSize = 0, indexedMip = 0, indexedMipSize = 0;  // as in gSurfaceRanges
    uint64_t lastWrite = 0;  // GPU write serial: the latest-written alias of an address holds its data
    uint64_t lastUse = 0;    // frame index, for evicting aliases
    Image snapshot;         // copy sampled by draws that read a surface they render to
    uint64_t snapshotOf = ~0ull;  // the lastWrite the snapshot was copied at (still current: no copy needed)
    ViewCache snapshotViews;
};

struct ShaderModule
{
    VkShaderModule module = VK_NULL_HANDLE;
    uint32_t textureMask = 0, outputMask = 0, bufferMask = 0;
    std::string error;
};

// The renderer's lock: every renderer call takes it (about 20,000 times a frame, nearly always the GPU thread, so
// uncontended). std::recursive_timed_mutex is a mutex, a condition variable and owner tracking in MSVC's library (about
// 5% of the GPU thread); this is an SRW lock with an owner check for recursion. try_lock_for (Shutdown) polls.
class RendererMutex
{
public:
    void lock()
    {
        const uintptr_t me = Me();
        if (owner.load(std::memory_order_relaxed) == me)
        {
            depth++;
            return;
        }
        m.lock();
        owner.store(me, std::memory_order_relaxed);
        depth = 1;
    }
    bool try_lock()
    {
        const uintptr_t me = Me();
        if (owner.load(std::memory_order_relaxed) == me)
        {
            depth++;
            return true;
        }
        if (!m.try_lock())
            return false;
        owner.store(me, std::memory_order_relaxed);
        depth = 1;
        return true;
    }
    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& d)
    {
        const auto until = std::chrono::steady_clock::now() + d;
        while (!try_lock())
        {
            if (std::chrono::steady_clock::now() >= until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    void unlock()
    {
        if (--depth == 0)
        {
            owner.store(0, std::memory_order_relaxed);
            m.unlock();
        }
    }

private:
    static uintptr_t Me()
    {
        static thread_local char token;
        return reinterpret_cast<uintptr_t>(&token);
    }
    std::mutex m;
    std::atomic<uintptr_t> owner{ 0 };
    uint32_t depth = 0;
};
RendererMutex gMutex;
bool gActive = false;
std::string gCacheDir;

// Internal resolution (graphics.resolution): render targets of the game's TV-derived sizes are made this many times
// larger (2 for 2560x1440). Read at startup.
float gRenderScale = 1.0f;

// Whether a surface the game describes at w x h is one of its TV-derived render targets: the 1280x720 frame and its
// halves and quarters (with or without a one-texel border, as the bloom chain has them).
bool Scalable(uint32_t w, uint32_t h)
{
    for (uint32_t d = 1; d <= 16; d *= 2)
        if ((w == 1280 / d && h == 720 / d) || (w == 1280 / d + 2 && h == 720 / d + 2))
            return true;
    return false;
}

// Shadow quality (graphics.shadow_quality): shadow maps are made this many times larger (2 = 2048x1024 for the game's
// 1024x512). Read at startup.
float gShadowScale = 1.0f;

// Whether a render target the game describes is one of its shadow maps: a power-of-two depth buffer, or the 32-bit float
// color target the game writes depth into beside it, at least 256 pixels a side and not one of the TV-derived sizes.
// (The luminance chain's R32F targets are 64x64 and smaller.)
bool ShadowScalable(uint32_t w, uint32_t h, VkFormat format, bool asDepth, uint32_t levels, bool is3D, bool cube)
{
    auto pow2 = [](uint32_t v) { return v && !(v & (v - 1)); };
    return levels == 1 && !is3D && !cube && pow2(w) && pow2(h) && std::min(w, h) >= 256 && std::max(w, h) <= 4096 &&
           !Scalable(w, h) && (asDepth || format == VK_FORMAT_R32_SFLOAT);
}

// The surface TextureView last resolved (the draw's pixel stage notes the shadow maps it samples).
thread_local struct GpuSurface* gViewSurface = nullptr;

std::unordered_map<uint32_t, State> gContexts;
State gDefaultContext;
State* gState = &gDefaultContext;

std::unordered_map<uint32_t, std::vector<AttribStream>> gFetchShaders;
std::unordered_map<uint32_t, SamplerState> gSamplerStates;
std::map<SamplerState, VkSampler> gSamplers;
// Surfaces by description (address, size, format...): games reuse render-target memory for other passes with
// other sizes and formats, so several aliases of one address coexist. gAtAddr lists them per address.
std::unordered_map<uint64_t, GpuSurface> gSurfaces;
// Each alias with its surface (map nodes do not move; every removal from gSurfaces also removes it here): looking each
// key up again cost about 10% of the GPU thread (LatestWritten runs twice per texture per draw).
struct Alias
{
    uint64_t key;
    GpuSurface* g;
    bool operator==(uint64_t k) const { return key == k; }
};
std::unordered_map<uint32_t, std::vector<Alias>> gAtAddr;
// Addresses with a GPU-written alias, and how many: LatestWritten runs twice per texture per draw (20,000 times a
// frame in level 2's heavy areas) and almost every texture was never rendered, so this small map answers first (it
// had been 12% of the GPU thread walking every texture's aliases).
std::unordered_map<uint32_t, uint32_t> gOwnedAt;
uint64_t gOwnedGen = 0;  // changes whenever a surface's ownership or an address's aliases change (LatestWritten's memo)

void SetOwned(GpuSurface& g, bool owned)
{
    if (g.gpuOwned == owned)
        return;
    g.gpuOwned = owned;
    gOwnedGen++;
    if (owned)
        gOwnedAt[g.addr]++;
    else if (auto it = gOwnedAt.find(g.addr); it != gOwnedAt.end() && --it->second == 0)
        gOwnedAt.erase(it);
}

// Guest address ranges -> objects, so GX2Invalidate (over a hundred calls a frame) finds what overlaps its range
// without scanning every surface and cached buffer.
template <typename Key>
class RangeIndex
{
public:
    void Add(uint32_t start, uint32_t length, Key key)
    {
        if (!length)
            return;
        if (length > LargeLimit)
        {
            large.push_back({ start, length, key });
            return;
        }
        for (uint32_t p = start >> PageBits, last = LastPage(start, length); p <= last; p++)
            pages[p].push_back({ start, length, key });
        count++;
    }
    void Remove(uint32_t start, Key key)
    {
        for (const Entry& e : pages[start >> PageBits])
            if (e.start == start && e.key == key)
            {
                const Entry found = e;
                for (uint32_t p = start >> PageBits, last = LastPage(start, found.length); p <= last; p++)
                {
                    std::vector<Entry>& b = pages[p];
                    for (size_t i = 0; i < b.size(); i++)
                        if (b[i].start == start && b[i].key == key && b[i].length == found.length)
                        {
                            b[i] = b.back();
                            b.pop_back();
                            break;
                        }
                }
                count--;
                return;
            }
        for (auto it = large.begin(); it != large.end(); ++it)
            if (it->start == start && it->key == key)
            {
                large.erase(it);
                return;
            }
    }
    void Clear()
    {
        for (std::vector<Entry>& b : pages)
            b.clear();
        large.clear();
        count = 0;
    }
    size_t Size() const { return count; }
    size_t LargeSize() const { return large.size(); }
    // Calls f(key) for each range overlapping [addr, addr + size).
    template <typename F>
    void ForOverlaps(uint32_t addr, uint32_t size, F&& f) const
    {
        const uint64_t end = uint64_t(addr) + size;
        const uint32_t first = addr >> PageBits, last = size ? uint32_t((end - 1) >> PageBits) : first;
        for (uint32_t p = first; p <= last; p++)
            for (const Entry& e : pages[p])
                // (a range on several pages is reported from the first page both it and the query cover)
                if (e.start < end && uint64_t(e.start) + e.length > addr && std::max(e.start >> PageBits, first) == p)
                    f(e.key);
        for (const Entry& l : large)
            if (l.start < end && uint64_t(l.start) + l.length > addr)
                f(l.key);
    }

private:
    // Ranges are listed on every 64 KB page they cover, so a lookup reads only the pages of the query: no tree walk.
    // (std::multimaps by size class, ~90,000 buffer ranges in level 2, cost about 1 us of dependent cache misses per
    // GX2Invalidate, some 570 a frame.) The few ranges over 16 MB are scanned in full.
    static constexpr uint32_t PageBits = 16, LargeLimit = 16u << 20;
    struct Entry
    {
        uint32_t start, length;
        Key key;
    };
    static uint32_t LastPage(uint32_t start, uint32_t length) { return uint32_t((uint64_t(start) + length - 1) >> PageBits); }
    std::vector<std::vector<Entry>> pages = std::vector<std::vector<Entry>>(size_t(1) << (32 - PageBits));
    std::vector<Entry> large;
    size_t count = 0;
};
RangeIndex<uint64_t> gSurfaceRanges;  // surface keys by image and mip range
uint64_t gWriteSerial = 0;
constexpr size_t MaxAliases = 8;
// DT_LOG_COPIES (environment): logs texture copies, block moves, reverts to guest memory and retired GPU-written
// surfaces (texture streaming diagnostics).
const bool gLogCopies = getenv("DT_LOG_COPIES") != nullptr;
// Images and views retired while the GPU may still use them, destroyed a few frames later.
struct Retired
{
    uint64_t frame;
    Image image, snapshot;
    std::vector<VkImageView> views;
    Image upload;
};
std::vector<Retired> gRetired;
std::unordered_map<uint64_t, ShaderModule> gShaders;
// A map from small integer keys (attribute buffer numbers) kept in a fixed array in key order: per-draw bindings
// without heap allocation. Iterates as std::pair<uint32_t, V>, like std::map.
template <typename V, size_t Capacity = 16>
class SmallMap
{
public:
    V& operator[](uint32_t key)
    {
        size_t i = 0;
        while (i < count && items[i].first < key)
            i++;
        if (i < count && items[i].first == key)
            return items[i].second;
        if (count == Capacity)
            throw std::length_error("SmallMap full");
        for (size_t j = count; j > i; j--)
            items[j] = items[j - 1];
        count++;
        items[i] = { key, V{} };
        return items[i].second;
    }
    const V* find(uint32_t key) const
    {
        for (size_t i = 0; i < count; i++)
            if (items[i].first == key)
                return &items[i].second;
        return nullptr;
    }
    void clear() { count = 0; }
    size_t size() const { return count; }
    std::pair<uint32_t, V>* begin() { return items.data(); }
    std::pair<uint32_t, V>* end() { return items.data() + count; }
    const std::pair<uint32_t, V>* begin() const { return items.data(); }
    const std::pair<uint32_t, V>* end() const { return items.data() + count; }

private:
    std::array<std::pair<uint32_t, V>, Capacity> items{};
    size_t count = 0;
};

// Pipelines by key: the draw's state as 32-bit words (PipelineKey), hashed; the words are kept to compare.
struct PipelineDesc;
struct PipelineEntry
{
    std::vector<uint32_t> key;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool pending = false;  // being compiled in the background (graphics.async_shaders)
    // Precompiled from a record: the description it was built from, compared once with what this build makes for the
    // key (a renderer change may build the same key differently; then it is compiled again).
    std::shared_ptr<const PipelineDesc> verify;
};
std::unordered_map<uint64_t, std::vector<PipelineEntry>> gPipelines;

// The pipeline key under construction (reused across draws, so it does not allocate).
struct PipelineKey
{
    std::vector<uint32_t> words;
    void Clear() { words.clear(); }
    void Add(uint32_t v) { words.push_back(v); }
    void Add64(uint64_t v) { words.push_back(uint32_t(v)), words.push_back(uint32_t(v >> 32)); }
    uint64_t Hash() const
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : words)
            h = (h ^ w) * 1099511628211ull;
        return h;
    }
};

// The entry for `key` (its pipeline null if not made yet).
PipelineEntry& FindPipeline(const PipelineKey& key)
{
    std::vector<PipelineEntry>& bucket = gPipelines[key.Hash()];
    for (PipelineEntry& e : bucket)
        if (e.key == key.words)
            return e;
    bucket.push_back({ key.words });
    return bucket.back();
}
VkPipelineCache gPipelineCache = VK_NULL_HANDLE;  // persisted as <shader cache>/pipelines.bin
uint64_t gPipelinesUnsaved = 0;                   // pipelines created since the cache was last saved
std::set<std::string> gReported;  // one-time warnings

VkDescriptorSetLayout gSetLayout = VK_NULL_HANDLE;
VkPipelineLayout gPipelineLayout = VK_NULL_HANDLE;

// Everything a graphics pipeline is made from, as plain data: recorded in <shader cache>/pipelines.keys with the
// pipeline's key, so the next start can build every pipeline seen before (Precompile) instead of hitching on first use.
// Zero-filled before use, so the bytes (padding included) are deterministic.
struct PipelineDesc
{
    uint64_t vs, ps;  // shader hashes: <shader cache>/<hash>.spv
    uint32_t attrCount, bindingCount;
    VkVertexInputAttributeDescription attrs[32];
    VkVertexInputBindingDescription bindings[16];
    uint32_t divisors[16];  // per binding: instance-rate divisor above 1 (VK_KHR_vertex_attribute_divisor), else 0
    VkPrimitiveTopology topology;
    VkCullModeFlags cull;
    VkFrontFace frontFace;
    VkBool32 depthBias, depthClamp, depthTest, depthWrite, stencilTest;
    VkCompareOp depthCompare;
    VkStencilOpState stencilFront, stencilBack;
    uint32_t colorCount;
    VkFormat colorFormats[8];
    VkPipelineColorBlendAttachmentState blends[8];
    VkFormat depthFormat, stencilFormat;
};

VkPipeline CreatePipeline(const PipelineDesc& d, VkShaderModule vs, VkShaderModule ps)
{
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = d.bindingCount;
    vi.pVertexBindingDescriptions = d.bindings;
    vi.vertexAttributeDescriptionCount = d.attrCount;
    vi.pVertexAttributeDescriptions = d.attrs;
    VkVertexInputBindingDivisorDescriptionKHR divisors[16];
    VkPipelineVertexInputDivisorStateCreateInfoKHR divisorState{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR };
    for (uint32_t i = 0; i < d.bindingCount; i++)
        if (d.divisors[i] > 1)
            divisors[divisorState.vertexBindingDivisorCount++] = { d.bindings[i].binding, d.divisors[i] };
    if (divisorState.vertexBindingDivisorCount)
    {
        divisorState.pVertexBindingDivisors = divisors;
        vi.pNext = &divisorState;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d.topology;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = d.frontFace;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = d.depthBias;
    rs.depthClampEnable = d.depthClamp;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = d.depthTest;
    dss.depthWriteEnable = d.depthWrite;
    dss.depthCompareOp = d.depthCompare;
    dss.stencilTestEnable = d.stencilTest;
    dss.front = d.stencilFront;
    dss.back = d.stencilBack;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = d.blends;
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = uint32_t(std::size(dyn));
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = d.colorCount;
    ri.pColorAttachmentFormats = d.colorFormats;
    ri.depthAttachmentFormat = d.depthFormat;
    ri.stencilAttachmentFormat = d.stencilFormat;
    VkGraphicsPipelineCreateInfo gci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.pNext = &ri;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &dss;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &ds;
    gci.layout = gPipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(GetDevice().Handle(), gPipelineCache, 1, &gci, nullptr, &pipeline));
    return pipeline;
}

// <shader cache>/pipelines.keys: a header, then per pipeline its key's word count, the words and a PipelineDesc. Kept
// across builds (outside the versioned folder), like programs.bin: a later record for a key replaces an earlier one.
constexpr char PipelineKeysMagic[8] = { 'D', 'T', 'P', 'K', 'E', 'Y', 'S', '2' };
std::mutex gPipelineKeysMutex;
std::string gBaseCacheDir;  // --shader-cache itself; gCacheDir is this build's folder in it
std::string PipelineKeysPath() { return gBaseCacheDir.empty() ? "" : gBaseCacheDir + "/pipelines.keys"; }

void RecordPipeline(const std::vector<uint32_t>& key, const PipelineDesc& d)
{
    const std::string path = PipelineKeysPath();
    if (path.empty())
        return;
    std::lock_guard lock(gPipelineKeysMutex);
    FILE* f = fopen(path.c_str(), "ab");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);  // in append mode the position can read 0 until the first write (Windows' CRT)
    if (ftell(f) == 0)
    {
        const uint32_t descSize = sizeof(PipelineDesc);
        fwrite(PipelineKeysMagic, 1, sizeof(PipelineKeysMagic), f);
        fwrite(&descSize, 4, 1, f);
    }
    const uint32_t n = uint32_t(key.size());
    fwrite(&n, 4, 1, f);
    fwrite(key.data(), 4, n, f);
    fwrite(&d, sizeof(d), 1, f);
    fclose(f);
}
VkDescriptorPool gPools[2] = {};
uint64_t gPoolFrame = ~0ull;
Image gDummy[4];  // per shader::TexDim: 2D, 3D, cube, 2D array
Image gScan;
VkBuffer gScratch = VK_NULL_HANDLE;  // device-local staging for image copies through memory (EnsureScratch)
VkDeviceMemory gScratchMemory = VK_NULL_HANDLE;
VkDeviceSize gScratchSize = 0;

// The open dynamic-rendering pass and its attachments.
struct PassTarget
{
    GpuSurface* surface;
    uint32_t mip, layer;
};
bool gRendering = false;
// The open rendering pass's attachments: color views 0-7, then depth (all null when no pass is open).
std::array<uint64_t, 9> gPassKey{};
std::vector<PassTarget> gPassTargets;

// --capture-frame: every pass of one frame saved as PNGs, with a log of its draws.
uint64_t gCaptureFrame = ~0ull;           // the frame being captured (or the first, before)
std::set<uint64_t> gCaptureFrames;        // --capture-frame (repeatable)
std::string gCaptureBase;                 // --capture-dir; with several frames, each goes to <dir>/f<frame>
std::string gCaptureDir;
bool gCapturing = false;
uint32_t gCapturePass = 0;
void CapturePass(const std::vector<PassTarget>& targets);
std::set<std::pair<const void*, uint64_t>> gCapturedTextures;  // (surface, write serial) already saved
std::set<uint64_t> gCapturedShaders;
void CaptureTexture(GpuSurface& g, const Texture& t);

// --gpu-stats: counters logged every StatsInterval frames.
struct Stats
{
    uint64_t draws = 0, pipelines = 0, passes = 0, uploads = 0, uploadBytes = 0, snapshots = 0, recreated = 0;
    uint64_t overwritten = 0;  // GPU-written surfaces whose guest memory the CPU overwrote
    uint64_t guestCopies = 0;  // GX2CopySurface copies also made in guest memory (CopyInGuest)
    uint64_t keptLevels = 0;  // GPU-written surfaces that kept GPU-copied levels over a CPU write (KeepGpuLevels)
    uint64_t blockCopies = 0;  // compressed textures filled from block copies (BlocksAsTexture)
    uint64_t shaderModules = 0;  // shader modules made (read from the disk cache or translated), in total
    uint64_t asyncSkips = 0;     // draws skipped while their shaders or pipeline compiled in the background
    uint64_t asyncShaders = 0, asyncPipelines = 0, staleRecords = 0;  // in total: background compiles; precompiled
                                                                       // pipelines whose description had changed
    double tCompile = 0;         // translating shaders, making shader modules and pipelines (in total, not reset)
    uint64_t cacheTrims = 0, uploadBytesTotal = 0;  // buffer-cache flushes; texture upload bytes (in total, not reset)
    uint64_t cacheEvictions = 0;                     // buffer-cache blocks evicted (least recently used)
    uint64_t relocated = 0;                          // GPU-written surfaces moved with the texture pool (in total)
    double tCreate = 0, tDetile = 0;  // making surface images; detiling texture data for upload (in total, not reset)
    std::map<uint64_t, uint64_t> snapshotsBySurface;  // (address << 32 | is depth) -> snapshots taken
    uint64_t invalidates = 0;  // GX2Invalidate calls
    uint64_t invalidateHits = 0, invalidateMaxSize = 0;  // cached buffers and surfaces they touched; largest range
    double tInvalidate = 0;    // time in them
    uint64_t bufferUploads = 0;  // vertex/index buffers converted into the cache
    // CPU time per phase of a draw, and in Present (which waits for the GPU), in seconds.
    uint64_t rebuildsDeferred = 0, rebuildBytes = 0, perDrawBytes = 0, vtxBytes = 0, rebuilds = 0, perDraws = 0;  // buffer paths (slow-frame lines)
    double tSetup = 0, tShaders = 0, tTextures = 0, tBuffers = 0, tPipeline = 0, tDescriptors = 0, tRecord = 0, tPresent = 0;
} gStats;

// Adds the time since the last Lap() (or construction) to a counter.
struct PhaseTimer
{
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    void Lap(double& counter)
    {
        const auto now = std::chrono::steady_clock::now();
        counter += std::chrono::duration<double>(now - t).count();
        t = now;
    }
};
bool gStatsOn = false;
FILE* gFrameCsv = nullptr;  // --frame-csv: one row per frame


double gLastPipelineTime = 0, gLastShaderTime = 0;  // phase times at the last swap, for slow-frame reports
constexpr uint64_t StatsInterval = 300;
std::chrono::steady_clock::time_point gStatsStart;

std::string gShotDir;
bool gShotWindow = false;  // --screenshot-window: the presented image, overlay included
bool gNoCull = false;  // --no-cull: draw both faces of everything (debugging)
std::set<uint64_t> gShotFrames;
std::atomic<int> gShotRequests{ 0 };  // RequestScreenshot: the next frames presented
uint64_t gShotEvery = 0;

std::string Hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%X", v);
    return buf;
}

void ReportOnce(const std::string& msg)
{
    if (gReported.insert(msg).second)
        dtlog::Important("[gpu] %s\n", msg.c_str());
}

Device& Dev() { return GetDevice(); }

// ---- occlusion queries ----
// The game issues about 130 a frame (bounding boxes of primitives) and reads each in the next frame, waiting for it
// (RHIGetOcclusionQueryResult); hidden primitives are then not drawn. Reporting everything visible (as before) cost
// the game its occlusion culling: level 2's town drew 1,900 draws where far fewer were visible. A GX2 query becomes a
// Vulkan occlusion query around its draws (a piece per render pass, summed); the work up to the end of a run of
// queries is submitted early (Device::SubmitNoWait), so the GPU has run them by the time the game asks.
constexpr uint32_t QueriesPerSlot = 2048;
VkQueryPool gQueryPool = VK_NULL_HANDLE;
struct OpenQuery
{
    bool active = false, begun = false, forceVisible = false;
    uint32_t addr = 0;
    uint64_t issue = 0;
    std::vector<uint32_t> indices;
};
OpenQuery gOpenQuery;
struct PendingQuery
{
    uint32_t addr;
    uint64_t issue;
    std::vector<uint32_t> indices;
    uint64_t epoch;  // the command buffer it was recorded in (submitted once CommandEpoch() has moved on)
    uint32_t slot;
    bool forceVisible;
};
std::vector<PendingQuery> gPendingQueries;
uint64_t gQueryFrame = ~0ull;
uint32_t gQueryNext = 0;
uint32_t gDrawsSinceQueryEnd = 0;
std::mutex gQueryResultsMutex;
std::condition_variable gQueryResultsChanged;
std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> gQueryResults;  // GX2 query -> (issue, samples)
struct QueryStats
{
    uint64_t resolved = 0, hidden = 0, early = 0, forced = 0;
} gQueryStats;

void StoreQueryResult(uint32_t addr, uint64_t issue, uint64_t samples)
{
    std::lock_guard lock(gQueryResultsMutex);
    gQueryResults[addr] = { issue, samples };
}

// Read pending queries' results: all of a reused slot's, or every one (`wait`: until the GPU has counted them; else
// only those already counted, without blocking, and only from command buffers already submitted).
void ResolvePending(bool onlyReusedSlot, uint32_t slot, bool wait = true)
{
    const uint64_t epoch = Dev().CommandEpoch();
    bool stored = false;
    for (auto it = gPendingQueries.begin(); it != gPendingQueries.end();)
    {
        if ((onlyReusedSlot && it->slot != slot) || (!wait && it->epoch == epoch))
        {
            ++it;
            continue;
        }
        uint64_t samples = 0;
        bool ready = true;
        for (uint32_t index : it->indices)
        {
            uint64_t v = 0;
            const VkResult r = vkGetQueryPoolResults(Dev().Handle(), gQueryPool, index, 1, sizeof(v), &v, sizeof(v),
                VK_QUERY_RESULT_64_BIT | (wait ? VK_QUERY_RESULT_WAIT_BIT : 0));
            if (r == VK_NOT_READY)
            {
                ready = false;
                break;
            }
            samples += v;
        }
        if (!ready)
        {
            ++it;
            continue;
        }
        // (counted at the internal resolution: the game's thresholds are in its own pixels)
        const double area = double(gRenderScale) * gRenderScale;
        uint64_t reported = samples ? std::max<uint64_t>(1, uint64_t(double(samples) / area + 0.5)) : 0;
        if (it->forceVisible)
            reported = std::max<uint64_t>(reported, 1000), gQueryStats.forced++;
        gQueryStats.resolved++;
        gQueryStats.hidden += reported == 0;
        StoreQueryResult(it->addr, it->issue, reported);
        stored = true;
        it = gPendingQueries.erase(it);
    }
    if (stored)
        gQueryResultsChanged.notify_all();
}

// Submit what has been recorded so far if it holds ended queries (the GPU starts on them now).
void SubmitQueriesEarly()
{
    const uint64_t epoch = Dev().CommandEpoch();
    if (std::none_of(gPendingQueries.begin(), gPendingQueries.end(), [epoch](const PendingQuery& q) { return q.epoch == epoch; }))
        return;
    Dev().SubmitNoWait();  // (EndPass runs first: SetBeforeSubmit)
    gQueryStats.early++;
}

// Inside a render pass, before a draw: start the open query's piece for this pass.
void QueryBeforeDraw(VkCommandBuffer cmd)
{
    if (!gOpenQuery.active || gOpenQuery.begun || !gQueryPool)
        return;
    if (gQueryNext >= QueriesPerSlot)
    {
        gOpenQuery.forceVisible = true;
        return;
    }
    const uint32_t index = Dev().Slot() * QueriesPerSlot + gQueryNext++;
    vkCmdBeginQuery(cmd, gQueryPool, index, Dev().PreciseOcclusion() ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    gOpenQuery.begun = true;
    gOpenQuery.indices.push_back(index);
}

void QueryEndPiece()
{
    if (gOpenQuery.begun)
        vkCmdEndQuery(Dev().Cmd(), gQueryPool, gOpenQuery.indices.back()), gOpenQuery.begun = false;
}

void EndPass()
{
    if (gRendering)
    {
        QueryEndPiece();
        vkCmdEndRendering(Dev().Cmd());
        gRendering = false;
        gPassKey = {};
        const std::vector<PassTarget> targets = std::move(gPassTargets);
        gPassTargets.clear();
        if (gCapturing)
            CapturePass(targets);
    }
}

// Move an image to a layout outside a rendering pass (a no-op when it is already there).
void EnsureLayout(Image& img, VkImageLayout layout)
{
    if (img.layout == layout)
        return;
    EndPass();
    Dev().Transition(Dev().Cmd(), img, layout);
}

// ---------------------------------------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------------------------------------
// GX2 depth formats. Depth is always stored as 32-bit float (with 8-bit stencil where the format has stencil):
// every GPU supports it, it holds 16- and 24-bit depth exactly enough, and depth copied into a texture is then
// plain floats.
std::optional<std::pair<VkFormat, bool>> DepthFormat(uint32_t format)
{
    switch (format)
    {
    case 0x005: return std::pair{ VK_FORMAT_D32_SFLOAT, false };          // UNORM_R16 (D16)
    case 0x80E: return std::pair{ VK_FORMAT_D32_SFLOAT, false };          // FLOAT_R32 (D32F)
    case 0x011: return std::pair{ VK_FORMAT_D32_SFLOAT_S8_UINT, true };   // UNORM_R24_X8 (D24S8)
    case 0x811: return std::pair{ VK_FORMAT_D32_SFLOAT_S8_UINT, true };   // FLOAT_D24_S8
    case 0x81C: return std::pair{ VK_FORMAT_D32_SFLOAT_S8_UINT, true };   // FLOAT_X8_X24 (D32F S8)
    default: return std::nullopt;
    }
}

uint32_t SurfaceLayers(const Surface& s)
{
    switch (s.dim)
    {
    case surface::DimCube: return std::max(6u, s.depth);
    case surface::Dim1DArray: case surface::Dim2DArray: case surface::Dim2DMsaaArray: return std::max(1u, s.depth);
    default: return 1;
    }
}

void RetireSurface(uint64_t key)
{
    auto it = gSurfaces.find(key);
    if (it == gSurfaces.end())
        return;
    GpuSurface& g = it->second;
    if (gLogCopies && g.gpuOwned)
        dtlog::Important("[copies] f%llu retire owned %08X %ux%u vk%d levels %u gpuLevels %X\n",
            (unsigned long long)Dev().FrameIndex(), g.addr, g.guestWidth, g.guestHeight, int(g.format), g.levels, g.gpuLevels);
    SetOwned(g, false);
    gSurfaceRanges.Remove(g.indexedImage, key);
    gSurfaceRanges.Remove(g.indexedMip, key);
    Retired r{ Dev().FrameIndex(), g.image, g.snapshot, {}, g.upload };
    for (auto* views : { &g.views, &g.snapshotViews })
        for (auto& [k, v] : views->map)
            r.views.push_back(v);
    gRetired.push_back(std::move(r));
    gSurfaces.erase(it);
}

// Destroy retired images once the frames that could use them have finished (two frames in flight).
void FreeRetired()
{
    const uint64_t frame = Dev().FrameIndex();
    for (auto it = gRetired.begin(); it != gRetired.end();)
    {
        if (it->frame + 3 > frame)
        {
            ++it;
            continue;
        }
        for (VkImageView v : it->views)
            vkDestroyImageView(Dev().Handle(), v, nullptr);
        Dev().DestroyImage(it->image);
        Dev().DestroyImage(it->snapshot);
        Dev().DestroyImage(it->upload);
        it = gRetired.erase(it);
    }
}

uint64_t SampleHash(uint32_t addr, uint32_t size);

// A fingerprint of the guest memory behind a surface.
uint64_t GuestSample(const GpuSurface& g)
{
    return SampleHash(g.addr, g.imageSize) ^ SampleHash(g.mipAddr, g.mipSize) * 31;
}

// Whether to fingerprint a surface's guest memory this frame (StillGpuOwned, Upload). Each fingerprint reads 128
// scattered words (about 10 us of cache misses) and every texture in use was checked every frame: about 1 ms a frame
// in level 2. GX2Invalidate reports CPU writes at once; this sampling is the fallback, so surfaces unchanged for 30
// frames are checked every 4th frame, staggered by address.
bool CheckDue(const GpuSurface& g)
{
    const uint64_t frame = Dev().FrameIndex();
    return frame - g.lastChange < 30 || ((frame + (g.addr >> 12)) & 3) == 0;
}

// LatestWritten's answers, reused until something they depend on changes: a GPU write to that address (MarkWritten
// forgets its entry), an ownership or alias change (gOwnedGen), a new frame (StillGpuOwned checks once a frame). It
// runs twice per texture per draw; every draw writes some target, so a global write counter forgot everything.
struct LatestMemo
{
    uint32_t addr = 0;
    bool depthOnly = false;
    uint64_t gen = 0, frame = ~0ull;
    GpuSurface* result = nullptr;
};
LatestMemo gLatestMemo[4096];
inline LatestMemo& LatestMemoFor(uint32_t addr) { return gLatestMemo[(addr >> 8) & 4095]; }

constexpr uint32_t AllLevels = ~0u;

// A GPU write into the surface: all of it, or one level of it (`level`, described by `s`).
void MarkWritten(GpuSurface& g, int level = -1, const Surface* s = nullptr)
{
    LatestMemoFor(g.addr).addr = 0;
    // Fingerprint the guest memory at the first GPU write of a frame (later writes in the frame leave it alone).
    if (!g.gpuOwned || g.ownedCheckedFrame != Dev().FrameIndex())
        g.ownedSample = GuestSample(g), g.ownedCheckedFrame = Dev().FrameIndex();
    if (!g.gpuOwned)
        g.gpuLevels = g.cpuLevels = 0;
    if (level < 0 || level >= 14 || !s)
        g.gpuLevels = AllLevels, g.cpuLevels = 0, g.mirrored = false;
    else
    {
        const uint32_t bit = 1u << level;
        g.levelAddr[level] = surface::LevelAddress(*s, level);
        g.levelBytes[level] = uint32_t(std::min<uint64_t>(surface::ComputeLevel(*s, level).size, 0xFFFFFFFFu));
        g.levelSample[level] = SampleHash(g.levelAddr[level], g.levelBytes[level]);
        g.gpuLevels |= bit, g.cpuLevels &= ~bit;
    }
    SetOwned(g, true);
    g.cpuDirty = false, g.lastWrite = ++gWriteSerial, g.lastChange = Dev().FrameIndex();
}

// The CPU wrote some of a GPU-written surface's memory. If the GPU wrote only some levels and their memory is
// unchanged, those keep the GPU's data and the rest are marked for upload from guest memory (true); otherwise the
// caller reverts the whole surface to guest memory.
bool KeepGpuLevels(GpuSurface& g)
{
    if (g.gpuLevels == AllLevels)
        return false;
    uint32_t keep = 0;
    for (uint32_t l = 0; l < std::min(g.levels, 14u); l++)
        if ((g.gpuLevels >> l & 1) && SampleHash(g.levelAddr[l], g.levelBytes[l]) == g.levelSample[l])
            keep |= 1u << l;
    if (!keep)
        return false;
    g.gpuLevels = keep;
    g.cpuLevels = ((1u << g.levels) - 1) & ~keep;
    g.ownedSample = GuestSample(g), g.lastChange = Dev().FrameIndex();
    gStats.keptLevels++;
    return true;
}

// Whether a GPU-written surface still holds the address's contents: once per frame, check that the CPU has not
// overwritten its guest memory since. If it has, the guest data is newer and the surface reverts to it.
bool StillGpuOwned(GpuSurface& g)
{
    if (!g.gpuOwned || g.isDepth || g.ownedCheckedFrame == Dev().FrameIndex())
        return g.gpuOwned;
    g.ownedCheckedFrame = Dev().FrameIndex();
    if (!CheckDue(g) || GuestSample(g) == g.ownedSample)
        return true;
    gStats.overwritten++;
    if (KeepGpuLevels(g))
        return true;
    if (gLogCopies)
        dtlog::Important("[copies] f%llu revert (fingerprint) %08X %ux%u vk%d levels %u gpuLevels %X\n",
            (unsigned long long)Dev().FrameIndex(), g.addr, g.guestWidth, g.guestHeight, int(g.format), g.levels, g.gpuLevels);
    SetOwned(g, false);
    g.cpuDirty = true;
    return false;
}

// The alias of an address the GPU wrote last, if any: it holds the surface's current contents.

GpuSurface* LatestWrittenUncached(uint32_t addr, bool depthOnly);

GpuSurface* LatestWritten(uint32_t addr, bool depthOnly = false)
{
    const uint64_t frame = Dev().FrameIndex();
    LatestMemo& m = LatestMemoFor(addr);
    if (m.addr == addr && m.depthOnly == depthOnly && m.gen == gOwnedGen && m.frame == frame)
    {
        if (m.result)
            m.result->lastUse = frame;
        return m.result;
    }
    GpuSurface* result = LatestWrittenUncached(addr, depthOnly);
    m = { addr, depthOnly, gOwnedGen, frame, result };
    return result;
}

GpuSurface* LatestWrittenUncached(uint32_t addr, bool depthOnly)
{
    if (!gOwnedAt.count(addr))
        return nullptr;
    auto it = gAtAddr.find(addr);
    if (it == gAtAddr.end())
        return nullptr;
    GpuSurface* best = nullptr;
    for (const Alias& a : it->second)
    {
        GpuSurface& g = *a.g;
        if (StillGpuOwned(g) && (!depthOnly || g.isDepth) && (!best || g.lastWrite > best->lastWrite))
            best = &g;
    }
    if (best)
        best->lastUse = Dev().FrameIndex();
    return best;
}

// The GPU image for a guest surface as described (each distinct description of an address is its own alias).
// `asDepth` asks for a depth image (depth buffers); otherwise the color format of the surface is used.
// The key of a surface description in gSurfaces: its address, the game's size, layers or depth, levels, Vulkan format,
// and whether it is 3D or a cube.
uint64_t SurfaceKey(uint32_t addr, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, VkFormat format,
    bool is3D, bool cube)
{
    uint64_t key = 1469598103934665603ull;
    for (uint64_t f : { uint64_t(addr), uint64_t(width), uint64_t(height), uint64_t(depth), uint64_t(levels), uint64_t(format),
             uint64_t(is3D), uint64_t(cube) })
        key = (key ^ f) * 1099511628211ull;
    return key;
}

// `scale`: the internal resolution scale to make the image at if it is new and one of the game's TV-derived render
// targets (Scalable); render targets pass gRenderScale, copies their source's.
GpuSurface* GetSurface(const Surface& s, bool asDepth, float scale = 1.0f, bool target = false)
{
    VkFormat format;
    bool stencil = false;
    if (asDepth)
    {
        const auto d = DepthFormat(s.format & ~0x400u);
        if (!d)
        {
            ReportOnce("unsupported depth buffer format 0x" + Hex(s.format));
            return nullptr;
        }
        format = d->first, stencil = d->second;
    }
    else
    {
        const auto info = SurfaceFormat(s.format);
        if (!info)
        {
            ReportOnce("unsupported surface format 0x" + Hex(s.format));
            return nullptr;
        }
        format = info->format;
    }
    if (s.aa)
        ReportOnce("multisampled surfaces are drawn single-sampled");
    const bool is3D = s.dim == surface::Dim3D;
    const uint32_t width = std::max(1u, s.width), height = std::max(1u, s.height);
    const uint32_t depth = is3D ? std::max(1u, s.depth) : SurfaceLayers(s);
    // Games sometimes declare more levels than the full chain has; Vulkan does not allow that.
    const uint32_t fullChain = std::bit_width(std::max({ width, height, is3D ? depth : 1u }));
    const uint32_t levels = std::clamp(s.mipLevels, 1u, std::min(14u, fullChain));
    const uint64_t key = SurfaceKey(s.image, width, height, depth, levels, format, is3D, s.dim == surface::DimCube);
    auto [it, created] = gSurfaces.try_emplace(key);
    GpuSurface& g = it->second;
    if (created)
    {
        std::vector<Alias>& aliases = gAtAddr[s.image];
        if (aliases.size() >= MaxAliases)
        {
            // Retire the least recently used alias, unless every alias is in use this frame (the draw being set
            // up may hold any of them).
            auto lru = std::min_element(aliases.begin(), aliases.end(),
                [](const Alias& a, const Alias& b) { return a.g->lastUse < b.g->lastUse; });
            if (lru->g->lastUse < Dev().FrameIndex())
            {
                RetireSurface(lru->key);
                aliases.erase(lru), gOwnedGen++;
            }
        }
        aliases.push_back({ key, &g }), gOwnedGen++;
        gStats.recreated++;
    }
    g.lastUse = Dev().FrameIndex();
    if (!g.image.image)
    {
        float sc = scale != 1.0f && levels == 1 && !is3D && s.dim != surface::DimCube && Scalable(width, height) ? scale : 1.0f;
        if (target && gShadowScale != 1.0f && ShadowScalable(width, height, format, asDepth, levels, is3D, s.dim == surface::DimCube))
            sc = gShadowScale, g.shadow = true;
        const uint32_t imageW = std::max(1u, uint32_t(std::lround(width * sc))), imageH = std::max(1u, uint32_t(std::lround(height * sc)));
        ImageDesc d;
        d.width = imageW, d.height = imageH, d.mipLevels = levels, d.format = format, d.createView = false;
        if (is3D)
            d.depth = depth;
        else
            d.layers = depth;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkFormatFeatureFlags features = Dev().FormatFeatures(format);
        if (asDepth)
            d.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else if (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            d.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!asDepth && SrgbVariant(format) != format)
            d.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (s.dim == surface::DimCube && width == height && depth % 6 == 0)
            d.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        const auto createStart = std::chrono::steady_clock::now();
        g.image = Dev().CreateImage(d);
        gStats.tCreate += std::chrono::duration<double>(std::chrono::steady_clock::now() - createStart).count();
        g.addr = s.image, g.width = imageW, g.height = imageH, g.depth = depth, g.levels = levels, g.format = format;
        g.guestWidth = width, g.guestHeight = height, g.scale = sc;
        g.isDepth = asDepth, g.is3D = is3D, g.cube = (d.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
        (void)stencil;
    }
    g.dim = s.dim;
    g.desc = s;
    g.imageSize = s.imageSize;
    g.mipAddr = levels > 1 ? surface::LevelAddress(s, 1) : 0;
    g.mipSize = levels > 1 ? s.mipmapSize : 0;
    if (g.indexedImage != g.addr || g.indexedImageSize != g.imageSize || g.indexedMip != g.mipAddr ||
        g.indexedMipSize != g.mipSize)
    {
        gSurfaceRanges.Remove(g.indexedImage, key);
        gSurfaceRanges.Remove(g.indexedMip, key);
        gSurfaceRanges.Add(g.addr, g.imageSize, key);
        gSurfaceRanges.Add(g.mipAddr, g.mipSize, key);
        g.indexedImage = g.addr, g.indexedImageSize = g.imageSize, g.indexedMip = g.mipAddr, g.indexedMipSize = g.mipSize;
    }
    return &g;
}

VkImageView View(Image& image, ViewCache& cache, VkImageViewType type, VkFormat format,
    uint32_t baseMip, uint32_t mips, uint32_t baseLayer, uint32_t layers, VkComponentMapping swizzle, VkImageAspectFlags aspect)
{
    const uint32_t fields[] = { uint32_t(type), uint32_t(format), baseMip, mips, baseLayer, layers, uint32_t(swizzle.r),
        uint32_t(swizzle.g), uint32_t(swizzle.b), uint32_t(swizzle.a), aspect };
    uint64_t key = 1469598103934665603ull;
    for (uint32_t f : fields)
        key = (key ^ f) * 1099511628211ull;
    if (cache.views[0] && cache.keys[0] == key)
        return cache.views[0];
    if (cache.views[1] && cache.keys[1] == key)
    {
        std::swap(cache.keys[0], cache.keys[1]), std::swap(cache.views[0], cache.views[1]);
        return cache.views[0];
    }
    VkImageView& v = cache.map[key];
    if (!v)
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = image.image;
        vci.viewType = type;
        vci.format = format;
        vci.components = swizzle;
        vci.subresourceRange = { aspect, baseMip, mips, baseLayer, layers };
        VK_CHECK(vkCreateImageView(Dev().Handle(), &vci, nullptr, &v));
    }
    cache.keys[1] = cache.keys[0], cache.views[1] = cache.views[0];
    cache.keys[0] = key, cache.views[0] = v;
    return v;
}

// A cheap fingerprint of guest texture memory: 64 words spread over the data.
uint64_t SampleHash(uint32_t addr, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    if (!addr || size < 4)
        return h;
    for (uint32_t i = 0; i < 64; i++)
        h = (h ^ guest::Read32(addr + (uint64_t(size - 4) * i / 63 & ~3u))) * 1099511628211ull;
    return h;
}

// Copy CPU-written guest data into the image when it changed (and the GPU has not rendered into it since).
void GpuMarkHere(const char* kind, uint32_t addr = 0, uint32_t width = 0, uint32_t height = 0);

// `recheck`: also fingerprint the guest memory for CPU writes the game did not report (GX2Invalidate). Copies
// pass false: they run on the GPU thread up to a frame after the game issued them, when the source's memory may
// already hold what the game put there next (a texture streamed into the freed allocation); the invalidates that
// report such writes are queued in order with the copies.
// `forceLevels`: (copy sources) upload these levels from guest memory now whatever the checks say: the game's
// texture loads go through reused staging memory whose next contents the checks may not have seen yet.
void Upload(GpuSurface& g, const Surface& described, bool recheck = true, uint32_t forceLevels = 0)
{
    if (g.isDepth)
        return;
    // A GPU-written surface uploads only the levels the CPU rewrote since (KeepGpuLevels), as it was made.
    uint32_t levelMask = AllLevels;
    bool forced = false;
    if (g.gpuOwned)
    {
        if (!g.cpuLevels)
            return;
        levelMask = g.cpuLevels, g.cpuLevels = 0;
    }
    else if (forceLevels && !g.cpuDirty)
        levelMask = forceLevels, forced = true;
    const Surface& s = g.gpuOwned ? g.desc : described;
    if (g.gpuOwned || forced)
        g.cpuDirty = true;
    bool sampled = g.gpuOwned || forced;
    if (!sampled && recheck && g.checkedFrame != Dev().FrameIndex())
    {
        g.checkedFrame = Dev().FrameIndex();
        if (CheckDue(g))
        {
            const uint64_t sample = SampleHash(s.image, s.imageSize) ^ SampleHash(g.mipAddr, g.mipSize) * 31;
            sampled = true;
            if (sample != g.sample)
                g.cpuDirty = true, g.sample = sample;
        }
    }
    if (!g.cpuDirty)
        return;
    if (!sampled)  // (the fingerprint of what is uploaded now, for later checks)
        g.sample = SampleHash(s.image, s.imageSize) ^ SampleHash(g.mipAddr, g.mipSize) * 31;
    g.lastChange = Dev().FrameIndex();
    GpuMarkHere("upload", g.addr, g.width, g.height);
    const auto info = SurfaceFormat(s.format);
    // A render target made at a higher resolution receives CPU data (one level) at the game's size in `upload`,
    // which is then scaled into the image.
    const bool scaled = g.scale != 1.0f;
    if (scaled && !g.upload.image)
    {
        ImageDesc d;
        d.width = g.guestWidth, d.height = g.guestHeight, d.format = g.format, d.createView = false;
        d.layers = g.depth;
        d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        g.upload = Dev().CreateImage(d);
    }
    Image& target = scaled ? g.upload : g.image;
    EnsureLayout(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkCommandBuffer cmd = Dev().Cmd();
    std::vector<uint8_t> linear;
    for (uint32_t level = 0; level < g.levels; level++)
    {
        if (level < 32 && !(levelMask >> level & 1))
            continue;
        const auto detileStart = std::chrono::steady_clock::now();
        surface::Detile(s, level, linear);
        gStats.tDetile += std::chrono::duration<double>(std::chrono::steady_clock::now() - detileStart).count();
        gStats.uploads++, gStats.uploadBytes += linear.size(), gStats.uploadBytesTotal += linear.size();
        auto up = Dev().Allocate(linear.size(), 16);
        memcpy(up.ptr, linear.data(), linear.size());
        const uint32_t w = std::max(1u, (scaled ? g.guestWidth : g.width) >> level);
        const uint32_t h = std::max(1u, (scaled ? g.guestHeight : g.height) >> level);
        VkBufferImageCopy copy{};
        copy.bufferOffset = up.offset;
        // Rows are tightly packed elements; for BCn the row length is in texels (whole blocks).
        copy.bufferRowLength = (w + info->blockSize - 1) / info->blockSize * info->blockSize;
        copy.bufferImageHeight = (h + info->blockSize - 1) / info->blockSize * info->blockSize;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, g.is3D ? 1 : g.depth };
        copy.imageExtent = { w, h, g.is3D ? std::max(1u, g.depth >> level) : 1 };
        vkCmdCopyBufferToImage(cmd, up.buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    if (scaled)
    {
        EnsureLayout(g.upload, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        EnsureLayout(g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const bool linearFilter = VkNumberType(g.format) == 0 &&
            (Dev().FormatFeatures(g.format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        VkImageBlit bl{};
        bl.srcSubresource = bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, g.depth };
        bl.srcOffsets[1] = { int32_t(g.guestWidth), int32_t(g.guestHeight), 1 };
        bl.dstOffsets[1] = { int32_t(g.width), int32_t(g.height), 1 };
        vkCmdBlitImage(cmd, g.upload.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    // Order the upload before whatever writes the image next (a copy into it, say) even in the same layout.
    Dev().Transition(cmd, g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g.cpuDirty = false;
}

// --gpu-stats GPU timing: per frame-in-flight slot, timestamps at the frame's start (0) and end (1), pairs around each
// snapshot copy, and marks before each render pass and transfer (what follows a mark, up to the next one, is its
// segment); read when the slot is recorded again (its previous work has finished). Frames whose GPU time passes 40 ms
// log their costliest segments.
VkQueryPool gTimestamps = VK_NULL_HANDLE;
constexpr uint32_t TimestampsPerSlot = 2 + 2 * 1024 + 4096;
struct GpuMark
{
    uint32_t index;
    const char* kind;
    uint32_t addr, width, height, draws;
    uint32_t vs = 0, ps = 0, count = 0, instances = 0;  // the segment's last draw
};
struct TimestampSlot
{
    uint32_t used = 0;
    bool active = false;
    uint64_t frame = 0;
    std::vector<uint32_t> pairs;  // first index of each snapshot pair
    std::vector<GpuMark> marks;
};
std::array<TimestampSlot, 4> gTimestampSlots;
double gGpuFrameMs = 0, gGpuSnapshotMs = 0;  // summed over frames read since the last stats line
uint64_t gGpuFrames = 0;
double gGpuLastMs = 0;  // the GPU time of the latest frame read (two frames back), for --frame-csv

void BeginGpuTiming()
{
    if (!gStatsOn)
        return;
    VkCommandBuffer cmd = Dev().Cmd();
    if (!gTimestamps)
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = TimestampsPerSlot * Device::SlotCount();
        VK_CHECK(vkCreateQueryPool(Dev().Handle(), &qi, nullptr, &gTimestamps));
    }
    const uint32_t slot = Dev().Slot(), base = slot * TimestampsPerSlot;
    TimestampSlot& ts = gTimestampSlots[slot];
    if (ts.active && ts.used >= 2)
    {
        std::vector<uint64_t> t(ts.used);
        if (vkGetQueryPoolResults(Dev().Handle(), gTimestamps, base, ts.used, t.size() * 8, t.data(), 8,
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
        {
            const double period = Dev().Properties().limits.timestampPeriod / 1e6;  // ms per tick
            gGpuLastMs = double(t[1] - t[0]) * period;
            gGpuFrameMs += gGpuLastMs;
            for (uint32_t i : ts.pairs)
                gGpuSnapshotMs += double(t[i + 1] - t[i]) * period;
            gGpuFrames++;
            if (gGpuLastMs > 40 && !ts.marks.empty())
            {
                struct Segment
                {
                    double ms;
                    const GpuMark* mark;
                };
                std::vector<Segment> segments;
                double before = double(t[ts.marks[0].index] - t[0]) * period;
                for (size_t m = 0; m < ts.marks.size(); m++)
                {
                    const uint64_t end = m + 1 < ts.marks.size() ? t[ts.marks[m + 1].index] : t[1];
                    segments.push_back({ double(int64_t(end - t[ts.marks[m].index])) * period, &ts.marks[m] });
                }
                std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.ms > b.ms; });
                std::string top;
                char line[256];
                for (size_t k = 0; k < std::min<size_t>(segments.size(), 8); k++)
                {
                    const GpuMark& m = *segments[k].mark;
                    snprintf(line, sizeof line, "%s%s %08X %ux%u (%u draws) %.1f", top.empty() ? "" : "; ", m.kind, m.addr,
                        m.width, m.height, m.draws, segments[k].ms);
                    top += line;
                    if (m.draws)
                        snprintf(line, sizeof line, " [last: vs %08X ps %08X, %u vertices x %u]", m.vs, m.ps, m.count,
                            m.instances);
                    else
                        line[0] = 0;
                    top += line;
                }
                dtlog::Important("[gpu-stats] slow GPU frame %llu: %.1f ms, %zu segments, %.1f ms before the first: %s\n",
                    (unsigned long long)ts.frame, gGpuLastMs, ts.marks.size(), before, top.c_str());
            }
        }
    }
    vkCmdResetQueryPool(cmd, gTimestamps, base, TimestampsPerSlot);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gTimestamps, base);
    ts.used = 2, ts.active = true, ts.frame = Dev().FrameIndex();
    ts.pairs.clear(), ts.marks.clear();
}

// A mark before GPU work (a render pass, a copy, a clear): the work up to the next mark is charged to it.
void GpuMarkHere(const char* kind, uint32_t addr, uint32_t width, uint32_t height)
{
    if (!gStatsOn || !gTimestamps)
        return;
    TimestampSlot& ts = gTimestampSlots[Dev().Slot()];
    if (!ts.active || ts.used + 1 > TimestampsPerSlot)
        return;
    const uint32_t index = ts.used++;
    vkCmdWriteTimestamp(Dev().Cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gTimestamps, Dev().Slot() * TimestampsPerSlot + index);
    ts.marks.push_back({ index, kind, addr, width, height, 0 });
}

void GpuMarkDraw(uint32_t vs, uint32_t ps, uint32_t count, uint32_t instances)
{
    if (!gStatsOn)
        return;
    TimestampSlot& ts = gTimestampSlots[Dev().Slot()];
    if (!ts.marks.empty())
    {
        GpuMark& m = ts.marks.back();
        m.draws++, m.vs = vs, m.ps = ps, m.count = count, m.instances = instances;
    }
}

// A timestamp pair around GPU work of interest (snapshot copies); returns the first index, or ~0u.
uint32_t GpuTimestamp(VkPipelineStageFlagBits stage, uint32_t pairIndex = ~0u)
{
    if (!gStatsOn || !gTimestamps)
        return ~0u;
    TimestampSlot& ts = gTimestampSlots[Dev().Slot()];
    if (!ts.active)
        return ~0u;
    uint32_t index = pairIndex;
    if (index == ~0u)
    {
        if (ts.used + 2 > TimestampsPerSlot)
            return ~0u;
        index = ts.used, ts.used += 2;
        ts.pairs.push_back(index);
    }
    else
        index++;
    vkCmdWriteTimestamp(Dev().Cmd(), stage, gTimestamps, Dev().Slot() * TimestampsPerSlot + index);
    return index;
}

// A copy of a surface taken now, for a draw that samples a surface it renders to (GX2 allows that feedback loop;
// Vulkan does not allow an image to be an attachment and sampled at once).
Image& Snapshot(GpuSurface& g)
{
    // Unchanged since the last copy (draws that test against a depth buffer without writing it, say): reuse it.
    if (g.snapshot.image && g.snapshotOf == g.lastWrite)
        return g.snapshot;
    g.snapshotOf = g.lastWrite;
    EndPass();
    gStats.snapshots++;
    gStats.snapshotsBySurface[(uint64_t(g.addr) << 32) | (g.isDepth ? 1u : 0u)]++;
    VkCommandBuffer cmd = Dev().Cmd();
    if (!g.snapshot.image)
    {
        ImageDesc d;
        d.width = g.width, d.height = g.height, d.mipLevels = g.levels, d.format = g.format, d.createView = false;
        if (g.is3D)
            d.depth = g.depth;
        else
            d.layers = g.depth;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.flags = g.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        if (!g.isDepth && SrgbVariant(g.format) != g.format)
            d.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        g.snapshot = Dev().CreateImage(d);
    }
    EnsureLayout(g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Dev().Transition(cmd, g.snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> copies;
    for (uint32_t level = 0; level < g.levels; level++)
    {
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = { g.image.aspect, level, 0, g.is3D ? 1 : g.depth };
        c.extent = { std::max(1u, g.width >> level), std::max(1u, g.height >> level), g.is3D ? std::max(1u, g.depth >> level) : 1 };
        copies.push_back(c);
    }
    GpuMarkHere("snapshot", g.addr, g.width, g.height);
    const uint32_t timing = GpuTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    vkCmdCopyImage(cmd, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.snapshot.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    Dev().Transition(cmd, g.snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (timing != ~0u)
        GpuTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing);
    return g.snapshot;
}

VkComponentSwizzle Swizzle(uint32_t sel)
{
    switch (sel)
    {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

shader::TexDim TextureDim(uint32_t dim)
{
    switch (dim)
    {
    case surface::Dim3D: return shader::TexDim::Tex3D;
    case surface::DimCube: return shader::TexDim::Cube;
    case surface::Dim1DArray: case surface::Dim2DArray: case surface::Dim2DMsaaArray: return shader::TexDim::Tex2DArray;
    default: return shader::TexDim::Tex2D;
    }
}

// The device-local scratch buffer for copies between images through memory, grown to at least `bytes`.
void EnsureScratch(VkDeviceSize bytes)
{
    if (bytes <= gScratchSize)
        return;
    EndPass();
    Dev().SubmitAndWait();
    if (gScratch)
        vkDestroyBuffer(Dev().Handle(), gScratch, nullptr), vkFreeMemory(Dev().Handle(), gScratchMemory, nullptr);
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(Dev().Handle(), &bci, nullptr, &gScratch));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(Dev().Handle(), gScratch, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = Dev().MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(Dev().Handle(), &mai, nullptr, &gScratchMemory));
    VK_CHECK(vkBindBufferMemory(Dev().Handle(), gScratch, gScratchMemory, 0));
    gScratchSize = bytes;
}

void ScratchBarrier(VkCommandBuffer cmd, VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = gScratch;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0,
        nullptr);
}

// Bytes per element and block width (4 for BCn) of a Vulkan format the renderer makes surfaces in.
std::pair<uint32_t, uint32_t> FormatBlock(VkFormat f)
{
    static const std::unordered_map<VkFormat, std::pair<uint32_t, uint32_t>> table = [] {
        std::unordered_map<VkFormat, std::pair<uint32_t, uint32_t>> t;
        for (uint32_t gx2 = 0; gx2 < 0x1000; gx2++)
            if (const auto info = SurfaceFormat(gx2))
                t.emplace(info->format, std::pair{ info->bytesPerElement, info->blockSize });
        return t;
    }();
    const auto it = table.find(f);
    return it != table.end() ? it->second : std::pair{ 0u, 0u };
}

// One level of a copy between images: `layers` layers from srcLayer and dstLayer.
struct LevelCopy
{
    uint32_t srcMip, srcLayer, dstMip, dstLayer, layers;
};

// Copies levels between images whose elements have the same size, one a compressed (BCn) and the other an
// uncompressed view of the same data, block for element, through the scratch buffer (a buffer copy may end at a
// compressed image's edge, so levels smaller than a block need no special case).
void CopyBlocks(GpuSurface& src, GpuSurface& dst, const std::vector<LevelCopy>& levels)
{
    GpuMarkHere("block copy", dst.addr, dst.width, dst.height);
    const uint32_t bytes = FormatBlock(src.format).first, sb = FormatBlock(src.format).second,
                   db = FormatBlock(dst.format).second;
    std::vector<VkBufferImageCopy> out, in;
    VkDeviceSize offset = 0;
    for (const LevelCopy& l : levels)
    {
        const uint32_t sw = std::max(1u, src.width >> l.srcMip), sh = std::max(1u, src.height >> l.srcMip);
        const uint32_t dw = std::max(1u, dst.width >> l.dstMip), dh = std::max(1u, dst.height >> l.dstMip);
        const uint32_t bw = std::min((sw + sb - 1) / sb, (dw + db - 1) / db), bh = std::min((sh + sb - 1) / sb, (dh + db - 1) / db);
        VkBufferImageCopy c{};
        c.bufferOffset = offset;
        c.bufferRowLength = bw * sb, c.bufferImageHeight = bh * sb;
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l.srcMip, l.srcLayer, l.layers };
        c.imageExtent = { std::min(sw, bw * sb), std::min(sh, bh * sb), 1 };
        out.push_back(c);
        c.bufferRowLength = bw * db, c.bufferImageHeight = bh * db;
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l.dstMip, l.dstLayer, l.layers };
        c.imageExtent = { std::min(dw, bw * db), std::min(dh, bh * db), 1 };
        in.push_back(c);
        offset += (VkDeviceSize(bw) * bh * bytes * l.layers + 15) & ~VkDeviceSize(15);
    }
    EndPass();
    EnsureScratch(offset);
    EnsureLayout(src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EnsureLayout(dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkCommandBuffer cmd = Dev().Cmd();
    vkCmdCopyImageToBuffer(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScratch, uint32_t(out.size()), out.data());
    ScratchBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, gScratch, dst.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(in.size()), in.data());
    Dev().Transition(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // before the next write into it
    ScratchBarrier(cmd, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
}

// Whether two formats hold the same data as blocks and elements: a BCn format and an uncompressed one whose
// elements are the size of its blocks.
bool BlockCompatible(VkFormat a, VkFormat b)
{
    const auto [ab, aw] = FormatBlock(a);
    const auto [bb, bw] = FormatBlock(b);
    return ab && ab == bb && aw != bw;
}

// Texture streaming copies BCn data with GX2CopySurface described as an uncompressed format of the block's size
// (R32G32B32A32_UINT for BC2/3/5, R32G32_UINT for BC1/4), one element per block: on the hardware that is just
// memory. Here such a copy lands in an image of the uncompressed format at the texture's address. Whether
// `blocks` is that image for texture surface `s`:
bool IsBlockView(const GpuSurface& blocks, const Surface& s)
{
    const auto info = SurfaceFormat(s.format);
    return info && info->blockSize == 4 && !blocks.isDepth && !blocks.is3D && FormatBlock(blocks.format).second == 1 &&
        FormatBlock(blocks.format).first == info->bytesPerElement && blocks.guestWidth == (s.width + 3) / 4 &&
        blocks.guestHeight == (s.height + 3) / 4;
}

// Moves the blocks into the texture's own image and returns that (or null if it cannot).
GpuSurface* BlocksAsTexture(GpuSurface& blocks, const Surface& s)
{
    if (!IsBlockView(blocks, s))
        return nullptr;
    GpuSurface* dst = GetSurface(s, false);
    if (!dst || dst == &blocks || dst->is3D || dst->depth != blocks.depth)
        return nullptr;
    Upload(*dst, s);  // levels the blocks image lacks keep the guest data
    Upload(blocks, blocks.desc);  // levels of the blocks the CPU rewrote since the GPU wrote others (KeepGpuLevels)
    std::vector<LevelCopy> levels;
    for (uint32_t level = 0; level < std::min(blocks.levels, dst->levels); level++)
        levels.push_back({ level, 0, level, 0, blocks.depth });
    CopyBlocks(blocks, *dst, levels);
    // Per level, so that a later CPU write of one level (a new top level streamed in) keeps the others.
    const bool dstCurrent = !dst->gpuOwned || dst->mirrored;
    for (const LevelCopy& c : levels)
        MarkWritten(*dst, int(c.dstMip), &s);
    dst->mirrored = dstCurrent && blocks.mirrored;  // (the same memory: current if the blocks' was)
    if (gLogCopies)
        dtlog::Important("[copies] f%llu blocks->texture %08X %ux%u fmt %X levels %u (blocks %u levels, gpuLevels %X)\n",
            (unsigned long long)Dev().FrameIndex(), s.image, s.width, s.height, s.format, dst->levels, blocks.levels, blocks.gpuLevels);
    gStats.blockCopies++;
    return dst;
}

// The view a draw samples for a GX2 texture, or null if it cannot be sampled (the caller binds a dummy).
// `attached` lists the surfaces the draw renders to.
VkImageView TextureView(uint32_t texture, shader::TexDim dim, const std::vector<GpuSurface*>& attached)
{
    gViewSurface = nullptr;
    const Texture t = ReadTexture(texture);
    const Surface& s = t.surface;
    // A surface the GPU wrote (a render target or a depth buffer) is sampled as it is, whatever format this
    // texture gives it; anything else is (re)made from the texture's own description.
    GpuSurface* g = LatestWritten(s.image);
    if (g && (g->guestWidth != s.width || g->guestHeight != s.height))
        if (GpuSurface* texture = BlocksAsTexture(*g, s))
            g = texture;
    if (!g)
        g = GetSurface(s, false);
    if (!g)
        return VK_NULL_HANDLE;
    gViewSurface = g;
    Upload(*g, s);
    if (gCapturing)
        CaptureTexture(*g, t);
    Image* img = &g->image;
    if (std::find(attached.begin(), attached.end(), g) != attached.end())
        img = &Snapshot(*g);
    EnsureLayout(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const uint32_t firstMip = std::min(t.viewFirstMip, g->levels - 1);
    const uint32_t mips = std::clamp(t.viewNumMips, 1u, g->levels - firstMip);
    const uint32_t layers = g->is3D ? 1 : g->depth;
    const uint32_t firstSlice = std::min(t.viewFirstSlice, layers - 1);
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    uint32_t viewLayers = 1;
    switch (dim)
    {
    case shader::TexDim::Tex3D:
        if (!g->is3D)
            return VK_NULL_HANDLE;
        type = VK_IMAGE_VIEW_TYPE_3D;
        break;
    case shader::TexDim::Cube:
        if (!g->cube || firstSlice + 6 > layers)
            return VK_NULL_HANDLE;
        if (std::clamp(t.viewNumSlices, 1u, layers - firstSlice) > 6)
            ReportOnce("cube map arrays are sampled as their first cube");
        type = VK_IMAGE_VIEW_TYPE_CUBE, viewLayers = 6;
        break;
    case shader::TexDim::Tex2DArray:
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY, viewLayers = std::clamp(t.viewNumSlices, 1u, layers - firstSlice);
        break;
    default:
        if (g->is3D)
            return VK_NULL_HANDLE;
        break;
    }
    VkFormat format = g->format;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (g->isDepth)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if ((s.format & 0x400) && SrgbVariant(format) != format)
        format = SrgbVariant(format);
    const VkComponentMapping swizzle{ Swizzle((t.compMap >> 24) & 7), Swizzle((t.compMap >> 16) & 7),
        Swizzle((t.compMap >> 8) & 7), Swizzle(t.compMap & 7) };
    if (img == &g->snapshot)
        return View(g->snapshot, g->snapshotViews, type, format, firstMip, mips, firstSlice, viewLayers, swizzle, aspect);
    return View(g->image, g->views, type, format, firstMip, mips, firstSlice, viewLayers, swizzle, aspect);
}

// `integer`: the texture holds integer data, which can only be sampled with nearest filtering. `nearestMag`: magnified
// with nearest filtering (the game's bitmap fonts above 720p, see CanvasTextShader).
VkSampler GetSampler(uint32_t samplerAddr, bool integer, bool nearestMag = false)
{
    SamplerState s;
    if (auto it = gSamplerStates.find(samplerAddr); it != gSamplerStates.end())
        s = it->second;
    if (integer)
        s.mag = s.min = 0, s.mip = std::min(s.mip, 1u);
    if (nearestMag)
        s.mag = 0;
    // A direct-mapped cache in front of the map: once per texture per draw, the tree walk compared 40-byte keys at
    // every level (8% of DoDraw).
    static_assert(sizeof(SamplerState) == 40);
    struct CachedSampler
    {
        SamplerState state;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    static CachedSampler cache[256];
    uint64_t words[5];
    memcpy(words, &s, sizeof(words));
    uint64_t hash = 0;
    for (uint64_t w : words)
        hash = (hash ^ w) * 0x9E3779B97F4A7C15ull;
    CachedSampler& slot = cache[hash >> 56];
    if (slot.sampler && memcmp(&slot.state, &s, sizeof(s)) == 0)
        return slot.sampler;
    auto it = gSamplers.find(s);
    if (it != gSamplers.end())
    {
        slot = { s, it->second };
        return it->second;
    }
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = s.mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = s.min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = s.mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = AddressMode(s.clamp[0]);
    sci.addressModeV = AddressMode(s.clamp[1]);
    sci.addressModeW = AddressMode(s.clamp[2]);
    sci.minLod = s.minLod;
    sci.maxLod = s.mip == 0 ? 0.25f : s.maxLod;  // mip filter NONE: base level only
    sci.mipLodBias = s.bias;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // GX2TexAnisoRatio 0-4: 1x, 2x, 4x, 8x, 16x.
    if (s.aniso && Dev().Features().samplerAnisotropy && sci.minFilter == VK_FILTER_LINEAR)
    {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min(float(1u << std::min(s.aniso, 4u)), Dev().Properties().limits.maxSamplerAnisotropy);
    }
    VkSampler sampler;
    VK_CHECK(vkCreateSampler(Dev().Handle(), &sci, nullptr, &sampler));
    gSamplers[s] = sampler;
    if (gSamplers.size() % 500 == 0)
        dtlog::Important("[gpu] %zu samplers (the device allows %u)\n", gSamplers.size(),
            Dev().Properties().limits.maxSamplerAllocationCount);
    return sampler;
}

// ---------------------------------------------------------------------------------------------------------
// Shaders and pipelines
// ---------------------------------------------------------------------------------------------------------
// Shader cache file: "DTSC", format version, texture mask, output mask, buffer mask, warning count, then each warning (u32 length + bytes,
// padded to 4), then the SPIR-V words. A file with another version is ignored and rewritten.
constexpr uint32_t ShaderCacheMagic = 0x43535444, ShaderCacheVersion = 4;

bool ReadShaderCache(const std::string& path, shader::Result& r)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    size_t pos = 0;
    auto u32 = [&](uint32_t& v) {
        if (pos + 4 > bytes.size())
            return false;
        memcpy(&v, bytes.data() + pos, 4);
        pos += 4;
        return true;
    };
    uint32_t magic, version, count;
    if (!u32(magic) || !u32(version) || magic != ShaderCacheMagic || version != ShaderCacheVersion ||
        !u32(r.textureMask) || !u32(r.outputMask) || !u32(r.bufferMask) || !u32(count))
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t n;
        if (!u32(n) || pos + n > bytes.size())
            return false;
        r.warnings.emplace_back(bytes.data() + pos, n);
        pos += (n + 3) & ~3u;
    }
    if (pos >= bytes.size() || (bytes.size() - pos) % 4)
        return false;
    r.spirv.resize((bytes.size() - pos) / 4);
    memcpy(r.spirv.data(), bytes.data() + pos, r.spirv.size() * 4);
    return true;
}

void WriteShaderCache(const std::string& path, const shader::Result& r)
{
    std::vector<uint32_t> words{ ShaderCacheMagic, ShaderCacheVersion, r.textureMask, r.outputMask, r.bufferMask,
        uint32_t(r.warnings.size()) };
    for (const std::string& w : r.warnings)
    {
        words.push_back(uint32_t(w.size()));
        std::vector<uint32_t> text((w.size() + 3) / 4, 0);
        memcpy(text.data(), w.data(), w.size());
        words.insert(words.end(), text.begin(), text.end());
    }
    words.insert(words.end(), r.spirv.begin(), r.spirv.end());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
}

// The hash state of a shader program (shader::HashProgram), remembered per program so draws do not walk its
// bytes: keyed by address, size, stage and halfZ, and checked against a sample of its words.
uint64_t ProgramState(shader::Stage stage, bool halfZ, uint32_t program, uint32_t size)
{
    struct Entry
    {
        uint64_t state = 0, sample = 0, checkedFrame = ~0ull;
    };
    static std::unordered_map<uint64_t, Entry> cache;
    const uint64_t key = (uint64_t(program) << 32 | size) ^ (uint64_t(stage) << 62) ^ (uint64_t(halfZ) << 63);
    Entry& e = cache[key];
    // (the program's fingerprint once a frame: 64 scattered reads per stage per draw were 3% of the GPU thread, and
    // the game loads its shaders once)
    const uint64_t frame = Dev().FrameIndex();
    if (e.state && e.checkedFrame == frame)
        return e.state;
    e.checkedFrame = frame;
    const uint64_t sample = SampleHash(program, size);
    if (!e.state || e.sample != sample)
        e.state = shader::HashProgram(stage, halfZ, guest::Ptr(program), size), e.sample = sample;
    return e.state;
}

// Translates a shader (or reads it from the SPIR-V cache) and makes its module, without touching renderer state: the
// draw thread and the background compile workers both use it. Warnings and errors go to `reports`.
ShaderModule BuildShader(const shader::Request& req, uint64_t hash, std::vector<std::string>& reports)
{
    programs::Remember(hash, req);
    char name[32];
    snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)hash);
    const std::string path = gCacheDir.empty() ? "" : gCacheDir + "/" + name;
    const std::string what = std::string(req.stage == shader::Stage::Vertex ? "vertex" : "pixel") + " shader " + name;
    ShaderModule m;
    shader::Result r;
    if (!path.empty() && !ReadShaderCache(path, r))
        r = {};
    if (r.spirv.empty())
    {
        r = shader::Translate(req);
        if (!r.error.empty())
        {
            m.error = r.error;
            reports.push_back(what + " not translated: " + r.error);
            return m;
        }
        if (!path.empty())
            WriteShaderCache(path, r);
    }
    for (const std::string& w : r.warnings)
        reports.push_back(what + " " + w);
    m.textureMask = r.textureMask;
    m.outputMask = r.outputMask;
    m.bufferMask = r.bufferMask;
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = r.spirv.size() * 4;
    ci.pCode = r.spirv.data();
    VK_CHECK(vkCreateShaderModule(Dev().Handle(), &ci, nullptr, &m.module));
    return m;
}

const ShaderModule& GetShader(const shader::Request& req, uint64_t hash)
{
    auto it = gShaders.find(hash);
    if (it != gShaders.end())
        return it->second;
    const auto compileStart = std::chrono::steady_clock::now();
    std::vector<std::string> reports;
    ShaderModule& m = gShaders[hash] = BuildShader(req, hash, reports);
    for (const std::string& r : reports)
        ReportOnce(r);
    gStats.shaderModules++;
    gStats.tCompile += std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count();
    return m;
}

// ---- background compilation (graphics.async_shaders): a few workers translate shaders and build pipelines a draw
// needs, and hand them over under gMutex; meanwhile the draws that need them are skipped, instead of the frame waiting.
std::atomic<bool> gAsync{ true };
struct CompileQueue
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    unsigned running = 0;
    bool started = false, stopping = false;
} gCompile;
std::unordered_set<uint64_t> gPendingShaders;  // (gMutex)
// Finished background work, installed by the draw thread (it holds gMutex, and may be waiting for this very result).
std::mutex gInstallMutex;
std::condition_variable gInstallCv;
std::vector<std::function<void()>> gInstalls;
std::atomic<bool> gInstallsWaiting{ false };
double gWaitBudget = 0;  // seconds a frame's draws may still wait for background compiles (reset per frame)
constexpr double WaitPerFrame = 0.006;

void Install(std::function<void()> f)
{
    std::lock_guard lock(gInstallMutex);
    gInstalls.push_back(std::move(f));
    gInstallsWaiting = true;
    gInstallCv.notify_all();
}

// gMutex held.
void InstallFinished()
{
    if (!gInstallsWaiting)
        return;
    std::vector<std::function<void()>> done;
    {
        std::lock_guard lock(gInstallMutex);
        done.swap(gInstalls);
        gInstallsWaiting = false;
    }
    for (auto& f : done)
        f();
}

// gMutex held: waits until `ready()` or the frame's wait budget is spent, installing results as they come. A compile
// that finishes within the budget then costs a few milliseconds instead of a skipped (missing or black) draw.
template <typename Ready>
bool WaitForCompile(Ready ready)
{
    InstallFinished();
    if (ready())
        return true;
    if (gWaitBudget <= 0)
        return false;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(gWaitBudget));
    bool ok = false;
    for (;;)
    {
        {
            std::unique_lock lock(gInstallMutex);
            if (gInstalls.empty() && !gInstallCv.wait_until(lock, deadline, [] { return !gInstalls.empty(); }))
                break;
        }
        InstallFinished();
        if ((ok = ready()))
            break;
    }
    gWaitBudget -= std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return ok;
}

void QueueCompile(std::function<void()> job)
{
    std::lock_guard lock(gCompile.mutex);
    if (gCompile.stopping)
        return;
    if (!gCompile.started)
    {
        gCompile.started = true;
        const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 6, 2u, 4u);
        for (unsigned i = 0; i < workers; i++)
            std::thread([] {
                for (;;)
                {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(gCompile.mutex);
                        gCompile.cv.wait(lock, [] { return gCompile.stopping || !gCompile.jobs.empty(); });
                        if (gCompile.stopping)
                            return;
                        job = std::move(gCompile.jobs.front());
                        gCompile.jobs.pop_front();
                        gCompile.running++;
                    }
                    job();
                    std::lock_guard lock(gCompile.mutex);
                    gCompile.running--;
                    gCompile.cv.notify_all();
                }
            }).detach();
    }
    gCompile.jobs.push_back(std::move(job));
    gCompile.cv.notify_one();
}

// At exit (without gMutex held: a finishing job takes it to hand over): drop queued jobs, wait for running ones.
void StopCompileWorkers()
{
    std::unique_lock lock(gCompile.mutex);
    gCompile.stopping = true;
    gCompile.jobs.clear();
    gCompile.cv.notify_all();
    gCompile.cv.wait_for(lock, std::chrono::seconds(5), [] { return gCompile.running == 0; });
}

// Queues a shader's translation (gMutex held); the request and its program bytes are copied, guest memory may change.
void QueueShader(const shader::Request& req, uint64_t hash)
{
    if (!gPendingShaders.insert(hash).second)
        return;
    auto job = std::make_shared<std::pair<shader::Request, std::vector<uint8_t>>>();
    job->first = req;
    job->second.assign(req.program, req.program + req.size);
    job->first.program = job->second.data();
    QueueCompile([job, hash] {
        std::vector<std::string> reports;
        ShaderModule m = BuildShader(job->first, hash, reports);
        Install([m, hash, reports] {
            for (const std::string& r : reports)
                ReportOnce(r);
            if (!gShaders.emplace(hash, m).second && m.module)
                vkDestroyShaderModule(Dev().Handle(), m.module, nullptr);
            gPendingShaders.erase(hash);
            gStats.shaderModules++, gStats.asyncShaders++;
        });
    });
}

// Draws that may wait for a background compile: those into the game's TV-derived targets (the scene, its effects and
// the UI), which are drawn again next frame. Others (shadow maps, render-to-texture that may happen once) compile at once.
bool AsyncAllowed(const State& st)
{
    if (!gAsync)
        return false;
    if (st.colorBuffers[0])
    {
        const Surface s = ReadColorBuffer(st.colorBuffers[0]).surface;
        return Scalable(s.width, s.height);
    }
    if (st.depthBuffer)
    {
        const Surface s = ReadDepthBuffer(st.depthBuffer).surface;
        return Scalable(s.width, s.height);
    }
    return false;
}

void CreateLayouts()
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.push_back({ shader::BindingVsConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr });
    bindings.push_back({ shader::BindingPsConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    for (uint32_t i = 0; i < shader::MaxTextures; i++)
    {
        bindings.push_back({ shader::BindingPsTextures + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
        bindings.push_back({ shader::BindingVsTextures + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr });
        bindings.push_back({ shader::BindingVsBuffers + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
            nullptr });
    }
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = uint32_t(bindings.size());
    lci.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(Dev().Handle(), &lci, nullptr, &gSetLayout));
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &gSetLayout;
    VK_CHECK(vkCreatePipelineLayout(Dev().Handle(), &pci, nullptr, &gPipelineLayout));
    for (auto& pool : gPools)
    {
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 } };
        VkDescriptorPoolCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dci.maxSets = 8192;
        dci.poolSizeCount = uint32_t(std::size(sizes));
        dci.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(Dev().Handle(), &dci, nullptr, &pool));
    }
    // Black stand-ins for textures that are missing or cannot be sampled, one per image type a shader can declare.
    VkCommandBuffer cmd = Dev().Cmd();
    for (int i = 0; i < 4; i++)
    {
        ImageDesc d;
        d.width = d.height = 1;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.createView = false;
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
        if (i == int(shader::TexDim::Tex3D))
            d.depth = 2, type = VK_IMAGE_VIEW_TYPE_3D;  // depth > 1 selects a 3D image
        else if (i == int(shader::TexDim::Cube))
            d.layers = 6, d.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, type = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (i == int(shader::TexDim::Tex2DArray))
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        gDummy[i] = Dev().CreateImage(d);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = gDummy[i].image;
        vci.viewType = type;
        vci.format = d.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers };
        VK_CHECK(vkCreateImageView(Dev().Handle(), &vci, nullptr, &gDummy[i].view));
        Dev().Transition(cmd, gDummy[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers };
        vkCmdClearColorImage(cmd, gDummy[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        Dev().Transition(cmd, gDummy[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

VkDescriptorPool CurrentPool()
{
    const uint64_t f = Dev().FrameIndex();
    VkDescriptorPool pool = gPools[f % 2];
    if (gPoolFrame != f)
    {
        // The device waited for this frame slot's previous use before handing it out again.
        VK_CHECK(vkResetDescriptorPool(Dev().Handle(), pool, 0));
        gPoolFrame = f;
    }
    return pool;
}

VkPrimitiveTopology Topology(uint32_t mode, bool& quads)
{
    quads = false;
    switch (mode)
    {
    case 0x01: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x03: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x04: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0x06: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x13: quads = true; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // QUADS, expanded with an index buffer
    case 0x14: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;               // QUAD_STRIP has the same vertex order
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------------------------------------
// A shader program's identity for logs and DT_DEBUG_PS: a hash of its bytes only (stable across translator changes).
uint32_t ProgramHash(const shader::Request& r)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < r.size; i++)
        h = (h ^ r.program[i]) * 16777619u;
    return h;
}

// A shader's loop variables (GX2LoopVar: register offset, value) as the DX9 loop constants it runs with.
void LoopConstants(uint32_t count, uint32_t vars, std::array<uint32_t, 32>& out)
{
    for (uint32_t i = 0; i < count && i < 32 && vars; i++)
        out[guest::Read32(vars + i * 8) & 31] = guest::Read32(vars + i * 8 + 4);
}
// ---------------------------------------------------------------------------------------------------------
// Buffer cache: guest vertex and index buffers converted once (byte-swapped to what Vulkan reads) and kept in
// host-visible GPU memory. Buffers that keep changing (UE3's dynamic vertex rings) are marked dynamic and go
// through the per-draw upload instead.
// ---------------------------------------------------------------------------------------------------------
struct CacheBlock
{
    VkBuffer buffer = VK_NULL_HANDLE;  // null: a freed slot (indices stay valid: entries name their block by index)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0, used = 0;
    VkDeviceSize live = 0;   // bytes of current entries in it (a rebuilt entry's old copy no longer counts)
    uint64_t lastFrame = 0;  // the last frame that drew from or wrote to it
};
std::vector<CacheBlock> gCacheBlocks;
VkDeviceSize gCacheTotal = 0;
constexpr VkDeviceSize CacheBlockSize = 64ull << 20, CacheLimit = 1536ull << 20;
constexpr uint32_t DynamicAfterChanges = 3;

struct CachedBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t addr = 0, size = 0;   // guest range it was made from
    uint64_t sample = 0, checkedFrame = ~0ull, firstSeen = ~0ull;
    uint64_t lastChange = 0;       // the last frame it was found or marked changed (CheckCached)
    uint32_t changes = 0;
    bool dirty = true, dynamic = false;
    uint32_t block = ~0u;          // its cache block, and the bytes it takes there
    VkDeviceSize allocated = 0;
};
std::unordered_map<uint64_t, CachedBuffer> gVertexCache, gIndexCache;
RangeIndex<CachedBuffer*> gCacheRanges;  // entries that have a buffer, by guest range (map nodes do not move)
std::vector<uint8_t> gScratchBytes;  // conversion buffer in ordinary memory
// The per-draw path's indices (read, and rebased for upload), reused: a fresh vector per draw grown by push_back was
// the hottest spot in DoDraw in level 2's heavy areas.
std::vector<uint32_t> gScratchSource, gScratchIndices;

struct CacheSpan
{
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* ptr;
    uint32_t block;
    VkDeviceSize size;
};

CacheSpan CacheAlloc(VkDeviceSize size)
{
    size = (size + 255) & ~VkDeviceSize(255);
    const uint64_t frame = Dev().FrameIndex();
    for (uint32_t i = 0; i < gCacheBlocks.size(); i++)
    {
        CacheBlock& b = gCacheBlocks[i];
        if (b.buffer && b.used + size <= b.size)
        {
            CacheSpan s{ b.buffer, b.used, b.ptr + b.used, i, size };
            b.used += size, b.lastFrame = frame;
            return s;
        }
    }
    CacheBlock b;
    b.size = std::max(size, CacheBlockSize);
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = b.size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(Dev().Handle(), &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(Dev().Handle(), b.buffer, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = Dev().MemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(Dev().Handle(), &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(Dev().Handle(), b.buffer, b.memory, 0));
    void* p;
    VK_CHECK(vkMapMemory(Dev().Handle(), b.memory, 0, b.size, 0, &p));
    b.ptr = static_cast<uint8_t*>(p);
    gCacheTotal += b.size;
    b.used = size, b.lastFrame = frame;
    uint32_t slot = 0;
    while (slot < gCacheBlocks.size() && gCacheBlocks[slot].buffer)
        slot++;
    if (slot == gCacheBlocks.size())
        gCacheBlocks.push_back(b);
    else
        gCacheBlocks[slot] = b;
    return { b.buffer, 0, b.ptr, slot, size };
}

// An entry's new copy: its old one (which the GPU may still read) stops counting as live in its block.
void CacheAssign(CachedBuffer& e, const CacheSpan& span)
{
    if (e.block < gCacheBlocks.size())
        gCacheBlocks[e.block].live -= std::min(gCacheBlocks[e.block].live, e.allocated);
    e.block = span.block, e.allocated = span.size;
    gCacheBlocks[span.block].live += span.size;
}

void CacheTouch(const CachedBuffer& e)
{
    if (e.block < gCacheBlocks.size())
        gCacheBlocks[e.block].lastFrame = Dev().FrameIndex();
}

// Evicted blocks, freed once the GPU is done with them (3 frames: 2 are in flight): (frame evicted, block).
std::vector<std::pair<uint64_t, CacheBlock>> gEvictedBlocks;

// Rebuilt buffers get fresh space (the GPU may still read the old copy), so the cache grows, and blocks fill with dead
// copies among live entries. Past the limit: evict the blocks holding the fewest live bytes (their entries are converted
// again when next drawn, so the least work), a few per draw at most; an evicted block's memory is freed 3 frames
// later, so a block in use this frame can go too. Nothing waits. (Flushing everything stalled the GPU and reconverted the
// whole scene: frames of 100-195 ms. Evicting only blocks unused for a few frames was not enough: in level 3 every block
// held something drawn every frame, so it fell back to the flush.)
void TrimBufferCache()
{
    const uint64_t frame = Dev().FrameIndex();
    for (auto it = gEvictedBlocks.begin(); it != gEvictedBlocks.end();)
        if (it->first + 3 <= frame)
        {
            vkDestroyBuffer(Dev().Handle(), it->second.buffer, nullptr);
            vkFreeMemory(Dev().Handle(), it->second.memory, nullptr);
            it = gEvictedBlocks.erase(it);
        }
        else
            ++it;
    if (gCacheTotal <= CacheLimit)
        return;
    for (int n = 0; n < 4 && gCacheTotal > CacheLimit * 3 / 4; n++)
    {
        uint32_t pick = ~0u;
        for (uint32_t i = 0; i < gCacheBlocks.size(); i++)
        {
            const CacheBlock& c = gCacheBlocks[i];
            if (c.buffer && (pick == ~0u || c.live < gCacheBlocks[pick].live ||
                                (c.live == gCacheBlocks[pick].live && c.lastFrame < gCacheBlocks[pick].lastFrame)))
                pick = i;
        }
        if (pick == ~0u)
            break;
        for (auto* cache : { &gVertexCache, &gIndexCache })
            for (auto& [key, e] : *cache)
                if (e.block == pick)
                {
                    gCacheRanges.Remove(e.addr, &e);
                    e.buffer = VK_NULL_HANDLE, e.block = ~0u, e.allocated = 0, e.dirty = true;
                }
        gCacheTotal -= gCacheBlocks[pick].size;
        gEvictedBlocks.push_back({ frame, gCacheBlocks[pick] });
        gCacheBlocks[pick] = {};
        gStats.cacheEvictions++;
    }
}

enum class CacheUse { Cached, Rebuild, PerDraw };

// Decide how to use the cache entry for guest memory [addr, addr + size): as it is, rebuilt, or not at all.
// A buffer is cached only once it is seen again in a later frame: per-draw data (DrawPrimitiveUP rings, whose
// address moves with every draw) never gets there, and buffers that keep changing become dynamic.
constexpr uint64_t RebuildBudget = 24ull << 20;
uint64_t gRebuildBytes = 0;  // converted this frame (reset where the frame ends)

CacheUse CheckCached(CachedBuffer& e, uint32_t addr, uint32_t size)
{
    const uint64_t frame = Dev().FrameIndex();
    if (e.dynamic)
        return CacheUse::PerDraw;
    if (!e.buffer && (e.firstSeen == ~0ull || e.firstSeen == frame))
    {
        e.firstSeen = std::min(e.firstSeen, frame);
        return CacheUse::PerDraw;
    }
    // (fingerprinted every frame while recently changed, else every 4th frame, staggered: GX2Invalidate reports CPU
    // writes at once, as the hardware requires; this is the fallback, and 64 scattered reads per buffer per frame were
    // 3% of the GPU thread)
    if (e.checkedFrame != frame && (frame - e.lastChange < 30 || ((frame + (addr >> 12)) & 3) == 0))
    {
        e.checkedFrame = frame;
        const uint64_t sample = SampleHash(addr, size);
        if (e.buffer && sample != e.sample)
            e.dirty = true;
        e.sample = sample;
    }
    if (!e.dirty)
        return CacheUse::Cached;
    e.lastChange = frame;
    if (e.checkedFrame != frame)  // (the fingerprint of what is converted now)
        e.checkedFrame = frame, e.sample = SampleHash(addr, size);
    // At most RebuildBudget bytes converted per frame (at least one buffer): new models appear in bursts (520 buffers,
    // 190 MB in one frame at a level's start, 120 ms). Over budget, this draw uploads just the range it uses and the
    // buffer is converted on a later frame.
    if (gRebuildBytes > 0 && gRebuildBytes + size > RebuildBudget)
    {
        gStats.rebuildsDeferred++;
        return CacheUse::PerDraw;
    }
    if (e.buffer && ++e.changes >= DynamicAfterChanges)
    {
        e.dynamic = true;  // keeps changing: upload per draw instead
        return CacheUse::PerDraw;
    }
    gRebuildBytes += size;
    return CacheUse::Rebuild;
}

// Forget entries that were seen once and never cached (per-draw ring data), so the maps stay small.
void PruneBufferCache()
{
    const uint64_t frame = Dev().FrameIndex();
    for (auto* cache : { &gVertexCache, &gIndexCache })
        for (auto it = cache->begin(); it != cache->end();)
            if (!it->second.buffer && !it->second.dynamic && it->second.firstSeen + 120 < frame)
                it = cache->erase(it);
            else
                ++it;
}

// A render target view: a surface, the attachment view of one mip and slice, and that level's size.
struct Attachment
{
    GpuSurface* surface = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mip = 0, layer = 0;
};

std::optional<Attachment> ColorAttachment(uint32_t colorBuffer)
{
    const ColorBuffer cb = ReadColorBuffer(colorBuffer);
    GpuSurface* g = GetSurface(cb.surface, false, gRenderScale, true);
    if (!g)
        return std::nullopt;
    if (!(Dev().FormatFeatures(g->format) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    {
        ReportOnce("render target format 0x" + Hex(cb.surface.format) + " cannot be rendered to on this GPU");
        return std::nullopt;
    }
    if (cb.viewNumSlices > 1)
        ReportOnce("layered rendering (a color buffer view of several slices) draws to the first slice only");
    Attachment a;
    a.surface = g;
    const uint32_t mip = std::min(cb.viewMip, g->levels - 1);
    a.format = (cb.surface.format & 0x400) && SrgbVariant(g->format) != g->format ? SrgbVariant(g->format) : g->format;
    if (g->is3D)
    {
        ReportOnce("rendering into a 3D texture is not supported");
        return std::nullopt;
    }
    a.mip = mip, a.layer = std::min(cb.viewFirstSlice, g->depth - 1);
    a.view = View(g->image, g->views, VK_IMAGE_VIEW_TYPE_2D, a.format, mip, 1, a.layer, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    a.width = std::max(1u, g->width >> mip), a.height = std::max(1u, g->height >> mip);
    return a;
}

std::optional<Attachment> DepthAttachment(uint32_t depthBuffer)
{
    const DepthBuffer db = ReadDepthBuffer(depthBuffer);
    GpuSurface* g = GetSurface(db.surface, true, gRenderScale, true);
    if (!g)
        return std::nullopt;
    Attachment a;
    a.surface = g;
    a.format = g->format;
    const uint32_t mip = std::min(db.viewMip, g->levels - 1);
    a.mip = mip, a.layer = std::min(db.viewFirstSlice, g->depth - 1);
    a.view = View(g->image, g->views, VK_IMAGE_VIEW_TYPE_2D, g->format, mip, 1, a.layer, 1, {}, g->image.aspect);
    a.width = std::max(1u, g->width >> mip), a.height = std::max(1u, g->height >> mip);
    return a;
}

void DoDraw(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices, uint32_t firstVertex, uint32_t instances)
{
    // A run of occlusion queries ended a while ago: submit it now, so its results are ready when the game asks (it reads
    // them at the start of the next frame).
    if (!gOpenQuery.active && gDrawsSinceQueryEnd >= 48 && !gPendingQueries.empty())
        SubmitQueriesEarly(), gDrawsSinceQueryEnd = 0;
    // Results counted since: hand them over now, before the game asks (every 16 draws, without blocking).
    static uint32_t pollCounter = 0;
    if (!gPendingQueries.empty() && (++pollCounter & 15) == 0)
        ResolvePending(false, 0, false);
    State& st = *gState;
    PhaseTimer timer;
    if (count == 0 || !st.rasterizer)
        return;  // rasterizer discard: nothing reaches the targets (stream out is not supported)
    if (!st.vertexShader || !st.pixelShader)
    {
        ReportOnce("draw without a vertex or pixel shader");
        return;
    }
    bool quads;
    const VkPrimitiveTopology topology = Topology(mode, quads);
    if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
    {
        ReportOnce("unsupported primitive mode 0x" + Hex(mode));
        return;
    }
    if ((topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) && st.lineWidth != 1.0f)
        ReportOnce("lines are drawn 1 pixel wide (GX2SetLineWidth asked for another width)");
    const VertexShader vs = ReadVertexShader(st.vertexShader);
    const PixelShader ps = ReadPixelShader(st.pixelShader);
    auto fsIt = gFetchShaders.find(st.fetchShader);
    if (fsIt == gFetchShaders.end())
    {
        ReportOnce("draw with a fetch shader that was never initialised");
        return;
    }

    // ---- vertex inputs: GX2 attribute location -> semantic slot i -> GPR i+1 ----
    // The requests are reused from draw to draw (reset to defaults, keeping their vectors' capacity).
    auto reset = [](shader::Request& r) {
        auto inputs = std::move(r.vertexInputs);
        auto pixelInputs = std::move(r.pixelInputs);
        auto defaults = std::move(r.pixelInputDefaults);
        r = shader::Request{};
        inputs.clear(), pixelInputs.clear(), defaults.clear();
        r.vertexInputs = std::move(inputs), r.pixelInputs = std::move(pixelInputs), r.pixelInputDefaults = std::move(defaults);
    };
    static shader::Request vsReq, psReq;
    reset(vsReq);
    vsReq.stage = shader::Stage::Vertex;
    vsReq.program = guest::Ptr(vs.program);
    vsReq.size = vs.size;
    vsReq.halfZ = st.halfZ;
    vsReq.programState = ProgramState(shader::Stage::Vertex, st.halfZ, vs.program, vs.size);
    LoopConstants(vs.loopVarCount, vs.loopVars, vsReq.loopConsts);
    static std::vector<VkVertexInputAttributeDescription> attrs;
    attrs.clear();
    SmallMap<uint32_t> bindingStride;
    SmallMap<uint32_t> bindingDivisor;  // per-instance bindings: their divisor (1 = one value per instance)
    static PipelineKey key;
    key.Clear();
    for (const AttribStream& a : fsIt->second)
    {
        int slot = -1;
        for (uint32_t i = 0; i < vs.SemanticCount() && i < 32; i++)
            if ((vs.Semantic(i) & 0xFF) == a.location)
                slot = int(i);
        if (slot < 0)
            continue;
        const auto fmt = VertexFormat(a.format);
        if (!fmt)
        {
            ReportOnce("unsupported vertex attribute format 0x" + Hex(a.format));
            return;
        }
        // GX2AttribIndexType: 0 per vertex, 1 per instance (aluDivisor 0 or 1: one value per instance).
        if (a.type == 1)
        {
            uint32_t div = std::max(1u, a.aluDivisor);
            if (div > 1 && !Dev().AttributeDivisor())
            {
                ReportOnce("instance divisors above 1 need VK_KHR_vertex_attribute_divisor; drawn with 1");
                div = 1;
            }
            bindingDivisor[a.buffer] = div;
            ReportOnce("per-instance vertex attributes in use (format 0x" + Hex(a.format) + ", divisor " + std::to_string(div) + ")");
        }
        else if (a.type != 0)
        {
            ReportOnce("unsupported vertex attribute index type " + std::to_string(a.type));
            return;
        }
        const std::array<uint8_t, 4> sel{ uint8_t(a.mask >> 24), uint8_t(a.mask >> 16), uint8_t(a.mask >> 8), uint8_t(a.mask) };
        // Integer attributes (0x100, not float layouts) reach the shader as raw integer bits.
        const bool isFloatLayout = a.format & 0x800 && ((a.format & 0x3F) == 0x03 || (a.format & 0x3F) == 0x06 ||
            (a.format & 0x3F) == 0x08 || (a.format & 0x3F) == 0x09 || (a.format & 0x3F) == 0x0D ||
            (a.format & 0x3F) == 0x0F || (a.format & 0x3F) == 0x11 || (a.format & 0x3F) == 0x13);
        const uint32_t inputType = (a.format & 0x100) && !isFloatLayout ? ((a.format & 0x200) ? 2u : 1u) : 0u;
        vsReq.vertexInputs.push_back({ uint32_t(slot), uint32_t(slot) + 1, sel, inputType });
        attrs.push_back({ uint32_t(slot), a.buffer, fmt->format, a.offset });
        bindingStride[a.buffer] = st.attribs[a.buffer & 15].stride;
        key.Add(uint32_t(slot) | a.buffer << 8 | a.type << 16), key.Add(a.format), key.Add(a.offset), key.Add(a.mask);
    }

    // ---- pixel inputs: match the pixel shader's input semantics with the vertex shader's parameters ----
    reset(psReq);
    psReq.stage = shader::Stage::Pixel;
    psReq.program = guest::Ptr(ps.program);
    psReq.size = ps.size;
    psReq.programState = ProgramState(shader::Stage::Pixel, false, ps.program, ps.size);
    LoopConstants(ps.loopVarCount, ps.loopVars, psReq.loopConsts);
    psReq.alphaFunc = st.alphaTest ? st.alphaFunc : 7;
    for (uint32_t i = 0; i < ps.NumInputs() && i < 32; i++)
    {
        const uint32_t semantic = ps.InputCntl(i) & 0xFF;
        int32_t location = -1;
        for (uint32_t p = 0; p < vs.NumOutputs() * 4 && p < 32; p++)
            if (((vs.OutId(p / 4) >> ((p % 4) * 8)) & 0xFF) == semantic)
                location = int32_t(p);
        psReq.pixelInputs.push_back(location);
        psReq.pixelInputDefaults.push_back(uint8_t((ps.InputCntl(i) >> 8) & 3));
    }
    // SPI_PS_IN_CONTROL_0: POSITION_ENA (bit 8) puts the pixel position in GPR POSITION_ADDR (bits 10-14).
    psReq.positionGpr = (ps.regs[2] & 0x100) ? int32_t((ps.regs[2] >> 10) & 31) : -1;
    // Texture types come from the bound textures and are part of the shader (they decide the sampler declarations).
    // Number types (float / uint / sint) of textures and render targets too: integer data stays raw bits.
    for (int stage = 0; stage < 2; stage++)
        for (uint32_t i = 0; i < shader::MaxTextures; i++)
            if (const uint32_t t = st.textures[stage][i])
            {
                shader::Request& r = stage ? psReq : vsReq;
                r.textureDims[i] = TextureDim(guest::Read32(t));  // GX2Surface.dim
                // A surface the GPU rendered is sampled in its own format (a float texture over a UINT target
                // reads the raw bits, as the hardware's reinterpretation does).
                // Compressed blocks the GPU copied as integers are moved into the texture's own image first
                // (BlocksAsTexture).
                const GpuSurface* latest = LatestWritten(guest::Read32(t + 0x24));  // GX2Surface.image
                if (latest && IsBlockView(*latest, ReadSurface(t)))
                    latest = nullptr;
                r.textureTypes[i] = latest ? uint8_t(VkNumberType(latest->format))
                                           : uint8_t(NumberType(guest::Read32(t + 0x14)));  // GX2Surface.format
            }
    for (uint32_t i = 0; i < 8; i++)
        if (st.colorBuffers[i])
            psReq.outputTypes[i] = uint8_t(NumberType(guest::Read32(st.colorBuffers[i] + 0x14)));

    timer.Lap(gStats.tSetup);
    // DT_DEBUG_PS=<pixel shader hash, as in capture logs>,<gpr>[,<cf>]: render target 0 shows that GPR as it is
    // after CF instruction cf (or as interpolated), with alpha 1000 so captures can report its range.
    struct DebugPs
    {
        uint32_t hash = 0;
        int32_t gpr = -1, cf = -1;
    };
    static const DebugPs debugPs = [] {
        DebugPs d;
        const char* v = getenv("DT_DEBUG_PS");
        unsigned hash = 0;
        int gpr = -1, cf = -1;
        char what = 0;
        if (v && sscanf(v, "%x,%d,%d,%c", &hash, &gpr, &cf, &what) >= 2)
            d.hash = hash, d.gpr = gpr | (what == 'w' ? 0x100 : 0), d.cf = cf;  // ",w": the GPR's w in every channel
        if (v)
            dtlog::Important("[gpu] DT_DEBUG_PS=\"%s\": pixel shader %08X, R%d after CF %d\n", v, d.hash, d.gpr, d.cf);
        return d;
    }();
    if (debugPs.gpr >= 0 && ProgramHash(psReq) == debugPs.hash)
    {
        static bool logged = false;
        if (!std::exchange(logged, true))
            dtlog::Important("[gpu] DT_DEBUG_PS: probing pixel shader %08X\n", debugPs.hash);
        psReq.debugGpr = debugPs.gpr, psReq.debugCf = debugPs.cf;
    }
    const uint64_t vsHash = shader::Hash(vsReq), psHash = shader::Hash(psReq);
    // Full-screen and other small passes (6 vertices or fewer: post-processing, composites) never wait in the
    // background: skipping one blanks the picture or a whole effect for a frame.
    InstallFinished();
    const bool async = AsyncAllowed(st) && count > 6;
    if (async && (!gShaders.count(vsHash) || !gShaders.count(psHash)))
    {
        // Translated in the background; the draw waits within the frame's budget, else is skipped this time.
        if (!gShaders.count(vsHash))
            QueueShader(vsReq, vsHash);
        if (!gShaders.count(psHash))
            QueueShader(psReq, psHash);
        if (!WaitForCompile([&] { return gShaders.count(vsHash) && gShaders.count(psHash); }))
        {
            gStats.asyncSkips++, gOpenQuery.forceVisible |= gOpenQuery.active;
            return;
        }
    }
    const ShaderModule& vsm = GetShader(vsReq, vsHash);
    const ShaderModule& psm = GetShader(psReq, psHash);
    if (!vsm.module || !psm.module)
        return;

    timer.Lap(gStats.tShaders);
    // ---- attachments: the render targets the pixel shader writes, and depth/stencil when it is in use ----
    std::array<std::optional<Attachment>, 8> colors{};
    uint32_t colorCount = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (st.colorBuffers[i] && (psm.outputMask & (1u << i)))
            if ((colors[i] = ColorAttachment(st.colorBuffers[i])))
                colorCount = i + 1;
    std::optional<Attachment> depth;
    if (st.depthBuffer && (st.depthTest || st.depthWrite || st.stencilTest))
        depth = DepthAttachment(st.depthBuffer);
    if (!colorCount && !depth)
        return;  // nothing to draw into
    static std::vector<GpuSurface*> attached;
    attached.clear();
    uint32_t areaW = ~0u, areaH = ~0u;
    for (auto& c : colors)
        if (c)
            attached.push_back(c->surface), areaW = std::min(areaW, c->width), areaH = std::min(areaH, c->height);
    if (depth)
        attached.push_back(depth->surface), areaW = std::min(areaW, depth->width), areaH = std::min(areaH, depth->height);

    // ---- resources (all transfers happen before the rendering pass begins) ----
    VkCommandBuffer cmd = Dev().Cmd();
    VkDescriptorImageInfo images[2][16];
    // The engine's canvas text (subtitles, loading tips, SKIP: pixel shader 118E83C5, a textured quad) samples a bitmap
    // font whose glyphs are packed tightly in one texture. Drawn 1:1 at 720p that is exact; magnified (1440p), bilinear
    // filtering at a glyph's edge took in a quarter of the next glyph's texels: "_" marks under spaces and punctuation.
    // Nearest magnification keeps each glyph to its own texels.
    static std::unordered_map<uint64_t, bool> canvasTextShaders;
    bool canvasText = false;
    if (gRenderScale != 1.0f)
    {
        auto [it, fresh] = canvasTextShaders.try_emplace(psHash, false);
        if (fresh)
            it->second = ProgramHash(psReq) == 0x118E83C5u;
        canvasText = it->second;
    }
    std::vector<GpuSurface*> shadowMaps;  // larger shadow maps the pixel stage samples (graphics.shadow_quality)
    std::vector<GpuSurface*> readsOwnTarget;  // targets this draw also samples (through their snapshot)
    for (int stage = 0; stage < 2; stage++)
    {
        const uint32_t mask = stage ? psm.textureMask : vsm.textureMask;
        const shader::Request& req = stage ? psReq : vsReq;
        for (uint32_t i = 0; i < 16; i++)
        {
            if (!(mask & (1u << i)))
                continue;
            const shader::TexDim dim = req.textureDims[i];
            VkImageView view = st.textures[stage][i] ? TextureView(st.textures[stage][i], dim, attached) : VK_NULL_HANDLE;
            if (view && stage == 1 && gViewSurface && gViewSurface->shadow)
                shadowMaps.push_back(gViewSurface);
            if (view && gViewSurface && std::find(attached.begin(), attached.end(), gViewSurface) != attached.end())
                readsOwnTarget.push_back(gViewSurface);
            if (!view)
                view = gDummy[int(dim)].view;
            images[stage][i] = { GetSampler(st.samplers[stage][i], req.textureTypes[i] != 0, stage == 1 && canvasText), view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
    }

    timer.Lap(gStats.tTextures);
    // ---- vertex and index data ----
    // The byte-swap unit of each attribute (GX2EndianSwapMode: 0 none, 1 8-in-16, 2 8-in-32, 3 the format's default).
    auto swapUnit = [](const AttribStream& a, const std::optional<AttribFormat>& fmt) -> uint32_t {
        return a.endianSwap == 1 ? 2 : a.endianSwap == 2 ? 4 : a.endianSwap == 3 && fmt ? fmt->componentSize : 1;
    };
    // Swap the attributes of `binding` in `vertices` vertices at p (stride apart), not reading past `limit`.
    // Several attribute streams may describe the same bytes (the game's fetch shaders list unused locations at
    // the offset of a used one); each byte range is swapped once, or an even number of swaps would undo it.
    auto swapVertices = [&](uint32_t binding, uint32_t stride, uint8_t* p0, uint32_t vertices, const uint8_t* limit) {
        std::vector<std::pair<uint32_t, uint32_t>> swapped;  // (offset, bytes) already done
        for (const AttribStream& a : fsIt->second)
        {
            if (a.buffer != binding)
                continue;
            const auto fmt = VertexFormat(a.format);
            const uint32_t unit = swapUnit(a, fmt);
            if (!fmt || unit == 1)
                continue;
            const uint32_t attribBytes = fmt->componentSize * fmt->components;
            if (std::any_of(swapped.begin(), swapped.end(), [&](auto r) { return a.offset < r.first + r.second && r.first < a.offset + attribBytes; }))
            {
                if (std::find(swapped.begin(), swapped.end(), std::pair{ a.offset, attribBytes }) == swapped.end())
                    ReportOnce("vertex attributes overlap with different layouts (offset " + std::to_string(a.offset) + ")");
                continue;
            }
            swapped.push_back({ a.offset, attribBytes });
            // Swap each component (unit bytes) of this attribute in every vertex; whole 16/32-bit words directly.
            for (uint32_t v = 0; v < vertices; v++)
            {
                uint8_t* p = p0 + size_t(v) * stride + a.offset;
                if (p + attribBytes <= limit && unit == 4)
                    for (uint32_t c = 0; c + 4 <= attribBytes; c += 4)
                    {
                        uint32_t w;
                        memcpy(&w, p + c, 4), w = __builtin_bswap32(w), memcpy(p + c, &w, 4);
                    }
                else if (p + attribBytes <= limit && unit == 2)
                    for (uint32_t c = 0; c + 2 <= attribBytes; c += 2)
                    {
                        uint16_t h;
                        memcpy(&h, p + c, 2), h = __builtin_bswap16(h), memcpy(p + c, &h, 2);
                    }
                else
                    for (uint32_t c = 0; c + unit <= attribBytes && p + unit <= limit; c += unit, p += unit)
                        std::reverse(p, p + unit);
            }
        }
    };

    // Cached path first: every binding a static buffer converted once.
    SmallMap<std::pair<VkBuffer, VkDeviceSize>> vertexBinds;
    bool cached = !quads;
    TrimBufferCache();
    for (auto& [binding, stride] : bindingStride)
    {
        const AttribBuffer& ab = st.attribs[binding & 15];
        if (!ab.addr || !ab.size)
        {
            ReportOnce("draw reads attribute buffer " + std::to_string(binding) + ", which is not set");
            return;
        }
        uint64_t key = (uint64_t(ab.addr) << 32 | ab.size) * 1099511628211ull ^ stride;
        for (const AttribStream& a : fsIt->second)
            if (a.buffer == binding)
                key = (key ^ (uint64_t(a.offset) << 8 | swapUnit(a, VertexFormat(a.format)))) * 1099511628211ull;
        CachedBuffer& e = gVertexCache[key];
        const CacheUse use = CheckCached(e, ab.addr, ab.size);
        if (use == CacheUse::Rebuild)
        {
            // Convert in ordinary memory: mapped GPU memory is often write-combined, and reading it back (as an
            // in-place swap would) is very slow.
            const CacheSpan span = CacheAlloc(ab.size);
            gScratchBytes.assign(guest::Ptr(ab.addr), guest::Ptr(ab.addr) + ab.size);
            swapVertices(binding, stride, gScratchBytes.data(), stride ? (ab.size + stride - 1) / stride : 1,
                gScratchBytes.data() + ab.size);
            memcpy(span.ptr, gScratchBytes.data(), ab.size);
            if (!e.buffer)
                gCacheRanges.Add(ab.addr, ab.size, &e);
            CacheAssign(e, span);
            e.buffer = span.buffer, e.offset = span.offset, e.addr = ab.addr, e.size = ab.size, e.dirty = false;
            gStats.bufferUploads++, gStats.uploadBytes += ab.size;
            gStats.rebuilds++, gStats.rebuildBytes += ab.size;
        }
        if (use == CacheUse::PerDraw)
            cached = false;
        CacheTouch(e);
        vertexBinds[binding] = { e.buffer, e.offset };
    }
    // GX2IndexType: 0 U16 little-endian, 1 U32 little-endian, 4 U16 big-endian, 9 U32 big-endian.
    const bool is32 = indexType == 1 || indexType == 9;
    const bool swapIndices = indexType == 4 || indexType == 9;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
    uint32_t drawCount = count;
    int32_t vertexOffset = 0;    // added to each index (GX2's base vertex)
    uint32_t drawFirstVertex = 0;
    uint32_t vertexBase = 0;     // vertex 0 of the bound buffers in guest terms (for VTX fetches)
    if (cached && indices)
    {
        const uint32_t bytes = count * (is32 ? 4 : 2);
        CachedBuffer& e = gIndexCache[(uint64_t(indices) << 32 | count) * 1099511628211ull ^ indexType];
        const CacheUse use = CheckCached(e, indices, bytes);
        if (use == CacheUse::Rebuild)
        {
            const CacheSpan span = CacheAlloc(bytes);
            gScratchBytes.assign(guest::Ptr(indices), guest::Ptr(indices) + bytes);
            if (swapIndices)
                for (uint32_t i = 0; i < count; i++)
                    std::reverse(gScratchBytes.data() + i * (is32 ? 4 : 2), gScratchBytes.data() + (i + 1) * (is32 ? 4 : 2));
            memcpy(span.ptr, gScratchBytes.data(), bytes);
            if (!e.buffer)
                gCacheRanges.Add(indices, bytes, &e);
            CacheAssign(e, span);
            e.buffer = span.buffer, e.offset = span.offset, e.addr = indices, e.size = bytes, e.dirty = false;
            gStats.bufferUploads++, gStats.uploadBytes += bytes;
            gStats.rebuilds++, gStats.rebuildBytes += bytes;
        }
        if (use == CacheUse::PerDraw)
            cached = false;
        CacheTouch(e);
        indexBuffer = e.buffer, indexOffset = e.offset;
        vkIndexType = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vertexOffset = int32_t(firstVertex);
    }
    else if (cached)
        drawFirstVertex = firstVertex;

    if (!cached)
    {
        // Per-draw path: read the indices, find the vertex range they use, and upload just that range.
        gStats.perDraws++;
        vertexBinds.clear();
        indexBuffer = VK_NULL_HANDLE, vertexOffset = 0, drawFirstVertex = 0;
        std::vector<uint32_t>& source = gScratchSource;
        uint32_t lo = firstVertex, hi = firstVertex + count - 1;
        if (indices)
        {
            source.resize(count);
            lo = ~0u, hi = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v;
                if (is32)
                {
                    memcpy(&v, guest::Ptr(indices + i * 4), 4);
                    if (swapIndices)
                        v = __builtin_bswap32(v);
                }
                else
                {
                    uint16_t h;
                    memcpy(&h, guest::Ptr(indices + i * 2), 2);
                    v = swapIndices ? __builtin_bswap16(h) : h;
                }
                source[i] = v + firstVertex;
                lo = std::min(lo, source[i]);
                hi = std::max(hi, source[i]);
            }
        }
        for (auto& [binding, stride] : bindingStride)
        {
            const AttribBuffer& ab = st.attribs[binding & 15];
            // Per-vertex data: the range the indices use; per-instance data: from instance 0.
            const uint32_t* div = bindingDivisor.find(binding);
            const uint64_t begin = div ? 0 : uint64_t(lo) * stride;
            const uint64_t elements = div ? (std::max(1u, instances) + *div - 1) / *div : uint64_t(hi - lo + 1);
            if (begin >= ab.size)
            {
                ReportOnce("draw reads vertices outside its attribute buffer " + std::to_string(binding));
                return;
            }
            const uint32_t uploadBytes = uint32_t(std::min<uint64_t>(ab.size - begin, elements * std::max(stride, 1u)));
            auto up = Dev().Allocate(uploadBytes, 16);
            gScratchBytes.assign(guest::Ptr(ab.addr + uint32_t(begin)), guest::Ptr(ab.addr + uint32_t(begin)) + uploadBytes);
            swapVertices(binding, stride, gScratchBytes.data(), stride ? (uploadBytes + stride - 1) / stride : 1,
                gScratchBytes.data() + uploadBytes);
            memcpy(up.ptr, gScratchBytes.data(), uploadBytes);  // write-only: the ring may be write-combined
            gStats.perDrawBytes += uploadBytes;
            vertexBinds[binding] = { up.buffer, up.offset };
        }
        // Indices rebased to the uploaded range (and quads expanded to triangles).
        if (quads || indices)
        {
            std::vector<uint32_t>& out = gScratchIndices;
            auto idx = [&](uint32_t i) { return (indices ? source[i] : firstVertex + i) - lo; };
            out.resize(quads ? count / 4 * 6 : count);
            uint32_t* o = out.data();
            if (quads)
                for (uint32_t q = 0; q + 3 < count; q += 4)
                    for (uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u })
                        *o++ = idx(q + k);
            else
                for (uint32_t i = 0; i < count; i++)
                    *o++ = idx(i);
            auto up = Dev().Allocate(out.size() * 4, 4);
            memcpy(up.ptr, out.data(), out.size() * 4);
            indexBuffer = up.buffer, indexOffset = up.offset, vkIndexType = VK_INDEX_TYPE_UINT32;
            drawCount = uint32_t(out.size());
        }
        vertexBase = lo;
    }

    // Constant files, converted from big-endian: GX2's 256 constants, then renderer data (see translator.h).
    VkDescriptorBufferInfo constants[2];
    for (int stage = 0; stage < 2; stage++)
    {
        constexpr uint32_t Size = shader::ConstantRegisters * 16;
        auto up = Dev().Allocate(Size, Dev().Properties().limits.minUniformBufferOffsetAlignment);
        // A larger shadow map: the game's filtering works in its texels (the projection shader multiplies by the map's
        // size, takes floor and fraction, and multiplies back by the texel size), so its constants holding the map's
        // size and texel size are made the larger map's. (Adjusted in a copy in ordinary memory: the upload ring is
        // write-combined, and reading the 256 registers back from it was a quarter of DoDraw's time at Ultra shadows.)
        static float scaled[1024];
        const float* source = st.constants[stage];
        if (stage == 1 && !shadowMaps.empty())
        {
            memcpy(scaled, st.constants[stage], 4096);
            source = scaled;
        }
        if (stage == 1)
            for (const GpuSurface* m : shadowMaps)
            {
                float* c = scaled;
                const float w = float(m->guestWidth), h = float(m->guestHeight);
                auto near = [](float a, float b) { return std::abs(a - b) <= std::abs(b) * 1e-4f; };
                for (uint32_t r = 0; r < 256; r++)
                {
                    float* v = c + r * 4;
                    if (near(v[0], w) && near(v[1], h))
                        v[0] *= m->scale, v[1] *= m->scale;
                    else if (near(v[0], 1.0f / w) && near(v[1], 1.0f / h))
                        v[0] /= m->scale, v[1] /= m->scale;
                }
            }
        memcpy(up.ptr, source, 4096);
        uint32_t extra[20] = {};
        extra[0] = std::bit_cast<uint32_t>(st.alphaRef);
        extra[1] = vertexBase;  // VTX fetches index the whole guest buffer
        extra[2] = std::bit_cast<uint32_t>(1.0f / (attached.empty() ? 1.0f : attached[0]->scale));  // (pixel position)
        for (uint32_t n = 0; n < 16; n++)
            extra[4 + n] = st.attribs[n].stride;
        memcpy(up.ptr + 4096, extra, sizeof(extra));
        constants[stage] = { up.buffer, up.offset, Size };
    }

    // Attribute buffers read by VTX clauses, whole and raw (the shader does the byte swapping).
    std::array<VkDescriptorBufferInfo, 16> vtxBuffers{};
    for (uint32_t n = 0; n < 16; n++)
    {
        if (!(vsm.bufferMask & (1u << n)))
            continue;
        const AttribBuffer& ab = st.attribs[n];
        const uint32_t bytes = std::max(4u, (ab.addr ? ab.size : 0) + 3 & ~3u);
        auto up = Dev().Allocate(bytes, Dev().Properties().limits.minStorageBufferOffsetAlignment);
        memset(up.ptr, 0, bytes);
        if (ab.addr)
            memcpy(up.ptr, guest::Ptr(ab.addr), ab.size);
        gStats.vtxBytes += bytes;
        vtxBuffers[n] = { up.buffer, up.offset, bytes };
    }

    timer.Lap(gStats.tBuffers);
    // ---- pipeline ----
    std::array<VkFormat, 8> colorFormats{};
    std::array<VkPipelineColorBlendAttachmentState, 8> blends{};
    const bool independent = Dev().Features().independentBlend;
    for (uint32_t i = 0; i < colorCount; i++)
    {
        colorFormats[i] = colors[i] ? colors[i]->format : VK_FORMAT_UNDEFINED;
        const uint32_t t = independent ? i : 0;  // without independent blend every target uses target 0's state
        const Blend& bl = st.blend[t];
        VkPipelineColorBlendAttachmentState& att = blends[i];
        att.blendEnable = ((st.blendMask >> t) & 1) && psReq.outputTypes[i] == 0;  // integer targets cannot blend
        att.srcColorBlendFactor = BlendFactor(bl.colorSrc);
        att.dstColorBlendFactor = BlendFactor(bl.colorDst);
        att.colorBlendOp = BlendOp(bl.colorOp);
        att.srcAlphaBlendFactor = BlendFactor(bl.separate ? bl.alphaSrc : bl.colorSrc);
        att.dstAlphaBlendFactor = BlendFactor(bl.separate ? bl.alphaDst : bl.colorDst);
        att.alphaBlendOp = BlendOp(bl.separate ? bl.alphaOp : bl.colorOp);
        att.colorWriteMask = st.colorWrite && colors[i] ? (st.channelMasks[independent ? i : 0] & 0xF) : 0;
        key.Add(0xC0000000u | i), key.Add(colorFormats[i]);
        key.Add(att.blendEnable | att.srcColorBlendFactor << 1 | att.dstColorBlendFactor << 6 | att.colorBlendOp << 11 |
            att.srcAlphaBlendFactor << 14 | att.dstAlphaBlendFactor << 19 | att.alphaBlendOp << 24 | att.colorWriteMask << 27);
    }
    const VkFormat depthFormat = depth ? depth->format : VK_FORMAT_UNDEFINED;
    const bool hasStencil = depth && (depth->surface->image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    const bool depthClamp = !st.zClip && Dev().Features().depthClamp;
    key.Add(0xD0000000u), key.Add64(vsHash), key.Add64(psHash);
    key.Add(uint32_t(topology) | st.cullFront << 8 | st.cullBack << 9 | (st.frontFace & 1) << 10);
    key.Add(depthFormat);
    if (depth)
    {
        key.Add(st.depthTest | st.depthWrite << 1 | (st.depthFunc & 7) << 2 | (st.stencilTest && hasStencil) << 5 |
            st.backfaceStencil << 6 | st.polygonOffset << 7 | depthClamp << 8);
        for (const StencilFace* f : { &st.stencilFront, &st.stencilBack })
            key.Add((f->func & 7) | (f->zPass & 7) << 3 | (f->zFail & 7) << 6 | (f->fail & 7) << 9);
    }
    for (auto& [b, s] : bindingStride)
    {
        const uint32_t* div = bindingDivisor.find(b);
        key.Add(0xB0000000u | b), key.Add(s), key.Add(div ? *div : 0);
    }
    // Everything the pipeline is made from (its key decides it, so equal keys give equal descriptions).
    auto makeDesc = [&] {
        PipelineDesc d;
        memset(&d, 0, sizeof(d));
        d.vs = vsHash, d.ps = psHash;
        for (auto& [b, stride] : bindingStride)
        {
            const uint32_t* div = bindingDivisor.find(b);
            d.divisors[d.bindingCount] = div && *div > 1 ? *div : 0;
            d.bindings[d.bindingCount++] = { b, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
        }
        for (const VkVertexInputAttributeDescription& a : attrs)
            if (d.attrCount < std::size(d.attrs))
                d.attrs[d.attrCount++] = a;
        d.topology = topology;
        d.cull = gNoCull ? VK_CULL_MODE_NONE
                         : (st.cullFront ? VK_CULL_MODE_FRONT_BIT : 0) | (st.cullBack ? VK_CULL_MODE_BACK_BIT : 0);
        // GX2 (PA_SU_SC_MODE_CNTL.FACE: 0 = counter-clockwise front) decides facing in the hardware's screen space,
        // rows running down from the first in memory. The flipped viewport makes Vulkan's framebuffer that same
        // space, so the winding maps as it is.
        d.frontFace = st.frontFace == 0 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        d.depthBias = depth && st.polygonOffset;
        d.depthClamp = depthClamp;
        if (depth)
        {
            d.depthTest = st.depthTest;
            d.depthWrite = st.depthWrite;
            d.depthCompare = VkCompareOp(st.depthFunc & 7);  // GX2CompareFunction matches VkCompareOp
            d.stencilTest = st.stencilTest && hasStencil;
            auto face = [](const StencilFace& f) {
                VkStencilOpState s{};
                s.failOp = VkStencilOp(f.fail & 7), s.passOp = VkStencilOp(f.zPass & 7);
                s.depthFailOp = VkStencilOp(f.zFail & 7), s.compareOp = VkCompareOp(f.func & 7);
                return s;  // compare/write masks and the reference are dynamic
            };
            d.stencilFront = face(st.stencilFront);
            d.stencilBack = face(st.backfaceStencil ? st.stencilBack : st.stencilFront);
        }
        d.colorCount = colorCount;
        for (uint32_t i = 0; i < colorCount; i++)
            d.colorFormats[i] = colorFormats[i], d.blends[i] = blends[i];
        d.depthFormat = depthFormat;
        d.stencilFormat = hasStencil ? depthFormat : VK_FORMAT_UNDEFINED;
        return d;
    };
    PipelineEntry& entry = FindPipeline(key);
    if (entry.pipeline && entry.verify)
    {
        // Precompiled from a record: built as this build builds it? Otherwise compile it again.
        const PipelineDesc d = makeDesc();
        if (memcmp(&d, entry.verify.get(), sizeof(d)) != 0)
        {
            vkDestroyPipeline(Dev().Handle(), entry.pipeline, nullptr);  // never used yet
            entry.pipeline = VK_NULL_HANDLE;
            gStats.staleRecords++;
        }
        entry.verify.reset();
    }
    if (!entry.pipeline)
    {
        const PipelineDesc d = makeDesc();
        if (async)
        {
            // Built in the background; the draw waits for it within the frame's budget, else is skipped this time.
            if (!entry.pending)
            {
                entry.pending = true;
                const VkShaderModule vsModule = vsm.module, psModule = psm.module;
                const bool record = !gNoCull;  // --no-cull pipelines are not what normal runs need
                QueueCompile([words = key.words, d, vsModule, psModule, record] {
                    const VkPipeline made = CreatePipeline(d, vsModule, psModule);
                    if (record)
                        RecordPipeline(words, d);
                    Install([words, made] {
                        PipelineKey k;
                        k.words = words;
                        PipelineEntry& e = FindPipeline(k);
                        e.pending = false;
                        if (e.pipeline)
                            vkDestroyPipeline(Dev().Handle(), made, nullptr);
                        else
                            e.pipeline = made;
                        gPipelinesUnsaved++;
                        gStats.pipelines++, gStats.asyncPipelines++;
                    });
                });
            }
            if (!WaitForCompile([&] { return FindPipeline(key).pipeline != VK_NULL_HANDLE; }))
            {
                gStats.asyncSkips++, gOpenQuery.forceVisible |= gOpenQuery.active;
                return;
            }
        }
        else
        {
            // (A background build of the same pipeline, if any, is discarded when it arrives.)
            const auto compileStart = std::chrono::steady_clock::now();
            const VkPipeline made = CreatePipeline(d, vsm.module, psm.module);
            FindPipeline(key).pipeline = made;
            gStats.tCompile += std::chrono::duration<double>(std::chrono::steady_clock::now() - compileStart).count();
            gPipelinesUnsaved++;
            gStats.pipelines++;
            if (!gNoCull)  // --no-cull pipelines are not what normal runs need
                RecordPipeline(key.words, d);
        }
    }
    const VkPipeline pipeline = FindPipeline(key).pipeline;

    timer.Lap(gStats.tPipeline);
    // ---- descriptors ----
    VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = CurrentPool();
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &gSetLayout;
    VkDescriptorSet set;
    VK_CHECK(vkAllocateDescriptorSets(Dev().Handle(), &dai, &set));
    static std::vector<VkWriteDescriptorSet> writes;  // (reused: one draw at a time, under gMutex)
    writes.clear();
    for (int stage = 0; stage < 2; stage++)
    {
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.dstBinding = stage ? shader::BindingPsConstants : shader::BindingVsConstants;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &constants[stage];
        writes.push_back(w);
        if (stage == 0)
            for (uint32_t n = 0; n < 16; n++)
                if (vsm.bufferMask & (1u << n))
                {
                    VkWriteDescriptorSet b{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    b.dstSet = set;
                    b.dstBinding = shader::BindingVsBuffers + n;
                    b.descriptorCount = 1;
                    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    b.pBufferInfo = &vtxBuffers[n];
                    writes.push_back(b);
                }
        const uint32_t mask = stage ? psm.textureMask : vsm.textureMask;
        for (uint32_t i = 0; i < 16; i++)
        {
            if (!(mask & (1u << i)))
                continue;
            VkWriteDescriptorSet t{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            t.dstSet = set;
            t.dstBinding = (stage ? shader::BindingPsTextures : shader::BindingVsTextures) + i;
            t.descriptorCount = 1;
            t.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            t.pImageInfo = &images[stage][i];
            writes.push_back(t);
        }
    }
    vkUpdateDescriptorSets(Dev().Handle(), uint32_t(writes.size()), writes.data(), 0, nullptr);

    timer.Lap(gStats.tDescriptors);
    // ---- render ----
    std::array<uint64_t, 9> passKey{};
    for (uint32_t i = 0; i < 8; i++)
        passKey[i] = uint64_t(colors[i] ? colors[i]->view : VK_NULL_HANDLE);
    passKey[8] = uint64_t(depth ? depth->view : VK_NULL_HANDLE);
    if (!gRendering || gPassKey != passKey)
    {
        EndPass();
        std::array<VkRenderingAttachmentInfo, 8> colorInfo{};
        for (uint32_t i = 0; i < colorCount; i++)
        {
            colorInfo[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            if (!colors[i])
                continue;  // a gap: no attachment at this location
            Dev().Transition(cmd, colors[i]->surface->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            colorInfo[i].imageView = colors[i]->view;
            colorInfo[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorInfo[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colorInfo[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depthInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        if (depth)
        {
            Dev().Transition(cmd, depth->surface->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            depthInfo.imageView = depth->view;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { areaW, areaH } };
        ri.layerCount = 1;
        ri.colorAttachmentCount = colorCount;
        ri.pColorAttachments = colorInfo.data();
        ri.pDepthAttachment = depth ? &depthInfo : nullptr;
        ri.pStencilAttachment = hasStencil ? &depthInfo : nullptr;
        GpuMarkHere("pass", colors[0] ? colors[0]->surface->addr : depth ? depth->surface->addr : 0, areaW, areaH);
        vkCmdBeginRendering(cmd, &ri);
        gStats.passes++;
        gRendering = true;
        gPassKey = passKey;
        gPassTargets.clear();
        for (uint32_t i = 0; i < colorCount; i++)
            if (colors[i])
                gPassTargets.push_back({ colors[i]->surface, colors[i]->mip, colors[i]->layer });
        if (depth)
            gPassTargets.push_back({ depth->surface, depth->mip, depth->layer });
    }
    // Only attachments the draw can change are written: color targets with a write mask, depth/stencil with depth
    // or stencil writes. (A draw that only tests depth leaves it as it was, so its snapshot stays current.)
    // A draw that samples the target it blends into (smoke, glass, heat haze: the scene color) reads its snapshot. Its
    // own writes leave that snapshot current, so a run of such effects shares one copy (100-145 full-target copies a
    // frame in heavy fights, each ending the pass: 20 ms of GPU at 1440p). They see the scene as it was before the run,
    // as the Wii U's texture cache largely did. Any other draw into the target makes the next one copy again.
    // DT_EXACT_SNAPSHOTS (environment) copies before every such draw, as before.
    static const bool exactSnapshots = getenv("DT_EXACT_SNAPSHOTS") != nullptr;
    for (uint32_t i = 0; i < colorCount; i++)
        if (colors[i] && blends[i].colorWriteMask)
        {
            GpuSurface& g = *colors[i]->surface;
            const bool current = g.snapshot.image && g.snapshotOf == g.lastWrite;
            MarkWritten(g);
            if (!exactSnapshots && current && std::find(readsOwnTarget.begin(), readsOwnTarget.end(), &g) != readsOwnTarget.end())
                g.snapshotOf = g.lastWrite;
        }
    if (depth && ((st.depthTest && st.depthWrite) ||
            (st.stencilTest && (st.maskFront.write || (st.backfaceStencil && st.maskBack.write)))))
        MarkWritten(*depth->surface);
    // State already recorded in this command buffer is not recorded again (it persists across rendering passes).
    struct Recorded
    {
        uint64_t epoch = ~0ull;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<std::pair<VkBuffer, VkDeviceSize>, 16> vertexBuffers{};
        VkViewport viewport{};
        VkRect2D scissor{};
        float depthBias[3] = {}, blend[4] = {};
        uint8_t stencil[6] = {};
        bool dynamicValid = false;
    };
    static Recorded rec;
    if (rec.epoch != Dev().CommandEpoch())
        rec = {}, rec.epoch = Dev().CommandEpoch();
    if (rec.pipeline != pipeline)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline), rec.pipeline = pipeline;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gPipelineLayout, 0, 1, &set, 0, nullptr);
    for (auto& [b, bind] : vertexBinds)
        if (b >= 16 || rec.vertexBuffers[b] != bind)
        {
            vkCmdBindVertexBuffers(cmd, b, 1, &bind.first, &bind.second);
            if (b < 16)
                rec.vertexBuffers[b] = bind;
        }
    // GX2 viewports are y-up; flip to Vulkan's y-down with a negative height. Targets made at a higher internal
    // resolution scale the game's viewport and scissor with them.
    const float ps2 = attached.empty() ? 1.0f : attached[0]->scale;
    const float* v = st.viewport;
    VkViewport viewport{};
    if (st.viewportSet)
        viewport = { v[0] * ps2, (v[1] + v[3]) * ps2, v[2] * ps2, -v[3] * ps2, v[4], v[5] };
    else
        viewport = { 0, float(areaH), float(areaW), -float(areaH), 0, 1 };
    VkRect2D scissor{ { 0, 0 }, { areaW, areaH } };
    if (st.scissorSet)
    {
        // Keep the scissor inside the render area.
        const uint32_t sx = uint32_t(std::lround(st.scissor[0] * ps2)), sy = uint32_t(std::lround(st.scissor[1] * ps2));
        const uint32_t sw = uint32_t(std::lround(st.scissor[2] * ps2)), sh = uint32_t(std::lround(st.scissor[3] * ps2));
        const int32_t x = int32_t(std::min(sx, areaW)), y = int32_t(std::min(sy, areaH));
        scissor = { { x, y }, { std::min(sw, areaW - uint32_t(x)), std::min(sh, areaH - uint32_t(y)) } };
    }
    const float depthBias[3] = { st.polyOffset, Dev().Features().depthBiasClamp ? st.polyClamp : 0.0f, st.polyScale };
    const StencilMask& back = st.backfaceStencil ? st.maskBack : st.maskFront;
    const uint8_t stencil[6] = { st.maskFront.compare, st.maskFront.write, st.maskFront.ref, back.compare, back.write, back.ref };
    const bool valid = rec.dynamicValid;
    if (!valid || memcmp(&rec.viewport, &viewport, sizeof(viewport)))
        vkCmdSetViewport(cmd, 0, 1, &viewport), rec.viewport = viewport;
    if (!valid || memcmp(&rec.scissor, &scissor, sizeof(scissor)))
        vkCmdSetScissor(cmd, 0, 1, &scissor), rec.scissor = scissor;
    if (!valid || memcmp(rec.depthBias, depthBias, sizeof(depthBias)))
        vkCmdSetDepthBias(cmd, depthBias[0], depthBias[1], depthBias[2]), memcpy(rec.depthBias, depthBias, sizeof(depthBias));
    if (!valid || memcmp(rec.blend, st.blendConstant, sizeof(rec.blend)))
        vkCmdSetBlendConstants(cmd, st.blendConstant), memcpy(rec.blend, st.blendConstant, sizeof(rec.blend));
    if (!valid || memcmp(rec.stencil, stencil, sizeof(stencil)))
    {
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[0]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[1]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[2]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[3]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[4]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[5]);
        memcpy(rec.stencil, stencil, sizeof(stencil));
    }
    rec.dynamicValid = true;
    gStats.draws++;
    if (gStatsOn && gTimestamps)
    {
        // Program hashes (for the slow-GPU-frame lines) read every byte of a program: once per shader, not per draw.
        static std::unordered_map<uint64_t, uint32_t> programHashes;
        auto programHash = [&](uint64_t key, const shader::Request& r) {
            auto [it, fresh] = programHashes.try_emplace(key, 0);
            if (fresh)
                it->second = ProgramHash(r);
            return it->second;
        };
        GpuMarkDraw(programHash(vsHash, vsReq), programHash(psHash, psReq), count, instances);
    }
    {
        // DT_LOG_SHADER=<hash>[,<hash>...] (environment): the first draw with that vertex or pixel shader logs its
        // disassembly, loop constants, target and textures.
        static const std::set<uint32_t> logShaders = [] {
            std::set<uint32_t> out;
            if (const char* v = getenv("DT_LOG_SHADER"))
                for (const char* p = v; *p;)
                {
                    char* end = nullptr;
                    out.insert(uint32_t(strtoul(p, &end, 16)));
                    p = *end ? end + 1 : end;
                }
            return out;
        }();
        static std::set<uint32_t> logged;
        for (const shader::Request* r : { &vsReq, &psReq })
        {
            if (logShaders.empty())
                break;
            const uint32_t h = ProgramHash(*r);
            if (!logShaders.empty() && logShaders.count(h) && logged.insert(h).second)
            {
                std::string loops;
                for (uint32_t i = 0; i < 32; i++)
                    if (r->loopConsts[i])
                        loops += " i" + std::to_string(i) + "=" + Hex(r->loopConsts[i]);
                std::string textures;
                for (uint32_t i = 0; i < 16; i++)
                    if (const uint32_t t = st.textures[r == &psReq ? 1 : 0][i])
                    {
                        const Surface ts = ReadSurface(t);
                        textures += " t" + std::to_string(i) + "=" + Hex(ts.image) + " " + std::to_string(ts.width) + "x" +
                                    std::to_string(ts.height) + "x" + std::to_string(ts.depth) + " fmt " + Hex(ts.format) +
                                    " mips " + std::to_string(ts.mipLevels);
                    }
                dtlog::Important("[gpu] shader %08X (%s) in draw %llu, %u vertices x %u, target %ux%u; loops:%s; textures:%s\n%s",
                    h, r == &psReq ? "pixel" : "vertex", (unsigned long long)gStats.draws, count, instances, areaW, areaH,
                    loops.c_str(), textures.c_str(), r700::Disassemble(r700::Decode(r->program, r->size)).c_str());
            }
        }
    }
    if (gCapturing)
        for (const auto& [stage, prog, size, hash] : { std::tuple{ "vertex", vsReq.program, vsReq.size, uint64_t(ProgramHash(vsReq)) },
                 std::tuple{ "pixel", psReq.program, psReq.size, uint64_t(ProgramHash(psReq)) } })
            if (gCapturedShaders.insert(hash).second)
                dtlog::Write("[capture] %s shader %X:\n%s", stage, uint32_t(hash),
                    r700::Disassemble(r700::Decode(prog, size)).c_str());
    if (gCapturing)
    {
        std::string line = "[capture] draw " + std::to_string(gStats.draws) + " pass " + std::to_string(gCapturePass) +
            ": mode 0x" + Hex(mode) + " count " + std::to_string(count) + " vs " + Hex(ProgramHash(vsReq)) +
            " ps " + Hex(ProgramHash(psReq)) + " depth " + std::to_string(st.depthTest) + std::to_string(st.depthWrite) +
            "/" + std::to_string(st.depthFunc) + " blend 0x" + Hex(st.blendMask) + " (" + std::to_string(st.blend[0].colorSrc) + "," +
            std::to_string(st.blend[0].colorDst) + "," + std::to_string(st.blend[0].colorOp) + (st.blend[0].separate ? " alpha " +
            std::to_string(st.blend[0].alphaSrc) + "," + std::to_string(st.blend[0].alphaDst) + "," + std::to_string(st.blend[0].alphaOp) : "") +
            ") textures:";
        for (uint32_t i = 0; i < 16; i++)
            if ((psm.textureMask >> i) & 1 && st.textures[1][i])
            {
                const Surface ts = ReadTexture(st.textures[1][i]).surface;
                line += " t" + std::to_string(i) + "=0x" + Hex(ts.image) + "(" + std::to_string(ts.width) + "x" +
                    std::to_string(ts.height) + " fmt 0x" + Hex(ts.format) + " dim " + std::to_string(ts.dim) + ")";
            }
        dtlog::Write("%s\n", line.c_str());
        // Interface and constants: pixel inputs (semantic -> matched vertex output location) and c0-c11.
        // Vertex fetch: per attribute stream, the semantic it feeds (and the GPR, or - if the shader has no such
        // semantic), buffer, offset, GX2 format, destination selects, endian swap; then the buffers' strides.
        std::string fetch = "[capture]   attribs:";
        for (const AttribStream& a : fsIt->second)
        {
            int slot = -1;
            for (uint32_t i = 0; i < vs.SemanticCount() && i < 32; i++)
                if ((vs.Semantic(i) & 0xFF) == a.location)
                    slot = int(i);
            fetch += " loc" + std::to_string(a.location) + "->" + (slot < 0 ? std::string("-") : "R" + std::to_string(slot + 1)) +
                " b" + std::to_string(a.buffer) + "+" + std::to_string(a.offset) + " fmt 0x" + Hex(a.format) + " sel 0x" +
                Hex(a.mask) + " swap " + std::to_string(a.endianSwap) + (a.type ? " inst" : "") + ";";
        }
        fetch += " strides:";
        for (auto& [b, stride] : bindingStride)
            fetch += " b" + std::to_string(b) + "=" + std::to_string(stride);
        dtlog::Write("%s\n", fetch.c_str());
        // The first two vertices of each buffer, as guest bytes.
        for (auto& [b, stride] : bindingStride)
        {
            const AttribBuffer& ab = st.attribs[b & 15];
            std::string bytes = "[capture]   b" + std::to_string(b) + " @0x" + Hex(ab.addr) + " size " + std::to_string(ab.size) + ":";
            for (uint32_t i = 0; i < std::min(2 * stride, ab.size) && i < 64; i++)
            {
                char hx[4];
                snprintf(hx, sizeof(hx), "%02x", guest::Read8(ab.addr + i));
                bytes += (i % 4 ? "" : " ") + std::string(hx);
            }
            dtlog::Write("%s\n", bytes.c_str());
        }
        std::string io = "[capture]   vs out ids:";
        for (uint32_t i = 0; i < vs.NumOutputs() && i < 10; i++)
            io += " " + Hex(vs.OutId(i));
        io += " | ps inputs:";
        for (uint32_t i = 0; i < ps.NumInputs() && i < 32; i++)
            io += " " + Hex(ps.InputCntl(i)) + "->" + std::to_string(psReq.pixelInputs[i]);
        io += " | spi_ps_in_control 0x" + Hex(ps.regs[2]) + " 0x" + Hex(ps.regs[3]);
        dtlog::Write("%s\n", io.c_str());
        std::string vcs = "[capture]   vs c0-c19:";
        for (uint32_t i = 0; i < 20; i++)
        {
            char b[80];
            snprintf(b, sizeof(b), " (%g %g %g %g)", st.constants[0][i * 4], st.constants[0][i * 4 + 1],
                st.constants[0][i * 4 + 2], st.constants[0][i * 4 + 3]);
            vcs += b;
        }
        dtlog::Write("%s\n", vcs.c_str());
        std::string cs = "[capture]   ps c0-c19:";
        for (uint32_t i = 0; i < 20; i++)
        {
            char b[80];
            snprintf(b, sizeof(b), " (%g %g %g %g)", st.constants[1][i * 4], st.constants[1][i * 4 + 1],
                st.constants[1][i * 4 + 2], st.constants[1][i * 4 + 3]);
            cs += b;
        }
        dtlog::Write("%s\n", cs.c_str());
    }
    struct RecordLap
    {
        PhaseTimer& t;
        ~RecordLap() { t.Lap(gStats.tRecord); }
    } recordLap{ timer };
    QueryBeforeDraw(cmd);
    if (indexBuffer)
    {
        vkCmdBindIndexBuffer(cmd, indexBuffer, indexOffset, vkIndexType);
        vkCmdDrawIndexed(cmd, drawCount, std::max(1u, instances), 0, vertexOffset, 0);
    }
    else
        vkCmdDraw(cmd, drawCount, std::max(1u, instances), drawFirstVertex, 0);
    gDrawsSinceQueryEnd++;
    // DT_CAPTURE_EACH_DRAW (environment): during a capture, end the pass after every draw, so each draw's result is
    // saved (and DT_CAPTURE_PIXELS logged) on its own.
    static const bool eachDraw = getenv("DT_CAPTURE_EACH_DRAW") != nullptr;
    if (gCapturing && eachDraw)
        EndPass();
}

// Copy one mip/layer of an image (in TRANSFER_SRC layout) into host memory: `bytesPerTexel` per texel, rows packed.
std::vector<uint8_t> Readback(Image& img, VkImageAspectFlags aspect, uint32_t mip, uint32_t layer, uint32_t w, uint32_t h,
    uint32_t bytesPerTexel)
{
    EndPass();
    VkCommandBuffer cmd = Dev().Cmd();
    const VkDeviceSize size = VkDeviceSize(w) * h * bytesPerTexel;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer;
    VK_CHECK(vkCreateBuffer(Dev().Handle(), &bci, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(Dev().Handle(), buffer, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = Dev().MemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;
    VK_CHECK(vkAllocateMemory(Dev().Handle(), &mai, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(Dev().Handle(), buffer, memory, 0));
    EnsureLayout(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { aspect, mip, layer, 1 };
    copy.imageExtent = { w, h, 1 };
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    Dev().SubmitAndWait();
    void* data;
    VK_CHECK(vkMapMemory(Dev().Handle(), memory, 0, size, 0, &data));
    std::vector<uint8_t> bytes(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    vkUnmapMemory(Dev().Handle(), memory);
    vkDestroyBuffer(Dev().Handle(), buffer, nullptr);
    vkFreeMemory(Dev().Handle(), memory, nullptr);
    return bytes;
}

// F12 (host/window.cpp): the TV image at the internal resolution, saved as src/notes/screenshots/<date>_<time>_frame<N>.png
// beside the repository the game runs from (gitignored), or in an installed release's screenshots folder, with the frame
// number in the log.
std::atomic<bool> gUserShot{ false };
std::string gUserShotDir = "src/notes/screenshots";
void SaveUserScreenshot(uint64_t frame)
{
    if (!gScan.image)
        return;
    std::vector<uint8_t> pixels = Readback(gScan, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, gScan.width, gScan.height, 4);
    for (size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;
    std::error_code ec;
    std::filesystem::create_directories(gUserShotDir, ec);
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    const std::string path = gUserShotDir + "/" + std::string(stamp) + "_frame" + std::to_string(frame) + ".png";
    // The PNG is compressed and written on a thread of its own: done here, it held the game up 400 ms per F12.
    const uint32_t w = gScan.width, h = gScan.height;
    std::thread([pixels = std::move(pixels), path, w, h, frame] {
        if (WritePng(path, pixels.data(), w, h, w * 4))
            dtlog::Important("[gpu] F12 screenshot at frame %llu: %s (%ux%u)\n", (unsigned long long)frame, path.c_str(), w, h);
        else
            dtlog::Important("[gpu] F12 screenshot at frame %llu: could not write %s\n", (unsigned long long)frame, path.c_str());
    }).detach();
}

void SaveScreenshot(uint64_t frame)
{
    char shot[64];
    snprintf(shot, sizeof(shot), "/frame_%05llu.png", (unsigned long long)frame);
    if (gShotWindow)
        return Dev().RequestWindowShot(gShotDir + shot);
    if (!gScan.image)
        return;
    std::vector<uint8_t> pixels = Readback(gScan, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, gScan.width, gScan.height, 4);
    for (size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;  // scan-out ignores alpha
    char name[64];
    snprintf(name, sizeof(name), "/frame_%05llu.png", (unsigned long long)frame);
    const std::string path = gShotDir + name;
    if (WritePng(path, pixels.data(), gScan.width, gScan.height, gScan.width * 4))
        dtlog::Important("[gpu] screenshot %s\n", path.c_str());
}

// ---- frame capture (--capture-frame) ----
// Save one render target of a pass: color converted to RGBA8 by a blit (opaque, so alpha-only targets still show),
// depth as grayscale stretched over the range it holds.
void CaptureTarget(const PassTarget& t, const std::string& path)
{
    GpuSurface& g = *t.surface;
    const uint32_t w = std::max(1u, g.width >> t.mip), h = std::max(1u, g.height >> t.mip);
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    if (g.isDepth)
    {
        const std::vector<uint8_t> raw = Readback(g.image, VK_IMAGE_ASPECT_DEPTH_BIT, t.mip, t.layer, w, h, 4);
        float lo = 1e30f, hi = -1e30f;
        for (size_t i = 0; i < size_t(w) * h; i++)
        {
            float d;
            memcpy(&d, &raw[i * 4], 4);
            lo = std::min(lo, d), hi = std::max(hi, d);
        }
        for (size_t i = 0; i < size_t(w) * h; i++)
        {
            float d;
            memcpy(&d, &raw[i * 4], 4);
            const uint8_t v = uint8_t(hi > lo ? (d - lo) / (hi - lo) * 255.0f : 0);
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v, rgba[i * 4 + 3] = 255;
        }
        dtlog::Write("[capture]   depth range %.6f .. %.6f\n", lo, hi);
    }
    else if (g.format == VK_FORMAT_R16G16B16A16_SFLOAT)
    {
        // HDR: report what the values are (NaN, infinity, range), then tone-map x / (1 + x) so bright values stay
        // visible; NaN shows magenta and infinity cyan.
        const std::vector<uint8_t> raw = Readback(g.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mip, t.layer, w, h, 8);
        auto half = [](uint16_t v) {
            const uint32_t sign = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
            float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
            return sign ? -f : f;
        };
        size_t nans = 0, infs = 0;
        float lo[4] = { INFINITY, INFINITY, INFINITY, INFINITY }, hi[4] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };
        for (size_t i = 0; i < size_t(w) * h; i++)
        {
            float c[4];
            bool nan = false, inf = false;
            for (int k = 0; k < 4; k++)
            {
                uint16_t v;
                memcpy(&v, &raw[i * 8 + k * 2], 2);
                c[k] = half(v);
                nan |= std::isnan(c[k]), inf |= std::isinf(c[k]);
                if (std::isfinite(c[k]))
                    lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
            }
            nans += nan, infs += inf && !nan;
            uint8_t* o = &rgba[i * 4];
            if (nan)
                o[0] = 255, o[1] = 0, o[2] = 255;
            else if (inf)
                o[0] = 0, o[1] = 255, o[2] = 255;
            else
                for (int k = 0; k < 3; k++)
                    o[k] = uint8_t(std::clamp(std::max(c[k], 0.0f) / (1.0f + std::max(c[k], 0.0f)), 0.0f, 1.0f) * 255.0f);
            o[3] = 255;
        }
        dtlog::Write("[capture]   RGBA16F: %zu NaN pixels, %zu infinite; r %g..%g g %g..%g b %g..%g a %g..%g\n", nans, infs,
            lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
        // DT_CAPTURE_PIXELS="x,y;x,y;...": those pixels' values, to follow a spot through the frame's passes.
        static const std::vector<std::pair<uint32_t, uint32_t>> probes = [] {
            std::vector<std::pair<uint32_t, uint32_t>> v;
            const char* e = getenv("DT_CAPTURE_PIXELS");
            for (const char* q = e; q && *q;)
            {
                unsigned x, y;
                if (sscanf(q, "%u,%u", &x, &y) == 2)
                    v.push_back({ x, y });
                q = strchr(q, ';');
                q = q ? q + 1 : nullptr;
            }
            return v;
        }();
        for (auto [x, y] : probes)
            if (x < w && y < h)
            {
                float c[4];
                for (int k = 0; k < 4; k++)
                {
                    uint16_t v;
                    memcpy(&v, &raw[(size_t(y) * w + x) * 8 + k * 2], 2);
                    c[k] = half(v);
                }
                dtlog::Write("[capture]   pixel %u,%u: %g %g %g %g\n", x, y, c[0], c[1], c[2], c[3]);
            }
        // Pixels written by a DT_DEBUG_PS shader (alpha exactly 1000): the inspected input's xyz ranges.
        size_t marked = 0, markedBad = 0;
        float mlo[3] = { INFINITY, INFINITY, INFINITY }, mhi[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (size_t i = 0; i < size_t(w) * h; i++)
        {
            uint16_t av;
            memcpy(&av, &raw[i * 8 + 6], 2);
            if (half(av) != 1000.0f)
                continue;
            marked++;
            for (int k = 0; k < 3; k++)
            {
                uint16_t v;
                memcpy(&v, &raw[i * 8 + k * 2], 2);
                const float c = half(v);
                if (!std::isfinite(c))
                    markedBad++;
                else
                    mlo[k] = std::min(mlo[k], c), mhi[k] = std::max(mhi[k], c);
            }
        }
        if (marked)
            dtlog::Write("[capture]   debug pixels: %zu, non-finite components %zu; x %g..%g y %g..%g z %g..%g\n", marked,
                markedBad, mlo[0], mhi[0], mlo[1], mhi[1], mlo[2], mhi[2]);
    }
    else
    {
        if (VkNumberType(g.format) != 0 || !(Dev().FormatFeatures(g.format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            return;  // integer targets cannot be blitted to RGBA8
        ImageDesc d;
        d.width = w, d.height = h, d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        d.createView = false;
        Image tmp = Dev().CreateImage(d);
        EnsureLayout(g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkCommandBuffer cmd = Dev().Cmd();
        Dev().Transition(cmd, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, t.mip, t.layer, 1 };
        blit.srcOffsets[1] = { int32_t(w), int32_t(h), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
        vkCmdBlitImage(cmd, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);
        rgba = Readback(tmp, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, w, h, 4);
        Dev().DestroyImage(tmp);
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
    }
    WritePng(path, rgba.data(), w, h, w * 4);
}

// Save a sampled texture's level 0 (first slice) as the GPU holds it, once per contents, and log how it is viewed.
void CaptureTexture(GpuSurface& g, const Texture& t)
{
    if (!gCapturedTextures.insert({ &g, g.lastWrite ^ g.sample }).second)
        return;
    char name[128];
    snprintf(name, sizeof(name), "/tex_%08X_%ux%u_fmt%X_dim%u.png", g.addr, g.width, g.height, t.surface.format, t.surface.dim);
    dtlog::Write("[capture] texture 0x%08X %ux%u gx2 format 0x%X dim %u mips %u tile %u swizzle 0x%X compMap 0x%08X view "
                 "mips %u+%u slices %u+%u -> %s\n", g.addr, g.width, g.height, t.surface.format, t.surface.dim,
        t.surface.mipLevels, t.surface.tileMode, t.surface.swizzle, t.compMap, t.viewFirstMip, t.viewNumMips,
        t.viewFirstSlice, t.viewNumSlices, name + 1);
    CaptureTarget({ &g, 0, 0 }, gCaptureDir + name);
    // (smaller levels too: what draws further away sample)
    for (uint32_t mip = 1; mip < std::min(g.levels, 4u); mip++)
    {
        char mipName[160];
        snprintf(mipName, sizeof(mipName), "/tex_%08X_%ux%u_fmt%X_dim%u_mip%u.png", g.addr, g.width, g.height,
            t.surface.format, t.surface.dim, mip);
        CaptureTarget({ &g, mip, 0 }, gCaptureDir + mipName);
    }
}

void CapturePass(const std::vector<PassTarget>& targets)
{
    const uint32_t pass = gCapturePass++;
    for (size_t i = 0; i < targets.size(); i++)
    {
        const PassTarget& t = targets[i];
        char name[128];
        snprintf(name, sizeof(name), "/pass%03u_%s%zu_%08X_%dx%d_mip%u.png", pass, t.surface->isDepth ? "depth" : "color", i,
            t.surface->addr, std::max(1u, t.surface->width >> t.mip), std::max(1u, t.surface->height >> t.mip), t.mip);
        dtlog::Write("[capture] pass %u target %zu: surface 0x%08X %ux%u format %d mip %u layer %u -> %s\n", pass, i,
            t.surface->addr, t.surface->width, t.surface->height, int(t.surface->format), t.mip, t.layer, name + 1);
        CaptureTarget(t, gCaptureDir + name);
    }
}
} // namespace

// The pipeline cache on disk: compiled pipelines survive between runs, so a state seen before is not compiled
// again on first use (some drivers keep their own cache; this makes it independent of the driver).
std::string PipelineCachePath() { return gCacheDir.empty() ? "" : gCacheDir + "/pipelines.bin"; }

void LoadPipelineCache()
{
    std::vector<char> data;
    if (const std::string path = PipelineCachePath(); !path.empty())
        if (std::ifstream in(path, std::ios::binary); in)
            data.assign(std::istreambuf_iterator<char>(in), {});
    // VkPipelineCacheHeaderVersionOne: header size, version, vendor, device, pipelineCacheUUID. Data from another
    // GPU or driver is dropped (drivers are expected to ignore it, but not all are trusted to).
    const VkPhysicalDeviceProperties& p = Dev().Properties();
    uint32_t h[4] = {};
    if (data.size() >= 32)
        memcpy(h, data.data(), 16);
    if (data.size() < 32 || h[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h[2] != p.vendorID || h[3] != p.deviceID ||
        memcmp(data.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        data.clear();
    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    VK_CHECK(vkCreatePipelineCache(Dev().Handle(), &ci, nullptr, &gPipelineCache));
    if (!data.empty())
        dtlog::Important("[gpu] pipeline cache: %zu KB from %s\n", data.size() / 1024, PipelineCachePath().c_str());
}

// Builds every pipeline recorded in pipelines.keys on low-priority worker threads, then hands them (and the shader
// modules they need) to the renderer: a draw then finds its pipeline ready instead of compiling it on first use. The
// driver's and our pipeline caches make each build quick. Runs in the background while the game boots. Shaders this
// build has not translated yet (after a translator or renderer change) are translated from programs.bin first.
void Precompile()
{
    struct Record
    {
        std::vector<uint32_t> key;
        PipelineDesc desc;
    };
    std::vector<Record> records;
    size_t duplicates = 0;
    {
        std::lock_guard lock(gPipelineKeysMutex);
        std::ifstream in(PipelineKeysPath(), std::ios::binary);
        char magic[8] = {};
        uint32_t descSize = 0;
        in.read(magic, 8).read(reinterpret_cast<char*>(&descSize), 4);
        if (!in || memcmp(magic, PipelineKeysMagic, 8) != 0 || descSize != sizeof(PipelineDesc))
        {
            if (in)
                dtlog::Important("[gpu] %s is from another version; starting a new one\n", PipelineKeysPath().c_str());
            in.close();
            std::error_code ec;
            std::filesystem::remove(PipelineKeysPath(), ec);
        }
        std::map<std::vector<uint32_t>, size_t> seen;  // key -> its record (a later record replaces an earlier one)
        for (; in && memcmp(magic, PipelineKeysMagic, 8) == 0 && descSize == sizeof(PipelineDesc);)
        {
            uint32_t n = 0;
            if (!in.read(reinterpret_cast<char*>(&n), 4) || n > 4096)
                break;
            Record r;
            r.key.resize(n);
            if (!in.read(reinterpret_cast<char*>(r.key.data()), std::streamsize(n * 4)) ||
                !in.read(reinterpret_cast<char*>(&r.desc), sizeof(PipelineDesc)))
                break;
            auto [it, added] = seen.emplace(r.key, records.size());
            if (added)
                records.push_back(std::move(r));
            else
                records[it->second] = std::move(r), duplicates++;
        }
    }
    const auto start = std::chrono::steady_clock::now();

    // Shader modules from the SPIR-V cache: every program the game has shown (some are needed by pipelines not
    // recorded yet), and every shader a recorded pipeline uses.
    std::vector<uint64_t> hashes = programs::Hashes();
    if (records.empty() && hashes.empty())
        return;
    for (const Record& r : records)
        hashes.push_back(r.desc.vs), hashes.push_back(r.desc.ps);
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    std::unordered_map<uint64_t, ShaderModule> modules;
    std::vector<VkPipeline> pipelines(records.size(), VK_NULL_HANDLE);
    std::mutex modulesMutex;
    const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 8u);
    auto parallel = [&](size_t count, auto&& fn) {
        std::atomic<size_t> next{ 0 };
        std::vector<std::thread> threads;
        for (unsigned w = 0; w < workers; w++)
            threads.emplace_back([&] {
                platform::LowerCurrentThreadPriority();
                for (size_t i; (i = next++) < count;)
                    fn(i);
            });
        for (auto& t : threads)
            t.join();
    };
    std::atomic<size_t> translated{ 0 };
    parallel(hashes.size(), [&](size_t i) {
        char name[32];
        snprintf(name, sizeof(name), "/%016llx.spv", (unsigned long long)hashes[i]);
        shader::Result r;
        if (!ReadShaderCache(gCacheDir + name, r) || r.spirv.empty())
        {
            // Not translated by this build yet: from the game's program, as saved when it was first seen.
            shader::Request req;
            std::vector<uint8_t> storage;
            if (!programs::Find(hashes[i], req, storage) || shader::Hash(req) != hashes[i])
                return;
            r = shader::Translate(req);
            if (!r.error.empty() || r.spirv.empty())
                return;
            WriteShaderCache(gCacheDir + name, r);
            translated++;
        }
        ShaderModule m;
        m.textureMask = r.textureMask, m.outputMask = r.outputMask, m.bufferMask = r.bufferMask;
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = r.spirv.size() * 4;
        ci.pCode = r.spirv.data();
        if (vkCreateShaderModule(Dev().Handle(), &ci, nullptr, &m.module) != VK_SUCCESS)
            return;
        std::lock_guard lock(modulesMutex);
        modules[hashes[i]] = m;
    });
    size_t missing = 0;
    parallel(records.size(), [&](size_t i) {
        auto vs = modules.find(records[i].desc.vs), ps = modules.find(records[i].desc.ps);
        if (vs == modules.end() || ps == modules.end())
            return;
        pipelines[i] = CreatePipeline(records[i].desc, vs->second.module, ps->second.module);
    });

    // Hand them over; anything the renderer made meanwhile wins and our copy is destroyed.
    size_t built = 0;
    {
        std::lock_guard lock(gMutex);
        for (auto& [hash, m] : modules)
            if (!gShaders.emplace(hash, m).second)
                vkDestroyShaderModule(Dev().Handle(), m.module, nullptr);
        for (size_t i = 0; i < records.size(); i++)
        {
            if (!pipelines[i])
            {
                missing++;
                continue;
            }
            PipelineKey key;
            key.words = records[i].key;
            PipelineEntry& slot = FindPipeline(key);
            if (slot.pipeline)
                vkDestroyPipeline(Dev().Handle(), pipelines[i], nullptr);
            else
                slot.pipeline = pipelines[i], slot.verify = std::make_shared<PipelineDesc>(records[i].desc), built++;
        }
    }
    dtlog::Important("[gpu] precompiled %zu pipelines and %zu shaders (%zu translated from the program library) in %.1f s "
                     "on %u threads (%zu without their shaders' programs, %zu replaced records)\n", built, modules.size(),
        translated.load(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), workers, missing,
        duplicates);
    // Rewrite the record file without duplicates.
    if (duplicates)
    {
        std::lock_guard lock(gPipelineKeysMutex);
        std::error_code ec;
        std::filesystem::remove(PipelineKeysPath(), ec);
    }
    if (duplicates)
        for (const Record& r : records)
            RecordPipeline(r.key, r.desc);
}

// Saves the pipeline cache if pipelines were created since the last save. The file is written on another thread
// (a temporary file renamed into place), so saving does not hold up the frame.
// Serialising the cache (70 MB and more after long play) takes the driver about 20 ms, so it runs on a low-priority
// thread, as does the file write: on the presenting thread it held up a frame every save. Vulkan allows
// reading the cache while pipelines are created into it. One save at a time; `wait` (at exit) waits for it.
std::thread& gPipelineSaver = *new std::thread;  // never destroyed: a quick exit must not find it joinable
std::atomic<bool> gPipelineSaverDone{ true };

void SavePipelineCache(bool wait)
{
    const std::string path = PipelineCachePath();
    if (!gPipelineCache || path.empty())
        return;
    if (gPipelineSaver.joinable() && (wait || gPipelineSaverDone))
        gPipelineSaver.join();
    if (gPipelineSaver.joinable() || !gPipelinesUnsaved)
        return;  // the previous save is still running: the next interval picks these up
    gPipelinesUnsaved = 0;
    gPipelineSaverDone = false;
    gPipelineSaver = std::thread([path, wait, device = Dev().Handle(), cache = gPipelineCache] {
        if (!wait)
            platform::LowerCurrentThreadPriority();
        size_t size = 0;
        std::vector<char> data;
        if (vkGetPipelineCacheData(device, cache, &size, nullptr) == VK_SUCCESS && size)
        {
            data.resize(size);
            if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS)
                data.clear();
            data.resize(std::min(size, data.size()));
        }
        if (!data.empty())
        {
            const std::string tmp = path + ".tmp";
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(data.data(), std::streamsize(data.size()));
            out.close();
            std::error_code ec;
            if (out)
                std::filesystem::rename(tmp, path, ec);
        }
        gPipelineSaverDone = true;
    });
    if (wait)
        gPipelineSaver.join();
}

// The caches of this build: <dir>/<version>, the version being a hash of the translator's and renderer's sources
// (cache_version.h), so a change to either never reuses shaders or pipelines made by an older build. Older versions'
// folders, and the files of the unversioned layout (<dir>/*.spv, pipelines.bin), are deleted. What survives builds is
// the game's own data: <dir>/programs.bin and <dir>/pipelines.keys, from which Precompile rebuilds the rest.
std::string VersionedCacheDir(const std::string& dir)
{
    if (dir.empty())
        return "";
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string version = DT_SHADER_CACHE_VERSION;
    std::vector<fs::path> old;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        const std::string name = e.path().filename().string();
        const bool oldVersion = e.is_directory(ec) && name.size() == version.size() && name != version &&
                                name.find_first_not_of("0123456789abcdef") == std::string::npos;
        const bool oldFile = e.is_regular_file(ec) && (e.path().extension() == ".spv" || name == "pipelines.bin");
        if (oldVersion || oldFile)
            old.push_back(e.path());
    }
    size_t removed = 0;
    for (const fs::path& p : old)
        removed += fs::remove_all(p, ec) > 0;
    const std::string out = dir + "/" + version;
    fs::create_directories(out, ec);
    dtlog::Important("[gpu] shader cache %s%s\n", out.c_str(),
                     removed ? (" (" + std::to_string(removed) + " entries of older builds removed)").c_str() : "");
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------------------
void Init(const std::string& shaderCacheDir)
{
    std::lock_guard lock(gMutex);
    if (!GetDevice().Ready())
        return;
    gBaseCacheDir = shaderCacheDir;
    gCacheDir = VersionedCacheDir(shaderCacheDir);
    if (!gBaseCacheDir.empty())
        programs::Open(gBaseCacheDir + "/programs.bin");
    gAsync = settings::Get("graphics.async_shaders", "on") != "off";
    const std::string res = settings::Get("graphics.resolution", "720p");
    {
        const std::string sq = settings::Get("graphics.shadow_quality", "1x");
        gShadowScale = sq == "4x" ? 4.0f : sq == "2x" ? 2.0f : 1.0f;
        if (gShadowScale != 1.0f)
            dtlog::Important("[gpu] shadow maps %.0fx the game's size (graphics.shadow_quality)\n", gShadowScale);
    }
    gRenderScale = res == "1080p" ? 1.5f : res == "1440p" ? 2.0f : res == "2160p" ? 3.0f
                 : res == "custom" ? std::clamp(settings::GetFloat("graphics.render_scale", 2.0f), 1.0f, 4.0f) : 1.0f;
    if (gRenderScale != 1.0f)
        dtlog::Important("[gpu] internal resolution %ux%u (%.2fx the game's 1280x720)\n", uint32_t(std::lround(1280 * gRenderScale)),
                         uint32_t(std::lround(720 * gRenderScale)), gRenderScale);
    CreateLayouts();
    LoadPipelineCache();
    GetDevice().SetBeforeSubmit(EndPass);
    gActive = true;
    if (!gCacheDir.empty() && !gNoCull)
        std::thread(Precompile).detach();
}

bool Active() { return gActive; }

void Report(const std::string& message)
{
    std::lock_guard lock(gMutex);
    ReportOnce(message);
}

void Shutdown()
{
    if (!gActive)
        return;
    StopCompileWorkers();  // (before gMutex: a finishing job takes it to hand over)
    // Another thread may be mid-draw: wait for it briefly, then let the GPU finish.
    if (gMutex.try_lock_for(std::chrono::seconds(2)))
    {
        vkDeviceWaitIdle(Dev().Handle());
        SavePipelineCache(true);
    }
    else if (gPipelineSaver.joinable())
        gPipelineSaver.join();
}

void SetupContextState(uint32_t context)
{
    std::lock_guard lock(gMutex);
    const uint32_t copies = gContexts[context].copies;
    gContexts[context] = State{};
    gContexts[context].copies = copies;
    gState = &gContexts[context];
}

void SetContextState(uint32_t context)
{
    std::lock_guard lock(gMutex);
    if (!context)
        return;
    gState = &gContexts[context];  // creates a default state if this context was never set up
}

void SetColorBuffer(uint32_t colorBuffer, uint32_t target)
{
    std::lock_guard lock(gMutex);
    if (target < 8)
        gState->colorBuffers[target] = StateCopy(*gState, SlotColor + target, colorBuffer, 0x9C);
}

void SetDepthBuffer(uint32_t depthBuffer)
{
    std::lock_guard lock(gMutex);
    gState->depthBuffer = StateCopy(*gState, SlotDepth, depthBuffer, 0xAC);
}

void ClearColor(uint32_t colorBuffer, float r, float g, float b, float a)
{
    std::lock_guard lock(gMutex);
    EndPass();  // transfer commands cannot run inside a rendering pass
    const ColorBuffer cb = ReadColorBuffer(colorBuffer);
    GpuSurface* gs = GetSurface(cb.surface, false, gRenderScale, true);
    if (!gs)
        return;
    EnsureLayout(gs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue c{};
    c.float32[0] = r, c.float32[1] = g, c.float32[2] = b, c.float32[3] = a;
    const uint32_t layers = gs->is3D ? 1 : gs->depth;
    const uint32_t first = std::min(cb.viewFirstSlice, layers - 1);
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, std::min(cb.viewMip, gs->levels - 1), 1, first,
        std::clamp(cb.viewNumSlices, 1u, layers - first) };
    GpuMarkHere("clear", gs->addr, gs->width, gs->height);
    vkCmdClearColorImage(Dev().Cmd(), gs->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    // Order this write before the next one into the image (a clear of another slice, say).
    Dev().Transition(Dev().Cmd(), gs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    MarkWritten(*gs);
}

void ClearDepthStencil(uint32_t depthBuffer, float depth, uint8_t stencil, uint32_t flags)
{
    std::lock_guard lock(gMutex);
    EndPass();  // transfer commands cannot run inside a rendering pass
    const DepthBuffer db = ReadDepthBuffer(depthBuffer);
    GpuSurface* gs = GetSurface(db.surface, true, gRenderScale, true);
    if (!gs)
        return;
    VkImageAspectFlags aspect = 0;
    if (flags & 1)
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & 2) && (gs->image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect)
        return;
    EnsureLayout(gs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue value{ depth, stencil };
    const uint32_t first = std::min(db.viewFirstSlice, gs->depth - 1);
    const VkImageSubresourceRange range{ aspect, std::min(db.viewMip, gs->levels - 1), 1, first,
        std::clamp(db.viewNumSlices, 1u, gs->depth - first) };
    GpuMarkHere("clear depth", gs->addr, gs->width, gs->height);
    vkCmdClearDepthStencilImage(Dev().Cmd(), gs->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    // Order this write before the next one into the image (a clear of another slice, say).
    Dev().Transition(Dev().Cmd(), gs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    MarkWritten(*gs);
}

// GX2CopySurface writes the destination's memory on the hardware. The game loads every texture level into staging
// memory and copies it into the texture this way (as uncompressed views of the blocks), and streaming moves mips into
// a texture's new allocation the same way. Made only in the renderer's images, the texture's memory kept whatever was
// there before, and anything that re-uploaded it (a CPU write nearby, the levels a block view lacks) showed other
// textures. So copies are made in guest memory too, element for element, when the source's memory is current. The
// copy runs before GX2CopySurface returns (gx2.cpp), so the source is read before the game reuses it.
bool CopyInGuest(const Surface& ss, uint32_t srcLevel, uint32_t srcSlice, const Surface& ds, uint32_t dstLevel, uint32_t dstSlice)
{
    if (ss.dim == surface::Dim3D || ds.dim == surface::Dim3D || ss.aa || ds.aa ||
        ((ss.use | ds.use) & surface::UseDepthBuffer) || !ss.image || !ds.image)
        return false;
    if (srcLevel >= std::max(1u, ss.mipLevels) || dstLevel >= std::max(1u, ds.mipLevels))
        return false;
    const surface::Level a = surface::ComputeLevel(ss, srcLevel), b = surface::ComputeLevel(ds, dstLevel);
    if (a.bpp != b.bpp || !a.bpp || srcSlice >= a.depth || dstSlice >= b.depth)
        return false;
    static std::vector<uint8_t> from, to;  // (under gMutex)
    surface::Detile(ss, srcLevel, from);
    if (a.width == b.width && a.height == b.height && a.depth == 1 && b.depth == 1)
    {
        surface::Tile(ds, dstLevel, from);
        return true;
    }
    surface::Detile(ds, dstLevel, to);
    const size_t bpe = a.bpp / 8;
    const uint32_t w = std::min(a.width, b.width), h = std::min(a.height, b.height);
    for (uint32_t y = 0; y < h; y++)
        memcpy(&to[((size_t(dstSlice) * b.height + y) * b.width) * bpe], &from[((size_t(srcSlice) * a.height + y) * a.width) * bpe],
            w * bpe);
    surface::Tile(ds, dstLevel, to);
    return true;
}

void CopySurface(uint32_t srcAddr, uint32_t srcLevel, uint32_t srcSlice, uint32_t dstAddr, uint32_t dstLevel, uint32_t dstSlice)
{
    std::lock_guard lock(gMutex);
    EndPass();  // transfer commands cannot run inside a rendering pass
    GpuMarkHere("copy", dstAddr);
    const Surface ss = ReadSurface(srcAddr), ds = ReadSurface(dstAddr);
    // Use what the GPU has for the source if it rendered it; otherwise its guest data.
    GpuSurface* src = LatestWritten(ss.image);
    if (src)
        Upload(*src, src->desc);  // (only levels the CPU rewrote since, if any)
    else if ((src = GetSurface(ss, false)))
        Upload(*src, ss, false, 1u << std::min(srcLevel, src->levels - 1));  // the copied level as it is now
    if (!src)
        return;
    // A copy of a render target made at a higher resolution keeps that resolution when it is the same size.
    GpuSurface* dst = GetSurface(ds, src->isDepth, ds.width == src->guestWidth && ds.height == src->guestHeight ? src->scale : 1.0f);
    if (!dst || dst == src)
        return;
    if (gLogCopies)
        dtlog::Important("[copies] f%llu copy %08X %ux%u fmt %X lv %u/%u (alias %ux%u vk%d owned %d gpuLevels %X) -> %08X %ux%u fmt %X lv %u/%u (alias vk%d owned %d gpuLevels %X)\n",
            (unsigned long long)Dev().FrameIndex(), ss.image, ss.width, ss.height, ss.format, srcLevel, ss.mipLevels,
            src->guestWidth, src->guestHeight, int(src->format), src->gpuOwned, src->gpuLevels, ds.image, ds.width, ds.height,
            ds.format, dstLevel, ds.mipLevels, int(dst->format), dst->gpuOwned, dst->gpuLevels);
    // A rendered alias of the source memory in another number type (an R8 target where the copy reads 64-bit blocks,
    // say) cannot be converted into the destination by a blit; use the source as described, from its guest data.
    if (!src->isDepth && src->format != dst->format && !BlockCompatible(src->format, dst->format) &&
        VkNumberType(src->format) != VkNumberType(dst->format))
        if (GpuSurface* described = GetSurface(ss, false); described && described != dst)
        {
            if (described != src)
                Upload(*described, ss, false);
            src = described;
        }
    // Made in guest memory first (so the destination's own upload below already sees it), when the source's is current.
    const bool dstCurrent = !dst->gpuOwned || dst->mirrored;
    const bool inGuest = (!src->gpuOwned || src->mirrored) && !src->isDepth && !dst->isDepth &&
        CopyInGuest(ss, srcLevel, srcSlice, ds, dstLevel, dstSlice);
    if (inGuest)
    {
        // The copy is in guest memory, which now holds the destination's contents: everything over the written level
        // uploads from there on its next use, as after a CPU write. (Kept on the GPU, the copies needed ownership
        // tracking per alias and level, which kept showing other textures' data on distant surfaces.)
        gStats.guestCopies++;
        const surface::Level lv = surface::ComputeLevel(ds, dstLevel);
        gSurfaceRanges.ForOverlaps(surface::LevelAddress(ds, dstLevel), uint32_t(std::min<uint64_t>(lv.size, 0xFFFFFFFFu)),
            [](uint64_t key) {
                GpuSurface& g = gSurfaces[key];
                if (g.isDepth)
                    return;
                SetOwned(g, false);
                g.cpuDirty = true;
            });
        if (!dst->isDepth)
            SetOwned(*dst, false), dst->cpuDirty = true;
        return;
    }
    // Levels past the image's chain (a block view has fewer levels than its texture: 1x1 blocks for the last ones)
    // exist only in guest memory: the texture uploads them from there.
    if (srcLevel >= src->levels || dstLevel >= dst->levels)
    {
        if (inGuest && !dst->gpuOwned)
            dst->cpuDirty = true;  // (uploads again on its next use)
        return;
    }
    Upload(*dst, ds);  // the rest of the destination keeps its guest data
    const uint32_t sMip = srcLevel, dMip = dstLevel;
    // Marked per level, so that when the CPU then writes another level (texture streaming copies the lower mips into
    // a new allocation, then loads the new top level from disk) this one is kept, not re-uploaded from guest memory.
    auto markWritten = [&] {
        if (dst->scale == 1.0f && !dst->shadow)
            MarkWritten(*dst, int(dMip), &ds);
        else
            MarkWritten(*dst);
        dst->mirrored = dstCurrent && inGuest && dst->gpuLevels != AllLevels;
        if (inGuest)
            dst->ownedSample = GuestSample(*dst);  // (its memory changed with the copy: not a CPU overwrite)
    };
    const uint32_t sw = std::max(1u, src->width >> sMip), sh = std::max(1u, src->height >> sMip);
    const uint32_t dw = std::max(1u, dst->width >> dMip), dh = std::max(1u, dst->height >> dMip);
    EnsureLayout(src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EnsureLayout(dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkCommandBuffer cmd = Dev().Cmd();
    const VkImageSubresourceLayers sl{ src->image.aspect, sMip, src->is3D ? 0 : std::min(srcSlice, src->depth - 1), 1 };
    const VkImageSubresourceLayers dl{ dst->image.aspect, dMip, dst->is3D ? 0 : std::min(dstSlice, dst->depth - 1), 1 };
    const int32_t sz = src->is3D ? int32_t(srcSlice) : 0, dz = dst->is3D ? int32_t(dstSlice) : 0;
    if (BlockCompatible(src->format, dst->format))
    {
        // Compressed data copied as uncompressed elements of the block size (texture streaming), or back.
        CopyBlocks(*src, *dst, { { sMip, sl.baseArrayLayer, dMip, dl.baseArrayLayer, 1 } });
        if (sz || dz)
            ReportOnce("GX2CopySurface of compressed blocks between 3D slices is not supported");
        markWritten();
        return;
    }
    if (src->format == dst->format && sw == dw && sh == dh)
    {
        VkImageCopy c{};
        c.srcSubresource = sl, c.dstSubresource = dl;
        c.srcOffset = { 0, 0, sz }, c.dstOffset = { 0, 0, dz };
        c.extent = { sw, sh, 1 };
        vkCmdCopyImage(cmd, src->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    else if (!src->isDepth && (Dev().FormatFeatures(src->format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        (Dev().FormatFeatures(dst->format) & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
        VkNumberType(src->format) == VkNumberType(dst->format))
    {
        // Different formats or sizes: a scaling blit converts between them. Integer data, and formats without linear
        // filtering, are scaled by nearest texel.
        const bool linear = VkNumberType(src->format) == 0 &&
            (Dev().FormatFeatures(src->format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        VkImageBlit bl{};
        bl.srcSubresource = sl, bl.dstSubresource = dl;
        bl.srcOffsets[0] = { 0, 0, sz }, bl.srcOffsets[1] = { int32_t(sw), int32_t(sh), sz + 1 };
        bl.dstOffsets[0] = { 0, 0, dz }, bl.dstOffsets[1] = { int32_t(dw), int32_t(dh), dz + 1 };
        vkCmdBlitImage(cmd, src->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    else
    {
        ReportOnce("GX2CopySurface between formats 0x" + Hex(ss.format) + " and 0x" + Hex(ds.format) + " (Vulkan " +
            std::to_string(src->format) + " -> " + std::to_string(dst->format) + ") is not supported");
        return;
    }
    // Order this write before the next one into the image (copies into each cube face, say).
    Dev().Transition(cmd, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    markWritten();
}

void ConvertDepthBuffer(uint32_t depthBuffer, uint32_t dstSurface, uint32_t level, uint32_t slice)
{
    std::lock_guard lock(gMutex);
    EndPass();  // transfer commands cannot run inside a rendering pass
    const DepthBuffer db = ReadDepthBuffer(depthBuffer);
    GpuSurface* latest = LatestWritten(db.surface.image, true);
    if (!latest)
        return;  // never rendered: nothing on the GPU to convert
    GpuSurface& src = *latest;
    const Surface dsts = ReadSurface(dstSurface);
    GpuSurface* dst = GetSurface(dsts, false, dsts.width == src.guestWidth && dsts.height == src.guestHeight ? src.scale : 1.0f);
    if (!dst || dst->isDepth || dst->is3D)
        return;
    if (dst->format != VK_FORMAT_R32_SFLOAT)
    {
        ReportOnce("depth converted to a texture of format other than 32-bit float is not supported");
        return;
    }
    if (dst->scale != src.scale)
        ReportOnce("depth converted between surfaces of different internal resolution: only part of it is copied");
    const uint32_t srcMip = std::min(db.viewMip, src.levels - 1), dstMip = std::min(level, dst->levels - 1);
    const uint32_t w = std::min(std::max(1u, src.width >> srcMip), std::max(1u, dst->width >> dstMip));
    const uint32_t h = std::min(std::max(1u, src.height >> srcMip), std::max(1u, dst->height >> dstMip));
    // Depth and color images cannot be copied into each other directly; go through a buffer (float depth).
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    EnsureScratch(bytes);
    EnsureLayout(src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EnsureLayout(dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkCommandBuffer cmd = Dev().Cmd();
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, srcMip, std::min(db.viewFirstSlice, src.depth - 1), 1 };
    copy.imageExtent = { w, h, 1 };
    vkCmdCopyImageToBuffer(cmd, src.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScratch, 1, &copy);
    ScratchBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, dstMip, std::min(slice, dst->depth - 1), 1 };
    vkCmdCopyBufferToImage(cmd, gScratch, dst->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Dev().Transition(cmd, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // before the next write into it
    // The next user of the scratch buffer writes it: order that after this copy-in.
    ScratchBarrier(cmd, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    MarkWritten(*dst);
}

void SetViewport(float x, float y, float w, float h, float zNear, float zFar)
{
    std::lock_guard lock(gMutex);
    float* v = gState->viewport;
    v[0] = x, v[1] = y, v[2] = w, v[3] = h, v[4] = zNear, v[5] = zFar;
    gState->viewportSet = true;
}

void SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::lock_guard lock(gMutex);
    uint32_t* s = gState->scissor;
    s[0] = x, s[1] = y, s[2] = w, s[3] = h;
    gState->scissorSet = true;
}

void RelocateSurfaces(uint32_t dst, uint32_t src, uint32_t bytes)
{
    std::lock_guard lock(gMutex);
    const uint64_t end = uint64_t(src) + bytes;
    auto inside = [&](uint32_t addr, uint32_t size) { return addr >= src && uint64_t(addr) + std::max(size, 1u) <= end; };
    // GPU-written surfaces wholly inside the source: their data exists only in their images. Taken out first, so that
    // none is mistaken for a stale alias at the destination while the others move.
    std::vector<decltype(gSurfaces)::node_type> moving;
    for (auto it = gSurfaces.begin(); it != gSurfaces.end();)
    {
        GpuSurface& g = it->second;
        if (g.gpuOwned && inside(g.addr, g.imageSize) && (!g.mipAddr || inside(g.mipAddr, g.mipSize)))
        {
            const uint64_t key = it->first;
            gSurfaceRanges.Remove(g.indexedImage, key);
            gSurfaceRanges.Remove(g.indexedMip, key);
            auto& aliases = gAtAddr[g.addr];
            aliases.erase(std::remove(aliases.begin(), aliases.end(), key), aliases.end()), gOwnedGen++;
            SetOwned(g, false);  // (owned again at its new address below)
            moving.push_back(gSurfaces.extract(it++));
        }
        else
            ++it;
    }
    if (moving.empty())
        return;
    const int64_t delta = int64_t(dst) - int64_t(src);
    for (auto& node : moving)
    {
        GpuSurface& g = node.mapped();
        g.addr = uint32_t(int64_t(g.addr) + delta);
        if (g.mipAddr)
            g.mipAddr = uint32_t(int64_t(g.mipAddr) + delta);
        for (uint32_t l = 0; l < 14; l++)
            if (g.gpuLevels != AllLevels && (g.gpuLevels >> l & 1))
                g.levelAddr[l] = uint32_t(int64_t(g.levelAddr[l]) + delta);
        g.desc.image = g.addr;
        if (g.desc.mipmaps)
            g.desc.mipmaps = uint32_t(int64_t(g.desc.mipmaps) + delta);
        const uint64_t key = SurfaceKey(g.addr, g.guestWidth, g.guestHeight, g.depth, g.levels, g.format, g.is3D,
            g.dim == surface::DimCube);
        if (gSurfaces.count(key))
        {
            // An alias of the same description at the destination: its memory now holds the moved surface.
            RetireSurface(key);
            auto& aliases = gAtAddr[g.addr];
            aliases.erase(std::remove(aliases.begin(), aliases.end(), key), aliases.end()), gOwnedGen++;
        }
        gSurfaceRanges.Add(g.addr, g.imageSize, key);
        gSurfaceRanges.Add(g.mipAddr, g.mipSize, key);
        g.indexedImage = g.addr, g.indexedImageSize = g.imageSize, g.indexedMip = g.mipAddr, g.indexedMipSize = g.mipSize;
        node.key() = key;
        SetOwned(g, true);
        gAtAddr[g.addr].push_back({ key, &node.mapped() }), gOwnedGen++;  // the node keeps its element's address when inserted
        gSurfaces.insert(std::move(node));
    }
    gStats.relocated += moving.size();
}

void InitFetchShader(uint32_t fetchShader, uint32_t attribCount, uint32_t attribs)
{
    std::lock_guard lock(gMutex);
    gFetchShaders[fetchShader] = ReadAttribStreams(attribs, attribCount, 0x20);
}

void SetFetchShader(uint32_t fs)
{
    std::lock_guard lock(gMutex);
    gState->fetchShader = fs;
}

void SetVertexShader(uint32_t s)
{
    std::lock_guard lock(gMutex);
    gState->vertexShader = StateCopy(*gState, SlotVertexShader, s, 0x134);
}

void SetPixelShader(uint32_t s)
{
    std::lock_guard lock(gMutex);
    gState->pixelShader = StateCopy(*gState, SlotPixelShader, s, 0xE8);
}

void SetUniforms(bool pixel, uint32_t offset, const uint32_t* guestWords, uint32_t count)
{
    std::lock_guard lock(gMutex);
    float* c = gState->constants[pixel ? 1 : 0];
    for (uint32_t i = 0; i < count && offset + i < 1024; i++)
    {
        uint32_t v;
        memcpy(&v, guestWords + i, 4);
        c[offset + i] = std::bit_cast<float>(__builtin_bswap32(v));
    }
}

void SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer)
{
    std::lock_guard lock(gMutex);
    if (index < 16)
        gState->attribs[index] = { buffer, size, stride };
}

void SetTexture(bool pixel, uint32_t texture, uint32_t unit)
{
    std::lock_guard lock(gMutex);
    if (unit < 16)
        gState->textures[pixel ? 1 : 0][unit] = StateCopy(*gState, SlotTexture + (pixel ? 16 : 0) + unit, texture, 0x9C);
}

void SetSampler(bool pixel, uint32_t sampler, uint32_t unit)
{
    std::lock_guard lock(gMutex);
    if (unit < 16)
        gState->samplers[pixel ? 1 : 0][unit] = sampler;
}

void InitSampler(uint32_t sampler, uint32_t clamp, uint32_t filter)
{
    std::lock_guard lock(gMutex);
    SamplerState s;
    s.clamp[0] = s.clamp[1] = s.clamp[2] = clamp;
    s.mag = s.min = filter;
    gSamplerStates[sampler] = s;
}

void InitSamplerClamping(uint32_t sampler, uint32_t x, uint32_t y, uint32_t z)
{
    std::lock_guard lock(gMutex);
    SamplerState& s = gSamplerStates[sampler];
    s.clamp[0] = x, s.clamp[1] = y, s.clamp[2] = z;
}

void InitSamplerXYFilter(uint32_t sampler, uint32_t mag, uint32_t min, uint32_t aniso)
{
    std::lock_guard lock(gMutex);
    SamplerState& s = gSamplerStates[sampler];
    s.mag = mag, s.min = min, s.aniso = aniso;
}

void InitSamplerZMFilter(uint32_t sampler, uint32_t z, uint32_t mip)
{
    std::lock_guard lock(gMutex);
    (void)z;
    gSamplerStates[sampler].mip = mip;
}

void InitSamplerLOD(uint32_t sampler, float minLod, float maxLod, float bias)
{
    std::lock_guard lock(gMutex);
    SamplerState& s = gSamplerStates[sampler];
    // The sampler registers hold LODs in 1/64 steps (min/max 0-15.98, bias -32-31.98). Keeping only that precision
    // matches the hardware and keeps continuously varied values (streaming fades) from each making a new VkSampler.
    auto q = [](float v, float lo, float hi) { return std::clamp(std::round(v * 64.0f) / 64.0f, lo, hi); };
    s.minLod = q(minLod, 0.0f, 15.984375f), s.maxLod = q(maxLod, 0.0f, 15.984375f), s.bias = q(bias, -32.0f, 31.984375f);
}

void EndQuery(uint32_t addr);

void BeginQuery(uint32_t addr, uint64_t issue)
{
    std::lock_guard lock(gMutex);
    if (!gQueryPool)
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = QueriesPerSlot * Device::SlotCount();
        VK_CHECK(vkCreateQueryPool(Dev().Handle(), &qi, nullptr, &gQueryPool));
    }
    if (gOpenQuery.active)  // (an unended query: whatever it counted, report it visible)
        EndQuery(gOpenQuery.addr), gPendingQueries.back().forceVisible = true;
    if (gQueryFrame != Dev().FrameIndex())
    {
        // A new frame on this slot: its queries from two frames ago are done (BeginFrame waited for them); read any the
        // game has not asked for, then reset the slot's range (outside a pass).
        gQueryFrame = Dev().FrameIndex();
        ResolvePending(true, Dev().Slot());
        EndPass();
        vkCmdResetQueryPool(Dev().Cmd(), gQueryPool, Dev().Slot() * QueriesPerSlot, QueriesPerSlot);
        gQueryNext = 0;
    }
    gOpenQuery = {};
    gOpenQuery.active = true, gOpenQuery.addr = addr, gOpenQuery.issue = issue;
}

void EndQuery(uint32_t addr)
{
    std::lock_guard lock(gMutex);
    if (!gOpenQuery.active || gOpenQuery.addr != addr)
        return;
    QueryEndPiece();
    gPendingQueries.push_back({ addr, gOpenQuery.issue, std::move(gOpenQuery.indices), Dev().CommandEpoch(), Dev().Slot(),
        gOpenQuery.forceVisible });
    gOpenQuery = {};
    gDrawsSinceQueryEnd = 0;
}

void ResolveQueries()
{
    std::lock_guard lock(gMutex);
    if (gOpenQuery.active)
        return;  // (the game waits on an earlier query while one is open: resolved when it ends)
    SubmitQueriesEarly();
    ResolvePending(false, 0);
}

bool QueryResult(uint32_t addr, uint64_t issue, uint64_t& samples, double waitMs)
{
    std::unique_lock lock(gQueryResultsMutex);
    auto ready = [&] {
        auto it = gQueryResults.find(addr);
        return it != gQueryResults.end() && it->second.first == issue;
    };
    if (!ready() && (waitMs <= 0 || !gQueryResultsChanged.wait_for(lock, std::chrono::duration<double, std::milli>(waitMs), ready)))
        return false;
    samples = gQueryResults[addr].second;
    return true;
}

void Invalidate(uint32_t mode, uint32_t addr, uint32_t size)
{
    std::lock_guard lock(gMutex);
    PhaseTimer timer;
    struct Lap { PhaseTimer& t; ~Lap() { t.Lap(gStats.tInvalidate); } } lap{ timer };
    gStats.invalidates++;
    gStats.invalidateMaxSize = std::max<uint64_t>(gStats.invalidateMaxSize, size);
    // ATTRIBUTE_BUFFER (1) or CPU (0x40): cached vertex and index buffers in the range are stale.
    if (mode & (0x1 | InvalidateCpu))
        gCacheRanges.ForOverlaps(addr, size, [](CachedBuffer* e) { e->dirty = true, gStats.invalidateHits++; });
    // GX2InvalidateMode: TEXTURE (2) says texture caches must see new data, CPU (0x40) that the CPU wrote it.
    // A surface the GPU rendered into only becomes stale when the CPU writes it; the texture-cache flush games
    // issue after rendering to a texture must not reload it from (stale) guest memory.
    if (!(mode & (InvalidateTexture | InvalidateCpu)))
        return;
    gSurfaceRanges.ForOverlaps(addr, size, [mode](uint64_t key) {
        gStats.invalidateHits++;
        GpuSurface& g = gSurfaces[key];
        if (g.isDepth)
            return;
        if (!g.gpuOwned)
            g.cpuDirty = true;
        else if ((mode & InvalidateCpu) && !KeepGpuLevels(g))
        {
            if (gLogCopies)
                dtlog::Important("[copies] f%llu revert (invalidate) %08X %ux%u vk%d levels %u gpuLevels %X\n",
                    (unsigned long long)Dev().FrameIndex(), g.addr, g.guestWidth, g.guestHeight, int(g.format), g.levels, g.gpuLevels);
            SetOwned(g, false), g.cpuDirty = true;
        }
    });
}

void SetBlendControl(uint32_t target, uint32_t colorSrc, uint32_t colorDst, uint32_t colorOp, bool separateAlpha,
    uint32_t alphaSrc, uint32_t alphaDst, uint32_t alphaOp)
{
    std::lock_guard lock(gMutex);
    if (target < 8)
        gState->blend[target] = { colorSrc, colorDst, colorOp, alphaSrc, alphaDst, alphaOp, separateAlpha };
}

void SetColorControl(uint32_t logicOp, uint32_t blendMask, bool multiWrite, bool colorWrite)
{
    std::lock_guard lock(gMutex);
    (void)logicOp, (void)multiWrite;
    gState->blendMask = blendMask;
    gState->colorWrite = colorWrite;
}

void SetTargetChannelMasks(const uint32_t masks[8])
{
    std::lock_guard lock(gMutex);
    memcpy(gState->channelMasks, masks, sizeof(gState->channelMasks));
}

void SetNoCull(bool noCull) { gNoCull = noCull; }

void SetPolygonControl(uint32_t frontFace, bool cullFront, bool cullBack, bool polygonOffset)
{
    std::lock_guard lock(gMutex);
    gState->frontFace = frontFace;
    gState->cullFront = cullFront;
    gState->cullBack = cullBack;
    gState->polygonOffset = polygonOffset;
}

void SetPolygonOffset(float offset, float scale, float clamp)
{
    std::lock_guard lock(gMutex);
    gState->polyOffset = offset, gState->polyScale = scale, gState->polyClamp = clamp;
}

void SetRasterizerClip(bool rasterizer, bool zClip, bool halfZ)
{
    std::lock_guard lock(gMutex);
    gState->rasterizer = rasterizer, gState->zClip = zClip, gState->halfZ = halfZ;
    if (!zClip && !Dev().Features().depthClamp)
        ReportOnce("depth clipping cannot be turned off on this GPU (no depthClamp)");
}

void SetLineWidth(float width)
{
    std::lock_guard lock(gMutex);
    gState->lineWidth = width;
}

void SetBlendConstant(float r, float g, float b, float a)
{
    std::lock_guard lock(gMutex);
    float* c = gState->blendConstant;
    c[0] = r, c[1] = g, c[2] = b, c[3] = a;
}

void SetDepthStencilControl(bool depthTest, bool depthWrite, uint32_t depthFunc, bool stencilTest, bool backfaceStencil,
    StencilFace front, StencilFace back)
{
    std::lock_guard lock(gMutex);
    State& st = *gState;
    st.depthTest = depthTest, st.depthWrite = depthWrite, st.depthFunc = depthFunc;
    st.stencilTest = stencilTest, st.backfaceStencil = backfaceStencil;
    st.stencilFront = front, st.stencilBack = back;
}

void SetStencilMask(StencilMask front, StencilMask back)
{
    std::lock_guard lock(gMutex);
    gState->maskFront = front, gState->maskBack = back;
}

void SetAlphaTest(bool enable, uint32_t func, float ref)
{
    std::lock_guard lock(gMutex);
    gState->alphaTest = enable, gState->alphaFunc = func, gState->alphaRef = ref;
}

void Draw(uint32_t mode, uint32_t count, uint32_t firstVertex, uint32_t instances)
{
    std::lock_guard lock(gMutex);
    DoDraw(mode, count, 0, 0, firstVertex, instances);
}

void DrawIndexed(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices, uint32_t firstVertex, uint32_t instances)
{
    std::lock_guard lock(gMutex);
    DoDraw(mode, count, indexType, indices, firstVertex, instances);
}

void CopyColorBufferToScanBuffer(uint32_t colorBuffer, uint32_t target)
{
    std::lock_guard lock(gMutex);
    EndPass();  // transfer commands cannot run inside a rendering pass
    if (!(target & ScanTargetTV))
    {
        ReportOnce("the GamePad (DRC) screen is not shown");
        return;
    }
    const ColorBuffer cb = ReadColorBuffer(colorBuffer);
    GpuSurface* gs = GetSurface(cb.surface, false, gRenderScale, true);
    if (!gs)
        return;
    Upload(*gs, cb.surface);
    const uint32_t mip = std::min(cb.viewMip, gs->levels - 1);
    const uint32_t w = std::max(1u, gs->width >> mip), h = std::max(1u, gs->height >> mip);
    if (!gScan.image || gScan.width != w || gScan.height != h)
    {
        EndPass();
        Dev().SubmitAndWait();
        Dev().DestroyImage(gScan);
        gScan = Dev().CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    EnsureLayout(gs->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkCommandBuffer cmd = Dev().Cmd();
    Dev().Transition(cmd, gScan, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, gs->is3D ? 0 : std::min(cb.viewFirstSlice, gs->depth - 1), 1 };
    blit.srcOffsets[1] = { int32_t(w), int32_t(h), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
    vkCmdBlitImage(cmd, gs->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gScan.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);
}

void Swap(uint64_t frame)
{
    std::lock_guard lock(gMutex);
    if (frame % 300 == 0 && gQueryStats.resolved)
    {
        dtlog::Important("[gpu] occlusion queries, last 300 frames: %llu resolved, %llu hidden, %llu early submits, %llu "
                         "forced visible\n", (unsigned long long)gQueryStats.resolved, (unsigned long long)gQueryStats.hidden,
            (unsigned long long)gQueryStats.early, (unsigned long long)gQueryStats.forced);
        gQueryStats = {};
    }
    gAsync = settings::Get("graphics.async_shaders", "on") != "off";  // (the Graphics tab changes it while playing)
    InstallFinished();
    gWaitBudget = WaitPerFrame;
    gRebuildBytes = 0;
    EndPass();
    FreeRetired();
    if (gStatsOn)
    {
        // A slow frame: where its time went. The renderer: compiling (shaders and pipelines seen for the first time),
        // its other per-draw work, and presenting (waiting for the GPU). The game and rendering threads: time in
        // imports by kind (trace::ImportKind) and the rest, which is the game's own code.
        static auto last = std::chrono::steady_clock::now();
        static uint64_t lastPipelines = 0, lastShaders = 0;
        static double lastCompile = 0, lastWork = 0, lastPresent = 0;
        static uint64_t lastSurfaces = 0, lastTrims = 0, lastUpload = 0;
        static double lastCreate = 0, lastDetile = 0;
        static double lastPhases[8] = {};
        static uint64_t lastBuf[6] = {};
        static Device::PresentTimes lastTimes;
        static uint64_t lastGame[trace::KindCount] = {}, lastRender[trace::KindCount] = {};
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        const double work = gStats.tSetup + gStats.tShaders + gStats.tTextures + gStats.tBuffers + gStats.tPipeline +
            gStats.tDescriptors + gStats.tRecord;
        uint64_t game[trace::KindCount], render[trace::KindCount];
        trace::ImportTimes("Game Thread", game);
        trace::ImportTimes("RenderingThread", render);
        // Work and present times reset every StatsInterval frames; a negative delta means a reset happened.
        auto delta = [](double cur, double prev) { return (cur >= prev ? cur - prev : cur) * 1000; };
        static uint64_t lastDraws = 0;
        if (gFrameCsv)
        {
            if (frame <= 1)
                fprintf(gFrameCsv, "frame,ms,compile_ms,pipelines,shaders,draws,draw_ms,present_ms,acquire_ms,submit_ms,"
                                   "queue_present_ms,fence_ms,new_surfaces,create_ms,upload_kb,detile_ms,"
                                   "game_code,game_wait,game_fs,game_swap,game_gx2,game_other,"
                                   "render_code,render_wait,render_fs,render_swap,render_gx2,render_other,gpu_ms_2_frames_back\n");
            auto kinds = [&](const uint64_t* cur, const uint64_t* prev) {
                double k[trace::KindCount], sum = 0;
                for (int i = 0; i < trace::KindCount; i++)
                    k[i] = double(cur[i] - prev[i]) / 1e6, sum += k[i];
                char b[160];
                snprintf(b, sizeof(b), "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", std::max(0.0, ms - sum), k[trace::KindWait],
                    k[trace::KindFs], k[trace::KindSwap], k[trace::KindGx2], k[trace::KindOther]);
                return std::string(b);
            };
            const Device::PresentTimes& t = Dev().Times();
            fprintf(gFrameCsv, "%llu,%.3f,%.2f,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%llu,%.2f,%s,%s,%.2f\n",
                (unsigned long long)frame, ms, (gStats.tCompile - lastCompile) * 1000,
                (unsigned long long)(gStats.pipelines - lastPipelines), (unsigned long long)(gStats.shaderModules - lastShaders),
                (unsigned long long)(gStats.draws >= lastDraws ? gStats.draws - lastDraws : gStats.draws), delta(work, lastWork),
                delta(gStats.tPresent, lastPresent), (t.acquire - lastTimes.acquire) * 1000, (t.submit - lastTimes.submit) * 1000,
                (t.present - lastTimes.present) * 1000, (t.fence - lastTimes.fence) * 1000,
                (unsigned long long)(gStats.recreated - lastSurfaces), (gStats.tCreate - lastCreate) * 1000,
                (unsigned long long)((gStats.uploadBytesTotal - lastUpload) / 1024), (gStats.tDetile - lastDetile) * 1000,
                kinds(game, lastGame).c_str(), kinds(render, lastRender).c_str(), gGpuLastMs);
            gGpuLastMs = 0;
            fflush(gFrameCsv);  // runs often end with _Exit (--frames, a quit): keep every row
        }
        lastDraws = gStats.draws;
        if (ms > 40 && frame > 1)
        {
            auto split = [&](const uint64_t* cur, const uint64_t* prev) {
                double k[trace::KindCount], sum = 0;
                for (int i = 0; i < trace::KindCount; i++)
                    k[i] = double(cur[i] - prev[i]) / 1e6, sum += k[i];
                char b[160];
                snprintf(b, sizeof(b), "code %.0f, wait %.0f, fs %.0f, swap %.0f, gx2 %.0f, other %.0f", std::max(0.0, ms - sum),
                    k[trace::KindWait], k[trace::KindFs], k[trace::KindSwap], k[trace::KindGx2], k[trace::KindOther]);
                return std::string(b);
            };
            const double dWork = delta(work, lastWork), dPresent = delta(gStats.tPresent, lastPresent);
            const double phases[8] = { gStats.tSetup, gStats.tShaders, gStats.tTextures, gStats.tBuffers, gStats.tPipeline,
                gStats.tDescriptors, gStats.tRecord, gStats.tInvalidate };
            char split8[400];
            snprintf(split8, sizeof split8, "setup %.0f, shaders %.0f, textures %.0f, buffers %.0f, pipeline %.0f, descriptors %.0f, "
                "record %.0f, invalidates %.0f", delta(phases[0], lastPhases[0]), delta(phases[1], lastPhases[1]),
                delta(phases[2], lastPhases[2]), delta(phases[3], lastPhases[3]), delta(phases[4], lastPhases[4]),
                delta(phases[5], lastPhases[5]), delta(phases[6], lastPhases[6]), delta(phases[7], lastPhases[7]));
            const size_t used = strlen(split8);
            snprintf(split8 + used, sizeof split8 - used, "; %llu buffers converted (%llu KB), %llu per-draw uploads (%llu KB), "
                "%llu KB of VTX buffers, %llu conversions deferred", (unsigned long long)(gStats.rebuilds - lastBuf[0]),
                (unsigned long long)((gStats.rebuildBytes - lastBuf[1]) >> 10), (unsigned long long)(gStats.perDraws - lastBuf[2]),
                (unsigned long long)((gStats.perDrawBytes - lastBuf[3]) >> 10), (unsigned long long)((gStats.vtxBytes - lastBuf[4]) >> 10),
                (unsigned long long)(gStats.rebuildsDeferred - lastBuf[5]));
            dtlog::Important("[gpu-stats] slow frame %llu: %.0f ms | renderer: compile %.0f (%llu pipelines, %llu shaders), "
                             "draws %.0f (%s), present %.0f (acquire %.0f, submit %.0f, present %.0f, fence %.0f), %llu new surfaces (%.0f ms), %llu KB uploaded (detiling %.0f ms), %llu buffer-cache flushes | "
                             "game thread: %s | render thread: %s\n",
                (unsigned long long)frame, ms, (gStats.tCompile - lastCompile) * 1000,
                (unsigned long long)(gStats.pipelines - lastPipelines), (unsigned long long)(gStats.shaderModules - lastShaders),
                dWork, split8, dPresent, (Dev().Times().acquire - lastTimes.acquire) * 1000, (Dev().Times().submit - lastTimes.submit) * 1000,
                (Dev().Times().present - lastTimes.present) * 1000, (Dev().Times().fence - lastTimes.fence) * 1000,
                (unsigned long long)(gStats.recreated - lastSurfaces), (gStats.tCreate - lastCreate) * 1000,
                (unsigned long long)((gStats.uploadBytesTotal - lastUpload) / 1024), (gStats.tDetile - lastDetile) * 1000, (unsigned long long)(gStats.cacheTrims - lastTrims),
                split(game, lastGame).c_str(), split(render, lastRender).c_str());
        }
        last = now, lastPipelines = gStats.pipelines, lastShaders = gStats.shaderModules;
        lastCompile = gStats.tCompile, lastWork = work, lastPresent = gStats.tPresent;
        lastPhases[0] = gStats.tSetup, lastPhases[1] = gStats.tShaders, lastPhases[2] = gStats.tTextures,
        lastPhases[3] = gStats.tBuffers, lastPhases[4] = gStats.tPipeline, lastPhases[5] = gStats.tDescriptors,
        lastPhases[6] = gStats.tRecord, lastPhases[7] = gStats.tInvalidate;
        lastBuf[0] = gStats.rebuilds, lastBuf[1] = gStats.rebuildBytes, lastBuf[2] = gStats.perDraws,
        lastBuf[3] = gStats.perDrawBytes, lastBuf[4] = gStats.vtxBytes, lastBuf[5] = gStats.rebuildsDeferred;
        lastSurfaces = gStats.recreated, lastTrims = gStats.cacheTrims, lastUpload = gStats.uploadBytesTotal;
        lastTimes = Dev().Times();
        lastCreate = gStats.tCreate, lastDetile = gStats.tDetile;
        std::copy(game, game + trace::KindCount, lastGame);
        std::copy(render, render + trace::KindCount, lastRender);
        gLastPipelineTime = gStats.tPipeline, gLastShaderTime = gStats.tShaders;
    }
    if (frame % 120 == 0)
        PruneBufferCache();
    // New pipelines are saved at quiet moments only (a Bink movie, the game's pause menu, its front end, our page):
    // even off this thread, serialising the cache (70 MB) delayed a gameplay frame by up to 10 ms.
    if (frame % 300 == 0 && gPipelinesUnsaved && (host::menu::IsOpen() || game_ui::Current() != game_ui::State::Gameplay))
        SavePipelineCache(false);
    // The frame being finished is `frame`: capture covers the draws between the previous swap and this one.
    if (gCapturing && frame == gCaptureFrame)
    {
        gCapturing = false;
        dtlog::Important("[gpu] captured %u passes of frame %llu to %s\n", gCapturePass, (unsigned long long)frame,
            gCaptureDir.c_str());
    }
    if (gCaptureFrames.count(frame + 1))
    {
        gCaptureFrame = frame + 1;
        gCaptureDir = gCaptureFrames.size() > 1 ? gCaptureBase + "/f" + std::to_string(frame + 1) : gCaptureBase;
        std::filesystem::create_directories(gCaptureDir);
        gCapturing = true, gCapturePass = 0, gCapturedTextures.clear(), gCapturedShaders.clear();
    }
    if (gStatsOn && frame % StatsInterval == 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (frame > StatsInterval)
        {
            const double seconds = std::chrono::duration<double>(now - gStatsStart).count();
            const double n = double(StatsInterval);
            dtlog::Important("[gpu-stats] frames %llu-%llu: %.1f fps; per frame %.0f draws, %.1f passes, %.2f snapshots, "
                             "%.2f uploads, %.2f buffers converted (%.0f KB), %.2f rendered surfaces overwritten by the CPU, %.2f block copies into compressed textures, %.2f copies made in guest memory, %.2f surfaces keeping GPU-copied levels over a CPU write; %llu pipelines "
                             "and %llu surfaces created in total; %llu draws skipped while compiling in the background (%llu shaders, "
                             "%llu pipelines in total; %llu precompiled pipelines built differently now)\n",
                (unsigned long long)(frame - StatsInterval), (unsigned long long)frame, n / seconds, gStats.draws / n,
                gStats.passes / n, gStats.snapshots / n, gStats.uploads / n, gStats.bufferUploads / n, gStats.uploadBytes / n / 1024,
                gStats.overwritten / n, gStats.blockCopies / n, gStats.guestCopies / n, gStats.keptLevels / n, (unsigned long long)gStats.pipelines, (unsigned long long)gStats.recreated,
                (unsigned long long)gStats.asyncSkips, (unsigned long long)gStats.asyncShaders,
                (unsigned long long)gStats.asyncPipelines, (unsigned long long)gStats.staleRecords);
            const double ms = 1000.0 / n;
            dtlog::Important("[gpu-stats]   ms per frame: setup %.2f, shaders %.2f, textures %.2f, buffers %.2f, pipeline %.2f, "
                             "descriptors %.2f, record %.2f, present %.2f; %.0f invalidates %.2f ms (%.0f hits, largest %llu KB; indexed %zu+%zu surface, %zu+%zu buffer ranges); buffer cache %zu+%zu entries, %llu MB, %llu blocks evicted; %llu GPU-written surfaces moved with the texture pool\n",
                gStats.tSetup * ms, gStats.tShaders * ms, gStats.tTextures * ms, gStats.tBuffers * ms, gStats.tPipeline * ms,
                gStats.tDescriptors * ms, gStats.tRecord * ms, gStats.tPresent * ms, gStats.invalidates / n,
                gStats.tInvalidate * ms, gStats.invalidateHits / n, (unsigned long long)(gStats.invalidateMaxSize / 1024), gSurfaceRanges.Size(), gSurfaceRanges.LargeSize(),
                gCacheRanges.Size(), gCacheRanges.LargeSize(), gVertexCache.size(), gIndexCache.size(),
                (unsigned long long)(gCacheTotal >> 20), (unsigned long long)gStats.cacheEvictions,
                (unsigned long long)gStats.relocated);
            std::string snaps;
            for (auto [key, count] : gStats.snapshotsBySurface)
                if (count >= StatsInterval)
                    snaps += " 0x" + Hex(uint32_t(key >> 32)) + ((key & 1) ? " depth" : "") + " " + std::to_string(count / StatsInterval);
            if (!snaps.empty())
                dtlog::Important("[gpu-stats]   snapshots per frame by surface:%s\n", snaps.c_str());
            dtlog::Important("[gpu-stats]   imports, per frame:%s\n", cafe::TakeTopImportTimes(12, n).c_str());
            {
                // The game and rendering threads over the window: own code (outside imports) and waiting, per frame.
                static uint64_t lastGame[trace::KindCount] = {}, lastRender[trace::KindCount] = {};
                uint64_t game[trace::KindCount], render[trace::KindCount];
                trace::ImportTimes("Game Thread", game);
                trace::ImportTimes("RenderingThread", render);
                const double wallMs = seconds * 1000 / n;
                auto line = [&](const uint64_t* cur, const uint64_t* prev) {
                    double sum = 0;
                    for (int k = 0; k < trace::KindCount; k++)
                        sum += double(cur[k] - prev[k]) / 1e6 / n;
                    char b[96];
                    snprintf(b, sizeof(b), "code %.2f ms, wait %.2f ms", std::max(0.0, wallMs - sum),
                        double(cur[trace::KindWait] - prev[trace::KindWait]) / 1e6 / n);
                    return std::string(b);
                };
                dtlog::Important("[gpu-stats]   per frame: game thread %s; rendering thread %s\n", line(game, lastGame).c_str(),
                    line(render, lastRender).c_str());
                std::copy(game, game + trace::KindCount, lastGame);
                std::copy(render, render + trace::KindCount, lastRender);
            }
            if (gGpuFrames)
                dtlog::Important("[gpu-stats]   GPU per frame: %.2f ms, of which snapshot copies %.2f ms (%llu frames timed)\n",
                    gGpuFrameMs / gGpuFrames, gGpuSnapshotMs / gGpuFrames, (unsigned long long)gGpuFrames);
            gGpuFrameMs = gGpuSnapshotMs = 0, gGpuFrames = 0;
            if (uint64_t usage, budget; Dev().MemoryBudget(usage, budget))
                dtlog::Important("[gpu-stats]   GPU memory: %llu MB used of a %llu MB budget\n",
                    (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20));
        }
        gStatsStart = now;
        gStats.asyncSkips = 0;
        gStats.draws = gStats.passes = gStats.snapshots = gStats.uploads = gStats.uploadBytes = gStats.bufferUploads =
            gStats.overwritten = gStats.blockCopies = gStats.guestCopies = gStats.keptLevels = 0;
        gStats.tSetup = gStats.tShaders = gStats.tTextures = gStats.tBuffers = gStats.tPipeline = gStats.tDescriptors =
            gStats.tRecord = gStats.tPresent = gStats.tInvalidate = 0;
        gLastPipelineTime = gLastShaderTime = 0;
        gStats.invalidates = gStats.invalidateHits = gStats.invalidateMaxSize = 0;
        gStats.snapshotsBySurface.clear();
    }
    bool requested = false;
    for (int n = gShotRequests.load(); n > 0 && !requested;)
        requested = gShotRequests.compare_exchange_weak(n, n - 1);
    if (requested || gShotFrames.count(frame) || (gShotEvery && frame % gShotEvery == 0))
        SaveScreenshot(frame);
    if (gUserShot.exchange(false))
        SaveUserScreenshot(frame);
    GpuMarkHere("present");
    if (gTimestamps && gTimestampSlots[Dev().Slot()].active)
        vkCmdWriteTimestamp(Dev().Cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gTimestamps, Dev().Slot() * TimestampsPerSlot + 1);
    PhaseTimer present;
    Dev().Present(gScan.image ? &gScan : nullptr);
    present.Lap(gStats.tPresent);
    BeginGpuTiming();
}

void EnableStats() { gStatsOn = true; }

void SetFrameCsv(const std::string& path)
{
    gStatsOn = true;
    gFrameCsv = fopen(path.c_str(), "w");
    if (!gFrameCsv)
        dtlog::Important("[gpu] cannot write %s\n", path.c_str());
}

void SetCapture(const std::string& dir, uint64_t frame)
{
    gCaptureBase = gCaptureDir = dir;
    gCaptureFrames.insert(frame);
    std::filesystem::create_directories(dir);
}

void RequestScreenshot() { gShotRequests++; }
void RequestUserScreenshot() { gUserShot = true; }
void SetUserScreenshotDir(const std::string& dir) { gUserShotDir = dir; }

void SetScreenshots(const std::string& dir, const std::vector<uint64_t>& frames, uint64_t every, bool window)
{
    gShotDir = dir;
    gShotWindow = window;
    if (window)
        Dev().EnableWindowShots();  // (before the swapchain exists: it is made copyable)
    gShotEvery = every;
    gShotFrames.insert(frames.begin(), frames.end());
    std::filesystem::create_directories(dir);
}
} // namespace gpu::renderer
