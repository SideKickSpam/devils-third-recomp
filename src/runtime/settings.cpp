#include "settings.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace settings
{
namespace
{
std::mutex gMutex;
std::map<std::string, std::string> gValues, gOverrides;
std::string gPath;
bool gLoaded = false;
std::atomic<uint64_t> gGeneration{ 0 };

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string DefaultPath()
{
    char* dir = SDL_GetPrefPath(nullptr, "dt-port");
    std::string path = dir ? std::string(dir) + "settings.ini" : "settings.ini";
    SDL_free(dir);
    return path;
}

// Under the lock.
const std::string* Find(const std::string& key)
{
    if (auto it = gOverrides.find(key); it != gOverrides.end())
        return &it->second;
    if (auto it = gValues.find(key); it != gValues.end())
        return &it->second;
    return nullptr;
}
} // namespace

void Load(const std::string& path)
{
    std::lock_guard lock(gMutex);
    gPath = path.empty() ? DefaultPath() : path;
    gValues.clear();
    gLoaded = true;
    std::ifstream in(gPath);
    std::string line, section;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        gValues[section.empty() ? key : section + "." + key] = Trim(line.substr(eq + 1));
    }
    gGeneration++;
    dtlog::Important("[settings] %s (%zu values%s)\n", gPath.c_str(), gValues.size(), in ? "" : ", new file");
}

bool Loaded() { return gLoaded; }

const std::string& Path() { return gPath; }

std::string Directory()
{
    const size_t slash = gPath.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : gPath.substr(0, slash + 1);
}

void Save()
{
    std::lock_guard lock(gMutex);
    if (!gLoaded)
        return;
    // Grouped by section (the part before the first dot), in key order.
    std::map<std::string, std::map<std::string, std::string>> sections;
    for (auto& [key, value] : gValues)
    {
        const size_t dot = key.find('.');
        sections[dot == std::string::npos ? "" : key.substr(0, dot)][dot == std::string::npos ? key : key.substr(dot + 1)] = value;
    }
    std::ostringstream out;
    out << "# dt-port settings. Edit while the game is closed, or use the in-game menu.\n";
    for (auto& [section, values] : sections)
    {
        out << "\n";
        if (!section.empty())
            out << "[" << section << "]\n";
        for (auto& [key, value] : values)
            out << key << " = " << value << "\n";
    }
    // Write a temporary file and rename it, so a crash never leaves half a file.
    const std::string tmp = gPath + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << out.str();
        if (!f)
        {
            dtlog::Important("[settings] cannot write %s\n", tmp.c_str());
            return;
        }
    }
    std::remove(gPath.c_str());
    if (std::rename(tmp.c_str(), gPath.c_str()) != 0)
        dtlog::Important("[settings] cannot replace %s\n", gPath.c_str());
}

std::string Get(const std::string& key, const std::string& fallback)
{
    std::lock_guard lock(gMutex);
    const std::string* v = Find(key);
    return v ? *v : fallback;
}

int GetInt(const std::string& key, int fallback)
{
    const std::string v = Get(key, "");
    try
    {
        return v.empty() ? fallback : std::stoi(v);
    }
    catch (...)
    {
        return fallback;
    }
}

float GetFloat(const std::string& key, float fallback)
{
    const std::string v = Get(key, "");
    try
    {
        return v.empty() ? fallback : std::stof(v);
    }
    catch (...)
    {
        return fallback;
    }
}

bool GetBool(const std::string& key, bool fallback)
{
    const std::string v = Get(key, "");
    if (v == "1" || v == "true" || v == "on" || v == "yes")
        return true;
    if (v == "0" || v == "false" || v == "off" || v == "no")
        return false;
    return fallback;
}

void Set(const std::string& key, const std::string& value)
{
    std::lock_guard lock(gMutex);
    gOverrides.erase(key);
    if (auto it = gValues.find(key); it != gValues.end() && it->second == value)
        return;
    gValues[key] = value;
    gGeneration++;
}

void SetInt(const std::string& key, int value) { Set(key, std::to_string(value)); }

void SetFloat(const std::string& key, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", value);
    Set(key, buf);
}

void SetBool(const std::string& key, bool value) { Set(key, value ? "true" : "false"); }

void Override(const std::string& key, const std::string& value)
{
    std::lock_guard lock(gMutex);
    gOverrides[key] = value;
    gGeneration++;
}

uint64_t Generation() { return gGeneration; }
} // namespace settings
