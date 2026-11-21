// Maps Cafe OS paths (/vol/content, /vol/code, /vol/save) onto the extracted game folders.
#pragma once

#include <optional>
#include <string>

namespace vfs
{
// gameDir holds the extracted titles: 00050000xxxxxxxx_vN (base) and optionally 0005000exxxxxxxx_vN (update).
bool Init(const std::string& gameDir, std::string& error);
// Where /vol/save lives (default <game-dir>/save). Call before Init.
void SetSaveDir(const std::string& dir);

// Host path for an existing guest file or directory, or nullopt. Lookups are case-insensitive, like the
// console's file system; files in the update shadow the base title's.
std::optional<std::string> Resolve(const std::string& guestPath);

// Host path where a guest file may be created (only under writable mounts such as /vol/save).
std::optional<std::string> ResolveForWrite(const std::string& guestPath);

// Make a guest path absolute and normalise "." / ".." / duplicate slashes.
std::string Normalize(const std::string& path, const std::string& cwd = "/");
} // namespace vfs
