// dt_shader: development tool for the R700 shader translator.
//
//   dt_shader prog.bin...    disassemble GX2 shader programs saved by `dt_port --gx2-dump DIR`, translate them
//                            to SPIR-V and write prog.spv next to each one
//
// The stage comes from the file name (vs_* / ps_*). Vertex inputs are derived from the shader's semantic
// table in the matching .gx2 header (input i -> GPR i+1 at location i, components unchanged); pixel input i
// reads location i.
#include "r700.h"
#include "translator.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t off)
{
    return off + 4 <= d.size() ? uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 | d[off + 3] : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s program.bin...\n", argv[0]);
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++)
    {
        const std::string path = argv[i];
        const std::vector<uint8_t> data = ReadFile(path);
        const gpu::r700::Program p = gpu::r700::Decode(data.data(), data.size());
        printf("== %s (%zu bytes)\n%s", path.c_str(), data.size(), gpu::r700::Disassemble(p).c_str());

        const std::string base = path.substr(0, path.rfind('.'));
        const std::string name = base.substr(base.find_last_of("/\\") + 1);
        gpu::shader::Request req;
        req.stage = name.rfind("ps_", 0) == 0 ? gpu::shader::Stage::Pixel : gpu::shader::Stage::Vertex;
        req.program = data.data();
        req.size = data.size();
        const std::vector<uint8_t> header = ReadFile(base + ".gx2");
        if (req.stage == gpu::shader::Stage::Vertex)
        {
            const uint32_t count = Be32(header, 16 * 4);  // num_sq_vtx_semantic
            for (uint32_t k = 0; k < count && k < 32; k++)
                req.vertexInputs.push_back({ k, k + 1, { 0, 1, 2, 3 } });
        }
        else
        {
            const uint32_t count = Be32(header, 4 * 4);  // num_spi_ps_input_cntl
            for (uint32_t k = 0; k < count && k < 32; k++)
                req.pixelInputs.push_back(int32_t(k));
        }
        const gpu::shader::Result r = gpu::shader::Translate(req);
        if (!r.error.empty())
        {
            printf("translate: %s\n", r.error.c_str());
            status = 1;
            continue;
        }
        std::ofstream out(base + ".spv", std::ios::binary);
        out.write(reinterpret_cast<const char*>(r.spirv.data()), std::streamsize(r.spirv.size() * 4));
        printf("translate: %zu words -> %s.spv (textures 0x%X, outputs 0x%X)\n", r.spirv.size(), base.c_str(),
            r.textureMask, r.outputMask);
    }
    return status;
}
