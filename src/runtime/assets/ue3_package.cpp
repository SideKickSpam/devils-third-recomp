#include "ue3_package.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace assets
{
namespace
{
constexpr uint32_t Tag = 0x9E2A83C1;

struct Reader
{
    const std::vector<uint8_t>& d;
    size_t o;
    bool ok = true;
    uint32_t U32()
    {
        if (o + 4 > d.size())
            return ok = false, 0;
        const uint32_t v = uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
        o += 4;
        return v;
    }
    int32_t I32() { return int32_t(U32()); }
    void Skip(size_t n) { o += n; }
    std::string FString()
    {
        const int32_t n = I32();
        std::string s;
        if (n > 0 && o + size_t(n) <= d.size())
            s.assign(reinterpret_cast<const char*>(&d[o]), size_t(n) - 1), o += size_t(n);
        else if (n < 0 && o + size_t(-n) * 2 <= d.size())
        {
            for (int32_t i = 0; i < -n - 1; i++)
                s += char(d[o + size_t(i) * 2 + 1]);  // UTF-16 BE: keep the low byte (names are ASCII)
            o += size_t(-n) * 2;
        }
        return s;
    }
};

// One compressed chunk at r: its zlib blocks, inflated and appended to out.
bool Chunk(Reader& r, std::vector<uint8_t>& out)
{
    if (r.U32() != Tag)
        return false;
    const int32_t block = r.I32();
    r.I32();  // compressed size
    const int32_t total = r.I32();
    if (block <= 0 || total < 0)
        return false;
    std::vector<std::pair<int32_t, int32_t>> blocks;
    for (int32_t i = 0; i < (total + block - 1) / block; i++)
    {
        const int32_t c = r.I32(), u = r.I32();
        blocks.push_back({ c, u });
    }
    for (auto [c, u] : blocks)
    {
        if (!r.ok || r.o + size_t(c) > r.d.size())
            return false;
        const size_t at = out.size();
        out.resize(at + size_t(u));
        uLongf len = uLongf(u);
        if (uncompress(out.data() + at, &len, &r.d[r.o], uLong(c)) != Z_OK || len != uLongf(u))
            return false;
        r.Skip(size_t(c));
    }
    return true;
}
} // namespace

bool Package::Load(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return error = "cannot open " + path, false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), {});
    if (std::filesystem::exists(path + ".uncompressed_size"))
    {
        // Fully compressed: the whole file is a run of chunks.
        Reader r{ raw, 0 };
        while (r.o + 4 <= raw.size() && r.ok)
        {
            Reader peek{ raw, r.o };
            if (peek.U32() != Tag)
                break;
            if (!Chunk(r, data))
                return error = path + ": bad compressed chunk", false;
        }
    }
    else
        data = std::move(raw);

    Reader r{ data, 0 };
    if (r.U32() != Tag)
        return error = path + ": not an Unreal package", false;
    const int32_t version = r.I32() & 0xFFFF;
    r.I32();       // header size
    r.FString();   // folder
    r.U32();       // package flags
    const int32_t nc = r.I32(), no = r.I32(), ec = r.I32(), eo = r.I32(), ic = r.I32(), io = r.I32();
    r.I32();  // depends offset
    if (version >= 623)
        r.Skip(12);
    if (version >= 584)
        r.Skip(4);
    r.Skip(16);  // guid
    const int32_t generations = r.I32();
    r.Skip(size_t(std::max(0, generations)) * 12);
    r.Skip(8);  // engine and cooker versions
    const uint32_t compression = r.U32();
    const int32_t chunkCount = r.I32();
    if (chunkCount > 0)
    {
        if (compression != 1)
            return error = path + ": unsupported compression", false;
        std::vector<std::array<int32_t, 4>> chunks;
        chunks.resize(size_t(chunkCount));
        for (auto& c : chunks)
            c = { r.I32(), r.I32(), r.I32(), r.I32() };
        std::vector<uint8_t> out = data;
        for (auto& c : chunks)
        {
            Reader cr{ data, size_t(c[2]) };
            std::vector<uint8_t> block;
            if (!Chunk(cr, block))
                return error = path + ": bad compressed chunk", false;
            if (out.size() < size_t(c[0]) + block.size())
                out.resize(size_t(c[0]) + block.size());
            memcpy(out.data() + c[0], block.data(), block.size());
        }
        data = std::move(out);
    }
    if (!r.ok)
        return error = path + ": truncated header", false;

    Reader nr{ data, size_t(no) };
    for (int32_t i = 0; i < nc && nr.ok; i++)
        names.push_back(nr.FString()), nr.Skip(8);
    auto name = [&](Reader& rr) {
        const int32_t i = rr.I32(), n = rr.I32();
        std::string s = i >= 0 && size_t(i) < names.size() ? names[size_t(i)] : "?";
        return n ? s + "_" + std::to_string(n - 1) : s;
    };
    Reader ir{ data, size_t(io) };
    for (int32_t i = 0; i < ic && ir.ok; i++)
    {
        name(ir), name(ir);
        ir.I32();
        importNames.push_back(name(ir));
    }
    Reader er{ data, size_t(eo) };
    for (int32_t i = 0; i < ec && er.ok; i++)
    {
        Export e;
        e.cls = er.I32();
        er.I32();  // super
        e.outer = er.I32();
        e.name = name(er);
        er.I32();     // archetype
        er.Skip(8);   // object flags
        e.size = er.U32();
        e.offset = er.U32();
        er.U32();  // export flags
        const int32_t netObjects = er.I32();
        er.Skip(size_t(std::max(0, netObjects)) * 4);
        er.Skip(20);  // package guid and flags
        exports.push_back(std::move(e));
    }
    if (!er.ok)
        return error = path + ": truncated export table", false;
    return true;
}

std::string Package::ObjectName(int32_t index) const
{
    if (index > 0 && size_t(index) <= exports.size())
        return exports[size_t(index) - 1].name;
    if (index < 0 && size_t(-index) <= importNames.size())
        return importNames[size_t(-index) - 1];
    return "Class";
}

std::optional<std::vector<uint8_t>> Package::Find(const std::string& cls, const std::string& name) const
{
    for (const Export& e : exports)
        if (e.name == name && ObjectName(e.cls) == cls && size_t(e.offset) + e.size <= data.size())
            return std::vector<uint8_t>(data.begin() + e.offset, data.begin() + e.offset + e.size);
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> MovieBytes(const std::vector<uint8_t>& swfMovie)
{
    for (size_t i = 0; i + 8 <= swfMovie.size(); i++)
        if (swfMovie[i] == 'G' && swfMovie[i + 1] == 'F' && swfMovie[i + 2] == 'X')
        {
            const uint32_t length = uint32_t(swfMovie[i + 4]) | uint32_t(swfMovie[i + 5]) << 8 |
                uint32_t(swfMovie[i + 6]) << 16 | uint32_t(swfMovie[i + 7]) << 24;
            if (i + length > swfMovie.size())
                return std::nullopt;
            return std::vector<uint8_t>(swfMovie.begin() + i, swfMovie.begin() + i + length);
        }
    return std::nullopt;
}
} // namespace assets
