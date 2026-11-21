// Function-entry tracing, per-thread state for reports, crash handling and hang detection.
#pragma once

#include "ppc_context.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace trace
{
// Per host thread that runs guest code.
// Kinds of host import, for timing where a guest thread's time goes (--gpu-stats).
enum ImportKind
{
    KindWait,   // blocking: waits on events, semaphores, mutexes, messages, joins; sleeps and yields
    KindFs,     // file system
    KindSwap,   // GX2SwapScanBuffers: frame pacing and presenting
    KindGx2,    // other GX2: state, draws (the renderer's work)
    KindOther,  // everything else
    KindCount
};

struct ThreadState
{
    std::string name;
    PPCTraceRing* ring = nullptr;
    PPCContext* ctx = nullptr;
    std::atomic<const char*> inImport{ nullptr };  // "lib::name" while inside a host import, else null
    std::atomic<uint64_t> importCalls{ 0 };
    // Nanoseconds spent in outermost import calls, by kind (counted only while import timing is on).
    std::atomic<uint64_t> importNs[KindCount]{};
    uint32_t importDepth = 0;
    void* osThread = nullptr;  // platform::CurrentThreadHandle(), for the sampler
};

// The import times of the thread named `name` (zeros if there is none).
void ImportTimes(const char* name, uint64_t out[KindCount]);

// Call on every host thread before it runs guest code.
ThreadState& RegisterThread(const std::string& name, PPCContext* ctx);
void UnregisterThread();
ThreadState* Current();

// Symbol names for reports and traces (filled from the ELF's .symtab by the loader).
void AddSymbol(uint32_t addr, uint32_t size, std::string name);
void FinishSymbols();
std::string Describe(uint32_t addr);
uint32_t FindSymbol(const std::string& name);  // a symbol's address by its (mangled) name, or 0

void EnableFullTrace(const char* path);
void InstallCrashHandlers();
void StartWatchdog(int seconds);
// Reports a hang when no frame has been presented for `seconds` after the first (threads may still be spinning,
// which the progress watchdog counts as progress). NoteFrame is called on every swap.
void StartFrameWatchdog(int seconds);
// --sample FILE: sample the native stacks of the threads named in `threads` about 1000 times a second (Windows) and
// write one line per sample: the frame being made, the thread and its code addresses (src/tools/sample_report.py).
void StartSampler(const std::string& path, const std::vector<std::string>& threads);
void NoteFrame();
// Sample the named guest thread's most recent function entry (or the import it is in) every millisecond and
// log the most frequent ones every 5 seconds. A cheap way to see where a thread spends its time.
void StartProfiler(const std::string& threadName);
// The registered name of the calling thread ("?" for an unregistered host thread).
std::string CurrentThreadName();

// Print reason plus every thread's most recent function entries, then exit with code.
[[noreturn]] void Stop(int code, const char* reasonFmt, ...) __attribute__((format(printf, 2, 3)));
} // namespace trace
