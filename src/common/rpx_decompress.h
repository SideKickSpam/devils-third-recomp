// Wii U RPX/RPL decompression: sections flagged RPL_ZLIB hold their uncompressed size (big-endian) and a zlib stream.
// The result is a plain ELF32 with every section stored uncompressed, which dt_recomp and the runtime load.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rpx
{
// Decompresses `in` into `out`. False with `error` set if `in` is not an RPX/RPL or a section is damaged.
bool Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& error);
} // namespace rpx
