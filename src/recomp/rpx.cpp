#include "rpx.h"

#include "decoder.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace
{
constexpr uint32_t SHT_PROGBITS = 1;
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_RELA = 4;
constexpr uint32_t SHT_RPL_IMPORTS = 0x80000002;
constexpr uint32_t SHF_ALLOC = 0x2;
constexpr uint32_t SHF_EXECINSTR = 0x4;
constexpr uint32_t SHF_RPL_ZLIB = 0x08000000;
constexpr uint8_t STT_SECTION = 3;
constexpr uint8_t STT_FUNC = 2;
constexpr uint8_t STB_GLOBAL = 1;

constexpr uint32_t R_PPC_ADDR32 = 1;
constexpr uint32_t R_PPC_ADDR16_LO = 4;
constexpr uint32_t R_PPC_ADDR16_HI = 5;
constexpr uint32_t R_PPC_ADDR16_HA = 6;
constexpr uint32_t R_PPC_REL24 = 10;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutBe16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = uint8_t(v); }
void PutBe32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
} // namespace

bool RpxImage::Load(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    file.assign(std::istreambuf_iterator<char>(in), {});
    if (!ParseElf(error))
        return false;
    CollectImports();
    CollectCode();
    CollectFunctions();
    return ApplyCodeRelocations(error);
}

bool RpxImage::ParseElf(std::string& error)
{
    const uint8_t* d = file.data();
    if (file.size() < 0x34 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 2)
    {
        error = "not an ELF32 big-endian file";
        return false;
    }
    if (Be16(d + 0x12) != 20)
    {
        error = "not a PowerPC ELF";
        return false;
    }
    entry = Be32(d + 0x18);
    const uint32_t shoff = Be32(d + 0x20);
    const uint16_t shentsize = Be16(d + 0x2E), shnum = Be16(d + 0x30), shstrndx = Be16(d + 0x32);
    for (uint32_t i = 0; i < shnum; i++)
    {
        const uint8_t* s = d + shoff + i * shentsize;
        sections.push_back({ "", i, Be32(s + 4), Be32(s + 8), Be32(s + 12), Be32(s + 16), Be32(s + 20),
            Be32(s + 24), Be32(s + 28), Be32(s + 36) });
    }
    const ElfSection& shstr = sections[shstrndx];
    for (uint32_t i = 0; i < shnum; i++)
    {
        const uint32_t nameOff = Be32(d + shoff + i * shentsize);
        sections[i].name = reinterpret_cast<const char*>(d + shstr.offset + nameOff);
        if (sections[i].flags & SHF_RPL_ZLIB)
        {
            error = "section " + sections[i].name + " is still zlib-compressed (run wiiurpxtool -d first)";
            return false;
        }
    }
    auto symtab = std::find_if(sections.begin(), sections.end(), [](auto& s) { return s.type == SHT_SYMTAB; });
    if (symtab == sections.end())
    {
        error = "no symbol table";
        return false;
    }
    const ElfSection& strtab = sections[symtab->link];
    for (uint32_t off = 0; off + 16 <= symtab->size; off += 16)
    {
        const uint8_t* s = d + symtab->offset + off;
        symbols.push_back({ reinterpret_cast<const char*>(d + strtab.offset + Be32(s)), Be32(s + 4), Be32(s + 8),
            uint8_t(s[12] & 0xF), uint8_t(s[12] >> 4), Be16(s + 14) });
    }
    return true;
}

void RpxImage::CollectImports()
{
    for (const ElfSymbol& sym : symbols)
    {
        if (sym.shndx >= sections.size() || sym.type == STT_SECTION)
            continue;
        const ElfSection& sec = sections[sym.shndx];
        if (sec.type != SHT_RPL_IMPORTS)
            continue;
        const bool data = sec.name.rfind(".dimport_", 0) == 0;
        imports.push_back({ sec.name.substr(9), sym.name, sym.value, data });  // strip ".fimport_"/".dimport_"
    }
    std::sort(imports.begin(), imports.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
    uint32_t nextData = DataImportBase;
    for (size_t i = 0; i < imports.size(); i++)
    {
        importByAddr[imports[i].addr] = i;  // keyed by the original symbol address
        if (imports[i].data)
        {
            imports[i].addr = nextData;
            nextData += DataImportStride;
        }
    }
}

void RpxImage::CollectCode()
{
    for (const ElfSection& sec : sections)
    {
        // .text and .syscall. The RPL export/import tables are also flagged executable but are not code.
        if ((sec.flags & (SHF_ALLOC | SHF_EXECINSTR)) != (SHF_ALLOC | SHF_EXECINSTR) || sec.type != SHT_PROGBITS)
            continue;
        code.push_back({ sec.name, sec.addr,
            std::vector<uint8_t>(file.begin() + sec.offset, file.begin() + sec.offset + sec.size) });
    }
    std::sort(code.begin(), code.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
}

void RpxImage::CollectFunctions()
{
    // One function per distinct address. Several symbols can alias an address (e.g. strlen/__ghs_strlen);
    // prefer a GLOBAL symbol, then the first one seen.
    std::map<uint32_t, const ElfSymbol*> best;
    for (const ElfSymbol& sym : symbols)
    {
        if (sym.type != STT_FUNC || sym.size == 0 || !InCode(sym.value))
            continue;
        auto [it, inserted] = best.emplace(sym.value, &sym);
        if (!inserted && it->second->bind != STB_GLOBAL && sym.bind == STB_GLOBAL)
            it->second = &sym;
    }
    for (auto& [addr, sym] : best)
        functions.push_back({ addr, sym->size, sym->name });
}

bool RpxImage::InCode(uint32_t addr) const
{
    for (const CodeSection& c : code)
        if (addr >= c.addr && addr - c.addr < c.bytes.size())
            return true;
    return false;
}

uint8_t* RpxImage::CodePtr(uint32_t addr)
{
    for (CodeSection& c : code)
        if (addr >= c.addr && addr - c.addr < c.bytes.size())
            return c.bytes.data() + (addr - c.addr);
    return nullptr;
}

uint32_t RpxImage::ReadCode(uint32_t addr) const
{
    for (const CodeSection& c : code)
        if (addr >= c.addr && addr - c.addr + 4 <= c.bytes.size())
            return Be32(c.bytes.data() + (addr - c.addr));
    return 0;
}

uint32_t RpxImage::CodeBase() const { return code.front().addr; }
uint32_t RpxImage::CodeEnd() const { return code.back().addr + uint32_t(code.back().bytes.size()); }

bool RpxImage::ApplyCodeRelocations(std::string& error)
{
    // The file's code is already relocated for the default load addresses, except for references to imports,
    // which point past the end of .text. Re-apply every relocation against the import's canonical address
    // (its .fimport/.dimport symbol value) and verify that internal ones are no-ops.
    std::unordered_set<uint32_t> functionStarts;
    for (const Function& f : functions)
        functionStarts.insert(f.addr);

    for (const ElfSection& rel : sections)
    {
        if (rel.type != SHT_RELA || rel.info >= sections.size())
            continue;
        const ElfSection& target = sections[rel.info];
        if (!(target.flags & SHF_EXECINSTR) || target.type != SHT_PROGBITS)
            continue;
        for (uint32_t off = 0; off + 12 <= rel.size; off += 12)
        {
            const uint8_t* r = file.data() + rel.offset + off;
            const uint32_t where = Be32(r), info = Be32(r + 4);
            const int32_t addend = int32_t(Be32(r + 8));
            const uint32_t type = info & 0xFF, symIndex = info >> 8;
            if (symIndex >= symbols.size())
            {
                error = "relocation references a missing symbol";
                return false;
            }
            const ElfSymbol& sym = symbols[symIndex];
            const bool toImport = sym.shndx < sections.size() && sections[sym.shndx].type == SHT_RPL_IMPORTS;
            uint32_t value = sym.value + uint32_t(addend);
            if (toImport)
            {
                // Data imports live in their dedicated regions; function imports keep their symbol address.
                auto imp = importByAddr.find(sym.value);
                if (imp != importByAddr.end() && imports[imp->second].data)
                    value = imports[imp->second].addr + uint32_t(addend);
            }
            uint8_t* p = CodePtr(where & ~3u);
            if (!p)
            {
                relocStats.unsupported++;
                continue;
            }
            p += where & 3;

            if (type == R_PPC_REL24)
            {
                if (toImport)
                {
                    auto it = importByAddr.find(sym.value);
                    if (it == importByAddr.end())
                    {
                        error = "REL24 to unknown import " + sym.name;
                        return false;
                    }
                    importCalls[where] = it->second;
                    relocStats.importCalls++;
                    continue;
                }
                const uint32_t insn = Be32(p);
                const uint32_t patched = (insn & ~0x03FFFFFCu) | ((value - where) & 0x03FFFFFC);
                (patched == insn ? relocStats.internalVerified : relocStats.internalPatched)++;
                PutBe32(p, patched);
                continue;
            }

            uint32_t size = 2, newValue;
            switch (type)
            {
            case R_PPC_ADDR32: size = 4; newValue = value; break;
            case R_PPC_ADDR16_LO: newValue = value & 0xFFFF; break;
            case R_PPC_ADDR16_HI: newValue = value >> 16; break;
            case R_PPC_ADDR16_HA: newValue = ((value >> 16) + ((value >> 15) & 1)) & 0xFFFF; break;
            default: relocStats.unsupported++; continue;
            }
            const uint32_t old = size == 4 ? Be32(p) : Be16(p);
            if (size == 4)
                PutBe32(p, newValue);
            else
                PutBe16(p, uint16_t(newValue));
            if (toImport)
                relocStats.importPatched++;
            else
                (old == newValue ? relocStats.internalVerified : relocStats.internalPatched)++;

            if (type == R_PPC_ADDR16_LO && !toImport && InCode(value) && !functionStarts.count(value))
                codeRefsInsideFunctions.insert(value);
        }
    }
    return true;
}

size_t RpxImage::AddSecondaryEntryPoints()
{
    // Original symbol ranges, used to find the range that contains a target.
    std::vector<Function> ranges = functions;
    std::unordered_set<uint32_t> starts;
    for (const Function& f : functions)
        starts.insert(f.addr);

    auto rangeEnd = [&](uint32_t target, std::string& name) -> uint32_t {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), target, [](uint32_t a, const Function& f) { return a < f.addr; });
        if (it != ranges.begin())
        {
            const Function& prev = *std::prev(it);
            if (target < prev.addr + prev.size)
            {
                name = std::format("{}+0x{:X}", prev.name, target - prev.addr);
                return prev.addr + prev.size;
            }
        }
        name = "(unnamed code)";
        return it != ranges.end() ? it->addr : CodeEnd();  // gap: up to the next symbol
    };

    size_t added = 0;
    std::vector<Function> work = functions;
    while (!work.empty())
    {
        std::vector<Function> found;
        for (const Function& f : work)
        {
            for (uint32_t a = f.addr; a < f.addr + f.size; a += 4)
            {
                const uint32_t w = ReadCode(a);
                const Op op = Decode(w);
                uint32_t target;
                if (op == Op::B && !insn::AA(w) && !importCalls.count(a))
                    target = a + insn::LI(w);
                else if (op == Op::BC && !insn::AA(w))
                    target = a + insn::BD(w);
                else
                    continue;
                const bool inside = target >= f.addr && target < f.addr + f.size;
                if (inside || starts.count(target) || !InCode(target))
                    continue;
                std::string name;
                const uint32_t end = rangeEnd(target, name);
                starts.insert(target);
                found.push_back({ target, end - target, name });
            }
        }
        added += found.size();
        functions.insert(functions.end(), found.begin(), found.end());
        work = std::move(found);
    }
    std::sort(functions.begin(), functions.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
    return added;
}
