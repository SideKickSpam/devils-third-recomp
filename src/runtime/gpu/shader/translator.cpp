#include "translator.h"

#include "r700.h"
#include "spirv.h"

#include <bit>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gpu::shader
{
namespace
{
using namespace gpu::spirv;
using namespace gpu::r700;

struct Unsupported
{
    std::string what;
};

// Per-thread control flow, modelled on the R700 hardware (ISA chapter 3.7). The hardware runs a group of pixels
// in lockstep with a per-pixel state and only jumps when *all* pixels fail a test. One SPIR-V invocation is one
// pixel, so the translation follows the linear path with that pixel's state, which gives the same result: jumps
// taken only when all pixels are inactive skip work that would have no effect. The POP_COUNT of JUMP, ELSE,
// PUSH, LOOP_BREAK and LOOP_CONTINUE applies only when they jump (the skipped code would have done that
// popping), so per pixel those pops never happen. Compiled code confirms this: ALU_PUSH_BEFORE; JUMP pop 1;
// ALU_POP_AFTER, and ...; ELSE pop 1; ALU_POP_AFTER.
//
// State per invocation:
//   branch     0 = active, 1 = inactive-branch. Saved and restored by the push stack.
//   brk, cont  the innermost loop's inactive-break / inactive-continue flags (sticky across POP; saved per loop).
//   active   = branch == 0 && !brk && !cont.
// ALU and fetch clauses run only while active; stack operations and exports apply regardless.
constexpr uint32_t StackDepth = 64, LoopDepth = 16, CallDepth = 32;
// Guard against a loop that never terminates (a missing loop constant, say): give up after this many CF steps.
constexpr uint32_t MaxSteps = 1u << 20;

class Translator
{
public:
    Translator(const Request& r, const Program& p) : req(r), prog(p) {}

    Result Run()
    {
        Result result;
        try
        {
            flow = NeedsFlowControl();
            Setup();
            if (flow)
                EmitDispatch();
            else
                for (const Cf& cf : prog.cf)
                {
                    ExecuteCf(cf);
                    DebugSnapshot(int32_t(cf.index));
                }
            if (req.stage == Stage::Vertex && !wrotePosition)
                throw Unsupported{ "vertex shader never exports a position" };
            b.EmitNoResult(OpReturn, {});
            result.spirv = b.Finish(req.stage == Stage::Vertex ? Vertex : Fragment, function, interfaceVars);
            result.textureMask = textureMask;
            result.outputMask = outputMask;
            result.warnings.assign(warnings.begin(), warnings.end());
            result.bufferMask = bufferMask;
        }
        catch (const Unsupported& u)
        {
            result.error = u.what;
        }
        return result;
    }

private:
    const Request& req;
    const Program& prog;
    Builder b;
    bool flow = false;

    uint32_t tFloat = 0, tVec4 = 0, tVec2 = 0, tInt = 0, tUint = 0, tBool = 0;
    uint32_t function = 0, gprs = 0, constants = 0;
    std::vector<uint32_t> interfaceVars;
    std::array<uint32_t, MaxTextures> textures{};
    uint32_t textureMask = 0, outputMask = 0, bufferMask = 0;
    std::array<uint32_t, 16> buffers{};  // VTX storage buffers
    std::set<std::string> warnings;
    std::array<uint32_t, 32> params{};  // vertex: parameter outputs
    std::array<uint32_t, 8> colors{};   // pixel: render target outputs
    uint32_t position = 0;
    bool wrotePosition = false;
    uint32_t debugVar = 0;  // DT_DEBUG_PS: Function vec4 holding the inspected GPR
    std::array<uint32_t, 4> pv{};
    uint32_t ps = 0;

    // Function variables for flow control and relative addressing.
    uint32_t vBranch = 0, vBrk = 0, vCont = 0, vStack = 0, vSp = 0, vPred = 0, vAr = 0, vAl = 0;
    uint32_t vExecSet = 0, vExecCond = 0;  // PRED_SET with UPDATE_EXEC_MASK in the current clause
    uint32_t vLoopSp = 0, vLoopBranch = 0, vLoopBrk = 0, vLoopCont = 0, vLoopCount = 0, vLoopAl = 0, vLoopStep = 0;
    uint32_t vCount = 0, vStep = 0;        // the innermost loop's remaining trip count (-1 = DX10) and aL step
    uint32_t vCallSp = 0, vCallStack = 0, vPc = 0, vDone = 0, vSteps = 0;

    // ---- setup ----
    bool UsesVtx() const
    {
        for (const auto& clause : prog.vtx)
            if (!clause.empty())
                return true;
        return false;
    }

    bool NeedsFlowControl() const
    {
        for (const Cf& cf : prog.cf)
        {
            if (cf.kind == Cf::Alu && cf.inst != CF_ALU)
                return true;
            if (cf.kind == Cf::Normal && cf.inst != CF_NOP && cf.inst != CF_TEX && cf.inst != CF_VTX &&
                cf.inst != CF_VTX_TC && cf.inst != CF_CALL_FS)
                return true;
        }
        for (const auto& clause : prog.alu)
            for (const AluGroup& g : clause)
                for (const Alu& a : g.insts)
                    if (a.updateExecMask)
                        return true;
        return false;
    }

    uint32_t FunctionVar(uint32_t type, const char* name)
    {
        const uint32_t v = b.Emit(OpVariable, b.Pointer(Function, type), { Function });
        b.Name(v, name);
        return v;
    }

    void Setup()
    {
        tFloat = b.Float();
        tVec4 = b.Vec(tFloat, 4);
        tVec2 = b.Vec(tFloat, 2);
        tInt = b.Int();
        tUint = b.Uint();
        tBool = b.Bool();

        // Constant file: uniform block { vec4 c[ConstantRegisters]; } (from c[256] on: renderer data).
        const uint32_t arr = b.Array(tVec4, ConstantRegisters);
        b.Decorate(arr, DecArrayStride, { 16 });
        const uint32_t block = b.Struct({ arr });
        b.Decorate(block, DecBlock);
        b.MemberDecorate(block, 0, DecOffset, { 0 });
        constants = b.GlobalVar(Uniform, block, "cfile");
        b.Decorate(constants, DecDescriptorSet, { 0 });
        b.Decorate(constants, DecBinding, { req.stage == Stage::Vertex ? BindingVsConstants : BindingPsConstants });

        function = b.NewId();
        b.Name(function, "main");
        b.Label(b.NewId());
        // All function variables must be declared at the top of the entry block.
        gprs = FunctionVar(b.Array(tVec4, 128), "R");
        if (req.stage == Stage::Pixel && req.debugGpr >= 0)
            debugVar = FunctionVar(tVec4, "debug");
        vAr = FunctionVar(b.Array(tInt, 4), "AR");
        vAl = FunctionVar(tInt, "aL");
        vPred = FunctionVar(tBool, "pred");
        if (flow)
        {
            vBranch = FunctionVar(tInt, "branch");
            vBrk = FunctionVar(tInt, "brk");
            vCont = FunctionVar(tInt, "cont");
            vStack = FunctionVar(b.Array(tInt, StackDepth), "stack");
            vSp = FunctionVar(tInt, "sp");
            vExecSet = FunctionVar(tBool, "execSet");
            vExecCond = FunctionVar(tBool, "execCond");
            vLoopSp = FunctionVar(tInt, "loopSp");
            vLoopBranch = FunctionVar(b.Array(tInt, LoopDepth), "loopBranch");
            vLoopBrk = FunctionVar(b.Array(tInt, LoopDepth), "loopBrk");
            vLoopCont = FunctionVar(b.Array(tInt, LoopDepth), "loopCont");
            vLoopCount = FunctionVar(b.Array(tInt, LoopDepth), "loopCount");
            vLoopAl = FunctionVar(b.Array(tInt, LoopDepth), "loopAl");
            vLoopStep = FunctionVar(b.Array(tInt, LoopDepth), "loopStep");
            vCount = FunctionVar(tInt, "count");
            vStep = FunctionVar(tInt, "step");
            vCallSp = FunctionVar(tInt, "callSp");
            vCallStack = FunctionVar(b.Array(tInt, CallDepth), "callStack");
            vPc = FunctionVar(tInt, "pc");
            vDone = FunctionVar(tBool, "done");
            vSteps = FunctionVar(tUint, "steps");
        }
        for (uint32_t c = 0; c < 4; c++)
            b.EmitNoResult(OpStore, { ElemPtr(vAr, b.ConstI(int32_t(c))), b.ConstI(0) });
        b.EmitNoResult(OpStore, { vAl, b.ConstI(0) });
        b.EmitNoResult(OpStore, { vPred, b.ConstBool(false) });
        if (flow)
        {
            for (uint32_t v : { vBranch, vBrk, vCont, vSp, vLoopSp, vCount, vStep, vCallSp, vPc })
                b.EmitNoResult(OpStore, { v, b.ConstI(0) });
            b.EmitNoResult(OpStore, { vDone, b.ConstBool(false) });
            b.EmitNoResult(OpStore, { vSteps, b.ConstU(0) });
        }

        if (req.stage == Stage::Vertex && UsesVtx())
        {
            // The hardware starts a vertex shader with R0 = (vertex index, -, -, instance index) as integers; VTX
            // fetches index buffers with it. Draws are rebased to their lowest vertex, which c[256].y adds back.
            auto builtin = [&](BuiltIn which) {
                const uint32_t var = b.GlobalVar(Input, tInt);
                b.Decorate(var, DecBuiltIn, { uint32_t(which) });
                interfaceVars.push_back(var);
                return b.Emit(OpLoad, tInt, { var });
            };
            const uint32_t base = AsInt(LoadConstant(RendererConstants, 1));
            const uint32_t vertex = b.Emit(OpIAdd, tInt, { builtin(BuiltInVertexIndex), base });
            const uint32_t instance = builtin(BuiltInInstanceIndex);
            StoreGpr(0, b.Emit(OpCompositeConstruct, tVec4, { FromInt(vertex), b.ConstF(0), b.ConstF(0), FromInt(instance) }));
        }
        if (req.stage == Stage::Pixel)
        {
            if (debugVar)
                b.EmitNoResult(OpStore, { debugVar, b.ConstVec4(0, 0, 0, 0) });
            // Interpolated inputs land in GPR 0..n-1.
            for (size_t i = 0; i < req.pixelInputs.size(); i++)
            {
                const uint8_t def = i < req.pixelInputDefaults.size() ? req.pixelInputDefaults[i] : 0;
                const float d = def >= 2 ? 1.0f : 0.0f;
                uint32_t v = b.ConstVec4(d, d, d, def & 1 ? 1.0f : 0.0f);
                if (req.pixelInputs[i] >= 0)
                {
                    const uint32_t var = b.GlobalVar(Input, tVec4);
                    b.Decorate(var, DecLocation, { uint32_t(req.pixelInputs[i]) });
                    interfaceVars.push_back(var);
                    v = b.Emit(OpLoad, tVec4, { var });
                }
                StoreGpr(uint32_t(i), v);
            }
            if (req.positionGpr >= 0)
            {
                // The rasterizer's pixel position. Viewports are flipped so Vulkan's framebuffer rows run as the
                // hardware's (row 0 first in memory): FragCoord is the hardware's position as it is.
                const uint32_t var = b.GlobalVar(Input, tVec4);
                b.Decorate(var, DecBuiltIn, { BuiltInFragCoord });
                interfaceVars.push_back(var);
                uint32_t position = b.Emit(OpLoad, tVec4, { var });
                // In the game's pixel units at any internal resolution (c[256].z = 1 / scale).
                const uint32_t inv = LoadConstant(RendererConstants, 2);
                const uint32_t one = b.ConstF(1.0f);
                const uint32_t scale = b.Emit(OpCompositeConstruct, tVec4, { inv, inv, one, one });
                position = b.Emit(OpFMul, tVec4, { position, scale });
                StoreGpr(uint32_t(req.positionGpr), position);
            }
            DebugSnapshot(-1);
        }
    }

    // DT_DEBUG_PS: copy the inspected GPR after CF instruction `cf` (-1: after the inputs are loaded).
    void DebugSnapshot(int32_t cf)
    {
        if (debugVar && cf == req.debugCf)
        {
            uint32_t v = LoadGprVec(uint32_t(req.debugGpr) & 0xFF);
            if (req.debugGpr & 0x100)  // the w component in every channel
            {
                const uint32_t w = b.Emit(OpCompositeExtract, tFloat, { v, 3 });
                v = b.Emit(OpCompositeConstruct, tVec4, { w, w, w, w });
            }
            b.EmitNoResult(OpStore, { debugVar, v });
        }
    }

    // ---- variables ----
    uint32_t ElemPtr(uint32_t arrayVar, uint32_t index)
    {
        return b.Emit(OpAccessChain, b.Pointer(Function, tInt), { arrayVar, index });
    }
    uint32_t LoadI(uint32_t var) { return b.Emit(OpLoad, tInt, { var }); }
    void StoreI(uint32_t var, uint32_t value) { b.EmitNoResult(OpStore, { var, value }); }
    void StoreI(uint32_t var, int32_t value) { StoreI(var, b.ConstI(value)); }
    uint32_t LoadElem(uint32_t arrayVar, uint32_t index) { return b.Emit(OpLoad, tInt, { ElemPtr(arrayVar, index) }); }
    void StoreElem(uint32_t arrayVar, uint32_t index, uint32_t value)
    {
        b.EmitNoResult(OpStore, { ElemPtr(arrayVar, index), value });
    }
    uint32_t Clamp(uint32_t i, int32_t lo, int32_t hi) { return b.Ext(tInt, GlslSClamp, { i, b.ConstI(lo), b.ConstI(hi) }); }
    uint32_t IsZero(uint32_t i) { return b.Emit(OpIEqual, tBool, { i, b.ConstI(0) }); }
    uint32_t And(uint32_t x, uint32_t y) { return b.Emit(OpLogicalAnd, tBool, { x, y }); }
    uint32_t Not(uint32_t x) { return b.Emit(OpLogicalNot, tBool, { x }); }

    // Structured if: `then` emits the body and returns true if it ended the block itself (OpKill).
    void If(uint32_t cond, const std::function<bool()>& then)
    {
        const uint32_t thenLabel = b.NewId(), merge = b.NewId();
        b.EmitNoResult(OpSelectionMerge, { merge, 0 });
        b.EmitNoResult(OpBranchConditional, { cond, thenLabel, merge });
        b.Label(thenLabel);
        if (!then())
            b.EmitNoResult(OpBranch, { merge });
        b.Label(merge);
    }

    // ---- register file ----
    uint32_t GprPtr(uint32_t gpr, uint32_t chan)
    {
        return b.Emit(OpAccessChain, b.Pointer(Function, tFloat), { gprs, b.ConstU(gpr), b.ConstU(chan) });
    }
    uint32_t GprPtrDyn(uint32_t gprIndex, uint32_t chan)
    {
        return b.Emit(OpAccessChain, b.Pointer(Function, tFloat), { gprs, gprIndex, b.ConstU(chan) });
    }
    uint32_t LoadGpr(uint32_t gpr, uint32_t chan) { return b.Emit(OpLoad, tFloat, { GprPtr(gpr, chan) }); }
    void StoreGpr(uint32_t gpr, uint32_t chan, uint32_t value) { b.EmitNoResult(OpStore, { GprPtr(gpr, chan), value }); }
    uint32_t LoadGprVec(uint32_t gpr)
    {
        const uint32_t ptr = b.Emit(OpAccessChain, b.Pointer(Function, tVec4), { gprs, b.ConstU(gpr) });
        return b.Emit(OpLoad, tVec4, { ptr });
    }
    void StoreGpr(uint32_t gpr, uint32_t vec4)
    {
        const uint32_t ptr = b.Emit(OpAccessChain, b.Pointer(Function, tVec4), { gprs, b.ConstU(gpr) });
        b.EmitNoResult(OpStore, { ptr, vec4 });
    }
    uint32_t ConstantPtr(uint32_t index, uint32_t chan)
    {
        return b.Emit(OpAccessChain, b.Pointer(Uniform, tFloat), { constants, b.ConstU(0), index, b.ConstU(chan) });
    }
    uint32_t LoadConstant(uint32_t index, uint32_t chan) { return b.Emit(OpLoad, tFloat, { ConstantPtr(b.ConstU(index), chan) }); }

    // The index register an instruction's relative operands use: AR.x-w (INDEX_MODE 0-3) or the loop index aL (4).
    uint32_t IndexRegister(uint32_t indexMode)
    {
        if (indexMode < 4)
            return LoadElem(vAr, b.ConstI(int32_t(indexMode)));
        if (indexMode == 4)
            return LoadI(vAl);
        throw Unsupported{ "ALU index mode " + std::to_string(indexMode) };
    }

    // Component select used by exports and fetches: 0-3 = x-w of `vec`, 4 = 0.0, 5 = 1.0.
    uint32_t Select(uint32_t vec, const std::array<uint32_t, 4>& sel)
    {
        uint32_t comps[4];
        for (int c = 0; c < 4; c++)
        {
            if (sel[c] < 4)
                comps[c] = b.Emit(OpCompositeExtract, tFloat, { vec, sel[c] });
            else
                comps[c] = b.ConstF(sel[c] == 5 ? 1.0f : 0.0f);
        }
        return b.Emit(OpCompositeConstruct, tVec4, { comps[0], comps[1], comps[2], comps[3] });
    }

    uint32_t AsInt(uint32_t f) { return b.Emit(OpBitcast, tInt, { f }); }
    uint32_t AsUint(uint32_t f) { return b.Emit(OpBitcast, tUint, { f }); }
    uint32_t FromInt(uint32_t i) { return b.Emit(OpBitcast, tFloat, { i }); }
    uint32_t BitsF(uint32_t bits) { return b.ConstF(std::bit_cast<float>(bits)); }

    // ---- straight-line control flow ----
    void ExecuteCf(const Cf& cf)
    {
        switch (cf.kind)
        {
        case Cf::Alu:
            if (cf.inst != CF_ALU)
                throw Unsupported{ std::string("CF instruction ") + CfName(cf) };
            AluClause(cf);
            break;
        case Cf::Export: Export(cf); break;
        case Cf::Normal:
            switch (cf.inst)
            {
            case CF_NOP: break;
            case CF_CALL_FS: CallFs(); break;
            case CF_TEX: TexClause(cf); break;
            case CF_VTX: case CF_VTX_TC: VtxClause(cf); break;
            default: throw Unsupported{ std::string("CF instruction ") + CfName(cf) };
            }
            break;
        }
    }

    void CallFs()
    {
        if (req.stage != Stage::Vertex)
            throw Unsupported{ "CALL_FS outside a vertex shader" };
        FetchVertexInputs();
    }

    void AluClause(const Cf& cf)
    {
        if (cf.kcacheMode[0] || cf.kcacheMode[1])
            throw Unsupported{ "ALU clause uses constant caches (uniform blocks)" };
        pv = {};
        ps = 0;
        for (const AluGroup& g : prog.alu[cf.index])
            ExecuteGroup(g);
    }

    void TexClause(const Cf& cf)
    {
        for (const Tex& t : prog.tex[cf.index])
            Texture(t);
    }

    // ---- flow-controlled programs: loop { switch (pc) { case i: CF instruction i } } ----
    uint32_t Active()
    {
        return And(IsZero(LoadI(vBranch)), And(IsZero(LoadI(vBrk)), IsZero(LoadI(vCont))));
    }

    // CF_COND: 0 always, 1 never, 2 boolean constant, 3 its inverse. Boolean constants are not set by GX2
    // titles we know of, so they read as false.
    uint32_t CfCondition(const Cf& cf)
    {
        switch (cf.cond)
        {
        case 0: return b.ConstBool(true);
        case 1: return b.ConstBool(false);
        case 2: return b.ConstBool(false);
        default: return b.ConstBool(true);
        }
    }

    void Push()
    {
        const uint32_t sp = LoadI(vSp);
        StoreElem(vStack, Clamp(sp, 0, StackDepth - 1), LoadI(vBranch));
        StoreI(vSp, b.Emit(OpIAdd, tInt, { sp, b.ConstI(1) }));
    }

    void Pop(uint32_t count)
    {
        if (!count)
            return;
        const uint32_t sp = b.Ext(tInt, GlslSMax, { b.Emit(OpISub, tInt, { LoadI(vSp), b.ConstI(int32_t(count)) }), b.ConstI(0) });
        StoreI(vSp, sp);
        StoreI(vBranch, LoadElem(vStack, Clamp(sp, 0, StackDepth - 1)));
    }

    // ELSE: pixels that were active at the last surviving push (the stack top) and pass the test swap between
    // active and inactive-branch.
    void Else(uint32_t popCount, uint32_t cond)
    {
        Pop(popCount);
        const uint32_t top = LoadElem(vStack, Clamp(b.Emit(OpISub, tInt, { LoadI(vSp), b.ConstI(1) }), 0, StackDepth - 1));
        const uint32_t flip = And(cond, And(IsZero(top), b.Emit(OpSLessThan, tBool, { b.ConstI(0), LoadI(vSp) })));
        const uint32_t branch = LoadI(vBranch);
        StoreI(vBranch, b.Emit(OpSelect, tInt, { flip, b.Emit(OpISub, tInt, { b.ConstI(1), branch }), branch }));
    }

    // A clause that may update the exec mask: run it if active, then deactivate (as `deactivate` says) the pixels
    // whose PRED_SET failed.
    void AluClauseWithExec(const Cf& cf, uint32_t deactivateVar)
    {
        b.EmitNoResult(OpStore, { vExecSet, b.ConstBool(false) });
        If(Active(), [&] {
            AluClause(cf);
            return false;
        });
        // Only pixels that ran the clause (were active) can have set the exec mask.
        const uint32_t fail = And(b.Emit(OpLoad, tBool, { vExecSet }), Not(b.Emit(OpLoad, tBool, { vExecCond })));
        StoreI(deactivateVar, b.Emit(OpSelect, tInt, { fail, b.ConstI(1), LoadI(deactivateVar) }));
    }

    void LoopEnter(bool dx9, bool setAl, const Cf& cf)
    {
        // Save the per-pixel state and the enclosing loop's state, then start this loop.
        Push();
        const uint32_t lsp = LoadI(vLoopSp);
        const uint32_t slot = Clamp(lsp, 0, LoopDepth - 1);
        StoreElem(vLoopBrk, slot, LoadI(vBrk));
        StoreElem(vLoopCont, slot, LoadI(vCont));
        StoreElem(vLoopCount, slot, LoadI(vCount));
        StoreElem(vLoopAl, slot, LoadI(vAl));
        StoreElem(vLoopStep, slot, LoadI(vStep));
        StoreI(vLoopSp, b.Emit(OpIAdd, tInt, { lsp, b.ConstI(1) }));
        StoreI(vBrk, 0);
        StoreI(vCont, 0);
        if (dx9)
        {
            const uint32_t c = req.loopConsts[cf.cfConst & 31];
            StoreI(vCount, int32_t(c & 0xFFF));
            StoreI(vStep, int32_t(int8_t(c >> 24)));
            if (setAl)
                StoreI(vAl, int32_t((c >> 12) & 0xFFF));
        }
        else
        {
            StoreI(vCount, -1);  // DX10 loop: runs until every pixel breaks
            StoreI(vStep, 0);
        }
    }

    void LoopExit()
    {
        const uint32_t lsp = b.Ext(tInt, GlslSMax, { b.Emit(OpISub, tInt, { LoadI(vLoopSp), b.ConstI(1) }), b.ConstI(0) });
        StoreI(vLoopSp, lsp);
        const uint32_t slot = Clamp(lsp, 0, LoopDepth - 1);
        StoreI(vBrk, LoadElem(vLoopBrk, slot));
        StoreI(vCont, LoadElem(vLoopCont, slot));
        StoreI(vCount, LoadElem(vLoopCount, slot));
        StoreI(vAl, LoadElem(vLoopAl, slot));
        StoreI(vStep, LoadElem(vLoopStep, slot));
        Pop(1);
    }

    void EmitDispatch()
    {
        const uint32_t header = b.NewId(), body = b.NewId(), switchMerge = b.NewId(), cont = b.NewId(), merge = b.NewId();
        const uint32_t defaultLabel = b.NewId();
        b.EmitNoResult(OpBranch, { header });
        b.Label(header);
        b.EmitNoResult(OpLoopMerge, { merge, cont, 0 });
        b.EmitNoResult(OpBranch, { body });
        b.Label(body);
        const uint32_t pc = LoadI(vPc);
        std::vector<uint32_t> labels(prog.cf.size());
        std::vector<uint32_t> sw{ pc, defaultLabel };
        for (size_t i = 0; i < prog.cf.size(); i++)
        {
            labels[i] = b.NewId();
            sw.push_back(uint32_t(i));
            sw.push_back(labels[i]);
        }
        b.EmitNoResult(OpSelectionMerge, { switchMerge, 0 });
        b.EmitNoResult(OpSwitch, sw);
        for (size_t i = 0; i < prog.cf.size(); i++)
        {
            b.Label(labels[i]);
            StoreI(vPc, int32_t(i + 1));
            FlowCf(prog.cf[i], uint32_t(i));
            DebugSnapshot(int32_t(i));
            if (prog.cf[i].endOfProgram)
                b.EmitNoResult(OpStore, { vDone, b.ConstBool(true) });
            b.EmitNoResult(OpBranch, { switchMerge });
        }
        b.Label(defaultLabel);
        b.EmitNoResult(OpStore, { vDone, b.ConstBool(true) });
        b.EmitNoResult(OpBranch, { switchMerge });
        b.Label(switchMerge);
        b.EmitNoResult(OpBranch, { cont });
        b.Label(cont);
        const uint32_t steps = b.Emit(OpIAdd, tUint, { b.Emit(OpLoad, tUint, { vSteps }), b.ConstU(1) });
        b.EmitNoResult(OpStore, { vSteps, steps });
        const uint32_t done = b.Emit(OpLogicalOr, tBool,
            { b.Emit(OpLoad, tBool, { vDone }), b.Emit(OpUGreaterThan, tBool, { steps, b.ConstU(MaxSteps) }) });
        b.EmitNoResult(OpBranchConditional, { done, merge, header });
        b.Label(merge);
    }

    void FlowCf(const Cf& cf, uint32_t index)
    {
        (void)index;
        if (cf.kind == Cf::Export)
        {
            Export(cf);
            return;
        }
        if (cf.kind == Cf::Alu)
        {
            switch (cf.inst)
            {
            case CF_ALU: AluClauseWithExec(cf, vBranch); break;
            case CF_ALU_PUSH_BEFORE:
                Push();
                AluClauseWithExec(cf, vBranch);
                break;
            case CF_ALU_POP_AFTER:
                AluClauseWithExec(cf, vBranch);
                Pop(1);
                break;
            case CF_ALU_POP2_AFTER:
                AluClauseWithExec(cf, vBranch);
                Pop(2);
                break;
            case CF_ALU_ELSE_AFTER:
                AluClauseWithExec(cf, vBranch);
                Else(0, b.ConstBool(true));
                break;
            // ALU_BREAK / ALU_CONTINUE = PUSH, ALU, ELSE, BREAK/CONTINUE, POP: the pixels whose PRED_SET failed
            // break out of (or skip the rest of) the loop.
            case CF_ALU_BREAK: AluClauseWithExec(cf, vBrk); break;
            case CF_ALU_CONTINUE: AluClauseWithExec(cf, vCont); break;
            default: throw Unsupported{ std::string("CF instruction ") + CfName(cf) };
            }
            return;
        }
        switch (cf.inst)
        {
        case CF_NOP: break;
        case CF_CALL_FS: CallFs(); break;
        case CF_TEX:
            If(Active(), [&] {
                TexClause(cf);
                return false;
            });
            break;
        case CF_VTX: case CF_VTX_TC:
            If(Active(), [&] {
                VtxClause(cf);
                return false;
            });
            break;
        case CF_PUSH:
        {
            Push();
            const uint32_t fail = And(Active(), Not(CfCondition(cf)));
            StoreI(vBranch, b.Emit(OpSelect, tInt, { fail, b.ConstI(1), LoadI(vBranch) }));
            break;
        }
        case CF_POP: Pop(cf.popCount); break;
        // ELSE's POP_COUNT applies only with its jump (taken when every pixel is inactive after the ELSE), which
        // skips the clause that would otherwise pop: per pixel, ELSE only inverts.
        case CF_ELSE: Else(0, CfCondition(cf)); break;
        // JUMP pops and jumps only when every pixel fails (ISA table 3.7); the next instructions then do the
        // same stack work for an inactive pixel, so per pixel there is nothing to do.
        case CF_JUMP: break;
        case CF_LOOP_START:
        case CF_LOOP_START_NO_AL:
        case CF_LOOP_START_DX10:
        {
            // A pixel that is not active, or a DX9 loop with a zero trip count, skips the loop.
            const bool dx9 = cf.inst != CF_LOOP_START_DX10;
            uint32_t skip = Not(Active());
            if (dx9 && (req.loopConsts[cf.cfConst & 31] & 0xFFF) == 0)
                skip = b.ConstBool(true);
            const uint32_t enter = b.NewId(), leave = b.NewId(), merge = b.NewId();
            b.EmitNoResult(OpSelectionMerge, { merge, 0 });
            b.EmitNoResult(OpBranchConditional, { skip, leave, enter });
            b.Label(leave);
            Pop(cf.popCount);
            StoreI(vPc, int32_t(cf.addr));
            b.EmitNoResult(OpBranch, { merge });
            b.Label(enter);
            LoopEnter(dx9, cf.inst == CF_LOOP_START, cf);
            b.EmitNoResult(OpBranch, { merge });
            b.Label(merge);
            break;
        }
        case CF_LOOP_END:
        {
            // Continue-inactive pixels come back for the next iteration; the loop repeats unless this pixel broke
            // out or a DX9 trip count ran out.
            StoreI(vCont, 0);
            const uint32_t count = LoadI(vCount);
            const uint32_t counted = b.Emit(OpSLessThan, tBool, { b.ConstI(0), count });
            const uint32_t newCount = b.Emit(OpSelect, tInt, { counted, b.Emit(OpISub, tInt, { count, b.ConstI(1) }), count });
            StoreI(vCount, newCount);
            StoreI(vAl, b.Emit(OpIAdd, tInt, { LoadI(vAl), LoadI(vStep) }));
            const uint32_t again = And(IsZero(LoadI(vBrk)), b.Emit(OpINotEqual, tBool, { newCount, b.ConstI(0) }));
            const uint32_t loop = b.NewId(), exit = b.NewId(), merge = b.NewId();
            b.EmitNoResult(OpSelectionMerge, { merge, 0 });
            b.EmitNoResult(OpBranchConditional, { again, loop, exit });
            b.Label(loop);
            StoreI(vPc, int32_t(cf.addr));
            b.EmitNoResult(OpBranch, { merge });
            b.Label(exit);
            LoopExit();
            b.EmitNoResult(OpBranch, { merge });
            b.Label(merge);
            break;
        }
        case CF_LOOP_BREAK:
        case CF_LOOP_CONTINUE:
        {
            const uint32_t hit = And(Active(), CfCondition(cf));
            const uint32_t var = cf.inst == CF_LOOP_BREAK ? vBrk : vCont;
            StoreI(var, b.Emit(OpSelect, tInt, { hit, b.ConstI(1), LoadI(var) }));
            break;
        }
        case CF_CALL:
        {
            const uint32_t csp = LoadI(vCallSp);
            StoreElem(vCallStack, Clamp(csp, 0, CallDepth - 1), b.ConstI(int32_t(index + 1)));
            StoreI(vCallSp, b.Emit(OpIAdd, tInt, { csp, b.ConstI(1) }));
            StoreI(vPc, int32_t(cf.addr));
            break;
        }
        case CF_RETURN:
        {
            // Returning with an empty call stack ends the program.
            const uint32_t csp = b.Emit(OpISub, tInt, { LoadI(vCallSp), b.ConstI(1) });
            StoreI(vCallSp, b.Ext(tInt, GlslSMax, { csp, b.ConstI(0) }));
            const uint32_t empty = b.Emit(OpSLessThan, tBool, { csp, b.ConstI(0) });
            StoreI(vPc, b.Emit(OpSelect, tInt, { empty, b.ConstI(int32_t(prog.cf.size())),
                LoadElem(vCallStack, Clamp(csp, 0, CallDepth - 1)) }));
            break;
        }
        default: throw Unsupported{ std::string("CF instruction ") + CfName(cf) };
        }
    }

    void FetchVertexInputs()
    {
        for (const VertexInput& in : req.vertexInputs)
        {
            const uint32_t type = in.type == 0 ? tVec4 : b.Vec(in.type == 1 ? tUint : tInt, 4);
            const uint32_t var = b.GlobalVar(Input, type);
            b.Decorate(var, DecLocation, { in.location });
            interfaceVars.push_back(var);
            uint32_t v = b.Emit(OpLoad, type, { var });
            if (in.type != 0)
                v = b.Emit(OpBitcast, tVec4, { v });
            StoreGpr(in.gpr, Select(v, { in.sel[0], in.sel[1], in.sel[2], in.sel[3] }));
        }
    }

    // GX2CompareFunction: never, less, equal, lequal, greater, notequal, gequal, always.
    uint32_t Compare(uint32_t func, uint32_t x, uint32_t y)
    {
        switch (func)
        {
        case 0: return b.ConstBool(false);
        case 1: return b.Emit(OpFOrdLessThan, tBool, { x, y });
        case 2: return b.Emit(OpFOrdEqual, tBool, { x, y });
        case 3: return Not(b.Emit(OpFOrdGreaterThan, tBool, { x, y }));
        case 4: return b.Emit(OpFOrdGreaterThan, tBool, { x, y });
        case 5: return b.Emit(OpFUnordNotEqual, tBool, { x, y });
        case 6: return b.Emit(OpFOrdGreaterThanEqual, tBool, { x, y });
        default: return b.ConstBool(true);
        }
    }

    void Export(const Cf& cf)
    {
        if (cf.inst != CF_EXPORT && cf.inst != CF_EXPORT_DONE)
            throw Unsupported{ std::string("CF instruction ") + CfName(cf) };
        if (cf.rwRel)
            throw Unsupported{ "relative export" };
        for (uint32_t k = 0; k < cf.burstCount; k++)
        {
            const uint32_t value = Select(LoadGprVec(cf.rwGpr + k), cf.sel);
            const uint32_t target = cf.arrayBase + k;
            if (cf.exportType == EXPORT_POS)
            {
                if (target == 61)
                {
                    warnings.insert("exports point size / edge flag / render target index (POS 61), ignored");
                    continue;
                }
                if (target == 62 || target == 63)
                {
                    warnings.insert("exports clip/cull distances (POS 62-63), ignored");
                    continue;
                }
                if (target != 60)
                    throw Unsupported{ "position export to " + std::to_string(target) };
                if (!position)
                {
                    position = b.GlobalVar(Output, tVec4, "position");
                    b.Decorate(position, DecBuiltIn, { BuiltInPosition });
                    interfaceVars.push_back(position);
                }
                uint32_t pos = value;
                if (!req.halfZ)
                {
                    // GX2 clip space has z in [-w, w]; Vulkan wants [0, w].
                    const uint32_t z = b.Emit(OpCompositeExtract, tFloat, { pos, 2 });
                    const uint32_t w = b.Emit(OpCompositeExtract, tFloat, { pos, 3 });
                    const uint32_t nz = b.Emit(OpFMul, tFloat, { b.Emit(OpFAdd, tFloat, { z, w }), b.ConstF(0.5f) });
                    pos = b.Emit(OpCompositeConstruct, tVec4, { b.Emit(OpCompositeExtract, tFloat, { pos, 0 }),
                        b.Emit(OpCompositeExtract, tFloat, { pos, 1 }), nz, w });
                }
                b.EmitNoResult(OpStore, { position, pos });
                wrotePosition = true;
            }
            else if (cf.exportType == EXPORT_PARAM && req.stage == Stage::Vertex)
            {
                if (target >= params.size())
                    throw Unsupported{ "parameter export index out of range" };
                if (!params[target])
                {
                    params[target] = b.GlobalVar(Output, tVec4);
                    b.Decorate(params[target], DecLocation, { target });
                    interfaceVars.push_back(params[target]);
                }
                b.EmitNoResult(OpStore, { params[target], value });
                outputMask |= 1u << target;
            }
            else if (cf.exportType == EXPORT_PIXEL && req.stage == Stage::Pixel)
            {
                if (target == 61)
                {
                    warnings.insert("writes depth (pixel export 61), ignored: the interpolated depth is used");
                    continue;
                }
                if (target >= colors.size())
                    throw Unsupported{ "pixel export beyond render target 7" };
                const uint8_t outType = req.outputTypes[target];
                const uint32_t colorType = outType == 1 ? b.Vec(tUint, 4) : outType == 2 ? b.Vec(tInt, 4) : tVec4;
                if (!colors[target])
                {
                    colors[target] = b.GlobalVar(Output, colorType);
                    b.Decorate(colors[target], DecLocation, { target });
                    interfaceVars.push_back(colors[target]);
                }
                if (target == 0 && req.alphaFunc != 7)
                {
                    // Alpha test (the hardware applies it to render target 0's exported alpha).
                    const uint32_t alpha = b.Emit(OpCompositeExtract, tFloat, { value, 3 });
                    const uint32_t ref = LoadConstant(AlphaRefConstant, 0);
                    If(Not(Compare(req.alphaFunc, alpha, ref)), [&] {
                        b.EmitNoResult(OpKill, {});
                        return true;
                    });
                }
                // DT_DEBUG_PS: the input's xyz with alpha 1000, so captures can pick out exactly these pixels.
                const uint32_t out = target == 0 && debugVar
                    ? b.Emit(OpCompositeInsert, tVec4, { b.ConstF(1000.0f), b.Emit(OpLoad, tVec4, { debugVar }), 3 })
                    : value;
                b.EmitNoResult(OpStore, { colors[target], colorType == tVec4 ? out : b.Emit(OpBitcast, colorType, { out }) });
                outputMask |= 1u << target;
            }
            else
                throw Unsupported{ "export type does not match the shader stage" };
        }
    }

    // ---- vertex fetch ----
    uint32_t BufferVar(uint32_t n)
    {
        if (!buffers[n])
        {
            // buffer { uint words[]; }: the guest attribute buffer's raw bytes.
            const uint32_t arr = b.RuntimeArray(tUint);
            b.Decorate(arr, DecArrayStride, { 4 });
            const uint32_t block = b.Struct({ arr });
            b.Decorate(block, DecBufferBlock);
            b.MemberDecorate(block, 0, DecOffset, { 0 });
            b.MemberDecorate(block, 0, DecNonWritable);
            buffers[n] = b.GlobalVar(Uniform, block);
            b.Decorate(buffers[n], DecDescriptorSet, { 0 });
            b.Decorate(buffers[n], DecBinding, { BindingVsBuffers + n });
            bufferMask |= 1u << n;
        }
        return buffers[n];
    }

    // One byte of a buffer at a (uint) byte address.
    uint32_t Byte(uint32_t buffer, uint32_t addr)
    {
        const uint32_t ptr = b.Emit(OpAccessChain, b.Pointer(Uniform, tUint),
            { buffer, b.ConstI(0), b.Emit(OpShiftRightLogical, tUint, { addr, b.ConstU(2) }) });
        const uint32_t word = b.Emit(OpLoad, tUint, { ptr });
        const uint32_t shift = b.Emit(OpShiftLeftLogical, tUint, { b.Emit(OpBitwiseAnd, tUint, { addr, b.ConstU(3) }), b.ConstU(3) });
        return b.Emit(OpBitwiseAnd, tUint, { b.Emit(OpShiftRightLogical, tUint, { word, shift }), b.ConstU(0xFF) });
    }

    // VTX data formats this translation reads: component count, bits per component, float.
    struct VtxFormat
    {
        uint32_t count, bits;
        bool isFloat;
    };
    static std::optional<VtxFormat> VtxDataFormat(uint32_t f)
    {
        switch (f)
        {
        case 1: return VtxFormat{ 1, 8, false };    // FMT_8
        case 5: return VtxFormat{ 1, 16, false };   // FMT_16
        case 6: return VtxFormat{ 1, 16, true };    // FMT_16_FLOAT
        case 7: return VtxFormat{ 2, 8, false };    // FMT_8_8
        case 13: return VtxFormat{ 1, 32, false };  // FMT_32
        case 14: return VtxFormat{ 1, 32, true };   // FMT_32_FLOAT
        case 15: return VtxFormat{ 2, 16, false };  // FMT_16_16
        case 16: return VtxFormat{ 2, 16, true };   // FMT_16_16_FLOAT
        case 26: return VtxFormat{ 4, 8, false };   // FMT_8_8_8_8
        case 29: return VtxFormat{ 2, 32, false };  // FMT_32_32
        case 30: return VtxFormat{ 2, 32, true };   // FMT_32_32_FLOAT
        case 31: return VtxFormat{ 4, 16, false };  // FMT_16_16_16_16
        case 32: return VtxFormat{ 4, 16, true };   // FMT_16_16_16_16_FLOAT
        case 34: return VtxFormat{ 4, 32, false };  // FMT_32_32_32_32
        case 35: return VtxFormat{ 4, 32, true };   // FMT_32_32_32_32_FLOAT
        case 44: return VtxFormat{ 3, 8, false };   // FMT_8_8_8
        case 45: return VtxFormat{ 3, 16, false };  // FMT_16_16_16
        case 46: return VtxFormat{ 3, 16, true };   // FMT_16_16_16_FLOAT
        case 47: return VtxFormat{ 3, 32, false };  // FMT_32_32_32
        case 48: return VtxFormat{ 3, 32, true };   // FMT_32_32_32_FLOAT
        default: return std::nullopt;
        }
    }

    // A VTX clause: each fetch reads one element of a vertex buffer at index * stride + offset. The bytes are read
    // as the fetch unit sees them: ENDIAN_SWAP is an address XOR (8-in-16 ^1, 8-in-32 ^3) and components are then
    // little-endian, converted by NUM_FORMAT_ALL (norm, int, scaled) and FORMAT_COMP_ALL (signed).
    void VtxClause(const Cf& cf)
    {
        if (req.stage != Stage::Vertex)
            throw Unsupported{ "VTX clause outside a vertex shader" };
        for (const Vtx& v : prog.vtx[cf.index])
        {
            if (v.inst != VTX_FETCH)
                throw Unsupported{ v.inst == VTX_SEMANTIC ? "semantic vertex fetch" : "VTX memory read" };
            if (v.srcRel || v.dstRel)
                throw Unsupported{ "relative VTX registers" };
            if (v.useConstFields)
                throw Unsupported{ "VTX fetch with the format in the fetch constant" };
            if (v.bufferId < 160 || v.bufferId >= 176)
                throw Unsupported{ "VTX fetch from resource " + std::to_string(v.bufferId) + " (only vertex buffers 160-175)" };
            const auto fmt = VtxDataFormat(v.dataFormat);
            if (!fmt)
                throw Unsupported{ "VTX data format " + std::to_string(v.dataFormat) };
            const uint32_t n = v.bufferId - 160;
            const uint32_t buffer = BufferVar(n);
            const uint32_t index = AsUint(LoadGpr(v.srcGpr, v.srcSel));
            const uint32_t strideBits = LoadConstant(StrideConstants + n / 4, n % 4);
            const uint32_t base = b.Emit(OpIAdd, tUint,
                { b.Emit(OpIMul, tUint, { index, AsUint(strideBits) }), b.ConstU(v.offset) });
            const uint32_t swapXor = v.endianSwap == 1 ? 1 : v.endianSwap == 2 ? 3 : 0;
            const uint32_t bytes = fmt->bits / 8;
            uint32_t comps[4] = { b.ConstF(0), b.ConstF(0), b.ConstF(0), b.ConstF(1) };
            if (v.numFormat == 1)
                comps[3] = BitsF(1);  // integer data defaults w to integer 1
            for (uint32_t c = 0; c < fmt->count; c++)
            {
                uint32_t raw = b.ConstU(0);
                for (uint32_t k = 0; k < bytes; k++)
                {
                    uint32_t addr = b.Emit(OpIAdd, tUint, { base, b.ConstU(c * bytes + k) });
                    if (swapXor)
                        addr = b.Emit(OpBitwiseXor, tUint, { addr, b.ConstU(swapXor) });
                    raw = b.Emit(OpBitwiseOr, tUint, { raw, b.Emit(OpShiftLeftLogical, tUint, { Byte(buffer, addr), b.ConstU(8 * k) }) });
                }
                comps[c] = VtxConvert(raw, fmt->bits, fmt->isFloat, v.numFormat, v.formatSigned);
            }
            const uint32_t value = b.Emit(OpCompositeConstruct, tVec4, { comps[0], comps[1], comps[2], comps[3] });
            for (uint32_t c = 0; c < 4; c++)
            {
                const uint32_t sel = v.dstSel[c];
                if (sel == 7)
                    continue;
                StoreGpr(v.dstGpr, c, sel < 4 ? b.Emit(OpCompositeExtract, tFloat, { value, sel }) : b.ConstF(sel == 5 ? 1.0f : 0.0f));
            }
        }
    }

    // One fetched component (raw little-endian bits in a uint) to the value the GPR receives.
    uint32_t VtxConvert(uint32_t raw, uint32_t bits, bool isFloat, uint32_t numFormat, bool sign)
    {
        if (isFloat)
        {
            if (bits == 32)
                return b.Emit(OpBitcast, tFloat, { raw });
            const uint32_t pair = b.Ext(tVec2, Glsl(GlslUnpackHalf2x16), { raw });
            return b.Emit(OpCompositeExtract, tFloat, { pair, 0 });
        }
        // Sign-extend from `bits` with a left then arithmetic right shift.
        uint32_t value = raw;
        if (sign && bits < 32)
            value = AsUint(b.Emit(OpShiftRightArithmetic, tInt,
                { b.Emit(OpBitcast, tInt, { b.Emit(OpShiftLeftLogical, tUint, { raw, b.ConstU(32 - bits) }) }), b.ConstI(int32_t(32 - bits)) }));
        switch (numFormat)
        {
        case 1: return b.Emit(OpBitcast, tFloat, { value });  // INT: raw integer bits
        case 2: return sign ? b.Emit(OpConvertSToF, tFloat, { b.Emit(OpBitcast, tInt, { value }) })
                            : b.Emit(OpConvertUToF, tFloat, { value });  // SCALED
        default:
        {
            // NORM: [0, 1] or [-1, 1].
            if (!sign)
                return b.Emit(OpFDiv, tFloat, { b.Emit(OpConvertUToF, tFloat, { value }), b.ConstF(float((uint64_t(1) << bits) - 1)) });
            const uint32_t f = b.Emit(OpFDiv, tFloat,
                { b.Emit(OpConvertSToF, tFloat, { b.Emit(OpBitcast, tInt, { value }) }), b.ConstF(float((uint64_t(1) << (bits - 1)) - 1)) });
            return b.Ext(tFloat, GlslFMax, { f, b.ConstF(-1.0f) });
        }
        }
    }

    // ---- texture fetch ----
    uint32_t SampledType(uint32_t resource)
    {
        const uint8_t t = req.textureTypes[resource];
        return t == 1 ? tUint : t == 2 ? tInt : tFloat;
    }

    uint32_t ImageType(uint32_t resource)
    {
        const uint32_t type = SampledType(resource);
        switch (req.textureDims[resource])
        {
        case TexDim::Tex3D: return b.Image(Dim3D, false, type);
        case TexDim::Cube: return b.Image(DimCube, false, type);
        case TexDim::Tex2DArray: return b.Image(Dim2D, true, type);
        default: return b.Image(Dim2D, false, type);
        }
    }

    uint32_t TextureVar(uint32_t resource)
    {
        if (resource >= MaxTextures)
            throw Unsupported{ "texture resource id out of range" };
        if (!textures[resource])
        {
            textures[resource] = b.GlobalVar(UniformConstant, b.SampledImage(ImageType(resource)));
            b.Decorate(textures[resource], DecDescriptorSet, { 0 });
            const uint32_t base = req.stage == Stage::Vertex ? BindingVsTextures : BindingPsTextures;
            b.Decorate(textures[resource], DecBinding, { base + resource });
            textureMask |= 1u << resource;
        }
        return textures[resource];
    }

    // Cube coordinates as the texture unit receives them, (s, t, face) with s and t in [1, 2] (CUBE's S and T
    // divided by 2 * |major axis|, plus 1.5), back to a direction vector for Vulkan. Faces are +X, -X, +Y, -Y,
    // +Z, -Z with the usual cube-map S/T conventions (see Cube in ExecuteGroup).
    uint32_t CubeDirection(uint32_t s, uint32_t t, uint32_t face)
    {
        const uint32_t sc = b.Emit(OpFMul, tFloat, { b.Emit(OpFSub, tFloat, { s, b.ConstF(1.5f) }), b.ConstF(2.0f) });
        const uint32_t tc = b.Emit(OpFMul, tFloat, { b.Emit(OpFSub, tFloat, { t, b.ConstF(1.5f) }), b.ConstF(2.0f) });
        const uint32_t nsc = b.Emit(OpFNegate, tFloat, { sc }), ntc = b.Emit(OpFNegate, tFloat, { tc });
        const uint32_t one = b.ConstF(1.0f), mone = b.ConstF(-1.0f);
        const uint32_t tVec3 = b.Vec(tFloat, 3);
        const uint32_t dirs[6] = {
            b.Emit(OpCompositeConstruct, tVec3, { one, ntc, nsc }),   // +X: sc = -z, tc = -y
            b.Emit(OpCompositeConstruct, tVec3, { mone, ntc, sc }),   // -X: sc = +z, tc = -y
            b.Emit(OpCompositeConstruct, tVec3, { sc, one, tc }),     // +Y: sc = +x, tc = +z
            b.Emit(OpCompositeConstruct, tVec3, { sc, mone, ntc }),   // -Y: sc = +x, tc = -z
            b.Emit(OpCompositeConstruct, tVec3, { sc, ntc, one }),    // +Z: sc = +x, tc = -y
            b.Emit(OpCompositeConstruct, tVec3, { nsc, ntc, mone }),  // -Z: sc = -x, tc = -y
        };
        const uint32_t f = b.Emit(OpConvertFToS, tInt, { b.Ext(tFloat, GlslRoundEven, { face }) });
        // Select per component: SPIR-V 1.0's OpSelect needs a condition with as many components as the result.
        uint32_t comps[3];
        for (uint32_t c = 0; c < 3; c++)
        {
            comps[c] = b.Emit(OpCompositeExtract, tFloat, { dirs[5], c });
            for (int i = 4; i >= 0; i--)
                comps[c] = b.Emit(OpSelect, tFloat, { b.Emit(OpIEqual, tBool, { f, b.ConstI(i) }),
                    b.Emit(OpCompositeExtract, tFloat, { dirs[i], c }), comps[c] });
        }
        return b.Emit(OpCompositeConstruct, tVec3, { comps[0], comps[1], comps[2] });
    }

    void Texture(const Tex& t)
    {
        if (t.srcRel || t.dstRel)
            throw Unsupported{ "relative texture fetch registers" };
        const bool explicitLod = t.inst == TEX_SAMPLE_LZ || t.inst == TEX_SAMPLE_L || req.stage == Stage::Vertex;
        if (t.inst != TEX_SAMPLE && t.inst != TEX_SAMPLE_LZ && t.inst != TEX_SAMPLE_L && t.inst != TEX_SAMPLE_LB)
            throw Unsupported{ std::string("texture instruction ") + TexName(t.inst) };
        if (t.resourceId >= MaxTextures)
            throw Unsupported{ "texture resource id out of range" };
        const TexDim dim = req.textureDims[t.resourceId];
        const uint32_t src = LoadGprVec(t.srcGpr);
        auto comp = [&](uint32_t sel) {
            return sel < 4 ? b.Emit(OpCompositeExtract, tFloat, { src, sel }) : b.ConstF(sel == 5 ? 1.0f : 0.0f);
        };
        uint32_t u = comp(t.srcSel[0]), v = comp(t.srcSel[1]), w = comp(t.srcSel[2]);
        const uint32_t sampled = b.SampledImage(ImageType(t.resourceId));
        const uint32_t image = b.Emit(OpLoad, sampled, { TextureVar(t.resourceId) });
        const uint32_t tVec3 = b.Vec(tFloat, 3);
        uint32_t coord;
        if (dim == TexDim::Cube)
            coord = CubeDirection(u, v, w);
        else
        {
            const bool three = dim == TexDim::Tex3D;
            if (!t.coordNormalized[0] || !t.coordNormalized[1] || (three && !t.coordNormalized[2]))
            {
                // Unnormalized (texel) coordinates: divide by the level-0 size.
                const uint32_t img = b.Emit(OpImage, ImageType(t.resourceId), { image });
                const uint32_t n = dim == TexDim::Tex2D ? 2 : 3;
                const uint32_t size = b.Emit(OpImageQuerySizeLod, b.Vec(tInt, n), { img, b.ConstI(0) });
                auto extent = [&](uint32_t i) {
                    return b.Emit(OpConvertSToF, tFloat, { b.Emit(OpCompositeExtract, tInt, { size, i }) });
                };
                if (!t.coordNormalized[0])
                    u = b.Emit(OpFDiv, tFloat, { u, extent(0) });
                if (!t.coordNormalized[1])
                    v = b.Emit(OpFDiv, tFloat, { v, extent(1) });
                if (three && !t.coordNormalized[2])
                    w = b.Emit(OpFDiv, tFloat, { w, extent(2) });
            }
            // 2D arrays take the slice index (unnormalized) as the third coordinate.
            coord = dim == TexDim::Tex2D ? b.Emit(OpCompositeConstruct, tVec2, { u, v })
                                         : b.Emit(OpCompositeConstruct, tVec3, { u, v, w });
        }
        // SAMPLE_L takes the LOD and SAMPLE_LB a LOD bias from the source's w (vertex shaders have no implicit LOD,
        // so their bias acts as the LOD).
        const uint32_t texelType = b.Vec(SampledType(t.resourceId), 4);
        uint32_t texel;
        if (explicitLod || (t.inst == TEX_SAMPLE_LB && req.stage == Stage::Vertex))
        {
            const uint32_t lod = t.inst == TEX_SAMPLE_L || t.inst == TEX_SAMPLE_LB ? comp(t.srcSel[3]) : b.ConstF(0);
            texel = b.Emit(OpImageSampleExplicitLod, texelType, { image, coord, 0x2 /* Lod */, lod });
        }
        else if (t.inst == TEX_SAMPLE_LB)
            texel = b.Emit(OpImageSampleImplicitLod, texelType, { image, coord, 0x1 /* Bias */, comp(t.srcSel[3]) });
        else
            texel = b.Emit(OpImageSampleImplicitLod, texelType, { image, coord });
        if (texelType != tVec4)
            texel = b.Emit(OpBitcast, tVec4, { texel });  // integer texels travel as raw bits
        for (uint32_t c = 0; c < 4; c++)
        {
            const uint32_t sel = t.dstSel[c];
            if (sel == 7)
                continue;
            const uint32_t value = sel < 4 ? b.Emit(OpCompositeExtract, tFloat, { texel, sel }) : b.ConstF(sel == 5 ? 1.0f : 0.0f);
            StoreGpr(t.dstGpr, c, value);
        }
    }

    // ---- ALU ----
    uint32_t Source(const AluGroup& g, const Alu& a, const AluSource& s)
    {
        uint32_t v;
        if (s.sel < 128)
        {
            if (s.rel)
            {
                const uint32_t index = Clamp(b.Emit(OpIAdd, tInt, { b.ConstI(int32_t(s.sel)), IndexRegister(a.indexMode) }), 0, 127);
                v = b.Emit(OpLoad, tFloat, { GprPtrDyn(index, s.chan) });
            }
            else
                v = LoadGpr(s.sel, s.chan);
        }
        else if (s.sel >= SRC_CFILE)
        {
            if (s.rel)
            {
                const uint32_t index = Clamp(b.Emit(OpIAdd, tInt, { b.ConstI(int32_t(s.sel - SRC_CFILE)), IndexRegister(a.indexMode) }), 0, 255);
                v = b.Emit(OpLoad, tFloat, { ConstantPtr(index, s.chan) });
            }
            else
                v = LoadConstant(s.sel - SRC_CFILE, s.chan);
        }
        else if (s.sel < 192)
            throw Unsupported{ "kcache constant source" };
        else
        {
            if (s.rel)
                throw Unsupported{ "relative inline constant" };
            switch (s.sel)
            {
            case SRC_0: v = b.ConstF(0.0f); break;
            case SRC_1: v = b.ConstF(1.0f); break;
            case SRC_1_INT: v = BitsF(1); break;
            case SRC_M_1_INT: v = BitsF(0xFFFFFFFFu); break;
            case SRC_0_5: v = b.ConstF(0.5f); break;
            case SRC_LITERAL: v = BitsF(g.literals[s.chan]); break;
            case SRC_PV:
                if (!pv[s.chan])
                    throw Unsupported{ "PV read with no previous group" };
                v = pv[s.chan];
                break;
            case SRC_PS:
                if (!ps)
                    throw Unsupported{ "PS read with no previous group" };
                v = ps;
                break;
            default: throw Unsupported{ "ALU source select " + std::to_string(s.sel) };
            }
        }
        if (s.abs)
            v = b.Ext(tFloat, GlslFAbs, { v });
        if (s.neg)
            v = b.Emit(OpFNegate, tFloat, { v });
        return v;
    }

    // Legacy (DX9) multiply, used by MUL, MULADD and DOT4: zero times anything (even infinity or NaN) is zero.
    // The *_IEEE forms use IEEE rules. UE3's pow() is LOG_CLAMPED, MUL, EXP_IEEE, which relies on this.
    uint32_t LegacyMul(uint32_t x, uint32_t y)
    {
        const uint32_t zero = b.ConstF(0.0f);
        const uint32_t anyZero = b.Emit(OpLogicalOr, tBool,
            { b.Emit(OpFOrdEqual, tBool, { x, zero }), b.Emit(OpFOrdEqual, tBool, { y, zero }) });
        return b.Emit(OpSelect, tFloat, { anyZero, zero, b.Emit(OpFMul, tFloat, { x, y }) });
    }

    // Infinities replaced: by +-FLT_MAX (the *_CLAMPED instructions) or by zero (the *_FF ones).
    uint32_t ClampInf(uint32_t r, bool toZero)
    {
        const uint32_t inf = b.Emit(OpIsInf, tBool, { r });
        if (toZero)
            return b.Emit(OpSelect, tFloat, { inf, b.ConstF(0.0f), r });
        return b.Emit(OpSelect, tFloat, { inf, b.Ext(tFloat, GlslFClamp, { r, b.ConstF(-3.402823466e38f), b.ConstF(3.402823466e38f) }), r });
    }

    uint32_t FloatBool(uint32_t cond, bool dx10)
    {
        return dx10 ? b.Emit(OpSelect, tFloat, { cond, BitsF(0xFFFFFFFFu), BitsF(0) })
                    : b.Emit(OpSelect, tFloat, { cond, b.ConstF(1.0f), b.ConstF(0.0f) });
    }

    // The condition of a PRED_SET* or KILL* instruction, or nothing for other instructions.
    std::optional<uint32_t> Condition(const Alu& a, const std::array<uint32_t, 3>& s)
    {
        if (a.op3)
            return std::nullopt;
        const uint32_t x = s[0], y = s[1];
        switch (a.inst)
        {
        case 32: case 44: return b.Emit(OpFOrdEqual, tBool, { x, y });              // PRED_SETE, KILLE
        case 33: case 45: return b.Emit(OpFOrdGreaterThan, tBool, { x, y });        // PRED_SETGT, KILLGT
        case 34: case 46: return b.Emit(OpFOrdGreaterThanEqual, tBool, { x, y });   // PRED_SETGE, KILLGE
        case 35: case 47: return b.Emit(OpFUnordNotEqual, tBool, { x, y });         // PRED_SETNE, KILLNE
        // Opcode numbers from the ISA's microcode table (10.x); its per-instruction pages give 70 and 71 to
        // PRED_SETLT_INT / PRED_SETLE_INT, but compiled code uses them as KILLE_INT / KILLGT_INT.
        case 30: case 64: return b.Emit(OpUGreaterThan, tBool, { AsUint(x), AsUint(y) });       // PRED_SETGT_UINT, KILLGT_UINT
        case 31: case 65: return b.Emit(OpUGreaterThanEqual, tBool, { AsUint(x), AsUint(y) });  // PRED_SETGE_UINT, KILLGE_UINT
        case 66: case 70: return b.Emit(OpIEqual, tBool, { AsInt(x), AsInt(y) });               // PRED_SETE_INT, KILLE_INT
        case 67: case 71: return b.Emit(OpSGreaterThan, tBool, { AsInt(x), AsInt(y) });         // PRED_SETGT_INT, KILLGT_INT
        case 68: case 72: return b.Emit(OpSGreaterThanEqual, tBool, { AsInt(x), AsInt(y) });    // PRED_SETGE_INT, KILLGE_INT
        case 69: case 73: return b.Emit(OpINotEqual, tBool, { AsInt(x), AsInt(y) });            // PRED_SETNE_INT, KILLNE_INT
        default: return std::nullopt;
        }
    }

    // CUBE with src0 = R.zzxy, src1 = R.yxzz: x = T, y = S, z = 2 * major axis (signed), w = face id, using the
    // cube-map conventions (major +X: S = -z, T = -y; -X: S = z, T = -y; +Y: S = x, T = z; -Y: S = x, T = -z;
    // +Z: S = x, T = -y; -Z: S = -x, T = -y). Shaders then form S / |2 ma| + 1.5 for the texture unit.
    // Arguments are the four slots' first sources: z, z, x, y.
    std::array<uint32_t, 4> Cube(uint32_t s0x, uint32_t s0y, uint32_t s0z, uint32_t s0w)
    {
        (void)s0y;
        const uint32_t rx = s0z, ry = s0w, rz = s0x;
        const uint32_t ax = b.Ext(tFloat, GlslFAbs, { rx }), ay = b.Ext(tFloat, GlslFAbs, { ry });
        const uint32_t az = b.Ext(tFloat, GlslFAbs, { rz });
        auto neg = [&](uint32_t v) { return b.Emit(OpFNegate, tFloat, { v }); };
        auto pos = [&](uint32_t v) { return b.Emit(OpFOrdGreaterThanEqual, tBool, { v, b.ConstF(0) }); };
        auto sel = [&](uint32_t c, uint32_t x, uint32_t y) { return b.Emit(OpSelect, tFloat, { c, x, y }); };
        // Major axis: x if |x| >= |y| and |x| >= |z|, else y if |y| >= |z|, else z.
        const uint32_t isX = And(b.Emit(OpFOrdGreaterThanEqual, tBool, { ax, ay }), b.Emit(OpFOrdGreaterThanEqual, tBool, { ax, az }));
        const uint32_t isY = And(Not(isX), b.Emit(OpFOrdGreaterThanEqual, tBool, { ay, az }));
        const uint32_t xs = sel(pos(rx), neg(rz), rz), xt = neg(ry);
        const uint32_t ys = rx, yt = sel(pos(ry), rz, neg(rz));
        const uint32_t zs = sel(pos(rz), rx, neg(rx)), zt = neg(ry);
        const uint32_t sc = sel(isX, xs, sel(isY, ys, zs));
        const uint32_t tc = sel(isX, xt, sel(isY, yt, zt));
        const uint32_t ma = sel(isX, rx, sel(isY, ry, rz));
        auto faceOf = [&](uint32_t positive, float base) {
            return sel(positive, b.ConstF(base), b.ConstF(base + 1));
        };
        const uint32_t face = sel(isX, faceOf(pos(rx), 0), sel(isY, faceOf(pos(ry), 2), faceOf(pos(rz), 4)));
        return { tc, sc, b.Emit(OpFMul, tFloat, { ma, b.ConstF(2.0f) }), face };
    }

    static bool HasCondition(const Alu& a)
    {
        return !a.op3 && ((a.inst >= 30 && a.inst <= 35) || (a.inst >= 44 && a.inst <= 47) || (a.inst >= 64 && a.inst <= 73));
    }
    static bool IsPredSet(const Alu& a)
    {
        return !a.op3 && ((a.inst >= 30 && a.inst <= 43) || (a.inst >= 66 && a.inst <= 69) || (a.inst >= 74 && a.inst <= 79));
    }
    static bool IsKill(const Alu& a)
    {
        return !a.op3 && ((a.inst >= 44 && a.inst <= 47) || a.inst == 64 || a.inst == 65 || (a.inst >= 70 && a.inst <= 73));
    }
    static bool IsMova(const Alu& a) { return !a.op3 && (a.inst == 21 || a.inst == 22 || a.inst == 24); }

    uint32_t Op2(const Alu& a, const std::array<uint32_t, 3>& s, uint32_t reduction, std::optional<uint32_t> cond)
    {
        const uint32_t x = s[0], y = s[1];
        if (cond)
            return FloatBool(*cond, false);  // PRED_SET* and KILL* write 1.0 or 0.0
        switch (a.inst)
        {
        case 0: return b.Emit(OpFAdd, tFloat, { x, y });                                  // ADD
        case 1: return LegacyMul(x, y);                                                   // MUL
        case 2: return b.Emit(OpFMul, tFloat, { x, y });                                  // MUL_IEEE
        case 3: case 5: return b.Ext(tFloat, GlslFMax, { x, y });                         // MAX, MAX_DX10
        case 4: case 6: return b.Ext(tFloat, GlslFMin, { x, y });                         // MIN, MIN_DX10
        case 8: return FloatBool(b.Emit(OpFOrdEqual, tBool, { x, y }), false);            // SETE
        case 9: return FloatBool(b.Emit(OpFOrdGreaterThan, tBool, { x, y }), false);      // SETGT
        case 10: return FloatBool(b.Emit(OpFOrdGreaterThanEqual, tBool, { x, y }), false); // SETGE
        case 11: return FloatBool(b.Emit(OpFUnordNotEqual, tBool, { x, y }), false);      // SETNE
        case 12: return FloatBool(b.Emit(OpFOrdEqual, tBool, { x, y }), true);            // SETE_DX10
        case 13: return FloatBool(b.Emit(OpFOrdGreaterThan, tBool, { x, y }), true);
        case 14: return FloatBool(b.Emit(OpFOrdGreaterThanEqual, tBool, { x, y }), true);
        case 15: return FloatBool(b.Emit(OpFUnordNotEqual, tBool, { x, y }), true);
        case 16: return b.Ext(tFloat, GlslFract, { x });                                  // FRACT
        case 17: return b.Ext(tFloat, GlslTrunc, { x });                                  // TRUNC
        case 18: return b.Ext(tFloat, GlslCeil, { x });                                   // CEIL
        case 19: return b.Ext(tFloat, GlslRoundEven, { x });                              // RNDNE
        case 20: return b.Ext(tFloat, GlslFloor, { x });                                  // FLOOR
        case 21: return b.Ext(tFloat, GlslRoundEven, { x });                              // MOVA
        case 22: return b.Ext(tFloat, GlslFloor, { x });                                  // MOVA_FLOOR
        case 24: return x;                                                                // MOVA_INT
        case 25: return x;                                                                // MOV
        case 48: return FromInt(b.Emit(OpBitwiseAnd, tInt, { AsInt(x), AsInt(y) }));      // AND_INT
        case 49: return FromInt(b.Emit(OpBitwiseOr, tInt, { AsInt(x), AsInt(y) }));       // OR_INT
        case 50: return FromInt(b.Emit(OpBitwiseXor, tInt, { AsInt(x), AsInt(y) }));      // XOR_INT
        case 51: return FromInt(b.Emit(OpNot, tInt, { AsInt(x) }));                       // NOT_INT
        case 52: return FromInt(b.Emit(OpIAdd, tInt, { AsInt(x), AsInt(y) }));            // ADD_INT
        case 53: return FromInt(b.Emit(OpISub, tInt, { AsInt(x), AsInt(y) }));            // SUB_INT
        case 54: return FromInt(b.Ext(tInt, GlslSMax, { AsInt(x), AsInt(y) }));           // MAX_INT
        case 55: return FromInt(b.Ext(tInt, GlslSMin, { AsInt(x), AsInt(y) }));           // MIN_INT
        case 56: return FromInt(b.Ext(tUint, GlslUMax, { AsUint(x), AsUint(y) }));        // MAX_UINT
        case 57: return FromInt(b.Ext(tUint, GlslUMin, { AsUint(x), AsUint(y) }));        // MIN_UINT
        case 58: return FloatBool(b.Emit(OpIEqual, tBool, { AsInt(x), AsInt(y) }), true);          // SETE_INT
        case 59: return FloatBool(b.Emit(OpSGreaterThan, tBool, { AsInt(x), AsInt(y) }), true);    // SETGT_INT
        case 60: return FloatBool(b.Emit(OpSGreaterThanEqual, tBool, { AsInt(x), AsInt(y) }), true);
        case 61: return FloatBool(b.Emit(OpINotEqual, tBool, { AsInt(x), AsInt(y) }), true);       // SETNE_INT
        case 62: return FloatBool(b.Emit(OpUGreaterThan, tBool, { AsUint(x), AsUint(y) }), true);  // SETGT_UINT
        case 63: return FloatBool(b.Emit(OpUGreaterThanEqual, tBool, { AsUint(x), AsUint(y) }), true);
        case 80: case 81: case 82: case 83: return reduction;                             // DOT4(_IEEE), CUBE, MAX4
        case 97: return b.Ext(tFloat, GlslExp2, { x });                                   // EXP_IEEE
        case 98: return ClampInf(b.Ext(tFloat, GlslLog2, { x }), false);                  // LOG_CLAMPED: -inf -> -FLT_MAX
        case 99: return b.Ext(tFloat, GlslLog2, { x });                                   // LOG_IEEE
        case 100: return ClampInf(b.Emit(OpFDiv, tFloat, { b.ConstF(1.0f), x }), false);  // RECIP_CLAMPED
        case 101: return ClampInf(b.Emit(OpFDiv, tFloat, { b.ConstF(1.0f), x }), true);   // RECIP_FF
        case 102: return b.Emit(OpFDiv, tFloat, { b.ConstF(1.0f), x });                   // RECIP_IEEE
        case 103: return ClampInf(b.Ext(tFloat, GlslInverseSqrt, { x }), false);          // RECIPSQRT_CLAMPED
        case 104: return ClampInf(b.Ext(tFloat, GlslInverseSqrt, { x }), true);           // RECIPSQRT_FF
        case 105: return b.Ext(tFloat, GlslInverseSqrt, { x });                           // RECIPSQRT_IEEE
        case 106: return b.Ext(tFloat, GlslSqrt, { x });                                  // SQRT_IEEE
        case 107: return FromInt(b.Emit(OpConvertFToS, tInt, { x }));                     // FLT_TO_INT
        case 108: return b.Emit(OpConvertSToF, tFloat, { AsInt(x) });                     // INT_TO_FLT
        case 109: return b.Emit(OpConvertUToF, tFloat, { AsUint(x) });                    // UINT_TO_FLT
        case 110: return b.Ext(tFloat, GlslSin, { x });                                   // SIN
        case 111: return b.Ext(tFloat, GlslCos, { x });                                   // COS
        case 112: return FromInt(b.Emit(OpShiftRightArithmetic, tInt, { AsInt(x), AsInt(y) }));  // ASHR_INT
        case 113: return FromInt(b.Emit(OpShiftRightLogical, tUint, { AsUint(x), AsUint(y) }));  // LSHR_INT
        case 114: return FromInt(b.Emit(OpShiftLeftLogical, tUint, { AsUint(x), AsUint(y) }));   // LSHL_INT
        case 115: case 117: return FromInt(b.Emit(OpIMul, tInt, { AsInt(x), AsInt(y) }));        // MULLO_INT/UINT
        case 121: return FromInt(b.Emit(OpConvertFToU, tUint, { x }));                    // FLT_TO_UINT
        default:
            throw Unsupported{ std::string("ALU instruction ") + AluName(false, a.inst) };
        }
    }

    uint32_t Op3(const Alu& a, const std::array<uint32_t, 3>& s)
    {
        switch (a.inst)
        {
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:  // MULADD[_IEEE][_M2/_M4/_D2]
        {
            // 16-19 are the legacy forms (zero times anything is zero), 20-23 the IEEE ones.
            const uint32_t product = a.inst < 20 ? LegacyMul(s[0], s[1]) : b.Emit(OpFMul, tFloat, { s[0], s[1] });
            uint32_t r = b.Emit(OpFAdd, tFloat, { product, s[2] });
            const uint32_t variant = (a.inst - 16) & 3;  // 0 none, 1 *2, 2 *4, 3 /2
            if (variant)
                r = b.Emit(OpFMul, tFloat, { r, b.ConstF(variant == 1 ? 2.0f : variant == 2 ? 4.0f : 0.5f) });
            return r;
        }
        case 24: return b.Emit(OpSelect, tFloat, { b.Emit(OpFOrdEqual, tBool, { s[0], b.ConstF(0) }), s[1], s[2] });  // CMOVE
        case 25: return b.Emit(OpSelect, tFloat, { b.Emit(OpFOrdGreaterThan, tBool, { s[0], b.ConstF(0) }), s[1], s[2] });
        case 26: return b.Emit(OpSelect, tFloat, { b.Emit(OpFOrdGreaterThanEqual, tBool, { s[0], b.ConstF(0) }), s[1], s[2] });
        case 28: return b.Emit(OpSelect, tFloat, { b.Emit(OpIEqual, tBool, { AsInt(s[0]), b.ConstI(0) }), s[1], s[2] });
        case 29: return b.Emit(OpSelect, tFloat, { b.Emit(OpSGreaterThan, tBool, { AsInt(s[0]), b.ConstI(0) }), s[1], s[2] });
        case 30: return b.Emit(OpSelect, tFloat, { b.Emit(OpSGreaterThanEqual, tBool, { AsInt(s[0]), b.ConstI(0) }), s[1], s[2] });
        default:
            throw Unsupported{ std::string("ALU instruction ") + AluName(true, a.inst) };
        }
    }

    void ExecuteGroup(const AluGroup& g)
    {
        // Every instruction in a group reads its sources before any result is written.
        std::vector<std::array<uint32_t, 3>> sources(g.insts.size());
        for (size_t i = 0; i < g.insts.size(); i++)
        {
            const Alu& a = g.insts[i];
            if (a.updateExecMask && !flow)
                throw Unsupported{ "exec mask update outside flow control" };  // NeedsFlowControl prevents this
            // PRED_SET_INV/POP/CLR/RESTORE and the *_PUSH forms drive the hardware's predicate counters.
            if (IsPredSet(a) && !HasCondition(a))
                throw Unsupported{ std::string("ALU instruction ") + AluName(false, a.inst) };
            for (uint32_t k = 0; k < a.numSrc; k++)
                sources[i][k] = Source(g, a, a.src[k]);
        }
        // The predicate as it was before this group (PRED_SEL reads it; PRED_SET in the group updates it after).
        const uint32_t pred = b.Emit(OpLoad, tBool, { vPred });

        // Reductions (DOT4, MAX4, CUBE) combine the sources of the four vector-slot instructions.
        uint32_t reduction = 0;
        std::array<uint32_t, 4> cube{};
        for (size_t i = 0; i < g.insts.size(); i++)
        {
            const Alu& a = g.insts[i];
            if (!IsReduction(a) || reduction)
                continue;
            std::array<int, 4> slotInst{ -1, -1, -1, -1 };
            for (size_t k = 0; k < g.insts.size(); k++)
                if (g.insts[k].slot < 4 && IsReduction(g.insts[k]))
                    slotInst[g.insts[k].slot] = int(k);
            for (int c = 0; c < 4; c++)
                if (slotInst[c] < 0)
                    throw Unsupported{ "incomplete DOT4/MAX4 group" };
            if (a.inst == 82)
            {
                cube = Cube(sources[slotInst[0]][0], sources[slotInst[1]][0], sources[slotInst[2]][0],
                    sources[slotInst[3]][0]);
                reduction = cube[0];
            }
            else if (a.inst == 83)
            {
                reduction = sources[slotInst[0]][0];
                for (int c = 1; c < 4; c++)
                    reduction = b.Ext(tFloat, GlslFMax, { reduction, sources[slotInst[c]][0] });
            }
            else
            {
                // DOT4 uses the legacy multiply (zero times anything is zero), DOT4_IEEE the IEEE one.
                auto mul = [&](int c) {
                    const uint32_t x = sources[slotInst[c]][0], y = sources[slotInst[c]][1];
                    return a.inst == 80 ? LegacyMul(x, y) : b.Emit(OpFMul, tFloat, { x, y });
                };
                reduction = mul(0);
                for (int c = 1; c < 4; c++)
                    reduction = b.Emit(OpFAdd, tFloat, { reduction, mul(c) });
            }
        }

        std::array<uint32_t, 4> newPv = pv;
        uint32_t newPs = ps;
        struct Write
        {
            uint32_t gpr, gprDyn, chan, value, predicate;
        };
        std::vector<Write> writes;
        std::vector<std::pair<uint32_t, uint32_t>> arWrites;  // (component, int value)
        std::optional<uint32_t> newPred, execCond, kill;
        for (size_t i = 0; i < g.insts.size(); i++)
        {
            const Alu& a = g.insts[i];
            if (!a.op3 && a.inst == 26)  // NOP
                continue;
            const std::optional<uint32_t> cond = Condition(a, sources[i]);
            uint32_t r = a.op3 ? Op3(a, sources[i]) : Op2(a, sources[i], reduction, cond);
            if (!a.op3 && a.inst == 82 && a.slot < 4)
                r = cube[a.slot];  // CUBE writes a different result to each slot
            if (!a.op3 && a.omod)
                r = b.Emit(OpFMul, tFloat, { r, b.ConstF(a.omod == 1 ? 2.0f : a.omod == 2 ? 4.0f : 0.5f) });
            if (a.clamp)
                r = b.Ext(tFloat, GlslFClamp, { r, b.ConstF(0.0f), b.ConstF(1.0f) });
            if (a.slot < 4)
                newPv[a.slot] = r;
            else
                newPs = r;
            // PRED_SEL: 2 = execute where the predicate is false, 3 = where it is true.
            uint32_t predicate = 0;
            if (a.predSel == 2)
                predicate = Not(pred);
            else if (a.predSel == 3)
                predicate = pred;
            if (IsPredSet(a))
            {
                if (a.updatePred)
                    newPred = *cond;
                if (a.updateExecMask)
                    execCond = *cond;
            }
            if (IsKill(a) && req.stage == Stage::Pixel)
                kill = predicate ? And(predicate, *cond) : *cond;
            if (IsMova(a))
            {
                // MOVA*: the address register component of this slot (clamped like the hardware's 9-bit AR).
                const uint32_t i32 = a.inst == 24 ? AsInt(sources[i][0]) : b.Emit(OpConvertFToS, tInt, { r });
                arWrites.push_back({ a.slot & 3, Clamp(i32, -256, 255) });
            }
            if (a.writeMask)
            {
                uint32_t dyn = 0;
                if (a.dstRel)
                    dyn = Clamp(b.Emit(OpIAdd, tInt, { b.ConstI(int32_t(a.dstGpr)), IndexRegister(a.indexMode) }), 0, 127);
                writes.push_back({ a.dstGpr, dyn, a.dstChan, r, predicate });
            }
        }
        for (const Write& w : writes)
        {
            const uint32_t ptr = w.gprDyn ? GprPtrDyn(w.gprDyn, w.chan) : GprPtr(w.gpr, w.chan);
            uint32_t value = w.value;
            if (w.predicate)
                value = b.Emit(OpSelect, tFloat, { w.predicate, value, b.Emit(OpLoad, tFloat, { ptr }) });
            b.EmitNoResult(OpStore, { ptr, value });
        }
        for (auto [c, v] : arWrites)
            StoreElem(vAr, b.ConstI(int32_t(c)), v);
        if (newPred)
            b.EmitNoResult(OpStore, { vPred, *newPred });
        if (execCond)
        {
            b.EmitNoResult(OpStore, { vExecSet, b.ConstBool(true) });
            b.EmitNoResult(OpStore, { vExecCond, *execCond });
        }
        pv = newPv;
        ps = newPs;
        if (kill)
            If(*kill, [&] {
                b.EmitNoResult(OpKill, {});
                return true;
            });
    }
};
} // namespace

Result Translate(const Request& request)
{
    const Program p = Decode(request.program, request.size);
    if (!p.error.empty())
        return { {}, "decode: " + p.error, 0, 0 };
    return Translator(request, p).Run();
}

uint64_t HashProgram(Stage stageValue, bool halfZValue, const uint8_t* program, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 1099511628211ull;
    };
    const uint32_t stage = uint32_t(stageValue), halfZ = halfZValue, version = 6;
    mix(&version, 4);  // bump when the translation changes, so stale disk-cache entries are not reused
    mix(&stage, 4);
    mix(&halfZ, 4);
    mix(program, size);
    return h;
}

namespace
{
// The bytes Hash mixes after the program state, in order (FNV-1a streams, so hashing them in one piece gives the
// same value). False if they do not fit `buf`.
bool HashBytes(const Request& r, uint8_t* buf, size_t cap, size_t& n)
{
    n = 0;
    bool fits = true;
    auto put = [&](const void* data, size_t len) {
        if (n + len > cap)
        {
            fits = false;
            return;
        }
        memcpy(buf + n, data, len);
        n += len;
    };
    for (const VertexInput& v : r.vertexInputs)
        put(&v, sizeof(v));
    for (int32_t v : r.pixelInputs)
        put(&v, 4);
    put(r.loopConsts.data(), sizeof(r.loopConsts));
    put(&r.alphaFunc, 4);
    put(r.textureDims.data(), r.textureDims.size());
    put(r.textureTypes.data(), r.textureTypes.size());
    put(r.outputTypes.data(), r.outputTypes.size());
    for (uint8_t d : r.pixelInputDefaults)
        if (d)
            put(&d, 1);  // only non-zero defaults, so hashes of shaders without them are unchanged
    if (r.positionGpr >= 0)
        put(&r.positionGpr, 4);  // only when set, as above
    if (r.debugGpr >= 0)
    {
        put(&r.debugGpr, 4);  // only when set, so normal hashes match capture logs
        put(&r.debugCf, 4);
    }
    return fits;
}

uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// A quick hash of a key's bytes, 8 at a time (only to find the memo entry; the key itself is compared).
struct QuickHash
{
    using is_transparent = void;
    size_t operator()(std::string_view k) const
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ k.size();
        size_t i = 0;
        for (; i + 8 <= k.size(); i += 8)
        {
            uint64_t v;
            memcpy(&v, k.data() + i, 8);
            h = (h ^ v) * 0xFF51AFD7ED558CCDull;
            h ^= h >> 29;
        }
        for (; i < k.size(); i++)
            h = (h ^ uint8_t(k[i])) * 0x100000001B3ull;
        return size_t(h ^ (h >> 32));
    }
};
} // namespace

uint64_t Hash(const Request& r)
{
    // Two per draw over ~300 bytes, one byte at a time: about 5% of the GPU thread in heavy scenes. The values key the
    // shader and pipeline caches on disk, so they stay as they are and are memoized per thread instead.
    const uint64_t h0 = r.programState ? r.programState : HashProgram(r.stage, r.halfZ, r.program, r.size);
    uint8_t buf[2048];
    memcpy(buf, &h0, 8);
    size_t n;
    if (!HashBytes(r, buf + 8, sizeof(buf) - 8, n))
    {
        // (too large to memoize: hash the fields directly)
        std::vector<uint8_t> big(64 * 1024);
        size_t m;
        HashBytes(r, big.data(), big.size(), m);
        return Fnv(h0, big.data(), m);
    }
    thread_local std::unordered_map<std::string, uint64_t, QuickHash, std::equal_to<>> memo;
    const std::string_view key(reinterpret_cast<const char*>(buf), n + 8);
    if (auto it = memo.find(key); it != memo.end())
        return it->second;
    const uint64_t h = Fnv(h0, buf + 8, n);
    if (memo.size() > 50000)
        memo.clear();
    memo.emplace(std::string(key), h);
    return h;
}
} // namespace gpu::shader
