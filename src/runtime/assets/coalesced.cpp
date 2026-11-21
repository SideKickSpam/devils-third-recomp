#include "coalesced.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace assets::coalesced
{
namespace
{
struct Entry
{
    std::string file, section, key, value;  // file, section and key in lower case
};
std::mutex gMutex;
std::vector<Entry> gEntries;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void AppendUtf8(std::string& out, uint32_t c)
{
    if (c < 0x80)
        out += char(c);
    else if (c < 0x800)
        out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
    else
        out += char(0xE0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
}

// The file's layout: big-endian counts; strings are UE3 FStrings (a count, negative for UTF-16 little-endian, with the
// terminator). files { name, sections { name, pairs { key, value } } }
struct Reader
{
    const uint8_t* p;
    size_t size, o = 0;
    bool ok = true;
    int32_t I32()
    {
        if (o + 4 > size)
            return ok = false, 0;
        const int32_t v = int32_t(uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 | p[o + 3]);
        o += 4;
        return v;
    }
    std::string Str()
    {
        const int32_t n = I32();
        std::string s;
        if (n < 0)
        {
            const size_t count = size_t(-int64_t(n));
            if (count > (size - o) / 2)
                return ok = false, s;
            for (size_t i = 0; i < count; i++)
                if (const uint32_t c = p[o + i * 2] | p[o + i * 2 + 1] << 8)
                    AppendUtf8(s, c);
            o += count * 2;
        }
        else
        {
            if (size_t(n) > size - o)
                return ok = false, s;
            for (int32_t i = 0; i < n; i++)
                if (p[o + i])
                    AppendUtf8(s, p[o + i]);  // Latin-1
            o += size_t(n);
        }
        return s;
    }
};
} // namespace

void Offer(const uint8_t* data, size_t size)
{
    Reader r{ data, size };
    const int32_t files = r.I32();
    if (files <= 0 || files > 10000)
        return;
    std::vector<Entry> entries;
    for (int32_t f = 0; f < files && r.ok; f++)
    {
        const std::string file = Lower(r.Str());
        const int32_t sections = r.I32();
        for (int32_t s = 0; s < sections && r.ok; s++)
        {
            const std::string section = Lower(r.Str());
            const int32_t pairs = r.I32();
            for (int32_t k = 0; k < pairs && r.ok; k++)
            {
                std::string key = Lower(r.Str());
                std::string value = r.Str();
                entries.push_back({ file, section, std::move(key), std::move(value) });
            }
        }
    }
    if (!r.ok || r.o + 16 < size)  // not the coalesced file (allowing for the cipher's block padding)
        return;
    // DT_DUMP_COALESCED=<file> (testing): writes every entry as text ("file [section] key=value"). Game data: write it
    // under build/ and never commit it.
    if (const char* dump = getenv("DT_DUMP_COALESCED"))
        if (FILE* f = fopen(dump, "wb"))
        {
            for (const Entry& e : entries)
                fprintf(f, "%s [%s] %s=%s\n", e.file.c_str(), e.section.c_str(), e.key.c_str(), e.value.c_str());
            fclose(f);
        }
    std::lock_guard lock(gMutex);
    gEntries = std::move(entries);
}

bool Loaded()
{
    std::lock_guard lock(gMutex);
    return !gEntries.empty();
}

std::vector<std::string> Values(const std::string& file, const std::string& section, const std::string& key)
{
    const std::string f = Lower(file), s = Lower(section), k = Lower(key);
    std::vector<std::string> out;
    std::lock_guard lock(gMutex);
    for (const Entry& e : gEntries)
        if (e.key == k && e.section == s && e.file.size() >= f.size() && e.file.compare(e.file.size() - f.size(), f.size(), f) == 0)
            out.push_back(e.value);
    return out;
}

std::vector<std::array<std::string, 3>> Entries(const std::string& sectionContains)
{
    const std::string c = Lower(sectionContains);
    std::vector<std::array<std::string, 3>> out;
    std::lock_guard lock(gMutex);
    for (const Entry& e : gEntries)
        if (e.section.find(c) != std::string::npos)
            out.push_back({ e.section, e.key, e.value });
    return out;
}

std::string Value(const std::string& file, const std::string& section, const std::string& key, const std::string& fallback)
{
    auto v = Values(file, section, key);
    return v.empty() ? fallback : v.front();
}
} // namespace assets::coalesced
