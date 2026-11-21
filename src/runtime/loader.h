// Loads the decompressed RPX's sections into guest memory at their link addresses.
#pragma once

#include <cstdint>
#include <map>
#include <string>

struct RpxInfo
{
    uint32_t entry = 0;
    uint32_t dataEnd = 0;  // end of .data/.bss
    // From the RPL file-info section.
    uint32_t sdaBase = 0, sda2Base = 0, stackSize = 0, heapSize = 0;
    std::map<std::string, uint32_t> exports;  // .fexports / .dexports
};

bool LoadRpx(const std::string& path, RpxInfo& info, std::string& error);
