#include "decoder.h"

const char* OpName(Op op)
{
    static const char* names[] = {
#define PPC_OP_NAME(id, name) name,
        PPC_OPS(PPC_OP_NAME)
#undef PPC_OP_NAME
    };
    return names[static_cast<int>(op)];
}

static Op DecodeOp4(uint32_t w)
{
    // Order matters: 6-bit XO (psq_*x), then 10-bit XO, then 5-bit A-form XO.
    switch ((w >> 1) & 0x3F)
    {
    case 6: return Op::PSQ_LX;
    case 7: return Op::PSQ_STX;
    case 38: return Op::PSQ_LUX;
    case 39: return Op::PSQ_STUX;
    }
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::PS_CMPU0;
    case 32: return Op::PS_CMPO0;
    case 40: return Op::PS_NEG;
    case 64: return Op::PS_CMPU1;
    case 72: return Op::PS_MR;
    case 96: return Op::PS_CMPO1;
    case 136: return Op::PS_NABS;
    case 264: return Op::PS_ABS;
    case 528: return Op::PS_MERGE00;
    case 560: return Op::PS_MERGE01;
    case 592: return Op::PS_MERGE10;
    case 624: return Op::PS_MERGE11;
    case 1014: return Op::DCBZ_L;
    }
    switch ((w >> 1) & 0x1F)
    {
    case 10: return Op::PS_SUM0;
    case 11: return Op::PS_SUM1;
    case 12: return Op::PS_MULS0;
    case 13: return Op::PS_MULS1;
    case 14: return Op::PS_MADDS0;
    case 15: return Op::PS_MADDS1;
    case 18: return Op::PS_DIV;
    case 20: return Op::PS_SUB;
    case 21: return Op::PS_ADD;
    case 23: return Op::PS_SEL;
    case 24: return Op::PS_RES;
    case 25: return Op::PS_MUL;
    case 26: return Op::PS_RSQRTE;
    case 28: return Op::PS_MSUB;
    case 29: return Op::PS_MADD;
    case 30: return Op::PS_NMSUB;
    case 31: return Op::PS_NMADD;
    }
    return Op::INVALID;
}

static Op DecodeOp19(uint32_t w)
{
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::MCRF;
    case 16: return Op::BCLR;
    case 33: return Op::CRNOR;
    case 50: return Op::RFI;
    case 129: return Op::CRANDC;
    case 150: return Op::ISYNC;
    case 193: return Op::CRXOR;
    case 225: return Op::CRNAND;
    case 257: return Op::CRAND;
    case 289: return Op::CREQV;
    case 417: return Op::CRORC;
    case 449: return Op::CROR;
    case 528: return Op::BCCTR;
    }
    return Op::INVALID;
}

static Op DecodeOp31(uint32_t w)
{
    // X-form ops use a 10-bit XO. None of their values collide with the XO-form
    // arithmetic ops below (9-bit XO plus OE), with or without OE set.
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::CMP;
    case 4: return Op::TW;
    case 19: return Op::MFCR;
    case 20: return Op::LWARX;
    case 23: return Op::LWZX;
    case 24: return Op::SLW;
    case 26: return Op::CNTLZW;
    case 28: return Op::AND;
    case 32: return Op::CMPL;
    case 54: return Op::DCBST;
    case 55: return Op::LWZUX;
    case 60: return Op::ANDC;
    case 83: return Op::MFMSR;
    case 86: return Op::DCBF;
    case 87: return Op::LBZX;
    case 119: return Op::LBZUX;
    case 124: return Op::NOR;
    case 144: return Op::MTCRF;
    case 146: return Op::MTMSR;
    case 150: return Op::STWCX_RC;
    case 151: return Op::STWX;
    case 183: return Op::STWUX;
    case 210: return Op::MTSR;
    case 215: return Op::STBX;
    case 242: return Op::MTSRIN;
    case 246: return Op::DCBTST;
    case 247: return Op::STBUX;
    case 278: return Op::DCBT;
    case 279: return Op::LHZX;
    case 284: return Op::EQV;
    case 306: return Op::TLBIE;
    case 310: return Op::ECIWX;
    case 311: return Op::LHZUX;
    case 316: return Op::XOR;
    case 339: return Op::MFSPR;
    case 343: return Op::LHAX;
    case 371: return Op::MFTB;
    case 375: return Op::LHAUX;
    case 407: return Op::STHX;
    case 412: return Op::ORC;
    case 438: return Op::ECOWX;
    case 439: return Op::STHUX;
    case 444: return Op::OR;
    case 467: return Op::MTSPR;
    case 470: return Op::DCBI;
    case 476: return Op::NAND;
    case 512: return Op::MCRXR;
    case 533: return Op::LSWX;
    case 534: return Op::LWBRX;
    case 535: return Op::LFSX;
    case 536: return Op::SRW;
    case 566: return Op::TLBSYNC;
    case 567: return Op::LFSUX;
    case 595: return Op::MFSR;
    case 597: return Op::LSWI;
    case 598: return Op::SYNC;
    case 599: return Op::LFDX;
    case 631: return Op::LFDUX;
    case 659: return Op::MFSRIN;
    case 661: return Op::STSWX;
    case 662: return Op::STWBRX;
    case 663: return Op::STFSX;
    case 695: return Op::STFSUX;
    case 725: return Op::STSWI;
    case 727: return Op::STFDX;
    case 759: return Op::STFDUX;
    case 790: return Op::LHBRX;
    case 792: return Op::SRAW;
    case 824: return Op::SRAWI;
    case 854: return Op::EIEIO;
    case 918: return Op::STHBRX;
    case 922: return Op::EXTSH;
    case 954: return Op::EXTSB;
    case 982: return Op::ICBI;
    case 983: return Op::STFIWX;
    case 1014: return Op::DCBZ;
    }
    switch ((w >> 1) & 0x1FF)
    {
    case 8: return Op::SUBFC;
    case 10: return Op::ADDC;
    case 11: return (w & 0x400) ? Op::INVALID : Op::MULHWU;
    case 40: return Op::SUBF;
    case 75: return (w & 0x400) ? Op::INVALID : Op::MULHW;
    case 104: return Op::NEG;
    case 136: return Op::SUBFE;
    case 138: return Op::ADDE;
    case 200: return Op::SUBFZE;
    case 202: return Op::ADDZE;
    case 232: return Op::SUBFME;
    case 234: return Op::ADDME;
    case 235: return Op::MULLW;
    case 266: return Op::ADD;
    case 459: return Op::DIVWU;
    case 491: return Op::DIVW;
    }
    return Op::INVALID;
}

static Op DecodeOp59(uint32_t w)
{
    switch ((w >> 1) & 0x1F)
    {
    case 18: return Op::FDIVS;
    case 20: return Op::FSUBS;
    case 21: return Op::FADDS;
    case 22: return Op::FSQRTS;
    case 24: return Op::FRES;
    case 25: return Op::FMULS;
    case 28: return Op::FMSUBS;
    case 29: return Op::FMADDS;
    case 30: return Op::FNMSUBS;
    case 31: return Op::FNMADDS;
    }
    return Op::INVALID;
}

static Op DecodeOp63(uint32_t w)
{
    // A-form ops have a 5-bit XO of 16 or more; X-form ops all have low 5 bits below 16.
    const uint32_t xo5 = (w >> 1) & 0x1F;
    if (xo5 >= 16)
    {
        switch (xo5)
        {
        case 18: return Op::FDIV;
        case 20: return Op::FSUB;
        case 21: return Op::FADD;
        case 22: return Op::FSQRT;
        case 23: return Op::FSEL;
        case 25: return Op::FMUL;
        case 26: return Op::FRSQRTE;
        case 28: return Op::FMSUB;
        case 29: return Op::FMADD;
        case 30: return Op::FNMSUB;
        case 31: return Op::FNMADD;
        }
        return Op::INVALID;
    }
    switch ((w >> 1) & 0x3FF)
    {
    case 0: return Op::FCMPU;
    case 12: return Op::FRSP;
    case 14: return Op::FCTIW;
    case 15: return Op::FCTIWZ;
    case 32: return Op::FCMPO;
    case 38: return Op::MTFSB1;
    case 40: return Op::FNEG;
    case 64: return Op::MCRFS;
    case 70: return Op::MTFSB0;
    case 72: return Op::FMR;
    case 134: return Op::MTFSFI;
    case 136: return Op::FNABS;
    case 264: return Op::FABS;
    case 583: return Op::MFFS;
    case 711: return Op::MTFSF;
    }
    return Op::INVALID;
}

Op Decode(uint32_t w)
{
    switch (w >> 26)
    {
    case 3: return Op::TWI;
    case 4: return DecodeOp4(w);
    case 7: return Op::MULLI;
    case 8: return Op::SUBFIC;
    case 10: return insn::L(w) ? Op::INVALID : Op::CMPLI;
    case 11: return insn::L(w) ? Op::INVALID : Op::CMPI;
    case 12: return Op::ADDIC;
    case 13: return Op::ADDIC_RC;
    case 14: return Op::ADDI;
    case 15: return Op::ADDIS;
    case 16: return Op::BC;
    case 17: return (w & 2) ? Op::SC : Op::INVALID;
    case 18: return Op::B;
    case 19: return DecodeOp19(w);
    case 20: return Op::RLWIMI;
    case 21: return Op::RLWINM;
    case 23: return Op::RLWNM;
    case 24: return Op::ORI;
    case 25: return Op::ORIS;
    case 26: return Op::XORI;
    case 27: return Op::XORIS;
    case 28: return Op::ANDI_RC;
    case 29: return Op::ANDIS_RC;
    case 31:
    {
        const Op op = DecodeOp31(w);
        // cmp/cmpl with L=1 are 64-bit compares, invalid on Espresso.
        if ((op == Op::CMP || op == Op::CMPL) && insn::L(w))
            return Op::INVALID;
        return op;
    }
    case 32: return Op::LWZ;
    case 33: return Op::LWZU;
    case 34: return Op::LBZ;
    case 35: return Op::LBZU;
    case 36: return Op::STW;
    case 37: return Op::STWU;
    case 38: return Op::STB;
    case 39: return Op::STBU;
    case 40: return Op::LHZ;
    case 41: return Op::LHZU;
    case 42: return Op::LHA;
    case 43: return Op::LHAU;
    case 44: return Op::STH;
    case 45: return Op::STHU;
    case 46: return Op::LMW;
    case 47: return Op::STMW;
    case 48: return Op::LFS;
    case 49: return Op::LFSU;
    case 50: return Op::LFD;
    case 51: return Op::LFDU;
    case 52: return Op::STFS;
    case 53: return Op::STFSU;
    case 54: return Op::STFD;
    case 55: return Op::STFDU;
    case 56: return Op::PSQ_L;
    case 57: return Op::PSQ_LU;
    case 59: return DecodeOp59(w);
    case 60: return Op::PSQ_ST;
    case 61: return Op::PSQ_STU;
    case 63: return DecodeOp63(w);
    }
    return Op::INVALID;
}
