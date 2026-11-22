// Guest CPU state and helpers used by the recompiled code.
// Register model follows the Espresso (PowerPC 750 family): 32-bit GPRs, paired-single FPRs, GQRs.
// Loosely based on XenonRecomp's ppc_context.h (MIT, hedge-dev and contributors).
//
// Three builds use it:
// - the runtime with the generated code linked in (the default, and Linux);
// - DT_MODULE: the game module, the generated code built on the player's machine as a DLL (Windows releases): no C or
//   C++ library at all (only clang's own <stdint.h>/<stddef.h>), and the runtime's functions and data through
//   DtModuleHost, which the runtime hands over when it loads the module (dt_module_init);
// - DT_HOST_MODULE: the runtime that loads such a module (game_module.cpp): the generated tables are pointers it gets
//   from the module.
#pragma once

#if defined(DT_MODULE)
#include <stddef.h>
#include <stdint.h>
#else
#include <atomic>
#include <cstdint>
#include <cstring>
#endif

union PPCReg
{
    uint32_t u32;
    int32_t s32;
    uint16_t u16;
    int16_t s16;
    uint8_t u8;
    int8_t s8;
};

// Each FPR holds two values. Scalar double ops use ps0; single-precision ops and paired-single ops
// write both halves (as Espresso does with HID2[PSE] set).
struct PPCFPR
{
    double ps0;
    double ps1;
};

struct PPCXER
{
    uint8_t so, ov, ca;
};

struct alignas(64) PPCContext
{
    PPCReg r[32];
    PPCFPR f[32];
    uint8_t cr[32];  // one byte per CR bit, IBM numbering: cr[4*n + {0 lt, 1 gt, 2 eq, 3 so}]
    uint32_t lr;
    uint32_t ctr;
    PPCXER xer;
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t reserveAddr;
    uint32_t reserveValue;
    bool reserved;
};

using PPCFunc = void(PPCContext& __restrict ctx, uint8_t* __restrict base);

#define PPC_FUNC(name) void name(PPCContext& __restrict ctx, uint8_t* __restrict base)

struct PPCFuncMapping
{
    uint32_t addr;
    PPCFunc* func;
};

struct PPCImportMapping
{
    uint32_t addr;
    const char* lib;
    const char* name;
    PPCFunc* func;
};

struct PPCDataImport
{
    uint32_t addr;
    const char* lib;
    const char* name;
};

// Guest functions listed in src/recomp/hooks.txt: the runtime may set *hook to replace one; original is its translation.
struct PPCHookSlot
{
    uint32_t addr;
    PPCFunc** hook;
    PPCFunc* original;
};

#if defined(DT_HOST_MODULE)
extern const PPCFuncMapping* PPCFuncMappings;  // (from the loaded module; indexed and walked as the arrays are)
extern const PPCHookSlot* PPCHookSlots;
extern const PPCImportMapping* PPCImportMappings;
extern const PPCDataImport* PPCDataImports;
#else
extern const PPCFuncMapping PPCFuncMappings[];
extern const PPCHookSlot PPCHookSlots[];
extern const PPCImportMapping PPCImportMappings[];
extern const PPCDataImport PPCDataImports[];
#endif

// ---- the game module's interface (Windows releases) ----
// The runtime calls the module's exported dt_module_init with this table and gets the generated tables back. A
// different DT_MODULE_VERSION on either side refuses the module: the launcher then rebuilds it.
constexpr uint32_t DT_MODULE_VERSION = 2;
struct PPCTraceRing;
struct DtModuleHost
{
    uint32_t version;
    PPCFunc* (*lookup)(uint32_t addr);
    void (*import)(PPCContext& ctx, uint8_t* base, uint32_t index);
    void (*trap)(PPCContext& ctx, uint32_t addr);
    void (*unsupported)(PPCContext& ctx, uint32_t addr, const char* what);
    void (*bad_branch)(PPCContext& ctx, uint32_t from, uint32_t to);
    void (*syscall)(PPCContext& ctx, uint8_t* base, uint32_t addr);
    uint64_t (*timebase)();
    void (*fpscr_changed)(PPCContext& ctx);
    void (*trace_log)(uint32_t addr);
    uint64_t* lookup_cache;              // ppc_lookup_cache (std::atomic<uint64_t> has a plain uint64_t's layout)
    PPCFunc* const* const* func_list;    // &ppc_func_list
    const bool* trace_all;               // &ppc_trace_all
    uint32_t tls_index, ring_offset;     // the runtime's thread-local ppc_trace_ring: TLS slot and offset in it
};
struct DtModuleExports
{
    uint32_t version;
    const PPCFuncMapping* funcs;
    const PPCHookSlot* hooks;
    const PPCImportMapping* imports;
    const PPCDataImport* data;
    const char* abi;  // DT_MODULE_ABI (module_abi.h) of the build it was made for
};

// ---- runtime entry points ----
#if defined(DT_MODULE)
extern "C" DtModuleHost dt_host;  // (dt_module.cpp: a copy of the runtime's table)
inline PPCFunc* ppc_lookup(uint32_t addr) { return dt_host.lookup(addr); }
inline void ppc_import(PPCContext& ctx, uint8_t* base, uint32_t index) { dt_host.import(ctx, base, index); }
[[noreturn]] inline void ppc_trap(PPCContext& ctx, uint32_t addr) { dt_host.trap(ctx, addr); __builtin_unreachable(); }
[[noreturn]] inline void ppc_unsupported(PPCContext& ctx, uint32_t addr, const char* what)
{
    dt_host.unsupported(ctx, addr, what);
    __builtin_unreachable();
}
[[noreturn]] inline void ppc_bad_branch(PPCContext& ctx, uint32_t from, uint32_t to)
{
    dt_host.bad_branch(ctx, from, to);
    __builtin_unreachable();
}
inline void ppc_syscall(PPCContext& ctx, uint8_t* base, uint32_t addr) { dt_host.syscall(ctx, base, addr); }
inline uint64_t ppc_timebase() { return dt_host.timebase(); }
inline void ppc_fpscr_changed(PPCContext& ctx) { dt_host.fpscr_changed(ctx); }
#else
PPCFunc* ppc_lookup(uint32_t addr);
void ppc_import(PPCContext& ctx, uint8_t* base, uint32_t index);  // index into PPCImportMappings
[[noreturn]] void ppc_trap(PPCContext& ctx, uint32_t addr);
[[noreturn]] void ppc_unsupported(PPCContext& ctx, uint32_t addr, const char* what);
[[noreturn]] void ppc_bad_branch(PPCContext& ctx, uint32_t from, uint32_t to);
void ppc_syscall(PPCContext& ctx, uint8_t* base, uint32_t addr);
uint64_t ppc_timebase();
void ppc_fpscr_changed(PPCContext& ctx);
#endif

// Indirect calls (virtual calls, and every import call through lis/addi; mtctr; bctrl) look in a global cache inline
// first: each entry packs the address (low 32 bits) and the function's number in ppc_func_list (high 32 bits) in one
// word, so any thread reads it whole without a lock or thread-local storage (the out-of-line lookup with a per-thread
// cache was still 5% of the rendering thread). ppc_lookup fills it.
#if defined(_MSC_VER) && !defined(__clang__)
#define PPC_FORCEINLINE __forceinline
#define PPC_NOINLINE __declspec(noinline)
#else
#define PPC_FORCEINLINE inline __attribute__((always_inline))
#define PPC_NOINLINE __attribute__((noinline))
#endif
constexpr uint32_t PPCLookupCacheSize = 16384;
#if defined(DT_MODULE)
#define PPC_LOOKUP_CACHE_ENTRY(i) __atomic_load_n(dt_host.lookup_cache + (i), __ATOMIC_ACQUIRE)
#define ppc_func_list (*dt_host.func_list)
#else
extern std::atomic<uint64_t> ppc_lookup_cache[PPCLookupCacheSize];
extern PPCFunc* const* ppc_func_list;
#define PPC_LOOKUP_CACHE_ENTRY(i) ppc_lookup_cache[i].load(std::memory_order_acquire)
#endif
PPC_FORCEINLINE PPCFunc* ppc_lookup_fast(uint32_t addr)
{
    const uint64_t e = PPC_LOOKUP_CACHE_ENTRY((addr >> 2) & (PPCLookupCacheSize - 1));
    if (uint32_t(e) == addr && addr)
        return ppc_func_list[e >> 32];
    return ppc_lookup(addr);
}
#define PPC_CALL_INDIRECT(addr) ppc_lookup_fast(addr)(ctx, base)

// ---- function-entry tracing ----
// Every recompiled function records its address in a per-thread ring buffer (always on, used for crash and
// hang reports). With --trace the runtime also logs every entry.
struct PPCTraceRing
{
    uint32_t pos;
    uint32_t addr[256];
};
#if defined(_MSC_VER)
#define PPC_TLS __declspec(thread)
#else
#define PPC_TLS __thread
#endif
#if defined(DT_MODULE)
// The runtime's thread-local ring, reached as Windows' implicit TLS does: the thread's TLS array (TEB +0x58), the
// runtime's slot in it, the variable's offset in the slot.
PPC_FORCEINLINE PPCTraceRing& ppc_trace_ring_here()
{
    char** tls;
    __asm__("movq %%gs:0x58, %0" : "=r"(tls));
    return *reinterpret_cast<PPCTraceRing*>(tls[dt_host.tls_index] + dt_host.ring_offset);
}
#define ppc_trace_ring (ppc_trace_ring_here())
#define ppc_trace_all (*dt_host.trace_all)
inline void ppc_trace_log(uint32_t addr) { dt_host.trace_log(addr); }
#else
extern PPC_TLS PPCTraceRing ppc_trace_ring;
extern bool ppc_trace_all;
void ppc_trace_log(uint32_t addr);
#endif

#ifdef PPC_NO_TRACE_RING
// Built without function-entry tracing (DT_TRACE_RING=OFF): no ring for crash/hang reports, no --trace.
#define PPC_FUNC_PROLOGUE(a) ((void)(a))
#else
#define PPC_FUNC_PROLOGUE(a)                                       \
    do                                                             \
    {                                                              \
        ppc_trace_ring.addr[ppc_trace_ring.pos++ & 255] = (a);     \
        if (__builtin_expect(ppc_trace_all, 0))                    \
            ppc_trace_log(a);                                      \
    } while (0)
#endif

// ---- memory ----
#define PPC_MEMORY_SIZE 0x100000000ull  // the full 32-bit guest address space
#define PPC_LOAD_U8(ea) (*(volatile uint8_t*)(base + (uint32_t)(ea)))
#define PPC_LOAD_U16(ea) __builtin_bswap16(*(volatile uint16_t*)(base + (uint32_t)(ea)))
#define PPC_LOAD_U32(ea) __builtin_bswap32(*(volatile uint32_t*)(base + (uint32_t)(ea)))
#define PPC_LOAD_U64(ea) __builtin_bswap64(*(volatile uint64_t*)(base + (uint32_t)(ea)))
#define PPC_LOAD_U16_LE(ea) (*(volatile uint16_t*)(base + (uint32_t)(ea)))
#define PPC_LOAD_U32_LE(ea) (*(volatile uint32_t*)(base + (uint32_t)(ea)))
#define PPC_STORE_U8(ea, v) (*(volatile uint8_t*)(base + (uint32_t)(ea)) = (uint8_t)(v))
#define PPC_STORE_U16(ea, v) (*(volatile uint16_t*)(base + (uint32_t)(ea)) = __builtin_bswap16((uint16_t)(v)))
#define PPC_STORE_U32(ea, v) (*(volatile uint32_t*)(base + (uint32_t)(ea)) = __builtin_bswap32((uint32_t)(v)))
#define PPC_STORE_U64(ea, v) (*(volatile uint64_t*)(base + (uint32_t)(ea)) = __builtin_bswap64((uint64_t)(v)))
#define PPC_STORE_U16_LE(ea, v) (*(volatile uint16_t*)(base + (uint32_t)(ea)) = (uint16_t)(v))
#define PPC_STORE_U32_LE(ea, v) (*(volatile uint32_t*)(base + (uint32_t)(ea)) = (uint32_t)(v))

// ---- integer helpers ----
inline uint32_t ppc_rotl(uint32_t x, uint32_t n) { return (x << (n & 31)) | (x >> ((32 - n) & 31)); }

inline void ppc_cmp_s32(PPCContext& ctx, int field, int32_t a, int32_t b)
{
    uint8_t* c = ctx.cr + field * 4;
    c[0] = a < b;
    c[1] = a > b;
    c[2] = a == b;
    c[3] = ctx.xer.so;
}

inline void ppc_cmp_u32(PPCContext& ctx, int field, uint32_t a, uint32_t b)
{
    uint8_t* c = ctx.cr + field * 4;
    c[0] = a < b;
    c[1] = a > b;
    c[2] = a == b;
    c[3] = ctx.xer.so;
}

inline void ppc_record(PPCContext& ctx, uint32_t v) { ppc_cmp_s32(ctx, 0, int32_t(v), 0); }

inline void ppc_set_ov(PPCContext& ctx, uint32_t ov)
{
    ctx.xer.ov = uint8_t(ov);
    ctx.xer.so |= uint8_t(ov);
}

inline uint32_t ppc_divw(uint32_t a, uint32_t b)
{
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu))
        return int32_t(a) < 0 ? 0xFFFFFFFFu : 0;
    return uint32_t(int32_t(a) / int32_t(b));
}

inline uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

inline uint32_t ppc_slw(uint32_t a, uint32_t n) { return (n & 0x20) ? 0 : a << (n & 31); }
inline uint32_t ppc_srw(uint32_t a, uint32_t n) { return (n & 0x20) ? 0 : a >> (n & 31); }

inline uint32_t ppc_sraw(PPCContext& ctx, uint32_t a, uint32_t n)
{
    const int32_t s = int32_t(a);
    if (n & 0x20)
    {
        ctx.xer.ca = s < 0;
        return s < 0 ? 0xFFFFFFFFu : 0;
    }
    n &= 31;
    ctx.xer.ca = s < 0 && n && (a & ((1u << n) - 1));
    return uint32_t(s >> n);
}

// CR <-> the 32 flag bytes, 8 at a time (GHS prologues and epilogues save and restore CR in many functions: the
// bit-by-bit loops were 3% of the rendering thread). Host byte order is little-endian (x86-64, ARM64).
inline uint32_t ppc_mfcr(const PPCContext& ctx)
{
    uint32_t v = 0;
    for (int g = 0; g < 4; g++)
    {
        uint64_t x;
        __builtin_memcpy(&x, ctx.cr + g * 8, 8);
        x &= 0x0101010101010101ull;
        // byte i's low bit lands at bit 63 - i: cr[8g] becomes the group's most significant bit
        v |= uint32_t((x * 0x8040201008040201ull) >> 56) << (24 - 8 * g);
    }
    return v;
}

struct PPCCrSpread
{
    uint64_t bytes[256];  // a CR byte (bit 7 first) as 8 flag bytes
    constexpr PPCCrSpread() : bytes()
    {
        for (int v = 0; v < 256; v++)
            for (int k = 0; k < 8; k++)
                bytes[v] |= uint64_t((v >> (7 - k)) & 1) << (8 * k);
    }
};
inline constexpr PPCCrSpread ppc_cr_spread{};

inline void ppc_mtcrf(PPCContext& ctx, uint32_t crm, uint32_t v)
{
    for (int g = 0; g < 4; g++)
    {
        // fields 2g and 2g+1 (crm bits 0x80 >> field) are the group's low and high four bytes
        const uint64_t mask = ((crm & (0x80u >> (2 * g))) ? 0x00000000FFFFFFFFull : 0) |
                              ((crm & (0x40u >> (2 * g))) ? 0xFFFFFFFF00000000ull : 0);
        if (!mask)
            continue;
        uint64_t x;
        __builtin_memcpy(&x, ctx.cr + g * 8, 8);
        x = (x & ~mask) | (ppc_cr_spread.bytes[(v >> (24 - 8 * g)) & 0xFF] & mask);
        __builtin_memcpy(ctx.cr + g * 8, &x, 8);
    }
}

inline uint32_t ppc_get_xer(const PPCContext& ctx)
{
    return uint32_t(ctx.xer.so) << 31 | uint32_t(ctx.xer.ov) << 30 | uint32_t(ctx.xer.ca) << 29;
}

inline void ppc_set_xer(PPCContext& ctx, uint32_t v)
{
    ctx.xer.so = (v >> 31) & 1;
    ctx.xer.ov = (v >> 30) & 1;
    ctx.xer.ca = (v >> 29) & 1;
}

inline bool ppc_stwcx(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t value)
{
    bool ok = false;
    if (ctx.reserved && ctx.reserveAddr == ea)
    {
        uint32_t expected = __builtin_bswap32(ctx.reserveValue);
        ok = __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(base + ea), &expected, __builtin_bswap32(value),
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    ctx.reserved = false;
    ctx.cr[0] = 0;
    ctx.cr[1] = 0;
    ctx.cr[2] = ok;
    ctx.cr[3] = ctx.xer.so;
    return ok;
}

inline void ppc_lswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rd, uint32_t nb)
{
    if (nb == 0)
        nb = 32;
    uint32_t r = rd, shift = 24;
    ctx.r[r].u32 = 0;
    for (uint32_t i = 0; i < nb; i++)
    {
        ctx.r[r].u32 |= uint32_t(PPC_LOAD_U8(ea + i)) << shift;
        if (shift == 0 && i + 1 < nb)
        {
            r = (r + 1) & 31;
            ctx.r[r].u32 = 0;
            shift = 24;
        }
        else
            shift -= 8;
    }
}

inline void ppc_stswi(PPCContext& ctx, uint8_t* base, uint32_t ea, uint32_t rs, uint32_t nb)
{
    if (nb == 0)
        nb = 32;
    uint32_t r = rs, shift = 24;
    for (uint32_t i = 0; i < nb; i++)
    {
        PPC_STORE_U8(ea + i, ctx.r[r].u32 >> shift);
        if (shift == 0)
        {
            r = (r + 1) & 31;
            shift = 24;
        }
        else
            shift -= 8;
    }
}

// ---- floating point helpers ----
// (clang's builtins throughout, no <cmath>: the game module has no C library. With SSE4.1 and -fno-math-errno, sqrt,
// trunc and nearbyint compile to single instructions.)
inline uint64_t ppc_bits(double d) { return __builtin_bit_cast(uint64_t, d); }
inline double ppc_from_bits(uint64_t v) { return __builtin_bit_cast(double, v); }
inline double ppc_sqrt(double d) { return __builtin_sqrt(d); }
// x * 2^n for the quantizer's scales (-32..31): exact, as ldexp is for these values.
inline double ppc_ldexp(double x, int n) { return x * ppc_from_bits(uint64_t(1023 + n) << 52); }
inline double ppc_round_single(double d) { return double(float(d)); }
inline double ppc_fneg(double d) { return ppc_from_bits(ppc_bits(d) ^ 0x8000000000000000ull); }
inline double ppc_fabs(double d) { return ppc_from_bits(ppc_bits(d) & ~0x8000000000000000ull); }
inline double ppc_fnabs(double d) { return ppc_from_bits(ppc_bits(d) | 0x8000000000000000ull); }

inline double ppc_load_f32(uint8_t* base, uint32_t ea) { return double(__builtin_bit_cast(float, uint32_t(PPC_LOAD_U32(ea)))); }
inline double ppc_load_f64(uint8_t* base, uint32_t ea) { return ppc_from_bits(PPC_LOAD_U64(ea)); }
inline void ppc_store_f32(uint8_t* base, uint32_t ea, double v) { PPC_STORE_U32(ea, __builtin_bit_cast(uint32_t, float(v))); }
inline void ppc_store_f64(uint8_t* base, uint32_t ea, double v) { PPC_STORE_U64(ea, ppc_bits(v)); }

inline void ppc_fcmp(PPCContext& ctx, int field, double a, double b)
{
    uint8_t* c = ctx.cr + field * 4;
    const bool un = __builtin_isnan(a) || __builtin_isnan(b);
    c[0] = !un && a < b;
    c[1] = !un && a > b;
    c[2] = !un && a == b;
    c[3] = un;
}

// fctiw/fctiwz: result in the low word of the FPR, upper word 0xFFF80000 as on hardware.
inline double ppc_fctiw(double v, bool truncate)
{
    int32_t r;
    if (__builtin_isnan(v))
        r = INT32_MIN;
    else
    {
        const double t = truncate ? __builtin_trunc(v) : __builtin_nearbyint(v);
        r = t >= 2147483647.0 ? INT32_MAX : t <= -2147483648.0 ? INT32_MIN : int32_t(t);
    }
    return ppc_from_bits(0xFFF8000000000000ull | uint32_t(r));
}

inline void ppc_record_fp(PPCContext& ctx)
{
    for (int i = 0; i < 4; i++)
        ctx.cr[4 + i] = (ctx.fpscr >> (31 - i)) & 1;
}

inline void ppc_mtfsf(PPCContext& ctx, uint32_t fm, uint32_t v)
{
    uint32_t mask = 0;
    for (int field = 0; field < 8; field++)
        if (fm & (0x80u >> field))
            mask |= 0xF0000000u >> (field * 4);
    ctx.fpscr = (ctx.fpscr & ~mask) | (v & mask);
    ppc_fpscr_changed(ctx);
}

inline void ppc_mtfsb(PPCContext& ctx, uint32_t bit, bool set)
{
    const uint32_t m = 0x80000000u >> bit;
    ctx.fpscr = set ? (ctx.fpscr | m) : (ctx.fpscr & ~m);
    ppc_fpscr_changed(ctx);
}

inline void ppc_mtfsfi(PPCContext& ctx, uint32_t field, uint32_t imm)
{
    const uint32_t shift = 28 - field * 4;
    ctx.fpscr = (ctx.fpscr & ~(0xFu << shift)) | ((imm & 0xF) << shift);
    ppc_fpscr_changed(ctx);
}

// ---- paired-single quantised loads/stores (GQR-driven) ----
// GQR layout: LD_SCALE bits 24-29, LD_TYPE bits 16-18, ST_SCALE bits 8-13, ST_TYPE bits 0-2.
// Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16. Scale is a signed 6-bit power of two.
inline int ppc_gqr_scale(uint32_t v) { return int(v << 26) >> 26; }

inline uint32_t ppc_quant_size(uint32_t type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

inline double ppc_dequantize(uint8_t* base, uint32_t ea, uint32_t type, int scale)
{
    switch (type)
    {
    case 4: return ppc_ldexp(double(PPC_LOAD_U8(ea)), -scale);
    case 5: return ppc_ldexp(double(PPC_LOAD_U16(ea)), -scale);
    case 6: return ppc_ldexp(double(int8_t(PPC_LOAD_U8(ea))), -scale);
    case 7: return ppc_ldexp(double(int16_t(PPC_LOAD_U16(ea))), -scale);
    default: return ppc_load_f32(base, ea);
    }
}

inline void ppc_quantize(uint8_t* base, uint32_t ea, double v, uint32_t type, int scale)
{
    auto clamp = [](double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; };
    switch (type)
    {
    case 4: PPC_STORE_U8(ea, uint8_t(clamp(ppc_ldexp(v, scale), 0, 255))); break;
    case 5: PPC_STORE_U16(ea, uint16_t(clamp(ppc_ldexp(v, scale), 0, 65535))); break;
    case 6: PPC_STORE_U8(ea, uint8_t(int8_t(clamp(ppc_ldexp(v, scale), -128, 127)))); break;
    case 7: PPC_STORE_U16(ea, uint16_t(int16_t(clamp(ppc_ldexp(v, scale), -32768, 32767)))); break;
    default: ppc_store_f32(base, ea, v); break;
    }
}

PPC_NOINLINE inline void ppc_psq_load_quantized(PPCContext& ctx, uint8_t* base, uint32_t fd, uint32_t ea, uint32_t w,
    uint32_t type, uint32_t gqr);
PPC_NOINLINE inline void ppc_psq_store_quantized(PPCContext& ctx, uint8_t* base, uint32_t fs, uint32_t ea, uint32_t w,
    uint32_t type, uint32_t gqr);

// Paired-single quantized loads and stores: plain floats (the common case) inline, the quantizer out of line.
PPC_FORCEINLINE void ppc_psq_load(PPCContext& ctx, uint8_t* base, uint32_t fd, uint32_t ea, uint32_t w, uint32_t i)
{
    const uint32_t gqr = ctx.gqr[i];
    const uint32_t type = (gqr >> 16) & 7;
    if (type == 0)  // plain floats, the common case: no scale
    {
        ctx.f[fd].ps0 = ppc_load_f32(base, ea);
        ctx.f[fd].ps1 = w ? 1.0 : double(ppc_load_f32(base, ea + 4));
        return;
    }
    ppc_psq_load_quantized(ctx, base, fd, ea, w, type, gqr);
}

PPC_NOINLINE inline void ppc_psq_load_quantized(PPCContext& ctx, uint8_t* base, uint32_t fd, uint32_t ea, uint32_t w,
    uint32_t type, uint32_t gqr)
{
    const int scale = ppc_gqr_scale(gqr >> 24);
    const double v0 = ppc_dequantize(base, ea, type, scale);
    const double v1 = w ? 1.0 : ppc_dequantize(base, ea + ppc_quant_size(type), type, scale);
    ctx.f[fd].ps0 = v0;
    ctx.f[fd].ps1 = v1;
}

PPC_FORCEINLINE void ppc_psq_store(PPCContext& ctx, uint8_t* base, uint32_t fs, uint32_t ea, uint32_t w, uint32_t i)
{
    const uint32_t gqr = ctx.gqr[i];
    const uint32_t type = gqr & 7;
    if (type == 0)  // plain floats, the common case
    {
        ppc_store_f32(base, ea, ctx.f[fs].ps0);
        if (!w)
            ppc_store_f32(base, ea + 4, ctx.f[fs].ps1);
        return;
    }
    ppc_psq_store_quantized(ctx, base, fs, ea, w, type, gqr);
}

PPC_NOINLINE inline void ppc_psq_store_quantized(PPCContext& ctx, uint8_t* base, uint32_t fs, uint32_t ea, uint32_t w,
    uint32_t type, uint32_t gqr)
{
    const int scale = ppc_gqr_scale(gqr >> 8);
    ppc_quantize(base, ea, ctx.f[fs].ps0, type, scale);
    if (!w)
        ppc_quantize(base, ea + ppc_quant_size(type), ctx.f[fs].ps1, type, scale);
}
