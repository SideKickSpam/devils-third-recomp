// coreinit: small system services (foreground/HOME button state, dimming, drivers, atomics).
#include "../imports.h"
#include "coreinit.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../log.h"

namespace cafe::coreinit
{
namespace
{
std::atomic<bool> gHomeButtonEnabled{ true };

void OSEnableForegroundExit() {}
void OSSavesDone_ReadyToRelease() {}
void OSEnforceInorderIO() {}
bool OSEnableHomeButtonMenu(bool enable)
{
    gHomeButtonEnabled = enable;
    return true;
}
bool OSIsHomeButtonMenuEnabled() { return gHomeButtonEnabled; }

// IM (idle/dimming management): report success; no screen dimming or auto power-down on the host.
int32_t IMDisableDim() { return 0; }
int32_t IMEnableDim() { return 0; }
int32_t IMGetTimeBeforeAPD(uint32_t outSeconds)
{
    if (outSeconds)
        guest::Write32(outSeconds, 3600);
    return 0;
}

int32_t OSDriver_Register(uint32_t moduleHandle, int32_t priority, uint32_t driverInterface, uint32_t driverId,
    uint32_t outA, uint32_t outB, uint32_t outC)
{
    (void)moduleHandle, (void)priority, (void)driverInterface, (void)driverId, (void)outA, (void)outB, (void)outC;
    return 0;
}

// int64 OSAddAtomic64(volatile int64* ptr, int64 value): returns the previous value.
int64_t OSAddAtomic64(uint32_t ptr, int64_t value)
{
    auto* p = reinterpret_cast<uint64_t*>(guest::Ptr(ptr));
    uint64_t old = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    for (;;)
    {
        const uint64_t next = __builtin_bswap64(uint64_t(int64_t(__builtin_bswap64(old)) + value));
        if (__atomic_compare_exchange_n(p, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return int64_t(__builtin_bswap64(old));
    }
}
} // namespace

CAFE_IMPORT(coreinit, "OSEnableForegroundExit", OSEnableForegroundExit);
CAFE_IMPORT(coreinit, "OSSavesDone_ReadyToRelease", OSSavesDone_ReadyToRelease);
CAFE_IMPORT(coreinit, "OSEnforceInorderIO", OSEnforceInorderIO);
CAFE_IMPORT(coreinit, "OSEnableHomeButtonMenu", OSEnableHomeButtonMenu);
CAFE_IMPORT(coreinit, "OSIsHomeButtonMenuEnabled", OSIsHomeButtonMenuEnabled);
CAFE_IMPORT(coreinit, "IMDisableDim", IMDisableDim);
CAFE_IMPORT(coreinit, "IMEnableDim", IMEnableDim);
CAFE_IMPORT(coreinit, "IMGetTimeBeforeAPD", IMGetTimeBeforeAPD);
CAFE_IMPORT(coreinit, "OSDriver_Register", OSDriver_Register);
CAFE_IMPORT(coreinit, "OSAddAtomic64", OSAddAtomic64);
} // namespace cafe::coreinit

// Hardware register access (OSReadRegister16 / __OSReadRegister32Ex). Used by the statically linked legacy
// VI/SI code behind PADInit to probe display timing. Registers behave as plain storage, initially zero
// (which reads as NTSC timing); every access is in the import log.
namespace cafe::coreinit
{
namespace
{
std::mutex gRegsMutex;
std::unordered_map<uint64_t, uint32_t> gRegs;  // (interface << 32 | offset) -> value

uint32_t ReadReg(uint32_t iface, uint32_t offset)
{
    std::lock_guard lock(gRegsMutex);
    auto it = gRegs.find(uint64_t(iface) << 32 | offset);
    return it == gRegs.end() ? 0 : it->second;
}
void WriteReg(uint32_t iface, uint32_t offset, uint32_t value)
{
    std::lock_guard lock(gRegsMutex);
    gRegs[uint64_t(iface) << 32 | offset] = value;
}

uint16_t OSReadRegister16(uint32_t iface, uint32_t offset) { return uint16_t(ReadReg(iface, offset)); }
void OSWriteRegister16(uint32_t iface, uint32_t offset, uint16_t value) { WriteReg(iface, offset, value); }
uint32_t OSReadRegister32Ex(uint32_t iface, uint32_t offset) { return ReadReg(iface | 0x100, offset); }
void OSWriteRegister32Ex(uint32_t iface, uint32_t offset, uint32_t value) { WriteReg(iface | 0x100, offset, value); }
} // namespace

CAFE_IMPORT(coreinit, "OSReadRegister16", OSReadRegister16);
CAFE_IMPORT(coreinit, "OSWriteRegister16", OSWriteRegister16);
CAFE_IMPORT(coreinit, "__OSReadRegister32Ex", OSReadRegister32Ex);
CAFE_IMPORT(coreinit, "__OSWriteRegister32Ex", OSWriteRegister32Ex);
} // namespace cafe::coreinit

// UC: system configuration (language, country, parental controls), read by the game's SCI helpers.
// UCSysConfig entries are 0x54 bytes: char name[64], u32 access, u32 dataType, s32 error, u32 dataSize, void* data.
namespace cafe::coreinit
{
namespace
{
struct ConfigValue
{
    uint32_t size;
    uint32_t value;
};
// A US English console with parental controls off.
const std::unordered_map<std::string, ConfigValue> gSysConfig = {
    { "cafe.language", { 4, 1 } },        // 1 = English
    { "cafe.cntry_reg", { 4, 49 } },      // 49 = United States
    { "cafe.version", { 2, 0x0101 } },
    { "cafe.initial_launch", { 1, 2 } },  // initial setup complete
    { "parent.enable", { 1, 0 } },
    { "p_acct1.network_launcher", { 1, 0 } },
};
constexpr int32_t UCErrorNotFound = -6;  // UC_ERROR_SYS_CONFIG_NOT_FOUND-like; per-entry error field

int32_t UCOpen() { return 1; }
int32_t UCClose(int32_t handle) { (void)handle; return 0; }

int32_t UCReadSysConfig(int32_t handle, uint32_t count, uint32_t entries)
{
    (void)handle;
    int32_t result = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t e = entries + i * 0x54;
        const std::string name = guest::ReadString(e, 64);
        const uint32_t size = guest::Read32(e + 0x4C), data = guest::Read32(e + 0x50);
        auto it = gSysConfig.find(name);
        if (it == gSysConfig.end() || !data)
        {
            dtlog::Write("UCReadSysConfig: unknown key %s\n", name.c_str());
            guest::Write32(e + 0x48, uint32_t(UCErrorNotFound));
            result = UCErrorNotFound;
            continue;
        }
        const ConfigValue& v = it->second;
        if (v.size == 1)
            guest::Write8(data, uint8_t(v.value));
        else if (v.size == 2)
            guest::Write16(data, uint16_t(v.value));
        else
            guest::Write32(data, v.value);
        (void)size;
        guest::Write32(e + 0x48, 0);
    }
    return result;
}
} // namespace

CAFE_IMPORT(coreinit, "UCOpen", UCOpen);
CAFE_IMPORT(coreinit, "UCClose", UCClose);
CAFE_IMPORT(coreinit, "UCReadSysConfig", UCReadSysConfig);
} // namespace cafe::coreinit

// MCP: console product information. A US (NTSC) console, consistent with the UC system config above.
// MCPSysProdSettings: product_area (u32), eeprom_version (u16), pad, game_region (u32), unknown, ntsc_pal[5], ...
namespace cafe::coreinit
{
namespace
{
constexpr uint32_t MCPRegionUSA = 0x02;
int32_t MCP_Open() { return 1; }
int32_t MCP_Close(int32_t handle) { (void)handle; return 0; }
int32_t MCP_GetSysProdSettings(int32_t handle, uint32_t out)
{
    (void)handle;
    memset(guest::Ptr(out), 0, 0x46);
    guest::Write32(out + 0x00, MCPRegionUSA);
    guest::Write16(out + 0x04, 1);
    guest::Write32(out + 0x08, MCPRegionUSA);
    guest::WriteString(out + 0x10, "NTSC", 5);
    return 0;
}
} // namespace

CAFE_IMPORT(coreinit, "MCP_Open", MCP_Open);
CAFE_IMPORT(coreinit, "MCP_Close", MCP_Close);
CAFE_IMPORT(coreinit, "MCP_GetSysProdSettings", MCP_GetSysProdSettings);
} // namespace cafe::coreinit

// OSDynLoad: run-time module loading. Modules the game acquires by name (system RPLs such as swkbd.rpl) do
// not exist here; FindExport returns a placeholder function per export that stops with its name when called,
// so each one can be implemented once the game actually uses it. Data exports get zeroed storage.
namespace cafe::coreinit
{
namespace
{
std::mutex gDynMutex;
std::unordered_map<std::string, uint32_t> gModules;          // name -> handle
std::unordered_map<uint32_t, std::string> gModuleNames;      // handle -> name
std::unordered_map<std::string, uint32_t> gDynExports;       // "module::export" -> guest address
std::vector<std::unique_ptr<std::string>> gDynNameStorage;   // keeps lib/name strings alive for the dispatcher

int32_t OSDynLoad_SetAllocator(uint32_t allocFn, uint32_t freeFn) { (void)allocFn, (void)freeFn; return 0; }

int32_t OSDynLoad_Acquire(GuestString name, uint32_t outModule)
{
    std::string n = name.str();
    if (auto dot = n.rfind('.'); dot != std::string::npos)
        n = n.substr(0, dot);  // "swkbd.rpl" -> "swkbd"
    std::lock_guard lock(gDynMutex);
    uint32_t& handle = gModules[n];
    if (!handle)
    {
        handle = guest::SystemAlloc(0x10);
        gModuleNames[handle] = n;
    }
    if (outModule)
        guest::Write32(outModule, handle);
    return 0;
}

void OSDynLoad_Release(uint32_t module) { (void)module; }

int32_t OSDynLoad_FindExport(uint32_t module, bool isData, GuestString name, uint32_t outAddr)
{
    std::lock_guard lock(gDynMutex);
    auto mod = gModuleNames.find(module);
    if (mod == gModuleNames.end())
        return -1;
    const std::string key = mod->second + "::" + name.str();
    uint32_t& addr = gDynExports[key];
    if (!addr)
    {
        if (isData)
            addr = guest::SystemAlloc(0x1000);
        else
        {
            gDynNameStorage.push_back(std::make_unique<std::string>(mod->second));
            const char* lib = gDynNameStorage.back()->c_str();
            gDynNameStorage.push_back(std::make_unique<std::string>(name.str()));
            const ImportImpl* impl = FindImplementation(key);
            addr = RegisterHostFunction(lib, gDynNameStorage.back()->c_str(), impl ? impl->call : nullptr,
                impl ? impl->args : nullptr, impl ? impl->result : nullptr);
        }
        dtlog::Write("OSDynLoad: %s %s -> 0x%08X\n", isData ? "data" : "function", key.c_str(), addr);
    }
    if (outAddr)
        guest::Write32(outAddr, addr);
    return 0;
}
} // namespace

CAFE_IMPORT(coreinit, "OSDynLoad_SetAllocator", OSDynLoad_SetAllocator);
CAFE_IMPORT(coreinit, "OSDynLoad_Acquire", OSDynLoad_Acquire);
CAFE_IMPORT(coreinit, "OSDynLoad_Release", OSDynLoad_Release);
CAFE_IMPORT(coreinit, "OSDynLoad_FindExport", OSDynLoad_FindExport);
} // namespace cafe::coreinit
