// Host OS abstraction: everything that differs between Linux (POSIX) and Windows (Win32).
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

namespace platform
{
// Reserve the 4 GB guest address space as zero-filled read/write memory. Returns null on failure.
uint8_t* ReserveGuestMemory(uint64_t size);

// Run fn on a new detached host thread with the given stack size. Recompiled code recurses deeply and keeps
// large frames, so guest threads need more than the Windows default of 1 MB.
constexpr size_t GuestThreadStackSize = 16 * 1024 * 1024;
void StartThread(std::function<void()> fn, size_t stackSize = GuestThreadStackSize);

// Call report(description, faultAddress) when the process takes a fatal CPU exception (access violation,
// illegal instruction, ...). report is expected not to return. Also called once per thread to prepare it.
using CrashReporter = void (*)(const char* what, uintptr_t faultAddress);
void InstallCrashHandler(CrashReporter report);
void PrepareThreadForCrashes();

// Call quit(reason) on a normal thread when the user interrupts the process: Ctrl+C or SIGTERM (Linux), or a
// console control event (Windows: Ctrl+C, Ctrl+Break, console closed, logoff, shutdown). quit is expected to exit.
void InstallQuitSignals(void (*quit)(const char* reason));

// CPU time the calling thread has used (user + kernel), in seconds.
double ThreadCpuSeconds();

// Run the calling thread below normal priority (background work that must not take time from the game).
void LowerCurrentThreadPriority();

// Make short sleeps accurate. Windows rounds Sleep and timed waits up to its timer tick (15.6 ms by
// default), which slows down guest code that sleeps for a fraction of a frame in a loop.
void UsePreciseTimers();

// On hybrid CPUs (Intel 12th gen and later: performance and efficiency cores), restrict the process's threads to
// one kind: "performance", "efficiency", or "all" (the OS decides). Returns what was done, for the log.
std::string RestrictCores(const std::string& which);

// Native sampling (Windows; elsewhere these return nothing). A handle to the calling thread that another thread
// can suspend; suspend `thread`, write up to `max` code addresses of its native stack (innermost first) and resume
// it. The caller must not allocate or take locks between the two (the thread may hold them): this only reads.
void* CurrentThreadHandle();
int SampleThreadStack(void* thread, uint64_t* out, int max);
// Debugging (Windows): a hardware data breakpoint on 4 bytes at `addr` in every thread of the process (threads started
// later get it within half a second). Each write calls report() on the writing thread with its native stack; a change
// found by polling (every 500 ms; kernel writes such as file reads) calls report(nullptr, rawValue) instead.
void WatchWrites(const void* addr, void (*report)(const uint64_t* stack, int depth));

// The executable's load address, and the module containing `addr` (name and base; empty if none).
uint64_t ImageBase();
std::string ModuleOf(uint64_t addr, uint64_t& base);

// Calendar time.
void GmTime(time_t t, struct tm* out);
void LocalTime(time_t t, struct tm* out);
time_t TimeGm(struct tm* tm);
long UtcOffsetSeconds(time_t t);

// 64-bit file positions.
int64_t FileTell(FILE* f);
int FileSeek(FILE* f, int64_t offset);
int64_t FileSize(FILE* f);
} // namespace platform
