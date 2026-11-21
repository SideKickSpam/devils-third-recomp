#include "ue3.h"

#include "guest.h"

#include <cstring>
#include <vector>

namespace ue3
{
namespace
{
// The game's functions (Green Hills C++ ABI: `this` in r3, structs passed by pointer).
constexpr uint32_t FName_FromString = 0x02147A40;  // FName::FName(const char*, EFindName)
constexpr uint32_t UObject_FindFunction = 0x021607F8;  // UFunction* UObject::FindFunction(FName, UBOOL global) const
constexpr uint32_t UObject_ProcessEvent = 0x02078FC4;  // void UObject::ProcessEvent(UFunction*, void* parms, void*)
constexpr uint32_t UObject_GetPathName = 0x0213C510;   // void UObject::GetPathName(const UObject* stop, FString&) const
constexpr uint32_t FindField_UProperty = 0x020738DC;   // UProperty* FindField<UProperty>(UStruct*, const wchar_t*)
constexpr uint32_t UUIManager_GetUIManager = 0x030FF924;  // static UUIManager* UUIManager::GetUIManager()
constexpr uint32_t AppRealloc = 0x0204B4E4;  // void* appRealloc(void*, DWORD size, DWORD alignment)

// Layouts in this build (32-bit), checked against live objects.
constexpr uint32_t ObjectOuter = 0x28, ObjectName = 0x2C, ObjectClass = 0x34;
constexpr uint32_t FieldNext = 0x40, StructSuper = 0x48, StructChildren = 0x4C;
constexpr uint32_t FunctionNumParms = 0x88, FunctionParmsSize = 0x8A, FunctionReturnOffset = 0x8C;
constexpr uint32_t PropertyElementSize = 0x4C, PropertyOffset = 0x5C, BoolPropertyMask = 0x70;

// Guest memory for names, strings and parameter blocks, reused by every call (Game Thread only).
uint32_t gArena = 0, gArenaUsed = 0;
constexpr uint32_t ArenaSize = 0x10000;

uint32_t Alloc(uint32_t size)
{
    if (!gArena)
        gArena = guest::SystemAlloc(ArenaSize);
    size = (size + 15) & ~15u;
    if (gArenaUsed + size > ArenaSize)
        gArenaUsed = 0;  // calls are short-lived: the oldest scratch is long done with
    const uint32_t at = gArena + gArenaUsed;
    gArenaUsed += size;
    memset(guest::base + at, 0, size);
    return at;
}

// Run a guest function from the current guest context, keeping the caller's registers.
uint32_t Guest(uint32_t addr, std::initializer_list<uint32_t> args)
{
    PPCContext& ctx = *guest::CurrentContext();
    const PPCContext saved = ctx;
    const uint32_t r = guest::Call(addr, args);
    ctx = saved;
    return r;
}

uint32_t MakeName(const char* name, bool add, uint32_t at)
{
    const uint32_t text = Alloc(uint32_t(strlen(name)) + 1);
    guest::WriteString(text, name, strlen(name) + 1);
    Guest(FName_FromString, { at, text, add ? 1u : 0u });
    return at;
}

uint32_t Wide(const std::string& s)
{
    const uint32_t at = Alloc(uint32_t(s.size() + 1) * 2);
    for (size_t i = 0; i < s.size(); i++)
        guest::Write16(at + uint32_t(i) * 2, uint8_t(s[i]));
    return at;
}

std::string ReadFString(uint32_t f)
{
    std::string out;
    const uint32_t data = guest::Read32(f), count = guest::Read32(f + 4);
    for (uint32_t i = 0; data && i + 1 < count && i < 512; i++)
        out += char(guest::Read16(data + i * 2));
    return out;
}

uint32_t FindFunction(uint32_t object, const char* name)
{
    const uint32_t fname = MakeName(name, false, Alloc(8));
    if (!guest::Read32(fname))  // the name does not exist, so neither does the function
        return 0;
    return Guest(UObject_FindFunction, { object, fname, 0 });
}

// The function's parameters (not the return value) in declaration order.
std::vector<uint32_t> Parameters(uint32_t function)
{
    std::vector<uint32_t> out;
    const uint32_t count = guest::Read16(function + FunctionNumParms), ret = guest::Read16(function + FunctionReturnOffset);
    uint32_t p = guest::Read32(function + StructChildren);
    for (uint32_t i = 0; p && i < count; i++, p = guest::Read32(p + FieldNext))
        if (guest::Read32(p + PropertyOffset) != ret || ret == 0xFFFF)
            out.push_back(p);
    return out;
}

std::optional<uint32_t> Run(uint32_t object, const char* function, std::initializer_list<Arg> args, uint32_t& returnSize)
{
    if (!object)
        return std::nullopt;
    const uint32_t mark = gArenaUsed;
    const uint32_t f = FindFunction(object, function);
    if (!f)
        return std::nullopt;
    const uint32_t size = guest::Read16(f + FunctionParmsSize), ret = guest::Read16(f + FunctionReturnOffset);
    const uint32_t parms = Alloc(std::max(size, 16u));
    const std::vector<uint32_t> params = Parameters(f);
    size_t i = 0;
    for (const Arg& a : args)
    {
        if (i >= params.size())
            break;
        const uint32_t p = params[i++], at = parms + guest::Read32(p + PropertyOffset);
        const uint32_t elementSize = guest::Read32(p + PropertyElementSize);
        switch (a.kind)
        {
        case Arg::Int:
            if (elementSize == 1)
                guest::Write8(at, uint8_t(a.u));
            else
                guest::Write32(at, a.u);
            break;
        case Arg::Float:
        {
            uint32_t bits;
            memcpy(&bits, &a.f, 4);
            guest::Write32(at, bits);
            break;
        }
        case Arg::Vector:
            for (int k = 0; k < 3; k++)
            {
                uint32_t bits;
                memcpy(&bits, &a.v[k], 4);
                guest::Write32(at + uint32_t(k) * 4, bits);
            }
            break;
        case Arg::Bool:
            guest::Write32(at, a.u ? guest::Read32(p + BoolPropertyMask) | 1u : 0u);
            break;
        case Arg::Name:
        case Arg::String:
            // By the parameter's own type: a name (8 bytes) or a string (an FString, 12 bytes).
            if (elementSize == 8)
            {
                MakeName(a.s.c_str(), true, at);
                break;
            }
            {
                const uint32_t text = Wide(a.s);
                guest::Write32(at, text), guest::Write32(at + 4, uint32_t(a.s.size() + 1)), guest::Write32(at + 8, uint32_t(a.s.size() + 1));
            }
            break;
        }
    }
    Guest(UObject_ProcessEvent, { object, f, parms, 0 });
    uint32_t result = 0;
    returnSize = 0;
    if (ret != 0xFFFF)
    {
        uint32_t p = guest::Read32(f + StructChildren);
        while (p && guest::Read32(p + PropertyOffset) != ret)
            p = guest::Read32(p + FieldNext);
        returnSize = p ? guest::Read32(p + PropertyElementSize) : 4;
        result = returnSize == 1 ? guest::Read8(parms + ret) : guest::Read32(parms + ret);
        if (p && ClassName(p) == "BoolProperty")
            result = result != 0;
    }
    gArenaUsed = mark;
    return result;
}
} // namespace

std::optional<uint32_t> Call(uint32_t object, const char* function, std::initializer_list<Arg> args)
{
    uint32_t size;
    return Run(object, function, args, size);
}

std::optional<float> CallFloat(uint32_t object, const char* function, std::initializer_list<Arg> args)
{
    const auto r = Call(object, function, args);
    if (!r)
        return std::nullopt;
    float f;
    memcpy(&f, &*r, 4);
    return f;
}

std::optional<Property> FindProperty(uint32_t object, const char* name)
{
    if (!object)
        return std::nullopt;
    const uint32_t mark = gArenaUsed;
    const uint32_t p = Guest(FindField_UProperty, { guest::Read32(object + ObjectClass), Wide(name) });
    gArenaUsed = mark;
    if (!p)
        return std::nullopt;
    Property out;
    out.address = object + guest::Read32(p + PropertyOffset);
    out.size = guest::Read32(p + PropertyElementSize);
    if (ClassName(p) == "BoolProperty")
        out.mask = guest::Read32(p + BoolPropertyMask);
    return out;
}

uint32_t FindPropertyField(uint32_t object, const char* name)
{
    if (!object)
        return 0;
    const uint32_t mark = gArenaUsed;
    const uint32_t p = Guest(FindField_UProperty, { guest::Read32(object + ObjectClass), Wide(name) });
    gArenaUsed = mark;
    return p;
}

uint32_t Realloc(uint32_t pointer, uint32_t size) { return Guest(AppRealloc, { pointer, size, 8 }); }

std::optional<uint32_t> GetObject(uint32_t object, const char* name)
{
    const auto p = FindProperty(object, name);
    if (!p || p->size != 4)
        return std::nullopt;
    return guest::Read32(p->address);
}

std::optional<bool> GetBool(uint32_t object, const char* name)
{
    const auto p = FindProperty(object, name);
    if (!p || !p->mask)
        return std::nullopt;
    return (guest::Read32(p->address) & p->mask) != 0;
}

bool SetBool(uint32_t object, const char* name, bool value)
{
    const auto p = FindProperty(object, name);
    if (!p || !p->mask)
        return false;
    const uint32_t w = guest::Read32(p->address);
    guest::Write32(p->address, value ? w | p->mask : w & ~p->mask);
    return true;
}

uint32_t UIManager() { return Guest(UUIManager_GetUIManager, {}); }

std::string PathName(uint32_t object)
{
    if (!object)
        return "";
    const uint32_t mark = gArenaUsed;
    const uint32_t s = Alloc(12);
    Guest(UObject_GetPathName, { object, 0, s });
    gArenaUsed = mark;
    return ReadFString(s);
}

uint32_t NameIndex(uint32_t object) { return object ? guest::Read32(object + ObjectName) : 0; }
uint32_t OuterOf(uint32_t object) { return object ? guest::Read32(object + ObjectOuter) : 0; }

std::string ClassName(uint32_t object)
{
    const std::string path = PathName(object ? guest::Read32(object + ObjectClass) : 0);
    return path.substr(path.find_last_of(".:") + 1);
}

bool IsA(uint32_t object, const char* className)
{
    if (!object)
        return false;
    const uint32_t want = FindNameIndex(className);
    if (!want)
        return false;
    for (uint32_t c = guest::Read32(object + ObjectClass); c; c = guest::Read32(c + StructSuper))
        if (guest::Read32(c + ObjectName) == want)
            return true;
    return false;
}

uint32_t FindNameIndex(const char* name)
{
    const uint32_t mark = gArenaUsed;
    const uint32_t n = MakeName(name, false, Alloc(8));
    const uint32_t index = guest::Read32(n);
    gArenaUsed = mark;
    return index;
}
} // namespace ue3
