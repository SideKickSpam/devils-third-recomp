// The player's copy of Devil's Third: a .wua archive (Cemu's format) or a folder of extracted titles. Finds the base
// title and the update, checks that they are the supported game and version, and copies them out.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace setup
{
// The supported release: Devil's Third, North America (USA), version 16 (the last update).
constexpr const char* BaseTitleId = "0005000010177600";
constexpr const char* UpdateTitleId = "0005000E10177600";
constexpr int SupportedVersion = 16;
// SHA-256 of code/DevilsThirdGame-WiiU-Shipping.rpx in version 16 (identical in the update and a v16 base title).
constexpr const char* RpxSha256 = "0659e26fb0bb8f1d444c401d1fd1dce4a3de568c38e56d54c97b1e3e410cbe75";
constexpr const char* RpxName = "code/DevilsThirdGame-WiiU-Shipping.rpx";
// The verified copy's base content folder (the update has no content files).
constexpr uint64_t ContentFiles = 2503;
constexpr uint64_t ContentBytes = 14364291143ull;

struct FileEntry
{
    std::string path;  // relative to the title, with '/' separators
    uint64_t size = 0;
};

// A place titles can be read from: a folder or a .wua archive.
class Source
{
public:
    virtual ~Source() = default;
    // Every file under `dir` ('' = the root), recursively, relative to `dir`.
    virtual std::vector<FileEntry> List(const std::string& dir) = 0;
    // Subdirectories of `dir` (names only).
    virtual std::vector<std::string> Dirs(const std::string& dir) = 0;
    virtual bool Exists(const std::string& path) = 0;
    // Reads the whole file (small files only), or a part of it.
    virtual bool ReadAll(const std::string& path, std::vector<uint8_t>& out) = 0;
    virtual bool Read(const std::string& path, uint64_t offset, uint64_t size, void* buffer) = 0;
    // Where the source is on disk, if titles can be used in place (folders); empty for archives.
    virtual std::filesystem::path HostPath(const std::string& path) = 0;
};

struct Title
{
    std::string dir;  // in the source ('/' separators, '' = the root)
    std::string titleId;  // from code/app.xml, upper case
    int version = 0;
    std::string region;  // from meta/meta.xml (hex)
    std::string productCode;
    std::vector<FileEntry> files;  // code/, content/, meta/
    uint64_t bytes = 0;
};

struct Dump
{
    std::filesystem::path path;  // what the player chose
    bool archive = false;
    std::unique_ptr<Source> source;
    Title base, update;  // update.dir empty if there is none
    std::string rpxTitleDir;  // the title whose RPX is used (the update's if present)
    std::vector<std::string> warnings;
    uint64_t TotalBytes() const { return base.bytes + update.bytes; }
};

// Opens and checks a dump. False with a message for the player if it is not the supported game and version.
bool Inspect(const std::filesystem::path& path, Dump& dump, std::string& error);

// Progress of long steps: bytes done of total; returns false to cancel.
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// Copies the titles into `dest` as <dest>/<titleId>_v<version> folders (the layout the runtime reads).
bool Extract(Dump& dump, const std::filesystem::path& dest, const Progress& progress, std::string& error);

// Reads the RPX the game runs from (the update's if there is one).
bool ReadRpx(Dump& dump, std::vector<uint8_t>& out, std::string& error);

std::string Sha256Hex(const uint8_t* data, size_t size);
std::string FormatBytes(uint64_t bytes);
} // namespace setup
