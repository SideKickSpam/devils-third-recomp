#pragma once

#include <cstdint>
#include <string>

namespace gpu
{
// Write 8-bit RGBA pixels (rows top to bottom, `stride` bytes apart) as a PNG file.
bool WritePng(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t stride);
}
