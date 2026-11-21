// Minimal SPIR-V module builder: just what the R700 translator needs. Types and constants are deduplicated.
// Opcode and enum values are from the SPIR-V 1.0 specification and the GLSL.std.450 extended instruction set.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gpu::spirv
{
enum Op : uint16_t
{
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeImage = 25, OpTypeRuntimeArray = 29, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageFetch = 95, OpImage = 100,
    OpImageQuerySizeLod = 103, OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112,
    OpBitcast = 124, OpSNegate = 126, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130, OpFSub = 131,
    OpIMul = 132, OpFMul = 133, OpUDiv = 134, OpFDiv = 136, OpDot = 148, OpUMulExtended = 151, OpSMulExtended = 152,
    OpIsNan = 156, OpIsInf = 157, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171,
    OpUGreaterThan = 172, OpSGreaterThan = 173, OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175,
    OpULessThan = 176, OpSLessThan = 177, OpFOrdEqual = 180, OpFUnordNotEqual = 183, OpFOrdLessThan = 184,
    OpFOrdGreaterThan = 186, OpFOrdGreaterThanEqual = 190, OpShiftRightLogical = 194, OpShiftRightArithmetic = 195,
    OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseXor = 198, OpBitwiseAnd = 199, OpNot = 200,
    OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250,
    OpSwitch = 251, OpKill = 252, OpReturn = 253, OpSLessThanEqual = 179,
};

enum GlslExtra : uint32_t { GlslUnpackHalf2x16 = 62 };
enum Glsl : uint32_t
{
    GlslRoundEven = 2, GlslTrunc = 3, GlslFAbs = 4, GlslFloor = 8, GlslCeil = 9, GlslFract = 10, GlslSin = 13,
    GlslCos = 14, GlslExp2 = 29, GlslLog2 = 30, GlslSqrt = 31, GlslInverseSqrt = 32, GlslFMin = 37, GlslUMin = 38,
    GlslSMin = 39, GlslFMax = 40, GlslUMax = 41, GlslSMax = 42, GlslFClamp = 43, GlslSClamp = 45, GlslNMin = 79, GlslNMax = 80,
};

enum StorageClass : uint32_t { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Function = 7 };
enum Decoration : uint32_t
{
    DecBlock = 2, DecBufferBlock = 3, DecArrayStride = 6, DecNonWritable = 24, DecBuiltIn = 11, DecFlat = 14, DecLocation = 30, DecBinding = 33,
    DecDescriptorSet = 34, DecOffset = 35,
};
enum BuiltIn : uint32_t { BuiltInPosition = 0, BuiltInFragCoord = 15, BuiltInVertexIndex = 42, BuiltInInstanceIndex = 43 };
enum ExecutionModel : uint32_t { Vertex = 0, Fragment = 4 };
enum Dim : uint32_t { Dim1D = 0, Dim2D = 1, Dim3D = 2, DimCube = 3 };
enum CapabilityId : uint32_t { CapClipDistance = 32, CapCullDistance = 33, CapSampledCubeArray = 45, CapImageQuery = 50 };

class Builder
{
public:
    Builder();

    uint32_t NewId() { return nextId++; }

    // ---- types (deduplicated) ----
    uint32_t Void();
    uint32_t Bool();
    uint32_t Float();
    uint32_t Int();
    uint32_t Uint();
    uint32_t Vec(uint32_t component, uint32_t n);
    uint32_t Array(uint32_t element, uint32_t length);
    uint32_t RuntimeArray(uint32_t element);  // not deduplicated (each gets its own ArrayStride decoration)
    uint32_t Pointer(StorageClass sc, uint32_t type);
    uint32_t FunctionType(uint32_t ret);
    // Sampled image of SPIR-V Dim (Dim2D, Dim3D, DimCube) whose texels are `sampledType` (float by default).
    uint32_t Image(uint32_t dim, bool arrayed, uint32_t sampledType = 0);
    uint32_t SampledImage(uint32_t image);       // combined image sampler
    uint32_t Image2D();         // 2D float sampled image
    uint32_t SampledImage2D();  // combined image sampler of Image2D
    void Capability(uint32_t capability);  // beyond Shader
    uint32_t Struct(std::initializer_list<uint32_t> members);  // not deduplicated

    // ---- constants (deduplicated) ----
    uint32_t ConstF(float v);
    uint32_t ConstU(uint32_t v);
    uint32_t ConstI(int32_t v);
    uint32_t ConstBool(bool v);
    uint32_t ConstVec4(float x, float y, float z, float w);

    // ---- globals, decorations, debug names ----
    uint32_t GlobalVar(StorageClass sc, uint32_t type, const char* name = nullptr);
    void Decorate(uint32_t id, Decoration d, std::initializer_list<uint32_t> args = {});
    void MemberDecorate(uint32_t type, uint32_t member, Decoration d, std::initializer_list<uint32_t> args = {});
    void Name(uint32_t id, const char* name);

    // ---- function bodies ----
    uint32_t GlslSet() const { return glsl; }
    // Append an instruction producing a result: operands follow the result-type/result-id pair.
    uint32_t Emit(Op op, uint32_t resultType, std::initializer_list<uint32_t> operands);
    void EmitNoResult(Op op, std::initializer_list<uint32_t> operands);
    void EmitNoResult(Op op, const std::vector<uint32_t>& operands);
    uint32_t Ext(uint32_t resultType, Glsl inst, std::initializer_list<uint32_t> args);
    void Label(uint32_t id);

    // Assemble the module with one entry point whose body is the emitted code.
    std::vector<uint32_t> Finish(ExecutionModel model, uint32_t functionId, const std::vector<uint32_t>& interfaceVars);

private:
    static void Put(std::vector<uint32_t>& out, Op op, std::initializer_list<uint32_t> words);
    static void Put(std::vector<uint32_t>& out, Op op, const std::vector<uint32_t>& words);
    uint32_t Cached(const std::vector<uint32_t>& key, Op op, std::initializer_list<uint32_t> operands);

    uint32_t nextId = 1;
    uint32_t glsl = 0;
    std::vector<uint32_t> annotations, debug, globals, code;
    std::map<std::vector<uint32_t>, uint32_t> cache;
    std::set<uint32_t> capabilities;
};
} // namespace gpu::spirv
