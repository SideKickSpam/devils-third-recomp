// Decoder and disassembler for R700-family (Latte) GPU programs, following AMD's "R700-Family Instruction Set
// Architecture" (rev. 1.0a, 2011), chapter 10 (microcode formats) and chapter 9 (opcodes).
// Programs are arrays of little-endian 32-bit words.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::r700
{
// ---- control flow (CF_WORD0/1, CF_ALU_WORD0/1, CF_ALLOC_EXPORT_WORD0/1_SWIZ) ----
enum CfInst : uint32_t
{
    CF_NOP = 0, CF_TEX = 1, CF_VTX = 2, CF_VTX_TC = 3, CF_LOOP_START = 4, CF_LOOP_END = 5, CF_LOOP_START_DX10 = 6,
    CF_LOOP_START_NO_AL = 7, CF_LOOP_CONTINUE = 8, CF_LOOP_BREAK = 9, CF_JUMP = 10, CF_PUSH = 11, CF_PUSH_ELSE = 12,
    CF_ELSE = 13, CF_POP = 14, CF_POP_JUMP = 15, CF_POP_PUSH = 16, CF_POP_PUSH_ELSE = 17, CF_CALL = 18, CF_CALL_FS = 19,
    CF_RETURN = 20, CF_EMIT_VERTEX = 21, CF_EMIT_CUT_VERTEX = 22, CF_CUT_VERTEX = 23, CF_KILL = 24, CF_WAIT_ACK = 26,
    CF_MEM_STREAM0 = 32, CF_MEM_SCRATCH = 36, CF_MEM_REDUCTION = 37, CF_MEM_RING = 38, CF_EXPORT = 39,
    CF_EXPORT_DONE = 40, CF_MEM_EXPORT = 58,
};

// ALU clause CF instructions (4-bit CF_INST in CF_ALU_WORD1).
enum CfAluInst : uint32_t
{
    CF_ALU = 8, CF_ALU_PUSH_BEFORE = 9, CF_ALU_POP_AFTER = 10, CF_ALU_POP2_AFTER = 11, CF_ALU_CONTINUE = 13,
    CF_ALU_BREAK = 14, CF_ALU_ELSE_AFTER = 15,
};

enum ExportType : uint32_t { EXPORT_PIXEL = 0, EXPORT_POS = 1, EXPORT_PARAM = 2 };

struct Cf
{
    enum Kind { Normal, Alu, Export } kind;
    uint32_t index;   // position in the CF program
    uint32_t inst;    // CfInst or CfAluInst
    uint32_t addr;    // clause address in 64-bit units (Normal, Alu)
    uint32_t count;   // instructions in the clause (already +1)
    bool endOfProgram, barrier, wholeQuadMode, validPixelMode;
    uint32_t popCount, cfConst, cond, callCount;
    // ALU clause constant caches.
    uint32_t kcacheBank[2], kcacheMode[2], kcacheAddr[2];
    // Exports.
    uint32_t exportType, arrayBase, rwGpr, rwRel, indexGpr, elemSize, burstCount;
    std::array<uint32_t, 4> sel;  // 0-3 xyzw, 4 zero, 5 one, 7 masked
};

// ---- ALU (ALU_WORD0, ALU_WORD1_OP2 / OP3) ----
enum AluSrc : uint32_t
{
    SRC_KCACHE0 = 128, SRC_KCACHE1 = 160, SRC_0 = 248, SRC_1 = 249, SRC_1_INT = 250, SRC_M_1_INT = 251,
    SRC_0_5 = 252, SRC_LITERAL = 253, SRC_PV = 254, SRC_PS = 255, SRC_CFILE = 256,
};

struct AluSource
{
    uint32_t sel, chan;
    bool rel, neg, abs;
};

struct Alu
{
    bool op3;
    uint32_t inst;  // OP2 opcode (11 bits) or OP3 opcode (5 bits)
    std::array<AluSource, 3> src;
    uint32_t numSrc;
    uint32_t indexMode, predSel, bankSwizzle;
    bool last, updateExecMask, updatePred, writeMask, clamp;
    uint32_t omod;
    uint32_t dstGpr, dstChan;
    bool dstRel;
    uint32_t slot;  // 0-3 vector x/y/z/w, 4 trans (assigned while decoding the group)
};

struct AluGroup
{
    std::vector<Alu> insts;
    std::array<uint32_t, 4> literals{};
};

// ---- texture fetch (TEX_WORD0/1/2) ----
enum TexInst : uint32_t
{
    TEX_LD = 3, TEX_GET_TEXTURE_RESINFO = 4, TEX_GET_GRADIENTS_H = 7, TEX_GET_GRADIENTS_V = 8, TEX_SET_GRADIENTS_H = 11,
    TEX_SET_GRADIENTS_V = 12, TEX_SAMPLE = 16, TEX_SAMPLE_L = 17, TEX_SAMPLE_LB = 18, TEX_SAMPLE_LZ = 19,
    TEX_SAMPLE_G = 20, TEX_SAMPLE_C = 24, TEX_SAMPLE_C_L = 25, TEX_SAMPLE_C_LZ = 27,
};

struct Tex
{
    uint32_t inst, resourceId, samplerId, srcGpr, dstGpr;
    bool srcRel, dstRel, fetchWholeQuad, bcFracMode;
    std::array<uint32_t, 4> srcSel, dstSel;  // dst: 0-3 xyzw, 4 zero, 5 one, 7 masked
    std::array<bool, 4> coordNormalized;
    int32_t lodBias;
    std::array<int32_t, 3> offset;
};

// ---- vertex fetch (VTX_WORD0/1/2) ----
enum VtxInst : uint32_t { VTX_FETCH = 0, VTX_SEMANTIC = 1, VTX_MEM = 2 };

struct Vtx
{
    uint32_t inst, fetchType, bufferId, srcGpr, srcSel, megaFetchCount;
    bool srcRel, dstRel, useConstFields, formatSigned, megaFetch;
    uint32_t dstGpr;  // or the semantic ID for VTX_SEMANTIC
    std::array<uint32_t, 4> dstSel;  // 0-3 xyzw, 4 zero, 5 one, 7 masked
    uint32_t dataFormat, numFormat;  // FMT_*, 0 norm / 1 int / 2 scaled
    uint32_t offset, endianSwap;     // byte offset; 0 none, 1 8-in-16, 2 8-in-32
};

// ---- a whole program ----
struct Program
{
    std::vector<Cf> cf;
    // Clause bodies, indexed by CF index (empty for CF instructions without a clause).
    std::vector<std::vector<AluGroup>> alu;
    std::vector<std::vector<Tex>> tex;
    std::vector<std::vector<Vtx>> vtx;
    std::string error;  // non-empty if decoding failed
};

Program Decode(const uint8_t* data, size_t size);
std::string Disassemble(const Program& p);

// Opcode properties.
const char* CfName(const Cf& cf);
const char* AluName(bool op3, uint32_t inst);
const char* TexName(uint32_t inst);
bool IsTransOnly(const Alu& a);   // must execute in the trans slot
bool IsReduction(const Alu& a);   // DOT4, DOT4_IEEE, CUBE, MAX4: uses all four vector slots
uint32_t NumSources(bool op3, uint32_t inst);
} // namespace gpu::r700
