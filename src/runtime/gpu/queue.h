// The GPU thread: GX2 calls become commands that one host thread executes in order, as the Wii U's GPU consumes its
// command buffer while the CPU goes on. The game's rendering thread then only queues work, and the GX2 -> Vulkan
// translation (draw processing) runs in parallel with it.
//
// GX2 timestamps follow the commands: Fence() is GX2Flush (a marker in the queue), Retired() the last marker the GPU
// thread has passed. The game (UE3's RHI fences, texture locks) waits on them before reusing memory a queued command
// may still read, exactly as it waits for the real GPU.
//
// Commands are written in place into 1 MB blocks (a header, then the callable or the data), so queueing one costs a
// short lock taken only by the threads that queue and no heap allocation; the GPU thread reads them through atomic
// positions without taking the lock. (A std::function per command, a std::vector per uniform call and one mutex shared
// with the GPU thread had cost the rendering thread about 2 ms a frame in level 2's heavy areas.)
//
// gpu.async = false (setting) runs every command at once on the calling thread instead, as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace gpu::queue
{
// Start the GPU thread (if gpu.async is on). Call after the renderer is initialised.
void Start();
bool Async();

namespace detail
{
// A command in a block: this header, then its payload at the next 16-byte boundary.
struct Header
{
    void (*run)(void* payload);
    void (*destroy)(void* payload);  // null if there is nothing to destroy
    uint32_t size;                   // header and payload, rounded to 16 bytes
};
constexpr size_t PayloadOffset = (sizeof(Header) + 15) & ~size_t(15);
// Room for a command of `payload` bytes, with the producers' lock held until Commit().
void* Reserve(size_t payload);
void Commit(bool publish = false);
// The rendering thread (nearly every GX2 call) stages commands in its own buffer and hands them over 32 at a time
// under one lock (a lock per call was 5% of that thread). Null on other threads: they use Reserve.
void* StageReserve(size_t payload);
void StageCommit();
void FlushStage();  // hand this thread's staged commands over (before anything queued another way)

template <class F>
void Emplace(F&& f)
{
    using T = std::decay_t<F>;
    static_assert(alignof(T) <= 16);
    if constexpr (std::is_trivially_copyable_v<T>)
    {
        if (void* p = StageReserve(sizeof(T)))
        {
            Header* h = static_cast<Header*>(p);
            h->run = [](void* self) { (*static_cast<T*>(self))(); };
            h->destroy = nullptr;
            new (static_cast<char*>(p) + PayloadOffset) T(std::forward<F>(f));
            StageCommit();
            return;
        }
    }
    FlushStage();
    void* p = Reserve(sizeof(T));
    Header* h = static_cast<Header*>(p);
    h->run = [](void* self) { (*static_cast<T*>(self))(); };
    h->destroy = std::is_trivially_destructible_v<T> ? nullptr : +[](void* self) { static_cast<T*>(self)->~T(); };
    new (static_cast<char*>(p) + PayloadOffset) T(std::forward<F>(f));
    Commit();
}
} // namespace detail

// Queue a command (or run it now when not async).
template <class F>
void Run(F&& command)
{
    if (!Async())
    {
        command();
        return;
    }
    detail::Emplace(std::forward<F>(command));
}

// Queue a call fn(pixel, offset, words, count) with `count` words copied from `words` now, as they are (uniform
// registers: the game's big-endian words; no allocation per call).
void RunWords(void (*fn)(bool pixel, uint32_t offset, const uint32_t* words, uint32_t count), bool pixel,
    uint32_t offset, const uint32_t* words, uint32_t count);

// Run a command after everything queued before it has been executed, on the calling thread (for the rare calls that
// read guest memory the game may change as soon as the call returns).
void RunSynchronous(const std::function<void()>& command);

// Make every queued command visible to the GPU thread (commands are published in batches). The import dispatcher calls
// it before every call that may block or read GPU results, so nothing waits on a command still unpublished.
void Flush();

// GX2 timestamps.
uint64_t Fence();      // queue a marker; returns its value (GX2GetLastSubmittedTimeStamp afterwards)
uint64_t Submitted();  // the last marker queued
uint64_t Retired();    // the last marker executed
void WaitRetired(uint64_t value);
} // namespace gpu::queue
