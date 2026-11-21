// Cafe OS heap allocators (MEM expanded / frame / block heaps), managing guest memory.
// Bookkeeping is kept on the host, keyed by the heap handle; the handle points at a small header in guest
// memory carrying the heap's tag, as on hardware.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace cafe
{
constexpr uint32_t TagExpHeap = 0x45585048;    // 'EXPH'
constexpr uint32_t TagFrmHeap = 0x46524D48;    // 'FRMH'
constexpr uint32_t TagBlockHeap = 0x424C4B48;  // 'BLKH'
constexpr uint32_t HeapHeaderSize = 0x80;

class Heap
{
public:
    Heap(uint32_t tag, uint32_t handle, uint32_t start, uint32_t end) : tag(tag), handle(handle), start(start), end(end) {}
    virtual ~Heap() = default;
    virtual uint32_t Alloc(uint32_t size, int32_t align) = 0;
    virtual bool Free(uint32_t ptr) = 0;
    virtual uint32_t AllocatableSize(int32_t align) = 0;
    virtual uint32_t TotalFree() = 0;
    virtual uint32_t BlockSize(uint32_t ptr) { (void)ptr; return 0; }
    bool Contains(uint32_t p) const { return p >= start && p < end; }

    const uint32_t tag, handle;
    uint32_t start, end;
    std::recursive_mutex mutex;
};

// First-fit from the bottom for positive alignment, from the top for negative alignment.
class ExpHeap : public Heap
{
public:
    ExpHeap(uint32_t handle, uint32_t start, uint32_t end, uint32_t tag = TagExpHeap);
    uint32_t Alloc(uint32_t size, int32_t align) override;
    bool Free(uint32_t ptr) override;
    uint32_t AllocatableSize(int32_t align) override;
    uint32_t TotalFree() override;
    uint32_t BlockSize(uint32_t ptr) override;
    uint32_t Adjust();  // shrink the heap to its highest used block; returns the new end

private:
    void AddFree(uint32_t addr, uint32_t size);
    std::map<uint32_t, uint32_t> freeRanges;  // addr -> size
    std::map<uint32_t, uint32_t> used;        // addr -> size
};

// Bump allocation from the bottom (head) or the top (tail); freed only wholesale.
class FrmHeap : public Heap
{
public:
    FrmHeap(uint32_t handle, uint32_t start, uint32_t end);
    uint32_t Alloc(uint32_t size, int32_t align) override;
    bool Free(uint32_t ptr) override { (void)ptr; return false; }
    void FreeMode(uint32_t mode);  // bit 0: reset head, bit 1: reset tail
    uint32_t AllocatableSize(int32_t align) override;
    uint32_t TotalFree() override { return tail - head; }

    uint32_t head, tail;
};

// Heap registry: every heap the guest can name by handle.
Heap* FindHeap(uint32_t handle);
Heap* FindHeapContaining(uint32_t ptr);
Heap* AddHeap(std::unique_ptr<Heap> heap);
std::unique_ptr<Heap> RemoveHeap(uint32_t handle);

// Startup: create the base heaps (MEM1 frame heap, MEM2 expanded heap, foreground frame heap).
void InitMemory(uint32_t mem2Start);
uint32_t BaseHeapHandle(uint32_t arena);  // 0 MEM1, 1 MEM2, 8 foreground
void SetBaseHeapHandle(uint32_t arena, uint32_t handle);
uint32_t Mem2Start();
} // namespace cafe
