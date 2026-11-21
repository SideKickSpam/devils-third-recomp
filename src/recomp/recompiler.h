#pragma once

#include "decoder.h"
#include "rpx.h"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_set>

struct RecompStats
{
    size_t functions = 0;
    size_t instructions = 0;
    std::array<size_t, static_cast<size_t>(Op::COUNT)> opCounts{};
    std::map<std::string, size_t> unsupported;  // ops emitted as ppc_unsupported(), by name
    size_t invalid = 0;                          // undecodable words inside functions
    size_t switchTables = 0;
    size_t switchEntries = 0;
    size_t importCallSites = 0;
    size_t fallthroughTailCalls = 0;
    size_t badBranches = 0;                      // branches to code that is not a function start
};

class Recompiler
{
public:
    explicit Recompiler(const RpxImage& image);

    // Appends the C++ for one function to out.
    void RecompileFunction(const Function& fn, std::string& out);

    static std::string FunctionName(uint32_t addr);
    std::string ImportName(size_t index) const;

    // Hooked functions (hooks.txt): emitted as orig_sub_X, with sub_X calling the runtime's hook if one is installed.
    std::unordered_set<uint32_t> hooked;

    RecompStats stats;

private:
    const RpxImage& image;
    std::unordered_set<uint32_t> functionStarts;
    std::vector<std::string> importNames;
};
