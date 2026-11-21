#include "programs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace gpu::programs
{
namespace
{
// programs.bin: "DTPROGS1", then per program its hash (u64), the body's length (u32) and the body (Serialize).
// Appended to as programs are first seen; a record cut short by a crash ends the file.
constexpr char Magic[8] = { 'D', 'T', 'P', 'R', 'O', 'G', 'S', '1' };

std::mutex gMutex;
std::string gPath;
std::unordered_map<uint64_t, std::vector<uint8_t>> gBodies;

void Put(std::vector<uint8_t>& b, uint32_t v)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}

std::vector<uint8_t> Serialize(const shader::Request& r)
{
    std::vector<uint8_t> b;
    Put(b, uint32_t(r.stage)), Put(b, r.halfZ), Put(b, uint32_t(r.positionGpr)), Put(b, r.alphaFunc);
    Put(b, uint32_t(r.vertexInputs.size()));
    for (const shader::VertexInput& v : r.vertexInputs)
    {
        uint32_t sel;
        memcpy(&sel, v.sel.data(), 4);
        Put(b, v.location), Put(b, v.gpr), Put(b, sel), Put(b, v.type);
    }
    Put(b, uint32_t(r.pixelInputs.size()));
    for (int32_t v : r.pixelInputs)
        Put(b, uint32_t(v));
    Put(b, uint32_t(r.pixelInputDefaults.size()));
    for (uint8_t v : r.pixelInputDefaults)
        Put(b, v);
    for (uint32_t v : r.loopConsts)
        Put(b, v);
    for (shader::TexDim v : r.textureDims)
        Put(b, uint32_t(v));
    for (uint8_t v : r.textureTypes)
        Put(b, v);
    for (uint8_t v : r.outputTypes)
        Put(b, v);
    Put(b, uint32_t(r.size));
    b.insert(b.end(), r.program, r.program + r.size);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, shader::Request& r, std::vector<uint8_t>& storage)
{
    size_t pos = 0;
    bool ok = true;
    auto get = [&]() -> uint32_t {
        uint32_t v = 0;
        if (pos + 4 > b.size())
            return ok = false, 0;
        memcpy(&v, b.data() + pos, 4);
        pos += 4;
        return v;
    };
    r = {};
    r.stage = shader::Stage(get()), r.halfZ = get() != 0, r.positionGpr = int32_t(get()), r.alphaFunc = get();
    const uint32_t vertexInputs = get();
    for (uint32_t i = 0; ok && i < vertexInputs && i < 64; i++)
    {
        shader::VertexInput v;
        v.location = get(), v.gpr = get();
        const uint32_t sel = get();
        memcpy(v.sel.data(), &sel, 4);
        v.type = get();
        r.vertexInputs.push_back(v);
    }
    const uint32_t pixelInputs = get();
    for (uint32_t i = 0; ok && i < pixelInputs && i < 64; i++)
        r.pixelInputs.push_back(int32_t(get()));
    const uint32_t defaults = get();
    for (uint32_t i = 0; ok && i < defaults && i < 64; i++)
        r.pixelInputDefaults.push_back(uint8_t(get()));
    for (uint32_t& v : r.loopConsts)
        v = get();
    for (shader::TexDim& v : r.textureDims)
        v = shader::TexDim(get());
    for (uint8_t& v : r.textureTypes)
        v = uint8_t(get());
    for (uint8_t& v : r.outputTypes)
        v = uint8_t(get());
    const uint32_t size = get();
    if (!ok || pos + size > b.size())
        return false;
    storage.assign(b.begin() + long(pos), b.begin() + long(pos + size));
    r.program = storage.data(), r.size = size;
    return true;
}
} // namespace

void Open(const std::string& path)
{
    std::lock_guard lock(gMutex);
    gPath = path;
    gBodies.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 8 || memcmp(data.data(), Magic, 8) != 0)
    {
        in.close();
        std::remove(path.c_str());  // another format: start a new library
        return;
    }
    size_t pos = 8;
    while (pos + 12 <= data.size())
    {
        uint64_t hash;
        uint32_t length;
        memcpy(&hash, data.data() + pos, 8);
        memcpy(&length, data.data() + pos + 8, 4);
        if (pos + 12 + length > data.size())
            break;
        gBodies[hash].assign(data.begin() + long(pos + 12), data.begin() + long(pos + 12 + length));
        pos += 12 + length;
    }
}

void Remember(uint64_t hash, const shader::Request& req)
{
    if (req.debugGpr >= 0 || !req.program || !req.size)
        return;  // DT_DEBUG_PS probes are not what the game runs
    std::lock_guard lock(gMutex);
    if (gPath.empty() || gBodies.count(hash))
        return;
    std::vector<uint8_t> body = Serialize(req);
    FILE* f = fopen(gPath.c_str(), "ab");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);  // in append mode the position can read 0 until the first write (Windows' CRT)
    if (ftell(f) == 0)
        fwrite(Magic, 1, sizeof(Magic), f);
    const uint32_t length = uint32_t(body.size());
    fwrite(&hash, 8, 1, f);
    fwrite(&length, 4, 1, f);
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    gBodies.emplace(hash, std::move(body));
}

bool Find(uint64_t hash, shader::Request& req, std::vector<uint8_t>& storage)
{
    std::lock_guard lock(gMutex);
    auto it = gBodies.find(hash);
    return it != gBodies.end() && Deserialize(it->second, req, storage);
}

std::vector<uint64_t> Hashes()
{
    std::lock_guard lock(gMutex);
    std::vector<uint64_t> out;
    for (auto& [hash, body] : gBodies)
        out.push_back(hash);
    return out;
}

size_t Count()
{
    std::lock_guard lock(gMutex);
    return gBodies.size();
}
} // namespace gpu::programs
