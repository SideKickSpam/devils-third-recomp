// Frame-rate independence for the game's UnrealScript (display.fps_cap above 50).
//
// The game was made for a fixed 50 Hz tick on the Wii U, and some of its script adds a constant every tick instead of
// an amount scaled by the frame time (a mash counter that counts 2 a tick, a value eased by a fixed Lerp alpha per
// tick...). At 144 fps such code runs nearly three times as fast.
//
// The script operators that accumulate (+=, -=, *=, ++, --, Lerp) are hooked here. Their natives are reimplemented the
// way the interpreter runs them (evaluate the left side as a reference, then the right side, then apply), so each call
// site can be:
//  - profiled (DT_FRAMETIE_PROFILE=<csv>): per site (script function and bytecode offset) how often it runs and what it
//    adds. Comparing a 50 fps run with an uncapped one finds the frame-tied sites: they run more often per second but
//    add the same amount each time, where correct code adds less;
//  - fixed: sites listed in kFixes get their amount scaled to the frame time, as at 50 Hz: an addition by dt / 0.02,
//    a factor or a Lerp alpha compounded to the same rate. The scale is at most 1, and at the 50 fps cap nothing
//    changes, so the original's behaviour is kept exactly there.
#include "frametie.h"

#include "game_patches.h"
#include "guest.h"
#include "hooks.h"
#include "host/input.h"
#include "log.h"
#include "settings.h"
#include "ue3.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace frametie
{
namespace
{
uint64_t gTotalTicks = 0;  // engine ticks so far
}
namespace
{
// The interpreter in this build.
constexpr uint32_t GNatives = 0x104DA240;     // FNativeFuncPtr GNatives[]: 8-byte GHS member-function pointers
constexpr uint32_t GPropAddr = 0x104635C0;    // BYTE* GPropAddr: the variable an lvalue expression evaluated to
constexpr uint32_t GPropObject = 0x104635C4;  // UObject* GPropObject: its owner, told when it changes (net dirty)
constexpr uint32_t GProperty = 0x104635A0;    // UProperty* GProperty
constexpr uint32_t FrameNode = 0x10, FrameObject = 0x14, FrameCode = 0x18;  // FFrame
constexpr float BaseTick = 0.02f;  // the game's own 50 Hz

enum class Op : uint8_t
{
    AddF, SubF, MulF, AddI, SubI, AddB, SubB, PreIncI, PreDecI, PostIncI, PostDecI, PreIncB, PreDecB, PostIncB, PostDecB,
    AddV, SubV, MulVF, AddR, SubR, Lerp, FRand, Rand,
};
const char* const kOpNames[] = { "+=f", "-=f", "*=f", "+=i", "-=i", "+=b", "-=b", "++i", "--i", "i++", "i--", "++b",
                                 "--b", "b++", "b--", "+=v", "-=v", "*=vf", "+=r", "-=r", "lerp", "frand", "rand" };

// Sites made frame-rate independent: the script function's path name and the bytecode offset of the operator's first
// operand (as the profile lists them).
struct Fix
{
    const char* function;
    uint32_t offset;           // the operator's first operand in the bytecode (as the profile lists it), or
    const char* variable = nullptr;  // every site in the function whose left operand is this property
};
const Fix kFixes[] = {
    // How long the run/dodge button (B) has been held and the sprint's start delay, in ticks: a tap dodges, a hold
    // runs. Counted per tick, the hold became a run sooner and the delay ran out sooner at higher frame rates.
    { "DevilsThirdGame.DevilsThirdPlayerInput:PlayerInput", 0, "bRunHold" },      // ++ while bRun, up to 255
    { "DevilsThirdGame.DevilsThirdPlayerInput:PlayerInput", 0, "bDodgeHold" },    // ++ while bDodge, up to 255
    { "DevilsThirdGame.DevilsThirdPlayerInput:PlayerInput", 0, "bSprintDelay" },  // -- while above 0
    // Escaping an enemy's throw: 2 counts every tick either stick moved more than 0.05 since the last tick.
    { "DevilsThirdGame.DevilsThirdPlayerPawn:UpdateThrowEscapeCount", 0, "m_ThrowEscapeCount" },
};

struct Site
{
    uint32_t node = 0;
    Op op{};
    std::string function;
    uint32_t offset = 0;
    bool fix = false;
    uint64_t calls = 0;
    double sum = 0, sumAbs = 0;
    // Continuity: the variable still holds what this site left in it last time (an accumulator, not a value rebuilt
    // every call).
    // Only the first call in a tick is compared with the last tick's: a loop counter starts again every tick.
    uint32_t lastVar = 0, last[3] = {};
    uint64_t lastTick = ~0ull, tickChecks = 0, continuous = 0;
    bool logShots = false;  // D3Weapon.NotifyWeaponFired, under DT_LOG_SHOTS
};

std::unordered_map<uint32_t, Site> gSites;  // by the code address of the first operand
std::unordered_map<uint32_t, float> gRemainders;  // integer variables under a fix: the fraction not yet added
bool gProfile = false;
std::string gProfilePath;
bool gFixing = false;  // a cap above 50 fps and a fix table
float gScale = 1;      // this tick's dt / 0.02, at most 1
thread_local bool tGameThread = false;
int gScriptOffset = -1;  // UStruct::Script (a TArray<BYTE>) in this build, found from the first site
uint64_t gTicks = 0;  // engine ticks in this profile segment, in all
double gSeconds = 0;
std::chrono::steady_clock::time_point gLastDump;

float F(uint32_t a) { return std::bit_cast<float>(guest::Read32(a)); }
void SetF(uint32_t a, float v) { guest::Write32(a, std::bit_cast<uint32_t>(v)); }

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5)
{
    ctx.r[3].u32 = r3;
    ctx.r[4].u32 = r4;
    ctx.r[5].u32 = r5;
    const uint64_t lr = ctx.lr;
    ctx.lr = 0;
    ppc_lookup(fn)(ctx, base);
    ctx.lr = lr;
}

// FFrame::Step: run the next expression, its value to `result`.
void Step(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t result)
{
    const uint32_t code = guest::Read32(frame + FrameCode);
    guest::Write32(frame + FrameCode, code + 1);
    const uint32_t entry = GNatives + guest::Read8(code) * 8;
    const int16_t delta = int16_t(guest::Read16(entry)), index = int16_t(guest::Read16(entry + 2));
    const uint32_t self = guest::Read32(frame + FrameObject) + delta;
    uint32_t fn;
    if (index < 0)
        fn = guest::Read32(entry + 4);
    else
        fn = guest::Read32(guest::Read32(self + int16_t(guest::Read16(entry + 6))) + index * 8 + 4);
    CallGuest(ctx, base, fn, self, frame, result);
}

// P_GET_*_REF: an lvalue's address (or `temp` if the expression was no variable), telling its owner it changed.
uint32_t StepRef(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t temp)
{
    guest::Write32(GPropAddr, 0);
    Step(ctx, base, frame, temp);
    if (const uint32_t owner = guest::Read32(GPropObject))
        CallGuest(ctx, base, guest::Read32(guest::Read32(owner) + 0x18C), owner, guest::Read32(GProperty), 0);
    const uint32_t at = guest::Read32(GPropAddr);
    return at ? at : temp;
}

int FindScriptOffset(uint32_t node, uint32_t code)
{
    for (int off = 0x50; off < 0xC0; off += 4)
    {
        const uint32_t data = guest::Read32(node + off), num = guest::Read32(node + off + 4);
        if (data && num < 0x100000 && code >= data && code < data + num)
            return off;
    }
    return -1;
}

// A plausible UObject pointer (its class's class exists too), so naming it cannot fault.
bool LooksLikeObject(uint32_t p)
{
    auto heap = [](uint32_t a) { return a >= 0x10000000 && a < 0x60000000 && !(a & 3); };
    if (!heap(p))
        return false;
    const uint32_t cls = guest::Read32(p + 0x34);
    return heap(cls) && heap(guest::Read32(cls + 0x34)) && guest::Read32(guest::Read32(cls + 0x34) + 0x34) ==
                                                                 guest::Read32(cls + 0x34);
}

// DT_FRAMETIE_DUMP=<text>: the bytecode of each script function whose path contains the text, with every embedded
// object pointer (variables, functions, classes) named, into <profile>.<function>.txt (testing).
void DumpScript(uint32_t node, const std::string& function)
{
    static const char* want = getenv("DT_FRAMETIE_DUMP");
    static std::vector<uint32_t> done;
    if (!want || !*want || gScriptOffset < 0)
        return;
    bool match = false;  // "text;text"
    for (std::string list = want; !list.empty();)
    {
        const size_t semi = list.find(';');
        const std::string item = list.substr(0, semi);
        match |= !item.empty() && function.find(item) != std::string::npos;
        list = semi == std::string::npos ? "" : list.substr(semi + 1);
    }
    if (!match)
        return;
    for (uint32_t d : done)
        if (d == node)
            return;
    done.push_back(node);
    const uint32_t data = guest::Read32(node + gScriptOffset), num = guest::Read32(node + gScriptOffset + 4);
    std::string name = function;
    for (char& c : name)
        if (c == ':' || c == '.' || c == '/' || c == '\\')
            c = '_';
    FILE* f = fopen((gProfilePath + "." + name + ".txt").c_str(), "w");
    if (!f)
        return;
    fprintf(f, "%s: %u bytes\n", function.c_str(), num);
    for (uint32_t i = 0; i < num; i += 16)
    {
        fprintf(f, "%04X ", i);
        for (uint32_t j = i; j < i + 16 && j < num; j++)
            fprintf(f, " %02X", guest::Read8(data + j));
        fprintf(f, "\n");
    }
    fprintf(f, "\nobject pointers:\n");
    for (uint32_t i = 0; i + 4 <= num; i++)
    {
        const uint32_t p = guest::Read32(data + i);
        if (LooksLikeObject(p))
            fprintf(f, "%04X %s %s\n", i, ue3::ClassName(p).c_str(), ue3::PathName(p).c_str());
    }
    fclose(f);
}

// Whether the operator's left operand (the expression at `code`) is the property `name`: the first property it names
// (an instance, local or default variable: a token then an 8-byte slot holding the pointer), or for a context
// expression (Outer.X) the last one before the operand's end, approximated as the first 40 bytes.
bool LeftOperandIs(uint32_t code, const char* name)
{
    for (uint32_t i = 0; i < 40; i++)
    {
        if (guest::Read32(code + i) != 0)
            continue;
        const uint32_t p = guest::Read32(code + i + 4);
        if (!LooksLikeObject(p))
            continue;
        const std::string path = ue3::PathName(p);
        const size_t cut = path.find_last_of(".:");
        if (cut != std::string::npos && path.compare(cut + 1, std::string::npos, name) == 0)
            return true;
    }
    return false;
}

Site* Lookup(uint32_t frame, uint32_t code, Op op)
{
    const uint32_t node = guest::Read32(frame + FrameNode);
    // A direct-mapped cache in front of the map (Game Thread only; the map's elements never move).
    struct CacheEntry
    {
        uint32_t code, node;
        Site* site;
    };
    static CacheEntry cache[1024] = {};
    CacheEntry& c = cache[(code >> 1) & 1023];
    if (c.site && c.code == code && c.node == node)
        return c.site;
    auto it = gSites.find(code);
    if (it != gSites.end() && it->second.node == node)
    {
        c = { code, node, &it->second };
        return &it->second;
    }
    Site& s = gSites[code];
    c = { code, node, &s };
    s = Site{};
    s.node = node;
    s.op = op;
    if (gScriptOffset < 0)
    {
        gScriptOffset = FindScriptOffset(node, code);
        dtlog::Important("[frametie] UStruct::Script at +0x%X (first site: %s)\n", unsigned(gScriptOffset),
                         ue3::PathName(node).c_str());
    }
    s.function = ue3::PathName(node);
    s.offset = gScriptOffset >= 0 ? code - guest::Read32(node + gScriptOffset) : code;
    for (const Fix& f : kFixes)
        if (f.function && s.function == f.function && (f.variable ? LeftOperandIs(code, f.variable) : f.offset == s.offset))
            s.fix = true;
    s.logShots = s.function == "DevilsThirdGame.D3Weapon:NotifyWeaponFired" && getenv("DT_LOG_SHOTS");
    if (s.fix)
        dtlog::Important("[frametie] fixing %s +0x%X (%s)\n", s.function.c_str(), s.offset, kOpNames[int(s.op)]);
    DumpScript(node, s.function);
    return &s;
}

void Note(Site* s, double value)
{
    if (!gProfile)
        return;
    s->calls++;
    s->sum += value;
    s->sumAbs += std::fabs(value);
}

int Words(Op op)
{
    switch (op)
    {
    case Op::AddV: case Op::SubV: case Op::MulVF: case Op::AddR: case Op::SubR: return 3;
    default: return 1;
    }
}

bool IsByte(Op op)
{
    return op == Op::AddB || op == Op::SubB || op == Op::PreIncB || op == Op::PreDecB || op == Op::PostIncB ||
           op == Op::PostDecB;
}

void ReadWords(Op op, uint32_t at, uint32_t out[3])
{
    const int n = Words(op);
    for (int i = 0; i < n; i++)
        out[i] = IsByte(op) ? guest::Read8(at) : guest::Read32(at + i * 4);
}

void CheckContinuity(Site* s, uint32_t var, const uint32_t before[3])
{
    if (!gProfile || s->lastTick == gTotalTicks)
        return;
    if (s->lastTick + 1 == gTotalTicks)
    {
        s->tickChecks++;
        if (s->lastVar == var && memcmp(s->last, before, 4 * Words(s->op)) == 0)
            s->continuous++;
    }
    s->lastTick = gTotalTicks;
}

void Remember(Site* s, uint32_t var, const uint32_t after[3])
{
    s->lastVar = var;
    memcpy(s->last, after, 4 * Words(s->op));
}

// An integer amount under a fix, scaled with the fraction carried per variable.
int32_t ScaleInt(uint32_t var, int32_t amount)
{
    float& rem = gRemainders[var];
    const float scaled = float(amount) * gScale + rem;
    const int32_t whole = int32_t(std::trunc(scaled));
    rem = scaled - float(whole);
    return whole;
}

float ScaleFactor(float factor) { return factor > 0 ? std::pow(factor, gScale) : factor; }

// DT_LOG_SHOTS=<csv>: every weapon shot (D3Weapon.NotifyWeaponFired) with the time and the owner's class, for
// comparing enemy fire at different frame rates (testing).
void LogShot(uint32_t weapon)
{
    static FILE* out = nullptr;
    static const auto start = std::chrono::steady_clock::now();
    if (!out)
    {
        out = fopen(getenv("DT_LOG_SHOTS"), "w");
        if (!out)
            return;
        fprintf(out, "t,weapon,owner,owner_class,weapon_class\n");
    }
    const uint32_t owner = ue3::GetObject(weapon, "Owner").value_or(0);
    fprintf(out, "%.4f,%08X,%08X,%s,%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
            weapon, owner, owner ? ue3::ClassName(owner).c_str() : "", ue3::ClassName(weapon).c_str());
    fflush(out);
}

template <uint32_t addr, Op op>
PPC_FUNC(Operator)
{
    const uint32_t frame = ctx.r[4].u32, result = ctx.r[5].u32;
    static const bool logShots = getenv("DT_LOG_SHOTS") != nullptr;
    if (logShots && tGameThread)
    {
        Site* site = Lookup(frame, guest::Read32(frame + FrameCode), op);
        if (site->logShots)
            LogShot(guest::Read32(frame + FrameObject));
        if (!gProfile && !(gFixing && site->fix))
        {
            hooks::Original(addr)(ctx, base);
            return;
        }
    }
    if (!tGameThread || (!gProfile && !gFixing))
    {
        hooks::Original(addr)(ctx, base);
        return;
    }
    const uint32_t code = guest::Read32(frame + FrameCode);
    Site* site = Lookup(frame, code, op);
    if (!gProfile && !site->fix)
    {
        hooks::Original(addr)(ctx, base);  // unchanged: the game's own native
        return;
    }
    const bool fix = gFixing && site->fix;
    const uint32_t sp = ctx.r[1].u32, frameSp = sp - 64;
    guest::Write32(frameSp, sp);
    ctx.r[1].u32 = frameSp;
    const uint32_t tempA = frameSp + 16, tempB = frameSp + 32, tempC = frameSp + 48;
    memset(guest::base + tempA, 0, 44);

    if constexpr (op == Op::Lerp)
    {
        Step(ctx, base, frame, tempA);
        Step(ctx, base, frame, tempB);
        Step(ctx, base, frame, tempC);
        float alpha = F(tempC);
        Note(site, alpha < 1 ? -std::log(1 - alpha) : 0);
        if (fix && alpha > 0 && alpha < 1)
            alpha = 1 - std::pow(1 - alpha, gScale);
        const float a = F(tempA), b = F(tempB);
        SetF(result, a + alpha * (b - a));
        const uint32_t before[3] = { guest::Read32(tempA) }, after[3] = { guest::Read32(result) };
        CheckContinuity(site, 1, before);
        Remember(site, 1, after);
    }
    else
    {
        const uint32_t var = StepRef(ctx, base, frame, tempA);
        uint32_t before[3] = {}, after[3] = {};
        ReadWords(op, var, before);
        CheckContinuity(site, var, before);
        constexpr bool unary = op == Op::PreIncI || op == Op::PreDecI || op == Op::PostIncI || op == Op::PostDecI ||
                               op == Op::PreIncB || op == Op::PreDecB || op == Op::PostIncB || op == Op::PostDecB;
        if constexpr (!unary)
            Step(ctx, base, frame, tempB);
        if constexpr (op == Op::AddF || op == Op::SubF)
        {
            float b = F(tempB);
            Note(site, op == Op::AddF ? b : -b);
            if (fix)
                b *= gScale;
            const float v = op == Op::AddF ? F(var) + b : F(var) - b;
            SetF(var, v);
            SetF(result, v);
        }
        else if constexpr (op == Op::MulF)
        {
            float b = F(tempB);
            Note(site, b > 0 ? std::log(b) : 0);
            if (fix)
                b = ScaleFactor(b);
            const float v = F(var) * b;
            SetF(var, v);
            SetF(result, v);
        }
        else if constexpr (op == Op::AddI || op == Op::SubI)
        {
            int32_t b = int32_t(guest::Read32(tempB));
            Note(site, op == Op::AddI ? b : -b);
            if (fix)
                b = ScaleInt(var, b);
            const int32_t v = op == Op::AddI ? int32_t(guest::Read32(var)) + b : int32_t(guest::Read32(var)) - b;
            guest::Write32(var, uint32_t(v));
            guest::Write32(result, uint32_t(v));
        }
        else if constexpr (op == Op::AddB || op == Op::SubB)
        {
            int32_t b = guest::Read8(tempB);
            Note(site, op == Op::AddB ? b : -b);
            if (fix)
                b = ScaleInt(var, b);
            const uint8_t v = uint8_t(op == Op::AddB ? guest::Read8(var) + b : guest::Read8(var) - b);
            guest::Write8(var, v);
            guest::Write8(result, v);
        }
        else if constexpr (op == Op::PreIncI || op == Op::PreDecI || op == Op::PostIncI || op == Op::PostDecI)
        {
            const bool inc = op == Op::PreIncI || op == Op::PostIncI;
            Note(site, inc ? 1 : -1);
            const int32_t step = fix ? ScaleInt(var, 1) : 1;
            const int32_t old = int32_t(guest::Read32(var)), v = inc ? old + step : old - step;
            guest::Write32(var, uint32_t(v));
            guest::Write32(result, uint32_t(op == Op::PreIncI || op == Op::PreDecI ? v : old));
        }
        else if constexpr (op == Op::PreIncB || op == Op::PreDecB || op == Op::PostIncB || op == Op::PostDecB)
        {
            const bool inc = op == Op::PreIncB || op == Op::PostIncB;
            Note(site, inc ? 1 : -1);
            const int32_t step = fix ? ScaleInt(var, 1) : 1;
            const uint8_t old = guest::Read8(var), v = uint8_t(inc ? old + step : old - step);
            guest::Write8(var, v);
            guest::Write8(result, op == Op::PreIncB || op == Op::PreDecB ? v : old);
        }
        else if constexpr (op == Op::AddV || op == Op::SubV)
        {
            float b[3] = { F(tempB), F(tempB + 4), F(tempB + 8) };
            Note(site, std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
            for (int i = 0; i < 3; i++)
            {
                const float bi = fix ? b[i] * gScale : b[i];
                const float v = op == Op::AddV ? F(var + i * 4) + bi : F(var + i * 4) - bi;
                SetF(var + i * 4, v);
                SetF(result + i * 4, v);
            }
        }
        else if constexpr (op == Op::MulVF)
        {
            float b = F(tempB);
            Note(site, b > 0 ? std::log(b) : 0);
            if (fix)
                b = ScaleFactor(b);
            for (int i = 0; i < 3; i++)
            {
                const float v = F(var + i * 4) * b;
                SetF(var + i * 4, v);
                SetF(result + i * 4, v);
            }
        }
        else if constexpr (op == Op::AddR || op == Op::SubR)
        {
            int32_t b[3] = { int32_t(guest::Read32(tempB)), int32_t(guest::Read32(tempB + 4)),
                             int32_t(guest::Read32(tempB + 8)) };
            Note(site, std::abs(b[0]) + std::abs(b[1]) + std::abs(b[2]));
            for (int i = 0; i < 3; i++)
            {
                const int32_t bi = fix ? ScaleInt(var + i * 4, b[i]) : b[i];
                const int32_t old = int32_t(guest::Read32(var + i * 4));
                const int32_t v = op == Op::AddR ? old + bi : old - bi;
                guest::Write32(var + i * 4, uint32_t(v));
                guest::Write32(result + i * 4, uint32_t(v));
            }
        }
        ReadWords(op, var, after);
        Remember(site, var, after);
    }
    guest::Write32(frame + FrameCode, guest::Read32(frame + FrameCode) + 1);  // P_FINISH: EX_EndFunctionParms
    ctx.r[1].u32 = sp;
}

// PhysX: how UE3 steps the rigid-body scenes (TickRBPhysScene -> NpScene::setTiming, simulate). Logged when the timing
// changes and as a summary every 10 s, to see how physics follows the frame rate (DT_LOG_PHYSICS, testing).
constexpr uint32_t NpScene_setTiming = 0x048389A0, NpScene_simulate = 0x0483966C;
struct PhysTiming
{
    float maxTimestep = -1;
    uint32_t maxIter = 0, method = 0;
};
std::mutex gPhysMutex;
std::unordered_map<uint32_t, PhysTiming> gPhysTiming;  // per scene
uint64_t gSimulates = 0;
double gSimulated = 0;

const bool gLogPhysics = getenv("DT_LOG_PHYSICS") != nullptr;

PPC_FUNC(PhysSetTiming)
{
    if (!gLogPhysics)
    {
        hooks::Original(NpScene_setTiming)(ctx, base);
        return;
    }
    const uint32_t scene = ctx.r[3].u32, maxIter = ctx.r[4].u32, method = ctx.r[5].u32;
    const float maxTimestep = float(ctx.f[1].ps0);
    {
        std::lock_guard lock(gPhysMutex);
        PhysTiming& t = gPhysTiming[scene];
        if (std::fabs(t.maxTimestep - maxTimestep) > 1e-5f || t.maxIter != maxIter || t.method != method)
        {
            static int logged = 0;
            if (logged++ < 200)
                dtlog::Important("[physics] scene %08X: setTiming(max step %.5f s, %u steps, %s)\n", scene, maxTimestep,
                                 maxIter, method == 0 ? "fixed" : method == 1 ? "variable" : "inherit");
            t = { maxTimestep, maxIter, method };
        }
    }
    hooks::Original(NpScene_setTiming)(ctx, base);
}

PPC_FUNC(PhysSimulate)
{
    if (!gLogPhysics)
    {
        hooks::Original(NpScene_simulate)(ctx, base);
        return;
    }
    {
        std::lock_guard lock(gPhysMutex);
        gSimulates++;
        gSimulated += ctx.f[1].ps0;
    }
    hooks::Original(NpScene_simulate)(ctx, base);
}
DT_HOOK(NpScene_setTiming, PhysSetTiming, "NpScene::setTiming");
DT_HOOK(NpScene_simulate, PhysSimulate, "NpScene::simulate");

// Random numbers: counted per call site in the profile, then the game's own native.
template <uint32_t addr, Op op>
PPC_FUNC(Random)
{
    if (gProfile && tGameThread)
    {
        const uint32_t frame = ctx.r[4].u32;
        Site* site = Lookup(frame, guest::Read32(frame + FrameCode), op);
        site->calls++;
        site->sum += 1, site->sumAbs += 1;
        if (site->lastTick + 1 == gTotalTicks)
            site->tickChecks++, site->continuous++;  // (consecutive ticks: a roll made every tick)
        else if (site->lastTick != gTotalTicks)
            site->tickChecks++;
        site->lastTick = gTotalTicks;
    }
    hooks::Original(addr)(ctx, base);
}
DT_HOOK(0x02099AE4, (Random<0x02099AE4, Op::FRand>), "UObject::execFRand");
DT_HOOK(0x0209623C, (Random<0x0209623C, Op::Rand>), "UObject::execRand");

#define DT_OPERATOR(addr, op, name) DT_HOOK(addr, (Operator<addr, Op::op>), "UObject::" name)
DT_OPERATOR(0x020986C4, AddF, "execAddEqual_FloatFloat");
DT_OPERATOR(0x020988A8, SubF, "execSubtractEqual_FloatFloat");
DT_OPERATOR(0x020982D4, MulF, "execMultiplyEqual_FloatFloat");
DT_OPERATOR(0x02095A54, AddI, "execAddEqual_IntInt");
DT_OPERATOR(0x02095C00, SubI, "execSubtractEqual_IntInt");
DT_OPERATOR(0x02093040, AddB, "execAddEqual_ByteByte");
DT_OPERATOR(0x020931EC, SubB, "execSubtractEqual_ByteByte");
DT_OPERATOR(0x02095DAC, PreIncI, "execAddAdd_PreInt");
DT_OPERATOR(0x02095ED0, PreDecI, "execSubtractSubtract_PreInt");
DT_OPERATOR(0x02095FF4, PostIncI, "execAddAdd_Int");
DT_OPERATOR(0x02096118, PostDecI, "execSubtractSubtract_Int");
DT_OPERATOR(0x02093398, PreIncB, "execAddAdd_PreByte");
DT_OPERATOR(0x020934BC, PreDecB, "execSubtractSubtract_PreByte");
DT_OPERATOR(0x020935E0, PostIncB, "execAddAdd_Byte");
DT_OPERATOR(0x02093708, PostDecB, "execSubtractSubtract_Byte");
DT_OPERATOR(0x0208CC64, AddV, "execAddEqual_VectorVector");
DT_OPERATOR(0x0208CA28, SubV, "execSubtractEqual_VectorVector");
DT_OPERATOR(0x0208D328, MulVF, "execMultiplyEqual_VectorFloat");
DT_OPERATOR(0x020897B8, AddR, "execAddEqual_RotatorRotator");
DT_OPERATOR(0x02089594, SubR, "execSubtractEqual_RotatorRotator");
DT_OPERATOR(0x0209A140, Lerp, "execLerp");

int gCap = 50;
std::vector<std::pair<uint64_t, int>> gSwitches;
int gSwitchAtTimed = -1;  // DT_TEST_FPS=t:<cap>  // DT_TEST_FPS: at engine tick N, change the cap (testing)

void Dump()
{
    std::string path = gProfilePath;
    if (!gSwitches.empty())  // one file per cap: <path>.<cap>.csv
        path += "." + std::to_string(gCap) + ".csv";
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return;
    fprintf(f, "# ticks %llu seconds %.3f fps %.2f\n", (unsigned long long)gTicks, gSeconds,
            gSeconds > 0 ? double(gTicks) / gSeconds : 0.0);
    fprintf(f, "function,offset,op,calls,sum,sum_abs,tick_checks,continuous\n");
    for (const auto& [code, s] : gSites)
        if (s.calls)
            fprintf(f, "\"%s\",0x%X,%s,%llu,%.6g,%.6g,%llu,%llu\n", s.function.c_str(), s.offset,
                    kOpNames[int(s.op)], (unsigned long long)s.calls, s.sum, s.sumAbs, (unsigned long long)s.tickChecks,
                    (unsigned long long)s.continuous);
    fclose(f);
}
} // namespace

void Tick(float deltaSeconds)
{
    static bool init = false;
    if (!init)
    {
        init = true;
        if (const char* p = getenv("DT_FRAMETIE_PROFILE"); p && *p)
        {
            gProfile = true;
            gProfilePath = p;
            dtlog::Important("[frametie] profiling script accumulation sites into %s\n", p);
        }
        if (const char* t = getenv("DT_TEST_FPS"))  // "tick:cap;tick:cap", or "t:cap": when the timed presses start
            for (const char* p = t; *p;)
            {
                if (p[0] == 't' && p[1] == ':')
                {
                    gSwitchAtTimed = int(strtol(p + 2, nullptr, 10));
                    break;
                }
                char* end;
                const uint64_t tick = strtoull(p, &end, 10);
                if (*end != ':')
                    break;
                gSwitches.push_back({ tick, int(strtol(end + 1, &end, 10)) });
                p = *end == ';' ? end + 1 : end;
            }
        size_t fixes = 0;
        for (const Fix& f : kFixes)
            fixes += f.function != nullptr;
        dtlog::Important("[frametie] %zu script sites made frame-rate independent\n", fixes);
        gLastDump = std::chrono::steady_clock::now();
    }
    static uint64_t generation = ~0ull;
    if (settings::Generation() != generation)
    {
        generation = settings::Generation();
        gCap = settings::GetInt("display.fps_cap", 50);
        bool any = false;
        for (const Fix& f : kFixes)
            any |= f.function != nullptr;
        const bool fixing = any && gCap != 50 && !getenv("DT_FRAMETIE_OFF");
        if (fixing != gFixing)
            dtlog::Important("[frametie] fixes %s (fps cap %d)\n", fixing ? "on" : "off", gCap);
        gFixing = fixing;
    }
    if (gSwitchAtTimed >= 0 && host::input::TimedSeconds() >= 0)
    {
        gSwitches.push_back({ gTotalTicks, gSwitchAtTimed });
        gSwitchAtTimed = -1;
    }
    for (const auto& [tick, cap] : gSwitches)
        if (tick == gTotalTicks)
        {
            if (gProfile)
                Dump();
            for (auto& [code, s] : gSites)
                s.calls = s.tickChecks = s.continuous = 0, s.sum = s.sumAbs = 0;
            gTicks = 0;
            gSeconds = 0;
            settings::Override("display.fps_cap", std::to_string(cap));
            game_patches::SetFrameRateCap(cap);
            dtlog::Important("[frametie] test: fps cap %d\n", cap);
        }
    tGameThread = true;
    gScale = std::clamp(deltaSeconds / BaseTick, 0.0f, 1.0f);
    gTicks++;
    gTotalTicks++;
    gSeconds += deltaSeconds;
    if (gProfile)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - gLastDump > std::chrono::seconds(10))
        {
            gLastDump = now;
            Dump();
        }
    }
    {
        static auto lastPhys = std::chrono::steady_clock::now();
        static uint64_t lastTicks = 0;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPhys > std::chrono::seconds(10))
        {
            std::lock_guard lock(gPhysMutex);
            const double secs = std::chrono::duration<double>(now - lastPhys).count();
            if (gSimulates && gLogPhysics)
                dtlog::Important("[physics] %.1f s: %llu simulate calls (%.1f a tick), %.3f s simulated in all scenes, "
                                 "%.0f fps\n", secs, (unsigned long long)gSimulates,
                                 double(gSimulates) / double(std::max<uint64_t>(1, gTotalTicks - lastTicks)), gSimulated,
                                 double(gTotalTicks - lastTicks) / secs);
            gSimulates = 0, gSimulated = 0, lastPhys = now, lastTicks = gTotalTicks;
        }
    }
}
} // namespace frametie
