// dt_recomp: statically recompile a Wii U RPX into C++.
//
//   dt_recomp <input.elf> <output_dir> [--hooks FILE] recompile everything; FILE (default: src/recomp/hooks.txt)
//                                                    lists the guest functions the runtime may replace
//   dt_recomp <input.elf> --decode-dump <file>       write "address mnemonic" for every code word
#include "decoder.h"
#include "recompiler.h"
#include "rpx.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
constexpr size_t InstructionsPerFile = 20000;

// Only rewrite files whose contents changed, so rebuilds stay incremental.
size_t gFilesWritten = 0;
void WriteIfChanged(const fs::path& path, const std::string& content)
{
    if (fs::exists(path) && fs::file_size(path) == content.size())
    {
        std::ifstream in(path, std::ios::binary);
        std::string old(content.size(), '\0');
        in.read(old.data(), old.size());
        if (old == content)
            return;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), content.size());
    gFilesWritten++;
}

int DecodeDump(const RpxImage& image, const char* path)
{
    std::ofstream out(path);
    for (const CodeSection& c : image.code)
        for (uint32_t a = c.addr; a + 4 <= c.addr + c.bytes.size(); a += 4)
        {
            const uint32_t w = image.ReadCode(a);
            out << std::format("{:08X} {}{}\n", a, OpName(Decode(w)), "");
        }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <input.elf> <output_dir>\n       %s <input.elf> --decode-dump <file>\n", argv[0], argv[0]);
        return 2;
    }

    RpxImage image;
    std::string error;
    if (!image.Load(argv[1], error))
    {
        fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const RelocStats& rs = image.relocStats;
    printf("loaded %s: %zu code sections (0x%08X-0x%08X), %zu functions, %zu imports\n", argv[1], image.code.size(),
        image.CodeBase(), image.CodeEnd(), image.functions.size(), image.imports.size());
    printf("relocations: %zu internal verified, %zu internal patched, %zu import refs patched, %zu import calls, %zu unsupported\n",
        rs.internalVerified, rs.internalPatched, rs.importPatched, rs.importCalls, rs.unsupported);

    const size_t secondary = image.AddSecondaryEntryPoints();
    printf("secondary entry points (branches into function bodies or unnamed code): %zu\n", secondary);

    if (std::string(argv[2]) == "--decode-dump")
        return argc > 3 ? DecodeDump(image, argv[3]) : 2;

    const fs::path outDir = argv[2];
    fs::create_directories(outDir);
    Recompiler recomp(image);

    // ---- hooks: "0xADDRESS" or a symbol name per line, '#' starts a comment ----
    std::string hooksPath = DT_HOOKS_FILE;
    for (int i = 3; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--hooks")
            hooksPath = argv[i + 1];
    if (std::ifstream in(hooksPath); in)
    {
        std::string line;
        while (std::getline(in, line))
        {
            line = line.substr(0, line.find('#'));
            line.erase(0, line.find_first_not_of(" \t\r"));
            line.erase(line.find_last_not_of(" \t\r") + 1);
            if (line.empty())
                continue;
            uint32_t addr = 0;
            if (line.starts_with("0x") || line.starts_with("0X"))
                addr = uint32_t(std::stoul(line, nullptr, 16));
            else
                for (const Function& fn : image.functions)
                    if (fn.name == line)
                        addr = fn.addr;
            if (!std::any_of(image.functions.begin(), image.functions.end(), [&](const Function& f) { return f.addr == addr; }))
            {
                fprintf(stderr, "error: %s: \"%s\" is not a function start\n", hooksPath.c_str(), line.c_str());
                return 1;
            }
            recomp.hooked.insert(addr);
        }
        printf("hooks: %zu functions from %s\n", recomp.hooked.size(), hooksPath.c_str());
    }

    // ---- function bodies ----
    size_t fileIndex = 0;
    std::string body;
    size_t bodyInstructions = 0;
    auto flush = [&]() {
        if (body.empty())
            return;
        WriteIfChanged(outDir / std::format("ppc_recomp.{}.cpp", fileIndex++), "#include \"ppc_recomp_shared.h\"\n\n" + body);
        body.clear();
        bodyInstructions = 0;
    };
    for (size_t i = 0; i < image.functions.size(); i++)
    {
        const Function& fn = image.functions[i];
        recomp.RecompileFunction(fn, body);
        bodyInstructions += fn.size / 4;
        if (bodyInstructions >= InstructionsPerFile)
            flush();
        if (i % 10000 == 0)
            printf("  %zu / %zu functions\n", i, image.functions.size());
    }
    flush();
    // Remove stale chunks from a previous run that produced more files.
    for (size_t i = fileIndex; fs::exists(outDir / std::format("ppc_recomp.{}.cpp", i)); i++)
        fs::remove(outDir / std::format("ppc_recomp.{}.cpp", i));

    // ---- config, declarations, mappings, import stubs ----
    std::string out;
    out += "#pragma once\n";
    out += std::format("#define PPC_CODE_BASE 0x{:08X}u\n", image.CodeBase());
    out += std::format("#define PPC_CODE_END 0x{:08X}u\n", image.CodeEnd());
    out += std::format("#define PPC_ENTRY_POINT 0x{:08X}u\n", image.entry);
    out += std::format("#define PPC_FUNCTION_COUNT {}\n", image.functions.size());
    out += std::format("#define PPC_RECOMP_FILE_COUNT {}\n", fileIndex);
    WriteIfChanged(outDir / "ppc_config.h", out);

    out = "#pragma once\n#include \"ppc_config.h\"\n#include \"ppc_context.h\"\n\n";
    for (const Function& fn : image.functions)
    {
        out += std::format("PPC_FUNC({});\n", Recompiler::FunctionName(fn.addr));
        if (recomp.hooked.count(fn.addr))
            out += std::format("PPC_FUNC(orig_{0});\nextern PPCFunc* ppc_hook_{0};\n", Recompiler::FunctionName(fn.addr));
    }
    for (size_t i = 0; i < image.imports.size(); i++)
        if (!image.imports[i].data)
            out += std::format("PPC_FUNC({});\n", recomp.ImportName(i));
    WriteIfChanged(outDir / "ppc_recomp_shared.h", out);

    out = "#include \"ppc_recomp_shared.h\"\n\nconst PPCFuncMapping PPCFuncMappings[] = {\n";
    for (const Function& fn : image.functions)
        out += std::format("\t{{ 0x{:08X}u, {} }},\n", fn.addr, Recompiler::FunctionName(fn.addr));
    out += "\t{ 0, nullptr }\n};\n\nconst PPCHookSlot PPCHookSlots[] = {\n";
    for (const Function& fn : image.functions)
        if (recomp.hooked.count(fn.addr))
            out += std::format("\t{{ 0x{0:08X}u, &ppc_hook_{1}, orig_{1} }},\n", fn.addr, Recompiler::FunctionName(fn.addr));
    out += "\t{ 0, nullptr, nullptr }\n};\n";
    WriteIfChanged(outDir / "ppc_func_mapping.cpp", out);

    // Each import stub hands off to the runtime's dispatcher, which logs the call and runs the host
    // implementation if there is one. The index is the stub's position in PPCImportMappings.
    out = "// Import stubs: forward to the runtime import dispatcher.\n#include \"ppc_recomp_shared.h\"\n\n";
    for (size_t i = 0, n = 0; i < image.imports.size(); i++)
    {
        const Import& imp = image.imports[i];
        if (!imp.data)
            out += std::format("PPC_FUNC({}) {{ ppc_import(ctx, base, {}); }}\n", recomp.ImportName(i), n++);
    }
    out += "\nconst PPCImportMapping PPCImportMappings[] = {\n";
    for (size_t i = 0; i < image.imports.size(); i++)
    {
        const Import& imp = image.imports[i];
        if (!imp.data)
            out += std::format("\t{{ 0x{:08X}u, \"{}\", \"{}\", {} }},\n", imp.addr, imp.lib, imp.name, recomp.ImportName(i));
    }
    out += "\t{ 0, nullptr, nullptr, nullptr }\n};\n\nconst PPCDataImport PPCDataImports[] = {\n";
    for (const Import& imp : image.imports)
        if (imp.data)
            out += std::format("\t{{ 0x{:08X}u, \"{}\", \"{}\" }},\n", imp.addr, imp.lib, imp.name);
    out += "\t{ 0, nullptr, nullptr }\n};\n";
    WriteIfChanged(outDir / "ppc_imports.cpp", out);

    // ---- report ----
    const RecompStats& st = recomp.stats;
    printf("recompiled %zu functions, %zu instructions into %zu files (%zu files changed)\n", st.functions,
        st.instructions, fileIndex, gFilesWritten);
    printf("import call sites: %zu, switch tables: %zu (%zu entries), fall-through tail calls: %zu, bad branches: %zu\n",
        st.importCallSites, st.switchTables, st.switchEntries, st.fallthroughTailCalls, st.badBranches);
    printf("invalid words inside functions: %zu\n", st.invalid);
    for (auto& [name, count] : st.unsupported)
        printf("unsupported instruction %s: %zu\n", name.c_str(), count);
    return 0;
}
