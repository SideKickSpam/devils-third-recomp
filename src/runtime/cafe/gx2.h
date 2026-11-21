#pragma once

#include <cstdint>
#include <string>

namespace cafe::gx2
{
// Stop with a report after this many GX2SwapScanBuffers calls (0 = no limit).
void SetFrameLimit(uint64_t frames);
// The latest GPU timestamp; every timestamp handed out is already retired (work completes when submitted).
uint64_t TimeStamp();
// GX2SwapScanBuffers calls so far.
uint64_t FrameCount();
// Where translated shaders are cached (empty = no disk cache).
void SetShaderCacheDir(const std::string& dir);
} // namespace cafe::gx2
