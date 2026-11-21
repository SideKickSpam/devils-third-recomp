#include "vfs.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace vfs
{
namespace
{
struct Mount
{
    std::string prefix;              // e.g. "/vol/content"
    std::vector<fs::path> roots;     // searched in order
    bool writable = false;
};

std::vector<Mount> gMounts;
std::string gSaveDir;  // --save-dir; empty = <game-dir>/save
std::mutex gCacheMutex;
std::unordered_map<std::string, std::string> gCaseCache;  // lowercase host path -> real host path

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Resolve rel under root, matching each component case-insensitively if needed.
std::optional<fs::path> FindCaseInsensitive(const fs::path& root, const std::string& rel)
{
    fs::path exact = root / rel;
    std::error_code ec;
    if (fs::exists(exact, ec))
        return exact;
    const std::string key = Lower(exact.string());
    {
        std::lock_guard lock(gCacheMutex);
        if (auto it = gCaseCache.find(key); it != gCaseCache.end())
            return fs::path(it->second);
    }
    fs::path cur = root;
    std::string part;
    std::stringstream ss(rel);
    for (const auto& comp : fs::path(rel))
    {
        if (comp.empty() || comp == "/")
            continue;
        fs::path next = cur / comp;
        if (!fs::exists(next, ec))
        {
            const std::string want = Lower(comp.string());
            bool found = false;
            for (const auto& e : fs::directory_iterator(cur, ec))
                if (Lower(e.path().filename().string()) == want)
                {
                    next = e.path();
                    found = true;
                    break;
                }
            if (!found)
                return std::nullopt;
        }
        cur = next;
    }
    std::lock_guard lock(gCacheMutex);
    gCaseCache[key] = cur.string();
    return cur;
}

const Mount* FindMount(const std::string& path, std::string& rel)
{
    for (const Mount& m : gMounts)
    {
        if (path == m.prefix || (path.rfind(m.prefix + "/", 0) == 0))
        {
            rel = path.size() > m.prefix.size() ? path.substr(m.prefix.size() + 1) : "";
            return &m;
        }
    }
    return nullptr;
}
} // namespace

bool Init(const std::string& gameDir, std::string& error)
{
    fs::path base, update;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(gameDir, ec))
    {
        const std::string name = e.path().filename().string();
        if (!e.is_directory() || name.size() < 16)
            continue;
        if (name.rfind("00050000", 0) == 0)
            base = e.path();
        else if (name.rfind("0005000e", 0) == 0)
            update = e.path();
    }
    if (base.empty())
    {
        error = "no base title folder (00050000*) in " + gameDir;
        return false;
    }
    auto roots = [&](const char* sub) {
        std::vector<fs::path> r;
        if (!update.empty() && fs::exists(update / sub))
            r.push_back(update / sub);
        r.push_back(base / sub);
        return r;
    };
    const fs::path save = gSaveDir.empty() ? fs::path(gameDir) / "save" : fs::path(gSaveDir);
    fs::create_directories(save, ec);
    gMounts = {
        { "/vol/content", roots("content"), false },
        { "/vol/code", roots("code"), false },
        { "/vol/meta", roots("meta"), false },
        { "/vol/save", { save }, true },
    };
    dtlog::Important("[dt-port] /vol/content -> %s%s\n", base.string().c_str(), update.empty() ? "" : " (+ update)");
    return true;
}

void SetSaveDir(const std::string& dir) { gSaveDir = dir; }

std::string Normalize(const std::string& path, const std::string& cwd)
{
    std::string full = path.empty() || path[0] != '/' ? cwd + "/" + path : path;
    std::vector<std::string> parts;
    std::string part;
    std::stringstream ss(full);
    while (std::getline(ss, part, '/'))
    {
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const auto& p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

std::optional<std::string> Resolve(const std::string& guestPath)
{
    std::string rel;
    const Mount* m = FindMount(Normalize(guestPath), rel);
    if (!m)
        return std::nullopt;
    for (const fs::path& root : m->roots)
        if (auto p = FindCaseInsensitive(root, rel))
            return p->string();
    return std::nullopt;
}

std::optional<std::string> ResolveForWrite(const std::string& guestPath)
{
    std::string rel;
    const Mount* m = FindMount(Normalize(guestPath), rel);
    if (!m || !m->writable)
        return std::nullopt;
    if (auto existing = FindCaseInsensitive(m->roots.front(), rel))
        return existing->string();
    return (m->roots.front() / rel).string();
}
} // namespace vfs
