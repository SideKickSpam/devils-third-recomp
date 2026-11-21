// Runtime hooks called directly by recompiled code (traps, unsupported instructions, timebase, FPSCR).
#include "guest.h"
#include "trace.h"

#include <cfenv>
#include <chrono>

#include "cafe/coreinit.h"

void ppc_trap(PPCContext& ctx, uint32_t addr)
{
    trace::Stop(5, "trap instruction at %s (lr 0x%08X)", trace::Describe(addr).c_str(), ctx.lr);
}

void ppc_unsupported(PPCContext& ctx, uint32_t addr, const char* what)
{
    trace::Stop(5, "unsupported instruction %s at %s (lr 0x%08X)", what, trace::Describe(addr).c_str(), ctx.lr);
}

void ppc_bad_branch(PPCContext& ctx, uint32_t from, uint32_t to)
{
    trace::Stop(5, "branch from %s to 0x%08X, which is not a function entry (lr 0x%08X)", trace::Describe(from).c_str(), to, ctx.lr);
}

void ppc_syscall(PPCContext& ctx, uint8_t*, uint32_t addr)
{
    trace::Stop(5, "system call at 0x%08X (r0 = 0x%08X)", addr, ctx.r[0].u32);
}

uint64_t ppc_timebase() { return cafe::coreinit::SystemTicks(); }

void ppc_fpscr_changed(PPCContext& ctx)
{
    static const int modes[] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[ctx.fpscr & 3]);
}
