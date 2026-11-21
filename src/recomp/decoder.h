// Espresso (Wii U PowerPC 750-family) instruction decoder.
// Decodes the 32-bit user ISA plus Gekko/Broadway/Espresso paired-single instructions.
// Simplified mnemonics are not produced: e.g. `mr` decodes as OR, `blr` as BCLR, `slwi` as RLWINM.
#pragma once

#include <cstdint>

#define PPC_OPS(X)                                                                                      \
    X(INVALID, "(invalid)")                                                                             \
    /* integer arithmetic / logic, immediate forms */                                                   \
    X(TWI, "twi") X(MULLI, "mulli") X(SUBFIC, "subfic") X(CMPLI, "cmpli") X(CMPI, "cmpi")               \
    X(ADDIC, "addic") X(ADDIC_RC, "addic.") X(ADDI, "addi") X(ADDIS, "addis")                           \
    X(RLWIMI, "rlwimi") X(RLWINM, "rlwinm") X(RLWNM, "rlwnm") X(ORI, "ori") X(ORIS, "oris")              \
    X(XORI, "xori") X(XORIS, "xoris") X(ANDI_RC, "andi.") X(ANDIS_RC, "andis.")                         \
    /* branches and CR */                                                                               \
    X(BC, "bc") X(SC, "sc") X(B, "b") X(MCRF, "mcrf") X(BCLR, "bclr") X(CRNOR, "crnor") X(RFI, "rfi")  \
    X(CRANDC, "crandc") X(ISYNC, "isync") X(CRXOR, "crxor") X(CRNAND, "crnand") X(CRAND, "crand")       \
    X(CREQV, "creqv") X(CRORC, "crorc") X(CROR, "cror") X(BCCTR, "bcctr")                               \
    /* primary opcode 31 */                                                                             \
    X(CMP, "cmp") X(TW, "tw") X(SUBFC, "subfc") X(ADDC, "addc") X(MULHWU, "mulhwu") X(MFCR, "mfcr")     \
    X(LWARX, "lwarx") X(LWZX, "lwzx") X(SLW, "slw") X(CNTLZW, "cntlzw") X(AND, "and") X(CMPL, "cmpl")   \
    X(SUBF, "subf") X(DCBST, "dcbst") X(LWZUX, "lwzux") X(ANDC, "andc") X(MULHW, "mulhw")              \
    X(MFMSR, "mfmsr") X(DCBF, "dcbf") X(LBZX, "lbzx") X(NEG, "neg") X(LBZUX, "lbzux") X(NOR, "nor")     \
    X(SUBFE, "subfe") X(ADDE, "adde") X(MTCRF, "mtcrf") X(MTMSR, "mtmsr") X(STWCX_RC, "stwcx.")         \
    X(STWX, "stwx") X(STWUX, "stwux") X(SUBFZE, "subfze") X(ADDZE, "addze") X(MTSR, "mtsr")             \
    X(STBX, "stbx") X(SUBFME, "subfme") X(ADDME, "addme") X(MULLW, "mullw") X(MTSRIN, "mtsrin")         \
    X(DCBTST, "dcbtst") X(STBUX, "stbux") X(ADD, "add") X(DCBT, "dcbt") X(LHZX, "lhzx") X(EQV, "eqv")   \
    X(TLBIE, "tlbie") X(ECIWX, "eciwx") X(LHZUX, "lhzux") X(XOR, "xor") X(MFSPR, "mfspr")               \
    X(LHAX, "lhax") X(MFTB, "mftb") X(LHAUX, "lhaux") X(STHX, "sthx") X(ORC, "orc") X(ECOWX, "ecowx")   \
    X(STHUX, "sthux") X(OR, "or") X(DIVWU, "divwu") X(MTSPR, "mtspr") X(DCBI, "dcbi") X(NAND, "nand")   \
    X(DIVW, "divw") X(MCRXR, "mcrxr") X(LSWX, "lswx") X(LWBRX, "lwbrx") X(LFSX, "lfsx") X(SRW, "srw")   \
    X(TLBSYNC, "tlbsync") X(LFSUX, "lfsux") X(MFSR, "mfsr") X(LSWI, "lswi") X(SYNC, "sync")             \
    X(LFDX, "lfdx") X(LFDUX, "lfdux") X(MFSRIN, "mfsrin") X(STSWX, "stswx") X(STWBRX, "stwbrx")         \
    X(STFSX, "stfsx") X(STFSUX, "stfsux") X(STSWI, "stswi") X(STFDX, "stfdx") X(STFDUX, "stfdux")       \
    X(LHBRX, "lhbrx") X(SRAW, "sraw") X(SRAWI, "srawi") X(EIEIO, "eieio") X(STHBRX, "sthbrx")           \
    X(EXTSH, "extsh") X(EXTSB, "extsb") X(ICBI, "icbi") X(STFIWX, "stfiwx") X(DCBZ, "dcbz")             \
    /* D-form loads and stores */                                                                       \
    X(LWZ, "lwz") X(LWZU, "lwzu") X(LBZ, "lbz") X(LBZU, "lbzu") X(STW, "stw") X(STWU, "stwu")           \
    X(STB, "stb") X(STBU, "stbu") X(LHZ, "lhz") X(LHZU, "lhzu") X(LHA, "lha") X(LHAU, "lhau")           \
    X(STH, "sth") X(STHU, "sthu") X(LMW, "lmw") X(STMW, "stmw") X(LFS, "lfs") X(LFSU, "lfsu")           \
    X(LFD, "lfd") X(LFDU, "lfdu") X(STFS, "stfs") X(STFSU, "stfsu") X(STFD, "stfd") X(STFDU, "stfdu")   \
    /* paired-single loads and stores */                                                                \
    X(PSQ_L, "psq_l") X(PSQ_LU, "psq_lu") X(PSQ_ST, "psq_st") X(PSQ_STU, "psq_stu")                     \
    X(PSQ_LX, "psq_lx") X(PSQ_STX, "psq_stx") X(PSQ_LUX, "psq_lux") X(PSQ_STUX, "psq_stux")             \
    /* floating point, primary opcode 59 (single) */                                                    \
    X(FDIVS, "fdivs") X(FSUBS, "fsubs") X(FADDS, "fadds") X(FSQRTS, "fsqrts") X(FRES, "fres")           \
    X(FMULS, "fmuls") X(FMSUBS, "fmsubs") X(FMADDS, "fmadds") X(FNMSUBS, "fnmsubs")                     \
    X(FNMADDS, "fnmadds")                                                                               \
    /* floating point, primary opcode 63 (double) */                                                    \
    X(FCMPU, "fcmpu") X(FRSP, "frsp") X(FCTIW, "fctiw") X(FCTIWZ, "fctiwz") X(FDIV, "fdiv")             \
    X(FSUB, "fsub") X(FADD, "fadd") X(FSQRT, "fsqrt") X(FSEL, "fsel") X(FMUL, "fmul")                   \
    X(FRSQRTE, "frsqrte") X(FMSUB, "fmsub") X(FMADD, "fmadd") X(FNMSUB, "fnmsub") X(FNMADD, "fnmadd")   \
    X(FCMPO, "fcmpo") X(MTFSB1, "mtfsb1") X(FNEG, "fneg") X(MCRFS, "mcrfs") X(MTFSB0, "mtfsb0")         \
    X(FMR, "fmr") X(MTFSFI, "mtfsfi") X(FNABS, "fnabs") X(FABS, "fabs") X(MFFS, "mffs")                 \
    X(MTFSF, "mtfsf")                                                                                   \
    /* paired single, primary opcode 4 */                                                               \
    X(PS_CMPU0, "ps_cmpu0") X(PS_CMPO0, "ps_cmpo0") X(PS_NEG, "ps_neg") X(PS_CMPU1, "ps_cmpu1")         \
    X(PS_MR, "ps_mr") X(PS_CMPO1, "ps_cmpo1") X(PS_NABS, "ps_nabs") X(PS_ABS, "ps_abs")                 \
    X(PS_MERGE00, "ps_merge00") X(PS_MERGE01, "ps_merge01") X(PS_MERGE10, "ps_merge10")                 \
    X(PS_MERGE11, "ps_merge11") X(DCBZ_L, "dcbz_l") X(PS_SUM0, "ps_sum0") X(PS_SUM1, "ps_sum1")         \
    X(PS_MULS0, "ps_muls0") X(PS_MULS1, "ps_muls1") X(PS_MADDS0, "ps_madds0")                           \
    X(PS_MADDS1, "ps_madds1") X(PS_DIV, "ps_div") X(PS_SUB, "ps_sub") X(PS_ADD, "ps_add")               \
    X(PS_SEL, "ps_sel") X(PS_RES, "ps_res") X(PS_MUL, "ps_mul") X(PS_RSQRTE, "ps_rsqrte")               \
    X(PS_MSUB, "ps_msub") X(PS_MADD, "ps_madd") X(PS_NMSUB, "ps_nmsub") X(PS_NMADD, "ps_nmadd")

enum class Op : uint16_t
{
#define PPC_OP_ENUM(id, name) id,
    PPC_OPS(PPC_OP_ENUM)
#undef PPC_OP_ENUM
    COUNT
};

const char* OpName(Op op);

// Decode one big-endian instruction word (already byte-swapped to host order).
Op Decode(uint32_t w);

// Field accessors (IBM bit numbering converted to shifts).
namespace insn
{
inline uint32_t RD(uint32_t w) { return (w >> 21) & 31; }   // also rS, frD, frS, BO, TO, crbD
inline uint32_t RA(uint32_t w) { return (w >> 16) & 31; }   // also BI, crbA, frA
inline uint32_t RB(uint32_t w) { return (w >> 11) & 31; }   // also SH, NB, crbB, frB
inline uint32_t RC(uint32_t w) { return (w >> 6) & 31; }    // frC, MB (rlw*)
inline uint32_t ME(uint32_t w) { return (w >> 1) & 31; }
inline bool Rc(uint32_t w) { return w & 1; }                 // also LK
inline bool OE(uint32_t w) { return (w >> 10) & 1; }
inline bool AA(uint32_t w) { return (w >> 1) & 1; }
inline int32_t SIMM(uint32_t w) { return int16_t(w & 0xFFFF); }
inline uint32_t UIMM(uint32_t w) { return w & 0xFFFF; }
inline uint32_t CRFD(uint32_t w) { return (w >> 23) & 7; }
inline uint32_t CRFS(uint32_t w) { return (w >> 18) & 7; }
inline uint32_t L(uint32_t w) { return (w >> 21) & 1; }
inline uint32_t CRM(uint32_t w) { return (w >> 12) & 0xFF; }
inline uint32_t FM(uint32_t w) { return (w >> 17) & 0xFF; }
inline uint32_t SPR(uint32_t w) { return ((w >> 16) & 31) | (((w >> 11) & 31) << 5); }
inline int32_t LI(uint32_t w) { int32_t v = w & 0x03FFFFFC; return (v & 0x02000000) ? v - 0x04000000 : v; }
inline int32_t BD(uint32_t w) { return int16_t(w & 0xFFFC); }
// psq_l/psq_st (D-form) and psq_lx/psq_stx (X-form, opcode 4)
inline uint32_t PSQ_W(uint32_t w) { return (w >> 15) & 1; }
inline uint32_t PSQ_I(uint32_t w) { return (w >> 12) & 7; }
inline int32_t PSQ_D(uint32_t w) { int32_t v = w & 0xFFF; return (v & 0x800) ? v - 0x1000 : v; }
inline uint32_t PSQX_W(uint32_t w) { return (w >> 10) & 1; }
inline uint32_t PSQX_I(uint32_t w) { return (w >> 7) & 7; }
} // namespace insn
