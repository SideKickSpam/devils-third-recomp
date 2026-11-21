// coreinit: OSThread, mapped one-to-one onto host threads.
#include "../imports.h"
#include "../log.h"
#include "../platform.h"
#include "../trace.h"
#include "coreinit.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>

namespace cafe::coreinit
{
namespace
{
// OSThread field offsets that the game or debugging tools may look at.
constexpr uint32_t OffTag = 0x320, OffState = 0x324, OffAttr = 0x325, OffId = 0x326, OffSuspend = 0x328,
                   OffPriority = 0x32C, OffBasePriority = 0x330, OffExitValue = 0x334, OffStackStart = 0x394,
                   OffStackEnd = 0x398, OffEntry = 0x39C, OffName = 0x5C0;

std::mutex gThreadsMutex;
std::map<uint32_t, std::unique_ptr<Thread>> gThreads;
thread_local Thread* tThread = nullptr;
uint16_t gNextId = 1;
uint32_t gSdaBase = 0, gSda2Base = 0;

// Thrown by OSExitThread to unwind the guest call stack back to the thread's host entry.
struct ThreadExit
{
    int32_t value;
};

void WriteState(Thread& t, uint8_t state)
{
    guest::Write8(t.guest + OffState, state);  // 1 ready, 2 running, 4 waiting, 8 moribund
}

void Finish(Thread& t, int32_t value)
{
    {
        std::lock_guard lock(t.mutex);
        t.finished = true;
        t.exitValue = value;
    }
    guest::Write32(t.guest + OffExitValue, uint32_t(value));
    WriteState(t, 8);
    t.cv.notify_all();
}

void HostThreadMain(Thread* t)
{
    tThread = t;
    guest::SetCurrentContext(&t->ctx);
    trace::RegisterThread(t->name, &t->ctx);
    WriteState(*t, 2);
    int32_t result;
    try
    {
        result = int32_t(guest::Call(t->entry, { t->argc, t->argv }));
    }
    catch (const ThreadExit& e)
    {
        result = e.value;
    }
    dtlog::Write("[%s] thread exited with %d\n", t->name.c_str(), result);
    Finish(*t, result);
    trace::UnregisterThread();
}

void Start(Thread& t)
{
    t.started = true;
    platform::StartThread([&t] { HostThreadMain(&t); });
}

Thread* Get(uint32_t guestThread)
{
    if (!guestThread)
        return tThread;
    return FindThread(guestThread);
}

// ---- imports ----
bool OSCreateThread(uint32_t thread, uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stackTop, uint32_t stackSize,
    int32_t priority, uint32_t attr)
{
    auto t = std::make_unique<Thread>();
    t->guest = thread;
    t->entry = entry;
    t->argc = argc;
    t->argv = argv;
    t->stackTop = stackTop;
    t->stackSize = stackSize;
    t->priority = priority;
    t->attr = attr;
    memset(guest::Ptr(thread), 0, OSThreadSize);
    guest::Write32(thread + OffTag, OSThreadTag);
    guest::Write8(thread + OffAttr, uint8_t(attr));
    guest::Write32(thread + OffSuspend, 1);
    guest::Write32(thread + OffPriority, uint32_t(priority));
    guest::Write32(thread + OffBasePriority, uint32_t(priority));
    guest::Write32(thread + OffStackStart, stackTop);
    guest::Write32(thread + OffStackEnd, stackTop - stackSize);
    guest::Write32(thread + OffEntry, entry);
    WriteState(*t, 1);

    std::lock_guard lock(gThreadsMutex);
    t->name = "T" + std::to_string(gNextId);
    guest::Write16(thread + OffId, gNextId++);
    PPCContext& c = t->ctx;
    c.r[1].u32 = guest::AlignDown(stackTop, 16) - 16;
    c.r[2].u32 = gSda2Base;
    c.r[13].u32 = gSdaBase;
    gThreads[thread] = std::move(t);
    return true;
}

int32_t OSResumeThread(uint32_t thread)
{
    Thread* t = FindThread(thread);
    if (!t)
        return 0;
    std::unique_lock lock(t->mutex);
    const int32_t previous = t->suspendCount;
    if (t->suspendCount > 0 && --t->suspendCount == 0)
    {
        guest::Write32(thread + OffSuspend, 0);
        if (!t->started)
        {
            lock.unlock();
            Start(*t);
        }
        else
            t->cv.notify_all();
    }
    return previous;
}

int32_t OSSuspendThread(uint32_t thread)
{
    Thread* t = Get(thread);
    if (!t)
        return 0;
    std::unique_lock lock(t->mutex);
    const int32_t previous = t->suspendCount++;
    guest::Write32(t->guest + OffSuspend, uint32_t(t->suspendCount));
    if (t == tThread)
        t->cv.wait(lock, [&] { return t->suspendCount == 0; });
    else
        dtlog::Write("warning: OSSuspendThread on another running thread (%s) is not enforced\n", t->name.c_str());
    return previous;
}

uint32_t OSGetCurrentThread() { return tThread ? tThread->guest : 0; }

void OSExitThread(int32_t value) { throw ThreadExit{ value }; }

bool OSJoinThread(uint32_t thread, uint32_t outValue)
{
    Thread* t = FindThread(thread);
    if (!t)
        return false;
    std::unique_lock lock(t->mutex);
    t->cv.wait(lock, [&] { return t->finished; });
    if (outValue)
        guest::Write32(outValue, uint32_t(t->exitValue));
    return true;
}

void OSDetachThread(uint32_t thread)
{
    if (Thread* t = FindThread(thread))
        t->detached = true;
}

void OSSetThreadName(uint32_t thread, GuestString name)
{
    if (Thread* t = Get(thread))
    {
        guest::Write32(t->guest + OffName, name.addr);
        t->name = name.str();
        if (trace::ThreadState* ts = trace::Current(); ts && t == tThread)
            ts->name = t->name;
    }
}

bool OSSetThreadPriority(uint32_t thread, int32_t priority)
{
    Thread* t = Get(thread);
    if (!t)
        return false;
    t->priority = priority;
    guest::Write32(t->guest + OffPriority, uint32_t(priority));
    return true;
}

int32_t OSGetThreadPriority(uint32_t thread)
{
    Thread* t = Get(thread);
    return t ? t->priority : 0;
}

bool OSSetThreadAffinity(uint32_t thread, uint32_t affinity)
{
    Thread* t = Get(thread);
    if (!t)
        return false;
    t->affinity = affinity;
    guest::Write8(t->guest + OffAttr, uint8_t((guest::Read8(t->guest + OffAttr) & ~7u) | (affinity & 7)));
    return true;
}

void OSSetThreadSpecific(uint32_t id, uint32_t value)
{
    if (tThread && id < 16)
        tThread->specific[id] = value;
}

uint32_t OSGetThreadSpecific(uint32_t id) { return tThread && id < 16 ? tThread->specific[id] : 0; }

void OSYieldThread() { std::this_thread::yield(); }

void OSSleepTicks(int64_t ticks)
{
    if (ticks <= 0)
        return;
    int64_t ns = TicksToNs(ticks);
    // The ErrorViewer thread (ErrorViewerWiiU::Run) sleeps 30 ms a pass and only then empties the ring buffer the
    // rendering thread fills with each frame's GamePad input (FViewportWiiU::ProcessInput): the rendering thread spun
    // waiting for room, which held the game to about 66 fps whatever the cap. Its passes do almost nothing, so they
    // run every millisecond instead.
    if (tThread && tThread->name == "ErrorViewer")
        ns = std::min<int64_t>(ns, 1000000);
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void OSBlockThreadsOnExit() {}
} // namespace

CAFE_IMPORT(coreinit, "OSCreateThread", OSCreateThread);
CAFE_IMPORT(coreinit, "OSResumeThread", OSResumeThread);
CAFE_IMPORT(coreinit, "OSSuspendThread", OSSuspendThread);
CAFE_IMPORT(coreinit, "OSGetCurrentThread", OSGetCurrentThread);
CAFE_IMPORT(coreinit, "OSExitThread", OSExitThread);
CAFE_IMPORT(coreinit, "OSJoinThread", OSJoinThread);
CAFE_IMPORT(coreinit, "OSDetachThread", OSDetachThread);
CAFE_IMPORT(coreinit, "OSSetThreadName", OSSetThreadName);
CAFE_IMPORT(coreinit, "OSSetThreadPriority", OSSetThreadPriority);
CAFE_IMPORT(coreinit, "OSGetThreadPriority", OSGetThreadPriority);
CAFE_IMPORT(coreinit, "OSSetThreadAffinity", OSSetThreadAffinity);
CAFE_IMPORT(coreinit, "OSSetThreadSpecific", OSSetThreadSpecific);
CAFE_IMPORT(coreinit, "OSGetThreadSpecific", OSGetThreadSpecific);
CAFE_IMPORT(coreinit, "OSYieldThread", OSYieldThread);
CAFE_IMPORT(coreinit, "OSSleepTicks", OSSleepTicks);
CAFE_IMPORT(coreinit, "OSBlockThreadsOnExit", OSBlockThreadsOnExit);

Thread* CurrentThread() { return tThread; }

Thread* FindThread(uint32_t guestThread)
{
    std::lock_guard lock(gThreadsMutex);
    auto it = gThreads.find(guestThread);
    return it == gThreads.end() ? nullptr : it->second.get();
}

uint32_t StartSystemThread(const char* name, uint32_t entry, int32_t priority)
{
    constexpr uint32_t StackSize = 256 * 1024;
    const uint32_t thread = guest::SystemAlloc(OSThreadSize, 8);
    const uint32_t stack = guest::SystemAlloc(StackSize, 16);
    OSCreateThread(thread, entry, 0, 0, stack + StackSize, StackSize, priority, 2 /* affinity core 1 */);
    FindThread(thread)->name = name;
    OSResumeThread(thread);
    return thread;
}

Thread& InitMainThread(uint32_t stackSize, uint32_t sdaBase, uint32_t sda2Base)
{
    gSdaBase = sdaBase;
    gSda2Base = sda2Base;
    const uint32_t thread = guest::SystemAlloc(OSThreadSize, 8);
    const uint32_t stack = guest::SystemAlloc(stackSize, 16);
    OSCreateThread(thread, 0, 0, 0, stack + stackSize, stackSize, 16, 2 /* affinity core 1 */);
    Thread* t = FindThread(thread);
    t->name = "main";
    t->started = true;
    t->suspendCount = 0;
    guest::Write32(thread + OffSuspend, 0);
    WriteState(*t, 2);
    tThread = t;
    guest::SetCurrentContext(&t->ctx);
    return *t;
}

void RunMainThread(uint32_t entry, uint32_t argc, uint32_t argv)
{
    Thread& t = *tThread;
    t.entry = entry;
    guest::Write32(t.guest + OffEntry, entry);
    int32_t result;
    try
    {
        result = int32_t(guest::Call(entry, { argc, argv }));
    }
    catch (const ThreadExit& e)
    {
        result = e.value;
    }
    Finish(t, result);
    trace::Stop(0, "main thread returned %d from the entry point", result);
}
} // namespace cafe::coreinit
