// Reading the game's own Unreal Engine 3 packages (.xxx: big-endian, version 868, zlib chunks) at runtime, to take
// assets such as menu fonts straight from the player's files (src/tools/ue3_package.py is the same reader in Python).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace assets
{
class Package
{
public:
    // Load a package from a host path; false (and error) if it cannot be read.
    bool Load(const std::string& path, std::string& error);

    struct Export
    {
        int32_t cls = 0, outer = 0;
        std::string name;
        uint32_t size = 0, offset = 0;
    };
    const std::vector<Export>& Exports() const { return exports; }
    std::string ObjectName(int32_t index) const;  // export > 0, import < 0
    // The serialized data of the first export of class `cls` named `name`, or nothing.
    std::optional<std::vector<uint8_t>> Find(const std::string& cls, const std::string& name) const;

private:
    std::vector<uint8_t> data;
    std::vector<std::string> names;
    std::vector<std::string> importNames;
    std::vector<Export> exports;
};

// The movie (SWF with a "GFX" signature) inside a SwfMovie's serialized data.
std::optional<std::vector<uint8_t>> MovieBytes(const std::vector<uint8_t>& swfMovie);
} // namespace assets
