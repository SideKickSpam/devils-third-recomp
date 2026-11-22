#include "install.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <chrono>
#include <ctime>
#include <fstream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace fs = std::filesystem;

namespace setup
{
namespace
{
std::string Utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path Env(const char* name)
{
    const char* v = getenv(name);
    return v && *v ? fs::u8path(v) : fs::path();
}

bool IsUserSaveDir(const fs::path& d)
{
    std::error_code ec;
    return fs::exists(d / "d3sys.sav", ec) || fs::exists(d / "d3auto.sav", ec);
}

// The newest account folder (with a save) under a title's save folder (<...>/user), plus its common folder.
bool FromUserRoot(const fs::path& userRoot, const std::string& label, SaveCandidate& out)
{
    std::error_code ec;
    bool found = false;
    for (const auto& e : fs::directory_iterator(userRoot, ec))
    {
        if (!e.is_directory(ec) || e.path().filename() == "common" || !IsUserSaveDir(e.path()))
            continue;
        const auto t = fs::last_write_time(e.path(), ec);
        if (!found || t > out.time)
        {
            out.user = e.path();
            out.time = t;
            found = true;
        }
    }
    if (found)
    {
        out.common = fs::exists(userRoot / "common", ec) ? userRoot / "common" : fs::path();
        out.label = label;
    }
    return found;
}

// Cemu's layout under an mlc01 folder.
fs::path CemuUserRoot(const fs::path& mlc) { return mlc / "usr" / "save" / "00050000" / "10177600" / "user"; }

// Cemu's mlc_path from its settings.xml, if set.
fs::path CemuMlcFromSettings(const fs::path& settings)
{
    std::ifstream in(settings);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const size_t a = text.find("<mlc_path>");
    const size_t b = a == std::string::npos ? std::string::npos : text.find("</mlc_path>", a);
    if (b == std::string::npos)
        return {};
    return fs::u8path(text.substr(a + 10, b - a - 10));
}

void CopyTree(const fs::path& from, const fs::path& to, std::error_code& ec)
{
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
}
} // namespace

fs::path SettingsDir()
{
    char* dir = SDL_GetPrefPath(nullptr, "dt-port");
    fs::path p = dir ? fs::u8path(dir) : fs::path();
    SDL_free(dir);
    return p;
}

fs::path SaveDir() { return SettingsDir() / "save"; }

bool WriteInstallIni(const fs::path& dataDir, const fs::path& gameDir, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dataDir, ec);
    std::ofstream out(dataDir / "install.ini", std::ios::binary | std::ios::trunc);
    out << "# Written by the dt-port setup tool.\r\ngame_dir=" << Utf8(gameDir) << "\r\n";
    if (!out)
        return error = "Cannot write " + Utf8(dataDir / "install.ini") + ".", false;
    return true;
}

fs::path InstalledGameDir(const fs::path& dataDir)
{
    std::ifstream in(dataDir / "install.ini");
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.rfind("game_dir=", 0) == 0)
            return fs::u8path(line.substr(9));
    }
    return {};
}

bool HasSave()
{
    std::error_code ec;
    return IsUserSaveDir(SaveDir() / "80000001");
}

std::vector<SaveCandidate> FindCemuSaves()
{
    std::vector<fs::path> mlcs;
#if defined(_WIN32)
    const fs::path appdata = Env("APPDATA");
    if (!appdata.empty())
    {
        if (fs::path m = CemuMlcFromSettings(appdata / "Cemu" / "settings.xml"); !m.empty())
            mlcs.push_back(m);
        mlcs.push_back(appdata / "Cemu" / "mlc01");
    }
#else
    const fs::path home = Env("HOME");
    if (!home.empty())
    {
        if (fs::path m = CemuMlcFromSettings(home / ".config" / "Cemu" / "settings.xml"); !m.empty())
            mlcs.push_back(m);
        mlcs.push_back(home / ".local" / "share" / "Cemu" / "mlc01");
    }
#endif
    std::vector<SaveCandidate> out;
    for (const fs::path& mlc : mlcs)
    {
        SaveCandidate c;
        if (FromUserRoot(CemuUserRoot(mlc), "Cemu (" + Utf8(mlc) + ")", c))
        {
            bool dup = false;
            for (const SaveCandidate& o : out)
                dup |= o.user == c.user;
            if (!dup)
                out.push_back(std::move(c));
        }
    }
    return out;
}

bool FindSaveIn(const fs::path& dir, SaveCandidate& out)
{
    std::error_code ec;
    if (IsUserSaveDir(dir))
    {
        out.user = dir;
        out.common = fs::exists(dir.parent_path() / "common", ec) ? dir.parent_path() / "common" : fs::path();
        out.label = Utf8(dir);
        out.time = fs::last_write_time(dir, ec);
        return true;
    }
    if (FromUserRoot(dir, Utf8(dir), out))  // a folder of account folders (Cemu's user/, dt-port's save/)
        return true;
    if (FromUserRoot(CemuUserRoot(dir), Utf8(dir), out))  // an mlc01 folder
        return true;
    return FromUserRoot(CemuUserRoot(dir / "mlc01"), Utf8(dir), out);  // a portable Cemu folder
}

bool ImportSave(const SaveCandidate& save, std::string& error)
{
    std::error_code ec;
    const fs::path dest = SaveDir();
    if (fs::exists(dest, ec) && !fs::is_empty(dest, ec))
    {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
        fs::rename(dest, SettingsDir() / ("save-backup-" + std::string(stamp)), ec);
        if (ec)
            return error = "Cannot back up the current save (" + ec.message() + ").", false;
    }
    CopyTree(save.user, dest / "80000001", ec);
    if (!ec && !save.common.empty())
        CopyTree(save.common, dest / "common", ec);
    if (ec)
        return error = "Cannot copy the save: " + ec.message() + ".", false;
    return true;
}

fs::path GameExe()
{
    const char* base = SDL_GetBasePath();
    const fs::path dir = base ? fs::u8path(base) : fs::path();
#if defined(_WIN32)
    return dir / "dt_port.exe";
#else
    return dir / "dt_port";
#endif
}

bool CreateShortcuts(bool desktop, bool startMenu, std::string& error)
{
#if defined(_WIN32)
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = true;
    auto make = [&](REFKNOWNFOLDERID folder) {
        PWSTR dir = nullptr;
        if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &dir)))
            return false;
        const fs::path link = fs::path(dir) / L"Devil's Third.lnk";
        CoTaskMemFree(dir);
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                reinterpret_cast<void**>(&sl))))
            return false;
        const fs::path exe = GameExe();
        sl->SetPath(exe.c_str());
        sl->SetWorkingDirectory(exe.parent_path().c_str());
        sl->SetDescription(L"Devil's Third (dt-port)");
        sl->SetIconLocation(exe.c_str(), 0);
        IPersistFile* pf = nullptr;
        bool saved = false;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))))
        {
            saved = SUCCEEDED(pf->Save(link.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
        return saved;
    };
    if (desktop && !make(FOLDERID_Desktop))
        ok = false, error = "Cannot create the desktop shortcut.";
    if (startMenu && !make(FOLDERID_Programs))
        ok = false, error = "Cannot create the Start menu shortcut.";
    if (SUCCEEDED(init))
        CoUninitialize();
    return ok;
#else
    (void)desktop, (void)startMenu, (void)error;
    return true;
#endif
}

bool LaunchGame()
{
    const fs::path exe = GameExe();
#if defined(_WIN32)
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si,
            &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return std::system(("\"" + Utf8(exe) + "\" &").c_str()) == 0;
#endif
}
} // namespace setup
