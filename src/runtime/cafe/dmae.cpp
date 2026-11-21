// dmae: the DMA engine. Copies run synchronously (after the GX2 commands queued before them, on the GPU thread), so the
// timestamp each call returns has already retired (it is shared with GX2's: the GPU thread's last retired marker).
// UE3's texture pool moves textures with it when it defragments (FGX2TexturePool::PlatformRelocate); textures the GPU
// wrote (mips streamed with GX2CopySurface) have their data only in the renderer's images, which move with the memory.
#include "../guest.h"
#include "../imports.h"
#include "../gpu/queue.h"
#include "../gpu/renderer.h"
#include "gx2.h"

#include <algorithm>
#include <cstring>

namespace cafe::dmae
{
namespace
{
// DMAECopyMem(dst, src, numWords, endianSwap): endianSwap 0 none, 1 8-in-16, 2 8-in-32.
uint64_t DMAECopyMem(uint32_t dst, uint32_t src, uint32_t numWords, uint32_t endianSwap)
{
    const size_t bytes = size_t(numWords) * 4;
    gpu::queue::RunSynchronous([=] {
        memmove(guest::Ptr(dst), guest::Ptr(src), bytes);
        const size_t unit = endianSwap == 1 ? 2 : endianSwap == 2 ? 4 : 1;
        if (unit > 1)
            for (uint8_t* p = guest::Ptr(dst); p + unit <= guest::Ptr(dst) + bytes; p += unit)
                std::reverse(p, p + unit);
        else
            gpu::renderer::RelocateSurfaces(dst, src, uint32_t(bytes));
    });
    return gx2::TimeStamp();
}

// DMAESemaphore(uint64_t* semaphore, action): action 0 = wait (decrement once > 0), 1 = signal (increment).
// Nothing else runs on the engine, so a wait never has to block.
uint64_t DMAESemaphore(uint32_t sem, uint32_t action)
{
    const uint64_t v = guest::Read64(sem);
    guest::Write64(sem, action == 1 ? v + 1 : (v ? v - 1 : 0));
    return gx2::TimeStamp();
}
} // namespace

CAFE_IMPORT(dmae, "DMAECopyMem", DMAECopyMem);
CAFE_IMPORT(dmae, "DMAESemaphore", DMAESemaphore);
} // namespace cafe::dmae
