// Builds the game module (dt_game.dll) from the player's RPX: decompress it, recompile it with dt_recomp, compile the
// generated C++ with the bundled clang-cl and link it with lld-link. Only files that changed since the last build are
// compiled again (a dt-port update usually changes few of them).
#pragma once

#include "dump.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace setup
{
struct BuildPaths
{
    std::filesystem::path tools;  // the release's tools/ folder: dt_recomp.exe, llvm/, sdk/
    std::filesystem::path data;   // %LOCALAPPDATA%\dt-port: dt_game.dll and build/
};

enum class BuildStep
{
    Idle,
    Decompress,
    Recompile,
    Compile,
    Link,
    Done,
    Failed,
};

struct BuildStatus
{
    std::atomic<BuildStep> step{ BuildStep::Idle };
    std::atomic<uint64_t> done{ 0 }, total{ 0 };  // Compile: bytes of source compiled / to compile
    std::atomic<int> filesDone{ 0 }, filesTotal{ 0 }, jobs{ 0 };
    std::atomic<bool> cancel{ false };
    std::mutex mutex;
    std::string error;
    std::vector<std::string> log;  // tool output, for the details view
    void Log(const std::string& line);
};

// Where the tools are: <exe dir>/tools, or DT_SETUP_TOOLS (development).
BuildPaths DefaultPaths();
// Checks that the tools are there; false with a message if not.
bool CheckTools(const BuildPaths& paths, std::string& error);
// Builds dt_game.dll from `dump`'s RPX. Runs on a worker thread; `status` is read by the UI.
bool BuildModule(Dump& dump, const BuildPaths& paths, BuildStatus& status);
} // namespace setup
