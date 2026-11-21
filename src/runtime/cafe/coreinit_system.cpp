// coreinit: time, system information, console output, errno, process exit.
#include "../imports.h"
#include "../log.h"
#include "../platform.h"
#include "../trace.h"
#include "coreinit.h"

#include <chrono>
#include <ctime>

namespace cafe::coreinit
{
namespace
{
const auto gStart = std::chrono::steady_clock::now();
uint32_t gSystemInfo = 0;

// ---- guest printf ----
// Reads variadic arguments the way the PowerPC EABI passes them: GPRs up to r10, FPRs up to f8, then the
// caller's parameter save area on the stack.
struct VarArgs
{
    PPCContext& ctx;
    int gpr, fpr = 1;
    uint32_t stack;

    VarArgs(PPCContext& c, int firstGpr) : ctx(c), gpr(firstGpr), stack(c.r[1].u32 + 8) {}
    uint32_t U32()
    {
        if (gpr <= 10)
            return ctx.r[gpr++].u32;
        const uint32_t v = guest::Read32(stack);
        stack += 4;
        return v;
    }
    uint64_t U64()
    {
        if (gpr <= 9)
        {
            if (!(gpr & 1))
                gpr++;
            const uint64_t v = uint64_t(ctx.r[gpr].u32) << 32 | ctx.r[gpr + 1].u32;
            gpr += 2;
            return v;
        }
        gpr = 11;
        stack = guest::AlignUp(stack, 8);
        const uint64_t v = guest::Read64(stack);
        stack += 8;
        return v;
    }
    double F64()
    {
        if (fpr <= 8)
            return ctx.f[fpr++].ps0;
        stack = guest::AlignUp(stack, 8);
        uint64_t bits = guest::Read64(stack);
        stack += 8;
        return ppc_from_bits(bits);
    }
};

std::string FormatGuest(const std::string& fmt, VarArgs& va)
{
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++)
    {
        if (fmt[i] != '%')
        {
            out += fmt[i];
            continue;
        }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && strchr("-+ #0", fmt[j]))
            spec += fmt[j++];
        auto number = [&] {
            if (j < fmt.size() && fmt[j] == '*')
            {
                spec += std::to_string(int32_t(va.U32()));
                j++;
            }
            while (j < fmt.size() && isdigit(static_cast<unsigned char>(fmt[j])))
                spec += fmt[j++];
        };
        number();
        if (j < fmt.size() && fmt[j] == '.')
        {
            spec += fmt[j++];
            number();
        }
        int longs = 0;
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j]))
            longs += fmt[j++] == 'l' ? 1 : (fmt[j - 1] == 'L' || fmt[j - 1] == 'q' || fmt[j - 1] == 'j') ? 2 : 0;
        if (j >= fmt.size())
            break;
        const char conv = fmt[j];
        char buf[512];
        switch (conv)
        {
        case 'd': case 'i':
            if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)va.U64());
            else snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(va.U32()));
            out += buf;
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (longs >= 2) snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)va.U64());
            else snprintf(buf, sizeof(buf), (spec + conv).c_str(), va.U32());
            out += buf;
            break;
        case 'p': snprintf(buf, sizeof(buf), "0x%08X", va.U32()); out += buf; break;
        case 'c': out += char(va.U32()); break;
        case 's':
        {
            const std::string s = guest::ReadString(va.U32());
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), va.F64());
            out += buf;
            break;
        case '%': out += '%'; break;
        default: out += spec + conv; break;
        }
        i = j;
    }
    return out;
}

void Print(const char* prefix, const std::string& s)
{
    std::string text = s;
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    dtlog::Important("[%s] %s\n", prefix, text.c_str());
}

// ---- raw (varargs) imports ----
void OSReport(PPCContext& ctx, uint8_t*)
{
    VarArgs va(ctx, 4);
    Print("OSReport", FormatGuest(guest::ReadString(ctx.r[3].u32), va));
}

void OSPanic(PPCContext& ctx, uint8_t*)
{
    // OSPanic(const char* file, int line, const char* fmt, ...)
    VarArgs va(ctx, 6);
    const std::string msg = FormatGuest(guest::ReadString(ctx.r[5].u32), va);
    trace::Stop(8, "OSPanic at %s:%d: %s", guest::ReadString(ctx.r[3].u32).c_str(), ctx.r[4].s32, msg.c_str());
}

void OSFatal(GuestString msg) { trace::Stop(8, "OSFatal: %s", msg.str().c_str()); }

void OSConsoleWrite(uint32_t buf, uint32_t size)
{
    Print("console", std::string(reinterpret_cast<const char*>(guest::Ptr(buf)), size));
}

// ---- time ----
// As on the Wii U, OSTime is the boot time (local wall-clock time, read once) plus the monotonic system timer: it never
// jumps when the host clock is adjusted, and it is cheap (the game's frame limiter calls it ~50,000 times a frame).
int64_t gBaseTime = 0;
int64_t OSGetTime() { return gBaseTime + int64_t(SystemTicks()); }
int64_t OSGetSystemTime() { return int64_t(SystemTicks()); }
uint32_t OSGetTick() { return uint32_t(SystemTicks()); }

// OSCalendarTime: sec, min, hour, mday, mon (0-11), year, wday, yday, msec, usec (all int32).
void OSTicksToCalendarTime(int64_t ticks, uint32_t out)
{
    const int64_t totalUs = TicksToUs(ticks);
    const time_t secs = time_t(totalUs / 1000000) + 946684800;  // epoch 2000-01-01
    struct tm tm;
    platform::GmTime(secs, &tm);
    const int32_t fields[] = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year + 1900, tm.tm_wday,
        tm.tm_yday, int32_t((totalUs / 1000) % 1000), int32_t(totalUs % 1000) };
    for (int i = 0; i < 10; i++)
        guest::Write32(out + i * 4, uint32_t(fields[i]));
}

int64_t OSCalendarTimeToTicks(uint32_t in)
{
    auto f = [&](int i) { return int32_t(guest::Read32(in + i * 4)); };
    struct tm tm{};
    tm.tm_sec = f(0), tm.tm_min = f(1), tm.tm_hour = f(2), tm.tm_mday = f(3), tm.tm_mon = f(4), tm.tm_year = f(5) - 1900;
    const int64_t secs = int64_t(platform::TimeGm(&tm)) - 946684800;
    return secs * int64_t(TimerHz) + (int64_t(f(8)) * 1000 + f(9)) * int64_t(TimerHz) / 1000000;
}

// ---- system info ----
uint32_t OSGetSystemInfo() { return gSystemInfo; }
bool OSIsDebuggerInitialized() { return false; }
uint32_t bspGetHardwareVersion(uint32_t out)
{
    if (out)
        guest::Write32(out, 0x26100010);  // retail Wii U (Latte ACR)
    return 0;
}

// ---- errno (per thread) ----
uint32_t ErrnoAddr()
{
    Thread* t = CurrentThread();
    static uint32_t hostErrno = guest::SystemAlloc(4);
    if (!t)
        return hostErrno;
    if (!t->errnoAddr)
        t->errnoAddr = guest::SystemAlloc(4);
    return t->errnoAddr;
}
uint32_t __gh_errno_ptr() { return ErrnoAddr(); }
void __gh_set_errno(int32_t v) { guest::Write32(ErrnoAddr(), uint32_t(v)); }

// ---- process exit ----
void Exit(int32_t code)
{
    // As Cafe OS's exit does through the C library's exit handlers: flush the game's stdio first, so buffered output
    // (UE3 prints its fatal error message with wprintf) reaches OSConsoleWrite and the log.
    static const uint32_t fflush = trace::FindSymbol("fflush");
    if (fflush && guest::CurrentContext())
        guest::Call(fflush, { 0 });
    trace::Stop(0, "game called exit(%d)", code);
}
} // namespace

CAFE_IMPORT_RAW(coreinit, "OSReport", OSReport);
CAFE_IMPORT_RAW(coreinit, "OSPanic", OSPanic);
CAFE_IMPORT(coreinit, "OSFatal", OSFatal);
CAFE_IMPORT(coreinit, "OSConsoleWrite", OSConsoleWrite);
CAFE_IMPORT(coreinit, "OSGetTime", OSGetTime);
CAFE_IMPORT(coreinit, "OSGetSystemTime", OSGetSystemTime);
CAFE_IMPORT(coreinit, "OSGetTick", OSGetTick);
CAFE_IMPORT(coreinit, "OSTicksToCalendarTime", OSTicksToCalendarTime);
CAFE_IMPORT(coreinit, "OSCalendarTimeToTicks", OSCalendarTimeToTicks);
CAFE_IMPORT(coreinit, "OSGetSystemInfo", OSGetSystemInfo);
CAFE_IMPORT(coreinit, "OSIsDebuggerInitialized", OSIsDebuggerInitialized);
CAFE_IMPORT(coreinit, "bspGetHardwareVersion", bspGetHardwareVersion);
CAFE_IMPORT(coreinit, "__gh_errno_ptr", __gh_errno_ptr);
CAFE_IMPORT(coreinit, "__gh_set_errno", __gh_set_errno);
CAFE_IMPORT(coreinit, "exit", Exit);
CAFE_IMPORT(coreinit, "_Exit", Exit);

uint64_t SystemTicks()
{
    const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gStart).count());
    // Whole seconds and the remainder separately: ns * TimerHz overflows 64 bits after 297 s (and the clock then
    // jumped back by that much, which hung UE3's frame limiter).
    return ns / 1000000000ull * TimerHz + ns % 1000000000ull * TimerHz / 1000000000ull;
}

int64_t OSTimeNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const time_t t = system_clock::to_time_t(now);
    const int64_t us = duration_cast<microseconds>(now.time_since_epoch()).count() + int64_t(platform::UtcOffsetSeconds(t)) * 1000000;
    return UsToTicks(us - int64_t(946684800) * 1000000);
}

void InitSystem()
{
    // OSSystemInfo: busClockSpeed, coreClockSpeed, baseTime (OSTime at boot), L2 cache sizes per core.
    gSystemInfo = guest::SystemAlloc(0x20);
    guest::Write32(gSystemInfo + 0x0, BusHz);
    guest::Write32(gSystemInfo + 0x4, CoreHz);
    gBaseTime = OSTimeNow() - int64_t(SystemTicks());
    guest::Write64(gSystemInfo + 0x8, uint64_t(gBaseTime));
    guest::Write32(gSystemInfo + 0x10, 512 * 1024);
    guest::Write32(gSystemInfo + 0x14, 2048 * 1024);
    guest::Write32(gSystemInfo + 0x18, 512 * 1024);
    dtlog::Write("[dt-port] OSSystemInfo at 0x%08X\n", gSystemInfo);
}
} // namespace cafe::coreinit
