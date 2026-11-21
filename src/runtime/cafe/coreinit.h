// Shared declarations for the coreinit implementation.
#pragma once

#include "../ppc_context.h"

#include <condition_variable>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace cafe::coreinit
{
// Espresso timer: bus clock (248.625 MHz) / 4.
constexpr uint64_t TimerHz = 62156250;
constexpr uint32_t BusHz = 248625000;
constexpr uint32_t CoreHz = 1243125000;

// ---- threads ----
constexpr uint32_t OSThreadSize = 0x6A0;
constexpr uint32_t OSThreadTag = 0x74487244;  // 'tHrD' at +0x320

struct Thread
{
    uint32_t guest = 0;  // OSThread*
    std::string name;
    PPCContext ctx{};
    uint32_t entry = 0, argc = 0, argv = 0, stackTop = 0, stackSize = 0, attr = 0;
    int32_t priority = 16;
    uint32_t affinity = 7;
    uint32_t specific[16] = {};
    uint32_t errnoAddr = 0;

    std::mutex mutex;
    std::condition_variable cv;
    int32_t suspendCount = 1;
    bool started = false, finished = false, detached = false;
    int32_t exitValue = 0;
};

Thread* CurrentThread();
Thread* FindThread(uint32_t guestThread);
// Create the OSThread for the host's main thread and make it current; returns its initial stack top.
Thread& InitMainThread(uint32_t stackSize, uint32_t sdaBase, uint32_t sda2Base);
void RunMainThread(uint32_t entry, uint32_t argc, uint32_t argv);
// Create and start a system guest thread named `name` running guest address `entry` (a registered host function,
// say) with a 256 KB stack; returns the OSThread.
uint32_t StartSystemThread(const char* name, uint32_t entry, int32_t priority);

// ---- time ----
uint64_t SystemTicks();  // since process start
// Unit conversions that cannot overflow for any tick count (whole seconds and the remainder separately).
inline int64_t TicksToNs(int64_t ticks)
{
    return ticks / int64_t(TimerHz) * 1000000000 + ticks % int64_t(TimerHz) * 1000000000 / int64_t(TimerHz);
}
inline int64_t TicksToUs(int64_t ticks)
{
    return ticks / int64_t(TimerHz) * 1000000 + ticks % int64_t(TimerHz) * 1000000 / int64_t(TimerHz);
}
inline int64_t UsToTicks(int64_t us) { return us / 1000000 * int64_t(TimerHz) + us % 1000000 * int64_t(TimerHz) / 1000000; }
int64_t OSTimeNow();     // ticks since 2000-01-01, local time

// ---- sync ----
// Post a 16-byte OSMessage (four big-endian words) to a guest OSMessageQueue.
bool SendMessage(uint32_t queue, const uint32_t words[4], bool block);

// ---- file system (coreinit_fs.cpp), shared with nn_save ----
// Open a normalised guest path ("/vol/save/...") with an fopen-style mode; writes the handle on success.
int32_t FsOpen(const std::string& guestPath, const std::string& mode, uint32_t outHandle);
// Complete an FS async command later, as the Wii U does: the result goes out after the call returns, through
// the AppIO thread (FSAsyncData callback) or the caller's message queue. `op` runs on the FS worker thread first,
// in order with the client's other async commands. Without asyncData the operation runs now and its status is
// returned.
int32_t FsAsync(uint32_t client, uint32_t block, uint32_t asyncData, std::function<int32_t()> op);
// FsAsync for a command whose status is already known.
int32_t FsComplete(uint32_t client, uint32_t block, int32_t status, uint32_t asyncData);

// ---- misc ----
void InitDataImports();
void InitSystem();
} // namespace cafe::coreinit
