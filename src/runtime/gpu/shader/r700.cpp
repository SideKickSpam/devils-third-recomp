#include "r700.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gpu::r700
{
namespace
{
#include "r700_opcodes.inc"

uint32_t Bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

const char* kCfAluNames[16] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "ALU",
    "ALU_PUSH_BEFORE", "ALU_POP_AFTER", "ALU_POP2_AFTER", nullptr, "ALU_CONTINUE", "ALU_BREAK", "ALU_ELSE_AFTER" };

// OP2 opcodes with one source operand.
bool OneSource(uint32_t op)
{
    switch (op)
    {
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 24: case 25:  // FRACT..MOV
    case 51:                                                                           // NOT_INT
    case 36: case 39:                                                                  // PRED_SET_INV, _RESTORE
    case 97: case 98: case 99: case 100: case 101: case 102: case 103: case 104: case 105: case 106:
    case 107: case 108: case 109: case 110: case 111: case 119: case 120: case 121:
        return true;
    default:
        return false;
    }
}

bool ZeroSources(uint32_t op) { return op == 26 /* NOP */ || op == 38 /* PRED_SET_CLR */; }
} // namespace

const char* AluName(bool op3, uint32_t inst)
{
    const char* n = op3 ? (inst < 32 ? kOp3Names[inst] : nullptr) : (inst < 128 ? kOp2Names[inst] : nullptr);
    return n ? n : "?";
}

const char* TexName(uint32_t inst) { return inst < 32 && kTexNames[inst] ? kTexNames[inst] : "?"; }

const char* CfName(const Cf& cf)
{
    if (cf.kind == Cf::Alu)
        return cf.inst < 16 && kCfAluNames[cf.inst] ? kCfAluNames[cf.inst] : "ALU?";
    // The generated table took the ALU clause names for 8-15 (the ISA lists both encodings together).
    static const char* const kFlow[] = { "LOOP_CONTINUE", "LOOP_BREAK", "JUMP", "PUSH", "PUSH_ELSE", "ELSE", "POP",
        "POP_JUMP" };
    if (cf.inst >= 8 && cf.inst < 16)
        return kFlow[cf.inst - 8];
    return cf.inst < 64 && kCfNames[cf.inst] ? kCfNames[cf.inst] : "CF?";
}

uint32_t NumSources(bool op3, uint32_t inst)
{
    if (op3)
        return 3;
    if (ZeroSources(inst))
        return 0;
    return OneSource(inst) ? 1 : 2;
}

bool IsTransOnly(const Alu& a)
{
    if (a.op3)
        return false;
    return (a.inst >= 97 && a.inst <= 111) || (a.inst >= 115 && a.inst <= 121);  // EXP_IEEE..COS, MULLO_INT..
}

bool IsReduction(const Alu& a) { return !a.op3 && a.inst >= 80 && a.inst <= 83; }  // DOT4, DOT4_IEEE, CUBE, MAX4

Program Decode(const uint8_t* data, size_t size)
{
    Program p;
    const size_t words = size / 4;
    auto word = [&](size_t i) -> uint32_t {
        if (i >= words)
            return 0;
        uint32_t v;
        memcpy(&v, data + i * 4, 4);  // little-endian GPU words; hosts are little-endian
        return v;
    };

    for (uint32_t i = 0;; i++)
    {
        if (2 * i + 1 >= words)
        {
            p.error = "CF program runs past the end without END_OF_PROGRAM";
            return p;
        }
        const uint32_t w0 = word(2 * i), w1 = word(2 * i + 1);
        Cf cf{};
        cf.index = i;
        cf.barrier = Bits(w1, 31, 31);
        cf.wholeQuadMode = Bits(w1, 30, 30);
        if (Bits(w1, 29, 29))  // CF_ALU_WORD0/1: the 4-bit CF_INST is 8-15
        {
            cf.kind = Cf::Alu;
            cf.inst = Bits(w1, 29, 26);
            cf.addr = Bits(w0, 21, 0);
            cf.kcacheBank[0] = Bits(w0, 25, 22);
            cf.kcacheBank[1] = Bits(w0, 29, 26);
            cf.kcacheMode[0] = Bits(w0, 31, 30);
            cf.kcacheMode[1] = Bits(w1, 1, 0);
            cf.kcacheAddr[0] = Bits(w1, 9, 2);
            cf.kcacheAddr[1] = Bits(w1, 17, 10);
            cf.count = Bits(w1, 24, 18) + 1;
        }
        else
        {
            cf.inst = Bits(w1, 29, 23);
            cf.endOfProgram = Bits(w1, 21, 21);
            cf.validPixelMode = Bits(w1, 22, 22);
            if (cf.inst >= 32)  // CF_ALLOC_EXPORT_WORD0 / WORD1_SWIZ
            {
                cf.kind = Cf::Export;
                cf.arrayBase = Bits(w0, 12, 0);
                cf.exportType = Bits(w0, 14, 13);
                cf.rwGpr = Bits(w0, 21, 15);
                cf.rwRel = Bits(w0, 22, 22);
                cf.indexGpr = Bits(w0, 29, 23);
                cf.elemSize = Bits(w0, 31, 30);
                for (int k = 0; k < 4; k++)
                    cf.sel[k] = Bits(w1, 3 * k + 2, 3 * k);
                cf.burstCount = Bits(w1, 20, 17) + 1;
            }
            else
            {
                cf.kind = Cf::Normal;
                cf.addr = w0;
                cf.popCount = Bits(w1, 2, 0);
                cf.cfConst = Bits(w1, 7, 3);
                cf.cond = Bits(w1, 9, 8);
                cf.count = (Bits(w1, 12, 10) | (Bits(w1, 19, 19) << 3)) + 1;
                cf.callCount = Bits(w1, 18, 13);
            }
        }
        p.cf.push_back(cf);
        if (cf.endOfProgram)
            break;
        if (i > 4096)
        {
            p.error = "CF program too long";
            return p;
        }
    }

    p.alu.resize(p.cf.size());
    p.tex.resize(p.cf.size());
    p.vtx.resize(p.cf.size());
    for (const Cf& cf : p.cf)
    {
        if (cf.kind == Cf::Alu)
        {
            // cf.count is the number of 64-bit slots in the clause, literals included.
            size_t pos = cf.addr * 2;
            const size_t end = pos + size_t(cf.count) * 2;
            while (pos < end)
            {
                AluGroup g;
                bool used[4] = {};
                uint32_t maxLiteralChan = 0;
                bool hasLiteral = false;
                for (int n = 0;; n++)
                {
                    if (pos + 1 >= words || n >= 5)
                    {
                        p.error = "malformed ALU group";
                        return p;
                    }
                    const uint32_t a0 = word(pos), a1 = word(pos + 1);
                    pos += 2;
                    Alu a{};
                    a.op3 = Bits(a1, 17, 15) != 0;
                    a.inst = a.op3 ? Bits(a1, 17, 13) : Bits(a1, 17, 7);
                    a.src[0] = { Bits(a0, 8, 0), Bits(a0, 11, 10), bool(Bits(a0, 9, 9)), bool(Bits(a0, 12, 12)), false };
                    a.src[1] = { Bits(a0, 21, 13), Bits(a0, 24, 23), bool(Bits(a0, 22, 22)), bool(Bits(a0, 25, 25)), false };
                    a.indexMode = Bits(a0, 28, 26);
                    a.predSel = Bits(a0, 30, 29);
                    a.last = Bits(a0, 31, 31);
                    a.bankSwizzle = Bits(a1, 20, 18);
                    a.dstGpr = Bits(a1, 27, 21);
                    a.dstRel = Bits(a1, 28, 28);
                    a.dstChan = Bits(a1, 30, 29);
                    a.clamp = Bits(a1, 31, 31);
                    if (a.op3)
                    {
                        a.src[2] = { Bits(a1, 8, 0), Bits(a1, 11, 10), bool(Bits(a1, 9, 9)), bool(Bits(a1, 12, 12)), false };
                        a.writeMask = true;
                    }
                    else
                    {
                        a.src[0].abs = Bits(a1, 0, 0);
                        a.src[1].abs = Bits(a1, 1, 1);
                        a.updateExecMask = Bits(a1, 2, 2);
                        a.updatePred = Bits(a1, 3, 3);
                        a.writeMask = Bits(a1, 4, 4);
                        a.omod = Bits(a1, 6, 5);
                    }
                    a.numSrc = NumSources(a.op3, a.inst);
                    for (uint32_t s = 0; s < a.numSrc; s++)
                        if (a.src[s].sel == SRC_LITERAL)
                        {
                            hasLiteral = true;
                            maxLiteralChan = std::max(maxLiteralChan, a.src[s].chan);
                        }
                    // Slot assignment: in order, to the vector slot of the destination channel unless the
                    // instruction is trans-only or that slot is taken, in which case to the trans slot.
                    if (IsTransOnly(a) || used[a.dstChan])
                        a.slot = 4;
                    else
                    {
                        a.slot = a.dstChan;
                        used[a.dstChan] = true;
                    }
                    g.insts.push_back(a);
                    if (a.last)
                        break;
                }
                if (hasLiteral)
                {
                    const uint32_t n = maxLiteralChan >= 2 ? 4 : 2;  // literals come in 64-bit pairs
                    for (uint32_t k = 0; k < n; k++)
                        g.literals[k] = word(pos + k);
                    pos += n;
                }
                p.alu[cf.index].push_back(g);
            }
        }
        else if (cf.kind == Cf::Normal && (cf.inst == CF_VTX || cf.inst == CF_VTX_TC))
        {
            for (uint32_t k = 0; k < cf.count; k++)
            {
                const size_t base = cf.addr * 2 + k * 4;
                const uint32_t v0 = word(base), v1 = word(base + 1), v2 = word(base + 2);
                Vtx v{};
                v.inst = Bits(v0, 4, 0);
                v.fetchType = Bits(v0, 6, 5);
                v.bufferId = Bits(v0, 15, 8);
                v.srcGpr = Bits(v0, 22, 16);
                v.srcRel = Bits(v0, 23, 23);
                v.srcSel = Bits(v0, 25, 24);
                v.megaFetchCount = Bits(v0, 31, 26) + 1;
                v.dstGpr = v.inst == VTX_SEMANTIC ? Bits(v1, 7, 0) : Bits(v1, 6, 0);
                v.dstRel = v.inst != VTX_SEMANTIC && Bits(v1, 7, 7);
                for (int c = 0; c < 4; c++)
                    v.dstSel[c] = Bits(v1, 11 + 3 * c, 9 + 3 * c);
                v.useConstFields = Bits(v1, 21, 21);
                v.dataFormat = Bits(v1, 27, 22);
                v.numFormat = Bits(v1, 29, 28);
                v.formatSigned = Bits(v1, 30, 30);
                v.offset = Bits(v2, 15, 0);
                v.endianSwap = Bits(v2, 17, 16);
                v.megaFetch = Bits(v2, 19, 19);
                p.vtx[cf.index].push_back(v);
            }
        }
        else if (cf.kind == Cf::Normal && cf.inst == CF_TEX)
        {
            for (uint32_t k = 0; k < cf.count; k++)
            {
                const size_t base = cf.addr * 2 + k * 4;
                const uint32_t t0 = word(base), t1 = word(base + 1), t2 = word(base + 2);
                Tex t{};
                t.inst = Bits(t0, 4, 0);
                t.bcFracMode = Bits(t0, 5, 5);
                t.fetchWholeQuad = Bits(t0, 7, 7);
                t.resourceId = Bits(t0, 15, 8);
                t.srcGpr = Bits(t0, 22, 16);
                t.srcRel = Bits(t0, 23, 23);
                t.dstGpr = Bits(t1, 6, 0);
                t.dstRel = Bits(t1, 7, 7);
                for (int c = 0; c < 4; c++)
                {
                    t.dstSel[c] = Bits(t1, 11 + 3 * c, 9 + 3 * c);
                    t.coordNormalized[c] = Bits(t1, 28 + c, 28 + c);
                    t.srcSel[c] = Bits(t2, 22 + 3 * c, 20 + 3 * c);
                }
                const int32_t lod = int32_t(Bits(t1, 27, 21));
                t.lodBias = lod >= 64 ? lod - 128 : lod;  // signed 7-bit, 1/16 units... kept raw
                for (int c = 0; c < 3; c++)
                {
                    const int32_t o = int32_t(Bits(t2, 4 + 5 * c, 5 * c));
                    t.offset[c] = o >= 16 ? o - 32 : o;
                }
                t.samplerId = Bits(t2, 19, 15);
                p.tex[cf.index].push_back(t);
            }
        }
    }
    return p;
}

std::string Disassemble(const Program& p)
{
    std::string out;
    char buf[256];
    auto srcText = [&](const AluSource& s, const AluGroup& g) {
        std::string t = s.neg ? "-" : "";
        if (s.abs)
            t += "|";
        const char* ch = "xyzw";
        if (s.sel < 128)
            snprintf(buf, sizeof(buf), "R%u.%c", s.sel, ch[s.chan]);
        else if (s.sel < 160)
            snprintf(buf, sizeof(buf), "KC0[%u].%c", s.sel - 128, ch[s.chan]);
        else if (s.sel < 192)
            snprintf(buf, sizeof(buf), "KC1[%u].%c", s.sel - 160, ch[s.chan]);
        else if (s.sel >= SRC_CFILE)
            snprintf(buf, sizeof(buf), "C%u.%c", s.sel - SRC_CFILE, ch[s.chan]);
        else
            switch (s.sel)
            {
            case SRC_0: snprintf(buf, sizeof(buf), "0.0"); break;
            case SRC_1: snprintf(buf, sizeof(buf), "1.0"); break;
            case SRC_1_INT: snprintf(buf, sizeof(buf), "1i"); break;
            case SRC_M_1_INT: snprintf(buf, sizeof(buf), "-1i"); break;
            case SRC_0_5: snprintf(buf, sizeof(buf), "0.5"); break;
            case SRC_LITERAL:
            {
                float f;
                memcpy(&f, &g.literals[s.chan], 4);
                snprintf(buf, sizeof(buf), "(0x%08X %g)", g.literals[s.chan], double(f));
                break;
            }
            case SRC_PV: snprintf(buf, sizeof(buf), "PV.%c", ch[s.chan]); break;
            case SRC_PS: snprintf(buf, sizeof(buf), "PS"); break;
            default: snprintf(buf, sizeof(buf), "SRC%u", s.sel); break;
            }
        t += buf;
        if (s.rel)
            t += "[AL]";
        if (s.abs)
            t += "|";
        return t;
    };
    const char* sel = "xyzw01?_";
    for (const Cf& cf : p.cf)
    {
        snprintf(buf, sizeof(buf), "%02u %s", cf.index, CfName(cf));
        out += buf;
        if (cf.kind == Cf::Alu)
        {
            snprintf(buf, sizeof(buf), " addr %u count %u", cf.addr, cf.count);
            out += buf;
            if (cf.kcacheMode[0] || cf.kcacheMode[1])
            {
                snprintf(buf, sizeof(buf), " kcache0 bank %u addr %u mode %u, kcache1 bank %u addr %u mode %u",
                    cf.kcacheBank[0], cf.kcacheAddr[0], cf.kcacheMode[0], cf.kcacheBank[1], cf.kcacheAddr[1], cf.kcacheMode[1]);
                out += buf;
            }
        }
        else if (cf.kind == Cf::Export)
        {
            static const char* types[] = { "PIXEL", "POS", "PARAM", "?" };
            snprintf(buf, sizeof(buf), " %s%u R%u.%c%c%c%c burst %u", types[cf.exportType & 3], cf.arrayBase, cf.rwGpr,
                sel[cf.sel[0]], sel[cf.sel[1]], sel[cf.sel[2]], sel[cf.sel[3]], cf.burstCount);
            out += buf;
        }
        else if (cf.inst != CF_NOP)
        {
            snprintf(buf, sizeof(buf), " addr %u count %u cond %u pop %u", cf.addr, cf.count, cf.cond, cf.popCount);
            out += buf;
        }
        if (cf.endOfProgram)
            out += " END";
        out += "\n";
        int groupIndex = 0;
        for (const AluGroup& g : p.alu[cf.index])
        {
            for (const Alu& a : g.insts)
            {
                const char* slots = "xyzwt";
                std::string line = "     " + std::to_string(groupIndex) + " " + slots[a.slot] + ": " + AluName(a.op3, a.inst);
                if (a.clamp)
                    line += "_SAT";
                if (a.op3 || a.writeMask)
                    line += " R" + std::to_string(a.dstGpr) + "." + "xyzw"[a.dstChan];
                else
                    line += " ____";
                for (uint32_t s = 0; s < a.numSrc; s++)
                    line += ", " + srcText(a.src[s], g);
                if (a.omod)
                    line += a.omod == 1 ? " *2" : a.omod == 2 ? " *4" : " /2";
                if (a.predSel)
                    line += " pred" + std::to_string(a.predSel);
                out += line + "\n";
            }
            groupIndex++;
        }
        for (const Vtx& v : p.vtx[cf.index])
        {
            const char* inst = v.inst == VTX_FETCH ? "VFETCH" : v.inst == VTX_SEMANTIC ? "VFETCH_SEMANTIC" : "VTX_MEM";
            snprintf(buf, sizeof(buf), "     %s %s%u.%c%c%c%c, R%u.%c, b%u fmt %u num %u%s off %u swap %u type %u\n", inst,
                v.inst == VTX_SEMANTIC ? "SEM" : "R", v.dstGpr, sel[v.dstSel[0]], sel[v.dstSel[1]], sel[v.dstSel[2]],
                sel[v.dstSel[3]], v.srcGpr, "xyzw"[v.srcSel], v.bufferId, v.dataFormat, v.numFormat,
                v.formatSigned ? " signed" : "", v.offset, v.endianSwap, v.fetchType);
            out += buf;
        }
        for (const Tex& t : p.tex[cf.index])
        {
            snprintf(buf, sizeof(buf), "     %s R%u.%c%c%c%c, R%u.%c%c%c%c, t%u, s%u%s\n", TexName(t.inst), t.dstGpr,
                sel[t.dstSel[0]], sel[t.dstSel[1]], sel[t.dstSel[2]], sel[t.dstSel[3]], t.srcGpr, sel[t.srcSel[0]],
                sel[t.srcSel[1]], sel[t.srcSel[2]], sel[t.srcSel[3]], t.resourceId, t.samplerId,
                t.coordNormalized[0] ? "" : " (unnormalized)");
            out += buf;
        }
    }
    if (!p.error.empty())
        out += "error: " + p.error + "\n";
    return out;
}
} // namespace gpu::r700
