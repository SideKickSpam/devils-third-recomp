#include "dump.h"

#include <zarchive/zarchivereader.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace setup
{
namespace
{
std::string Upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return s;
}

std::string Join(const std::string& a, const std::string& b) { return a.empty() ? b : b.empty() ? a : a + "/" + b; }

std::string Utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- folders ----
class FolderSource : public Source
{
public:
    explicit FolderSource(fs::path root) : root_(std::move(root)) {}

    std::vector<FileEntry> List(const std::string& dir) override
    {
        std::vector<FileEntry> out;
        const fs::path base = HostPath(dir);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec))
                out.push_back({ Utf8(it->path().lexically_relative(base).generic_u8string()), it->file_size(ec) });
        return out;
    }

    std::vector<std::string> Dirs(const std::string& dir) override
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(HostPath(dir), ec))
            if (e.is_directory(ec))
                out.push_back(Utf8(e.path().filename()));
        return out;
    }

    bool Exists(const std::string& path) override
    {
        std::error_code ec;
        return fs::exists(HostPath(path), ec);
    }

    bool ReadAll(const std::string& path, std::vector<uint8_t>& out) override
    {
        std::ifstream in(HostPath(path), std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }

    bool Read(const std::string& path, uint64_t offset, uint64_t size, void* buffer) override
    {
        std::ifstream in(HostPath(path), std::ios::binary);
        in.seekg(std::streamoff(offset));
        in.read(static_cast<char*>(buffer), std::streamsize(size));
        return in.gcount() == std::streamsize(size);
    }

    fs::path HostPath(const std::string& path) override { return path.empty() ? root_ : root_ / fs::u8path(path); }

private:
    fs::path root_;
};

// ---- .wua archives (ZArchive) ----
class WuaSource : public Source
{
public:
    explicit WuaSource(ZArchiveReader* reader) : z_(reader) {}
    ~WuaSource() override { delete z_; }

    std::vector<FileEntry> List(const std::string& dir) override
    {
        std::vector<FileEntry> out;
        Walk(dir, "", out);
        return out;
    }

    std::vector<std::string> Dirs(const std::string& dir) override
    {
        std::vector<std::string> out;
        const ZArchiveNodeHandle h = z_->LookUp(dir, false, true);
        if (h == ZARCHIVE_INVALID_NODE)
            return out;
        for (uint32_t i = 0, n = z_->GetDirEntryCount(h); i < n; i++)
        {
            ZArchiveReader::DirEntry e;
            if (z_->GetDirEntry(h, i, e) && e.isDirectory)
                out.emplace_back(e.name);
        }
        return out;
    }

    bool Exists(const std::string& path) override { return z_->LookUp(path) != ZARCHIVE_INVALID_NODE; }

    bool ReadAll(const std::string& path, std::vector<uint8_t>& out) override
    {
        const ZArchiveNodeHandle h = z_->LookUp(path, true, false);
        if (h == ZARCHIVE_INVALID_NODE)
            return false;
        out.resize(size_t(z_->GetFileSize(h)));
        return z_->ReadFromFile(h, 0, out.size(), out.data()) == out.size();
    }

    bool Read(const std::string& path, uint64_t offset, uint64_t size, void* buffer) override
    {
        if (path != lastPath_)
        {
            lastPath_ = path;
            last_ = z_->LookUp(path, true, false);
        }
        return last_ != ZARCHIVE_INVALID_NODE && z_->ReadFromFile(last_, offset, size, buffer) == size;
    }

    fs::path HostPath(const std::string&) override { return {}; }

private:
    void Walk(const std::string& dir, const std::string& rel, std::vector<FileEntry>& out)
    {
        const ZArchiveNodeHandle h = z_->LookUp(Join(dir, rel), false, true);
        if (h == ZARCHIVE_INVALID_NODE)
            return;
        for (uint32_t i = 0, n = z_->GetDirEntryCount(h); i < n; i++)
        {
            ZArchiveReader::DirEntry e;
            if (!z_->GetDirEntry(h, i, e))
                continue;
            const std::string child = Join(rel, std::string(e.name));
            if (e.isDirectory)
                Walk(dir, child, out);
            else if (e.isFile)
                out.push_back({ child, z_->GetFileSize(z_->LookUp(Join(dir, child), true, false)) });
        }
    }

    ZArchiveReader* z_;
    std::string lastPath_;
    ZArchiveNodeHandle last_ = ZARCHIVE_INVALID_NODE;
};

// The text of <tag ...>value</tag> in a Wii U metadata file.
std::string XmlValue(const std::vector<uint8_t>& xml, const std::string& tag)
{
    const std::string text(xml.begin(), xml.end());
    const size_t open = text.find("<" + tag);
    if (open == std::string::npos)
        return {};
    const size_t start = text.find('>', open);
    const size_t end = start == std::string::npos ? std::string::npos : text.find('<', start);
    if (end == std::string::npos)
        return {};
    std::string v = text.substr(start + 1, end - start - 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r'))
        v.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\n'))
        v.erase(v.begin());
    return v;
}

// Reads a title's identity from code/app.xml and meta/meta.xml.
bool ReadTitle(Source& src, const std::string& dir, Title& t)
{
    std::vector<uint8_t> app, meta;
    if (!src.ReadAll(Join(dir, "code/app.xml"), app))
        return false;
    src.ReadAll(Join(dir, "meta/meta.xml"), meta);
    t.dir = dir;
    t.titleId = Upper(XmlValue(app, "title_id"));
    // app.xml gives the version in hex, meta.xml in decimal.
    const std::string v = XmlValue(app, "title_version");
    t.version = v.empty() ? 0 : int(strtol(v.c_str(), nullptr, 16));
    t.region = XmlValue(meta, "region");
    t.productCode = XmlValue(meta, "product_code");
    return !t.titleId.empty();
}

// Titles at the chosen place, one level down or two (a folder holding title folders, or Cemu's
// mlc01/usr/title/00050000/10177600 layout).
void FindTitles(Source& src, const std::string& dir, int depth, std::vector<Title>& out)
{
    if (src.Exists(Join(dir, "code/app.xml")))
    {
        Title t;
        if (ReadTitle(src, dir, t))
            out.push_back(std::move(t));
        return;
    }
    if (depth == 0)
        return;
    for (const std::string& d : src.Dirs(dir))
        FindTitles(src, Join(dir, d), depth - 1, out);
}

void ListFiles(Source& src, Title& t)
{
    for (const char* sub : { "code", "content", "meta" })
        for (FileEntry e : src.List(Join(t.dir, sub)))
        {
            e.path = std::string(sub) + "/" + e.path;
            t.bytes += e.size;
            t.files.push_back(std::move(e));
        }
}

// ---- SHA-256 (FIPS 180-4) ----
struct Sha256
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t block[64];
    size_t used = 0;
    uint64_t length = 0;

    static uint32_t Rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

    void Compress(const uint8_t* p)
    {
        static const uint32_t k[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c,
            0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
        {
            const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }

    void Update(const uint8_t* p, size_t n)
    {
        length += n;
        while (n)
        {
            const size_t take = std::min(n, 64 - used);
            memcpy(block + used, p, take);
            used += take, p += take, n -= take;
            if (used == 64)
                Compress(block), used = 0;
        }
    }

    std::string Hex()
    {
        const uint64_t bits = length * 8;
        const uint8_t pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (used != 56)
            Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++)
            len[i] = uint8_t(bits >> (56 - 8 * i));
        Update(len, 8);
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (uint32_t v : h)
            for (int i = 28; i >= 0; i -= 4)
                s += digits[(v >> i) & 15];
        return s;
    }
};
} // namespace

std::string Sha256Hex(const uint8_t* data, size_t size)
{
    Sha256 s;
    s.Update(data, size);
    return s.Hex();
}

std::string FormatBytes(uint64_t bytes)
{
    char buf[32];
    if (bytes >= (1ull << 30))
        snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
    else
        snprintf(buf, sizeof(buf), "%.0f MB", double(bytes) / double(1ull << 20));
    return buf;
}

bool Inspect(const fs::path& path, Dump& dump, std::string& error)
{
    dump = Dump{};
    dump.path = path;
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        // A title folder itself: look at its parent, where the update sits beside it.
        fs::path root = path;
        if (fs::exists(path / "code" / "app.xml", ec) && path.has_parent_path())
            root = path.parent_path();
        dump.source = std::make_unique<FolderSource>(root);
    }
    else if (fs::is_regular_file(path, ec))
    {
        ZArchiveReader* z = ZArchiveReader::OpenFromFile(path);
        if (!z)
        {
            error = "This file is not a .wua archive (or it is damaged).";
            return false;
        }
        dump.archive = true;
        dump.source = std::make_unique<WuaSource>(z);
    }
    else
    {
        error = "Cannot open " + Utf8(path) + ".";
        return false;
    }

    std::vector<Title> titles;
    FindTitles(*dump.source, "", 4, titles);
    bool other = false;
    std::string otherIds;
    for (Title& t : titles)
    {
        if (t.titleId == BaseTitleId && (dump.base.dir.empty() || t.version > dump.base.version))
            dump.base = t;
        else if (t.titleId == UpdateTitleId && (dump.update.dir.empty() || t.version > dump.update.version))
            dump.update = t;
        else if (t.titleId.rfind("0005000E", 0) != 0 && t.titleId.rfind("0005000C", 0) != 0)
            other = true, otherIds += (otherIds.empty() ? "" : ", ") + t.titleId;
    }
    if (dump.base.dir.empty())
    {
        if (titles.empty())
            error = "No Wii U game was found here. Choose your Devil's Third .wua file, or the folder that holds its "
                    "extracted title folders (code, content and meta).";
        else if (other)
            error = "This is not Devil's Third's North American release (found title " + otherIds +
                    "; expected " + BaseTitleId + "). Only the North American (USA) release is supported.";
        else
            error = "Only an update was found here: the base game (title " + std::string(BaseTitleId) +
                    ") is missing. Choose the folder or archive that holds both.";
        return false;
    }
    if (dump.base.region != "00000002" && !dump.base.region.empty())
        dump.warnings.push_back("The game's region code is " + dump.base.region + ", not North America's (00000002).");

    // The RPX: the update's, else the base title's (a v16 base title carries the updated one).
    dump.rpxTitleDir = !dump.update.dir.empty() ? dump.update.dir : dump.base.dir;
    std::vector<uint8_t> rpx;
    if (!dump.source->ReadAll(Join(dump.rpxTitleDir, RpxName), rpx))
    {
        error = "The game's program (" + std::string(RpxName) + ") is missing.";
        return false;
    }
    if (Sha256Hex(rpx.data(), rpx.size()) != RpxSha256)
    {
        const int version = !dump.update.dir.empty() ? dump.update.version : dump.base.version;
        if (version < SupportedVersion)
            error = "This copy of Devil's Third is version " + std::to_string(version) + ". dt-port needs version " +
                    std::to_string(SupportedVersion) + ", the last update: install the update (title " +
                    UpdateTitleId + ") and dump it together with the game.";
        else
            error = "The game's program does not match version " + std::to_string(SupportedVersion) +
                    " of Devil's Third (its checksum differs). The dump may be damaged or modified: dump it again.";
        return false;
    }

    ListFiles(*dump.source, dump.base);
    if (!dump.update.dir.empty())
        ListFiles(*dump.source, dump.update);
    uint64_t contentFiles = 0, contentBytes = 0;
    for (const Title* t : { &dump.base, &dump.update })
        for (const FileEntry& f : t->files)
            if (f.path.rfind("content/", 0) == 0)
                contentFiles++, contentBytes += f.size;
    if (contentFiles < ContentFiles)
    {
        error = "The game's content is incomplete: " + std::to_string(contentFiles) + " files (" +
                FormatBytes(contentBytes) + ") instead of " + std::to_string(ContentFiles) + " (" +
                FormatBytes(ContentBytes) + "). Dump the game again.";
        return false;
    }
    if (contentFiles != ContentFiles || contentBytes != ContentBytes)
        dump.warnings.push_back("The content differs from the verified copy (" + std::to_string(contentFiles) +
                                " files, " + FormatBytes(contentBytes) + "); it should still work.");
    return true;
}

bool Extract(Dump& dump, const fs::path& dest, const Progress& progress, std::string& error)
{
    const uint64_t total = dump.TotalBytes();
    uint64_t done = 0;
    std::vector<uint8_t> buffer(8 << 20);
    for (const Title* t : { &dump.base, &dump.update })
    {
        if (t->dir.empty())
            continue;
        char name[64];
        snprintf(name, sizeof(name), "%s_v%d", t->titleId == BaseTitleId ? "0005000010177600" : "0005000e10177600",
            t->version);
        const fs::path root = dest / name;
        for (const FileEntry& f : t->files)
        {
            const fs::path out = root / fs::u8path(f.path);
            std::error_code ec;
            // Resumable: a file already there with the right size was copied by an earlier, interrupted run.
            if (fs::exists(out, ec) && fs::file_size(out, ec) == f.size)
            {
                done += f.size;
                if (progress && !progress(done, total))
                    return error = "Cancelled.", false;
                continue;
            }
            fs::create_directories(out.parent_path(), ec);
            const fs::path part = out.string() + ".part";
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            if (!o)
            {
                error = "Cannot write " + Utf8(out) + ".";
                return false;
            }
            for (uint64_t off = 0; off < f.size;)
            {
                const uint64_t n = std::min<uint64_t>(buffer.size(), f.size - off);
                if (!dump.source->Read(Join(t->dir, f.path), off, n, buffer.data()))
                {
                    error = "Cannot read " + f.path + " from the dump.";
                    return false;
                }
                o.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(n));
                if (!o)
                {
                    error = "Cannot write " + Utf8(out) + " (is the disk full?).";
                    return false;
                }
                off += n, done += n;
                if (progress && !progress(done, total))
                    return error = "Cancelled.", false;
            }
            o.close();
            fs::rename(part, out, ec);
            if (ec)
            {
                error = "Cannot write " + Utf8(out) + ".";
                return false;
            }
        }
    }
    return true;
}

bool ReadRpx(Dump& dump, std::vector<uint8_t>& out, std::string& error)
{
    if (!dump.source->ReadAll(Join(dump.rpxTitleDir, RpxName), out))
        return error = "Cannot read the game's program.", false;
    return true;
}
} // namespace setup
