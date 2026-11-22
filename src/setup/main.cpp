// DevilsThirdSetup: the player points it at their copy of Devil's Third (a .wua archive or an extracted folder); it
// checks the copy, extracts it if needed, builds the game module (dt_game.dll) from it with the bundled tools, imports a
// save if wanted, and makes shortcuts. With --rebuild (started by dt_port.exe after a dt-port update) it rebuilds the
// module from the installed copy and starts the game.
//
// Command line (testing): --check PATH, --install PATH [--extract-to DIR] (no window; progress on stdout).
#include "dump.h"
#include "install.h"
#include "module_build.h"
#include "overlay_font.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <zlib.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace setup;
using Clock = std::chrono::steady_clock;

namespace
{
std::string Utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The folder whose title folders the runtime can use where they are: the base title and the update directly inside
// it, named as the runtime looks for them (00050000..., 0005000e...), and no other game beside them.
std::optional<fs::path> InPlaceRoot(Dump& dump)
{
    if (dump.archive)
        return std::nullopt;
    const fs::path root = dump.source->HostPath("");
    auto direct = [](const Title& t, const char* prefix) {
        return t.dir.empty() || (t.dir.find('/') == std::string::npos && t.dir.rfind(prefix, 0) == 0);
    };
    if (!direct(dump.base, "00050000") || dump.base.dir.empty() || !direct(dump.update, "0005000e"))
        return std::nullopt;
    int bases = 0, updates = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec))
    {
        const std::string n = Utf8(e.path().filename());
        bases += n.rfind("00050000", 0) == 0;
        updates += n.rfind("0005000e", 0) == 0;
    }
    if (bases != 1 || updates > 1)
        return std::nullopt;
    return root;
}

uint64_t FreeSpace(fs::path p)
{
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec))
        p = p.parent_path();
    const auto s = fs::space(p, ec);
    return ec ? 0 : s.available;
}

std::string Minutes(double seconds)
{
    if (seconds < 60)
        return "less than a minute";
    const int m = int(std::ceil(seconds / 60));
    return "about " + std::to_string(m) + (m == 1 ? " minute" : " minutes");
}

// ---- the work, shared by the window and the command line ----
struct Job
{
    Dump dump;
    BuildPaths paths;
    bool extract = false;
    bool fromArchive = false;
    fs::path gameDir;  // where the runtime reads the game: the folder in place, or the extraction target
    std::optional<SaveCandidate> save;
    BuildStatus build;
    std::atomic<int> phase{ 0 };  // 0 extracting, 1 building, 2 finishing, 3 done, 4 failed
    std::atomic<uint64_t> extracted{ 0 }, extractTotal{ 0 };
    std::atomic<bool> cancel{ false };
    std::mutex mutex;
    std::string error;
    Clock::time_point phaseStart = Clock::now();
    std::string warning;  // non-fatal (save import, shortcuts)

    void Fail(const std::string& e)
    {
        std::lock_guard lock(mutex);
        error = e;
        phase = 4;
    }

    void Run()
    {
        std::string err;
        if (extract)
        {
            phase = 0;
            phaseStart = Clock::now();
            extractTotal = dump.TotalBytes();
            if (!Extract(dump, gameDir, [&](uint64_t done, uint64_t) { extracted = done; return !cancel.load(); }, err))
                return Fail(err);
            // Build from the extracted copy (the archive is no longer needed).
            Dump copy;
            if (!Inspect(gameDir, copy, err))
                return Fail("The extracted copy does not check out: " + err);
            dump = std::move(copy);
        }
        phase = 1;
        phaseStart = Clock::now();
        if (!BuildModule(dump, paths, build))
        {
            std::lock_guard lock(build.mutex);
            return Fail(build.error);
        }
        phase = 2;
        if (save && !ImportSave(*save, err))
            warning = err;
        if (!WriteInstallIni(paths.data, gameDir, err))
            return Fail(err);
        phase = 3;
    }
};

int CommandLine(int argc, char** argv)
{
    const std::string mode = argv[1];
    if (argc < 3)
        return fprintf(stderr, "usage: DevilsThirdSetup [--check PATH | --install PATH [--extract-to DIR]]\n"), 2;
    Job job;
    std::string error;
    if (!Inspect(fs::u8path(argv[2]), job.dump, error))
        return printf("not supported: %s\n", error.c_str()), 1;
    printf("ok: base %s v%d (%s), update %s v%d, %s\n", job.dump.base.titleId.c_str(), job.dump.base.version,
        job.dump.base.region.c_str(), job.dump.update.dir.empty() ? "none" : job.dump.update.titleId.c_str(),
        job.dump.update.version, FormatBytes(job.dump.TotalBytes()).c_str());
    for (const std::string& w : job.dump.warnings)
        printf("warning: %s\n", w.c_str());
    if (mode == "--check")
        return 0;
    if (mode == "--extract" && argc >= 4)  // testing: only the extraction
    {
        const auto start = Clock::now();
        if (!Extract(job.dump, fs::u8path(argv[3]), {}, error))
            return printf("failed: %s\n", error.c_str()), 1;
        printf("extracted in %.1f s\n", std::chrono::duration<double>(Clock::now() - start).count());
        return 0;
    }
    job.paths = DefaultPaths();
    if (!CheckTools(job.paths, error))
        return printf("error: %s\n", error.c_str()), 1;
    const auto root = InPlaceRoot(job.dump);
    job.extract = !root || (argc >= 5 && std::string(argv[3]) == "--extract-to");
    job.gameDir = job.extract ? (argc >= 5 ? fs::u8path(argv[4]) : job.paths.data / "game") : *root;
    printf("game directory: %s%s\n", Utf8(job.gameDir).c_str(), job.extract ? " (extracting)" : " (in place)");
    std::thread worker([&] { job.Run(); });
    int lastPhase = -1, lastPercent = -1;
    while (job.phase < 3)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const int p = job.phase;
        int percent = 0;
        if (p == 0 && job.extractTotal)
            percent = int(job.extracted * 100 / job.extractTotal);
        else if (p == 1 && job.build.total)
            percent = int(job.build.done * 100 / job.build.total);
        const int step = int(job.build.step.load());
        if (p != lastPhase || percent / 5 != lastPercent / 5)
        {
            printf("phase %d step %d: %d%% (%d/%d files, %d jobs)\n", p, step, percent, job.build.filesDone.load(),
                job.build.filesTotal.load(), job.build.jobs.load());
            fflush(stdout);
            lastPhase = p, lastPercent = percent;
        }
    }
    worker.join();
    {
        std::lock_guard lock(job.build.mutex);
        for (const std::string& l : job.build.log)
            printf("  %s\n", l.c_str());
    }
    if (job.phase == 4)
        return printf("failed: %s\n", job.error.c_str()), 1;
    printf("done: %s\n", Utf8(job.paths.data / "dt_game.dll").c_str());
    return 0;
}

// ---- the window ----
enum class Page
{
    Welcome,
    Checking,
    Checked,
    Saves,
    Working,
    Done,
    Error,
};

struct Colors
{
    ImVec4 bg{ 0.075f, 0.075f, 0.085f, 1 }, panel{ 0.12f, 0.12f, 0.13f, 1 }, text{ 0.90f, 0.88f, 0.84f, 1 },
        dim{ 0.60f, 0.58f, 0.55f, 1 }, amber{ 0.91f, 0.64f, 0.23f, 1 }, good{ 0.45f, 0.80f, 0.45f, 1 },
        bad{ 0.92f, 0.40f, 0.35f, 1 };
} C;

struct App
{
    SDL_Window* window = nullptr;
    ImFont* font = nullptr;
    float scale = 1;
    Page page = Page::Welcome;
    std::string error;
    bool rebuild = false;  // --rebuild: started by the game after an update

    std::unique_ptr<Job> job;
    std::thread worker;
    std::atomic<bool> checked{ false };
    bool checkOk = false;

    // Choices.
    bool inPlace = false;
    std::optional<fs::path> inPlaceRoot;
    fs::path extractTo;
    std::vector<SaveCandidate> saves;
    int saveChoice = -1;  // -1: keep the current save / start fresh
    bool desktop = true, startMenu = true;

    // Results of the system file dialogs (they answer on another thread).
    std::mutex pickMutex;
    std::optional<fs::path> picked;
    bool pickedForSave = false;

    void Join()
    {
        if (worker.joinable())
            worker.join();
    }

    void StartCheck(const fs::path& path)
    {
        Join();
        job = std::make_unique<Job>();
        job->paths = DefaultPaths();
        page = Page::Checking;
        checked = false;
        worker = std::thread([this, path] {
            std::string err;
            checkOk = Inspect(path, job->dump, err) && CheckTools(job->paths, err);
            error = err;
            checked = true;
        });
    }

    void AfterCheck()
    {
        Join();
        if (!checkOk)
        {
            page = Page::Error;
            return;
        }
        inPlaceRoot = InPlaceRoot(job->dump);
        inPlace = inPlaceRoot.has_value();
        extractTo = job->paths.data / "game";
        if (rebuild)
            return StartWork();
        page = Page::Checked;
    }

    void StartWork()
    {
        Join();
        job->extract = !inPlace;
        job->fromArchive = job->dump.archive;
        job->gameDir = inPlace ? *inPlaceRoot : extractTo;
        if (saveChoice >= 0 && saveChoice < int(saves.size()))
            job->save = saves[size_t(saveChoice)];
        page = Page::Working;
        worker = std::thread([this] { job->Run(); });
    }

    void Pick(bool folder, bool forSave)
    {
        pickedForSave = forSave;
        auto cb = [](void* user, const char* const* files, int) {
            App* app = static_cast<App*>(user);
            if (files && files[0])
            {
                std::lock_guard lock(app->pickMutex);
                app->picked = fs::u8path(files[0]);
            }
        };
        static const SDL_DialogFileFilter filters[] = { { "Wii U archive (*.wua)", "wua" } };
        if (folder)
            SDL_ShowOpenFolderDialog(cb, this, window, nullptr, false);
        else
            SDL_ShowOpenFileDialog(cb, this, window, filters, 1, nullptr, false);
    }

    void Picked(const fs::path& p)
    {
        if (!pickedForSave)
            return StartCheck(p);
        SaveCandidate c;
        if (FindSaveIn(p, c))
        {
            saves.push_back(c);
            saveChoice = int(saves.size()) - 1;
        }
        else
            saveError = "No Devil's Third save was found in " + Utf8(p) + ".";
    }
    std::string saveError;

    // ---- drawing ----
    bool BigButton(const char* label, bool primary = true, float width = 0)
    {
        if (primary)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.36f, 0.10f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.48f, 0.15f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, C.amber);
        }
        const bool r = ImGui::Button(label, ImVec2(width, 44 * scale));
        if (primary)
            ImGui::PopStyleColor(3);
        return r;
    }

    void Heading(const char* text)
    {
        ImGui::PushFont(font, 26 * scale);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Spacing();
    }

    void Check(bool ok, const std::string& text)
    {
        ImGui::TextColored(ok ? C.good : C.bad, ok ? "  OK " : "  X  ");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text.c_str());
    }

    void Spinner()
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = 14 * scale, t = float(SDL_GetTicks()) / 1000.0f;
        ImDrawList* d = ImGui::GetWindowDrawList();
        d->PathArcTo(ImVec2(p.x + r, p.y + r), r, t * 5, t * 5 + 4.2f, 32);
        d->PathStroke(ImGui::GetColorU32(C.amber), 3 * scale);
        ImGui::Dummy(ImVec2(2 * r, 2 * r));
    }

    // Buttons at the bottom right; returns the index pressed or -1.
    int Footer(std::initializer_list<std::pair<const char*, bool>> buttons)
    {
        const float w = 170 * scale, gap = ImGui::GetStyle().ItemSpacing.x;
        const float total = float(buttons.size()) * w + float(buttons.size() - 1) * gap;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - total - 32 * scale, ImGui::GetWindowHeight() - 76 * scale));
        int i = 0, pressed = -1;
        for (const auto& [label, primary] : buttons)
        {
            if (i)
                ImGui::SameLine();
            if (BigButton(label, primary, w))
                pressed = i;
            i++;
        }
        if (pressed < 0 && AutoPress())
            pressed = int(buttons.size()) - 1;
        return pressed;
    }

    void DrawWelcome()
    {
        const fs::path dataDir = DefaultPaths().data;
        const fs::path installed = InstalledGameDir(dataDir);
        std::error_code ec;
        const bool ready = !installed.empty() && fs::exists(dataDir / "dt_game.dll", ec);
        Heading("Set up Devil's Third");
        ImGui::TextWrapped("dt-port runs the Wii U version of Devil's Third natively on your PC. It needs your own copy "
                           "of the game: the North American release with its last update (version 16), as a .wua file "
                           "(Cemu's format) or as a folder of extracted titles.");
        ImGui::Spacing();
        ImGui::TextColored(C.dim, "%s", "The setup checks your copy, then translates the game for your PC. This takes a "
                                        "few minutes and needs no internet connection.");
        ImGui::Spacing();
        ImGui::Spacing();
        if (BigButton("Choose a .wua file...", true, 300 * scale))
            Pick(false, false);
        ImGui::SameLine();
        if (BigButton("Choose a game folder...", false, 300 * scale))
            Pick(true, false);
        ImGui::Spacing();
        ImGui::TextColored(C.dim, "%s", "You can also drop the file or folder on this window.");
        if (ready)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped("dt-port is already set up (game files in %s).", Utf8(installed).c_str());
            if (BigButton("Play", true, 200 * scale) && LaunchGame())
                done = true;
        }
    }

    void DrawChecked()
    {
        Dump& d = job->dump;
        Heading("Your copy of Devil's Third");
        Check(true, "Devil's Third, North America (" + d.base.productCode + ", title " + d.base.titleId + ")");
        Check(true, "Version " + std::to_string(d.update.dir.empty() ? d.base.version : d.update.version) +
                        (d.update.dir.empty() ? " (in the base title)" : " (update installed)"));
        Check(true, "Game program verified (SHA-256)");
        size_t files = d.base.files.size() + d.update.files.size();
        Check(true, std::to_string(files) + " files, " + FormatBytes(d.TotalBytes()));
        for (const std::string& w : d.warnings)
            ImGui::TextColored(C.amber, "  !  %s", w.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const uint64_t buildBytes = 2ull << 30;  // generated code and objects
        if (inPlaceRoot)
        {
            if (ImGui::RadioButton("Use the game files where they are (recommended)", inPlace))
                inPlace = true;
            ImGui::TextColored(C.dim, "    %s", Utf8(*inPlaceRoot).c_str());
            if (ImGui::RadioButton("Copy them into dt-port's own folder", !inPlace))
                inPlace = false;
        }
        else
            ImGui::TextWrapped("The game files will be %s into:", d.archive ? "extracted" : "copied");
        if (!inPlace)
        {
            ImGui::TextColored(C.dim, "    %s", Utf8(extractTo).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Change..."))
            {
                pickedForTarget = true;
                SDL_ShowOpenFolderDialog(
                    [](void* user, const char* const* files, int) {
                        App* app = static_cast<App*>(user);
                        if (files && files[0])
                        {
                            std::lock_guard lock(app->pickMutex);
                            app->target = fs::u8path(files[0]) / "Devil's Third";
                        }
                    },
                    this, window, nullptr, false);
            }
        }
        const fs::path where = inPlace ? job->paths.data : extractTo;
        const uint64_t need = (inPlace ? 0 : d.TotalBytes()) + buildBytes, have = FreeSpace(where);
        const bool enough = have >= need;
        ImGui::Spacing();
        ImGui::TextColored(enough ? C.dim : C.bad, "Space needed: %s (free: %s)%s", FormatBytes(need).c_str(),
            FormatBytes(have).c_str(), enough ? "" : " - not enough space on this drive");
        const int b = Footer({ { "Back", false }, { "Continue", true } });
        if (b == 0)
            page = Page::Welcome;
        if (b == 1 && enough)
        {
            saves = FindCemuSaves();
            saveChoice = HasSave() || saves.empty() ? -1 : 0;
            page = Page::Saves;
        }
    }
    bool pickedForTarget = false;
    std::optional<fs::path> target;

    void DrawSaves()
    {
        Heading("Your save");
        const bool have = HasSave();
        if (have)
            ImGui::TextWrapped("dt-port already has a save (%s). Importing another one keeps it as a backup.",
                Utf8(SaveDir()).c_str());
        else
            ImGui::TextWrapped("You can bring your progress from Cemu or from another folder, or start fresh.");
        ImGui::Spacing();
        if (ImGui::RadioButton(have ? "Keep the current save" : "Start fresh", saveChoice < 0))
            saveChoice = -1;
        for (size_t i = 0; i < saves.size(); i++)
        {
            char when[64] = "";
            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(saves[i].time);
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&t));
            const std::string label = "Import the save from " + saves[i].label + "  (" + when + ")";
            if (ImGui::RadioButton(label.c_str(), saveChoice == int(i)))
                saveChoice = int(i);
        }
        ImGui::Spacing();
        if (ImGui::Button("Choose a save folder..."))
        {
            saveError.clear();
            Pick(true, true);
        }
        if (!saveError.empty())
            ImGui::TextColored(C.bad, "%s", saveError.c_str());
        ImGui::Spacing();
        ImGui::TextColored(C.dim, "%s", "Saves are kept in your user folder, so they survive dt-port updates.");
        const int b = Footer({ { "Back", false }, { "Set up", true } });
        if (b == 0)
            page = Page::Checked;
        if (b == 1)
            StartWork();
    }

    void Step(const char* label, int state)  // 0 waiting, 1 running, 2 done
    {
        ImGui::TextColored(state == 2 ? C.good : state == 1 ? C.amber : C.dim, state == 2 ? "  OK " : state == 1 ? "  >  " : "     ");
        ImGui::SameLine();
        ImGui::TextColored(state ? C.text : C.dim, "%s", label);
    }

    void DrawWorking()
    {
        Job& j = *job;
        Heading(rebuild ? "Updating the game for this version of dt-port" : "Setting up");
        const int phase = j.phase;
        const BuildStep step = j.build.step;
        auto state = [&](int p, BuildStep s) {
            if (phase > 1)
                return 2;
            if (p == 0)
                return phase == 0 ? 1 : 2;
            if (phase == 0)
                return 0;
            return step == s ? 1 : step > s ? 2 : 0;
        };
        if (j.extract)
            Step(j.fromArchive ? "Extracting the game files" : "Copying the game files", state(0, BuildStep::Idle));
        Step("Reading the game's program", state(1, BuildStep::Decompress));
        Step("Translating it to C++ (static recompilation)", state(1, BuildStep::Recompile));
        Step("Compiling it for your PC", state(1, BuildStep::Compile));
        Step("Linking the game module", state(1, BuildStep::Link));
        if (j.save)
            Step("Importing your save", phase >= 2 ? (phase >= 3 ? 2 : 1) : 0);
        ImGui::Spacing();
        ImGui::Spacing();
        double fraction = -1, elapsed = std::chrono::duration<double>(Clock::now() - j.phaseStart).count();
        std::string detail;
        if (phase == 0 && j.extractTotal)
        {
            fraction = double(j.extracted) / double(j.extractTotal);
            detail = FormatBytes(j.extracted) + " of " + FormatBytes(j.extractTotal);
        }
        else if (phase == 1 && step == BuildStep::Compile && j.build.total)
        {
            fraction = double(j.build.done) / double(j.build.total);
            detail = std::to_string(j.build.filesDone.load()) + " of " + std::to_string(j.build.filesTotal.load()) +
                     " files, " + std::to_string(j.build.jobs.load()) + " at a time";
            if (step != lastStep)
                stepStart = Clock::now();
            elapsed = std::chrono::duration<double>(Clock::now() - stepStart).count();
        }
        lastStep = step;
        if (fraction >= 0)
        {
            ImGui::ProgressBar(float(fraction), ImVec2(-1, 26 * scale), "");
            std::string eta;
            if (fraction > 0.03 && elapsed > 5)
                eta = "  -  " + Minutes(elapsed / fraction - elapsed) + " left";
            ImGui::TextColored(C.dim, "%d%%  -  %s%s", int(fraction * 100), detail.c_str(), eta.c_str());
        }
        else if (phase < 3)
            Spinner();
        ImGui::Spacing();
        if (ImGui::TreeNode("Details"))
        {
            ImGui::BeginChild("log", ImVec2(-1, 150 * scale), ImGuiChildFlags_Borders);
            std::lock_guard lock(j.build.mutex);
            for (const std::string& l : j.build.log)
                ImGui::TextUnformatted(l.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1);
            ImGui::EndChild();
            ImGui::TreePop();
        }
        if (phase == 3)
        {
            Join();
            if (rebuild)
            {
                if (!auto_)
                    LaunchGame();
                done = true;
            }
            page = Page::Done;
        }
        else if (phase == 4)
        {
            Join();
            error = j.error;
            page = Page::Error;
        }
        else if (Footer({ { "Cancel", false } }) == 0)
        {
            j.cancel = true;
            j.build.cancel = true;
        }
    }
    BuildStep lastStep = BuildStep::Idle;
    Clock::time_point stepStart = Clock::now();

    void DrawDone()
    {
        Heading("Ready to play");
        ImGui::TextWrapped("Devil's Third is set up. Your settings and saves are in %s.", Utf8(SettingsDir()).c_str());
        if (!job->warning.empty())
            ImGui::TextColored(C.amber, "%s", job->warning.c_str());
        ImGui::Spacing();
        ImGui::Checkbox("Desktop shortcut", &desktop);
        ImGui::Checkbox("Start menu shortcut", &startMenu);
        ImGui::Spacing();
        ImGui::TextColored(C.dim, "%s", "When dt-port is updated, the game rebuilds itself the first time you start it.");
        const int b = Footer({ { "Close", false }, { "Play", true } });
        if (b >= 0)
        {
            std::string err;
            if (!auto_)
                CreateShortcuts(desktop, startMenu, err);
            if (b == 1 && !auto_)
                LaunchGame();
            done = true;
        }
    }

    void DrawError()
    {
        Heading("This didn't work");
        ImGui::PushStyleColor(ImGuiCol_Text, C.bad);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        if (job && !job->build.log.empty() && ImGui::TreeNode("Details"))
        {
            ImGui::BeginChild("log", ImVec2(-1, 180 * scale), ImGuiChildFlags_Borders);
            std::lock_guard lock(job->build.mutex);
            for (const std::string& l : job->build.log)
                ImGui::TextUnformatted(l.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
        if (Footer({ { "Back", true } }) == 0)
        {
            rebuild = false;
            page = Page::Welcome;
        }
    }

    bool done = false;

    // Testing: DT_SETUP_AUTO=<dump> walks the pages with that dump (no shortcuts, no game start); the Footer of each page
    // presses its last button two seconds after the page appears.
    const char* auto_ = getenv("DT_SETUP_AUTO");
    Page shownPage = Page::Error;
    Clock::time_point pageSince = Clock::now();
    bool AutoPress()
    {
        return auto_ && page != Page::Working && page != Page::Error &&
               Clock::now() - pageSince > std::chrono::seconds(2);
    }

    void Frame()
    {
        if (page != shownPage)
            shownPage = page, pageSince = Clock::now();
        if (auto_ && page == Page::Welcome && Clock::now() - pageSince > std::chrono::seconds(2))
            StartCheck(fs::u8path(auto_));
        {
            std::lock_guard lock(pickMutex);
            if (picked)
            {
                const fs::path p = *picked;
                picked.reset();
                Picked(p);
            }
            if (target)
            {
                extractTo = *target;
                target.reset();
            }
        }
        if (page == Page::Checking && checked)
            AfterCheck();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("setup", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus);
        // Title bar: the game's name, then an amber rule.
        ImGui::PushFont(font, 34 * scale);
        ImGui::TextColored(C.text, "DEVIL'S THIRD");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14 * scale);
        ImGui::TextColored(C.amber, "PC SETUP  -  dt-port");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + 2 * scale),
            ImGui::GetColorU32(C.amber));
        ImGui::Dummy(ImVec2(0, 18 * scale));
        switch (page)
        {
        case Page::Welcome: DrawWelcome(); break;
        case Page::Checking:
            Heading("Checking your copy...");
            ImGui::TextColored(C.dim, "%s", "Reading the game's metadata and verifying its program.");
            ImGui::Spacing();
            Spinner();
            break;
        case Page::Checked: DrawChecked(); break;
        case Page::Saves: DrawSaves(); break;
        case Page::Working: DrawWorking(); break;
        case Page::Done: DrawDone(); break;
        case Page::Error: DrawError(); break;
        }
        // Footer note.
        ImGui::SetCursorPos(ImVec2(32 * scale, ImGui::GetWindowHeight() - 26 * scale));
        ImGui::TextColored(C.dim, "%s", "Not affiliated with Nintendo or Valhalla Game Studios. No game files are included.");
        ImGui::End();
    }
};

// Testing: DT_SETUP_SHOTS=<dir> saves the window as PNG when a page appears and every 20 s on the progress page.
void SavePng(SDL_Renderer* renderer, const std::string& path)
{
    SDL_Surface* raw = SDL_RenderReadPixels(renderer, nullptr);
    SDL_Surface* s = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(raw);
    if (!s)
        return;
    std::vector<uint8_t> rows;
    for (int y = 0; y < s->h; y++)
    {
        rows.push_back(0);
        const uint8_t* p = static_cast<const uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch);
        rows.insert(rows.end(), p, p + size_t(s->w) * 3);
    }
    uLongf zsize = compressBound(uLong(rows.size()));
    std::vector<uint8_t> z(zsize);
    compress2(z.data(), &zsize, rows.data(), uLong(rows.size()), 6);
    z.resize(zsize);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return SDL_DestroySurface(s);
    auto be = [](uint8_t* b, uint32_t v) { b[0] = uint8_t(v >> 24), b[1] = uint8_t(v >> 16), b[2] = uint8_t(v >> 8), b[3] = uint8_t(v); };
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t n) {
        uint8_t len[4], crcb[4];
        be(len, n);
        fwrite(len, 1, 4, f);
        fwrite(type, 1, 4, f);
        if (n)
            fwrite(data, 1, n, f);
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        crc = crc32(crc, data, n);
        be(crcb, uint32_t(crc));
        fwrite(crcb, 1, 4, f);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {};
    be(ihdr, uint32_t(s->w));
    be(ihdr + 4, uint32_t(s->h));
    ihdr[8] = 8, ihdr[9] = 2;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), uint32_t(z.size()));
    chunk("IEND", nullptr, 0);
    fclose(f);
    SDL_DestroySurface(s);
}

void Style(float scale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(32, 26);
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 9);
    s.FrameRounding = 3;
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = C.bg;
    c[ImGuiCol_ChildBg] = C.panel;
    c[ImGuiCol_Text] = C.text;
    c[ImGuiCol_TextDisabled] = C.dim;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.24f, 0.22f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.30f, 0.16f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.22f, 0.20f, 1);
    c[ImGuiCol_CheckMark] = C.amber;
    c[ImGuiCol_PlotHistogram] = C.amber;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.20f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.24f, 0.22f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.24f, 0.23f, 1);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.24f, 0.23f, 1);
}
} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // Command-line mode from a console: write there.
    if (argc > 1 && std::string(argv[1]).rfind("--", 0) == 0 && std::string(argv[1]) != "--rebuild" &&
        GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    if (argc > 1 && (std::string(argv[1]) == "--check" || std::string(argv[1]) == "--install" ||
                     std::string(argv[1]) == "--extract"))
        return CommandLine(argc, argv);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()), 1;
    App app;
    app.rebuild = argc > 1 && std::string(argv[1]) == "--rebuild";
    app.scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    app.window = SDL_CreateWindow("Devil's Third - PC setup", int(920 * app.scale), int(640 * app.scale), 0);
    SDL_Renderer* renderer = app.window ? SDL_CreateRenderer(app.window, nullptr) : nullptr;
    if (!renderer)
        return fprintf(stderr, "window: %s\n", SDL_GetError()), 1;
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    app.font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kOverlayFont), int(sizeof(kOverlayFont)),
        18 * app.scale, &cfg);
    Style(app.scale);
    ImGui_ImplSDL3_InitForSDLRenderer(app.window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    if (app.rebuild)
    {
        const fs::path game = InstalledGameDir(DefaultPaths().data);
        if (game.empty())
            app.rebuild = false;
        else
            app.StartCheck(game);
    }

    const char* shots = getenv("DT_SETUP_SHOTS");
    Page shotPage = Page::Error;
    Clock::time_point shotDue = Clock::time_point::max();
    int shotCount = 0;
    while (!app.done)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT)
            {
                if (app.job)
                    app.job->cancel = app.job->build.cancel = true;
                app.done = true;
            }
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && (app.page == Page::Welcome || app.page == Page::Error))
            {
                app.pickedForSave = false;
                app.StartCheck(fs::u8path(e.drop.data));
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.Frame();
        ImGui::Render();
        SDL_SetRenderDrawColorFloat(renderer, C.bg.x, C.bg.y, C.bg.z, 1);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (shots && app.page != shotPage)
        {
            shotPage = app.page;
            shotDue = Clock::now() + std::chrono::milliseconds(700);
        }
        if (shots && Clock::now() >= shotDue)
        {
            SavePng(renderer, std::string(shots) + "/setup_" + std::to_string(++shotCount) + ".png");
            shotDue = app.page == Page::Working ? Clock::now() + std::chrono::seconds(20) : Clock::time_point::max();
        }
        SDL_RenderPresent(renderer);
    }
    app.Join();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
