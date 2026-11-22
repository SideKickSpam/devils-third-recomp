#include "game_module.h"

#include "ppc_context.h"

#if defined(DT_HOST_MODULE)
#include "log.h"
#include "module_abi.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <intrin.h>
#include <windows.h>

// The generated tables, from the module (ppc_context.h: pointers in this build).
const PPCFuncMapping* PPCFuncMappings = nullptr;
const PPCHookSlot* PPCHookSlots = nullptr;
const PPCImportMapping* PPCImportMappings = nullptr;
const PPCDataImport* PPCDataImports = nullptr;

extern "C" unsigned long _tls_index;  // (the C runtime's: this executable's slot in each thread's TLS array)

namespace game_module
{
namespace
{
std::filesystem::path ExeDir()
{
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, n)).parent_path();
}

std::filesystem::path LocalAppData()
{
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return n && n < MAX_PATH ? std::filesystem::path(std::wstring(buffer, n)) : std::filesystem::path();
}

std::string Utf8(const std::filesystem::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

void Trap(PPCContext& ctx, uint32_t addr) { ppc_trap(ctx, addr); }
void Unsupported(PPCContext& ctx, uint32_t addr, const char* what) { ppc_unsupported(ctx, addr, what); }
void BadBranch(PPCContext& ctx, uint32_t from, uint32_t to) { ppc_bad_branch(ctx, from, to); }
} // namespace

bool Used() { return true; }

std::string DataDir()
{
    const auto local = LocalAppData();
    return local.empty() ? std::string() : Utf8(local / "dt-port");
}

std::string InstalledGameDir()
{
    const std::string dir = DataDir();
    if (dir.empty())
        return {};
    std::ifstream in(std::filesystem::u8path(dir) / "install.ini");
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.rfind("game_dir=", 0) == 0)
            return line.substr(9);
    }
    return {};
}

bool LaunchSetup(const std::string& args)
{
    const std::filesystem::path setup = ExeDir() / "DevilsThirdSetup.exe";
    std::error_code ec;
    if (!std::filesystem::exists(setup, ec))
        return false;
    std::wstring command = L"\"" + setup.wstring() + L"\" ";
    command += std::filesystem::u8path(args).wstring();
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool Load(const std::string& path, std::string& error)
{
    std::vector<std::filesystem::path> candidates;
    if (!path.empty())
        candidates.push_back(std::filesystem::u8path(path));
    else
    {
        candidates.push_back(ExeDir() / "dt_game.dll");
        if (const auto local = LocalAppData(); !local.empty())
            candidates.push_back(local / "dt-port" / "dt_game.dll");
    }
    HMODULE module = nullptr;
    std::filesystem::path loaded;
    for (const auto& c : candidates)
    {
        std::error_code ec;
        if (!std::filesystem::exists(c, ec))
            continue;
        module = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module)
        {
            loaded = c;
            break;
        }
    }
    if (!module)
    {
        error = "The game has not been set up on this computer yet (no dt_game.dll). Run the setup tool and point it at "
                "your copy of Devil's Third.";
        return false;
    }
    using Init = const DtModuleExports* (*)(const DtModuleHost*);
    const auto init = reinterpret_cast<Init>(GetProcAddress(module, "dt_module_init"));
    // This thread's TLS array, and the trace ring's place in this executable's slot of it.
    char** tls = reinterpret_cast<char**>(__readgsqword(0x58));
    DtModuleHost host{};
    host.version = DT_MODULE_VERSION;
    host.lookup = ppc_lookup;
    host.import = ppc_import;
    host.trap = Trap;
    host.unsupported = Unsupported;
    host.bad_branch = BadBranch;
    host.syscall = ppc_syscall;
    host.timebase = ppc_timebase;
    host.fpscr_changed = ppc_fpscr_changed;
    host.trace_log = ppc_trace_log;
    host.lookup_cache = reinterpret_cast<uint64_t*>(ppc_lookup_cache);
    host.func_list = &ppc_func_list;
    host.trace_all = &ppc_trace_all;
    host.tls_index = uint32_t(_tls_index);
    host.ring_offset = uint32_t(reinterpret_cast<char*>(&ppc_trace_ring) - tls[_tls_index]);
    const DtModuleExports* exports = init ? init(&host) : nullptr;
    if (!exports || exports->version != DT_MODULE_VERSION || !exports->abi || strcmp(exports->abi, DT_MODULE_ABI) != 0)
    {
        FreeLibrary(module);
        error = "The game module (" + Utf8(loaded) + ") was made for another version of dt-port. Run the setup tool "
                "again to rebuild it.";
        return false;
    }
    PPCFuncMappings = exports->funcs;
    PPCHookSlots = exports->hooks;
    PPCImportMappings = exports->imports;
    PPCDataImports = exports->data;
    dtlog::Important("[dt-port] game module %s\n", Utf8(loaded).c_str());
    return true;
}
} // namespace game_module

#else

namespace game_module
{
bool Used() { return false; }
bool Load(const std::string&, std::string&) { return true; }
std::string DataDir() { return {}; }
std::string InstalledGameDir() { return {}; }
bool LaunchSetup(const std::string&) { return false; }
} // namespace game_module

#endif
