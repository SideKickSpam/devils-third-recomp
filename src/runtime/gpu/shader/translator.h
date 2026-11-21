// R700 (Latte) shader program -> SPIR-V for Vulkan.
//
// Resource interface (descriptor set 0), shared by every translated pipeline:
//   binding 0          vertex shader constant file: uniform block { vec4 c[256]; }
//   binding 1          pixel shader constant file
//   binding 2 + n      pixel shader texture resource n (combined image sampler), n < 16
//   binding 18 + n     vertex shader texture resource n
// Vertex inputs are at the locations given in the request. Vertex parameter exports go to output location =
// parameter index; pixel shader inputs read the locations given in the request; pixel exports to render
// target k go to output location k.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::shader
{
constexpr uint32_t BindingVsConstants = 0;
constexpr uint32_t BindingPsConstants = 1;
constexpr uint32_t BindingPsTextures = 2;
constexpr uint32_t BindingVsTextures = 18;
constexpr uint32_t MaxTextures = 16;
// Vertex shaders' VTX clauses read GX2 attribute buffer n (fetch resource 160 + n) as a raw storage buffer here.
constexpr uint32_t BindingVsBuffers = 34;

enum class Stage { Vertex, Pixel };

// One vertex attribute: Vulkan input location -> GPR, with GX2 component selects (0-3 = x-w, 4 = 0, 5 = 1).
struct VertexInput
{
    uint32_t location;
    uint32_t gpr;
    std::array<uint8_t, 4> sel;
    uint32_t type = 0;  // 0 float, 1 unsigned integer, 2 signed integer (bits land in the GPR unconverted)
};

// What kind of image a texture resource is (the GX2 texture's dimension), which decides how it is declared
// and how SAMPLE coordinates are read.
enum class TexDim : uint8_t { Tex2D = 0, Tex3D = 1, Cube = 2, Tex2DArray = 3 };

struct Request
{
    Stage stage = Stage::Vertex;
    const uint8_t* program = nullptr;
    size_t size = 0;
    std::vector<VertexInput> vertexInputs;  // vertex shaders: what CALL_FS loads
    std::vector<int32_t> pixelInputs;       // pixel shaders: input i (GPR i) comes from this location, -1 = default
    // Pixel shaders: the value of an input with no matching vertex output (SPI_PS_INPUT_CNTL DEFAULT_VAL):
    // 0 (0,0,0,0), 1 (0,0,0,1), 2 (1,1,1,0), 3 (1,1,1,1).
    std::vector<uint8_t> pixelInputDefaults;
    // Pixel shaders: the GPR that receives the pixel's position (x, y, depth, 1/w) when SPI_PS_IN_CONTROL_0 has
    // POSITION_ENA (its POSITION_ADDR), or -1. The input list has an entry for that GPR too.
    int32_t positionGpr = -1;
    bool halfZ = false;                     // clip-space z already in [0, w] (GX2 halfZ); otherwise [-w, w]
    // DX9-style loop constants (SQ_LOOP_CONST: trip count [11:0], initial aL [23:12], signed step [31:24]),
    // from the shader's loop variables.
    std::array<uint32_t, 32> loopConsts{};
    // Pixel shaders: alpha test against render target 0's alpha (GX2CompareFunction, 7 = always = off). The
    // reference value is read from the pixel constant file at AlphaRefConstant.x.
    uint32_t alphaFunc = 7;
    std::array<TexDim, MaxTextures> textureDims{};  // of the textures bound for this stage
    // Number type of each texture's data and (pixel shaders) each render target: 0 float, 1 uint, 2 sint.
    // Integer data moves through the GPRs as raw bits, as on the hardware.
    std::array<uint8_t, MaxTextures> textureTypes{};
    std::array<uint8_t, 8> outputTypes{};
    // Debugging (DT_DEBUG_PS): render target 0 receives GPR debugGpr as it is right after CF instruction debugCf
    // (debugCf -1: as interpolated), instead of the shader's result. Bit 8 of debugGpr: its w in every channel.
    int32_t debugGpr = -1, debugCf = -1;
    // HashProgram(stage, halfZ, program, size) if the caller has it, else 0.
    uint64_t programState = 0;
};

// Renderer data after GX2's 256 constants, in each stage's constant file:
//   c[256].x  alpha-test reference (pixel)
//   c[256].y  base vertex as uint bits (vertex): the draw's vertices are rebased, VTX fetches index the full buffer
//   c[256].z  1 / the internal resolution scale of the draw's targets (pixel): the pixel position (POSITION_ENA) is
//             multiplied by it, so shaders see the game's own pixel units at any resolution without a new variant
//   c[257..260]  attribute buffer strides as uint bits, four per register (vertex, VTX clauses)
constexpr uint32_t AlphaRefConstant = 256, RendererConstants = 256, StrideConstants = 257, ConstantRegisters = 261;

struct Result
{
    std::vector<uint32_t> spirv;
    std::string error;          // empty on success
    uint32_t textureMask = 0;   // texture resources used
    uint32_t outputMask = 0;    // vertex: parameters exported; pixel: render targets written
    std::vector<std::string> warnings;  // features the program uses that the translation leaves out
    uint32_t bufferMask = 0;    // vertex: attribute buffers read by VTX clauses
};

Result Translate(const Request& request);

// Stable 64-bit hash of a request (program bytes plus everything that changes the output), for caching.
uint64_t Hash(const Request& request);
// The hash state after the request's fixed prefix and program bytes. A caller that sees the same program often can
// keep it and pass it as Request::programState, so Hash does not walk the program bytes again.
uint64_t HashProgram(Stage stage, bool halfZ, const uint8_t* program, uint32_t size);
} // namespace gpu::shader
