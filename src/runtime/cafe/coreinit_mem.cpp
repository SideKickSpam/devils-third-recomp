// coreinit: heaps (MEM*), memory bounds, block copies and data-cache maintenance.
#include "../gpu/dump.h"
#include "../imports.h"
#include "../log.h"
#include "coreinit.h"
#include "heap.h"

#include <cstring>
#include <unordered_map>

namespace cafe::coreinit
{
namespace
{
template <class T>
T* As(uint32_t handle)
{
    return dynamic_cast<T*>(FindHeap(handle));
}

// ---- expanded heap ----
uint32_t MEMCreateExpHeapEx(uint32_t addr, uint32_t size, uint16_t flags)
{
    (void)flags;
    const uint32_t handle = guest::AlignUp(addr, 4);
    const uint32_t end = guest::AlignDown(addr + size, 4);
    if (end <= handle + HeapHeaderSize)
        return 0;
    return AddHeap(std::make_unique<ExpHeap>(handle, handle + HeapHeaderSize, end))->handle;
}

uint32_t MEMDestroyExpHeap(uint32_t heap)
{
    auto h = RemoveHeap(heap);
    return h ? heap : 0;
}

uint32_t MEMAllocFromExpHeapEx(uint32_t heap, uint32_t size, int32_t align)
{
    Heap* h = FindHeap(heap);
    return h ? h->Alloc(size, align) : 0;
}

void MEMFreeToExpHeap(uint32_t heap, uint32_t ptr)
{
    if (Heap* h = FindHeap(heap); h && ptr)
        h->Free(ptr);
}

uint32_t MEMGetAllocatableSizeForExpHeapEx(uint32_t heap, int32_t align)
{
    Heap* h = FindHeap(heap);
    return h ? h->AllocatableSize(align) : 0;
}

uint32_t MEMGetSizeForMBlockExpHeap(uint32_t ptr)
{
    Heap* h = FindHeapContaining(ptr);
    return h ? h->BlockSize(ptr) : 0;
}

uint32_t MEMAdjustExpHeap(uint32_t heap)
{
    auto* h = As<ExpHeap>(heap);
    return h ? h->Adjust() - heap : 0;
}

// ---- frame heap ----
uint32_t MEMAllocFromFrmHeapEx(uint32_t heap, uint32_t size, int32_t align)
{
    Heap* h = FindHeap(heap);
    return h ? h->Alloc(size, align) : 0;
}

void MEMFreeToFrmHeap(uint32_t heap, uint32_t mode)
{
    if (auto* h = As<FrmHeap>(heap))
        h->FreeMode(mode);
}

uint32_t MEMGetAllocatableSizeForFrmHeapEx(uint32_t heap, int32_t align)
{
    Heap* h = FindHeap(heap);
    return h ? h->AllocatableSize(align) : 0;
}

// ---- block heap (tracked like an expanded heap over [start, end)) ----
uint32_t MEMInitBlockHeap(uint32_t heap, uint32_t start, uint32_t end, uint32_t blocks, uint32_t blocksSize, uint32_t flags)
{
    (void)blocks, (void)blocksSize, (void)flags;
    if (!heap || end <= start)
        return 0;
    return AddHeap(std::make_unique<ExpHeap>(heap, start, end, TagBlockHeap))->handle;
}

uint32_t MEMAllocFromBlockHeapEx(uint32_t heap, uint32_t size, int32_t align)
{
    Heap* h = FindHeap(heap);
    return h ? h->Alloc(size, align) : 0;
}

int32_t MEMFreeToBlockHeap(uint32_t heap, uint32_t ptr)
{
    Heap* h = FindHeap(heap);
    return h && h->Free(ptr) ? 0 : -1;
}

uint32_t MEMGetTotalFreeSizeForBlockHeap(uint32_t heap)
{
    Heap* h = FindHeap(heap);
    return h ? h->TotalFree() : 0;
}

// ---- base heaps and bounds ----
uint32_t MEMGetBaseHeapHandle(uint32_t arena) { return BaseHeapHandle(arena); }

void MEMDumpHeap(uint32_t heap)
{
    if (Heap* h = FindHeap(heap))
        dtlog::Write("MEMDumpHeap 0x%08X: [0x%08X, 0x%08X), %u bytes free\n", heap, h->start, h->end, h->TotalFree());
}

// type 1: MEM1, type 2: MEM2.
int32_t OSGetMemBound(uint32_t type, uint32_t outAddr, uint32_t outSize)
{
    uint32_t addr, size;
    if (type == 1)
        addr = guest::Mem1Base, size = guest::Mem1Size;
    else if (type == 2)
        addr = Mem2Start(), size = guest::Mem2End - Mem2Start();
    else
        return -1;
    if (outAddr)
        guest::Write32(outAddr, addr);
    if (outSize)
        guest::Write32(outSize, size);
    return 0;
}

// ---- default heap hooks (coreinit data imports holding function pointers) ----
uint32_t DefaultAlloc(uint32_t size) { return FindHeap(BaseHeapHandle(1))->Alloc(size, 0x40); }
uint32_t DefaultAllocEx(uint32_t size, int32_t align) { return FindHeap(BaseHeapHandle(1))->Alloc(size, align); }
void DefaultFree(uint32_t ptr)
{
    if (!ptr)
        return;
    if (Heap* h = FindHeapContaining(ptr))
        h->Free(ptr);
}

// ---- block copies and cache maintenance ----
uint32_t Memcpy(uint32_t dst, uint32_t src, uint32_t size)
{
    gpu::dump::Copy(dst, size);
    memmove(guest::Ptr(dst), guest::Ptr(src), size);  // memmove: tolerate overlap from sloppy callers
    return dst;
}
uint32_t Memset(uint32_t dst, int32_t value, uint32_t size)
{
    memset(guest::Ptr(dst), value, size);
    return dst;
}
uint32_t OSBlockMove(uint32_t dst, uint32_t src, uint32_t size, int32_t flush)
{
    (void)flush;
    gpu::dump::Copy(dst, size);
    memmove(guest::Ptr(dst), guest::Ptr(src), size);
    return dst;
}
uint32_t OSBlockSet(uint32_t dst, int32_t value, uint32_t size)
{
    memset(guest::Ptr(dst), value, size);
    return dst;
}
void DCRange(uint32_t addr, uint32_t size) { (void)addr, (void)size; }  // caches are coherent on the host
void DCZeroRange(uint32_t addr, uint32_t size) { memset(guest::Ptr(guest::AlignDown(addr, 32)), 0, guest::AlignUp(size, 32)); }
} // namespace

CAFE_IMPORT(coreinit, "MEMCreateExpHeapEx", MEMCreateExpHeapEx);
CAFE_IMPORT(coreinit, "MEMDestroyExpHeap", MEMDestroyExpHeap);
CAFE_IMPORT(coreinit, "MEMAllocFromExpHeapEx", MEMAllocFromExpHeapEx);
CAFE_IMPORT(coreinit, "MEMFreeToExpHeap", MEMFreeToExpHeap);
CAFE_IMPORT(coreinit, "MEMGetAllocatableSizeForExpHeapEx", MEMGetAllocatableSizeForExpHeapEx);
CAFE_IMPORT(coreinit, "MEMGetSizeForMBlockExpHeap", MEMGetSizeForMBlockExpHeap);
CAFE_IMPORT(coreinit, "MEMAdjustExpHeap", MEMAdjustExpHeap);
CAFE_IMPORT(coreinit, "MEMAllocFromFrmHeapEx", MEMAllocFromFrmHeapEx);
CAFE_IMPORT(coreinit, "MEMFreeToFrmHeap", MEMFreeToFrmHeap);
CAFE_IMPORT(coreinit, "MEMGetAllocatableSizeForFrmHeapEx", MEMGetAllocatableSizeForFrmHeapEx);
CAFE_IMPORT(coreinit, "MEMInitBlockHeap", MEMInitBlockHeap);
CAFE_IMPORT(coreinit, "MEMAllocFromBlockHeapEx", MEMAllocFromBlockHeapEx);
CAFE_IMPORT(coreinit, "MEMFreeToBlockHeap", MEMFreeToBlockHeap);
CAFE_IMPORT(coreinit, "MEMGetTotalFreeSizeForBlockHeap", MEMGetTotalFreeSizeForBlockHeap);
CAFE_IMPORT(coreinit, "MEMGetBaseHeapHandle", MEMGetBaseHeapHandle);
CAFE_IMPORT(coreinit, "MEMDumpHeap", MEMDumpHeap);
CAFE_IMPORT(coreinit, "OSGetMemBound", OSGetMemBound);
CAFE_IMPORT(coreinit, "memcpy", Memcpy);
CAFE_IMPORT(coreinit, "memmove", Memcpy);
CAFE_IMPORT(coreinit, "memset", Memset);
CAFE_IMPORT(coreinit, "OSBlockMove", OSBlockMove);
CAFE_IMPORT(coreinit, "OSBlockSet", OSBlockSet);
CAFE_IMPORT(coreinit, "DCFlushRange", DCRange);
CAFE_IMPORT(coreinit, "DCFlushRangeNoSync", DCRange);
CAFE_IMPORT(coreinit, "DCInvalidateRange", DCRange);
CAFE_IMPORT(coreinit, "DCStoreRange", DCRange);
CAFE_IMPORT(coreinit, "DCZeroRange", DCZeroRange);

void InitDataImports()
{
    // Values for coreinit's exported variables. The MEM* ones are function pointers the game calls (and may
    // replace); the C runtime hooks (__atexit_cleanup, ...) start out null and are filled in by the game.
    constexpr uint32_t FopenMax = 20;  // GHS default FOPEN_MAX; _iob gets one 64 KB region, ample for 20 FILEs
    const std::unordered_map<std::string, uint32_t> values = {
        { "MEMAllocFromDefaultHeap", HostFunction<&DefaultAlloc>("coreinit", "MEMAllocFromDefaultHeap") },
        { "MEMAllocFromDefaultHeapEx", HostFunction<&DefaultAllocEx>("coreinit", "MEMAllocFromDefaultHeapEx") },
        { "MEMFreeToDefaultHeap", HostFunction<&DefaultFree>("coreinit", "MEMFreeToDefaultHeap") },
        { "__gh_FOPEN_MAX", FopenMax },
    };
    for (const PPCDataImport* d = PPCDataImports; d->name; d++)
    {
        if (auto it = values.find(d->name); it != values.end())
            guest::Write32(d->addr, it->second);
        else if (std::string_view(d->name) == "environ")
            guest::Write32(d->addr, d->addr + 8);  // points at an empty, NULL-terminated array right after it
    }
}
} // namespace cafe::coreinit

// LC: Espresso's locked cache, a per-core scratchpad filled by DMA. Memory here is flat and coherent, so
// locked-cache buffers are ordinary system allocations and DMA is a synchronous copy of 32-byte blocks.
namespace cafe::coreinit
{
namespace
{
uint32_t LCAlloc(uint32_t size) { return guest::SystemAlloc(guest::AlignUp(size ? size : 512, 512), 512); }
void LCDealloc(uint32_t addr)
{
    if (addr)
        guest::SystemFree(addr);
}
bool LCEnableDMA() { return true; }
void LCDisableDMA() {}
uint32_t DmaBytes(uint32_t blocks) { return (blocks ? blocks : 128) * 32; }  // 0 means 128 blocks
void LCLoadDMABlocks(uint32_t lcDst, uint32_t memSrc, uint32_t blocks) { memmove(guest::Ptr(lcDst), guest::Ptr(memSrc), DmaBytes(blocks)); }
void LCStoreDMABlocks(uint32_t memDst, uint32_t lcSrc, uint32_t blocks) { memmove(guest::Ptr(memDst), guest::Ptr(lcSrc), DmaBytes(blocks)); }
void LCWaitDMAQueue(uint32_t length) { (void)length; }
} // namespace

CAFE_IMPORT(coreinit, "LCAlloc", LCAlloc);
CAFE_IMPORT(coreinit, "LCDealloc", LCDealloc);
CAFE_IMPORT(coreinit, "LCEnableDMA", LCEnableDMA);
CAFE_IMPORT(coreinit, "LCDisableDMA", LCDisableDMA);
CAFE_IMPORT(coreinit, "LCLoadDMABlocks", LCLoadDMABlocks);
CAFE_IMPORT(coreinit, "LCStoreDMABlocks", LCStoreDMABlocks);
CAFE_IMPORT(coreinit, "LCWaitDMAQueue", LCWaitDMAQueue);
} // namespace cafe::coreinit
