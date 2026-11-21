// Host replacements for guest functions (listed in src/recomp/hooks.txt, so the recompiler gives them a hook slot).
// A hook receives the guest call as is (arguments in r3.., result in r3) and can run the game's own code with
// hooks::Original(addr)(ctx, base). Register with DT_HOOK at namespace scope; hooks::InstallAll() runs before the game.
#pragma once

#include "ppc_context.h"

#include <cstdint>

namespace hooks
{
void Register(uint32_t addr, PPCFunc* fn, const char* name);
void InstallAll();
PPCFunc* Original(uint32_t addr);

struct Registration
{
    Registration(uint32_t addr, PPCFunc* fn, const char* name) { Register(addr, fn, name); }
};
} // namespace hooks

#define DT_HOOK_CONCAT2(a, b) a##b
#define DT_HOOK_CONCAT(a, b) DT_HOOK_CONCAT2(a, b)
#define DT_HOOK(addr, fn, name) static ::hooks::Registration DT_HOOK_CONCAT(gHook_, __LINE__)(addr, fn, name)
