#include "queue.h"
#include "../trace.h"

#include "../log.h"
#include "../platform.h"
#include "../settings.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define DT_PAUSE() _mm_pause()
#else
#define DT_PAUSE() std::this_thread::yield()
#endif

namespace gpu::queue
{
namespace
{
bool gAsync = false;

// ---- the command stream: blocks written by the queueing threads (under gProduce), read by the GPU thread ----
constexpr size_t BlockSize = 1 << 20;
struct Block
{
    std::atomic<size_t> published{ 0 };  // bytes of whole commands the GPU thread may run
    std::atomic<Block*> next{ nullptr };  // set once the producers have moved on (after the last publish)
    alignas(16) unsigned char data[BlockSize];
};

std::mutex gProduce;
Block* gTail = nullptr;    // the block being written (gProduce)
size_t gTailUsed = 0;      // bytes committed in it (gProduce)
size_t gReserved = 0;      // the command being written: its size (gProduce)
// Commands are published to the GPU thread 16 at a time (and at every fence and Flush): publishing each one made the
// GPU thread, polling right behind, pull the cache line back on every GX2 call.
constexpr uint32_t PublishEvery = 16;
uint32_t gUnpublished = 0;                // committed commands not yet published (gProduce)
std::atomic<bool> gHasUnpublished{ false };

std::mutex gFreeMutex;
std::vector<Block*> gFree;  // blocks the GPU thread has finished

std::mutex gSleepMutex;
std::condition_variable gWork;
std::atomic<bool> gIdle{ false };  // the GPU thread is asleep (or about to be) waiting for work
Block* gHead = nullptr;            // the GPU thread's block (set before it starts)

std::atomic<uint64_t> gSubmitted{ 0 }, gRetired{ 0 };
std::mutex gRetiredMutex;
std::condition_variable gRetiredChanged;

void SetRetired(uint64_t value)
{
    {
        std::lock_guard lock(gRetiredMutex);
        gRetired = std::max(gRetired.load(), value);
    }
    gRetiredChanged.notify_all();
}

Block* NewBlock()
{
    {
        std::lock_guard lock(gFreeMutex);
        if (!gFree.empty())
        {
            Block* b = gFree.back();
            gFree.pop_back();
            b->published.store(0, std::memory_order_relaxed);
            b->next.store(nullptr, std::memory_order_relaxed);
            return b;
        }
    }
    return new Block;
}

void Wake()
{
    // (seq_cst with the GPU thread's gIdle store and its re-check of the positions: either it sees the new command or
    // this sees it idle)
    if (gIdle.load(std::memory_order_seq_cst))
    {
        { std::lock_guard lock(gSleepMutex); }
        gWork.notify_one();
    }
}

bool HasWork(Block* head, size_t pos)
{
    return head->published.load(std::memory_order_seq_cst) > pos || head->next.load(std::memory_order_seq_cst) != nullptr;
}

void GpuThread()
{
    // (registered as "GPU": named in hang and crash reports, and the sampling profiler can follow it: --sample-threads)
    trace::RegisterThread("GPU", nullptr);
    Block* head = gHead;
    size_t pos = 0;
    for (;;)
    {
        const size_t published = head->published.load(std::memory_order_acquire);
        if (pos < published)
        {
            while (pos < published)
            {
                auto* h = reinterpret_cast<detail::Header*>(head->data + pos);
                void* payload = head->data + pos + detail::PayloadOffset;
                h->run(payload);
                if (h->destroy)
                    h->destroy(payload);
                pos += h->size;
            }
            continue;
        }
        if (Block* next = head->next.load(std::memory_order_acquire))
        {
            if (pos < head->published.load(std::memory_order_acquire))
                continue;  // (the last commands were published before `next`)
            {
                std::lock_guard lock(gFreeMutex);
                gFree.push_back(head);
            }
            head = next, pos = 0;
            continue;
        }
        // Nothing queued: spin briefly (the next command usually follows within microseconds; sleeping and being
        // woken for each costs two system calls), then sleep.
        bool found = false;
        for (int i = 0; i < 4000 && !found; i++)
        {
            DT_PAUSE();
            found = HasWork(head, pos);
        }
        if (found)
            continue;
        std::unique_lock lock(gSleepMutex);
        gIdle.store(true, std::memory_order_seq_cst);
        gWork.wait(lock, [&] { return HasWork(head, pos); });
        gIdle.store(false, std::memory_order_relaxed);
    }
}

// (gProduce held)
void PublishLocked()
{
    gTail->published.store(gTailUsed, std::memory_order_seq_cst);
    gUnpublished = 0;
    gHasUnpublished.store(false, std::memory_order_relaxed);
}

// ---- the rendering thread's staging buffer ----
struct Stage
{
    int decided = 0;  // 1 stages, -1 does not, 0 not decided (the thread may not be named yet)
    std::vector<unsigned char> buf;
    size_t used = 0, reserved = 0;
    uint32_t count = 0;
};
thread_local Stage tStage;
constexpr uint32_t StageEvery = 32;
constexpr size_t StageBytes = 256 * 1024;

bool Stages()
{
    Stage& st = tStage;
    if (st.decided == 0)
    {
        const trace::ThreadState* t = trace::Current();
        if (!t)
            return false;
        if (t->name == "RenderingThread")
            st.decided = 1, st.buf.resize(StageBytes + BlockSize / 4);
        else if (!(t->name.size() > 1 && t->name[0] == 'T' && isdigit(uint8_t(t->name[1]))))  // (named: decided)
            st.decided = -1;
    }
    return st.decided > 0;
}

struct WordsCommand
{
    void (*fn)(bool, uint32_t, const uint32_t*, uint32_t);
    uint32_t offset, count;
    bool pixel;
    // the words follow
};

struct FenceCommand
{
    uint64_t value;
    void operator()() const { SetRetired(value); }
};
} // namespace

namespace detail
{
void* StageReserve(size_t payload)
{
    if (!gAsync || !Stages())
        return nullptr;
    Stage& st = tStage;
    const size_t size = (PayloadOffset + payload + 15) & ~size_t(15);
    if (st.used + size > st.buf.size())
        FlushStage();
    if (size > st.buf.size())
        return nullptr;
    st.reserved = size;
    void* p = st.buf.data() + st.used;
    static_cast<Header*>(p)->size = uint32_t(size);
    return p;
}

void StageCommit()
{
    Stage& st = tStage;
    st.used += st.reserved;
    if (++st.count >= StageEvery || st.used >= StageBytes)
        FlushStage();
}

void FlushStage()
{
    Stage& st = tStage;
    if (!st.used)
        return;
    {
        std::lock_guard lock(gProduce);
        for (size_t pos = 0; pos < st.used;)
        {
            const size_t size = reinterpret_cast<const Header*>(st.buf.data() + pos)->size;
            if (gTailUsed + size > BlockSize)
            {
                PublishLocked();
                Block* b = NewBlock();
                gTail->next.store(b, std::memory_order_seq_cst);
                gTail = b, gTailUsed = 0;
            }
            memcpy(gTail->data + gTailUsed, st.buf.data() + pos, size);
            gTailUsed += size;
            pos += size;
        }
        PublishLocked();
    }
    st.used = 0, st.count = 0;
    Wake();
}

void* Reserve(size_t payload)
{
    const size_t size = (PayloadOffset + payload + 15) & ~size_t(15);
    if (size > BlockSize)
        trace::Stop(9, "GPU command of %zu bytes is larger than a queue block", size);
    gProduce.lock();
    if (gTailUsed + size > BlockSize)
    {
        PublishLocked();  // (the block's last commands before `next`: the GPU thread finishes a block it sees a next for)
        Block* b = NewBlock();
        gTail->next.store(b, std::memory_order_seq_cst);
        gTail = b, gTailUsed = 0;
    }
    gReserved = size;
    void* p = gTail->data + gTailUsed;
    static_cast<Header*>(p)->size = uint32_t(size);
    return p;
}

void Commit(bool publish)
{
    gTailUsed += gReserved;
    if (publish || ++gUnpublished >= PublishEvery)
    {
        PublishLocked();
        gProduce.unlock();
        Wake();
        return;
    }
    gHasUnpublished.store(true, std::memory_order_relaxed);
    gProduce.unlock();
}
} // namespace detail

void Start()
{
    gAsync = settings::GetBool("gpu.async", true);
    dtlog::Important("[gpu] draw processing %s (gpu.async)\n", gAsync ? "on its own thread" : "on the calling thread");
    if (gAsync)
    {
        gHead = gTail = new Block;
        platform::StartThread(GpuThread, 8 << 20);
    }
}

bool Async() { return gAsync; }

void RunWords(void (*fn)(bool, uint32_t, const uint32_t*, uint32_t), bool pixel, uint32_t offset, const uint32_t* words,
    uint32_t count)
{
    if (!gAsync)
    {
        fn(pixel, offset, words, count);
        return;
    }
    const size_t payload = sizeof(WordsCommand) + size_t(count) * 4;
    void* p = detail::StageReserve(payload);
    const bool staged = p != nullptr;
    if (!staged)
    {
        detail::FlushStage();
        p = detail::Reserve(payload);
    }
    auto* h = static_cast<detail::Header*>(p);
    h->run = [](void* self) {
        const auto* c = static_cast<const WordsCommand*>(self);
        c->fn(c->pixel, c->offset, reinterpret_cast<const uint32_t*>(c + 1), c->count);
    };
    h->destroy = nullptr;
    auto* c = reinterpret_cast<WordsCommand*>(static_cast<char*>(p) + detail::PayloadOffset);
    *c = { fn, offset, count, pixel };
    memcpy(c + 1, words, size_t(count) * 4);
    if (staged)
        detail::StageCommit();
    else
        detail::Commit();
}

void RunSynchronous(const std::function<void()>& command)
{
    if (gAsync)
        WaitRetired(Fence());
    command();
}

uint64_t Fence()
{
    if (!gAsync)
    {
        const uint64_t v = ++gSubmitted;
        gRetired = v;
        return v;
    }
    // Numbered under the producers' lock, so markers from different threads are queued in order.
    detail::FlushStage();
    void* p = detail::Reserve(sizeof(FenceCommand));
    const uint64_t v = ++gSubmitted;
    auto* h = static_cast<detail::Header*>(p);
    h->run = [](void* self) { (*static_cast<FenceCommand*>(self))(); };
    h->destroy = nullptr;
    new (static_cast<char*>(p) + detail::PayloadOffset) FenceCommand{ v };
    detail::Commit(true);
    return v;
}

void Flush()
{
    detail::FlushStage();
    if (!gHasUnpublished.load(std::memory_order_relaxed))
        return;
    {
        std::lock_guard lock(gProduce);
        PublishLocked();
    }
    Wake();
}

uint64_t Submitted() { return gSubmitted; }
uint64_t Retired() { return gRetired; }

void WaitRetired(uint64_t value)
{
    value = std::min(value, gSubmitted.load());
    if (gRetired >= value)
        return;
    std::unique_lock lock(gRetiredMutex);
    gRetiredChanged.wait(lock, [value] { return gRetired >= value; });
}
} // namespace gpu::queue
