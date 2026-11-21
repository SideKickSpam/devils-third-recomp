#include "trace.h"

#include "guest.h"
#include "host/audio.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>

PPC_TLS PPCTraceRing ppc_trace_ring;
bool ppc_trace_all = false;

namespace trace
{
namespace
{
struct Symbol
{
    uint32_t addr, size;
    std::string name;
};

std::mutex gThreadsMutex;
std::vector<ThreadState*> gThreads;
thread_local ThreadState* tState = nullptr;

std::vector<Symbol> gSymbols;
std::unordered_map<uint32_t, std::string> gImportNames;

std::mutex gTraceMutex;
FILE* gTraceFile = nullptr;

std::atomic<bool> gStopping{ false };
constexpr int ReportDepth = 32;

void PrintThread(ThreadState& t)
{
    if (!t.ring)  // exited: its trace ring went with it
    {
        dtlog::Important("--- thread %s: %llu import calls\n", t.name.c_str(), (unsigned long long)t.importCalls.load());
        return;
    }
    const PPCTraceRing& r = *t.ring;
    const uint32_t pos = r.pos;
    const uint32_t n = std::min<uint32_t>(pos, ReportDepth);
    const char* imp = t.inImport.load();
    dtlog::Important("--- thread %s: %u function entries, %llu import calls%s%s\n", t.name.c_str(), pos,
        (unsigned long long)t.importCalls.load(), imp ? ", currently in " : "", imp ? imp : "");
    if (t.ctx)
    {
        dtlog::Important("    lr=0x%08X r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n", t.ctx->lr, t.ctx->r[1].u32,
            t.ctx->r[3].u32, t.ctx->r[4].u32, t.ctx->r[5].u32);
        // The guest stack's back chain: each frame's saved LR (the word after the caller's back-chain pointer).
        std::string chain;
        uint32_t sp = t.ctx->r[1].u32;
        for (int i = 0; i < 16 && sp && sp < 0xFFFFFFF0u; i++)
        {
            const uint32_t next = guest::Read32(sp);
            if (!next || next <= sp || next >= 0xFFFFFFF0u)
                break;
            char b[24];
            snprintf(b, sizeof(b), " 0x%08X", guest::Read32(next + 4));
            chain += b;
            sp = next;
        }
        dtlog::Important("    guest return addresses:%s\n", chain.c_str());
        dtlog::Important("    f1=%g f29=%g f30=%g f31=%g (ps1 %g %g %g)\n", t.ctx->f[1].ps0, t.ctx->f[29].ps0, t.ctx->f[30].ps0,
            t.ctx->f[31].ps0, t.ctx->f[29].ps1, t.ctx->f[30].ps1, t.ctx->f[31].ps1);
    }
    dtlog::Important("    last %u function entries, oldest first:\n", n);
    for (uint32_t i = pos - n; i != pos; i++)
        dtlog::Important("      %s\n", Describe(r.addr[i & 255]).c_str());
    // The native stack (another thread only: a thread cannot suspend itself). Recompiled functions are native, so this
    // is the guest call chain too; src/tools/symbolize.py names the e+ offsets with build/game/dt_port.map.
    if (t.osThread && &t != tState)
    {
        uint64_t addrs[48];
        const int count = platform::SampleThreadStack(t.osThread, addrs, 48);
        const uint64_t image = platform::ImageBase();
        std::string line;
        for (int i = 0; i < count; i++)
        {
            char buf[32];
            if (addrs[i] >= image && addrs[i] - image < 0x40000000)
                snprintf(buf, sizeof(buf), " e+%llx", (unsigned long long)(addrs[i] - image));
            else
                snprintf(buf, sizeof(buf), " ?%llx", (unsigned long long)addrs[i]);
            line += buf;
        }
        if (count)
            dtlog::Important("    native stack, innermost first:%s\n", line.c_str());
    }
}
} // namespace

ThreadState& RegisterThread(const std::string& name, PPCContext* ctx)
{
    auto* t = new ThreadState;  // kept alive for reports even after the thread exits
    t->name = name;
    t->ring = &ppc_trace_ring;
    t->ctx = ctx;
    t->osThread = platform::CurrentThreadHandle();
    tState = t;

    platform::PrepareThreadForCrashes();

    std::lock_guard lock(gThreadsMutex);
    gThreads.push_back(t);
    return *t;
}

void UnregisterThread()
{
    // Under the lock: the profiler and import timing read thread names, and the trace ring is thread-local
    // storage that goes away with the thread.
    std::lock_guard lock(gThreadsMutex);
    if (tState)
        tState->name += " (exited)", tState->ring = nullptr;
}

ThreadState* Current() { return tState; }

std::string CurrentThreadName() { return tState ? tState->name : "?"; }

void ImportTimes(const char* name, uint64_t out[KindCount])
{
    for (int k = 0; k < KindCount; k++)
        out[k] = 0;
    std::lock_guard lock(gThreadsMutex);
    for (ThreadState* t : gThreads)
        if (t->name == name)
            for (int k = 0; k < KindCount; k++)
                out[k] += t->importNs[k].load(std::memory_order_relaxed);
}

void AddSymbol(uint32_t addr, uint32_t size, std::string name) { gSymbols.push_back({ addr, size, std::move(name) }); }

void FinishSymbols()
{
    std::sort(gSymbols.begin(), gSymbols.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
    for (const PPCImportMapping* m = PPCImportMappings; m->func; m++)
        gImportNames[m->addr] = std::string(m->lib) + "::" + m->name;
}

uint32_t FindSymbol(const std::string& name)
{
    for (const Symbol& s : gSymbols)
        if (s.name == name)
            return s.addr;
    return 0;
}

std::string Describe(uint32_t addr)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08X", addr);
    if (auto it = gImportNames.find(addr); it != gImportNames.end())
        return std::string(buf) + " [import] " + it->second;
    auto it = std::upper_bound(gSymbols.begin(), gSymbols.end(), addr, [](uint32_t a, const Symbol& s) { return a < s.addr; });
    if (it == gSymbols.begin())
        return buf;
    const Symbol& s = *std::prev(it);
    if (addr >= s.addr + std::max<uint32_t>(s.size, 4))
        return buf;
    std::string out = std::string(buf) + " " + s.name;
    if (addr != s.addr)
    {
        snprintf(buf, sizeof(buf), "+0x%X", addr - s.addr);
        out += buf;
    }
    return out;
}

void EnableFullTrace(const char* path)
{
    gTraceFile = fopen(path, "w");
    if (!gTraceFile)
    {
        perror(path);
        return;
    }
    setvbuf(gTraceFile, nullptr, _IOFBF, 1 << 20);
    ppc_trace_all = true;
}

void InstallCrashHandlers()
{
    platform::InstallCrashHandler([](const char* what, uintptr_t fault) {
        const uintptr_t b = reinterpret_cast<uintptr_t>(guest::base);
        const char* name = tState ? tState->name.c_str() : "(non-guest thread)";
        if (b && fault >= b && fault - b < PPC_MEMORY_SIZE)
            Stop(7, "crash: %s at guest address 0x%08X in thread %s", what, uint32_t(fault - b), name);
        Stop(7, "crash: %s at host address 0x%llX in thread %s", what, (unsigned long long)fault, name);
    });
}

std::atomic<uint64_t> gFramesSeen{ 0 };
void NoteFrame() { gFramesSeen.fetch_add(1, std::memory_order_relaxed); }

void StartFrameWatchdog(int seconds)
{
    std::thread([seconds] {
        uint64_t last = 0;
        int idle = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t frames = gFramesSeen.load(std::memory_order_relaxed);
            idle = frames && frames == last ? idle + 1 : 0;
            last = frames;
            if (idle >= seconds)
                Stop(6, "hang: no frame presented for %d seconds (after frame %llu)", seconds, (unsigned long long)frames);
        }
    }).detach();
}

void StartWatchdog(int seconds)
{
    std::thread([seconds] {
        uint64_t last = ~0ull;
        int idle = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t progress = 0;
            {
                std::lock_guard lock(gThreadsMutex);
                for (ThreadState* t : gThreads)
                    progress += (t->ring ? t->ring->pos : 0) + t->importCalls.load();
            }
            idle = progress == last ? idle + 1 : 0;
            last = progress;
            if (idle >= seconds)
                Stop(6, "hang: no guest function entries or import calls for %d seconds", seconds);
        }
    }).detach();
}

void StartProfiler(const std::string& threadName)
{
    std::thread([threadName] {
        std::unordered_map<std::string, uint64_t> imports;
        std::unordered_map<uint32_t, uint64_t> functions;
        uint64_t samples = 0;
        auto last = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard lock(gThreadsMutex);
                for (ThreadState* t : gThreads)
                {
                    if (t->name != threadName || !t->ring)
                        continue;
                    samples++;
                    if (const char* imp = t->inImport.load())
                        imports[imp]++;
                    else
                        functions[t->ring->addr[(t->ring->pos - 1) & 255]]++;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(5) || !samples)
                continue;
            last = now;
            std::vector<std::pair<uint64_t, std::string>> top;
            for (auto& [name, n] : imports)
                top.push_back({ n, "[import] " + name });
            for (auto& [addr, n] : functions)
                top.push_back({ n, Describe(addr) });
            std::sort(top.begin(), top.end(), std::greater<>());
            dtlog::Important("[profile] %s, %llu samples:\n", threadName.c_str(), (unsigned long long)samples);
            for (size_t i = 0; i < top.size() && i < 20; i++)
                dtlog::Important("  %5.1f%%  %s\n", 100.0 * top[i].first / samples, top[i].second.c_str());
            imports.clear(), functions.clear(), samples = 0;
        }
    }).detach();
}

void StartSampler(const std::string& path, const std::vector<std::string>& threads)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
    {
        dtlog::Important("[sample] cannot write %s\n", path.c_str());
        return;
    }
    const uint64_t image = platform::ImageBase();
    fprintf(f, "# dt-port samples: frame thread addresses (innermost first); e+X = executable offset, m:NAME+X = module\n");
    for (size_t i = 0; i < threads.size(); i++)
        fprintf(f, "thread %zu %s\n", i, threads[i].c_str());
    std::thread([f, threads, image] {
        constexpr int MaxDepth = 32;
        std::vector<std::pair<size_t, void*>> targets;
        uint64_t addrs[MaxDepth];
        auto lastFlush = std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            targets.clear();
            {
                std::lock_guard lock(gThreadsMutex);
                for (ThreadState* t : gThreads)
                    for (size_t i = 0; i < threads.size(); i++)
                        if (t->osThread && t->ring && t->name == threads[i])
                            targets.push_back({ i, t->osThread });
            }
            for (auto& [index, handle] : targets)
            {
                const uint64_t frame = gFramesSeen.load(std::memory_order_relaxed) + 1;
                const int n = platform::SampleThreadStack(handle, addrs, MaxDepth);  // no allocation in here
                if (!n)
                    continue;
                fprintf(f, "%llu %zu", (unsigned long long)frame, index);
                for (int i = 0; i < n; i++)
                {
                    uint64_t base = 0;
                    if (addrs[i] >= image && addrs[i] - image < 0x40000000)
                    {
                        fprintf(f, " e+%llx", (unsigned long long)(addrs[i] - image));
                        continue;
                    }
                    const std::string name = platform::ModuleOf(addrs[i], base);
                    if (name.empty())
                        fprintf(f, " ?%llx", (unsigned long long)addrs[i]);
                    else
                        fprintf(f, " m:%s+%llx", name.c_str(), (unsigned long long)(addrs[i] - base));
                }
                fputc('\n', f);
            }
            if (std::chrono::steady_clock::now() - lastFlush > std::chrono::seconds(1))
                fflush(f), lastFlush = std::chrono::steady_clock::now();
        }
    }).detach();
    dtlog::Important("[sample] sampling %zu thread name(s) into %s\n", threads.size(), path.c_str());
}

void Stop(int code, const char* fmt, ...)
{
    if (gStopping.exchange(true))
    {
        // Another thread is already reporting; wait for it to exit the process.
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    char reason[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);

    dtlog::Important("\n=== dt-port stopped: %s ===\n", reason);
    if (tState)
    {
        dtlog::Important("stopping thread:\n");
        PrintThread(*tState);
    }
    {
        std::lock_guard lock(gThreadsMutex);
        for (ThreadState* t : gThreads)
            if (t != tState)
                PrintThread(*t);
    }
    if (gTraceFile)
    {
        std::lock_guard lock(gTraceMutex);
        fflush(gTraceFile);
    }
    host::audio::Close();
    dtlog::Flush();
    fflush(stdout);
    std::_Exit(code);
}
} // namespace trace

void ppc_trace_log(uint32_t addr)
{
    if (!trace::gTraceFile)
        return;
    const trace::ThreadState* t = trace::tState;
    const std::string what = trace::Describe(addr);
    std::lock_guard lock(trace::gTraceMutex);
    fprintf(trace::gTraceFile, "%s %s\n", t ? t->name.c_str() : "?", what.c_str());
}
