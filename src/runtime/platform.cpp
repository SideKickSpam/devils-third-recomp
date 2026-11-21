#include "platform.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <timeapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "winmm.lib")
#else
#include <csignal>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

namespace platform
{
namespace
{
CrashReporter gReporter = nullptr;
// Set from a signal or console handler; a watcher thread does the actual quitting outside the handler.
std::atomic<const char*> gQuitReason{ nullptr };
}

void InstallQuitSignals(void (*quit)(const char* reason))
{
#if defined(_WIN32)
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        const char* why = type == CTRL_C_EVENT ? "Ctrl+C" : type == CTRL_BREAK_EVENT ? "Ctrl+Break"
            : type == CTRL_CLOSE_EVENT ? "console window closed" : type == CTRL_LOGOFF_EVENT ? "logoff" : "shutdown";
        fprintf(stderr, "[dt-port] console control event: %s\n", why);
        fflush(stderr);
        gQuitReason = why;
        if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
            Sleep(INFINITE);  // Windows ends the process when this returns; let the watcher exit cleanly first
        return TRUE;
    }, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = [](int sig) { gQuitReason = sig == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM"; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
    std::thread([quit] {
        for (;;)
        {
            if (const char* why = gQuitReason.load())
                quit(why);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

#if defined(_WIN32)

void UsePreciseTimers() { timeBeginPeriod(1); }

void LowerCurrentThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

double ThreadCpuSeconds()
{
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    auto s = [](const FILETIME& t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7; };
    return s(kernel) + s(user);
}

uint8_t* ReserveGuestMemory(uint64_t size)
{
    // Committed up front: pages are zero-filled on first touch, but the commit charge is the full 4 GB.
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

void StartThread(std::function<void()> fn, size_t stackSize)
{
    auto* heapFn = new std::function<void()>(std::move(fn));
    const uintptr_t h = _beginthreadex(
        nullptr, unsigned(stackSize),
        [](void* arg) -> unsigned {
            auto* f = static_cast<std::function<void()>*>(arg);
            (*f)();
            delete f;
            return 0;
        },
        heapFn, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h)
        CloseHandle(reinterpret_cast<HANDLE>(h));
}

void InstallCrashHandler(CrashReporter report)
{
    gReporter = report;
    // Unhandled-exception filter rather than a vectored handler: C++ exceptions and SEH probing inside system
    // libraries are first-chance exceptions that must not be treated as crashes.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        const EXCEPTION_RECORD* r = info->ExceptionRecord;
        const char* what = "CPU exception";
        uintptr_t addr = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
        switch (r->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            what = "access violation";
            if (r->NumberParameters >= 2)
                addr = uintptr_t(r->ExceptionInformation[1]);
            break;
        case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer divide by zero"; break;
        }
        // Say where it happened: the exception code, the faulting instruction and the module it belongs to.
        static char detail[512];
        char module[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        const uintptr_t pc = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
        uintptr_t offset = pc;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(pc), &mod))
        {
            GetModuleFileNameA(mod, module, sizeof(module));
            offset = pc - reinterpret_cast<uintptr_t>(mod);
        }
        snprintf(detail, sizeof(detail), "%s (code 0x%08lX) at %s+0x%llX, thread %lu", what, r->ExceptionCode, module,
            (unsigned long long)offset, GetCurrentThreadId());
        if (gReporter)
            gReporter(detail, addr);
        return EXCEPTION_CONTINUE_SEARCH;
    });
}

void PrepareThreadForCrashes()
{
    // Leave room on the stack for the report after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

void GmTime(time_t t, struct tm* out) { gmtime_s(out, &t); }
void LocalTime(time_t t, struct tm* out) { localtime_s(out, &t); }
time_t TimeGm(struct tm* tm) { return _mkgmtime(tm); }
long UtcOffsetSeconds(time_t t)
{
    struct tm local;
    localtime_s(&local, &t);
    return long(_mkgmtime(&local) - t);
}

std::string RestrictCores(const std::string& which)
{
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
    if (!length || !GetSystemCpuSetInformation(info, length, &length, GetCurrentProcess(), 0))
        return "no CPU set information";
    // EfficiencyClass: higher is faster. Performance cores have the highest class, efficiency cores the lowest.
    std::vector<std::pair<ULONG, BYTE>> sets;  // id, efficiency class
    BYTE lo = 255, hi = 0;
    for (ULONG off = 0; off < length;)
    {
        auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + off);
        if (e->Type == CpuSetInformation)
        {
            sets.push_back({ e->CpuSet.Id, e->CpuSet.EfficiencyClass });
            lo = std::min(lo, e->CpuSet.EfficiencyClass), hi = std::max(hi, e->CpuSet.EfficiencyClass);
        }
        off += e->Size;
    }
    const size_t fast = std::count_if(sets.begin(), sets.end(), [&](auto& s) { return s.second == hi; });
    const std::string cores = std::to_string(fast) + " performance and " + std::to_string(sets.size() - fast) + " efficiency logical processors";
    if (lo == hi)
        return "not a hybrid CPU (" + std::to_string(sets.size()) + " logical processors)";
    if (which != "performance" && which != "efficiency")
        return cores + "; all used";
    std::vector<ULONG> ids;
    for (auto& [id, cls] : sets)
        if (cls == (which == "performance" ? hi : lo))
            ids.push_back(id);
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(), ULONG(ids.size())))
        return cores + "; SetProcessDefaultCpuSets failed";
    return cores + "; restricted to the " + std::to_string(ids.size()) + " " + which + " ones";
}

void* CurrentThreadHandle()
{
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    return h;
}

namespace
{
// Walks a native stack from a thread context with the images' unwind data (.pdata).
int WalkStack(CONTEXT c, uint64_t* out, int max)
{
    int n = 0;
    while (n < max && c.Rip)
    {
        out[n++] = c.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!f)
        {
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handler, &establisher, nullptr);
    }
    return n;
}

std::atomic<uint64_t> gWatchAddr{ 0 };
void (*gWatchReport)(const uint64_t*, int) = nullptr;
} // namespace

void WatchWrites(const void* addr, void (*report)(const uint64_t* stack, int depth))
{
    gWatchAddr = uint64_t(addr);
    gWatchReport = report;
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* info) -> LONG {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1))
            return EXCEPTION_CONTINUE_SEARCH;
        uint64_t stack[48];
        const int n = WalkStack(*info->ContextRecord, stack, 48);
        gWatchReport(stack, n);
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    });
    // Debug registers are per thread: keep them set on every thread of the process.
    std::thread([] {
        const DWORD self = GetCurrentThreadId();
        for (;;)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{ sizeof(te) };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
                    continue;
                HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!h)
                    continue;
                if (SuspendThread(h) != DWORD(-1))
                {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(h, &c) && c.Dr0 != gWatchAddr)
                    {
                        c.Dr0 = gWatchAddr;
                        c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);  // L0, break on write, 4 bytes
                        SetThreadContext(h, &c);
                    }
                    ResumeThread(h);
                }
                CloseHandle(h);
            }
            CloseHandle(snap);
            // Writes the debug registers cannot see (the kernel's, as ReadFile filling a buffer) show up here.
            static uint32_t last = 0;
            const uint32_t now = *reinterpret_cast<const volatile uint32_t*>(gWatchAddr.load());
            if (now != last)
                last = now, gWatchReport(nullptr, int(now));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

int SampleThreadStack(void* thread, uint64_t* out, int max)
{
    HANDLE h = HANDLE(thread);
    if (SuspendThread(h) == DWORD(-1))
        return 0;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int n = 0;
    if (GetThreadContext(h, &c))
        n = WalkStack(c, out, max);  // a function without unwind data is a leaf: its return address is at rsp
    ResumeThread(h);
    return n;
}

uint64_t ImageBase() { return uint64_t(GetModuleHandleW(nullptr)); }

std::string ModuleOf(uint64_t addr, uint64_t& base)
{
    HMODULE m = nullptr;
    base = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr), &m))
        return "";
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(m, path, MAX_PATH);
    base = uint64_t(m);
    const std::string p(path, len);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

int64_t FileTell(FILE* f) { return _ftelli64(f); }
int FileSeek(FILE* f, int64_t offset) { return _fseeki64(f, offset, SEEK_SET); }
int64_t FileSize(FILE* f)
{
    struct _stat64 st;
    return _fstat64(_fileno(f), &st) == 0 ? int64_t(st.st_size) : -1;
}

#else

void UsePreciseTimers() {}

void LowerCurrentThreadPriority() { setpriority(PRIO_PROCESS, 0, 5); }  // Linux: per-thread nice value

double ThreadCpuSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::string RestrictCores(const std::string& which)
{
    return which == "all" ? "all cores" : "cpu.cores is only supported on Windows";
}

void* CurrentThreadHandle() { return nullptr; }
void WatchWrites(const void*, void (*)(const uint64_t*, int)) {}
int SampleThreadStack(void*, uint64_t*, int) { return 0; }
uint64_t ImageBase() { return 0; }
std::string ModuleOf(uint64_t, uint64_t& base)
{
    base = 0;
    return "";
}

uint8_t* ReserveGuestMemory(uint64_t size)
{
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
}

void StartThread(std::function<void()> fn, size_t stackSize)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto* heapFn = new std::function<void()>(std::move(fn));
    pthread_t t;
    pthread_create(
        &t, &attr,
        [](void* arg) -> void* {
            auto* f = static_cast<std::function<void()>*>(arg);
            (*f)();
            delete f;
            return nullptr;
        },
        heapFn);
    pthread_attr_destroy(&attr);
}

void InstallCrashHandler(CrashReporter report)
{
    gReporter = report;
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = [](int sig, siginfo_t* info, void*) {
        if (gReporter)
            gReporter(strsignal(sig), reinterpret_cast<uintptr_t>(info->si_addr));
        _exit(7);
    };
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE })
        sigaction(sig, &sa, nullptr);
}

void PrepareThreadForCrashes()
{
    // Alternate signal stack so the handler still runs after a stack overflow.
    stack_t ss{};
    ss.ss_size = 64 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, nullptr);
}

void GmTime(time_t t, struct tm* out) { gmtime_r(&t, out); }
void LocalTime(time_t t, struct tm* out) { localtime_r(&t, out); }
time_t TimeGm(struct tm* tm) { return timegm(tm); }
long UtcOffsetSeconds(time_t t)
{
    struct tm local;
    localtime_r(&t, &local);
    return local.tm_gmtoff;
}

int64_t FileTell(FILE* f) { return ftello(f); }
int FileSeek(FILE* f, int64_t offset) { return fseeko(f, off_t(offset), SEEK_SET); }
int64_t FileSize(FILE* f)
{
    struct stat st;
    return fstat(fileno(f), &st) == 0 ? int64_t(st.st_size) : -1;
}

#endif
} // namespace platform
