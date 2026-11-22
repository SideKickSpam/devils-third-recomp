// dt_port: host entry point. Sets up guest memory and coreinit, then runs the game's entry point.
#include "cafe/coreinit.h"
#include "cafe/gx2.h"
#include "cafe/heap.h"
#include "gpu/device.h"
#include "gpu/dump.h"
#include "gpu/renderer.h"
#include "gpu/surface.h"
#include "guest.h"
#include "host/audio.h"
#include "host/button_text.h"
#include "host/input.h"
#include "host/window.h"
#include "assets/game_fonts.h"
#include "game_module.h"
#include "game_patches.h"
#include "hooks.h"
#include "imports.h"
#include "loader.h"
#include "log.h"
#include "platform.h"
#include "settings.h"
#include "trace.h"
#include "vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <ctime>
#include <set>
#include <filesystem>
#include <fstream>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>


namespace guest
{
uint8_t* base = nullptr;
}

namespace
{
void Usage()
{
    fprintf(stderr,
        "usage: dt_port [options]\n"
        "  --game-dir DIR       extracted game (default: DT_GAME_DIR, build/game-dir.txt, or game); expects the title folders (dt.elf optional)\n"
        "  --save-dir DIR       keep saves (/vol/save) in DIR instead of <game-dir>/save\n"
        "  --log FILE           write the import call log to FILE instead of stderr\n"
        "  --no-import-log      do not log import calls\n"
        "  --quiet NAME         do not log calls to import NAME (repeatable, e.g. --quiet OSFastMutex_Lock)\n"
        "  --log-only NAME      log only calls to these imports (repeatable)\n"
        "  --trace FILE         log every guest function entry to FILE\n"
        "  --hang-timeout SECS  report a hang after SECS seconds without progress (default 15, 0 = off)\n"
        "  --frame-timeout SECS report a hang after SECS seconds without a presented frame (default off)\n"
        "  --frames N           stop with a report after N frames (GX2SwapScanBuffers calls)\n"
        "  --hidden             render normally (screenshots work) but do not show the window\n"
        "  --no-window          do not open the SDL window\n"
        "  --no-cull            draw both faces of every polygon (debugging)\n"
        "  --mute               play sound at zero volume (implied by --hidden; --no-window has no sound device)\n"
        "  --audio-dump FILE    save the mixed sound as a WAV file\n"
        "  --gx2-dump DIR       decode GX2 objects into the log and save shader programs to DIR\n"
        "  --selftest           run the renderer's self-tests (surface detiling against addrlib) and exit\n"
        "  --capture-frame N    save every render pass of frame N as PNGs (and log its draws) to --capture-dir\n"
        "                       (repeatable: each frame then goes to <dir>/f<N>)\n"
        "  --capture-dir DIR    where --capture-frame writes (default: capture)\n"
        "  --gpu-stats          log renderer counters and the frame rate every 300 frames\n"
        "  --frame-csv FILE     write each frame's time and where it went (compiling, game code, ...) as CSV\n"
        "  --validate           enable the Vulkan validation layer (needs the Vulkan SDK) and log its messages\n"
        "  --shader-cache DIR   cache translated shaders in DIR (default: shadercache; \"\" = off)\n"
        "  --rewrite-test FILE  print the button-name rewrites of a localisation dump for the --prompts device, and exit\n"
        "  --pad-press F:BUTTONS[:N] press buttons of a virtual Xbox-style controller (a, b, start, dpdown, ...) at frame F\n"
        "  --press F:BUTTONS[:N] press GamePad buttons (a, b, plus, a+plus, ...) or hold sticks (lup, rleft, ...) at frame F\n"
        "                        for N frames (default 6)\n"
        "  --profile THREAD     log where guest thread THREAD spends its time every 5 s (e.g. \"Game Thread\")\n"
        "  --sample FILE        sample native stacks of the game and rendering threads ~1000/s (Windows) into FILE\n"
        "                       (src/tools/sample_report.py); --sample-threads A,B picks other threads\n"
        "  --screenshot N       save the TV image at frame N as PNG (repeatable)\n"
        "  --screenshot-every N save the TV image every N frames\n"
        "  --screenshot-dir DIR where screenshots go (default: screenshots)\n"
        "  --screenshot-window  screenshots of the presented window image, overlay included\n"
        "  --record FIRST:COUNT copies COUNT presented window images from frame FIRST without stalling the game,\n"
        "                       then writes them as PNGs to --screenshot-dir (for checking flicker)\n"
        "  --prompts DEVICE     button prompts as for keyboard, xbox, playstation or nintendo (tests)\n"
        "  --config FILE        settings file (default: settings.ini in the user's config directory)\n"
        "  --set KEY=VALUE      override a setting for this run (e.g. --set display.vsync=mailbox)\n"
        "  --vsync MODE         fifo (default; use with G-Sync/FreeSync), mailbox or immediate\n"
        "  --display MODE       windowed, borderless or exclusive\n");
}
} // namespace

// diagnostics.session_logs (setting, default on): every run writes <settings dir>/sessions/<date>_<time>.log (the
// important log lines: slow frames, statistics, warnings, errors) and <date>_<time>-frames.csv (as --frame-csv),
// keeping the newest diagnostics.keep_sessions (default 5) sessions.
static void StartSessionLogs(bool frameCsvGiven, int argc, char** argv)
{
    if (!settings::GetBool("diagnostics.session_logs", true))
        return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(settings::Directory().empty() ? "." : settings::Directory()) / "sessions";
    fs::create_directories(dir, ec);
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm local;
    platform::LocalTime(now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    // Sessions are the files sharing a 19-character timestamp prefix; drop the oldest to make room for this one.
    std::set<std::string> stamps;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (const std::string n = e.path().filename().string(); n.size() >= 19 && n[4] == '-' && n[10] == '_')
            stamps.insert(n.substr(0, 19));
    const size_t keep = size_t(std::max(1, settings::GetInt("diagnostics.keep_sessions", 5)));
    while (stamps.size() >= keep)
    {
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().filename().string().rfind(*stamps.begin(), 0) == 0)
                fs::remove(e.path(), ec);
        stamps.erase(stamps.begin());
    }
    const std::string base = (dir / stamp).string();
    dtlog::Mirror((base + ".log").c_str());
    std::string args;
    for (int i = 1; i < argc; i++)
        args += std::string(" ") + argv[i];
    dtlog::Important("[dt-port] session %s; settings %s; arguments:%s\n", stamp, settings::Path().c_str(), args.c_str());
    if (!frameCsvGiven)
    {
        gpu::renderer::SetFrameCsv(base + "-frames.csv");
        cafe::SetImportTiming(true);
    }
}

// The game directory when no --game-dir is given, found as src/tools/dt-port.cmd finds it: DT_GAME_DIR, the first line
// of build/game-dir.txt, or game/.
std::string DefaultGameDir()
{
    if (const char* env = getenv("DT_GAME_DIR"); env && *env)
        return env;
    std::ifstream file("build/game-dir.txt");
    std::string line;
    if (file && std::getline(file, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            return line;
    }
    return "game";
}

// The repository whose build/game/ holds the executable (empty if it is not in one).
std::filesystem::path RepositoryRoot()
{
    const char* base = SDL_GetBasePath();  // the executable's directory, with a trailing separator
    if (!base)
        return {};
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(base).parent_path().parent_path().parent_path();
    if (!std::filesystem::exists(root / "src" / "runtime", ec) && !std::filesystem::exists(root / "build" / "game-dir.txt", ec))
        return {};
    return root;
}

// Without --game-dir: run from the repository root, as the launcher does, when the executable sits in its build/game/,
// so build/game-dir.txt, the shader cache and the logs are the launcher's wherever it was started from.
bool UseRepositoryRoot()
{
    const std::filesystem::path root = RepositoryRoot();
    if (root.empty())
        return false;
    std::error_code ec;
    std::filesystem::current_path(root, ec);
    return !ec;
}

int main(int argc, char** argv)
{
    std::string gameDir, tracePath;
    std::string gameModule;  // --game-module: the generated code's DLL (builds with DT_GAME_MODULE)
    std::string rewriteTest;  // --rewrite-test: a localisation dump to run host::button_text over
    // Double-clicked (no arguments): behave like the launcher (no call log, the log in build/), and show start-up errors
    // in a box, since the console closes with the process.
    // An installed release (the setup tool's install.ini, outside a repository): the game, the shader cache and the
    // log in %LOCALAPPDATA%\dt-port, saves in the settings directory; started without arguments, as the launcher.
    const bool repository = !RepositoryRoot().empty();
    const std::string installedGame = repository ? std::string() : game_module::InstalledGameDir();
    const std::string dataDir = game_module::DataDir();
    const bool installed = game_module::Used() && !repository && !dataDir.empty();
    const bool doubleClicked = argc == 1 && (repository || installed);
    if (doubleClicked)
    {
        if (repository)
            UseRepositoryRoot();
        std::error_code ec;
        const std::string logDir = installed ? dataDir : std::string("build");
        std::filesystem::create_directories(std::filesystem::u8path(logDir), ec);
        dtlog::Open((logDir + "/dt_port.log").c_str());
        cafe::SetImportLogging(false);
    }
    if (installed)
        cafe::gx2::SetShaderCacheDir(dataDir + "/shadercache");
    bool saveDirGiven = false;
    int hangTimeout = 15;
    int frameTimeout = 0;
    std::string shotDir = "screenshots";
    bool shotWindow = false;
    std::string recordSpec;  // --record FIRST:COUNT
    std::vector<uint64_t> shotFrames;
    uint64_t shotEvery = 0;
    bool selfTest = false;
    std::vector<uint64_t> captureFrames;  // --capture-frame (repeatable)
    std::string captureDir = "capture";
    std::string samplePath, sampleThreads = "Game Thread,RenderingThread";
    bool frameCsvGiven = false;
    std::string fontSampleDir;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                Usage();
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--game-dir") gameDir = value();
        else if (a == "--game-module") gameModule = value();
        else if (a == "--config") settings::Load(value());
        else if (a == "--vsync") settings::Override("display.vsync", value());
        else if (a == "--display") settings::Override("display.mode", value());
        else if (a == "--set")
        {
            const std::string kv = value();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos)
            {
                Usage();
                return 2;
            }
            settings::Override(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (a == "--log") dtlog::Open(value().c_str());
        else if (a == "--save-dir") vfs::SetSaveDir(value()), saveDirGiven = true;
        else if (a == "--no-import-log") cafe::SetImportLogging(false);
        else if (a == "--quiet") cafe::QuietImport(value());
        else if (a == "--log-only") cafe::LogOnlyImport(value());
        else if (a == "--trace") tracePath = value();
        else if (a == "--hang-timeout") hangTimeout = std::stoi(value());
        else if (a == "--frame-timeout") frameTimeout = std::stoi(value());
        else if (a == "--frames") cafe::gx2::SetFrameLimit(std::stoull(value()));
        else if (a == "--no-window") host::SetWindowEnabled(false);
        else if (a == "--hidden") host::SetWindowHidden(true), host::audio::SetMuted(true);
        else if (a == "--mute") host::audio::SetMuted(true);
        else if (a == "--no-cull") gpu::renderer::SetNoCull(true);
        else if (a == "--audio-dump") host::audio::SetDumpPath(value());
        else if (a == "--gx2-dump") gpu::dump::Enable(value());
        else if (a == "--pad-press")
        {
            const std::string spec = value();
            if (!host::input::AddPadPress(spec))
            {
                fprintf(stderr, "bad --pad-press %s\n", spec.c_str());
                return 2;
            }
        }
        else if (a == "--press")
        {
            const std::string spec = value();
            if (!host::input::AddScriptedPress(spec))
            {
                fprintf(stderr, "bad --press %s\n", spec.c_str());
                return 2;
            }
        }
        else if (a == "--profile") trace::StartProfiler(value());
        else if (a == "--sample") samplePath = value();
        else if (a == "--font-sample") fontSampleDir = value();
        else if (a == "--sample-threads") sampleThreads = value();
        else if (a == "--selftest") selfTest = true;
        else if (a == "--gpu-stats") gpu::renderer::EnableStats(), cafe::SetImportTiming(true, true);
        else if (a == "--frame-csv") gpu::renderer::SetFrameCsv(value()), cafe::SetImportTiming(true), frameCsvGiven = true;
        else if (a == "--capture-frame") captureFrames.push_back(std::stoull(value()));
        else if (a == "--capture-dir") captureDir = value();
        else if (a == "--validate") gpu::SetValidation(true);
        else if (a == "--shader-cache") cafe::gx2::SetShaderCacheDir(value());
        else if (a == "--screenshot") shotFrames.push_back(std::stoull(value()));
        else if (a == "--screenshot-every") shotEvery = std::stoull(value());
        else if (a == "--screenshot-dir") shotDir = value();
        else if (a == "--screenshot-window") shotWindow = true;
        else if (a == "--record") recordSpec = value();
        else if (a == "--rewrite-test") rewriteTest = value();
        else if (a == "--prompts")
        {
            if (!host::input::ForceDevice(value()))
            {
                Usage();
                return 1;
            }
        }
        else if (a == "--help" || a == "-h")
        {
            Usage();
            return 0;
        }
        else
        {
            Usage();
            return 2;
        }
    }

    if (!settings::Loaded())
        settings::Load();
    StartSessionLogs(frameCsvGiven, argc, argv);
    // cpu.cores: all (default), performance or efficiency (hybrid CPUs; for measurements).
    dtlog::Important("[dt-port] cpu: %s\n", platform::RestrictCores(settings::Get("cpu.cores", "all")).c_str());
    for (uint64_t f : captureFrames)
        gpu::renderer::SetCapture(captureDir, f);
    if (!shotFrames.empty() || shotEvery)
        gpu::renderer::SetScreenshots(shotDir, shotFrames, shotEvery, shotWindow);
    if (!recordSpec.empty())
    {
        const size_t colon = recordSpec.find(':');
        gpu::GetDevice().StartRecording(std::stoull(recordSpec.substr(0, colon)),
            colon == std::string::npos ? 60u : uint32_t(std::stoul(recordSpec.substr(colon + 1))), shotDir);
    }

    // Closing the window, Ctrl+C or SIGTERM exits at once, even mid-load.
    auto quit = [](const char* reason) {
        dtlog::Important("[dt-port] quit requested (%s) after %llu frames\n", reason,
            (unsigned long long)cafe::gx2::FrameCount());
        gpu::renderer::Shutdown();
        host::audio::Close();
        settings::Save();
        dtlog::Flush();
        fflush(stdout);
        std::_Exit(0);
    };
    host::SetQuitHandler(quit);
    platform::InstallQuitSignals(quit);
    if (!rewriteTest.empty())
    {
        // Every "key=value" line of a localisation dump ("[Section]" headers) whose text the rewrite changes, for the
        // device chosen with --prompts, with the default bindings or --config's.
        std::ifstream in(rewriteTest);
        std::string line, section;
        auto widen = [](const std::string& s) { return std::u16string(s.begin(), s.end()); };
        while (std::getline(in, line))
        {
            if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq), text = line.substr(eq + 1);
            if (const auto out = host::button_text::Rewrite(widen(section), widen(key), widen(text)))
                printf("%s.%s\n  %s\n  %s\n", section.c_str(), key.c_str(), text.c_str(), std::string(out->begin(), out->end()).c_str());
        }
        return 0;
    }
    platform::UsePreciseTimers();
    guest::base = platform::ReserveGuestMemory(PPC_MEMORY_SIZE);
    if (guest::base && selfTest)
        return gpu::surface::SelfTest() ? 1 : 0;
    if (!guest::base)
    {
        fprintf(stderr, "error: cannot reserve the 4 GB guest address space\n");
        return 1;
    }

    RpxInfo info;
    std::string error;
    if (installed && !saveDirGiven)
        vfs::SetSaveDir(settings::Directory() + "save");
    if (gameDir.empty() && installed && installedGame.empty() && !getenv("DT_GAME_DIR"))
    {
        // Not set up yet: the setup tool asks for the game and builds the module.
        if (game_module::LaunchSetup(""))
            return 0;
    }
    if (gameDir.empty() && !installedGame.empty() && !getenv("DT_GAME_DIR"))
    {
        gameDir = installedGame;
        dtlog::Important("[dt-port] game directory %s (from install.ini)\n", gameDir.c_str());
    }
    if (gameDir.empty())
    {
        UseRepositoryRoot();
        gameDir = DefaultGameDir();
        dtlog::Important("[dt-port] game directory %s (from %s)\n", gameDir.c_str(),
               getenv("DT_GAME_DIR") && *getenv("DT_GAME_DIR") ? "DT_GAME_DIR" : gameDir == "game" ? "the default" : "build/game-dir.txt");
    }
    // dt.elf (the decompressed RPX) if there is one, else the RPX in the update or base title's code folder.
    std::string rpxPath = gameDir + "/dt.elf";
    if (std::error_code ec; !std::filesystem::exists(std::filesystem::u8path(rpxPath), ec))
        for (const char* prefix : { "0005000e", "00050000" })
            for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(gameDir), ec))
                if (e.path().filename().string().rfind(prefix, 0) == 0 &&
                    std::filesystem::exists(e.path() / "code" / "DevilsThirdGame-WiiU-Shipping.rpx", ec) &&
                    !std::filesystem::exists(std::filesystem::u8path(rpxPath), ec))
                {
                    const auto p = (e.path() / "code" / "DevilsThirdGame-WiiU-Shipping.rpx").u8string();
                    rpxPath.assign(p.begin(), p.end());
                }
    if (!LoadRpx(rpxPath, info, error) || !vfs::Init(gameDir, error))
    {
        fprintf(stderr, "error: %s\n", error.c_str());
        if (doubleClicked)
        {
            const std::string text = installed
                ? error + "\n\nRun DevilsThirdSetup.exe again and point it at your copy of the game."
                : error + "\n\nPut the game directory (holding dt.elf and the extracted title folders) "
                          "in build\\game-dir.txt, or set DT_GAME_DIR.";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "dt-port", text.c_str(), nullptr);
        }
        return 1;
    }
    if (!fontSampleDir.empty())
    {
        assets::WriteFontSamples(fontSampleDir);  // debugging: which of the game's fonts is which
        return 0;
    }
    // The generated code: linked in, or (Windows releases) the game module the setup tool built from the player's copy.
    if (std::string moduleError; !game_module::Load(gameModule, moduleError))
    {
        fprintf(stderr, "error: %s\n", moduleError.c_str());
        // A release whose module is missing or from another version: the setup tool rebuilds it, then starts the game.
        if (installed && gameModule.empty() && game_module::LaunchSetup("--rebuild"))
            return 0;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Devil's Third", moduleError.c_str(), nullptr);
        return 1;
    }
    trace::FinishSymbols();
    cafe::InitImports();
    game_patches::Apply();
    hooks::InstallAll();

    const uint32_t mem2Start = guest::AlignUp(info.dataEnd, 0x10000);
    cafe::InitMemory(mem2Start);
    cafe::coreinit::InitSystem();
    cafe::coreinit::InitDataImports();

    // The RPL asks for a 64 KB main stack; a larger one is indistinguishable to the game and safer.
    const uint32_t stackSize = std::max<uint32_t>(info.stackSize, 1024 * 1024);
    cafe::coreinit::Thread& mainThread = cafe::coreinit::InitMainThread(stackSize, info.sdaBase, info.sda2Base);
    trace::RegisterThread("main", &mainThread.ctx);
    trace::InstallCrashHandlers();
    if (!tracePath.empty())
        trace::EnableFullTrace(tracePath.c_str());
    if (hangTimeout > 0)
        trace::StartWatchdog(hangTimeout);
    if (frameTimeout > 0)
        trace::StartFrameWatchdog(frameTimeout);
    // DT_WATCH=<guest address> (debugging, Windows): log every write to that guest word with the writer's stack.
    if (const char* watch = getenv("DT_WATCH"))
    {
        static uint32_t addr = uint32_t(strtoul(watch, nullptr, 16));
        dtlog::Important("[watch] writes to 0x%08X\n", addr);
        platform::WatchWrites(guest::Ptr(addr), [](const uint64_t* stack, int depth) {
            if (!stack)
            {
                dtlog::Important("[watch] 0x%08X is now 0x%08X (seen by polling) at frame %llu\n", addr, guest::Read32(addr),
                    (unsigned long long)cafe::gx2::FrameCount());
                return;
            }
            const uint64_t image = platform::ImageBase();
            std::string line;
            for (int i = 0; i < depth; i++)
            {
                char b[32];
                snprintf(b, sizeof(b), stack[i] - image < 0x40000000 ? " e+%llx" : " ?%llx",
                    (unsigned long long)(stack[i] - image < 0x40000000 ? stack[i] - image : stack[i]));
                line += b;
            }
            dtlog::Important("[watch] 0x%08X = 0x%08X on %s at frame %llu\n--- thread %s\n    native stack, innermost first:%s\n",
                addr, guest::Read32(addr), trace::CurrentThreadName().c_str(), (unsigned long long)cafe::gx2::FrameCount(),
                trace::CurrentThreadName().c_str(), line.c_str());
        });
    }
    if (!samplePath.empty())
    {
        std::vector<std::string> names;
        for (size_t start = 0; start <= sampleThreads.size();)
        {
            const size_t comma = std::min(sampleThreads.find(',', start), sampleThreads.size());
            if (comma > start)
                names.push_back(sampleThreads.substr(start, comma - start));
            start = comma + 1;
        }
        trace::StartSampler(samplePath, names);
    }

    // Like coreinit, give the application a chance to replace the base heaps before its entry point runs:
    // __preinit_user(MEMHeapHandle* mem1, MEMHeapHandle* foreground, MEMHeapHandle* mem2).
    if (auto it = info.exports.find("__preinit_user"); it != info.exports.end())
    {
        const uint32_t handles = guest::SystemAlloc(12);
        const uint32_t arenas[3] = { 0, 8, 1 };
        for (int i = 0; i < 3; i++)
            guest::Write32(handles + i * 4, cafe::BaseHeapHandle(arenas[i]));
        guest::Call(it->second, { handles, handles + 4, handles + 8 });
        for (int i = 0; i < 3; i++)
            cafe::SetBaseHeapHandle(arenas[i], guest::Read32(handles + i * 4));
        dtlog::Important("[dt-port] __preinit_user done: MEM1 heap 0x%08X, FG heap 0x%08X, MEM2 heap 0x%08X\n",
            cafe::BaseHeapHandle(0), cafe::BaseHeapHandle(8), cafe::BaseHeapHandle(1));
    }

    // argv[0] is the RPX name, as cos.xml's argstr gives it.
    const char* exe = "DevilsThirdGame-WiiU-Shipping.rpx";
    const uint32_t argvArray = guest::SystemAlloc(8);
    const uint32_t arg0 = guest::SystemAlloc(uint32_t(strlen(exe) + 1));
    guest::WriteString(arg0, exe, strlen(exe) + 1);
    guest::Write32(argvArray, arg0);

    dtlog::Important("[dt-port] MEM2 heap 0x%08X-0x%08X, main stack %u KB, sda 0x%08X/0x%08X, entry %s\n", mem2Start,
        guest::Mem2End, stackSize / 1024, info.sdaBase, info.sda2Base, trace::Describe(info.entry).c_str());
    cafe::coreinit::RunMainThread(info.entry, 1, argvArray);
}
