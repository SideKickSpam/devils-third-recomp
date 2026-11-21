// Host implementations of Cafe OS imports, and the dispatcher the generated stubs call.
//
// Implement an import as an ordinary C++ function and register it:
//     uint32_t OSGetCurrentThread() { ... }
//     CAFE_IMPORT(coreinit, "OSGetCurrentThread", OSGetCurrentThread);
// Arguments are read from the guest registers following the PowerPC EABI (r3-r10, f1-f8, 64-bit values in
// odd/even register pairs) and the result is written back to r3 / r3:r4 / f1. Calls are logged with their
// arguments and result. Use CAFE_IMPORT_RAW for functions that need the raw context (e.g. varargs).
#pragma once

#include "guest.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>

namespace cafe
{
// A guest pointer to a NUL-terminated string; logged as text.
struct GuestString
{
    uint32_t addr;
    std::string str() const { return guest::ReadString(addr); }
};

// PowerPC EABI argument passing: integers in r3-r10, floating point in f1-f8, 64-bit integers in an odd/even
// register pair. Arguments that do not fit go to the caller's parameter save area, starting at 8(r1), with
// 64-bit and double values 8-byte aligned.
struct ArgCursor
{
    int gpr = 3;
    int fpr = 1;
    uint32_t stack = 8;  // offset from r1 of the next stack argument
};

inline uint32_t StackWord(PPCContext& ctx, ArgCursor& c)
{
    const uint32_t v = guest::Read32(ctx.r[1].u32 + c.stack);
    c.stack += 4;
    return v;
}

inline uint64_t StackDword(PPCContext& ctx, ArgCursor& c)
{
    c.stack = guest::AlignUp(c.stack, 8);
    const uint64_t v = guest::Read64(ctx.r[1].u32 + c.stack);
    c.stack += 8;
    return v;
}

template <class T>
T ReadArg(PPCContext& ctx, ArgCursor& c)
{
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
    {
        if (c.fpr <= 8)
            return T(ctx.f[c.fpr++].ps0);
        return T(ppc_from_bits(StackDword(ctx, c)));
    }
    else if constexpr (sizeof(T) == 8)
    {
        if (!(c.gpr & 1))
            c.gpr++;  // 64-bit values start at an odd register
        if (c.gpr <= 9)
        {
            const uint64_t v = uint64_t(ctx.r[c.gpr].u32) << 32 | ctx.r[c.gpr + 1].u32;
            c.gpr += 2;
            return T(v);
        }
        c.gpr = 11;
        return T(StackDword(ctx, c));
    }
    else
    {
        const uint32_t v = c.gpr <= 10 ? ctx.r[c.gpr++].u32 : StackWord(ctx, c);
        if constexpr (std::is_same_v<T, GuestString>)
            return GuestString{ v };
        else
            return T(v);
    }
}

template <class T>
std::string FormatValue(const T& v)
{
    char buf[96];
    if constexpr (std::is_same_v<T, GuestString>)
    {
        if (!v.addr)
            return "NULL";
        std::string s = v.str();
        if (s.size() > 120)
            s = s.substr(0, 120) + "...";
        snprintf(buf, sizeof(buf), "0x%08X ", v.addr);
        return buf + ("\"" + s + "\"");
    }
    else if constexpr (std::is_same_v<T, bool>)
        return v ? "true" : "false";
    else if constexpr (std::is_floating_point_v<T>)
        snprintf(buf, sizeof(buf), "%g", double(v));
    else if constexpr (sizeof(T) == 8)
        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    else if constexpr (std::is_signed_v<T>)
        snprintf(buf, sizeof(buf), "%d", int(v));
    else
        snprintf(buf, sizeof(buf), "0x%X", unsigned(v));
    return buf;
}

template <class T>
void WriteResult(PPCContext& ctx, T v)
{
    if constexpr (std::is_floating_point_v<T>)
        ctx.f[1].ps0 = ctx.f[1].ps1 = double(v);
    else if constexpr (sizeof(T) == 8)
    {
        ctx.r[3].u32 = uint32_t(uint64_t(v) >> 32);
        ctx.r[4].u32 = uint32_t(v);
    }
    else if constexpr (std::is_signed_v<T>)
        ctx.r[3].s32 = int32_t(v);
    else
        ctx.r[3].u32 = uint32_t(v);
}

template <class T>
T ReadResult(PPCContext& ctx)
{
    if constexpr (std::is_floating_point_v<T>)
        return T(ctx.f[1].ps0);
    else if constexpr (sizeof(T) == 8)
        return T(uint64_t(ctx.r[3].u32) << 32 | ctx.r[4].u32);
    else
        return T(ctx.r[3].u32);
}

template <auto Fn>
struct Thunk;

template <class R, class... A, R (*Fn)(A...)>
struct Thunk<Fn>
{
    static void Call(PPCContext& ctx, uint8_t*)
    {
        ArgCursor c;
        std::tuple<A...> args{ ReadArg<A>(ctx, c)... };  // braced init: evaluated left to right
        if constexpr (std::is_void_v<R>)
            std::apply(Fn, args);
        else
            WriteResult(ctx, std::apply(Fn, args));
    }

    static std::string Args(PPCContext& ctx)
    {
        ArgCursor c;
        std::tuple<A...> args{ ReadArg<A>(ctx, c)... };
        std::string out;
        std::apply([&](const auto&... v) { ((out += (out.empty() ? "" : ", ") + FormatValue(v)), ...); }, args);
        return out;
    }

    static std::string Result(PPCContext& ctx)
    {
        if constexpr (std::is_void_v<R>)
            return {};
        else
            return FormatValue(ReadResult<R>(ctx));
    }
};

struct ImportImpl
{
    const char* lib;
    const char* name;
    PPCFunc* call;
    std::string (*args)(PPCContext&);    // null: log raw r3-r6
    std::string (*result)(PPCContext&);  // null: log r3
    std::string fullName;                // "lib::name", filled on registration
    bool quiet = false;                  // excluded from the call log
    mutable int8_t kind = -1;            // trace::ImportKind, classified on first timed call
    bool flushesGpu = true;              // publishes queued GPU commands first (all but GX2's state and draw calls)
    // Outermost calls while timing is on (updated through std::atomic_ref: several threads call one import).
    mutable uint64_t timedNs = 0, timedCalls = 0;
};

// The imports that took the most time since the last call (while timing is on), as "name ms calls" lines.
std::string TakeTopImportTimes(size_t count, double perFrames);

// Time import calls per thread and kind (trace::ThreadState::importNs), for the frames CSV and --gpu-stats;
// `perImport` also keeps each import's totals (TakeTopImportTimes, --gpu-stats).
void SetImportTiming(bool enabled, bool perImport = false);

void Register(ImportImpl impl);
struct Registrar
{
    explicit Registrar(ImportImpl impl) { Register(std::move(impl)); }
};

// Make a host function callable through a guest function pointer (e.g. coreinit's MEM* allocator hooks).
// The call is logged like an import. Returns the guest address to store in the pointer. With call == nullptr
// the function is a placeholder: calling it stops with "unimplemented import lib::name".
uint32_t RegisterHostFunction(const char* lib, const char* name, PPCFunc* call, std::string (*args)(PPCContext&),
    std::string (*result)(PPCContext&));

template <auto Fn>
uint32_t HostFunction(const char* lib, const char* name)
{
    return RegisterHostFunction(lib, name, &Thunk<Fn>::Call, &Thunk<Fn>::Args, &Thunk<Fn>::Result);
}

// The registered implementation of "lib::name", or null. Used for modules loaded through OSDynLoad.
const ImportImpl* FindImplementation(const std::string& fullName);

// Startup: resolve the generated import table against the registry and build the indirect-call tables.
void InitImports();
void SetImportLogging(bool enabled);
void QuietImport(const std::string& name);
void LogOnlyImport(const std::string& name);  // repeatable; restricts the call log to these imports

} // namespace cafe

#define CAFE_CAT2(a, b) a##b
#define CAFE_CAT(a, b) CAFE_CAT2(a, b)
#define CAFE_IMPORT(lib, name, fn)                                                                              \
    static ::cafe::Registrar CAFE_CAT(cafeImport_, __COUNTER__)(::cafe::ImportImpl{ #lib, name,                 \
        &::cafe::Thunk<&fn>::Call, &::cafe::Thunk<&fn>::Args, &::cafe::Thunk<&fn>::Result, {} })
#define CAFE_IMPORT_RAW(lib, name, fn)                                                                          \
    static ::cafe::Registrar CAFE_CAT(cafeImport_, __COUNTER__)(::cafe::ImportImpl{ #lib, name, &fn, nullptr, nullptr, {} })
