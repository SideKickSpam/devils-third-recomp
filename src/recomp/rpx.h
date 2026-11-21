// Loader for decompressed Wii U RPX files (ELF32 big-endian PowerPC, Cafe OS).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ElfSection
{
    std::string name;
    uint32_t index, type, flags, addr, offset, size, link, info, entsize;
};

struct ElfSymbol
{
    std::string name;
    uint32_t value, size;
    uint8_t type, bind;
    uint16_t shndx;
};

struct Import
{
    std::string lib;
    std::string name;
    uint32_t addr;     // functions: the .fimport symbol address; data: a dedicated region (see DataImportBase)
    bool data;
};

// Data imports (e.g. coreinit's _iob FILE array) are given their own 64 KB region each, because their
// .dimport slots are only 8 bytes apart and the game indexes into some of them.
constexpr uint32_t DataImportBase = 0xC1000000;
constexpr uint32_t DataImportStride = 0x10000;

struct Function
{
    uint32_t addr;
    uint32_t size;
    std::string name;  // canonical symbol name (raw, mangled)
};

struct CodeSection
{
    std::string name;
    uint32_t addr;
    std::vector<uint8_t> bytes;  // relocations already applied (see ApplyCodeRelocations)
};

struct RelocStats
{
    size_t internalVerified = 0;      // internal relocations whose result already matched the file
    size_t internalPatched = 0;       // internal relocations that changed the file contents
    size_t importPatched = 0;         // ADDR16/ADDR32 relocations against imports, patched to the import address
    size_t importCalls = 0;           // REL24 relocations against imports (recorded, not patchable)
    size_t unsupported = 0;           // relocation types we do not handle
};

class RpxImage
{
public:
    bool Load(const std::string& path, std::string& error);

    std::vector<uint8_t> file;
    std::vector<ElfSection> sections;
    std::vector<ElfSymbol> symbols;
    uint32_t entry = 0;

    std::vector<CodeSection> code;
    std::vector<Function> functions;                    // sorted by address
    std::vector<Import> imports;                        // sorted by address
    std::unordered_map<uint32_t, size_t> importByAddr;  // address -> index into imports
    std::unordered_map<uint32_t, size_t> importCalls;   // branch instruction address -> import index
    std::unordered_set<uint32_t> codeRefsInsideFunctions;  // ADDR16_LO targets in .text that are not function starts
    RelocStats relocStats;

    // Branches into the middle of a function (GHS multi-entry save/restore helpers) or into unnamed code
    // become extra functions running from the target to the end of the containing range. Repeats until no
    // new targets appear. Returns the number of entries added.
    size_t AddSecondaryEntryPoints();

    bool InCode(uint32_t addr) const;
    uint32_t ReadCode(uint32_t addr) const;  // big-endian word at addr, host order
    uint32_t CodeBase() const;
    uint32_t CodeEnd() const;

private:
    bool ParseElf(std::string& error);
    void CollectImports();
    void CollectCode();
    void CollectFunctions();
    bool ApplyCodeRelocations(std::string& error);
    uint8_t* CodePtr(uint32_t addr);
};
