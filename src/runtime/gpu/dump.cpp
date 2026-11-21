#include "dump.h"

#include "../log.h"
#include "gx2_types.h"
#include "surface.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace gpu::dump
{
void CheckTextureUploads(const Surface& s);

namespace
{
std::string gDir;
std::mutex gMutex;
std::set<std::pair<uint32_t, uint64_t>> gSeen;  // (address, content hash)

uint64_t Hash(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// True the first time this (address, contents) pair is seen.
bool First(uint32_t addr, uint32_t size)
{
    std::lock_guard lock(gMutex);
    return gSeen.insert({ addr, Hash(guest::Ptr(addr), size) }).second;
}

void Save(const std::string& name, uint32_t addr, uint32_t size)
{
    FILE* f = fopen((gDir + "/" + name).c_str(), "wb");
    if (!f)
        return;
    fwrite(guest::Ptr(addr), 1, size, f);
    fclose(f);
}

std::string Hex(const uint32_t* w, size_t n)
{
    std::string s;
    char buf[12];
    for (size_t i = 0; i < n; i++)
    {
        snprintf(buf, sizeof(buf), "%s%08X", i ? " " : "", w[i]);
        s += buf;
    }
    return s;
}

void LogSurface(const char* what, uint32_t addr, const Surface& s)
{
    dtlog::Write("[gx2dump] %s 0x%08X: dim %u %ux%ux%u mips %u format 0x%X aa %u use 0x%X tile %u swizzle 0x%X "
                 "pitch %u image 0x%08X size 0x%X mipmaps 0x%08X size 0x%X align 0x%X\n",
        what, addr, s.dim, s.width, s.height, s.depth, s.mipLevels, s.format, s.aa, s.use, s.tileMode, s.swizzle, s.pitch,
        s.image, s.imageSize, s.mipmaps, s.mipmapSize, s.alignment);
}
} // namespace

void Enable(const std::string& dir)
{
    gDir = dir;
    std::filesystem::create_directories(dir);
}

bool Enabled() { return !gDir.empty(); }

void VertexShader(uint32_t addr)
{
    if (!Enabled() || !First(addr, 0x134))
        return;
    const gpu::VertexShader s = ReadVertexShader(addr);
    const uint64_t h = Hash(guest::Ptr(s.program), s.size);
    char name[64];
    snprintf(name, sizeof(name), "vs_%016llx", (unsigned long long)h);
    Save(std::string(name) + ".bin", s.program, s.size);
    Save(std::string(name) + ".gx2", addr, 0x134);
    dtlog::Write("[gx2dump] vertex shader 0x%08X -> %s: size %u mode %u gprs %u outputs %u semantics %u uniformVars %u "
                 "samplers %u attribVars %u\n  regs %s\n",
        addr, name, s.size, s.mode, s.NumGprs(), s.NumOutputs(), s.SemanticCount(), s.uniformVarCount, s.samplerVarCount,
        s.attribVarCount, Hex(s.regs.data(), s.regs.size()).c_str());
}

void PixelShader(uint32_t addr)
{
    if (!Enabled() || !First(addr, 0xE8))
        return;
    const gpu::PixelShader s = ReadPixelShader(addr);
    const uint64_t h = Hash(guest::Ptr(s.program), s.size);
    char name[64];
    snprintf(name, sizeof(name), "ps_%016llx", (unsigned long long)h);
    Save(std::string(name) + ".bin", s.program, s.size);
    Save(std::string(name) + ".gx2", addr, 0xE8);
    dtlog::Write("[gx2dump] pixel shader 0x%08X -> %s: size %u mode %u gprs %u inputs %u mask 0x%X uniformVars %u "
                 "samplers %u\n  regs %s\n",
        addr, name, s.size, s.mode, s.NumGprs(), s.NumInputs(), s.ShaderMask(), s.uniformVarCount, s.samplerVarCount,
        Hex(s.regs.data(), s.regs.size()).c_str());
}

void FetchShaderInit(uint32_t fs, uint32_t attribCount, uint32_t attribs)
{
    if (!Enabled() || !First(attribs, attribCount * 0x24))
        return;
    dtlog::Write("[gx2dump] fetch shader 0x%08X: %u attribs at 0x%08X\n", fs, attribCount, attribs);
    for (uint32_t i = 0; i < attribCount; i++)
    {
        uint32_t w[9];
        for (int k = 0; k < 9; k++)
            w[k] = guest::Read32(attribs + i * 0x24 + k * 4);
        dtlog::Write("  attrib %u (raw, 9 words at stride 0x24): %s\n", i, Hex(w, 9).c_str());
    }
}

void Texture(uint32_t addr, uint32_t unit)
{
    if (!Enabled() || !First(addr, 0x9C))
        return;
    const gpu::Texture t = ReadTexture(addr);
    CheckTextureUploads(t.surface);
    char what[32];
    snprintf(what, sizeof(what), "texture unit %u", unit);
    LogSurface(what, addr, t.surface);
    dtlog::Write("  view mips %u+%u slices %u+%u compMap 0x%08X regs %s\n", t.viewFirstMip, t.viewNumMips,
        t.viewFirstSlice, t.viewNumSlices, t.compMap, Hex(t.regs.data(), 5).c_str());
}

void SurfaceLayout(uint32_t addr)
{
    if (!Enabled())
        return;
    const gpu::Surface s = ReadSurface(addr);
    dtlog::Write("[gx2dump] layout 0x%08X: dim %u %ux%ux%u mips %u format 0x%X use 0x%X tile %u swizzle 0x%X "
                 "imageSize 0x%X mipmapSize 0x%X align 0x%X pitch %u mipOffsets %s\n",
        addr, s.dim, s.width, s.height, s.depth, s.mipLevels, s.format, s.use, s.tileMode, s.swizzle, s.imageSize,
        s.mipmapSize, s.alignment, s.pitch, Hex(s.mipLevelOffset.data(), std::min(13u, s.mipLevels)).c_str());
}

void Sampler(uint32_t addr, uint32_t unit)
{
    if (!Enabled() || !First(addr, 12))
        return;
    const gpu::Sampler s = ReadSampler(addr);
    dtlog::Write("[gx2dump] sampler unit %u 0x%08X: %s\n", unit, addr, Hex(s.regs.data(), 3).c_str());
}

void ColorBuffer(uint32_t addr, uint32_t target)
{
    if (!Enabled() || !First(addr, 0x9C))
        return;
    const gpu::ColorBuffer c = ReadColorBuffer(addr);
    char what[32];
    snprintf(what, sizeof(what), "color buffer %u", target);
    LogSurface(what, addr, c.surface);
}

void AttribBuffer(uint32_t index, uint32_t size, uint32_t stride, uint32_t buffer)
{
    if (!Enabled() || !First(buffer, std::min<uint32_t>(size, 64)))
        return;
    uint32_t w[8] = {};
    for (uint32_t k = 0; k < 8 && k * 4 < size; k++)
        w[k] = guest::Read32(buffer + k * 4);
    dtlog::Write("[gx2dump] attrib buffer %u: 0x%08X size %u stride %u first words %s\n", index, buffer, size, stride,
        Hex(w, 8).c_str());
}

void Uniforms(bool pixel, uint32_t offset, uint32_t count, uint32_t data)
{
    if (!Enabled() || !First(data, count * 4))
        return;
    std::string vals;
    char buf[32];
    for (uint32_t i = 0; i < count && i < 32; i++)
    {
        snprintf(buf, sizeof(buf), "%s%g", i ? " " : "", double(std::bit_cast<float>(guest::Read32(data + i * 4))));
        vals += buf;
    }
    dtlog::Write("[gx2dump] %s uniforms at reg %u, %u floats: %s\n", pixel ? "pixel" : "vertex", offset, count, vals.c_str());
}

void Draw(uint32_t mode, uint32_t count, uint32_t indexType, uint32_t indices)
{
    if (!Enabled())
        return;
    dtlog::Write("[gx2dump] draw mode 0x%X count %u indexType 0x%X indices 0x%08X\n", mode, count, indexType, indices);
}

namespace
{
std::unordered_map<uint32_t, uint32_t> gCopies;   // destination -> size of the latest large guest write there
std::unordered_map<uint32_t, uint32_t> gRuns;     // start -> size of back-to-back writes (chunked decompression)
std::unordered_map<uint32_t, uint32_t> gRunEnds;  // end of a run -> its start
std::unordered_map<uint32_t, uint32_t> gUploaded; // address -> size copied there before an invalidate
std::set<std::pair<uint32_t, uint32_t>> gChecked; // (image, mipmaps) already compared
// (format, width, height, tile mode, level, copied, computed) -> first seen
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>> gReported;
uint32_t gMatches = 0, gMismatches = 0;
} // namespace

void Copy(uint32_t dst, uint32_t size)
{
    if (!Enabled() || size < 64)
        return;
    std::lock_guard lock(gMutex);
    gCopies[dst] = size;
    uint32_t start = dst;
    if (auto run = gRunEnds.find(dst); run != gRunEnds.end())
    {
        start = run->second;
        gRunEnds.erase(run);
        gRuns[start] += size;
    }
    else
        gRuns[dst] = size;
    gRunEnds[dst + size] = start;
}

void Invalidate(uint32_t addr, uint32_t size)
{
    (void)size;
    if (!Enabled())
        return;
    std::lock_guard lock(gMutex);
    auto copy = gCopies.find(addr);
    if (copy == gCopies.end())
        return;
    gUploaded[addr] = copy->second;
    gCopies.erase(copy);
}

void CheckTextureUploads(const Surface& s)
{
    std::lock_guard lock(gMutex);
    // Linear surfaces are often filled row by row (movie planes), so a single write says nothing.
    if (!s.image || s.tileMode == surface::TileLinearAligned || s.tileMode == surface::TileLinearSpecial ||
        !gChecked.insert({ s.image, s.mipmaps }).second)
        return;
    for (uint32_t level = 0; level < std::min(s.mipLevels, 14u); level++)
    {
        // Written then invalidated (a locked mip), or just written (data loaded straight into texture memory).
        const uint32_t addr = surface::LevelAddress(s, level);
        uint32_t copied = 0;
        if (auto it = gUploaded.find(addr); it != gUploaded.end())
            copied = it->second;
        else if (auto c = gCopies.find(addr); c != gCopies.end())
            copied = c->second;
        else if (auto r = gRuns.find(addr); r != gRuns.end())
            copied = r->second;
        else
            continue;
        const surface::Level l = surface::ComputeLevel(s, level);
        const bool match = l.size == copied;
        (match ? gMatches : gMismatches)++;
        if (gReported.insert({ s.format, s.width, s.height, s.tileMode, level, copied, l.size }).second || !match)
            dtlog::Important("[gx2dump] mip upload: format 0x%X %ux%u tile %u level %u (mode %u): copied 0x%X, layout "
                             "0x%llX %s  [totals: %u match, %u differ]\n",
                s.format, s.width, s.height, s.tileMode, level, l.tileMode, copied, (unsigned long long)l.size,
                match ? "ok" : "DIFFERENT", gMatches, gMismatches);
    }
}
} // namespace gpu::dump
