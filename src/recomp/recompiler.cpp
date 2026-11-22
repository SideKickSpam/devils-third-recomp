// Translates Espresso PowerPC functions into C++ against ppc_context.h.
// Instruction semantics adapted from XenonRecomp (MIT, hedge-dev and contributors), rewritten for a
// 32-bit register model, paired singles and Wii U RPX imports.
#include "recompiler.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <set>

using namespace insn;

namespace
{
std::string Hex(uint32_t v) { return std::format("0x{:X}u", v); }

// IBM mask for rlw*: bits mb..me set (bit 0 = MSB), wrapping when mb > me.
uint32_t RlwMask(uint32_t mb, uint32_t me)
{
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

std::string Gpr(uint32_t n) { return std::format("ctx.r[{}].u32", n); }
std::string GprS(uint32_t n) { return std::format("ctx.r[{}].s32", n); }

// (rA|0) + d
std::string EaD(uint32_t ra, int32_t d)
{
    if (ra == 0)
        return Hex(uint32_t(d));
    if (d == 0)
        return Gpr(ra);
    return std::format("{} + {}", Gpr(ra), Hex(uint32_t(d)));
}

// (rA|0) + rB
std::string EaX(uint32_t ra, uint32_t rb) { return ra == 0 ? Gpr(rb) : std::format("{} + {}", Gpr(ra), Gpr(rb)); }

bool IsUnconditionalEnd(Op op, uint32_t w)
{
    switch (op)
    {
    case Op::B: return !Rc(w);
    case Op::BCLR:
    case Op::BCCTR: return !Rc(w) && (RD(w) & 0x14) == 0x14;
    default: return false;
    }
}
} // namespace

Recompiler::Recompiler(const RpxImage& img) : image(img)
{
    for (const Function& f : image.functions)
        functionStarts.insert(f.addr);
    std::set<std::string> used;
    for (const Import& imp : image.imports)
    {
        std::string name = "imp_" + imp.lib + "_";
        for (char c : imp.name)
            name += (isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
        if (!used.insert(name).second)
            name += std::format("_{:08X}", imp.addr);
        importNames.push_back(name);
    }
}

std::string Recompiler::FunctionName(uint32_t addr) { return std::format("sub_{:08X}", addr); }
std::string Recompiler::ImportName(size_t index) const { return importNames[index]; }

void Recompiler::RecompileFunction(const Function& fn, std::string& out)
{
    auto print = [&out]<class... Args>(std::format_string<Args...> f, Args&&... args) {
        std::format_to(std::back_inserter(out), f, std::forward<Args>(args)...);
    };

    const uint32_t start = fn.addr, end = fn.addr + fn.size;
    auto inFunction = [&](uint32_t a) { return a >= start && a < end; };

    // ---- pass 1: labels and switch tables ----
    std::set<uint32_t> labels;
    std::map<uint32_t, uint32_t> switchTables;  // bcctr address -> number of entries
    for (uint32_t a = start; a < end; a += 4)
    {
        const uint32_t w = image.ReadCode(a);
        const Op op = Decode(w);
        if (op == Op::B && !AA(w) && !Rc(w) && inFunction(a + LI(w)))
            labels.insert(a + LI(w));
        else if (op == Op::BC && !AA(w) && inFunction(a + BD(w)))
            labels.insert(a + BD(w));
        else if (op == Op::BCCTR && !Rc(w) && image.codeRefsInsideFunctions.count(a + 4))
        {
            // GHS switch: ctr = (address of the table directly after this bcctr) + index * 4,
            // and the table is a run of unconditional branches.
            uint32_t n = 0;
            for (uint32_t e = a + 4; e < end && n < 4096; e += 4, n++)
            {
                const uint32_t ew = image.ReadCode(e);
                if (Decode(ew) != Op::B || AA(ew) || Rc(ew))
                    break;
                labels.insert(e);
            }
            if (n)
                switchTables[a] = n;
        }
    }

    // ---- emit ----
    stats.functions++;
    print("// {}\n", fn.name);
    const bool isHooked = hooked.count(start) != 0;
    print("PPC_FUNC({}{}) {{\n", isHooked ? "orig_" : "", FunctionName(start));
    print("\tPPC_FUNC_PROLOGUE({});\n", Hex(start));
    print("\tuint32_t ea;\n\t(void)ea;\n");

    auto call = [&](uint32_t from, uint32_t target) -> std::string {
        if (auto it = image.importCalls.find(from); it != image.importCalls.end())
        {
            stats.importCallSites++;
            return std::format("{}(ctx, base);", importNames[it->second]);
        }
        if (functionStarts.count(target))
            return std::format("{}(ctx, base);", FunctionName(target));
        stats.badBranches++;
        return std::format("ppc_bad_branch(ctx, {}, {});", Hex(from), Hex(target));
    };

    // Condition for bc/bclr/bcctr. Emits the CTR decrement first if BO asks for it.
    auto branchCondition = [&](uint32_t w) -> std::string {
        const uint32_t bo = RD(w), bi = RA(w);
        std::string cond;
        if (!(bo & 4))
        {
            print("\tctx.ctr--;\n");
            cond = (bo & 2) ? "ctx.ctr == 0" : "ctx.ctr != 0";
        }
        if (!(bo & 16))
        {
            std::string c = std::format("{}ctx.cr[{}]", (bo & 8) ? "" : "!", bi);
            cond = cond.empty() ? c : std::format("{} && {}", cond, c);
        }
        return cond;  // empty = always
    };

    auto unsupported = [&](uint32_t a, Op op) {
        stats.unsupported[OpName(op)]++;
        print("\tppc_unsupported(ctx, {}, \"{}\");\n", Hex(a), OpName(op));
    };

    uint32_t lastWord = 0;
    Op lastOp = Op::INVALID;
    for (uint32_t a = start; a < end; a += 4)
    {
        const uint32_t w = image.ReadCode(a);
        const Op op = Decode(w);
        lastWord = w;
        lastOp = op;
        stats.instructions++;
        stats.opCounts[static_cast<size_t>(op)]++;

        if (labels.count(a))
            print("loc_{:08X}:\n", a);
        print("\t// {:08X} {}\n", a, OpName(op));

        const uint32_t d = RD(w), ra = RA(w), rb = RB(w), rc = RC(w);
        const std::string next = Hex(a + 4);

        auto record = [&](const std::string& reg) {
            if (Rc(w))
                print("\tppc_record(ctx, {});\n", reg);
        };
        auto recordFp = [&]() {
            if (Rc(w))
                print("\tppc_record_fp(ctx);\n");
        };

        // XO-form arithmetic: result r from a (rA) and b (rB), optional carry and overflow.
        auto arith = [&](const char* body, const char* ca, const char* ov) {
            print("\t{{ uint32_t a = {}, b = {}; uint64_t t; uint32_t r; (void)a; (void)b; (void)t; {}", Gpr(ra), Gpr(rb), body);
            print(" {} = r;", Gpr(d));
            if (ca)
                print(" ctx.xer.ca = {};", ca);
            if (OE(w) && ov)
                print(" ppc_set_ov(ctx, {});", ov);
            if (Rc(w))
                print(" ppc_record(ctx, r);");
            print(" }}\n");
        };

        // Scalar FP: double result in ps0 only, or single result in both halves.
        auto fpDouble = [&](const std::string& expr) {
            print("\tctx.f[{}].ps0 = {};\n", d, expr);
            recordFp();
        };
        auto fpSingle = [&](const std::string& expr) {
            print("\t{{ double r = ppc_round_single({}); ctx.f[{}].ps0 = r; ctx.f[{}].ps1 = r; }}\n", expr, d, d);
            recordFp();
        };
        // Paired single: both lanes computed from operands captured first (D may alias A/B/C).
        auto ps = [&](const std::string& e0, const std::string& e1, bool round = true) {
            print("\t{{ double a0 = ctx.f[{0}].ps0, a1 = ctx.f[{0}].ps1, b0 = ctx.f[{1}].ps0, b1 = ctx.f[{1}].ps1, "
                  "c0 = ctx.f[{2}].ps0, c1 = ctx.f[{2}].ps1; (void)a0; (void)a1; (void)b0; (void)b1; (void)c0; (void)c1; ",
                ra, rb, rc);
            if (round)
                print("ctx.f[{0}].ps0 = ppc_round_single({1}); ctx.f[{0}].ps1 = ppc_round_single({2}); }}\n", d, e0, e1);
            else
                print("ctx.f[{0}].ps0 = {1}; ctx.f[{0}].ps1 = {2}; }}\n", d, e0, e1);
            recordFp();
        };
        const std::string fa = std::format("ctx.f[{}].ps0", ra), fb = std::format("ctx.f[{}].ps0", rb),
                          fc = std::format("ctx.f[{}].ps0", rc);

        // value is an expression in terms of `ea`, e.g. "PPC_LOAD_U32(ea)".
        auto load = [&](const char* value, const std::string& ea, bool update) {
            print("\tea = {};\n\t{} = {};\n", ea, Gpr(d), value);
            if (update)
                print("\t{} = ea;\n", Gpr(ra));
        };
        auto store = [&](const char* macro, const std::string& ea, const std::string& value, bool update) {
            print("\tea = {};\n\t{}(ea, {});\n", ea, macro, value);
            if (update)
                print("\t{} = ea;\n", Gpr(ra));
        };

        switch (op)
        {
        // ---- integer immediate ----
        case Op::ADDI: print("\t{} = {};\n", Gpr(d), EaD(ra, SIMM(w))); break;
        case Op::ADDIS: print("\t{} = {};\n", Gpr(d), EaD(ra, int32_t(UIMM(w) << 16))); break;
        case Op::ADDIC:
        case Op::ADDIC_RC:
            print("\t{{ uint32_t a = {}; {} = a + {}; ctx.xer.ca = {} < a; }}\n", Gpr(ra), Gpr(d), Hex(uint32_t(SIMM(w))), Gpr(d));
            if (op == Op::ADDIC_RC)
                print("\tppc_record(ctx, {});\n", Gpr(d));
            break;
        case Op::SUBFIC:
            print("\t{{ uint32_t a = {}; {} = {} - a; ctx.xer.ca = {} >= a; }}\n", Gpr(ra), Gpr(d), Hex(uint32_t(SIMM(w))), Hex(uint32_t(SIMM(w))));
            break;
        case Op::MULLI: print("\t{} = {} * {};\n", Gpr(d), Gpr(ra), Hex(uint32_t(SIMM(w)))); break;
        case Op::CMPI: print("\tppc_cmp_s32(ctx, {}, {}, {});\n", CRFD(w), GprS(ra), SIMM(w)); break;
        case Op::CMPLI: print("\tppc_cmp_u32(ctx, {}, {}, {});\n", CRFD(w), Gpr(ra), Hex(UIMM(w))); break;
        case Op::CMP: print("\tppc_cmp_s32(ctx, {}, {}, {});\n", CRFD(w), GprS(ra), GprS(rb)); break;
        case Op::CMPL: print("\tppc_cmp_u32(ctx, {}, {}, {});\n", CRFD(w), Gpr(ra), Gpr(rb)); break;
        case Op::ORI: print("\t{} = {} | {};\n", Gpr(ra), Gpr(d), Hex(UIMM(w))); break;
        case Op::ORIS: print("\t{} = {} | {};\n", Gpr(ra), Gpr(d), Hex(UIMM(w) << 16)); break;
        case Op::XORI: print("\t{} = {} ^ {};\n", Gpr(ra), Gpr(d), Hex(UIMM(w))); break;
        case Op::XORIS: print("\t{} = {} ^ {};\n", Gpr(ra), Gpr(d), Hex(UIMM(w) << 16)); break;
        case Op::ANDI_RC: print("\t{} = {} & {};\n\tppc_record(ctx, {});\n", Gpr(ra), Gpr(d), Hex(UIMM(w)), Gpr(ra)); break;
        case Op::ANDIS_RC: print("\t{} = {} & {};\n\tppc_record(ctx, {});\n", Gpr(ra), Gpr(d), Hex(UIMM(w) << 16), Gpr(ra)); break;

        // ---- integer XO-form ----
        case Op::ADD: arith("r = a + b;", nullptr, "((a ^ r) & (b ^ r)) >> 31"); break;
        case Op::ADDC: arith("r = a + b;", "r < a", "((a ^ r) & (b ^ r)) >> 31"); break;
        case Op::ADDE: arith("t = (uint64_t)a + b + ctx.xer.ca; r = (uint32_t)t;", "uint8_t(t >> 32)", "((a ^ r) & (b ^ r)) >> 31"); break;
        case Op::ADDZE: arith("t = (uint64_t)a + ctx.xer.ca; r = (uint32_t)t;", "uint8_t(t >> 32)", "((a ^ r) & r) >> 31"); break;
        case Op::ADDME: arith("t = (uint64_t)a + ctx.xer.ca + 0xFFFFFFFFu; r = (uint32_t)t;", "uint8_t(t >> 32)", "((a ^ r) & ~r) >> 31"); break;
        case Op::SUBF: arith("r = b - a;", nullptr, "((~a ^ r) & (b ^ r)) >> 31"); break;
        case Op::SUBFC: arith("r = b - a;", "b >= a", "((~a ^ r) & (b ^ r)) >> 31"); break;
        case Op::SUBFE: arith("t = (uint64_t)~a + b + ctx.xer.ca; r = (uint32_t)t;", "uint8_t(t >> 32)", "((~a ^ r) & (b ^ r)) >> 31"); break;
        case Op::SUBFZE: arith("t = (uint64_t)~a + ctx.xer.ca; r = (uint32_t)t;", "uint8_t(t >> 32)", "((~a ^ r) & r) >> 31"); break;
        case Op::SUBFME: arith("t = (uint64_t)~a + ctx.xer.ca + 0xFFFFFFFFu; r = (uint32_t)t;", "uint8_t(t >> 32)", "((~a ^ r) & ~r) >> 31"); break;
        case Op::NEG: arith("r = 0u - a;", nullptr, "a == 0x80000000u"); break;
        case Op::MULLW: arith("int64_t p = (int64_t)(int32_t)a * (int32_t)b; r = (uint32_t)p;", nullptr, "p != (int32_t)p"); break;
        case Op::MULHW: arith("r = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);", nullptr, nullptr); break;
        case Op::MULHWU: arith("r = (uint32_t)(((uint64_t)a * b) >> 32);", nullptr, nullptr); break;
        case Op::DIVW: arith("r = ppc_divw(a, b);", nullptr, "b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)"); break;
        case Op::DIVWU: arith("r = ppc_divwu(a, b);", nullptr, "b == 0"); break;

        // ---- logical, shifts, rotates (rA = rS op rB) ----
        case Op::AND: print("\t{} = {} & {};\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::ANDC: print("\t{} = {} & ~{};\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::OR: print("\t{} = {} | {};\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::ORC: print("\t{} = {} | ~{};\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::XOR: print("\t{} = {} ^ {};\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::NAND: print("\t{} = ~({} & {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::NOR: print("\t{} = ~({} | {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::EQV: print("\t{} = ~({} ^ {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::EXTSB: print("\t{} = (int32_t)ctx.r[{}].s8;\n", GprS(ra), d); record(Gpr(ra)); break;
        case Op::EXTSH: print("\t{} = (int32_t)ctx.r[{}].s16;\n", GprS(ra), d); record(Gpr(ra)); break;
        case Op::CNTLZW: print("\t{{ uint32_t s = {}; {} = s ? __builtin_clz(s) : 32; }}\n", Gpr(d), Gpr(ra)); record(Gpr(ra)); break;
        case Op::SLW: print("\t{} = ppc_slw({}, {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::SRW: print("\t{} = ppc_srw({}, {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::SRAW: print("\t{} = ppc_sraw(ctx, {}, {});\n", Gpr(ra), Gpr(d), Gpr(rb)); record(Gpr(ra)); break;
        case Op::SRAWI: print("\t{} = ppc_sraw(ctx, {}, {});\n", Gpr(ra), Gpr(d), rb); record(Gpr(ra)); break;
        case Op::RLWINM:
        {
            const uint32_t mask = RlwMask(rc, ME(w));
            const std::string rot = rb ? std::format("ppc_rotl({}, {})", Gpr(d), rb) : Gpr(d);
            print("\t{} = {} & {};\n", Gpr(ra), rot, Hex(mask));
            record(Gpr(ra));
            break;
        }
        case Op::RLWNM:
            print("\t{} = ppc_rotl({}, {} & 31) & {};\n", Gpr(ra), Gpr(d), Gpr(rb), Hex(RlwMask(rc, ME(w))));
            record(Gpr(ra));
            break;
        case Op::RLWIMI:
        {
            const uint32_t mask = RlwMask(rc, ME(w));
            print("\t{} = (ppc_rotl({}, {}) & {}) | ({} & {});\n", Gpr(ra), Gpr(d), rb, Hex(mask), Gpr(ra), Hex(~mask));
            record(Gpr(ra));
            break;
        }

        // ---- loads/stores ----
        case Op::LWZ: case Op::LWZU: load("PPC_LOAD_U32(ea)", EaD(ra, SIMM(w)), op == Op::LWZU); break;
        case Op::LBZ: case Op::LBZU: load("PPC_LOAD_U8(ea)", EaD(ra, SIMM(w)), op == Op::LBZU); break;
        case Op::LHZ: case Op::LHZU: load("PPC_LOAD_U16(ea)", EaD(ra, SIMM(w)), op == Op::LHZU); break;
        case Op::LHA: case Op::LHAU: load("(uint32_t)(int32_t)(int16_t)PPC_LOAD_U16(ea)", EaD(ra, SIMM(w)), op == Op::LHAU); break;
        case Op::LWZX: case Op::LWZUX: load("PPC_LOAD_U32(ea)", EaX(ra, rb), op == Op::LWZUX); break;
        case Op::LBZX: case Op::LBZUX: load("PPC_LOAD_U8(ea)", EaX(ra, rb), op == Op::LBZUX); break;
        case Op::LHZX: case Op::LHZUX: load("PPC_LOAD_U16(ea)", EaX(ra, rb), op == Op::LHZUX); break;
        case Op::LHAX: case Op::LHAUX: load("(uint32_t)(int32_t)(int16_t)PPC_LOAD_U16(ea)", EaX(ra, rb), op == Op::LHAUX); break;
        case Op::LWBRX: load("PPC_LOAD_U32_LE(ea)", EaX(ra, rb), false); break;
        case Op::LHBRX: load("PPC_LOAD_U16_LE(ea)", EaX(ra, rb), false); break;
        case Op::STW: case Op::STWU: store("PPC_STORE_U32", EaD(ra, SIMM(w)), Gpr(d), op == Op::STWU); break;
        case Op::STB: case Op::STBU: store("PPC_STORE_U8", EaD(ra, SIMM(w)), Gpr(d), op == Op::STBU); break;
        case Op::STH: case Op::STHU: store("PPC_STORE_U16", EaD(ra, SIMM(w)), Gpr(d), op == Op::STHU); break;
        case Op::STWX: case Op::STWUX: store("PPC_STORE_U32", EaX(ra, rb), Gpr(d), op == Op::STWUX); break;
        case Op::STBX: case Op::STBUX: store("PPC_STORE_U8", EaX(ra, rb), Gpr(d), op == Op::STBUX); break;
        case Op::STHX: case Op::STHUX: store("PPC_STORE_U16", EaX(ra, rb), Gpr(d), op == Op::STHUX); break;
        case Op::STWBRX: store("PPC_STORE_U32_LE", EaX(ra, rb), Gpr(d), false); break;
        case Op::STHBRX: store("PPC_STORE_U16_LE", EaX(ra, rb), Gpr(d), false); break;
        case Op::LMW:
            print("\tea = {};\n", EaD(ra, SIMM(w)));
            for (uint32_t r = d; r < 32; r++)
                print("\t{} = PPC_LOAD_U32(ea + {});\n", Gpr(r), (r - d) * 4);
            break;
        case Op::STMW:
            print("\tea = {};\n", EaD(ra, SIMM(w)));
            for (uint32_t r = d; r < 32; r++)
                print("\tPPC_STORE_U32(ea + {}, {});\n", (r - d) * 4, Gpr(r));
            break;
        case Op::LSWI: print("\tppc_lswi(ctx, base, {}, {}, {});\n", ra ? Gpr(ra) : std::string("0u"), d, rb); break;
        case Op::STSWI: print("\tppc_stswi(ctx, base, {}, {}, {});\n", ra ? Gpr(ra) : std::string("0u"), d, rb); break;
        case Op::LWARX:
            print("\tea = {};\n\tctx.reserved = true; ctx.reserveAddr = ea; ctx.reserveValue = PPC_LOAD_U32(ea); {} = ctx.reserveValue;\n",
                EaX(ra, rb), Gpr(d));
            break;
        case Op::STWCX_RC: print("\tppc_stwcx(ctx, base, {}, {});\n", EaX(ra, rb), Gpr(d)); break;
        case Op::DCBZ: case Op::DCBZ_L: print("\t__builtin_memset(base + (({}) & ~31u), 0, 32);\n", EaX(ra, rb)); break;
        case Op::DCBST: case Op::DCBF: case Op::DCBI: case Op::DCBT: case Op::DCBTST: case Op::ICBI:
        case Op::SYNC: case Op::ISYNC: case Op::EIEIO: case Op::TLBSYNC:
            break;  // cache and ordering hints: no-ops on the host

        // ---- floating-point loads/stores ----
        case Op::LFS: case Op::LFSU: case Op::LFSX: case Op::LFSUX:
        {
            const bool x = op == Op::LFSX || op == Op::LFSUX;
            print("\tea = {};\n", x ? EaX(ra, rb) : EaD(ra, SIMM(w)));
            print("\tctx.f[{0}].ps0 = ctx.f[{0}].ps1 = ppc_load_f32(base, ea);\n", d);
            if (op == Op::LFSU || op == Op::LFSUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        }
        case Op::LFD: case Op::LFDU: case Op::LFDX: case Op::LFDUX:
        {
            const bool x = op == Op::LFDX || op == Op::LFDUX;
            print("\tea = {};\n", x ? EaX(ra, rb) : EaD(ra, SIMM(w)));
            print("\tctx.f[{}].ps0 = ppc_load_f64(base, ea);\n", d);
            if (op == Op::LFDU || op == Op::LFDUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        }
        case Op::STFS: case Op::STFSU: case Op::STFSX: case Op::STFSUX:
        {
            const bool x = op == Op::STFSX || op == Op::STFSUX;
            print("\tea = {};\n\tppc_store_f32(base, ea, ctx.f[{}].ps0);\n", x ? EaX(ra, rb) : EaD(ra, SIMM(w)), d);
            if (op == Op::STFSU || op == Op::STFSUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        }
        case Op::STFD: case Op::STFDU: case Op::STFDX: case Op::STFDUX:
        {
            const bool x = op == Op::STFDX || op == Op::STFDUX;
            print("\tea = {};\n\tppc_store_f64(base, ea, ctx.f[{}].ps0);\n", x ? EaX(ra, rb) : EaD(ra, SIMM(w)), d);
            if (op == Op::STFDU || op == Op::STFDUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        }
        case Op::STFIWX: print("\tPPC_STORE_U32({}, (uint32_t)ppc_bits(ctx.f[{}].ps0));\n", EaX(ra, rb), d); break;

        // ---- paired-single loads/stores ----
        case Op::PSQ_L: case Op::PSQ_LU:
            print("\tea = {};\n\tppc_psq_load(ctx, base, {}, ea, {}, {});\n", EaD(ra, PSQ_D(w)), d, PSQ_W(w), PSQ_I(w));
            if (op == Op::PSQ_LU)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        case Op::PSQ_ST: case Op::PSQ_STU:
            print("\tea = {};\n\tppc_psq_store(ctx, base, {}, ea, {}, {});\n", EaD(ra, PSQ_D(w)), d, PSQ_W(w), PSQ_I(w));
            if (op == Op::PSQ_STU)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        case Op::PSQ_LX: case Op::PSQ_LUX:
            print("\tea = {};\n\tppc_psq_load(ctx, base, {}, ea, {}, {});\n", EaX(ra, rb), d, PSQX_W(w), PSQX_I(w));
            if (op == Op::PSQ_LUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;
        case Op::PSQ_STX: case Op::PSQ_STUX:
            print("\tea = {};\n\tppc_psq_store(ctx, base, {}, ea, {}, {});\n", EaX(ra, rb), d, PSQX_W(w), PSQX_I(w));
            if (op == Op::PSQ_STUX)
                print("\t{} = ea;\n", Gpr(ra));
            break;

        // ---- scalar floating point (frD, frA, frB, frC) ----
        case Op::FADD: fpDouble(std::format("{} + {}", fa, fb)); break;
        case Op::FSUB: fpDouble(std::format("{} - {}", fa, fb)); break;
        case Op::FMUL: fpDouble(std::format("{} * {}", fa, fc)); break;
        case Op::FDIV: fpDouble(std::format("{} / {}", fa, fb)); break;
        case Op::FMADD: fpDouble(std::format("{} * {} + {}", fa, fc, fb)); break;
        case Op::FMSUB: fpDouble(std::format("{} * {} - {}", fa, fc, fb)); break;
        case Op::FNMADD: fpDouble(std::format("-({} * {} + {})", fa, fc, fb)); break;
        case Op::FNMSUB: fpDouble(std::format("-({} * {} - {})", fa, fc, fb)); break;
        case Op::FSQRT: fpDouble(std::format("ppc_sqrt({})", fb)); break;
        case Op::FSEL: fpDouble(std::format("{} >= 0.0 ? {} : {}", fa, fc, fb)); break;
        case Op::FRSQRTE: fpDouble(std::format("1.0 / ppc_sqrt({})", fb)); break;
        case Op::FADDS: fpSingle(std::format("{} + {}", fa, fb)); break;
        case Op::FSUBS: fpSingle(std::format("{} - {}", fa, fb)); break;
        case Op::FMULS: fpSingle(std::format("{} * {}", fa, fc)); break;
        case Op::FDIVS: fpSingle(std::format("{} / {}", fa, fb)); break;
        case Op::FMADDS: fpSingle(std::format("{} * {} + {}", fa, fc, fb)); break;
        case Op::FMSUBS: fpSingle(std::format("{} * {} - {}", fa, fc, fb)); break;
        case Op::FNMADDS: fpSingle(std::format("-({} * {} + {})", fa, fc, fb)); break;
        case Op::FNMSUBS: fpSingle(std::format("-({} * {} - {})", fa, fc, fb)); break;
        case Op::FSQRTS: fpSingle(std::format("ppc_sqrt({})", fb)); break;
        case Op::FRES: fpSingle(std::format("1.0 / {}", fb)); break;
        case Op::FRSP: fpSingle(fb); break;
        case Op::FMR: fpDouble(fb); break;
        case Op::FNEG: fpDouble(std::format("ppc_fneg({})", fb)); break;
        case Op::FABS: fpDouble(std::format("ppc_fabs({})", fb)); break;
        case Op::FNABS: fpDouble(std::format("ppc_fnabs({})", fb)); break;
        case Op::FCTIW: fpDouble(std::format("ppc_fctiw({}, false)", fb)); break;
        case Op::FCTIWZ: fpDouble(std::format("ppc_fctiw({}, true)", fb)); break;
        case Op::FCMPU: case Op::FCMPO: print("\tppc_fcmp(ctx, {}, {}, {});\n", CRFD(w), fa, fb); break;
        case Op::MFFS: fpDouble("ppc_from_bits(0xFFF8000000000000ull | ctx.fpscr)"); break;
        case Op::MTFSF: print("\tppc_mtfsf(ctx, {}, (uint32_t)ppc_bits({}));\n", FM(w), fb); recordFp(); break;
        case Op::MTFSB0: print("\tppc_mtfsb(ctx, {}, false);\n", d); recordFp(); break;
        case Op::MTFSB1: print("\tppc_mtfsb(ctx, {}, true);\n", d); recordFp(); break;
        case Op::MTFSFI: print("\tppc_mtfsfi(ctx, {}, {});\n", CRFD(w), (w >> 12) & 0xF); recordFp(); break;

        // ---- paired single arithmetic ----
        case Op::PS_ADD: ps("a0 + b0", "a1 + b1"); break;
        case Op::PS_SUB: ps("a0 - b0", "a1 - b1"); break;
        case Op::PS_MUL: ps("a0 * c0", "a1 * c1"); break;
        case Op::PS_DIV: ps("a0 / b0", "a1 / b1"); break;
        case Op::PS_MADD: ps("a0 * c0 + b0", "a1 * c1 + b1"); break;
        case Op::PS_MSUB: ps("a0 * c0 - b0", "a1 * c1 - b1"); break;
        case Op::PS_NMADD: ps("-(a0 * c0 + b0)", "-(a1 * c1 + b1)"); break;
        case Op::PS_NMSUB: ps("-(a0 * c0 - b0)", "-(a1 * c1 - b1)"); break;
        case Op::PS_MULS0: ps("a0 * c0", "a1 * c0"); break;
        case Op::PS_MULS1: ps("a0 * c1", "a1 * c1"); break;
        case Op::PS_MADDS0: ps("a0 * c0 + b0", "a1 * c0 + b1"); break;
        case Op::PS_MADDS1: ps("a0 * c1 + b0", "a1 * c1 + b1"); break;
        case Op::PS_SUM0: ps("a0 + b1", "c1"); break;
        case Op::PS_SUM1: ps("c0", "a0 + b1"); break;
        case Op::PS_SEL: ps("a0 >= 0.0 ? c0 : b0", "a1 >= 0.0 ? c1 : b1", false); break;
        case Op::PS_RES: ps("1.0 / b0", "1.0 / b1"); break;
        case Op::PS_RSQRTE: ps("1.0 / ppc_sqrt(b0)", "1.0 / ppc_sqrt(b1)"); break;
        case Op::PS_MR: ps("b0", "b1", false); break;
        case Op::PS_NEG: ps("ppc_fneg(b0)", "ppc_fneg(b1)", false); break;
        case Op::PS_ABS: ps("ppc_fabs(b0)", "ppc_fabs(b1)", false); break;
        case Op::PS_NABS: ps("ppc_fnabs(b0)", "ppc_fnabs(b1)", false); break;
        case Op::PS_MERGE00: ps("a0", "b0", false); break;
        case Op::PS_MERGE01: ps("a0", "b1", false); break;
        case Op::PS_MERGE10: ps("a1", "b0", false); break;
        case Op::PS_MERGE11: ps("a1", "b1", false); break;
        case Op::PS_CMPU0: case Op::PS_CMPO0: print("\tppc_fcmp(ctx, {}, ctx.f[{}].ps0, ctx.f[{}].ps0);\n", CRFD(w), ra, rb); break;
        case Op::PS_CMPU1: case Op::PS_CMPO1: print("\tppc_fcmp(ctx, {}, ctx.f[{}].ps1, ctx.f[{}].ps1);\n", CRFD(w), ra, rb); break;

        // ---- condition register ----
        case Op::CRAND: print("\tctx.cr[{}] = ctx.cr[{}] & ctx.cr[{}];\n", d, ra, rb); break;
        case Op::CRANDC: print("\tctx.cr[{}] = ctx.cr[{}] & !ctx.cr[{}];\n", d, ra, rb); break;
        case Op::CREQV: print("\tctx.cr[{}] = !(ctx.cr[{}] ^ ctx.cr[{}]);\n", d, ra, rb); break;
        case Op::CRNAND: print("\tctx.cr[{}] = !(ctx.cr[{}] & ctx.cr[{}]);\n", d, ra, rb); break;
        case Op::CRNOR: print("\tctx.cr[{}] = !(ctx.cr[{}] | ctx.cr[{}]);\n", d, ra, rb); break;
        case Op::CROR: print("\tctx.cr[{}] = ctx.cr[{}] | ctx.cr[{}];\n", d, ra, rb); break;
        case Op::CRORC: print("\tctx.cr[{}] = ctx.cr[{}] | !ctx.cr[{}];\n", d, ra, rb); break;
        case Op::CRXOR: print("\tctx.cr[{}] = ctx.cr[{}] ^ ctx.cr[{}];\n", d, ra, rb); break;
        case Op::MCRF: print("\t__builtin_memcpy(ctx.cr + {}, ctx.cr + {}, 4);\n", CRFD(w) * 4, CRFS(w) * 4); break;
        case Op::MCRXR:
            print("\t{{ uint8_t* c = ctx.cr + {}; c[0] = ctx.xer.so; c[1] = ctx.xer.ov; c[2] = ctx.xer.ca; c[3] = 0; "
                  "ctx.xer.so = ctx.xer.ov = ctx.xer.ca = 0; }}\n", CRFD(w) * 4);
            break;
        case Op::MFCR: print("\t{} = ppc_mfcr(ctx);\n", Gpr(d)); break;
        case Op::MTCRF: print("\tppc_mtcrf(ctx, {}, {});\n", Hex(CRM(w)), Gpr(d)); break;

        // ---- special registers ----
        case Op::MFSPR:
        {
            const uint32_t spr = SPR(w);
            if (spr == 1) print("\t{} = ppc_get_xer(ctx);\n", Gpr(d));
            else if (spr == 8) print("\t{} = ctx.lr;\n", Gpr(d));
            else if (spr == 9) print("\t{} = ctx.ctr;\n", Gpr(d));
            else if (spr >= 896 && spr <= 903) print("\t{} = ctx.gqr[{}];\n", Gpr(d), spr - 896);
            else if (spr >= 912 && spr <= 919) print("\t{} = ctx.gqr[{}];\n", Gpr(d), spr - 912);
            else unsupported(a, op);
            break;
        }
        case Op::MTSPR:
        {
            const uint32_t spr = SPR(w);
            if (spr == 1) print("\tppc_set_xer(ctx, {});\n", Gpr(d));
            else if (spr == 8) print("\tctx.lr = {};\n", Gpr(d));
            else if (spr == 9) print("\tctx.ctr = {};\n", Gpr(d));
            else if (spr >= 896 && spr <= 903) print("\tctx.gqr[{}] = {};\n", spr - 896, Gpr(d));
            else if (spr >= 912 && spr <= 919) print("\tctx.gqr[{}] = {};\n", spr - 912, Gpr(d));
            else unsupported(a, op);
            break;
        }
        case Op::MFTB:
        {
            const uint32_t tbr = SPR(w);
            print("\t{} = (uint32_t)(ppc_timebase(){});\n", Gpr(d), tbr == 269 ? " >> 32" : "");
            break;
        }

        // ---- traps and system calls ----
        case Op::TW:
        case Op::TWI:
        {
            const uint32_t to = d;
            const std::string b = op == Op::TW ? GprS(rb) : std::format("{}", SIMM(w));
            const std::string bu = op == Op::TW ? Gpr(rb) : Hex(uint32_t(SIMM(w)));
            if (to == 31)
            {
                print("\tppc_trap(ctx, {});\n", Hex(a));
                break;
            }
            std::string cond;
            auto add = [&](const std::string& c) { cond += (cond.empty() ? "" : " || ") + c; };
            if (to & 16) add(std::format("{} < {}", GprS(ra), b));
            if (to & 8) add(std::format("{} > {}", GprS(ra), b));
            if (to & 4) add(std::format("{} == {}", GprS(ra), b));
            if (to & 2) add(std::format("{} < {}", Gpr(ra), bu));
            if (to & 1) add(std::format("{} > {}", Gpr(ra), bu));
            if (!cond.empty())
                print("\tif ({}) ppc_trap(ctx, {});\n", cond, Hex(a));
            break;
        }
        case Op::SC: print("\tppc_syscall(ctx, base, {});\n", Hex(a)); break;

        // ---- branches ----
        case Op::B:
        {
            const uint32_t target = AA(w) ? uint32_t(LI(w)) : a + LI(w);
            if (Rc(w))
            {
                print("\tctx.lr = {};\n\t{}\n", next, call(a, target));
            }
            else if (inFunction(target) && !image.importCalls.count(a))
                print("\tgoto loc_{:08X};\n", target);
            else
                print("\t{}\n\treturn;\n", call(a, target));
            break;
        }
        case Op::BC:
        {
            const uint32_t target = AA(w) ? uint32_t(BD(w)) : a + BD(w);
            const std::string cond = branchCondition(w);
            const std::string open = cond.empty() ? "\t{" : std::format("\tif ({}) {{", cond);
            if (Rc(w))
            {
                if (inFunction(target))  // "bcl 20,31,next" style: just sets LR
                    print("{} ctx.lr = {}; goto loc_{:08X}; }}\n", open, next, target);
                else
                    print("{} ctx.lr = {}; {} }}\n", open, next, call(a, target));
            }
            else if (inFunction(target))
                print("{} goto loc_{:08X}; }}\n", open, target);
            else
                print("{} {} return; }}\n", open, call(a, target));
            break;
        }
        case Op::BCLR:
        {
            const std::string cond = branchCondition(w);
            const std::string open = cond.empty() ? "\t{" : std::format("\tif ({}) {{", cond);
            if (Rc(w))
                print("{} uint32_t target = ctx.lr; ctx.lr = {}; PPC_CALL_INDIRECT(target); }}\n", open, next);
            else
                print("{} return; }}\n", open);
            break;
        }
        case Op::BCCTR:
        {
            const std::string cond = branchCondition(w);
            const std::string open = cond.empty() ? "\t{" : std::format("\tif ({}) {{", cond);
            if (Rc(w))
                print("{} ctx.lr = {}; PPC_CALL_INDIRECT(ctx.ctr); }}\n", open, next);
            else if (auto it = switchTables.find(a); it != switchTables.end())
            {
                stats.switchTables++;
                stats.switchEntries += it->second;
                print("{} switch ((ctx.ctr - {}) >> 2) {{\n", open, Hex(a + 4));
                for (uint32_t i = 0; i < it->second; i++)
                    print("\tcase {}: goto loc_{:08X};\n", i, a + 4 + i * 4);
                print("\tdefault: PPC_CALL_INDIRECT(ctx.ctr); return;\n\t}} }}\n");
            }
            else
                print("{} PPC_CALL_INDIRECT(ctx.ctr); return; }}\n", open);
            break;
        }

        // ---- privileged / unsupported ----
        case Op::MFMSR: case Op::MTMSR: case Op::MTSR: case Op::MTSRIN: case Op::MFSR: case Op::MFSRIN:
        case Op::TLBIE: case Op::RFI: case Op::ECIWX: case Op::ECOWX: case Op::LSWX: case Op::STSWX: case Op::MCRFS:
            unsupported(a, op);
            break;

        case Op::INVALID:
        default:
            stats.invalid++;
            print("\tppc_unsupported(ctx, {}, \"invalid instruction {:08X}\");\n", Hex(a), w);
            break;
        }
    }

    // A function whose last instruction can fall through continues into the next function.
    if (!IsUnconditionalEnd(lastOp, lastWord))
    {
        stats.fallthroughTailCalls++;
        if (functionStarts.count(end))
            print("\t{}(ctx, base);\n", FunctionName(end));
        else
            print("\tppc_bad_branch(ctx, {}, {});\n", Hex(end - 4), Hex(end));
    }
    print("}}\n\n");
    if (isHooked)
    {
        // The hook slot: the runtime may install a host function that replaces this one (and can call the original).
        const std::string name = FunctionName(start);
        print("PPCFunc* ppc_hook_{0} = nullptr;\n", name);
        print("PPC_FUNC({0}) {{\n\tif (PPCFunc* hook = ppc_hook_{0})\n\t\thook(ctx, base);\n\telse\n\t\torig_{0}(ctx, base);\n}}\n\n", name);
    }
}
