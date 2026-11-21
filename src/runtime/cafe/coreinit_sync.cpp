// coreinit: synchronisation primitives, backed by host objects keyed by the guest object's address.
#include "../imports.h"
#include "../log.h"
#include "coreinit.h"

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>

namespace cafe::coreinit
{
namespace
{
// Host object for a guest address. Init* calls replace it; use before init creates it on demand.
template <class T>
class Objects
{
public:
    T& Get(uint32_t addr)
    {
        std::lock_guard lock(mutex);
        auto& p = map[addr];
        if (!p)
            p = std::make_unique<T>();
        return *p;
    }
    T& Reset(uint32_t addr)
    {
        std::lock_guard lock(mutex);
        auto& p = map[addr];
        p = std::make_unique<T>();  // old object leaks if someone still waits on it, as on hardware
        return *p;
    }

private:
    std::mutex mutex;
    std::unordered_map<uint32_t, std::unique_ptr<T>> map;
};

// ---- mutexes (OSMutex, OSFastMutex, GHS locks): recursive, owned by a guest thread ----
struct Mutex
{
    std::mutex m;
    std::condition_variable cv;
    const void* owner = nullptr;
    int count = 0;
};

const void* Self()
{
    static thread_local char hostThreadMarker;  // identity for threads without an OSThread
    Thread* t = CurrentThread();
    return t ? static_cast<const void*>(t) : static_cast<const void*>(&hostThreadMarker);
}

void Lock(Mutex& mx)
{
    const void* self = Self();
    std::unique_lock l(mx.m);
    mx.cv.wait(l, [&] { return !mx.owner || mx.owner == self; });
    mx.owner = self;
    mx.count++;
}

bool TryLock(Mutex& mx)
{
    const void* self = Self();
    std::lock_guard l(mx.m);
    if (mx.owner && mx.owner != self)
        return false;
    mx.owner = self;
    mx.count++;
    return true;
}

void Unlock(Mutex& mx)
{
    std::lock_guard l(mx.m);
    if (mx.owner != Self())
    {
        dtlog::Write("warning: unlocking a mutex not owned by this thread\n");
        return;
    }
    if (--mx.count == 0)
    {
        mx.owner = nullptr;
        mx.cv.notify_all();
    }
}

// ---- condition variables: OSSignalCond wakes every waiter ----
struct Cond
{
    std::condition_variable cv;
    Mutex* mutex = nullptr;
    uint64_t generation = 0;
};

void WaitCond(Cond& c, Mutex& mx)
{
    const void* self = Self();
    std::unique_lock l(mx.m);
    c.mutex = &mx;
    const int saved = mx.count;
    mx.owner = nullptr;
    mx.count = 0;
    mx.cv.notify_all();
    const uint64_t gen = c.generation;
    c.cv.wait(l, [&] { return c.generation != gen; });
    mx.cv.wait(l, [&] { return !mx.owner; });
    mx.owner = self;
    mx.count = saved;
}

void SignalCond(Cond& c)
{
    if (Mutex* mx = c.mutex)
    {
        std::lock_guard l(mx->m);
        c.generation++;
        c.cv.notify_all();
    }
}

Objects<Mutex> gMutexes, gFastMutexes, gGhsMutexes;
Objects<Cond> gConds, gFastConds;
std::recursive_mutex gGhsGlobalLock;

void OSInitMutex(uint32_t m) { gMutexes.Reset(m); }
void OSLockMutex(uint32_t m) { Lock(gMutexes.Get(m)); }
bool OSTryLockMutex(uint32_t m) { return TryLock(gMutexes.Get(m)); }
void OSUnlockMutex(uint32_t m) { Unlock(gMutexes.Get(m)); }
void OSInitCond(uint32_t c) { gConds.Reset(c); }
void OSWaitCond(uint32_t c, uint32_t m) { WaitCond(gConds.Get(c), gMutexes.Get(m)); }
void OSSignalCond(uint32_t c) { SignalCond(gConds.Get(c)); }

void OSFastMutex_Init(uint32_t m, uint32_t name) { (void)name; gFastMutexes.Reset(m); }
void OSFastMutex_Lock(uint32_t m) { Lock(gFastMutexes.Get(m)); }
bool OSFastMutex_TryLock(uint32_t m) { return TryLock(gFastMutexes.Get(m)); }
void OSFastMutex_Unlock(uint32_t m) { Unlock(gFastMutexes.Get(m)); }
void OSFastCond_Init(uint32_t c, uint32_t name) { (void)name; gFastConds.Reset(c); }
void OSFastCond_Wait(uint32_t c, uint32_t m) { WaitCond(gFastConds.Get(c), gFastMutexes.Get(m)); }
void OSFastCond_Signal(uint32_t c) { SignalCond(gFastConds.Get(c)); }

// ---- events ----
struct Event
{
    std::mutex m;
    std::condition_variable cv;
    bool value = false;
    bool autoReset = false;
    uint64_t broadcast = 0;  // OSSignalEventAll generation
};
Objects<Event> gEvents;

void OSInitEvent(uint32_t e, bool value, uint32_t mode)
{
    Event& ev = gEvents.Reset(e);
    ev.value = value;
    ev.autoReset = mode == 1;  // OS_EVENT_MODE_MANUAL = 0, OS_EVENT_MODE_AUTO = 1
}

void OSSignalEvent(uint32_t e)
{
    Event& ev = gEvents.Get(e);
    std::lock_guard l(ev.m);
    ev.value = true;
    if (ev.autoReset)
        ev.cv.notify_one();
    else
        ev.cv.notify_all();
}

void OSSignalEventAll(uint32_t e)
{
    Event& ev = gEvents.Get(e);
    std::lock_guard l(ev.m);
    if (ev.autoReset)
        ev.broadcast++;
    else
        ev.value = true;
    ev.cv.notify_all();
}

void OSResetEvent(uint32_t e)
{
    Event& ev = gEvents.Get(e);
    std::lock_guard l(ev.m);
    ev.value = false;
}

template <class Wait>
bool WaitEvent(Event& ev, Wait wait)
{
    std::unique_lock l(ev.m);
    const uint64_t gen = ev.broadcast;
    if (!wait(l, [&] { return ev.value || ev.broadcast != gen; }))
        return false;
    if (ev.autoReset && ev.value && ev.broadcast == gen)
        ev.value = false;
    return true;
}

void OSWaitEvent(uint32_t e)
{
    Event& ev = gEvents.Get(e);
    WaitEvent(ev, [&](auto& l, auto pred) { ev.cv.wait(l, pred); return true; });
}

bool OSWaitEventWithTimeout(uint32_t e, int64_t timeoutNs)
{
    Event& ev = gEvents.Get(e);
    return WaitEvent(ev, [&](auto& l, auto pred) { return ev.cv.wait_for(l, std::chrono::nanoseconds(timeoutNs), pred); });
}

// ---- semaphores ----
struct Semaphore
{
    std::mutex m;
    std::condition_variable cv;
    int32_t count = 0;
};
Objects<Semaphore> gSemaphores;

void OSInitSemaphore(uint32_t s, int32_t count) { gSemaphores.Reset(s).count = count; }

int32_t OSWaitSemaphore(uint32_t s)
{
    Semaphore& sem = gSemaphores.Get(s);
    std::unique_lock l(sem.m);
    sem.cv.wait(l, [&] { return sem.count > 0; });
    return sem.count--;
}

int32_t OSTryWaitSemaphore(uint32_t s)
{
    Semaphore& sem = gSemaphores.Get(s);
    std::lock_guard l(sem.m);
    const int32_t previous = sem.count;
    if (sem.count > 0)
        sem.count--;
    return previous;
}

int32_t OSSignalSemaphore(uint32_t s)
{
    Semaphore& sem = gSemaphores.Get(s);
    std::lock_guard l(sem.m);
    sem.cv.notify_one();
    return sem.count++;
}

// ---- message queues: 16-byte messages ----
struct MessageQueue
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::array<uint8_t, 16>> messages;
    uint32_t capacity = 0;
};
Objects<MessageQueue> gQueues;
uint32_t gSystemQueue = 0;

void OSInitMessageQueue(uint32_t q, uint32_t messages, int32_t count)
{
    (void)messages;
    gQueues.Reset(q).capacity = count > 0 ? uint32_t(count) : 0;
}

bool OSSendMessage(uint32_t q, uint32_t msg, uint32_t flags)
{
    MessageQueue& mq = gQueues.Get(q);
    std::unique_lock l(mq.m);
    if (mq.messages.size() >= mq.capacity)
    {
        if (!(flags & 1))
            return false;
        mq.cv.wait(l, [&] { return mq.messages.size() < mq.capacity; });
    }
    std::array<uint8_t, 16> m;
    memcpy(m.data(), guest::Ptr(msg), 16);
    mq.messages.push_back(m);
    mq.cv.notify_all();
    return true;
}

bool OSReceiveMessage(uint32_t q, uint32_t msg, uint32_t flags)
{
    MessageQueue& mq = gQueues.Get(q);
    std::unique_lock l(mq.m);
    if (mq.messages.empty())
    {
        if (!(flags & 1))
            return false;
        mq.cv.wait(l, [&] { return !mq.messages.empty(); });
    }
    memcpy(guest::Ptr(msg), mq.messages.front().data(), 16);
    mq.messages.pop_front();
    mq.cv.notify_all();
    return true;
}

uint32_t OSGetSystemMessageQueue()
{
    if (!gSystemQueue)
    {
        gSystemQueue = guest::SystemAlloc(0x40);
        gQueues.Reset(gSystemQueue).capacity = 16;
    }
    return gSystemQueue;
}

// ---- interrupts: no host equivalent; report "were enabled" so callers restore cleanly ----
uint32_t OSDisableInterrupts() { return 1; }
uint32_t OSRestoreInterrupts(uint32_t state) { (void)state; return 0; }

// ---- GHS C library locks ----
void GhsLock() { gGhsGlobalLock.lock(); }
void GhsUnlock() { gGhsGlobalLock.unlock(); }
void GhsMtxInit(uint32_t mtx) { gGhsMutexes.Reset(mtx); }
void GhsMtxDestroy(uint32_t mtx) { (void)mtx; }
void GhsMtxLock(uint32_t mtx) { Lock(gGhsMutexes.Get(mtx)); }
void GhsMtxUnlock(uint32_t mtx) { Unlock(gGhsMutexes.Get(mtx)); }

// Per-FILE stdio locks. flockfile(f) does: p = __ghs_flock_ptr(f); if (p && *p) __ghs_flock_file(*p).
// So __ghs_flock_ptr returns the address of a word holding the FILE's lock handle; handles are host mutexes.
std::mutex gFlockMutex;
std::unordered_map<uint32_t, uint32_t> gFlockWords;  // FILE* -> guest word holding its lock handle
Objects<Mutex> gFileLocks;
uint32_t gNextFlockHandle = 1;

uint32_t GhsFlockPtr(uint32_t file)
{
    std::lock_guard lock(gFlockMutex);
    uint32_t& word = gFlockWords[file];
    if (!word)
    {
        word = guest::SystemAlloc(4);
        guest::Write32(word, gNextFlockHandle++);
    }
    return word;
}
void GhsFlockCreate(uint32_t handlePtr)
{
    std::lock_guard lock(gFlockMutex);
    if (handlePtr)
        guest::Write32(handlePtr, gNextFlockHandle++);
}
void GhsFlockDestroy(uint32_t handle) { (void)handle; }
void GhsFlockFile(uint32_t handle) { Lock(gFileLocks.Get(handle)); }
void GhsFunlockFile(uint32_t handle) { Unlock(gFileLocks.Get(handle)); }
} // namespace

bool SendMessage(uint32_t queue, const uint32_t words[4], bool block)
{
    MessageQueue& mq = gQueues.Get(queue);
    std::unique_lock l(mq.m);
    if (mq.messages.size() >= mq.capacity)
    {
        if (!block)
            return false;
        mq.cv.wait(l, [&] { return mq.messages.size() < mq.capacity; });
    }
    std::array<uint8_t, 16> m;
    for (int i = 0; i < 4; i++)
    {
        const uint32_t be = __builtin_bswap32(words[i]);
        memcpy(m.data() + i * 4, &be, 4);
    }
    mq.messages.push_back(m);
    mq.cv.notify_all();
    return true;
}

CAFE_IMPORT(coreinit, "OSInitMutex", OSInitMutex);
CAFE_IMPORT(coreinit, "OSLockMutex", OSLockMutex);
CAFE_IMPORT(coreinit, "OSTryLockMutex", OSTryLockMutex);
CAFE_IMPORT(coreinit, "OSUnlockMutex", OSUnlockMutex);
CAFE_IMPORT(coreinit, "OSInitCond", OSInitCond);
CAFE_IMPORT(coreinit, "OSWaitCond", OSWaitCond);
CAFE_IMPORT(coreinit, "OSSignalCond", OSSignalCond);
CAFE_IMPORT(coreinit, "OSFastMutex_Init", OSFastMutex_Init);
CAFE_IMPORT(coreinit, "OSFastMutex_Lock", OSFastMutex_Lock);
CAFE_IMPORT(coreinit, "OSFastMutex_TryLock", OSFastMutex_TryLock);
CAFE_IMPORT(coreinit, "OSFastMutex_Unlock", OSFastMutex_Unlock);
CAFE_IMPORT(coreinit, "OSFastCond_Init", OSFastCond_Init);
CAFE_IMPORT(coreinit, "OSFastCond_Wait", OSFastCond_Wait);
CAFE_IMPORT(coreinit, "OSFastCond_Signal", OSFastCond_Signal);
CAFE_IMPORT(coreinit, "OSInitEvent", OSInitEvent);
CAFE_IMPORT(coreinit, "OSSignalEvent", OSSignalEvent);
CAFE_IMPORT(coreinit, "OSSignalEventAll", OSSignalEventAll);
CAFE_IMPORT(coreinit, "OSResetEvent", OSResetEvent);
CAFE_IMPORT(coreinit, "OSWaitEvent", OSWaitEvent);
CAFE_IMPORT(coreinit, "OSWaitEventWithTimeout", OSWaitEventWithTimeout);
CAFE_IMPORT(coreinit, "OSInitSemaphore", OSInitSemaphore);
CAFE_IMPORT(coreinit, "OSWaitSemaphore", OSWaitSemaphore);
CAFE_IMPORT(coreinit, "OSTryWaitSemaphore", OSTryWaitSemaphore);
CAFE_IMPORT(coreinit, "OSSignalSemaphore", OSSignalSemaphore);
CAFE_IMPORT(coreinit, "OSInitMessageQueue", OSInitMessageQueue);
CAFE_IMPORT(coreinit, "OSSendMessage", OSSendMessage);
CAFE_IMPORT(coreinit, "OSReceiveMessage", OSReceiveMessage);
CAFE_IMPORT(coreinit, "OSGetSystemMessageQueue", OSGetSystemMessageQueue);
CAFE_IMPORT(coreinit, "OSDisableInterrupts", OSDisableInterrupts);
CAFE_IMPORT(coreinit, "OSRestoreInterrupts", OSRestoreInterrupts);
CAFE_IMPORT(coreinit, "__ghsLock", GhsLock);
CAFE_IMPORT(coreinit, "__ghsUnlock", GhsUnlock);
CAFE_IMPORT(coreinit, "__ghs_mtx_init", GhsMtxInit);
CAFE_IMPORT(coreinit, "__ghs_mtx_dst", GhsMtxDestroy);
CAFE_IMPORT(coreinit, "__ghs_mtx_lock", GhsMtxLock);
CAFE_IMPORT(coreinit, "__ghs_mtx_unlock", GhsMtxUnlock);
CAFE_IMPORT(coreinit, "__ghs_flock_ptr", GhsFlockPtr);
CAFE_IMPORT(coreinit, "__ghs_flock_create", GhsFlockCreate);
CAFE_IMPORT(coreinit, "__ghs_flock_destroy", GhsFlockDestroy);
CAFE_IMPORT(coreinit, "__ghs_flock_file", GhsFlockFile);
CAFE_IMPORT(coreinit, "__ghs_funlock_file", GhsFunlockFile);
} // namespace cafe::coreinit
