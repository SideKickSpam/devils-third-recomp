// Patches to the game's data, applied after the RPX is loaded. Each one names what it changes and why.
#include "game_patches.h"

#include "guest.h"
#include "log.h"
#include "settings.h"

#include <bit>

namespace game_patches
{
namespace
{
// UE3's frame limiter: FEngineLoop::Tick sleeps until 0.02 s (50 fps) have passed since the frame began. The period
// is a float in .rodata that only FEngineLoop::Tick loads (both its uses), so it can follow display.fps_cap.
constexpr uint32_t FrameLimiterPeriod = 0x102F8188;
bool gLimiterFound = false;

void FrameRateCap()
{
    const float original = std::bit_cast<float>(guest::Read32(FrameLimiterPeriod));
    if (original < 0.0199f || original > 0.0201f)
    {
        dtlog::Important("[patch] frame limiter period at 0x%08X is %g, not 0.02: not patched\n", FrameLimiterPeriod, original);
        return;
    }
    gLimiterFound = true;
    const int cap = settings::GetInt("display.fps_cap", 50);
    if (cap != 50)
        SetFrameRateCap(cap);
}
} // namespace

void Apply() { FrameRateCap(); }

void SetFrameRateCap(int fps)
{
    if (!gLimiterFound)
        return;
    // 50 (the original), 60, 120, 144, 240, or 0: unlimited (the limiter's period made 1 ms, which any frame passes).
    if (fps != 0 && (fps < 30 || fps > 1000))
        fps = 50;
    guest::Write32(FrameLimiterPeriod, std::bit_cast<uint32_t>(fps ? 1.0f / float(fps) : 0.001f));
    if (fps)
        dtlog::Important("[patch] UE3 frame limiter: %d fps (display.fps_cap)\n", fps);
    else
        dtlog::Important("[patch] UE3 frame limiter: unlimited (display.fps_cap)\n");
}
} // namespace game_patches
