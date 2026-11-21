#include "spirv.h"

#include <bit>
#include <cstring>

namespace gpu::spirv
{
namespace
{
constexpr uint32_t Magic = 0x07230203;
constexpr uint32_t Version = 0x00010000;  // SPIR-V 1.0: accepted by every Vulkan 1.x driver
constexpr uint32_t CapabilityShader = 1;
constexpr uint32_t AddressingLogical = 0, MemoryModelGLSL450 = 1;
constexpr uint32_t ExecutionModeOriginUpperLeft = 7;

// String operand: UTF-8, NUL-terminated, padded to a word boundary.
std::vector<uint32_t> StringWords(const char* s)
{
    const size_t n = strlen(s) + 1;
    std::vector<uint32_t> w((n + 3) / 4, 0);
    memcpy(w.data(), s, n);
    return w;
}
} // namespace

Builder::Builder() { glsl = NewId(); }

void Builder::Put(std::vector<uint32_t>& out, Op op, std::initializer_list<uint32_t> words)
{
    out.push_back(uint32_t(words.size() + 1) << 16 | op);
    out.insert(out.end(), words);
}

void Builder::Put(std::vector<uint32_t>& out, Op op, const std::vector<uint32_t>& words)
{
    out.push_back(uint32_t(words.size() + 1) << 16 | op);
    out.insert(out.end(), words.begin(), words.end());
}

uint32_t Builder::Cached(const std::vector<uint32_t>& key, Op op, std::initializer_list<uint32_t> operands)
{
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    const uint32_t id = NewId();
    std::vector<uint32_t> w;
    // Types have the result id first; constants have the result type first.
    const bool isConstant = op == OpConstant || op == OpConstantComposite || op == OpConstantTrue || op == OpConstantFalse;
    auto ops = operands.begin();
    if (isConstant)
    {
        w.push_back(*ops++);
        w.push_back(id);
    }
    else
        w.push_back(id);
    w.insert(w.end(), ops, operands.end());
    Put(globals, op, w);
    cache[key] = id;
    return id;
}

uint32_t Builder::Void() { return Cached({ OpTypeVoid }, OpTypeVoid, {}); }
uint32_t Builder::Bool() { return Cached({ OpTypeBool }, OpTypeBool, {}); }
uint32_t Builder::Float() { return Cached({ OpTypeFloat, 32 }, OpTypeFloat, { 32 }); }
uint32_t Builder::Int() { return Cached({ OpTypeInt, 32, 1 }, OpTypeInt, { 32, 1 }); }
uint32_t Builder::Uint() { return Cached({ OpTypeInt, 32, 0 }, OpTypeInt, { 32, 0 }); }
uint32_t Builder::Vec(uint32_t c, uint32_t n) { return Cached({ OpTypeVector, c, n }, OpTypeVector, { c, n }); }
uint32_t Builder::RuntimeArray(uint32_t e)
{
    const uint32_t id = NewId();
    Put(globals, OpTypeRuntimeArray, { id, e });
    return id;
}

uint32_t Builder::Array(uint32_t e, uint32_t length)
{
    const uint32_t len = ConstU(length);
    return Cached({ OpTypeArray, e, len }, OpTypeArray, { e, len });
}
uint32_t Builder::Pointer(StorageClass sc, uint32_t t) { return Cached({ OpTypePointer, sc, t }, OpTypePointer, { sc, t }); }
uint32_t Builder::FunctionType(uint32_t ret) { return Cached({ OpTypeFunction, ret }, OpTypeFunction, { ret }); }

uint32_t Builder::Image(uint32_t dim, bool arrayed, uint32_t sampledType)
{
    // OpTypeImage: sampled type, Dim, depth 0, arrayed, MS 0, sampled 1, format Unknown (0).
    const uint32_t f = sampledType ? sampledType : Float();
    const uint32_t a = arrayed ? 1 : 0;
    return Cached({ OpTypeImage, f, dim, 0, a, 0, 1, 0 }, OpTypeImage, { f, dim, 0, a, 0, 1, 0 });
}
uint32_t Builder::SampledImage(uint32_t image) { return Cached({ OpTypeSampledImage, image }, OpTypeSampledImage, { image }); }
uint32_t Builder::Image2D() { return Image(Dim2D, false); }
uint32_t Builder::SampledImage2D() { return SampledImage(Image2D()); }
void Builder::Capability(uint32_t capability) { capabilities.insert(capability); }

uint32_t Builder::Struct(std::initializer_list<uint32_t> members)
{
    const uint32_t id = NewId();
    std::vector<uint32_t> w{ id };
    w.insert(w.end(), members);
    Put(globals, OpTypeStruct, w);
    return id;
}

uint32_t Builder::ConstF(float v)
{
    const uint32_t f = Float(), bits = std::bit_cast<uint32_t>(v);
    return Cached({ OpConstant, f, bits }, OpConstant, { f, bits });
}
uint32_t Builder::ConstU(uint32_t v)
{
    const uint32_t u = Uint();
    return Cached({ OpConstant, u, v }, OpConstant, { u, v });
}
uint32_t Builder::ConstI(int32_t v)
{
    const uint32_t i = Int();
    return Cached({ OpConstant, i, uint32_t(v) }, OpConstant, { i, uint32_t(v) });
}
uint32_t Builder::ConstBool(bool v)
{
    const uint32_t b = Bool();
    return v ? Cached({ OpConstantTrue, b }, OpConstantTrue, { b }) : Cached({ OpConstantFalse, b }, OpConstantFalse, { b });
}
uint32_t Builder::ConstVec4(float x, float y, float z, float w)
{
    const uint32_t v4 = Vec(Float(), 4), a = ConstF(x), b = ConstF(y), c = ConstF(z), d = ConstF(w);
    return Cached({ OpConstantComposite, v4, a, b, c, d }, OpConstantComposite, { v4, a, b, c, d });
}

uint32_t Builder::GlobalVar(StorageClass sc, uint32_t type, const char* name)
{
    const uint32_t id = NewId();
    Put(globals, OpVariable, { Pointer(sc, type), id, sc });
    if (name)
        Name(id, name);
    return id;
}

void Builder::Decorate(uint32_t id, Decoration d, std::initializer_list<uint32_t> args)
{
    std::vector<uint32_t> w{ id, d };
    w.insert(w.end(), args);
    Put(annotations, OpDecorate, w);
}

void Builder::MemberDecorate(uint32_t type, uint32_t member, Decoration d, std::initializer_list<uint32_t> args)
{
    std::vector<uint32_t> w{ type, member, d };
    w.insert(w.end(), args);
    Put(annotations, OpMemberDecorate, w);
}

void Builder::Name(uint32_t id, const char* name)
{
    std::vector<uint32_t> w{ id };
    const auto s = StringWords(name);
    w.insert(w.end(), s.begin(), s.end());
    Put(debug, OpName, w);
}

uint32_t Builder::Emit(Op op, uint32_t resultType, std::initializer_list<uint32_t> operands)
{
    const uint32_t id = NewId();
    std::vector<uint32_t> w{ resultType, id };
    w.insert(w.end(), operands);
    Put(code, op, w);
    return id;
}

void Builder::EmitNoResult(Op op, std::initializer_list<uint32_t> operands) { Put(code, op, operands); }
void Builder::EmitNoResult(Op op, const std::vector<uint32_t>& operands) { Put(code, op, operands); }

uint32_t Builder::Ext(uint32_t resultType, Glsl inst, std::initializer_list<uint32_t> args)
{
    const uint32_t id = NewId();
    std::vector<uint32_t> w{ resultType, id, glsl, inst };
    w.insert(w.end(), args);
    Put(code, OpExtInst, w);
    return id;
}

void Builder::Label(uint32_t id) { Put(code, OpLabel, { id }); }

std::vector<uint32_t> Builder::Finish(ExecutionModel model, uint32_t functionId, const std::vector<uint32_t>& interfaceVars)
{
    // Make sure types referenced by the function header exist before computing the bound.
    const uint32_t voidType = Void(), fnType = FunctionType(voidType);
    std::vector<uint32_t> out{ Magic, Version, 0 /* generator */, 0 /* bound, patched below */, 0 };
    Put(out, OpCapability, { CapabilityShader });
    for (uint32_t c : capabilities)
        Put(out, OpCapability, { c });
    {
        std::vector<uint32_t> w{ glsl };
        const auto s = StringWords("GLSL.std.450");
        w.insert(w.end(), s.begin(), s.end());
        Put(out, OpExtInstImport, w);
    }
    Put(out, OpMemoryModel, { AddressingLogical, MemoryModelGLSL450 });
    {
        std::vector<uint32_t> w{ uint32_t(model), functionId };
        const auto s = StringWords("main");
        w.insert(w.end(), s.begin(), s.end());
        w.insert(w.end(), interfaceVars.begin(), interfaceVars.end());
        Put(out, OpEntryPoint, w);
    }
    if (model == Fragment)
        Put(out, OpExecutionMode, { functionId, ExecutionModeOriginUpperLeft });
    out.insert(out.end(), debug.begin(), debug.end());
    out.insert(out.end(), annotations.begin(), annotations.end());
    out.insert(out.end(), globals.begin(), globals.end());
    Put(out, OpFunction, { voidType, functionId, 0 /* FunctionControl None */, fnType });
    out.insert(out.end(), code.begin(), code.end());
    Put(out, OpFunctionEnd, {});
    out[3] = nextId;
    return out;
}
} // namespace gpu::spirv
