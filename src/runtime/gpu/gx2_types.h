// GX2 structures as they sit in guest memory (big-endian, 32-bit), decoded into host structs.
// Layouts follow the Cafe SDK's gx2 headers as documented by the homebrew community (wut).
#pragma once

#include "../guest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gpu
{
// ---- GX2Surface (0x74 bytes) ----
struct Surface
{
    uint32_t dim, width, height, depth, mipLevels, format, aa, use;
    uint32_t imageSize, image, mipmapSize, mipmaps;
    uint32_t tileMode, swizzle, alignment, pitch;
    std::array<uint32_t, 13> mipLevelOffset;
};

inline Surface ReadSurface(uint32_t a)
{
    Surface s;
    uint32_t* f = &s.dim;
    for (int i = 0; i < 16; i++)
        f[i] = guest::Read32(a + i * 4);
    for (int i = 0; i < 13; i++)
        s.mipLevelOffset[i] = guest::Read32(a + 0x40 + i * 4);
    return s;
}

// ---- GX2Texture (0x9C): surface, viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices, compMap, regs[5] ----
struct Texture
{
    Surface surface;
    uint32_t viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices, compMap;
    std::array<uint32_t, 5> regs;
};

inline Texture ReadTexture(uint32_t a)
{
    Texture t;
    t.surface = ReadSurface(a);
    t.viewFirstMip = guest::Read32(a + 0x74);
    t.viewNumMips = guest::Read32(a + 0x78);
    t.viewFirstSlice = guest::Read32(a + 0x7C);
    t.viewNumSlices = guest::Read32(a + 0x80);
    t.compMap = guest::Read32(a + 0x84);
    for (int i = 0; i < 5; i++)
        t.regs[i] = guest::Read32(a + 0x88 + i * 4);
    return t;
}

// ---- GX2ColorBuffer (0x9C): surface, viewMip, viewFirstSlice, viewNumSlices, aaBuffer, aaSize, regs[5] ----
struct ColorBuffer
{
    Surface surface;
    uint32_t viewMip, viewFirstSlice, viewNumSlices, aaBuffer, aaSize;
};

inline ColorBuffer ReadColorBuffer(uint32_t a)
{
    ColorBuffer c;
    c.surface = ReadSurface(a);
    c.viewMip = guest::Read32(a + 0x74);
    c.viewFirstSlice = guest::Read32(a + 0x78);
    c.viewNumSlices = guest::Read32(a + 0x7C);
    c.aaBuffer = guest::Read32(a + 0x80);
    c.aaSize = guest::Read32(a + 0x84);
    return c;
}

// ---- GX2DepthBuffer (0xAC): surface, viewMip, viewFirstSlice, viewNumSlices, hiZPtr, hiZSize, clear values ----
struct DepthBuffer
{
    Surface surface;
    uint32_t viewMip, viewFirstSlice, viewNumSlices, hiZPtr, hiZSize;
};

inline DepthBuffer ReadDepthBuffer(uint32_t a)
{
    DepthBuffer d;
    d.surface = ReadSurface(a);
    d.viewMip = guest::Read32(a + 0x74);
    d.viewFirstSlice = guest::Read32(a + 0x78);
    d.viewNumSlices = guest::Read32(a + 0x7C);
    d.hiZPtr = guest::Read32(a + 0x80);
    d.hiZSize = guest::Read32(a + 0x84);
    return d;
}

// ---- GX2Sampler: three sampler registers ----
struct Sampler
{
    std::array<uint32_t, 3> regs;
};

inline Sampler ReadSampler(uint32_t a) { return { { guest::Read32(a), guest::Read32(a + 4), guest::Read32(a + 8) } }; }

// ---- shaders ----
// GX2VertexShader (0x134). The register block is 52 words:
//   sq_pgm_resources_vs, vgt_primitiveid_en, spi_vs_out_config, num_spi_vs_out_id, spi_vs_out_id[10],
//   pa_cl_vs_out_cntl, sq_vtx_semantic_clear, num_sq_vtx_semantic, sq_vtx_semantic[32],
//   vgt_strmout_buffer_en, vgt_vertex_reuse_block_cntl, vgt_hos_reuse_depth
// then size, program, mode, and (count, pointer) pairs for uniform blocks, uniform vars, initial values,
// loop vars, sampler vars and attrib vars.
struct VertexShader
{
    std::array<uint32_t, 52> regs;
    uint32_t size, program, mode;
    uint32_t uniformBlockCount, uniformBlocks, uniformVarCount, uniformVars, initialValueCount, initialValues;
    uint32_t loopVarCount, loopVars, samplerVarCount, samplerVars, attribVarCount, attribVars;

    uint32_t NumGprs() const { return regs[0] & 0xFF; }
    uint32_t NumOutputs() const { return regs[3]; }        // num_spi_vs_out_id (packed 4 per word)
    // Word indices: 0 sq_pgm_resources_vs, 3 num_spi_vs_out_id, 4-13 spi_vs_out_id, 16 num_sq_vtx_semantic,
    // 17-48 sq_vtx_semantic.
    uint32_t SemanticCount() const { return regs[16]; }
    uint32_t Semantic(uint32_t i) const { return regs[17 + i]; }
    uint32_t OutId(uint32_t i) const { return regs[4 + i]; }
};

inline VertexShader ReadVertexShader(uint32_t a)
{
    VertexShader s;
    for (int i = 0; i < 52; i++)
        s.regs[i] = guest::Read32(a + i * 4);
    uint32_t* f = &s.size;
    for (int i = 0; i < 15; i++)
        f[i] = guest::Read32(a + 0xD0 + i * 4);
    return s;
}

// GX2PixelShader (0xE8). The register block is 41 words:
//   sq_pgm_resources_ps, sq_pgm_exports_ps, spi_ps_in_control_0, spi_ps_in_control_1, num_spi_ps_input_cntl,
//   spi_ps_input_cntls[32], cb_shader_mask, cb_shader_control, db_shader_control, spi_input_z
// then size, program, mode, and (count, pointer) pairs for uniform blocks, uniform vars, initial values,
// loop vars and sampler vars.
struct PixelShader
{
    std::array<uint32_t, 41> regs;
    uint32_t size, program, mode;
    uint32_t uniformBlockCount, uniformBlocks, uniformVarCount, uniformVars, initialValueCount, initialValues;
    uint32_t loopVarCount, loopVars, samplerVarCount, samplerVars;

    uint32_t NumGprs() const { return regs[0] & 0xFF; }
    uint32_t NumInputs() const { return regs[4]; }
    uint32_t InputCntl(uint32_t i) const { return regs[5 + i]; }
    uint32_t ShaderMask() const { return regs[37]; }  // cb_shader_mask: 4 bits per render target
};

inline PixelShader ReadPixelShader(uint32_t a)
{
    PixelShader s;
    for (int i = 0; i < 41; i++)
        s.regs[i] = guest::Read32(a + i * 4);
    uint32_t* f = &s.size;
    for (int i = 0; i < 13; i++)
        f[i] = guest::Read32(a + 0xA4 + i * 4);
    return s;
}

// GX2AttribStream (0x20): location, buffer, offset, format, type (vertex/instance index), aluDivisor,
// mask (destination component selects), endianSwap.
struct AttribStream
{
    uint32_t location, buffer, offset, format, type, aluDivisor, mask, endianSwap;
};

inline std::vector<AttribStream> ReadAttribStreams(uint32_t a, uint32_t count, uint32_t stride)
{
    std::vector<AttribStream> v(count);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t* f = &v[i].location;
        for (int k = 0; k < 8; k++)
            f[k] = guest::Read32(a + i * stride + k * 4);
    }
    return v;
}
} // namespace gpu
