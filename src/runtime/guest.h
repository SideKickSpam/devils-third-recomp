// Host-side access to guest memory, and the guest address-space layout.
#pragma once

#include "ppc_context.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace guest
{
// ---- address map (Cafe OS application view) ----
constexpr uint32_t CodeBase = 0x02000000;
constexpr uint32_t DataBase = 0x10000000;
constexpr uint32_t Mem2End = 0x50000000;         // cos.xml max_size 0x40000000 from DataBase
constexpr uint32_t ForegroundBase = 0xE0000000;  // foreground bucket, 40 MB
constexpr uint32_t ForegroundSize = 0x02800000;
constexpr uint32_t Mem1Base = 0xF4000000;        // MEM1, 32 MB
constexpr uint32_t Mem1Size = 0x02000000;
constexpr uint32_t SystemBase = 0x60000000;      // runtime-owned allocations (thread objects, stacks, ...)
constexpr uint32_t SystemSize = 0x10000000;
constexpr uint32_t HostFunctionBase = 0xC0B00000;  // fake addresses for host functions callable by the guest

extern uint8_t* base;

inline uint8_t* Ptr(uint32_t a) { return base + a; }
inline uint8_t Read8(uint32_t a) { return base[a]; }
inline uint16_t Read16(uint32_t a) { uint16_t v; memcpy(&v, base + a, 2); return __builtin_bswap16(v); }
inline uint32_t Read32(uint32_t a) { uint32_t v; memcpy(&v, base + a, 4); return __builtin_bswap32(v); }
inline uint64_t Read64(uint32_t a) { uint64_t v; memcpy(&v, base + a, 8); return __builtin_bswap64(v); }
inline void Write8(uint32_t a, uint8_t v) { base[a] = v; }
inline void Write16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(base + a, &v, 2); }
inline void Write32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(base + a, &v, 4); }
inline void Write64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(base + a, &v, 8); }

inline std::string ReadString(uint32_t a, size_t max = 4096)
{
    if (!a)
        return {};
    const char* p = reinterpret_cast<const char*>(base + a);
    return std::string(p, strnlen(p, max));
}

inline void WriteString(uint32_t a, const std::string& s, size_t max)
{
    const size_t n = s.size() < max ? s.size() : max - 1;
    memcpy(base + a, s.data(), n);
    base[a + n] = 0;
}

inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t AlignDown(uint32_t v, uint32_t a) { return v & ~(a - 1); }

// Allocate from the runtime-owned system area (never freed back to the game's heaps).
uint32_t SystemAlloc(uint32_t size, uint32_t align = 16);
void SystemFree(uint32_t addr);

// The context of the guest thread running on the calling host thread.
PPCContext* CurrentContext();
void SetCurrentContext(PPCContext* ctx);

// Call a guest function from host code on the current guest thread. Arguments go in r3.., the result
// comes back in r3. Volatile registers are clobbered as on a real call; LR is preserved.
uint32_t Call(uint32_t addr, std::initializer_list<uint32_t> args = {});

// Register a host function that the guest can call through a function pointer; returns its fake address.
uint32_t RegisterHostFunction(const char* name, PPCFunc* fn);
} // namespace guest
