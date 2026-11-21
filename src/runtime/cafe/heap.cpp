#include "heap.h"

#include "../guest.h"

#include <unordered_map>

namespace cafe
{
namespace
{
std::mutex gHeapsMutex;
std::unordered_map<uint32_t, std::unique_ptr<Heap>> gHeaps;
uint32_t gBaseHeaps[9] = {};
uint32_t gMem2Start = 0;

std::unique_ptr<ExpHeap> gSystemHeap;

uint32_t AbsAlign(int32_t align)
{
    uint32_t a = align < 0 ? uint32_t(-align) : uint32_t(align);
    return a < 4 ? 4 : a;
}
} // namespace

// ---- ExpHeap ----
ExpHeap::ExpHeap(uint32_t handle, uint32_t start, uint32_t end, uint32_t tag) : Heap(tag, handle, start, end)
{
    if (end > start)
        freeRanges[start] = end - start;
}

void ExpHeap::AddFree(uint32_t addr, uint32_t size)
{
    auto next = freeRanges.lower_bound(addr);
    if (next != freeRanges.end() && addr + size == next->first)
    {
        size += next->second;
        next = freeRanges.erase(next);
    }
    if (next != freeRanges.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr)
        {
            prev->second += size;
            return;
        }
    }
    freeRanges[addr] = size;
}

uint32_t ExpHeap::Alloc(uint32_t size, int32_t align)
{
    std::lock_guard lock(mutex);
    const uint32_t a = AbsAlign(align);
    size = guest::AlignUp(size ? size : 1, 4);
    auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t p) {
        const uint32_t rs = it->first, re = it->first + it->second;
        freeRanges.erase(it);
        if (p > rs)
            freeRanges[rs] = p - rs;
        if (p + size < re)
            freeRanges[p + size] = re - (p + size);
        used[p] = size;
        return p;
    };
    if (align >= 0)
    {
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it)
        {
            const uint64_t p = guest::AlignUp(it->first, a);
            if (p + size <= uint64_t(it->first) + it->second)
                return take(it, uint32_t(p));
        }
    }
    else
    {
        for (auto it = freeRanges.rbegin(); it != freeRanges.rend(); ++it)
        {
            const uint32_t re = it->first + it->second;
            if (it->second < size)
                continue;
            const uint32_t p = guest::AlignDown(re - size, a);
            if (p >= it->first)
                return take(std::next(it).base(), p);
        }
    }
    return 0;
}

bool ExpHeap::Free(uint32_t ptr)
{
    std::lock_guard lock(mutex);
    auto it = used.find(ptr);
    if (it == used.end())
        return false;
    AddFree(it->first, it->second);
    used.erase(it);
    return true;
}

uint32_t ExpHeap::AllocatableSize(int32_t align)
{
    std::lock_guard lock(mutex);
    const uint32_t a = AbsAlign(align);
    uint32_t best = 0;
    for (auto& [addr, size] : freeRanges)
    {
        const uint32_t p = guest::AlignUp(addr, a);
        if (p < addr + size && addr + size - p > best)
            best = addr + size - p;
    }
    return best & ~3u;
}

uint32_t ExpHeap::TotalFree()
{
    std::lock_guard lock(mutex);
    uint32_t total = 0;
    for (auto& [addr, size] : freeRanges)
        total += size;
    return total;
}

uint32_t ExpHeap::BlockSize(uint32_t ptr)
{
    std::lock_guard lock(mutex);
    auto it = used.find(ptr);
    return it == used.end() ? 0 : it->second;
}

uint32_t ExpHeap::Adjust()
{
    std::lock_guard lock(mutex);
    uint32_t top = start;
    if (!used.empty())
        top = std::prev(used.end())->first + std::prev(used.end())->second;
    for (auto it = freeRanges.lower_bound(top); it != freeRanges.end();)
        it = freeRanges.erase(it);
    if (!freeRanges.empty())
    {
        auto last = std::prev(freeRanges.end());
        if (last->first + last->second > top)
            last->second = top - last->first;
    }
    end = top;
    return end;
}

// ---- FrmHeap ----
FrmHeap::FrmHeap(uint32_t handle, uint32_t start, uint32_t end) : Heap(TagFrmHeap, handle, start, end), head(start), tail(end) {}

uint32_t FrmHeap::Alloc(uint32_t size, int32_t align)
{
    std::lock_guard lock(mutex);
    const uint32_t a = AbsAlign(align);
    size = guest::AlignUp(size ? size : 1, 4);
    if (align >= 0)
    {
        const uint64_t p = guest::AlignUp(head, a);
        if (p + size > tail)
            return 0;
        head = uint32_t(p + size);
        return uint32_t(p);
    }
    if (tail - head < size)
        return 0;
    const uint32_t p = guest::AlignDown(tail - size, a);
    if (p < head)
        return 0;
    tail = p;
    return p;
}

void FrmHeap::FreeMode(uint32_t mode)
{
    std::lock_guard lock(mutex);
    if (mode & 1)
        head = start;
    if (mode & 2)
        tail = end;
}

uint32_t FrmHeap::AllocatableSize(int32_t align)
{
    std::lock_guard lock(mutex);
    const uint32_t p = guest::AlignUp(head, AbsAlign(align));
    return p < tail ? tail - p : 0;
}

// ---- registry ----
Heap* FindHeap(uint32_t handle)
{
    std::lock_guard lock(gHeapsMutex);
    auto it = gHeaps.find(handle);
    return it == gHeaps.end() ? nullptr : it->second.get();
}

Heap* FindHeapContaining(uint32_t ptr)
{
    std::lock_guard lock(gHeapsMutex);
    Heap* best = nullptr;
    for (auto& [handle, heap] : gHeaps)
        if (heap->Contains(ptr) && (!best || heap->end - heap->start < best->end - best->start))
            best = heap.get();  // innermost heap (heaps can be created inside other heaps' blocks)
    return best;
}

Heap* AddHeap(std::unique_ptr<Heap> heap)
{
    guest::Write32(heap->handle, heap->tag);
    std::lock_guard lock(gHeapsMutex);
    Heap* h = heap.get();
    gHeaps[h->handle] = std::move(heap);
    return h;
}

std::unique_ptr<Heap> RemoveHeap(uint32_t handle)
{
    std::lock_guard lock(gHeapsMutex);
    auto it = gHeaps.find(handle);
    if (it == gHeaps.end())
        return nullptr;
    auto heap = std::move(it->second);
    gHeaps.erase(it);
    guest::Write32(handle, 0);
    return heap;
}

void InitMemory(uint32_t mem2Start)
{
    // Runtime-owned allocations live outside every guest-visible heap.
    gSystemHeap = std::make_unique<ExpHeap>(0, guest::SystemBase, guest::SystemBase + guest::SystemSize);

    gMem2Start = mem2Start;
    auto mem1 = std::make_unique<FrmHeap>(guest::Mem1Base, guest::Mem1Base + HeapHeaderSize, guest::Mem1Base + guest::Mem1Size);
    auto fg = std::make_unique<FrmHeap>(guest::ForegroundBase, guest::ForegroundBase + HeapHeaderSize,
        guest::ForegroundBase + guest::ForegroundSize);
    auto mem2 = std::make_unique<ExpHeap>(mem2Start, mem2Start + HeapHeaderSize, guest::Mem2End);
    gBaseHeaps[0] = AddHeap(std::move(mem1))->handle;
    gBaseHeaps[1] = AddHeap(std::move(mem2))->handle;
    gBaseHeaps[8] = AddHeap(std::move(fg))->handle;
}

uint32_t BaseHeapHandle(uint32_t arena) { return arena < 9 ? gBaseHeaps[arena] : 0; }
void SetBaseHeapHandle(uint32_t arena, uint32_t handle)
{
    if (arena < 9)
        gBaseHeaps[arena] = handle;
}
uint32_t Mem2Start() { return gMem2Start; }
} // namespace cafe

namespace guest
{
uint32_t SystemAlloc(uint32_t size, uint32_t align)
{
    const uint32_t p = cafe::gSystemHeap->Alloc(size, int32_t(align));
    if (p)
        memset(Ptr(p), 0, size);
    return p;
}

void SystemFree(uint32_t addr) { cafe::gSystemHeap->Free(addr); }
} // namespace guest
