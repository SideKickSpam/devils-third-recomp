#include "imports.h"
#include "gpu/queue.h"

#include "log.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <tuple>
#include <chrono>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace cafe
{
namespace
{
std::vector<ImportImpl>& Registry()
{
    static std::vector<ImportImpl> r;
    return r;
}

std::vector<const ImportImpl*> gResolved;  // per PPCImportMappings index
bool gLogImports = true;
bool gTimeImports = false;
bool gImportTotals = false;  // per-import totals (TakeTopImportTimes), for --gpu-stats

// Import timing is on in every session (the frames CSV), so it must cost next to nothing: steady_clock::now() went
// through ntdll's QueryPerformanceCounter twice per call, about 10% of the rendering thread's time. On x86-64 the
// timestamp counter is read directly (invariant on every CPU this runs on) and scaled to ns with a calibrated factor.
#if defined(_M_X64) || defined(__x86_64__)
uint64_t gNsPerTickQ32 = 0;  // ns per TSC tick, 32.32 fixed point

void CalibrateTsc()
{
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = __rdtsc();
    auto t1 = t0;
    while (t1 - t0 < std::chrono::milliseconds(20))
        t1 = std::chrono::steady_clock::now();
    const uint64_t c1 = __rdtsc();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    gNsPerTickQ32 = c1 > c0 ? uint64_t(ns / double(c1 - c0) * 4294967296.0) : 0;
}

inline uint64_t TimerNow() { return gNsPerTickQ32 ? __rdtsc() : uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }
inline uint64_t TimerToNs(uint64_t d)
{
    if (!gNsPerTickQ32)
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(d)).count());
    return uint64_t((unsigned __int128)d * gNsPerTickQ32 >> 32);
}
#else
void CalibrateTsc() {}
inline uint64_t TimerNow() { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }
inline uint64_t TimerToNs(uint64_t d)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(d)).count());
}
#endif

// Counters only their own thread writes: a plain load and store, not a locked read-modify-write.
inline void AddOwn(std::atomic<uint64_t>& a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }

trace::ImportKind Classify(const ImportImpl& impl)
{
    const std::string_view lib = impl.lib, name = impl.name;
    if (lib == "gx2")
        return name == "GX2SwapScanBuffers" ? trace::KindSwap : trace::KindGx2;
    if (lib == "coreinit" && name.starts_with("FS"))
        return trace::KindFs;
    for (std::string_view w : { "OSWait", "OSSleep", "OSYieldThread", "OSJoinThread", "OSReceiveMessage", "OSLockMutex",
             "OSFastMutex_Lock", "OSUninterruptibleSpinLock_Acquire", "OSSuspendThread" })
        if (name.starts_with(w))
            return trace::KindWait;
    return trace::KindOther;
}
std::unordered_set<std::string> gQuiet;
std::unordered_set<std::string> gOnly;  // if non-empty, log only these

// Host functions reachable through guest function pointers.
constexpr size_t MaxHostFunctions = 512;
std::array<ImportImpl, MaxHostFunctions> gHostFunctions;
size_t gHostFunctionCount = 0;

std::vector<PPCFunc*> gCodeTable;                 // (addr - gCodeBase) / 4 -> function
// The code's range: from the function table (no generated header: release runtimes are built without the game).
uint32_t gCodeBase = 0, gCodeEnd = 0;
std::unordered_map<uint32_t, PPCFunc*> gFarTable;  // import stubs and host functions

std::string RawArgs(PPCContext& ctx)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "r3=0x%X, r4=0x%X, r5=0x%X, r6=0x%X, r7=0x%X", ctx.r[3].u32, ctx.r[4].u32, ctx.r[5].u32,
        ctx.r[6].u32, ctx.r[7].u32);
    return buf;
}

void Dispatch(PPCContext& ctx, uint8_t* base, const ImportImpl* impl, const char* lib, const char* name, uint32_t addr)
{
    ppc_trace_ring.addr[ppc_trace_ring.pos++ & 255] = addr;
    trace::ThreadState* t = trace::Current();
    const char* thread = t ? t->name.c_str() : "?";

    if (!impl || !impl->call)  // no implementation (static import, or a dynamic export registered without one)
    {
        dtlog::Important("[%s] %s::%s(%s) from 0x%08X: NOT IMPLEMENTED\n", thread, lib, name, RawArgs(ctx).c_str(), ctx.lr);
        if (std::string_view(lib) == "gx2")
            trace::Stop(0, "reached gx2: first call is gx2::%s (from 0x%08X)", name, ctx.lr);
        trace::Stop(3, "unimplemented import %s::%s (from 0x%08X)", lib, name, ctx.lr);
    }

    const bool log = gLogImports && !impl->quiet;
    std::string args;
    if (log)
        args = impl->args ? impl->args(ctx) : RawArgs(ctx);
    const uint32_t lr = ctx.lr;
    if (t)
    {
        AddOwn(t->importCalls, 1);
        t->inImport.store(impl->fullName.c_str(), std::memory_order_relaxed);
    }
    if (impl->flushesGpu)
        gpu::queue::Flush();
    // GX2 state and draw calls (tens of thousands a frame on the rendering thread) are timed one in 16 and counted 16
    // times: two timestamp reads per call were most of the dispatcher's own cost. Everything else is timed each call.
    if (gTimeImports && t && t->importDepth == 0 && !impl->flushesGpu && (t->importCalls.load(std::memory_order_relaxed) & 15))
        impl->call(ctx, base);
    else if (gTimeImports && t && t->importDepth == 0)
    {
        // Only outermost calls count: guest code an import calls back into (FS callbacks, say) is its time.
        if (impl->kind < 0)
            impl->kind = int8_t(Classify(*impl));
        t->importDepth++;
        const uint64_t start = TimerNow();
        impl->call(ctx, base);
        const uint64_t ns = TimerToNs(TimerNow() - start) * (impl->flushesGpu ? 1 : 16);
        AddOwn(t->importNs[impl->kind], ns);
        if (gImportTotals)
        {
            std::atomic_ref<uint64_t>(impl->timedNs).fetch_add(ns, std::memory_order_relaxed);
            std::atomic_ref<uint64_t>(impl->timedCalls).fetch_add(impl->flushesGpu ? 1 : 16, std::memory_order_relaxed);
        }
        t->importDepth--;
    }
    else if (t && t->importDepth)
    {
        t->importDepth++;
        impl->call(ctx, base);
        t->importDepth--;
    }
    else
        impl->call(ctx, base);
    if (t)
        t->inImport.store(nullptr, std::memory_order_relaxed);
    if (log)
    {
        std::string result = impl->result ? impl->result(ctx) : "";
        dtlog::Write("[%s] %s(%s)%s%s  (from 0x%08X)\n", thread, impl->fullName.c_str(), args.c_str(),
            result.empty() ? "" : " -> ", result.c_str(), lr);
    }
}

template <size_t N>
void HostTrampoline(PPCContext& __restrict ctx, uint8_t* __restrict base)
{
    const ImportImpl& f = gHostFunctions[N];
    Dispatch(ctx, base, &f, f.lib, f.name, guest::HostFunctionBase + uint32_t(N) * 8);
}

template <size_t... I>
constexpr std::array<PPCFunc*, sizeof...(I)> MakeTrampolines(std::index_sequence<I...>)
{
    return { &HostTrampoline<I>... };
}
constexpr auto gTrampolines = MakeTrampolines(std::make_index_sequence<MaxHostFunctions>());
} // namespace

// GX2 calls that only queue state or work need not publish the commands queued before them; anything else may block
// or read what the GPU produced (GX2DrawDone, GX2GetRetiredTimeStamp, OSWaitEvent...).
bool FlushesGpu(std::string_view lib, std::string_view name)
{
    if (lib != "gx2")
        return true;
    for (std::string_view p : { "GX2Set", "GX2Draw", "GX2Init", "GX2Invalidate", "GX2Clear", "GX2Copy", "GX2Calc" })
        if (name.starts_with(p))
            return false;
    return true;
}

void Register(ImportImpl impl)
{
    impl.fullName = std::string(impl.lib) + "::" + impl.name;
    impl.flushesGpu = FlushesGpu(impl.lib, impl.name);
    Registry().push_back(std::move(impl));
}

uint32_t RegisterHostFunction(const char* lib, const char* name, PPCFunc* call, std::string (*args)(PPCContext&),
    std::string (*result)(PPCContext&))
{
    if (gHostFunctionCount == MaxHostFunctions)
        trace::Stop(1, "too many host functions");
    const size_t n = gHostFunctionCount++;
    gHostFunctions[n] = ImportImpl{ lib, name, call, args, result, std::string(lib) + "::" + name };
    const uint32_t addr = guest::HostFunctionBase + uint32_t(n) * 8;
    gFarTable[addr] = gTrampolines[n];
    return addr;
}

void InitImports()
{
    std::unordered_map<std::string, const ImportImpl*> byName;
    for (const ImportImpl& impl : Registry())
    {
        if (!byName.emplace(impl.fullName, &impl).second)
            trace::Stop(1, "import %s registered twice", impl.fullName.c_str());
    }
    size_t implemented = 0, total = 0;
    for (const PPCImportMapping* m = PPCImportMappings; m->func; m++, total++)
    {
        auto it = byName.find(std::string(m->lib) + "::" + m->name);
        gResolved.push_back(it != byName.end() ? it->second : nullptr);
        implemented += it != byName.end();
        gFarTable[m->addr] = m->func;
        byName.erase(std::string(m->lib) + "::" + m->name);
    }
    for (ImportImpl& impl : Registry())
        if (gQuiet.count(impl.fullName) || gQuiet.count(impl.name) ||
            (!gOnly.empty() && !gOnly.count(impl.fullName) && !gOnly.count(impl.name)))
            impl.quiet = true;
    std::unordered_set<std::string> staticLibs;
    for (const PPCImportMapping* m = PPCImportMappings; m->func; m++)
        staticLibs.insert(m->lib);
    for (auto& [name, impl] : byName)
        if (staticLibs.count(impl->lib))  // other libraries are reached through OSDynLoad
            dtlog::Important("warning: %s is implemented but the game does not import it\n", name.c_str());

    gCodeBase = ~0u, gCodeEnd = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->func; m++)
        gCodeBase = std::min(gCodeBase, m->addr), gCodeEnd = std::max(gCodeEnd, m->addr + 4);
    if (gCodeEnd <= gCodeBase)
        gCodeBase = gCodeEnd = 0;
    gCodeTable.assign((gCodeEnd - gCodeBase) / 4, nullptr);
    for (const PPCFuncMapping* m = PPCFuncMappings; m->func; m++)
        gCodeTable[(m->addr - gCodeBase) / 4] = m->func;
    dtlog::Important("[dt-port] %zu of %zu imports implemented\n", implemented, total);
}

const ImportImpl* FindImplementation(const std::string& fullName)
{
    for (const ImportImpl& impl : Registry())
        if (impl.fullName == fullName)
            return &impl;
    return nullptr;
}

void SetImportLogging(bool enabled) { gLogImports = enabled; }
void SetImportTiming(bool enabled, bool perImport)
{
    static std::once_flag calibrated;
    if (enabled)
        std::call_once(calibrated, CalibrateTsc);
    gTimeImports = enabled;
    gImportTotals = gImportTotals || (enabled && perImport);
}

std::string TakeTopImportTimes(size_t count, double perFrames)
{
    std::vector<std::tuple<uint64_t, uint64_t, const ImportImpl*>> all;
    auto take = [&](const ImportImpl& impl) {
        const uint64_t ns = std::atomic_ref<uint64_t>(impl.timedNs).exchange(0, std::memory_order_relaxed);
        const uint64_t calls = std::atomic_ref<uint64_t>(impl.timedCalls).exchange(0, std::memory_order_relaxed);
        if (ns)
            all.push_back({ ns, calls, &impl });
    };
    for (const ImportImpl& impl : Registry())
        take(impl);
    for (size_t i = 0; i < gHostFunctionCount; i++)
        take(gHostFunctions[i]);
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string out;
    for (size_t i = 0; i < all.size() && i < count; i++)
    {
        char b[160];
        snprintf(b, sizeof(b), " %s %.2f ms (%.0f calls);", std::get<2>(all[i])->fullName.c_str(),
            double(std::get<0>(all[i])) / 1e6 / perFrames, double(std::get<1>(all[i])) / perFrames);
        out += b;
    }
    return out;
}
void QuietImport(const std::string& name) { gQuiet.insert(name); }
void LogOnlyImport(const std::string& name) { gOnly.insert(name); }
} // namespace cafe

void ppc_import(PPCContext& ctx, uint8_t* base, uint32_t index)
{
    const PPCImportMapping& m = PPCImportMappings[index];
    cafe::Dispatch(ctx, base, cafe::gResolved[index], m.lib, m.name, m.addr);
}

// The slow path behind ppc_lookup_fast (ppc_context.h): the code table has a slot per instruction (89 MB) and imports
// are a hash map; each function found gets a number in ppc_func_list, and its address and number go into the cache.
std::atomic<uint64_t> ppc_lookup_cache[PPCLookupCacheSize];
namespace
{
constexpr size_t MaxListedFunctions = 1 << 20;
PPCFunc** gFuncList = new PPCFunc*[MaxListedFunctions]();
std::unordered_map<PPCFunc*, uint32_t> gFuncNumbers;  // every function known at InitImports (read-only after)
std::mutex gFuncListMutex;
std::unordered_map<PPCFunc*, uint32_t> gLateNumbers;  // functions registered later (gFuncListMutex)
uint32_t gNextNumber = 0;

PPCFunc* LookupSlow(uint32_t addr)
{
    if (addr - cafe::gCodeBase < cafe::gCodeEnd - cafe::gCodeBase && !(addr & 3))
        return cafe::gCodeTable[(addr - cafe::gCodeBase) >> 2];
    if (auto it = cafe::gFarTable.find(addr); it != cafe::gFarTable.end())
        return it->second;
    return nullptr;
}
} // namespace
PPCFunc* const* ppc_func_list = gFuncList;

// Numbers for every function known now (guest code, imports, host functions so far), read without a lock later.
void NumberFunctions()
{
    auto add = [](PPCFunc* f) {
        if (f && gFuncNumbers.try_emplace(f, gNextNumber).second && gNextNumber < MaxListedFunctions)
            gFuncList[gNextNumber++] = f;
    };
    for (const PPCFuncMapping* m = PPCFuncMappings; m->func; m++)
        add(m->func);
    for (auto& [a, f] : cafe::gFarTable)
        add(f);
}

PPCFunc* ppc_lookup(uint32_t addr)
{
    static std::once_flag numbered;
    std::call_once(numbered, NumberFunctions);
    if (PPCFunc* f = LookupSlow(addr))
    {
        if (addr)
        {
            uint32_t number = ~0u;
            if (auto it = gFuncNumbers.find(f); it != gFuncNumbers.end())
                number = it->second;
            else
            {
                std::lock_guard lock(gFuncListMutex);
                auto [late, added] = gLateNumbers.try_emplace(f, gNextNumber);
                if (added && gNextNumber < MaxListedFunctions)
                    gFuncList[gNextNumber++] = f;  // (stored before the cache entry that names it: release below)
                number = late->second < MaxListedFunctions ? late->second : ~0u;
            }
            if (number != ~0u)
                ppc_lookup_cache[(addr >> 2) & (PPCLookupCacheSize - 1)].store(uint64_t(number) << 32 | addr,
                    std::memory_order_release);
        }
        return f;
    }
    PPCContext* ctx = guest::CurrentContext();
    trace::Stop(4, "indirect call to 0x%08X (%s), which is not a function entry (lr 0x%08X)", addr,
        trace::Describe(addr).c_str(), ctx ? ctx->lr : 0);
}

namespace guest
{
namespace
{
thread_local PPCContext* tContext = nullptr;
}

PPCContext* CurrentContext() { return tContext; }
void SetCurrentContext(PPCContext* ctx) { tContext = ctx; }

uint32_t Call(uint32_t addr, std::initializer_list<uint32_t> args)
{
    PPCContext& ctx = *tContext;
    int r = 3;
    for (uint32_t a : args)
        ctx.r[r++].u32 = a;
    const uint32_t savedLr = ctx.lr;
    ctx.lr = 0;
    ppc_lookup(addr)(ctx, base);
    ctx.lr = savedLr;
    return ctx.r[3].u32;
}
} // namespace guest
