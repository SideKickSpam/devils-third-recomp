#include "module_build.h"

#include "rpx_decompress.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace setup
{
void BuildStatus::Log(const std::string& line)
{
    std::lock_guard lock(mutex);
    log.push_back(line);
    if (log.size() > 2000)
        log.erase(log.begin(), log.begin() + 500);
}

namespace
{
std::string Utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path ExeDir()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, n)).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

// The module's compiler flags: the same as the development build's dt_game target (src/runtime/CMakeLists.txt,
// DT_MODULE_FLAGS), which the ABI hash covers.
const char* const ModuleFlags[] = { "/nologo", "/c", "/O2", "/w", "/X", "/Zl", "/GS-", "/GR-", "/EHs-c-", "/std:c++20",
    "/DNDEBUG", "/DDT_MODULE", "/clang:-fno-math-errno", "/clang:-msse4.1", "/clang:-mno-stack-arg-probe",
    "/clang:-mmovbe", "/clang:-mlzcnt", "/clang:-mpopcnt", "/clang:-mbmi", "/clang:-mbmi2" };

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Runs a command line, collecting its output; returns its exit code (-1 if it could not start).
int Run(const std::string& command, std::string& output)
{
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring wide = fs::u8path(command).wstring();
    const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return -1;
    }
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &n, nullptr) && n)
        output.append(buffer, n);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
#else
    FILE* p = popen((command + " 2>&1").c_str(), "r");
    if (!p)
        return -1;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), p)) > 0)
        output.append(buffer, n);
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

void LogOutput(BuildStatus& status, const std::string& output)
{
    size_t start = 0;
    while (start < output.size())
    {
        size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            status.Log(line);
        start = end + 1;
    }
}

bool Fail(BuildStatus& status, const std::string& error)
{
    std::lock_guard lock(status.mutex);
    status.error = error;
    status.step = BuildStep::Failed;
    return false;
}

std::string ReadText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool WriteIfChanged(const fs::path& p, const std::vector<uint8_t>& data)
{
    std::error_code ec;
    if (fs::exists(p, ec) && fs::file_size(p, ec) == data.size())
    {
        const std::string old = ReadText(p);
        if (old.size() == data.size() && std::equal(old.begin(), old.end(), data.begin()))
            return true;
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(out);
}

// Parallel compile jobs: one per hardware thread, at most one per 1.5 GB of memory (the largest generated files need
// about 1 GB in clang at -O2).
int JobCount()
{
    int jobs = int(std::max(1u, std::thread::hardware_concurrency()));
#if defined(_WIN32)
    MEMORYSTATUSEX m{ sizeof(m) };
    if (GlobalMemoryStatusEx(&m))
        jobs = std::min<int>(jobs, std::max<int>(1, int(m.ullTotalPhys / (1536ull << 20))));
#endif
    return std::max(1, jobs);
}
} // namespace

BuildPaths DefaultPaths()
{
    BuildPaths p;
    if (const char* env = getenv("DT_SETUP_TOOLS"); env && *env)
        p.tools = fs::u8path(env);
    else
        p.tools = ExeDir() / "tools";
#if defined(_WIN32)
    if (const char* local = getenv("LOCALAPPDATA"); local && *local)
        p.data = fs::u8path(local) / "dt-port";
#else
    if (const char* home = getenv("HOME"); home && *home)
        p.data = fs::u8path(home) / ".local" / "share" / "dt-port";
#endif
    return p;
}

bool CheckTools(const BuildPaths& paths, std::string& error)
{
    std::error_code ec;
    for (const char* f : { "dt_recomp.exe", "llvm/bin/clang-cl.exe", "llvm/bin/lld-link.exe", "sdk/ppc_context.h",
             "sdk/module_abi.h", "sdk/dt_module.cpp", "sdk/hooks.txt" })
        if (!fs::exists(paths.tools / fs::u8path(f), ec))
        {
            error = "This copy of dt-port is incomplete: tools/" + std::string(f) +
                    " is missing. Download the release again and unpack all of it.";
            return false;
        }
    return true;
}

bool BuildModule(Dump& dump, const BuildPaths& paths, BuildStatus& status)
{
    std::error_code ec;
    const fs::path build = paths.data / "build";
    const fs::path generated = build / "generated";
    const fs::path obj = build / "obj";
    fs::create_directories(generated, ec);
    fs::create_directories(obj, ec);

    // ---- decompress the RPX ----
    status.step = BuildStep::Decompress;
    std::vector<uint8_t> rpxData, elf;
    std::string error;
    if (!ReadRpx(dump, rpxData, error) || !rpx::Decompress(rpxData, elf, error))
        return Fail(status, error);
    const fs::path elfPath = build / "dt.elf";
    if (!WriteIfChanged(elfPath, elf))
        return Fail(status, "Cannot write " + Utf8(elfPath) + ".");
    rpxData.clear();
    elf.clear();

    // ---- recompile (only files whose content changes are rewritten) ----
    status.step = BuildStep::Recompile;
    {
        std::string output;
        const std::string cmd = Quote(Utf8(paths.tools / "dt_recomp.exe")) + " " + Quote(Utf8(elfPath)) + " " +
                                Quote(Utf8(generated)) + " --hooks " + Quote(Utf8(paths.tools / "sdk" / "hooks.txt"));
        status.Log("> " + cmd);
        const int code = Run(cmd, output);
        LogOutput(status, output);
        if (code != 0)
            return Fail(status, "The recompiler failed (exit code " + std::to_string(code) + "). See the details.");
    }
    if (status.cancel)
        return Fail(status, "Cancelled.");

    // ---- compile ----
    status.step = BuildStep::Compile;
    const fs::path clang = paths.tools / "llvm" / "bin" / "clang-cl.exe";
    std::string flags;
    for (const char* f : ModuleFlags)
        flags += std::string(f) + " ";
    flags += "/I" + Quote(Utf8(paths.tools / "sdk")) + " /I" + Quote(Utf8(generated)) + " ";
    // A change of compiler, flags or any header the generated code includes (ppc_context.h, the generated headers)
    // rebuilds everything; otherwise only sources newer than their object (dt_recomp rewrites only changed files).
    // Contents, not dates: a new release's files all have new dates.
    {
        std::string version;
        Run(Quote(Utf8(clang)) + " --version", version);
        version = version.substr(0, version.find('\n'));  // (the rest names the install folder)
        std::string stamp;
        for (const char* f : ModuleFlags)
            stamp += std::string(f) + " ";
        stamp += "\n" + version + "\n" + ReadText(paths.tools / "sdk" / "ppc_context.h");
        std::vector<fs::path> headers;
        for (const auto& e : fs::directory_iterator(generated, ec))
            if (e.path().extension() == ".h")
                headers.push_back(e.path());
        std::sort(headers.begin(), headers.end());
        for (const fs::path& h : headers)
            stamp += "\n" + Utf8(h.filename()) + "\n" + ReadText(h);
        const std::string digest = Sha256Hex(reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size());
        const fs::path stampPath = obj / "stamp.txt";
        if (ReadText(stampPath) != digest)
        {
            for (const auto& e : fs::directory_iterator(obj, ec))
                fs::remove(e.path(), ec);
            std::ofstream(stampPath, std::ios::binary) << digest;
        }
    }
    struct Job
    {
        fs::path source, object;
        uint64_t size;
        bool needed;
    };
    std::vector<Job> jobs;
    auto addJob = [&](const fs::path& src, bool always) {
        Job j{ src, obj / (src.stem().string() + ".obj"), fs::file_size(src, ec), true };
        if (!always && fs::exists(j.object, ec))
            j.needed = fs::last_write_time(j.object, ec) < fs::last_write_time(src, ec);
        jobs.push_back(std::move(j));
    };
    for (const auto& e : fs::directory_iterator(generated, ec))
        if (e.path().extension() == ".cpp")
            addJob(e.path(), false);
    addJob(paths.tools / "sdk" / "dt_module.cpp", true);  // tiny; carries the ABI stamp
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.size > b.size; });  // big first
    uint64_t total = 0;
    int needed = 0;
    for (const Job& j : jobs)
        if (j.needed)
            total += j.size, needed++;
    status.total = total;
    status.done = 0;
    status.filesTotal = needed;
    status.filesDone = 0;
    status.Log(std::to_string(needed) + " of " + std::to_string(jobs.size()) + " files to compile");
    std::atomic<size_t> next{ 0 };
    std::atomic<bool> failed{ false };
    std::string failure;
    std::mutex failureMutex;
    const int jobCount = JobCount();
    status.jobs = jobCount;
    std::vector<std::thread> workers;
    for (int w = 0; w < jobCount; w++)
        workers.emplace_back([&] {
            for (;;)
            {
                const size_t i = next++;
                if (i >= jobs.size() || failed || status.cancel)
                    return;
                const Job& j = jobs[i];
                if (!j.needed)
                    continue;
                const fs::path tmp = j.object.string() + ".tmp";
                std::string output;
                const std::string cmd = Quote(Utf8(clang)) + " " + flags + Quote(Utf8(j.source)) + " /Fo" +
                                        Quote(Utf8(tmp));
                const int code = Run(cmd, output);
                if (code != 0)
                {
                    std::lock_guard lock(failureMutex);
                    if (!failed.exchange(true))
                        failure = Utf8(j.source.filename()) + ": the compiler failed (exit code " +
                                  std::to_string(code) + ")\n" + output.substr(0, 2000);
                    return;
                }
                std::error_code e2;
                fs::rename(tmp, j.object, e2);
                status.done += j.size;
                status.filesDone++;
            }
        });
    for (auto& t : workers)
        t.join();
    if (failed)
    {
        LogOutput(status, failure);
        return Fail(status, failure.substr(0, failure.find('\n')));
    }
    if (status.cancel)
        return Fail(status, "Cancelled.");

    // ---- link ----
    status.step = BuildStep::Link;
    {
        const fs::path rsp = build / "link.rsp";
        std::ofstream r(rsp, std::ios::binary | std::ios::trunc);
        const fs::path out = paths.data / "dt_game.dll.new";
        r << "/nologo /DLL /NODEFAULTLIB /NOENTRY /NOIMPLIB /MACHINE:X64 /OUT:" << Quote(Utf8(out)) << "\n";
        for (const Job& j : jobs)
            r << Quote(Utf8(j.object)) << "\n";
        r.close();
        std::string output;
        const std::string cmd = Quote(Utf8(paths.tools / "llvm" / "bin" / "lld-link.exe")) + " @" + Quote(Utf8(rsp));
        status.Log("> " + cmd);
        const int code = Run(cmd, output);
        LogOutput(status, output);
        if (code != 0)
            return Fail(status, "The linker failed (exit code " + std::to_string(code) + "). See the details.");
        const fs::path dll = paths.data / "dt_game.dll";
        fs::remove(dll, ec);
        fs::rename(out, dll, ec);
        if (ec)
            return Fail(status, "Cannot replace " + Utf8(dll) + ": close Devil's Third and try again.");
    }
    status.step = BuildStep::Done;
    return true;
}
} // namespace setup
