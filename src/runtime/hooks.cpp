#include "hooks.h"

#include "log.h"

#include <vector>

namespace hooks
{
namespace
{
struct Entry
{
    uint32_t addr;
    PPCFunc* fn;
    const char* name;
};

std::vector<Entry>& Entries()
{
    static std::vector<Entry> entries;  // filled by static registrations, before main
    return entries;
}
} // namespace

void Register(uint32_t addr, PPCFunc* fn, const char* name) { Entries().push_back({ addr, fn, name }); }

void InstallAll()
{
    size_t installed = 0, slots = 0;
    for (const PPCHookSlot* s = PPCHookSlots; s->addr; s++, slots++)
        for (const Entry& e : Entries())
            if (e.addr == s->addr)
                *s->hook = e.fn, installed++;
    for (const Entry& e : Entries())
    {
        bool found = false;
        for (const PPCHookSlot* s = PPCHookSlots; s->addr; s++)
            found |= s->addr == e.addr;
        if (!found)
            dtlog::Important("[hooks] %s (0x%08X) has no hook slot: add it to src/recomp/hooks.txt and run dt_recomp\n",
                e.name, e.addr);
    }
    dtlog::Important("[hooks] %zu of %zu hook slots installed\n", installed, slots);
}

PPCFunc* Original(uint32_t addr)
{
    for (const PPCHookSlot* s = PPCHookSlots; s->addr; s++)
        if (s->addr == addr)
            return s->original;
    return nullptr;
}
} // namespace hooks
