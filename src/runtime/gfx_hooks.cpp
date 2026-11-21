// Hooks into the game's Scaleform GFx integration (the menus): see src/notes/m6_plan.md, stage 4.
#include "game_settings.h"
#include "game_ui.h"
#include "assets/coalesced.h"
#include "assets/swf_patch.h"
#include "guest.h"
#include "hooks.h"
#include "log.h"
#include "trace.h"
#include "ue3.h"
#include "cafe/gx2.h"
#include "host/menu.h"
#include "host/button_text.h"
#include "host/prompts.h"
#include "host/swf_icons.h"
#include "host/input.h"
#include "gpu/renderer.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cctype>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdio>

namespace
{
constexpr uint32_t FGFxFileOpener_OpenFile = 0x03A35C80;  // FGFxFileOpener::OpenFile(const char* path, int, int)
constexpr uint32_t FGFxExternalInterface_Callback = 0x039C6E28;  // (Movie*, const char* method, const Value*, unsigned)
constexpr uint32_t DrawStringZ = 0x030A63C0;  // (FCanvas*, x, y, z (f1-f3), const wchar_t* text, UFont* font, ...)
constexpr uint32_t FGFxFile_ctor = 0x03A85C2C;  // FGFxFile::FGFxFile(const char* path, unsigned char* data, int size)
constexpr uint32_t UGFxMoviePlayer_Load = 0x03A5D744;   // (this, const FString& movie, unsigned): "UI_InGame.HUD" etc.
constexpr uint32_t UGFxMoviePlayer_Start = 0x03A608BC;  // (this, unsigned): opens the movie (loading it on first use)
constexpr uint32_t UGFxMoviePlayer_Close = 0x03A5D390;  // (this, unsigned unload)
constexpr uint32_t FFullScreenMovieBink_BinkDecodeFrame = 0x022E6B58;  // each frame of a full-screen Bink movie
constexpr uint32_t UGameEngine_Tick = 0x02B03DAC;                // UGameEngine::Tick(float)
constexpr uint32_t UWwiseAudio_Static_PlayUISound = 0x03ABE4B8;  // static (const FName& name, float)
constexpr uint32_t FName_FromString = 0x02147A40;                // FName::FName(const char*, EFindName)
constexpr uint32_t appDecryptData = 0x020011E8;                  // (unsigned char* data, unsigned long size), in place
// FString Localize(const TCHAR* section, const TCHAR* key, const TCHAR* package, const TCHAR* language, UBOOL optional):
// the result is written to the FString at r3 (data, count with the terminator, capacity; UTF-16 characters).
constexpr uint32_t Localize = 0x02101E34;
constexpr uint32_t appRealloc = 0x0204B4E4;                      // void* appRealloc(void*, unsigned long size, unsigned long align)

// Each distinct name once, so the log shows which movies load and what they call without flooding.
void LogOnce(const char* what, const std::string& name)
{
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard lock(m);
    if (seen.insert(std::string(what) + name).second)
        dtlog::Important("[gfx] %s %s\n", what, name.c_str());
}

std::string ReadFString(uint32_t f)  // TArray<wchar_t>: Data, Num (with the terminator), Max
{
    std::string out;
    const uint32_t data = guest::Read32(f), count = guest::Read32(f + 4);
    for (uint32_t i = 0; i + 1 < count && i < 256; i++)
        out += char(guest::Read16(data + i * 2));
    return out;
}

std::atomic<uint64_t> gEngineTicks{ 0 };  // UGameEngine::Tick calls so far (the Game Thread's frames)

// Canvas text: the text drawn in the engine's SkipFont is logged once each, with its characters' codes.
PPC_FUNC(CanvasText)
{
    static std::set<std::string> seen;
    static uint32_t skipFont = 0;
    const uint32_t font = ctx.r[5].u32;
    // DT_LOG_TEXT (environment, testing): every distinct canvas string once, with its font.
    static const bool logText = getenv("DT_LOG_TEXT") != nullptr;
    if (logText)
    {
        static std::set<std::string> texts;
        std::string text;
        for (uint32_t i = 0; i < 120; i++)
        {
            const uint16_t c = guest::Read16(ctx.r[4].u32 + i * 2);
            if (!c)
                break;
            if (c >= 32 && c < 127)
                text += char(c);
            else
            {
                char hex[12];
                snprintf(hex, sizeof hex, "\\u%04X", c);
                text += hex;
            }
        }
        if (texts.size() < 300 && texts.insert(text).second)
            dtlog::Important("[test] canvas text (%s): \"%s\"\n", ue3::PathName(font).c_str(), text.c_str());
        static uint32_t loggedFont = 0;
        if (font != loggedFont && ue3::PathName(font).find("SubtitleFont") != std::string::npos)
        {
            loggedFont = font;
            // FFontCharacter: StartU, StartV, USize, VSize (int), TextureIndex (byte, padded), VerticalOffset: 24 bytes.
            const auto chars = ue3::FindProperty(font, "Characters");
            const auto remapped = ue3::FindProperty(font, "IsRemapped");
            const uint32_t data = chars ? guest::Read32(chars->address) : 0, n = chars ? guest::Read32(chars->address + 4) : 0;
            dtlog::Important("[test] SubtitleFont: %u characters, IsRemapped %d\n", n, remapped ? int(guest::Read32(remapped->address)) : -1);
            for (uint32_t c : { 32u, 95u, 65u, 63u, 33u })
                if (c < n)
                    dtlog::Important("[test]   char %u: u %d v %d w %d h %d tex %u voff %d\n", c, int32_t(guest::Read32(data + c * 24)),
                        int32_t(guest::Read32(data + c * 24 + 4)), int32_t(guest::Read32(data + c * 24 + 8)),
                        int32_t(guest::Read32(data + c * 24 + 12)), guest::Read8(data + c * 24 + 16), int32_t(guest::Read32(data + c * 24 + 20)));
        }
        static int shots = 0;
        if (text.size() > 20 && shots < 3 && cafe::gx2::FrameCount() > 2500 && ue3::PathName(font).find("SubtitleFont") != std::string::npos)
            shots++, gpu::renderer::RequestScreenshot();
    }
    if (font && font != skipFont && !skipFont && ue3::PathName(font).find("SkipFont") != std::string::npos)
    {
        skipFont = font;
        // Which characters it has: Characters (TArray<FFontCharacter>), IsRemapped, CharRemap (TMap<WORD,WORD>).
        const auto chars = ue3::FindProperty(font, "Characters");
        const auto remapped = ue3::GetBool(font, "IsRemapped");
        const auto remap = ue3::FindProperty(font, "CharRemap");
        dtlog::Important("[gfx] SkipFont: %u characters, remapped %d\n", chars ? guest::Read32(chars->address + 4) : 0,
            remapped ? int(*remapped) : -1);
        if (remap && remapped && *remapped)
        {
            // TMap's pairs live in a TSparseArray: element data, then (key, value) per element, 12 bytes each here.
            const uint32_t data = guest::Read32(remap->address), n = guest::Read32(remap->address + 4);
            std::string keys;
            for (uint32_t i = 0; i < n && i < 400; i++)
            {
                char hex[8];
                snprintf(hex, sizeof hex, " %04X", guest::Read16(data + i * 12));
                keys += hex;
            }
            dtlog::Important("[gfx] SkipFont remap keys (%u):%s\n", n, keys.c_str());
        }
    }
    if (font && font == skipFont)
    {
        std::string codes;
        for (uint32_t i = 0; i < 32; i++)
        {
            const uint16_t c = guest::Read16(ctx.r[4].u32 + i * 2);
            if (!c)
                break;
            char hex[8];
            snprintf(hex, sizeof hex, " %04X", c);
            codes += hex;
        }
        if (seen.insert(codes).second)
            dtlog::Important("[gfx] SkipFont text:%s\n", codes.c_str());
        // The skip prompt: Wii U A (U+24B6, the font's circled A) and " SKIP". On PlayStation and the keyboard the A is
        // drawn as a space and the overlay draws the device's confirm button in its place (host::menu).
        if (guest::Read16(ctx.r[4].u32) == 0x24B6)
        {
            const float yScale = float(ctx.f[5].ps0);
            host::menu::NoteSkipPrompt(float(ctx.f[1].ps0), float(ctx.f[2].ps0), 18.0f * (yScale > 0 ? yScale : 1.0f));
            if (host::menu::SkipGlyphReplaced())
            {
                static const uint32_t copy = guest::SystemAlloc(128, 4);
                uint32_t n = 0;
                for (; n < 60 && guest::Read16(ctx.r[4].u32 + n * 2); n++)
                    guest::Write16(copy + n * 2, guest::Read16(ctx.r[4].u32 + n * 2));
                guest::Write16(copy + n * 2, 0);
                guest::Write16(copy, 0x0020);
                ctx.r[4].u32 = copy;
            }
        }
    }
    // Subtitles (SubtitleFont): the game's text puts a space before ? ! : ; ("back-up ? !"); drawn without it.
    {
        static uint32_t subtitleFont = 0;
        if (font && !subtitleFont && ue3::PathName(font).find("SubtitleFont") != std::string::npos)
            subtitleFont = font;
        if (font && font == subtitleFont)
        {
            static const uint32_t copy = guest::SystemAlloc(1024, 4);
            uint32_t n = 0, out = 0;
            bool changed = false;
            for (; n < 500 && guest::Read16(ctx.r[4].u32 + n * 2); n++)
            {
                const uint16_t c = guest::Read16(ctx.r[4].u32 + n * 2), next = guest::Read16(ctx.r[4].u32 + (n + 1) * 2);
                if (c == ' ' && (next == '?' || next == '!' || next == ':' || next == ';') && out > 0)
                {
                    changed = true;
                    continue;
                }
                guest::Write16(copy + out++ * 2, c);
            }
            guest::Write16(copy + out * 2, 0);
            if (changed)
                ctx.r[4].u32 = copy;
            // Sharp subtitles (host::menu): the game draws the line with its colour's alpha at 0 (nothing shows, and it
            // still measures it), and the overlay draws it at the window's resolution.
            if (host::menu::SharpSubtitles())
            {
                static float cellHeight = 0;  // the bitmap font's cell height (the tallest character's VSize)
                if (!cellHeight)
                {
                    const auto chars = ue3::FindProperty(font, "Characters");
                    const uint32_t data = chars ? guest::Read32(chars->address) : 0, count = chars ? guest::Read32(chars->address + 4) : 0;
                    for (uint32_t i = 0; i < count && i < 8000; i++)
                        cellHeight = std::max(cellHeight, float(int32_t(guest::Read32(data + i * 24 + 12))));
                    if (!cellHeight)
                        cellHeight = 24;
                    dtlog::Important("[gfx] subtitles: cell height %.0f px\n", cellHeight);
                }
                const float x = float(ctx.f[1].ps0), y = float(ctx.f[2].ps0), yScale = float(ctx.f[5].ps0);
                const uint32_t colour = ctx.r[6].u32;
                float rgba[4];
                for (int i = 0; i < 4; i++)
                {
                    const uint32_t bits = guest::Read32(colour + i * 4);
                    memcpy(&rgba[i], &bits, 4);
                }
                std::string text;
                const uint32_t textAt = ctx.r[4].u32;
                for (uint32_t i = 0; i < 500; i++)
                {
                    uint32_t c = guest::Read16(textAt + i * 2);
                    if (!c)
                        break;
                    if (c >= 0xD800 && c < 0xDC00)  // a surrogate pair
                        c = 0x10000 + ((c - 0xD800) << 10) + (guest::Read16(textAt + ++i * 2) - 0xDC00);
                    if (c < 0x80)
                        text += char(c);
                    else if (c < 0x800)
                        text += char(0xC0 | (c >> 6)), text += char(0x80 | (c & 63));
                    else if (c < 0x10000)
                        text += char(0xE0 | (c >> 12)), text += char(0x80 | ((c >> 6) & 63)), text += char(0x80 | (c & 63));
                    else
                        text += char(0xF0 | (c >> 18)), text += char(0x80 | ((c >> 12) & 63)), text += char(0x80 | ((c >> 6) & 63)),
                            text += char(0x80 | (c & 63));
                }
                static const uint32_t clear = guest::SystemAlloc(16, 4);
                for (int i = 0; i < 3; i++)
                    guest::Write32(clear + i * 4, guest::Read32(colour + i * 4));
                guest::Write32(clear + 12, 0);
                ctx.r[6].u32 = clear;
                hooks::Original(DrawStringZ)(ctx, base);
                const float width = float(int32_t(ctx.r[3].u32));
                // (linear to sRGB, as the canvas shows it)
                auto srgb = [](float v) { return uint32_t(std::clamp(std::pow(std::clamp(v, 0.0f, 1.0f), 1 / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f)); };
                const uint32_t packed = srgb(rgba[0]) << 24 | srgb(rgba[1]) << 16 | srgb(rgba[2]) << 8 |
                    uint32_t(std::clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f);
                static int logged = 0;
                if (logged < 12 && getenv("DT_LOG_TEXT"))
                    logged++, dtlog::Important("[test] subtitle at %.1f,%.1f scale %.2f width %.0f colour %08X: %s\n", x, y, yScale,
                        width, packed, text.c_str());
                host::menu::NoteSubtitle(gEngineTicks.load(std::memory_order_relaxed), x, y, cellHeight * (yScale > 0 ? yScale : 1.0f), width, packed, std::move(text));
                return;
            }
        }
    }
    hooks::Original(DrawStringZ)(ctx, base);
}

PPC_FUNC(OpenFile)
{
    LogOnce("opens", guest::ReadString(ctx.r[4].u32, 256));
    hooks::Original(FGFxFileOpener_OpenFile)(ctx, base);
}

PPC_FUNC(ExternalInterfaceCallback)
{
    const std::string method = guest::ReadString(ctx.r[5].u32, 128);
    LogOnce("ExternalInterface.call", method);
    // Our entries in the game's menus (assets/swf_patch.*): not passed on to UnrealScript.
    if (method == "DtPortSettings")
        return host::menu::OpenFromGameMenu();
    if (method == "DtPortFocus")
        return host::menu::PlayCursorSound();
    // The menus' mouse pointer clicked a button (swf_patch::AddMenuMouse): the menus' confirm, once per click; or an
    // arrow beside a choice (the weapon loadout's "<" ">"): the D-pad that way. Held as long as a key press.
    // (DtPortPress<B|X|Y|Plus|Minus>: a click on that button's prompt icon)
    if (method == "DtPortClick" || method == "DtPortLeft" || method == "DtPortRight" || method.rfind("DtPortPress", 0) == 0)
    {
        static uint64_t last = 0;
        const uint64_t now = cafe::gx2::FrameCount();
        using namespace host::input;
        const std::string p = method.rfind("DtPortPress", 0) == 0 ? method.substr(11) : std::string();
        const uint32_t button = method == "DtPortLeft" ? VpadLeft : method == "DtPortRight" ? VpadRight : p == "B" ? VpadB
                                : p == "X" ? VpadX : p == "Y" ? VpadY : p == "Plus" ? VpadPlus : p == "Minus" ? VpadMinus : VpadA;
        if (now > last + 6)
        {
            last = now, host::input::Inject(button, 8);
            dtlog::Important("[ui] %s at frame %llu\n", method.c_str(), (unsigned long long)now);
        }
        return;
    }
    hooks::Original(FGFxExternalInterface_Callback)(ctx, base);
}

// The movies we patch, in memory as the game reads them from its packages: "PC Settings" in the pause menu and the
// title menu. The patched copies stay allocated for the whole run (FGFxFile keeps the pointer).
PPC_FUNC(GFxFileCtor)
{
    static const struct
    {
        const char* path;  // part of the lower-cased movie path
        bool (*patch)(std::vector<uint8_t>&, const std::string&);
        const char* what;
    } patches[] = {
        // The prompt library: variants of its Wii U button pictures for other controllers and the keyboard.
        { "ui_common/buttonlibrary", [](std::vector<uint8_t>& m, const std::string&) { return assets::swf_patch::AddPromptVariants(m); },
            "the button prompts" },
        // The title's "START" plate says "Press any key/button" (button_text) without the key icon: the text takes the
        // icon's place and a smaller font.
        { "ui_frontend/titlemenu", [](std::vector<uint8_t>& m, const std::string&) {
             const bool a = assets::swf_patch::ResizeEditText(m, "MenuItem_title_Start", "textField", 48, 0, 0.72f, 3);
             const bool b = assets::swf_patch::ResizeEditText(m, "MenuItem_title_Start_pc", "textField", 48, 0, 0.72f, 3);
             // (the divider that stood between the key icon and the text: a full-screen rectangle squeezed to a line)
             // (both: a second, thinner one sits at depth 10)
             const bool c = assets::swf_patch::RemoveDepth(m, "MenuItem_title_Start", 8);
             assets::swf_patch::RemoveDepth(m, "MenuItem_title_Start", 10);
             return a || b || c;
         },
            "the title's press-START text" },
        // The game's Options: its footer draws its own "+ Save" and "Y Default" icons.
        { "ui_ingame/optionmenu", [](std::vector<uint8_t>& m, const std::string&) {
             return assets::swf_patch::UseLibraryPrompts(m, { { "save_label", "StartContainer" }, { "default_settings", "YButtonContainer" } });
         }, "the footer's button prompts" },
        // The chainsaw is X-gear: its icon (HUD and menus) in the HUD's X-gear light blue, as the other X-gear weapons show.
        { "ui_common/weaponlibrary", [](std::vector<uint8_t>& m, const std::string&) {
             return assets::swf_patch::TintSprite(m, "weapon_chainsaw_set", 31, 0, 82, 0, 86, 0);
         }, "the chainsaw's X-gear colour" },
        // The Score Attack loadout's slots turn light blue for X-gear weapons (ExtraWeaponsTick sets _global.dtXGear).
        { "ui_frontend/soloplay_menu", [](std::vector<uint8_t>& m, const std::string&) { return assets::swf_patch::AddXGearTint(m); },
            "the X-gear slot colour" },
        // (AddPauseMenuButton and AddTitleMenuButton add separate "PC Settings" entries when listed here; the game's own
        // Options entries open the PC page instead, see the ProcessEvent hook.)
    };
    std::string path = guest::ReadString(ctx.r[4].u32, 256);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const uint32_t data = ctx.r[5].u32, size = ctx.r[6].u32;
    // Menus (not the HUD's parts, fonts or libraries) also get the mouse pointer's script (AddMenuMouse).
    auto isMenu = [&] {
        for (const char* lib : { "library", "fonts", "/hud", "hudtext", "hudeventlog", "decorator", "minimap", "visor", "wait",
                 "drc_exchange", "monitor_living", "mousecursor", "subviewui" })
            if (path.find(lib) != std::string::npos)
                return false;
        for (const char* menu : { "ui_frontend/", "ui_ingame/", "ui_ingamesingle/", "ui_ingamemulti/", "ui_common/messagedialog" })
            if (path.find(menu) != std::string::npos)
                return true;
        return false;
    };
    if (!data || !size)
        return hooks::Original(FGFxFile_ctor)(ctx, base);
    static std::mutex m;
    static std::map<std::string, std::pair<uint32_t, uint32_t>> patched;  // path -> guest copy (0: none), size
    std::lock_guard lock(m);
    auto it = patched.find(path);
    if (it == patched.end())
    {
        std::vector<uint8_t> movie(base + data, base + data + size);
        if (path.find("ui_common/weaponlibrary") != std::string::npos)
            host::swf_icons::SetLibrary(movie.data(), movie.size());  // (the loadout picker's weapon pictures)
        bool changed = false;
        for (const auto& p : patches)
        {
            if (!p.path || path.find(p.path) == std::string::npos)
                continue;
            if (p.patch(movie, "PC Settings"))
                changed = true, dtlog::Important("[gfx] %s: patched (%s)\n", path.c_str(), p.what);
            else
                dtlog::Important("[gfx] %s: not laid out as expected for %s; left as it was\n", path.c_str(), p.what);
        }
        if (isMenu() && assets::swf_patch::AddMenuMouse(movie))
            changed = true, dtlog::Important("[gfx] %s: patched (the mouse pointer)\n", path.c_str());
        std::pair<uint32_t, uint32_t> copy{ 0, 0 };
        if (changed)
        {
            copy = { guest::SystemAlloc(uint32_t(movie.size())), uint32_t(movie.size()) };
            memcpy(base + copy.first, movie.data(), movie.size());
        }
        it = patched.emplace(path, copy).first;
    }
    if (it->second.first)
        ctx.r[5].u32 = it->second.first, ctx.r[6].u32 = it->second.second;
    hooks::Original(FGFxFile_ctor)(ctx, base);
}

// ---- which of the game's movies are open ----
std::mutex gMoviesMutex;
std::map<uint32_t, std::string> gMovieOf;  // movie player -> its movie, lower case ("ui_ingame.pausemenu_new")
std::set<uint32_t> gOpen;                   // started and not closed
std::set<uint32_t> gPromptsDue;             // started since the prompt variables were last set
uint64_t gLastBinkFrame = 0;                // gx2 frame of the last decoded Bink frame (0: none yet)

bool IsPauseMenu(const std::string& movie) { return movie.find("pausemenu") != std::string::npos; }
// Movies that stay open during play: the HUD's parts, the GamePad screen's, the hidden wait indicator.
bool IsPlayMovie(const std::string& movie)
{
    for (const char* part : { "hud", "decorator", "minimap", "visor", "wait_text", "drc_exchange", "monitor_living", "mousecursor" })
        if (movie.find(part) != std::string::npos)
            return true;
    return false;
}

void Opened(uint32_t player, bool open)
{
    bool pauseMenu = false;
    {
        std::lock_guard lock(gMoviesMutex);
        const bool changed = open ? gOpen.insert(player).second : gOpen.erase(player) > 0;
        if (!changed)
            return;
        auto it = gMovieOf.find(player);
        pauseMenu = it != gMovieOf.end() && IsPauseMenu(it->second);
    }
    if (pauseMenu)
        host::menu::OnPauseMenu(open);
    if (open)
    {
        std::lock_guard lock(gMoviesMutex);
        gPromptsDue.insert(player);  // its prompt variables, next tick
    }
}

PPC_FUNC(MovieLoad)
{
    std::string movie = ReadFString(ctx.r[4].u32);
    std::transform(movie.begin(), movie.end(), movie.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    {
        std::lock_guard lock(gMoviesMutex);
        gMovieOf[ctx.r[3].u32] = movie;
    }
    hooks::Original(UGFxMoviePlayer_Load)(ctx, base);
}

PPC_FUNC(MovieStart)
{
    const uint32_t player = ctx.r[3].u32;
    hooks::Original(UGFxMoviePlayer_Start)(ctx, base);  // (loads the movie the first time, so its name is known after)
    Opened(player, true);
}

PPC_FUNC(MovieClose)
{
    const uint32_t player = ctx.r[3].u32;
    Opened(player, false);
    hooks::Original(UGFxMoviePlayer_Close)(ctx, base);
}

PPC_FUNC(BinkDecodeFrame)
{
    gLastBinkFrame = cafe::gx2::FrameCount();
    hooks::Original(FFullScreenMovieBink_BinkDecodeFrame)(ctx, base);
}

// ---- the game's Options entries open the PC page ----
// The pause menu's and the title menu's Options handlers, as UnrealScript runs them for a click
// (D3UIWidget_PauseMenu.OnOptionButtonClik, D3FrontEnd_TitleMenu.OnOptionClick).
// - Pause menu: taken instead of run; the call is kept so the page's "Original Options" row can run it after all.
// - Title menu: run, and the page opens over the game's Options. The title disables its input on the click and only
//   re-enables it (with its focus and highlight) when its Options close (OnCloseOption -> SetMenuStage(9)), so closing
//   our page closes the game's Options the same way; "Original Options" just leaves them open.
constexpr uint32_t UObject_ProcessEvent = 0x02078FC4;  // (this, UFunction*, void* parms, void* result)
std::atomic<uint32_t> gOptionName1{ 0 }, gOptionName2{ 0 };
std::atomic<bool> gPassOptions{ false };
struct KeptCall
{
    uint32_t object = 0, function = 0, parms = 0;
};
std::mutex gKeptMutex;
KeptCall gKept;
std::atomic<bool> gRunKept{ false };
std::atomic<uint32_t> gTitleUnder{ 0 };  // the title menu whose Options are open under the page
std::atomic<bool> gCloseTitleOptions{ false };
// The Score Attack loadout's Confirm (D3FrontEnd_SingleplayMenu.OnMissionWeaponEquipClick) with an X-gear weapon in the
// Primary or Secondary slot (gXGearBits, ExtraWeaponsTick): kept, and run after the player accepts a warning.
std::atomic<uint32_t> gEquipName{ 0 }, gXGearBits{ 0 };
std::atomic<bool> gPassEquip{ false }, gRunEquip{ false };
std::atomic<uint32_t> gSPMenu{ 0 };  // the single-player menu (its loadout screen), for the ProcessEvent hook
std::atomic<bool> gSlotFocused{ false }, gOpenPicker{ false };  // a loadout slot has the focus; left/right asked for the picker
std::atomic<uint32_t> gConfirmedBits{ 0 };  // gXGearBits when the loadout was confirmed (bit 4: the chainsaw as Melee)
KeptCall gKeptEquip;

// D3HUD.PostBeginPlay makes the game's PC mouse cursor (D3GfxMovieMouseCursorHud) only when
// WorldInfo.IsConsoleBuild() is false: while it runs, IsConsoleBuild answers false (IsConsoleBuildHook).
std::atomic<int> gHudBeginPlay{ 0 };
constexpr uint32_t AWorldInfo_execIsConsoleBuild = 0x02A7BDE4;  // (this, FFrame&, void* result)
PPC_FUNC(IsConsoleBuildHook)
{
    const uint32_t result = ctx.r[5].u32;
    hooks::Original(AWorldInfo_execIsConsoleBuild)(ctx, base);
    if (gHudBeginPlay > 0 && result)
        guest::Write32(result, 0);
}

// The engine's exit request (the PC title menu's Exit, which keyboard mode shows): dt-port quits as when the window is
// closed. (The Wii U path waited for the system and left the window hanging.)
constexpr uint32_t appRequestExit = 0x03B0406C;  // void appRequestExit(UBOOL force)
PPC_FUNC(RequestExitHook)
{
    dtlog::Important("[dt-port] the game asked to exit\n");
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
    hooks::Original(appRequestExit)(ctx, base);
}

PPC_FUNC(ProcessEvent)
{
    const uint32_t function = ctx.r[4].u32;
    const uint32_t name = function ? guest::Read32(function + 0x2C) : 0;  // UObject::Name
    // DT_LOG_EVENTS=<text> (testing): logs every event whose function path contains the text (case-sensitive).
    static const char* const logEvents = getenv("DT_LOG_EVENTS");
    if (logEvents && function)
    {
        static std::map<uint32_t, bool> matches;  // function -> path matches
        auto it = matches.find(function);
        if (it == matches.end())
            it = matches.emplace(function, ue3::PathName(function).find(logEvents) != std::string::npos).first;
        if (it->second)
            dtlog::Important("[events] frame %llu %s\n", (unsigned long long)cafe::gx2::FrameCount(), ue3::PathName(function).c_str());
    }
    static uint32_t postBeginPlay = 0;
    if (!postBeginPlay)
        postBeginPlay = ue3::FindNameIndex("PostBeginPlay");
    if (name && name == postBeginPlay && ue3::IsA(ctx.r[3].u32, "D3HUD"))
    {
        const uint32_t hud = ctx.r[3].u32;
        gHudBeginPlay++;
        hooks::Original(UObject_ProcessEvent)(ctx, base);
        gHudBeginPlay--;
        dtlog::Important("[ui] %s began play (the game's mouse cursor: %s)\n", ue3::ClassName(hud).c_str(),
            ue3::GetObject(hud, "hudMouseCursor").value_or(0) ? "made" : "none");
        return;
    }
    if (function && ctx.r[3].u32 && ctx.r[3].u32 == gSPMenu)
    {
        // The Score Attack loadout: which slot has the focus, and left / right on it open the picker (LoadoutPickerTick)
        // instead of stepping through the weapons.
        static uint32_t slotFocus = 0, filter = 0, left = 0, right = 0;
        if (!slotFocus)
            slotFocus = ue3::FindNameIndex("OnMissionWeaponSlotFocusIn"), filter = ue3::FindNameIndex("FilterButtonInput"),
            left = ue3::FindNameIndex("XboxTypeS_DPad_Left"), right = ue3::FindNameIndex("XboxTypeS_DPad_Right");
        static std::map<uint32_t, bool> focusEvents;  // function -> a "...FocusIn" event
        if (name == slotFocus)
            gSlotFocused = true;
        else if (name != filter)
        {
            auto it = focusEvents.find(function);
            if (it == focusEvents.end())
                it = focusEvents.emplace(function, ue3::PathName(function).find("FocusIn") != std::string::npos).first;
            if (it->second)
                gSlotFocused = false;
        }
        static const bool on = settings::GetBool("gameplay.extra_weapons", true) && !getenv("DT_TEST_NO_PICKER");  // (testing: step)
        if (on && name && name == filter && gSlotFocused && ctx.r[5].u32)
        {
            struct Offsets
            {
                uint32_t button = ~0u, event = ~0u, result = ~0u, resultMask = 0;
            };
            static std::map<uint32_t, Offsets> offsets;
            auto it = offsets.find(function);
            if (it == offsets.end())
            {
                Offsets o;
                for (uint32_t q = guest::Read32(function + 0x4C); q; q = guest::Read32(q + 0x40))
                {
                    std::string pn = ue3::PathName(q);
                    pn = pn.substr(pn.find_last_of(".:") + 1);
                    if (pn == "ButtonName")
                        o.button = guest::Read32(q + 0x5C);
                    else if (pn == "InputEvent")
                        o.event = guest::Read32(q + 0x5C);
                    else if (pn == "ReturnValue")
                        o.result = guest::Read32(q + 0x5C), o.resultMask = guest::Read32(q + 0x70);
                }
                it = offsets.emplace(function, o).first;
            }
            const Offsets& o = it->second;
            const uint32_t parms = ctx.r[5].u32;
            if (o.button != ~0u && o.event != ~0u)
            {
                const uint32_t button = guest::Read32(parms + o.button);
                if (button && (button == left || button == right))
                {
                    // (no stepping: the slot's arrows are hidden and A, Enter or a click opens the picker)
                    if (o.result != ~0u && o.resultMask)
                        guest::Write32(parms + o.result, guest::Read32(parms + o.result) | o.resultMask);
                    return;
                }
            }
        }
    }
    if (name && name == gEquipName)
        gConfirmedBits = gXGearBits.load();
    static const bool acceptXGear = getenv("DT_TEST_ACCEPT_XGEAR") != nullptr;  // (testing: no warning)
    if (name && name == gEquipName && gXGearBits && !gPassEquip && !acceptXGear && ue3::IsA(ctx.r[3].u32, "D3FrontEnd_SingleplayMenu"))
    {
        static uint32_t copy = guest::SystemAlloc(0x400);
        const uint32_t size = std::min<uint32_t>(guest::Read16(function + 0x8A), 0x400);  // UFunction::ParmsSize
        if (ctx.r[5].u32 && size)
            memcpy(base + copy, base + ctx.r[5].u32, size);
        {
            std::lock_guard lock(gKeptMutex);
            gKeptEquip = { ctx.r[3].u32, function, copy };
        }
        dtlog::Important("[weapons] Confirm with X-gear selected: warning first\n");
        host::menu::AskOverGame("X-Gear Weapons",
            { "X-Gear weapons are special weapons and do not work like normal ones:",
                "they cannot be swapped for weapons found on the ground, and they",
                "are lost when a checkpoint is reloaded. They may cause issues.", "Continue with this loadout?" },
            [] { gRunEquip = true; });
        return;
    }
    if (name && (name == gOptionName1 || name == gOptionName2) && !gPassOptions)
    {
        if (name == gOptionName2)  // the title menu's
        {
            const uint32_t title = ctx.r[3].u32;
            hooks::Original(UObject_ProcessEvent)(ctx, base);
            gTitleUnder = title;
            dtlog::Important("[options] the game's Options opened the PC page over them (title menu)\n");
            host::menu::OpenFromGameOptions();
            return;
        }
        static uint32_t copy = guest::SystemAlloc(0x400);
        const uint32_t size = std::min<uint32_t>(guest::Read16(function + 0x8A), 0x400);  // UFunction::ParmsSize
        if (ctx.r[5].u32 && size)
            memcpy(base + copy, base + ctx.r[5].u32, size);
        {
            std::lock_guard lock(gKeptMutex);
            gKept = { ctx.r[3].u32, function, copy };
        }
        dtlog::Important("[options] the game's Options opened the PC page (%s)\n", ue3::PathName(function).c_str());
        host::menu::OpenFromGameOptions();
        return;
    }
    hooks::Original(UObject_ProcessEvent)(ctx, base);
}

// Game Thread: the names to look for once they exist, and the kept call when the page asks for it.
void OptionsTick()
{
    if (!gOptionName1)
        gOptionName1 = ue3::FindNameIndex("OnOptionButtonClik"), gOptionName2 = ue3::FindNameIndex("OnOptionClick");
    // Closing the page closes the game's Options under it (OnCloseOption). If that came while the title was still in its
    // Options opening sequence (NowMenuStage 6, E_TITLE_MS_OPTION_OPEN_SEQ), the sequence finished after it and left the
    // title at 7 (OPTION_OPEN) with its input off: nothing on the title responded. So for 5 s after the page closes,
    // an Options stage (6, 7) gets OnCloseOption again, twice a second, until the title is back.
    static uint32_t closing = 0;
    static int closingTicks = 0;
    if (gCloseTitleOptions.exchange(false))
        if (const uint32_t title = gTitleUnder.exchange(0))
        {
            const auto stage = ue3::FindProperty(title, "NowMenuStage");
            dtlog::Important("[options] closing the title's Options (stage %d)\n", stage ? int(guest::Read8(stage->address)) : -1);
            ue3::Call(title, "OnCloseOption");
            closing = title, closingTicks = 0;
        }
    if (closing && ++closingTicks % 30 == 0)
    {
        const auto stage = ue3::FindProperty(closing, "NowMenuStage");
        const int now = stage ? guest::Read8(stage->address) : -1;
        if (now == 6 || now == 7)
        {
            dtlog::Important("[options] the title is still in its Options (stage %d): closing them again\n", now);
            ue3::Call(closing, "OnCloseOption");
        }
        if ((now != 6 && now != 7 && now != 8 && now != 9) || closingTicks > 300)
            closing = 0;
    }
    if (!gEquipName)
        gEquipName = ue3::FindNameIndex("OnMissionWeaponEquipClick");
    if (gRunEquip.exchange(false))
    {
        KeptCall k;
        {
            std::lock_guard lock(gKeptMutex);
            k = gKeptEquip;
        }
        if (k.object)
        {
            dtlog::Important("[weapons] X-gear warning accepted: confirming the loadout\n");
            gPassEquip = true;
            guest::Call(UObject_ProcessEvent, { k.object, k.function, k.parms, 0 });
            gPassEquip = false;
        }
    }
    if (gRunKept.exchange(false))
    {
        KeptCall k;
        {
            std::lock_guard lock(gKeptMutex);
            k = gKept;
        }
        if (k.object)
        {
            gPassOptions = true;
            guest::Call(UObject_ProcessEvent, { k.object, k.function, k.parms, 0 });
            gPassOptions = false;
        }
    }
}

// ---- button prompts: the variables the patched prompt library reads (host::prompts::GameIcons) ----
// Set in every open movie when the device in use, the bindings or the layout change, and in each movie as it starts.
void PromptsTick()
{
    static std::string last;
    static int generation = 0;
    const auto icons = host::prompts::GameIcons(host::input::ActiveDevice());
    std::string now;
    for (const auto& i : icons)
        now += std::string(i.button) + ":" + std::to_string(i.kind) + ":" + i.text + ";";
    std::set<uint32_t> players;
    {
        std::lock_guard lock(gMoviesMutex);
        if (now != last)
            players = gOpen, generation++;
        else
            players = gPromptsDue;
        gPromptsDue.clear();
    }
    if (now != last)
        dtlog::Important("[prompts] %s\n", now.c_str());
    last = now;
    std::set<uint32_t> titlePlayers;
    {
        std::lock_guard lock(gMoviesMutex);
        for (uint32_t p : players)
            if (auto it = gMovieOf.find(p); it != gMovieOf.end() && it->second.find("titlemenu") != std::string::npos)
                titlePlayers.insert(p);
    }
    for (uint32_t player : players)
    {
        for (const auto& i : icons)
        {
            const std::string b = i.button;
            // The title's press-START prompt says "Press any key/button": no button icon beside it (kind 99 shows none).
            const bool hidden = b == "Plus" && titlePlayers.count(player);
            ue3::Call(player, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtK_" + b), hidden ? 99.0f : float(i.kind) });
            // The library's own text fields name the game's font in HTML; so do these.
            // The prompt labels' bold font (Mechanical-NewRodinUB): $NormalFont_en is not in the game's font library and fell back
            // to a thin default face. A tab separates a second cap's text (dtT2_: the Mash prompt's right key).
            auto html = [](const std::string& in) {
                std::string text;
                for (char c : in)
                    text += c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : c == '&' ? std::string("&amp;") : std::string(1, c);
                return "<p align=\"center\"><font face=\"$SpecialFont_en\" color=\"#0A0A0A\">" + text + "</font></p>";
            };
            const size_t tab = i.text.find('\t');
            ue3::Call(player, "SetVariableString", { ue3::Arg::MakeString("_global.dtT_" + b), ue3::Arg::MakeString(html(i.text.substr(0, tab))) });
            if (tab != std::string::npos)
                ue3::Call(player, "SetVariableString", { ue3::Arg::MakeString("_global.dtT2_" + b), ue3::Arg::MakeString(html(i.text.substr(tab + 1))) });
            ue3::Call(player, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtW_" + b), float(i.capWidth) });
            ue3::Call(player, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtS_" + b), float(i.textScale) });
        }
        ue3::Call(player, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtPG"), float(generation) });
        if (getenv("DT_LOG_PROMPT_VARS"))  // (testing) what a movie holds for the Mash prompt
            for (const char* v : { "_global.dtT_Mash", "_global.dtT2_Mash", "_global.dtK_Mash" })
            {
                std::string text;
                if (const uint32_t str = ue3::Call(player, "GetVariableString", { ue3::Arg::MakeString(v) }).value_or(0))
                    for (uint32_t k = 0; k < 300 && guest::Read16(str + k * 2); k++)
                        text += char(guest::Read16(str + k * 2));
                dtlog::Important("[test] %s %s = %s\n", ue3::ClassName(player).c_str(), v, text.c_str());
            }
    }
}

// In-engine cutscenes (Matinee): the local player controller's bCinematicMode, read every few frames. game_ui counts
// them as Busy, so their skip button follows the menu convention and the settings page waits for them.
std::atomic<bool> gCinematic{ false };

void CinematicTick(uint32_t engine)
{
    static int ticks = 0;
    if (++ticks < 5)
        return;
    ticks = 0;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const bool cinematic = pc && ue3::GetBool(pc, "bCinematicMode").value_or(false);
    if (gCinematic.exchange(cinematic) != cinematic)
        dtlog::Important("[ui] cinematic mode %s\n", cinematic ? "on" : "off");
}

// Mouse look speed: with the keyboard and mouse in gameplay, the game's turn speeds are host::input::MouseLookBoost times
// larger (the mouse's stick values are that much smaller, so slow motion turns as before and fast motion is no longer
// capped at full tilt), and the turn-rate smoothing (RotationRateInterpFactor 0.2, which made the mouse lag) is off.
// The game's own values come back for controllers.
void MouseSpeedTick(uint32_t engine)
{
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t input = pc ? ue3::GetObject(pc, "PlayerInput").value_or(0) : 0;
    if (!input)
        return;
    struct Field
    {
        const char* name;
        float original = 0;
        uint32_t address = 0;
    };
    static Field fields[3] = { { "LookRightScale" }, { "LookUpScale" }, { "RotationRateInterpFactor" } };
    static uint32_t current = 0;
    static bool boosted = false;
    auto read = [](uint32_t a) {
        float f;
        const uint32_t raw = guest::Read32(a);
        memcpy(&f, &raw, 4);
        return f;
    };
    auto write = [](uint32_t a, float f) {
        uint32_t raw;
        memcpy(&raw, &f, 4);
        guest::Write32(a, raw);
    };
    if (input != current)
    {
        // A new input object (a level load): its values are the game's own.
        current = input, boosted = false;
        for (Field& f : fields)
        {
            const auto p = ue3::FindProperty(input, f.name);
            f.address = p && p->size == 4 ? p->address : 0;
            if (f.address)
                f.original = read(f.address);
        }
    }
    const bool want = game_ui::Current() == game_ui::State::Gameplay &&
                      host::input::ActiveDevice() == host::input::Device::Keyboard && !getenv("DT_TEST_NOBOOST");
    if (want == boosted)
        return;
    boosted = want;
    for (int i = 0; i < 3; i++)
        if (fields[i].address)
            write(fields[i].address, !want ? fields[i].original : i < 2 ? fields[i].original * host::input::MouseLookBoost : 1.0f);
    dtlog::Important("[input] mouse look speed %s (LookRightScale %.0f)\n", want ? "boosted" : "the game's own",
        fields[0].address ? read(fields[0].address) : 0.0f);
}

// DT_TEST_STICK (environment, testing): log the view's turn rate (the local PlayerController's yaw) once a second.
void TurnRateTick(uint32_t engine)
{
    static const bool on = getenv("DT_TEST_STICK") || getenv("DT_TEST_MOUSE");
    static int ticks = 0;
    static uint32_t lastYaw = 0;
    static int64_t turned = 0;  // summed frame by frame (a frame's change never wraps; a second's can)
    if (!on || game_ui::Current() != game_ui::State::Gameplay)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const auto rotation = pc ? ue3::FindProperty(pc, "Rotation") : std::nullopt;
    if (!rotation)
        return;
    const uint32_t yaw = guest::Read32(rotation->address + 4);
    if (ticks)
        turned += int16_t(uint16_t(yaw - lastYaw));
    lastYaw = yaw;
    // (and the pawn's travel: the same per wall second at every frame rate if the game is frame-rate independent)
    static float lastX = 0, lastY = 0, travelled = 0;
    static auto since = std::chrono::steady_clock::now();
    if (const uint32_t pawn = ue3::GetObject(pc, "Pawn").value_or(0))
        if (const auto loc = ue3::FindProperty(pawn, "Location"))
        {
            const float x = std::bit_cast<float>(guest::Read32(loc->address)), y = std::bit_cast<float>(guest::Read32(loc->address + 4));
            if (ticks)
                travelled += std::hypot(x - lastX, y - lastY);
            lastX = x, lastY = y;
        }
    if (++ticks % 60)
        return;
    const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count();
    since = std::chrono::steady_clock::now();
    dtlog::Important("[test] stick %.2f: %lld units in the last 60 frames (%.2f s: %.0f units/s, %.0f fps; moved %.0f/s)\n",
        host::input::TestStickValue(), (long long)turned, secs, float(turned) / secs, 60.0f / secs, travelled / secs);
    turned = 0, travelled = 0;
    // Once, 5 s in: the properties of the controller, its input, camera and pawn whose names suggest the turn speed.
    if (ticks == 300)
    {
        const uint32_t objects[] = { pc, ue3::GetObject(pc, "PlayerInput").value_or(0), ue3::GetObject(pc, "PlayerCamera").value_or(0),
            ue3::GetObject(pc, "Pawn").value_or(0) };
        for (uint32_t object : objects)
        {
            if (!object)
                continue;
            dtlog::Important("[test] %s:\n", ue3::ClassName(object).c_str());
            for (uint32_t c = guest::Read32(object + 0x34); c; c = guest::Read32(c + 0x48))  // class, then super
                for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))  // children, next field
                {
                    const std::string kind = ue3::ClassName(f);
                    if (kind != "FloatProperty" && kind != "IntProperty" && kind != "StructProperty")
                        continue;
                    std::string name = ue3::PathName(f);
                    name = name.substr(name.find_last_of(".:") + 1);
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    static const char* const words[] = { "turn", "speed", "sens", "yaw", "rot", "look", "camera", "aim", "rate", "scale", "accel" };
                    if (std::none_of(std::begin(words), std::end(words), [&](const char* w) { return lower.find(w) != std::string::npos; }))
                        continue;
                    const uint32_t at = object + guest::Read32(f + 0x5C);
                    const uint32_t raw = guest::Read32(at);
                    float fv;
                    memcpy(&fv, &raw, 4);
                    dtlog::Important("[test]   %s %s = %s\n", kind.c_str(), name.c_str(),
                        kind == "FloatProperty" ? std::to_string(fv).c_str() : kind == "IntProperty" ? std::to_string(int32_t(raw)).c_str()
                                                                                                  : "(struct)");
                }
        }
    }
}

// The title's "press START" screen: D3FrontEnd_TitleMenu.NowMenuStage 1 (2 is the menu list). Any input starts there
// (host::input), the label says "Press any key/button" (button_text) and the START icon beside it is hidden (PromptsTick).
void TitleTick()
{
    uint32_t title = 0;
    {
        std::lock_guard lock(gMoviesMutex);
        for (uint32_t p : gOpen)
            if (auto it = gMovieOf.find(p); it != gMovieOf.end() && it->second.find("titlemenu") != std::string::npos)
                title = p;
    }
    const auto stage = title ? ue3::FindProperty(title, "NowMenuStage") : std::nullopt;
    const bool showing = stage && guest::Read8(stage->address) == 1 && game_ui::Current() == game_ui::State::FrontEnd;
    host::input::SetPressStartScreen(showing);
}

// DT_DUMP_TITLE (environment, testing): the title menu player's own properties (bool, byte, int, float, object) at
// frames 2300 and 2800, to compare the press-START screen with the menu list.
void DumpTitleTick()
{
    static const bool on = getenv("DT_DUMP_TITLE") != nullptr;
    const uint64_t frame = cafe::gx2::FrameCount();
    if (!on || (frame != 2300 && frame != 2800))
        return;
    std::vector<std::pair<uint32_t, std::string>> movies;
    {
        std::lock_guard lock(gMoviesMutex);
        for (uint32_t p : gOpen)
            if (auto it = gMovieOf.find(p); it != gMovieOf.end())
                movies.push_back({ p, it->second });
    }
    for (auto& [player, name] : movies)
    {
        if (name.find("titlemenu") == std::string::npos)
            continue;
        dtlog::Important("[test] frame %llu: %s %s\n", (unsigned long long)frame, name.c_str(), ue3::ClassName(player).c_str());
        for (uint32_t c = guest::Read32(player + 0x34); c; c = guest::Read32(c + 0x48))
        {
            const std::string cls = ue3::PathName(c);
            if (cls.find("GFxMoviePlayer") != std::string::npos && cls.find("D3") == std::string::npos)
                break;  // the engine's own base class: its fields are not the title's
            for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
            {
                const std::string kind = ue3::ClassName(f);
                if (kind != "BoolProperty" && kind != "ByteProperty" && kind != "IntProperty" && kind != "FloatProperty" &&
                    kind != "ObjectProperty")
                    continue;
                std::string fname = ue3::PathName(f);
                fname = fname.substr(fname.find_last_of(".:") + 1);
                const uint32_t at = player + guest::Read32(f + 0x5C);
                uint32_t raw = guest::Read32(at);
                std::string value;
                if (kind == "BoolProperty")
                    value = (raw & guest::Read32(f + 0x70)) ? "true" : "false";
                else if (kind == "ByteProperty")
                    value = std::to_string(raw >> 24);
                else if (kind == "FloatProperty")
                {
                    float fv;
                    memcpy(&fv, &raw, 4);
                    value = std::to_string(fv);
                }
                else
                    value = std::to_string(int32_t(raw));
                dtlog::Important("[test]   %s %s = %s\n", kind.c_str(), fname.c_str(), value.c_str());
            }
        }
    }
}

// DT_DUMP_MOVIE="<movie name part>:<frame>[;...]" (testing): at that frame, every field of each matching open movie
// player's class chain: properties with their values (arrays: count and first words) and functions by name.
void DumpMovieTick()
{
    static const std::vector<std::pair<std::string, uint64_t>> specs = [] {
        std::vector<std::pair<std::string, uint64_t>> v;
        if (const char* e = getenv("DT_DUMP_MOVIE"))
            for (std::string s = e; !s.empty();)
            {
                const size_t semi = s.find(';');
                const std::string item = s.substr(0, semi);
                s = semi == std::string::npos ? "" : s.substr(semi + 1);
                const size_t colon = item.rfind(':');
                if (colon != std::string::npos)
                    v.push_back({ item.substr(0, colon), std::stoull(item.substr(colon + 1)) });
            }
        return v;
    }();
    const uint64_t frame = cafe::gx2::FrameCount();
    for (const auto& [part, at] : specs)
    {
        if (frame != at)
            continue;
        std::vector<std::pair<uint32_t, std::string>> movies;
        {
            std::lock_guard lock(gMoviesMutex);
            for (uint32_t p : gOpen)
                if (auto it = gMovieOf.find(p); it != gMovieOf.end() && it->second.find(part) != std::string::npos)
                    movies.push_back({ p, it->second });
        }
        for (auto& [player, name] : movies)
        {
            dtlog::Important("[dump] frame %llu: %s %s\n", (unsigned long long)frame, name.c_str(), ue3::ClassName(player).c_str());
            for (uint32_t c = guest::Read32(player + 0x34); c; c = guest::Read32(c + 0x48))
            {
                const std::string cls = ue3::PathName(c);
                dtlog::Important("[dump]  class %s\n", cls.c_str());
                if (cls.find("GFxMoviePlayer") != std::string::npos && cls.find("D3") == std::string::npos)
                    break;
                for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
                {
                    const std::string kind = ue3::ClassName(f);
                    std::string fname = ue3::PathName(f);
                    fname = fname.substr(fname.find_last_of(".:") + 1);
                    if (kind == "Function" || kind == "State" || kind == "Const" || kind == "Enum" || kind == "ScriptStruct")
                    {
                        std::string params;
                        if (kind == "Function")
                            for (uint32_t q = guest::Read32(f + 0x4C); q; q = guest::Read32(q + 0x40))
                            {
                                std::string pn = ue3::PathName(q);
                                params += (params.empty() ? "" : ",") + ue3::ClassName(q) + " " + pn.substr(pn.find_last_of(".:") + 1);
                            }
                        dtlog::Important("[dump]   %s %s(%s)\n", kind.c_str(), fname.c_str(), params.c_str());
                        continue;
                    }
                    const uint32_t at2 = player + guest::Read32(f + 0x5C);
                    const uint32_t raw = guest::Read32(at2);
                    std::string value;
                    if (kind == "BoolProperty")
                        value = (raw & guest::Read32(f + 0x70)) ? "true" : "false";
                    else if (kind == "ByteProperty")
                        value = std::to_string(raw >> 24);
                    else if (kind == "FloatProperty")
                        value = std::to_string(std::bit_cast<float>(raw));
                    else if (kind == "ObjectProperty" || kind == "ClassProperty" || kind == "ComponentProperty")
                        value = raw ? ue3::PathName(raw) : "None";
                    else if (kind == "ArrayProperty" || kind == "StrProperty")
                    {
                        const uint32_t data = raw, count = guest::Read32(at2 + 4);
                        value = "count " + std::to_string(count);
                        if (kind == "StrProperty" && data && count && count < 200)
                        {
                            value += " \"";
                            for (uint32_t i = 0; i + 1 < count; i++)
                                value += char(guest::base[data + i * 2 + 1]);
                            value += "\"";
                        }
                        else if (data && count && count < 100000)
                        {
                            char buf[16];
                            for (uint32_t i = 0; i < std::min(count, 6u); i++)
                            {
                                const uint32_t w = guest::Read32(data + i * 4);
                                snprintf(buf, sizeof buf, " %08X", w);
                                value += buf;
                                if (w >= 0x10000000 && w < 0x50000000)
                                {
                                    const std::string n = ue3::PathName(w);
                                    if (!n.empty() && n.size() < 120)
                                        value += "(" + n + ")";
                                }
                            }
                        }
                    }
                    else
                        value = std::to_string(int32_t(raw));
                    dtlog::Important("[dump]   %s %s = %s\n", kind.c_str(), fname.c_str(), value.c_str());
                }
            }
        }
    }
}

// Score Attack loadout (gameplay.extra_weapons, not on the settings page, default on): the single-player menu's
// weapon lists (D3FrontEnd_SingleplayMenu.MissionRangeWeapons / MissionMeleeWeapons, config arrays of stMissionWeapon
// { byte mission[9]; byte weaponCode; byte underbarrelCode; }: per mission 0 hidden, bit 0 selectable, 2 the default
// first slot, 4 the default second) get every other player weapon of the game's weapon table
// (D3UIDataProvider_WeaponInfo sections with menu art), selectable in every mission, and each gun's underbarrel
// attachments as variants of it. The arrays are grown with the game's allocator when the menu movie opens.
struct MissionWeaponLayout
{
    uint32_t elemSize = 0, missionOffset = 0, missionCount = 0, codeOffset = 0, underOffset = 0;
    uint32_t codeEnum = 0, underEnum = 0;
};

// The value of an enum entry named `name`, or -1. UEnum::Names: TArray<FName> at +0x48 (FName: index, number).
int EnumValue(uint32_t uenum, const std::string& name)
{
    if (!uenum)
        return -1;
    const uint32_t index = ue3::FindNameIndex(name.c_str());
    if (!index)
        return -1;
    const uint32_t data = guest::Read32(uenum + 0x48), count = guest::Read32(uenum + 0x4C);
    for (uint32_t i = 0; data && i < count && i < 1024; i++)
        if (guest::Read32(data + i * 8) == index)
            return int(i);
    return -1;
}

bool ReadMissionWeaponLayout(uint32_t arrayProperty, MissionWeaponLayout& l)
{
    const uint32_t inner = guest::Read32(arrayProperty + 0x70);
    if (!inner || ue3::ClassName(inner) != "StructProperty")
        return false;
    l.elemSize = guest::Read32(inner + 0x4C);
    const uint32_t st = guest::Read32(inner + 0x70);
    bool mission = false, code = false, under = false;
    for (uint32_t f = st ? guest::Read32(st + 0x4C) : 0; f; f = guest::Read32(f + 0x40))
    {
        std::string n = ue3::PathName(f);
        n = n.substr(n.find_last_of(".:") + 1);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const uint32_t offset = guest::Read32(f + 0x5C);
        if (n == "mission")
            l.missionOffset = offset, mission = true;  // (its length: up to weaponCode)
        else if (n == "weaponcode")
            l.codeOffset = offset, l.codeEnum = guest::Read32(f + 0x70), code = true;
        else if (n == "underbarrelcode")
            l.underOffset = offset, l.underEnum = guest::Read32(f + 0x70), under = true;
    }
    if (mission && code)
        l.missionCount = std::min(l.codeOffset > l.missionOffset ? l.codeOffset - l.missionOffset : 9u, 9u);
    return mission && code && under && l.elemSize >= 3 && l.elemSize <= 64 && ue3::ClassName(l.codeEnum) == "Enum" &&
        ue3::ClassName(l.underEnum) == "Enum";
}

struct ExtraEntry
{
    std::string code, underbarrel;
};

// The game's weapon table as the extra Score Attack weapons use it: the entries to add, each weapon code's place in the
// menu's order (pistols and machine pistols, assault rifles, SMGs, shotguns, machine guns, sniper rifles, launchers,
// X-gear) and the X-gear weapons (the game's special weapons, which the player cannot swap or keep over a checkpoint).
struct WeaponTable
{
    std::vector<ExtraEntry> range, melee;
    std::map<std::string, int> rank;
    std::set<std::string> xgear;
    std::map<std::string, std::string> name;  // weapon code -> its name in the game's text (DT_Weapons)
    std::map<std::string, std::string> gfx;   // weapon code -> its picture in the weapon library (gfxidentifiername)
    std::set<std::string> underbarrels;       // every underbarrel code the table names
};

constexpr int XGearRank = 7;

const WeaponTable& Weapons()
{
    static const WeaponTable table = [] {
        WeaponTable t;
        struct Info
        {
            std::string code, group, gfx, locGroup, locName;
            std::vector<std::string> under;
        };
        std::map<std::string, Info> infos;
        std::vector<std::string> order;
        for (const auto& [section, key, value] : assets::coalesced::Entries("d3uidataprovider_weaponinfo"))
        {
            if (!infos.count(section))
                order.push_back(section);
            Info& i = infos[section];
            if (key == "weaponcode" && i.code.empty())
                i.code = value;
            else if (key == "weapongroup" && i.group.empty())
                i.group = value;
            else if (key == "gfxidentifiername" && i.gfx.empty())
                i.gfx = value;
            else if (key == "locgroupname" && i.locGroup.empty())
                i.locGroup = value;
            else if (key == "locweaponname" && i.locName.empty())
                i.locName = value;
            else if (key == "underattachmentlist" && std::find(i.under.begin(), i.under.end(), value) == i.under.end())
                i.under.push_back(value);
        }
        t.underbarrels.insert("NoUnderbarrelAttachment");
        for (const auto& [section, i] : infos)
        {
            for (const std::string& u : i.under)
                t.underbarrels.insert(u);
            if (i.code.empty() || t.name.count(i.code))
                continue;
            t.gfx[i.code] = i.gfx;
            std::string n = assets::coalesced::Value("dt_weapons.int", i.locGroup, i.locName);
            // (the names the Localize hook gives, for the same entries)
            if (i.locName == "ID_54" && n.empty())
                n = "9K38 Igla";
            else if (i.locName == "UID_9" && n == "Katana")
                n = "SOD Katana";
            else if (i.locName == "ID_1" && n == "Katana")
                n = "USA Katana";
            t.name[i.code] = n.empty() ? i.code.substr(i.code.rfind('_') + 1) : n;
        }
        static const std::map<std::string, int> groupRank = { { "WEAPONGROUP_HANDGUN", 0 }, { "WEAPONGROUP_MACHINE_PISTOL", 0 },
            { "WEAPONGROUP_ASSAULT_RIFLE", 1 }, { "WEAPONGROUP_SUBMACHINE_GUN", 2 }, { "WEAPONGROUP_SHOTGUN", 3 },
            { "WEAPONGROUP_LIGHT_MACHINE_GUN", 4 }, { "WEAPONGROUP_SNIPER_RIFLE", 5 }, { "WEAPONGROUP_ROCKET_LAUNCHER", 6 },
            { "WEAPONGROUP_XGEAR", XGearRank } };
        for (const std::string& s : order)
        {
            const Info& i = infos[s];
            if (i.code.empty() || i.gfx.empty() || i.code == "WEAPONCODE_AR_BEGIN")  // (no menu art; the training rifle)
                continue;
            // The chainsaw: a melee weapon and X-gear (its table entry is in the "etc" group).
            if (i.code == "WEAPONCODE_MELEE_ChainSaw")
            {
                t.xgear.insert(i.code);
                t.melee.push_back({ i.code, "NoUnderbarrelAttachment" });
                continue;
            }
            const auto rank = groupRank.find(i.group);
            if (rank != groupRank.end())
            {
                t.rank[i.code] = rank->second;
                if (rank->second == XGearRank)
                    t.xgear.insert(i.code);
                t.range.push_back({ i.code, "NoUnderbarrelAttachment" });
                for (const std::string& u : i.under)
                    t.range.push_back({ i.code, u });
            }
            else if (i.group.rfind("WEAPONGROUP_MELEE", 0) == 0)
                t.melee.push_back({ i.code, "NoUnderbarrelAttachment" });
        }
        return t;
    }();
    return table;
}

void ExtendMissionWeapons(uint32_t menu, const char* arrayName, const std::vector<ExtraEntry>& extra, bool sort)
{
    const uint32_t prop = ue3::FindPropertyField(menu, arrayName);
    MissionWeaponLayout l;
    if (!prop || !ReadMissionWeaponLayout(prop, l))
    {
        dtlog::Important("[weapons] %s: unexpected layout, left alone\n", arrayName);
        return;
    }
    const uint32_t array = menu + guest::Read32(prop + 0x5C);
    uint32_t data = guest::Read32(array), count = guest::Read32(array + 4);
    if (count > 4096)
        return;
    std::vector<std::vector<uint8_t>> entries;
    for (uint32_t i = 0; i < count; i++)
        entries.emplace_back(guest::base + data + i * l.elemSize, guest::base + data + (i + 1) * l.elemSize);
    // (the game's list ends with a WEAPONCODE_NON entry: it stays last)
    const int none = EnumValue(l.codeEnum, "WEAPONCODE_NON");
    std::vector<uint8_t> noneEntry;
    if (!entries.empty() && none >= 0 && entries.back()[l.codeOffset] == uint8_t(none))
        noneEntry = entries.back(), entries.pop_back();
    std::set<std::pair<int, int>> present;
    for (const auto& e : entries)
        present.insert({ e[l.codeOffset], e[l.underOffset] });
    size_t added = 0;
    std::string missing;
    for (const ExtraEntry& x : extra)
    {
        const int code = EnumValue(l.codeEnum, x.code), under = EnumValue(l.underEnum, x.underbarrel);
        if (code < 0 || under < 0)
        {
            missing += " " + x.code + "/" + x.underbarrel;
            continue;
        }
        if (!present.insert({ code, under }).second)
            continue;
        std::vector<uint8_t> e(l.elemSize, 0);
        for (uint32_t m = 0; m < l.missionCount; m++)
            e[l.missionOffset + m] = 1;
        e[l.codeOffset] = uint8_t(code), e[l.underOffset] = uint8_t(under);
        entries.push_back(std::move(e));
        added++;
    }
    if (sort)
    {
        // By the menu order's group, then by each weapon's first place in the list (the game's own entry first, its
        // variants after it).
        std::map<int, int> rankOf, firstAt;
        for (const auto& [codeName, rank] : Weapons().rank)
            if (const int v = EnumValue(l.codeEnum, codeName); v >= 0)
                rankOf[v] = rank;
        for (size_t i = 0; i < entries.size(); i++)
            firstAt.emplace(entries[i][l.codeOffset], int(i));
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            const int ca = a[l.codeOffset], cb = b[l.codeOffset];
            const int ra = rankOf.count(ca) ? rankOf[ca] : 99, rb = rankOf.count(cb) ? rankOf[cb] : 99;
            return ra != rb ? ra < rb : firstAt[ca] < firstAt[cb];
        });
    }
    if (!noneEntry.empty())
        entries.push_back(noneEntry);
    if (added || sort)
    {
        const uint32_t newCount = uint32_t(entries.size());
        if (newCount > count)
        {
            const uint32_t grown = ue3::Realloc(data, newCount * l.elemSize);
            if (!grown)
                return;
            data = grown;
        }
        for (uint32_t i = 0; i < newCount; i++)
            memcpy(guest::base + data + i * l.elemSize, entries[i].data(), l.elemSize);
        guest::Write32(array, data);
        guest::Write32(array + 4, newCount);
        if (newCount > count)
            guest::Write32(array + 8, newCount);
    }
    dtlog::Important("[weapons] %s: %u entries, %zu added%s%s%s\n", arrayName, count, added, sort ? ", sorted" : "",
        missing.empty() ? "" : "; not in the game's enums:", missing.c_str());
}

// DT_LOG_WEAPON (testing): logs the weapon the player holds whenever it changes, with its ammo (AmmoCount).
void LogWeaponTick(uint32_t engine)
{
    static const bool on = getenv("DT_LOG_WEAPON") != nullptr;
    static uint32_t last = 0;
    static int ticks = 0;
    if (!on || ++ticks % 10 || game_ui::Current() != game_ui::State::Gameplay)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t pawn = pc ? ue3::GetObject(pc, "Pawn").value_or(0) : 0;
    const uint32_t weapon = pawn ? ue3::GetObject(pawn, "Weapon").value_or(0) : 0;
    // Once, the whole inventory with its ammunition-related integers.
    static bool listed = false;
    if (pawn && !listed)
    {
        listed = true;
        const uint32_t inv = ue3::GetObject(pawn, "InvManager").value_or(0);
        uint32_t item = inv ? ue3::GetObject(inv, "InventoryChain").value_or(0) : 0;
        for (int n = 0; item && n < 32; n++, item = ue3::GetObject(item, "Inventory").value_or(0))
        {
            std::string fields;
            for (uint32_t c = guest::Read32(item + 0x34); c; c = guest::Read32(c + 0x48))
                for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
                {
                    const std::string kind = ue3::ClassName(f);
                    if (kind != "IntProperty" && kind != "BoolProperty")
                        continue;
                    std::string fname = ue3::PathName(f);
                    fname = fname.substr(fname.find_last_of(".:") + 1);
                    std::string lower = fname;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    if (lower.find("ammo") == std::string::npos && lower.find("bullet") == std::string::npos &&
                        lower.find("magazine") == std::string::npos && lower.find("empty") == std::string::npos)
                        continue;
                    const uint32_t raw = guest::Read32(item + guest::Read32(f + 0x5C));
                    fields += " " + fname + "=" + (kind == "BoolProperty" ? ((raw & guest::Read32(f + 0x70)) ? "1" : "0") : std::to_string(int32_t(raw)));
                }
            dtlog::Important("[weapons] inventory %s:%s\n", ue3::ClassName(item).c_str(), fields.c_str());
        }
    }
    if (!weapon || weapon == last)
        return;
    last = weapon;
    const auto ammo = ue3::FindProperty(weapon, "AmmoCount");
    dtlog::Important("[weapons] holding %s (ammo %d)\n", ue3::ClassName(weapon).c_str(),
        ammo ? int32_t(guest::Read32(ammo->address)) : -1);
}

// DT_DUMP_GAMEPLAY (testing): once in gameplay, the functions and properties of the player pawn's, its inventory
// manager's and the game info's classes (their whole class chains up to Engine's Actor).
void DumpClassChain(uint32_t object, const char* what)
{
    if (!object)
        return;
    dtlog::Important("[dump] %s %s\n", what, ue3::PathName(object).c_str());
    for (uint32_t c = guest::Read32(object + 0x34); c; c = guest::Read32(c + 0x48))
    {
        const std::string cls = ue3::PathName(c);
        if (cls == "Engine.Actor" || cls == "Core.Object")
            break;
        std::string line;
        for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
        {
            std::string fname = ue3::PathName(f);
            fname = fname.substr(fname.find_last_of(".:") + 1);
            const std::string kind = ue3::ClassName(f);
            std::string params;
            if (kind == "Function")
                for (uint32_t q = guest::Read32(f + 0x4C); q; q = guest::Read32(q + 0x40))
                {
                    std::string pn = ue3::PathName(q);
                    params += (params.empty() ? "" : ",") + ue3::ClassName(q).substr(0, ue3::ClassName(q).size() - 8) + " " +
                        pn.substr(pn.find_last_of(".:") + 1);
                }
            line += " " + (kind == "Function" ? fname + "(" + params + ")" : kind == "State" ? "state:" + fname : fname);
            if (line.size() > 1500)
            {
                dtlog::Important("[dump]  %s:%s\n", cls.c_str(), line.c_str());
                line.clear();
            }
        }
        if (!line.empty())
            dtlog::Important("[dump]  %s:%s\n", cls.c_str(), line.c_str());
    }
}

void DumpGameplayTick(uint32_t engine)
{
    static const bool on = getenv("DT_DUMP_GAMEPLAY") != nullptr;
    static int ticks = 0;
    if (!on || game_ui::Current() != game_ui::State::Gameplay || ++ticks != 100)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t pawn = pc ? ue3::GetObject(pc, "Pawn").value_or(0) : 0;
    DumpClassChain(pawn, "pawn");
    DumpClassChain(pawn ? ue3::GetObject(pawn, "InvManager").value_or(0) : 0, "inventory manager");
    DumpClassChain(world ? ue3::GetObject(world, "Game").value_or(0) : 0, "game");
    const uint32_t pri = pawn ? ue3::GetObject(pawn, "PlayerReplicationInfo").value_or(0) : 0;
    for (uint32_t c = pri ? guest::Read32(pri + 0x34) : 0; c; c = guest::Read32(c + 0x48))
    {
        const std::string cls = ue3::PathName(c);
        if (cls == "Engine.Info" || cls == "Engine.Actor")
            break;
        std::string line;
        for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
        {
            const std::string kind = ue3::ClassName(f);
            if (kind != "ByteProperty" && kind != "IntProperty" && kind != "ClassProperty" && kind != "ObjectProperty" &&
                kind != "StructProperty" && kind != "ArrayProperty")
                continue;
            std::string fname = ue3::PathName(f);
            fname = fname.substr(fname.find_last_of(".:") + 1);
            const uint32_t at = pri + guest::Read32(f + 0x5C), raw = guest::Read32(at);
            std::string v;
            if (kind == "ByteProperty")
                v = std::to_string(guest::base[at]);
            else if (kind == "IntProperty")
                v = std::to_string(int32_t(raw));
            else if (kind == "ClassProperty" || kind == "ObjectProperty")
                v = raw ? ue3::PathName(raw) : "None";
            else if (kind == "ArrayProperty")
                v = "[" + std::to_string(guest::Read32(at + 4)) + "]";
            else
            {
                char b[64];
                snprintf(b, sizeof b, "{%08X %08X %08X %08X}", raw, guest::Read32(at + 4), guest::Read32(at + 8), guest::Read32(at + 12));
                v = b;
            }
            line += " " + fname + "=" + v;
        }
        dtlog::Important("[dump] pri %s:%s\n", cls.c_str(), line.c_str());
    }
}

// X-gear weapons in a Score Attack loadout: the game creates the MGL and the Verutum from the loadout but not always
// the M202A1, the M214 or the M295 IACW, and the weapon table names the Machete's class for the chainsaw. Once per
// mission, after the game has given the loadout (D3PRISingleplayScoreAttack.InitialPrimary/SecondaryRangeWeaponClass), a
// range weapon the player lacks is created with Pawn.CreateInventory (the chainsaw as DevilsThirdMeleeWeapon_ChainSaw).
// As with the game's own X-gear, a checkpoint reload does not bring it back.
void GrantXGearTick(uint32_t engine)
{
    static const bool on = settings::GetBool("gameplay.extra_weapons", true);
    static uint32_t doneWorld = 0;
    static int ticks = 0;
    if (!on || game_ui::Current() != game_ui::State::Gameplay)
    {
        ticks = 0;
        return;
    }
    if (++ticks < 30)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    if (!world || world == doneWorld)
        return;
    const uint32_t pc = ue3::Call(world, "GetALocalPlayerController").value_or(0);
    const uint32_t pawn = pc ? ue3::GetObject(pc, "Pawn").value_or(0) : 0;
    const uint32_t pri = pawn ? ue3::GetObject(pawn, "PlayerReplicationInfo").value_or(0) : 0;
    if (!pri)
        return;  // (no pawn yet: again next tick)
    doneWorld = world;
    if (!ue3::IsA(pri, "D3PRISingleplayScoreAttack"))
        return;
    const uint32_t inv = ue3::GetObject(pawn, "InvManager").value_or(0);
    auto has = [&](uint32_t cls) {
        uint32_t item = inv ? ue3::GetObject(inv, "InventoryChain").value_or(0) : 0;
        for (int n = 0; item && n < 64; n++, item = ue3::GetObject(item, "Inventory").value_or(0))
            if (guest::Read32(item + 0x34) == cls)
                return true;
        return false;
    };
    const uint32_t classClass = guest::Read32(guest::Read32(pawn + 0x34) + 0x34);  // Core.Class
    for (const char* slot : { "InitialPrimaryRangeWeaponClass", "InitialSecondaryRangeWeaponClass", "InitialMeleeWeaponClass" })
    {
        uint32_t cls = ue3::GetObject(pri, slot).value_or(0);
        if (!cls)
            continue;
        std::string name = ue3::PathName(cls);
        // The chainsaw's entry gives the Machete (the weapon table names its class): the confirmed Melee was the chainsaw.
        const bool chainsaw = std::string(slot) == "InitialMeleeWeaponClass" && (gConfirmedBits & 4) &&
            name.find("MeleeWeapon_Machete") != std::string::npos;
        if (chainsaw)
        {
            name = "DevilsThirdGame.DevilsThirdMeleeWeapon_ChainSaw";
            cls = ue3::Call(pawn, "DynamicLoadObject", { ue3::Arg::MakeString(name), classClass }).value_or(0);
            uint32_t item = inv ? ue3::GetObject(inv, "InventoryChain").value_or(0) : 0;
            for (int n = 0; item && n < 64; n++)
            {
                const uint32_t next = ue3::GetObject(item, "Inventory").value_or(0);
                if (ue3::ClassName(item) == "DevilsThirdMeleeWeapon_Machete")
                {
                    ue3::Call(inv, "RemoveFromInventory", { item });
                    ue3::Call(item, "Destroy");
                    dtlog::Important("[weapons] removed the Machete given for the chainsaw's entry\n");
                }
                item = next;
            }
        }
        if (!cls || has(cls))
            continue;
        const uint32_t made = ue3::Call(pawn, "CreateInventory", { cls, false }).value_or(0);
        dtlog::Important("[weapons] %s %s was not given: created %s\n", slot, name.c_str(), made ? "it" : "nothing");
    }
}

// DT_DUMP_SETTINGS="<frame>;..." (testing): the system settings' byte, int and array properties whose names mention
// weapons, missions or loadouts, at those frames.
void DumpSettingsTick(uint32_t engine)
{
    static const std::vector<uint64_t> frames = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("DT_DUMP_SETTINGS"))
            for (std::string s = e; !s.empty();)
            {
                const size_t semi = s.find(';');
                v.push_back(std::stoull(s.substr(0, semi)));
                s = semi == std::string::npos ? "" : s.substr(semi + 1);
            }
        return v;
    }();
    const uint64_t frame = cafe::gx2::FrameCount();
    if (std::find(frames.begin(), frames.end(), frame) == frames.end())
        return;
    {
        uint32_t menu = 0;
        {
            std::lock_guard lock(gMoviesMutex);
            for (uint32_t p : gOpen)
                if (auto it = gMovieOf.find(p); it != gMovieOf.end() && it->second.find("soloplay_menu") != std::string::npos)
                    menu = p;
        }
        const uint32_t prop = menu ? ue3::FindPropertyField(menu, "EquipInfo") : 0;
        const uint32_t st = prop ? guest::Read32(prop + 0x70) : 0;
        std::string line;
        for (uint32_t f = st ? guest::Read32(st + 0x4C) : 0; f; f = guest::Read32(f + 0x40))
        {
            std::string fname = ue3::PathName(f);
            fname = fname.substr(fname.find_last_of(".:") + 1);
            const uint32_t at = menu + guest::Read32(prop + 0x5C) + guest::Read32(f + 0x5C), raw = guest::Read32(at);
            const std::string kind = ue3::ClassName(f);
            line += " " + fname + "=" + (kind == "ByteProperty" ? std::to_string(guest::base[at]) :
                (kind == "ClassProperty" || kind == "ObjectProperty") ? (raw ? ue3::PathName(raw) : "None") :
                kind + ":" + std::to_string(int32_t(raw)));
        }
        dtlog::Important("[dump] frame %llu EquipInfo:%s\n", (unsigned long long)frame, line.c_str());
        if (const uint32_t wi = menu ? ue3::FindPropertyField(menu, "MissionWeaponIndex") : 0)
        {
            const uint32_t at = menu + guest::Read32(wi + 0x5C);
            dtlog::Important("[dump] frame %llu MissionWeaponIndex (dim %u): %d %d %d %d; slot %d\n", (unsigned long long)frame,
                guest::Read32(wi + 0x40 + 0x8), int32_t(guest::Read32(at)), int32_t(guest::Read32(at + 4)),
                int32_t(guest::Read32(at + 8)), int32_t(guest::Read32(at + 12)),
                int32_t(ue3::GetObject(menu, "MissionWeaponSlotIndex").value_or(0)));
        }
    }
    const uint32_t settingsObject = ue3::Call(engine, "GetSystemSettings").value_or(0);
    for (uint32_t c = settingsObject ? guest::Read32(settingsObject + 0x34) : 0; c; c = guest::Read32(c + 0x48))
    {
        std::string line;
        for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
        {
            const std::string kind = ue3::ClassName(f);
            std::string fname = ue3::PathName(f);
            fname = fname.substr(fname.find_last_of(".:") + 1);
            std::string lower = fname;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (lower.find("weapon") == std::string::npos && lower.find("mission") == std::string::npos &&
                lower.find("loadout") == std::string::npos && lower.find("equip") == std::string::npos)
                continue;
            const uint32_t at = settingsObject + guest::Read32(f + 0x5C);
            std::string v;
            if (kind == "ByteProperty")
                v = std::to_string(guest::base[at]);
            else if (kind == "IntProperty")
                v = std::to_string(int32_t(guest::Read32(at)));
            else if (kind == "ArrayProperty")
            {
                const uint32_t d = guest::Read32(at), n = guest::Read32(at + 4);
                v = "[" + std::to_string(n) + "]";
                for (uint32_t i = 0; d && i < std::min(n, 8u); i++)
                    v += " " + std::to_string(guest::Read32(d + i * 4));
            }
            else
            {
                char b[48];
                snprintf(b, sizeof b, "%s %08X %08X", kind.c_str(), guest::Read32(at), guest::Read32(at + 4));
                v = b;
            }
            line += " " + fname + "=" + v;
        }
        if (!line.empty())
            dtlog::Important("[dump] frame %llu settings %s:%s\n", (unsigned long long)frame, ue3::PathName(c).c_str(), line.c_str());
    }
}

// Which of the loadout's Primary and Secondary slots hold an X-gear weapon (bits 1, 2): MissionWeaponIndex[slot] is the
// position in MissionRangeWeapons. Shown by the slots' colour (_global.dtXGear, swf_patch::AddXGearTint) and checked by
// the Confirm (ProcessEvent hook).
// The open menu's loadout arrays: their layout (one struct for both), where MissionWeaponIndex[3] (each slot's position in
// its array: Primary and Secondary in MissionRangeWeapons, Melee in MissionMeleeWeapons) and MissionWeaponSlotIndex (the
// focused slot) live, and the enum values' names.
struct LoadoutArrays
{
    uint32_t menu = 0, rangeProp = 0, meleeProp = 0, indexAt = 0, slotAt = 0;
    MissionWeaponLayout l;
    std::set<int> xgear;
    std::map<int, std::string> codeName, underName;
};

LoadoutArrays& Arrays(uint32_t menu)
{
    static LoadoutArrays a;
    if (a.menu != menu)
    {
        a = {};
        a.menu = menu;
        a.rangeProp = ue3::FindPropertyField(menu, "MissionRangeWeapons");
        a.meleeProp = ue3::FindPropertyField(menu, "MissionMeleeWeapons");
        if (!a.rangeProp || !a.meleeProp || !ReadMissionWeaponLayout(a.rangeProp, a.l))
            a.rangeProp = a.meleeProp = 0;
        else
        {
            for (const std::string& code : Weapons().xgear)
                if (const int v = EnumValue(a.l.codeEnum, code); v >= 0)
                    a.xgear.insert(v);
            for (const auto& [code, name] : Weapons().name)
                if (const int v = EnumValue(a.l.codeEnum, code); v >= 0)
                    a.codeName[v] = code;
            for (const std::string& u : Weapons().underbarrels)
                if (const int v = EnumValue(a.l.underEnum, u); v >= 0)
                    a.underName[v] = u;
        }
        if (const uint32_t wi = ue3::FindPropertyField(menu, "MissionWeaponIndex"))
            a.indexAt = guest::Read32(wi + 0x5C);
        if (const uint32_t si = ue3::FindPropertyField(menu, "MissionWeaponSlotIndex"))
            a.slotAt = guest::Read32(si + 0x5C);
    }
    return a;
}

// Entry `index` of a slot's array (null if out of range).
const uint8_t* LoadoutElement(const LoadoutArrays& a, int slot, int index, uint32_t* count = nullptr)
{
    const uint32_t prop = slot == 2 ? a.meleeProp : a.rangeProp;
    if (!prop)
        return nullptr;
    const uint32_t array = a.menu + guest::Read32(prop + 0x5C);
    const uint32_t data = guest::Read32(array), n = guest::Read32(array + 4);
    if (count)
        *count = n;
    if (!data || index < 0 || uint32_t(index) >= n || n > 4096)
        return nullptr;
    return guest::base + data + uint32_t(index) * a.l.elemSize;
}

// Which slots hold an X-gear weapon (bits 1 Primary, 2 Secondary, 4 Melee): shown by the slots' colour
// (_global.dtXGear, swf_patch::AddXGearTint) and checked by the Confirm (ProcessEvent hook).
void UpdateXGearSelection(uint32_t menu)
{
    static uint32_t shownMenu = 0, shown = ~0u;
    const LoadoutArrays& a = Arrays(menu);
    uint32_t bits = 0;
    if (a.indexAt)
        for (int slot = 0; slot < 3; slot++)
            if (const uint8_t* e = LoadoutElement(a, slot, int32_t(guest::Read32(menu + a.indexAt + uint32_t(slot) * 4))))
                if (a.xgear.count(e[a.l.codeOffset]))
                    bits |= 1u << slot;
    gXGearBits = bits;
    if (bits != shown || menu != shownMenu)
    {
        shown = bits, shownMenu = menu;
        ue3::Call(menu, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtXGear"), float(bits) });
    }
}

// A variant's name, from its underbarrel code: "AT_SCARH_Grenade" -> "Grenade Launcher".
std::string VariantName(const std::string& under)
{
    if (under.empty() || under == "NoUnderbarrelAttachment")
        return "Standard";
    std::string tail = under.substr(under.rfind('_') + 1);
    std::string lower = tail;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (lower == "grenade" || lower == "gl" || lower == "launcher")
        return "Grenade Launcher";
    if (lower == "flamethrower")
        return "Flamethrower";
    if (lower == "grip" || lower == "verticalgrip")
        return "Grip";
    return tail;
}

// The picker's entries for a slot: what the game's own stepping offers there (not the "none" entry; Primary leaves out
// the default Secondary, and Secondary the default Primary, as the game does: mission flags 4 and 2).
std::vector<host::menu::LoadoutEntry> LoadoutEntries(uint32_t menu, int slot)
{
    const LoadoutArrays& a = Arrays(menu);
    std::vector<host::menu::LoadoutEntry> out;
    uint32_t count = 0;
    LoadoutElement(a, slot, 0, &count);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* e = LoadoutElement(a, slot, int(i));
        if (!e)
            break;
        const int code = e[a.l.codeOffset], under = e[a.l.underOffset], flags = e[a.l.missionOffset];
        const auto codeName = a.codeName.find(code);
        if (codeName == a.codeName.end() || codeName->second == "WEAPONCODE_NON" || !(flags & 1) ||
            (slot == 0 && (flags & 4)) || (slot == 1 && (flags & 2)))
            continue;
        host::menu::LoadoutEntry x;
        x.index = int(i);
        x.weapon = Weapons().name.at(codeName->second);
        const auto u = a.underName.find(under);
        x.variant = VariantName(u == a.underName.end() ? "" : u->second);
        x.gfx = Weapons().gfx.count(codeName->second) ? Weapons().gfx.at(codeName->second) : "";
        x.part = x.variant == "Grenade Launcher" ? "under_grenade" : x.variant == "Flamethrower" ? "under_fire" :
                 x.variant == "Grip"             ? "under_grip"    : "under_default";
        const auto rank = Weapons().rank.find(codeName->second);
        x.category = rank == Weapons().rank.end() ? 0 : rank->second;
        x.xgear = a.xgear.count(code) != 0;
        out.push_back(std::move(x));
    }
    return out;
}

// The loadout picker (host::menu::OpenLoadoutPicker) replaces stepping through a slot's weapons: A on a focused slot
// opens it (Enter and a click are A in menus); left and right on it do nothing (FilterButtonInput, taken in the
// ProcessEvent hook) and its arrows are hidden; its pick sets the slot's
// MissionWeaponIndex and the game redraws the slot (StepMissionWeapon(0)).
void LoadoutPickerTick(uint32_t menu)
{
    static bool prevA = false;
    static int quiet = 0;  // ticks since the page last showed: the press that picked must not open the picker again
    quiet = host::menu::IsOpen() ? 0 : quiet + 1;
    const bool a = (host::input::GameButtons() & host::input::VpadA) != 0;  // (A, Enter, or a click on the slot)
    gOpenPicker = false;
    // The slot buttons (Widget_MissionWeaponSlotButton[3]) and which has the focus (CLIK's `_focused`), asked when A is
    // pressed: the menu's focus events miss the mouse moving the focus to Confirm, whose click had opened the picker
    // and left it over the mission's start.
    const uint32_t wp = ue3::FindPropertyField(menu, "Widget_MissionWeaponSlotButton");
    const uint32_t dim = wp ? std::min<uint32_t>(guest::Read32(wp + 0x48), 3) : 0;
    auto slotButton = [&](int slot) { return wp && uint32_t(slot) < dim ? guest::Read32(menu + guest::Read32(wp + 0x5C) + uint32_t(slot) * 4) : 0u; };
    int focusedSlot = -1;
    // ...and only while the loadout screen's own movie has the focus: the game's dialogs (Confirm's "progress will be
    // lost" Yes / No) are movies over it, and the slot that had the focus under them kept it (clicking Yes opened Melee's).
    auto onTop = [&] {
        const uint32_t gfx = guest::Read32(0x104A54E4);  // GGFxEngine
        const uint32_t movie = gfx ? guest::Call(0x039B2288, { gfx, 0, 0 }) : 0;  // FGFxEngine::GetFocusedMovieFromControllerID
        return movie && guest::Read32(movie + 0x64) == menu;                     // (FGFxMovie +0x64: its UGFxMoviePlayer)
    };
    if (a && !prevA && quiet > 15 && !host::menu::IsOpen() && onTop())
        for (int slot = 0; slot < int(dim); slot++)
            if (const uint32_t button = slotButton(slot))
                // (CLIK's `_focused`: its `focused` getter reads as false from here)
                if (ue3::Call(button, "GetBool", { ue3::Arg::MakeString("_focused") }).value_or(0) & 1)
                    focusedSlot = slot;
    prevA = a;
    const LoadoutArrays& arrays = Arrays(menu);
    if (focusedSlot >= 0 && arrays.indexAt)
    {
        const int slot = focusedSlot;
        if (slot >= 0 && slot < 3)
        {
            auto entries = LoadoutEntries(menu, slot);
            const int current = int32_t(guest::Read32(menu + arrays.indexAt + uint32_t(slot) * 4));
            dtlog::Important("[weapons] frame %llu: loadout picker for slot %d (%zu entries)\n",
                (unsigned long long)cafe::gx2::FrameCount(), slot, entries.size());
            if (!entries.empty())
                host::menu::OpenLoadoutPicker(slot, std::move(entries), current);
        }
    }
    int slot = 0, index = 0;
    if (host::menu::TakeLoadoutPick(slot, index) && arrays.indexAt && slot >= 0 && slot < 3)
    {
        guest::Write32(menu + arrays.indexAt + uint32_t(slot) * 4, uint32_t(index));
        ue3::Call(menu, "StepMissionWeapon", { 0 });
        // The slot's picture and name: UpdateWeaponSlotButton(Widget_MissionWeaponSlotButton[slot], code, underbarrel, slot).
        const uint32_t widget = slotButton(slot);
        if (const uint8_t* e = LoadoutElement(arrays, slot, index); e && widget)
            ue3::Call(menu, "UpdateWeaponSlotButton", { widget, uint32_t(e[arrays.l.codeOffset]), uint32_t(e[arrays.l.underOffset]),
                                                         uint32_t(slot) });
        dtlog::Important("[weapons] slot %d: entry %d picked (now %d; slot buttons %u, %08X)\n", slot, index,
            int32_t(guest::Read32(menu + arrays.indexAt + uint32_t(slot) * 4)), dim, widget);
    }
}

void ExtraWeaponsTick()
{
    static const bool on = settings::GetBool("gameplay.extra_weapons", true);
    static uint32_t done = 0;  // the menu object extended last
    if (!on || !assets::coalesced::Loaded())
        return;
    uint32_t menu = 0;
    {
        std::lock_guard lock(gMoviesMutex);
        for (uint32_t p : gOpen)
            if (auto it = gMovieOf.find(p); it != gMovieOf.end() && it->second.find("soloplay_menu") != std::string::npos)
                menu = p;
    }
    // The picker belongs to the loadout screen: closed when the menu is gone, a movie or cutscene plays, or the game has
    // left the front end (a mission starting).
    if (host::menu::LoadoutPickerOpen() &&
        (!menu || game_ui::CutsceneOrMovie() || game_ui::Current() == game_ui::State::Gameplay))
        host::menu::CloseLoadoutPicker("loadout screen left");
    if (!menu || !ue3::IsA(menu, "D3FrontEnd_SingleplayMenu"))
    {
        gXGearBits = 0, gSPMenu = 0, gSlotFocused = false;
        return;
    }
    gSPMenu = menu;
    if (menu != done)
    {
        done = menu;
        if (getenv("DT_DUMP_GAMEPLAY"))
            DumpClassChain(menu, "single-player menu");
        ExtendMissionWeapons(menu, "MissionRangeWeapons", Weapons().range, true);
        ExtendMissionWeapons(menu, "MissionMeleeWeapons", Weapons().melee, false);
    }
    UpdateXGearSelection(menu);
    LoadoutPickerTick(menu);
    // The slots' arrows (Widget_MissionWeaponLeft/Right, one per slot) are hidden: the picker replaces stepping. Again
    // every few ticks, as the game shows them with the focus.
    static int ticks = 0;
    if (++ticks % 4 == 0)
        for (const char* name : { "Widget_MissionWeaponLeft", "Widget_MissionWeaponRight" })
            if (const auto prop = ue3::FindProperty(menu, name))
            {
                const uint32_t data = guest::Read32(prop->address), count = guest::Read32(prop->address + 4);
                for (uint32_t i = 0; data && i < count && i < 8; i++)
                    if (const uint32_t widget = guest::Read32(data + i * 4))
                        ue3::Call(widget, "SetBool", { ue3::Arg::MakeString("_visible"), false });
            }
}

// Crosshair (gameplay.crosshair): the game shows its reticle at all times; "aiming" (the default) hides it unless aim
// or fire is held (or was within a second), through the HUD movie's own D3MovieHUDContent.SetReticleVisible. Only
// changes are sent, and only in gameplay, so the game's own hiding (cutscenes, menus) is left alone.
void CrosshairTick(uint32_t engine)
{
    static uint64_t lastAim = 0;
    static int shown = -1;  // what was sent last (-1: nothing, the game's own state)
    static int ticks = 0;
    const uint64_t frame = cafe::gx2::FrameCount();
    if (host::input::HeldButtons() & (host::input::VpadZL | host::input::VpadZR))
        lastAim = frame;
    const bool hideWhenIdle = settings::Get("gameplay.crosshair", "always") != "always";
    if (game_ui::Current() != game_ui::State::Gameplay || (!hideWhenIdle && shown != 0))
        return;
    const int want = !hideWhenIdle || frame < lastAim + 60 ? 1 : 0;
    if (want == shown && ++ticks % 30)  // (sent again twice a second: a respawn or weapon change may show it)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t hud = pc ? ue3::GetObject(pc, "myHUD").value_or(0) : 0;
    const uint32_t movie = hud ? ue3::GetObject(hud, "m_HudMovie").value_or(0) : 0;
    if (!movie)
        return;
    if (want != shown)
        dtlog::Important("[ui] crosshair %s\n", want ? "shown" : "hidden (not aiming)");
    ue3::Call(movie, "SetReticleVisible", { ue3::Arg(bool(want)) });
    shown = hideWhenIdle ? want : -1;
}

// DT_SHOW_GUIDE (environment, testing): in gameplay, the HUD's operation guide (DevilsThirdHud.ShowOperationGuide: button
// 1 and 2 from the OperationButtonInfo list, NONE 0, A 1, B 2 ... LT 7, LThumb 13, RThumb 14; text, text left and right of
// the button) cycled every 200 frames, to check the prompts in it.
// The menus' mouse pointer (host::input::GetPointer) with the game's own PC cursor (D3GfxMovieMouseCursorHud, which
// D3HUD.PostBeginPlay makes on a PC build: IsConsoleBuildHook), shown outside gameplay while the keyboard and mouse are
// in use and the PC page is closed.
constexpr uint32_t GGFxEngine = 0x104A54E4, GGFxEnabled = 0x10498418;
void PointerTick(uint32_t engine)
{
    static uint64_t lastSerial = 0;
    static bool lastDown = false, shown = false;
    static int lastWheel = 0;
    const host::input::Pointer p = host::input::GetPointer();
    const bool active = p.valid && game_ui::PointerWanted() && !host::menu::IsOpen();
    host::menu::SetMenuPointer(active);
    // The menus' input mode: D3UIManager.InputInterfaceType (eInputInterfaceType: EIIT_PAD 0, EIIT_KEYBOARD 1), which
    // the movies ask for (GetInputInterfaceType) and their D3Buttons follow: mouse roll-over and clicks only in keyboard
    // mode. The Wii U build is always PAD; keyboard while the keyboard and mouse are in use.
    // (every tick from the first: D3HUD.PostBeginPlay makes the game's cursor only if the mode is already keyboard)
    static uint32_t uiSeen = 0, modeAt = 0;
    if (const uint32_t ui = ue3::UIManager(); ui && ui != uiSeen)
    {
        uiSeen = ui, modeAt = 0;
        if (ue3::IsA(ui, "D3UIManager"))
            if (const auto prop = ue3::FindProperty(ui, "InputInterfaceType"))
                modeAt = prop->address;
    }
    if (modeAt)
    {
        const uint8_t want = (host::input::ActiveDevice() == host::input::Device::Keyboard || getenv("DT_TEST_POINTER")) ? 1 : 0;
        if (guest::Read8(modeAt) != want)
        {
            guest::Write8(modeAt, want);
            dtlog::Important("[ui] menus' input mode: %s\n", want ? "keyboard and mouse" : "controller");
        }
    }
    const uint32_t gfx = guest::Read32(GGFxEngine);
    // Mouse moves go to the menus at most 60 times a second: each one runs every menu's mouse listeners (our search for
    // what is under the pointer, and the game's own, which calls UpdateMousePosition in UnrealScript). Sent as often
    // as the mouse reported them, uncapped, they had cost 25 ms a frame (the results screen at 14 fps). Clicks and the
    // wheel are never held back.
    static auto lastMove = std::chrono::steady_clock::time_point{};
    const auto nowMove = std::chrono::steady_clock::now();
    const bool moveDue = p.serial != lastSerial && nowMove - lastMove >= std::chrono::milliseconds(16);
    if (active && gfx && guest::Read32(GGFxEnabled) && (moveDue || p.down != lastDown || p.wheel != lastWheel))
    {
        if (moveDue || p.down != lastDown)
            lastMove = nowMove;
        // Every open movie (FGFxEngine::GetOpenMovie; FGFxMovie +0x34 its Scaleform view, +0x64 its UGFxMoviePlayer)
        // gets Scaleform mouse events directly: the view's HandleEvent (the vtable entry FGFxEngine::InputTouch calls),
        // with Scaleform 4.1's MouseEvent {type, modifiers, x, y, scroll delta, button, mouse index}. (The engine's own
        // touch path serves only the GamePad screen's movies.) The movies' script (swf_patch::AddMenuMouse) does the
        // rest: the button under the pointer takes the focus, a click on it confirms.
        const uint32_t focused = guest::Call(0x039B2288, { gfx, 0, 0 });  // FGFxEngine::GetFocusedMovieFromControllerID
        std::vector<std::pair<uint32_t, bool>> clickables;  // open movies at a click (player, focused)
        for (uint32_t m = 0; m < 16; m++)
        {
            const uint32_t movie = guest::Call(0x039C02B0, { gfx, m });
            if (!movie)
                break;
            const uint32_t player = guest::Read32(movie + 0x64), view = guest::Read32(movie + 0x34);
            if (!player || !view)
                continue;
            // (menus only: the HUD's pieces, the GamePad screen's and the cursor's movie have no use for the mouse, and
            // their listeners cost time on every move)
            {
                std::lock_guard lock(gMoviesMutex);
                auto it = gMovieOf.find(player);
                if (it != gMovieOf.end() && IsPlayMovie(it->second))
                    continue;
            }
            if (ue3::GetBool(player, "bIgnoreMouseInput").value_or(false))
                ue3::SetBool(player, "bIgnoreMouseInput", false);
            // Scaleform takes mouse events only for its mouse cursors (MovieImpl::SetMouseCursorCount; none on the Wii U).
            if (guest::Call(0x03F503D0, { view }) == 0)
                guest::Call(0x03F503BC, { view, 1 });
            static const uint32_t event = guest::SystemAlloc(32, 4);
            auto send = [&](uint32_t type, float scroll = 0) {
                guest::Write32(event, type), guest::Write32(event + 4, 0);
                guest::Write32(event + 8, std::bit_cast<uint32_t>(p.x)), guest::Write32(event + 12, std::bit_cast<uint32_t>(p.y));
                guest::Write32(event + 16, std::bit_cast<uint32_t>(scroll)), guest::Write32(event + 20, 0), guest::Write32(event + 24, 0);
                guest::Call(guest::Read32(guest::Read32(view + 4) + 0x11C), { view, event });  // MovieImpl::HandleEvent
            };
            enum : uint32_t { MouseMove = 1, MouseDown = 2, MouseUp = 3, MouseWheel = 4 };
            if (moveDue || p.down != lastDown)  // (a click also brings the position up to date first)
                send(MouseMove);
            // A click (the left button released): counted in the focused menu's _global.dtClick, whose script confirms
            // the button under the pointer (swf_patch::AddMenuMouse). Mouse buttons never reach the movies themselves:
            // the game's own click works only where nothing covers a button, and then confirmed a second time.
            (void)MouseDown, (void)MouseUp;
            if (p.down != lastDown && !p.down)
                clickables.push_back({ player, movie == focused });
            if (p.down != lastDown && !p.down && movie != focused && m == 0 && !focused)
                dtlog::Important("[ui] click at %.0f,%.0f: no menu has the focus\n", p.x, p.y);
            if (p.wheel != lastWheel)
                send(MouseWheel, float(p.wheel - lastWheel) * 3);
            static std::set<uint32_t> seen;
            if (getenv("DT_TEST_POINTER") && seen.insert(player).second)
                dtlog::Important("[test] open movie %u: %s\n", m, ue3::ClassName(player).c_str());
        }
        // A click goes to the focused menu, or, if nothing of its own is under the pointer, to another open menu that has a
        // button or prompt there (the brightness and score screens' "A ✓" lie in a movie above the focused one).
        if (!clickables.empty())
        {
            auto read = [](uint32_t player, const char* v) {
                std::string text;
                if (const uint32_t str = ue3::Call(player, "GetVariableString", { ue3::Arg::MakeString(v) }).value_or(0))
                    for (uint32_t k = 0; k < 2000 && guest::Read16(str + k * 2); k++)
                        text += char(guest::Read16(str + k * 2));
                return text == "undefined" ? std::string() : text;
            };
            std::stable_partition(clickables.begin(), clickables.end(), [](const auto& c) { return c.second; });
            uint32_t target = clickables.front().first;
            std::string what;
            for (const auto& [player, isFocused] : clickables)
            {
                const std::string hover = read(player, "_global.dtHover"), prompt = read(player, "_global.dtPrompt");
                if (getenv("DT_TEST_POINTER"))
                    dtlog::Important("[test]   %s%s: hover '%s' prompt '%s' mouse %s\n", ue3::ClassName(player).c_str(),
                        isFocused ? " (focused)" : "", hover.c_str(), prompt.c_str(), "");
                if (!hover.empty() || !prompt.empty())
                {
                    target = player, what = !hover.empty() ? hover : "the " + prompt + " prompt";
                    break;
                }
            }
            static uint32_t clicks = 0;
            ue3::Call(target, "SetVariableNumber", { ue3::Arg::MakeString("_global.dtClick"), float(++clicks) });
            dtlog::Important("[ui] click at %.0f,%.0f in %s on %s\n", p.x, p.y, ue3::ClassName(target).c_str(),
                what.empty() ? "nothing" : what.c_str());
        }
        {
            static int logged = 0;
            if (logged < 20 && getenv("DT_TEST_POINTER"))
                logged++, dtlog::Important("[test] pointer %.0f,%.0f%s at frame %llu\n", p.x, p.y,
                    p.down != lastDown ? (p.down ? " down" : " up") : "",
                    (unsigned long long)cafe::gx2::FrameCount());
        }
    }
    if (active || !p.valid)
    {
        if (moveDue || p.down != lastDown || !active)
            lastSerial = p.serial;
        lastDown = p.down, lastWheel = p.wheel;
    }
    // The game's cursor.
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t hud = pc ? ue3::GetObject(pc, "myHUD").value_or(0) : 0;
    if (!hud || !ue3::IsA(hud, "D3HUD"))
        return;
    uint32_t cursor = ue3::GetObject(hud, "hudMouseCursor").value_or(0);
    if (!cursor)
        return;
    // The overlay draws the cursor (the game's own shape: host::menu::DrawGameCursor): the game's, part of the HUD,
    // was not shown over the mission score screen, and lagged the mouse by the game picture's frames. It stays hidden.
    static uint32_t cursorSeen = 0;
    if (cursor != cursorSeen)
        cursorSeen = cursor, shown = true;
    if (shown)
    {
        ue3::Call(hud, "ShowMouseCursor", { ue3::Arg(false), ue3::Arg(ue3::UIManager()) });
        shown = false;
    }
}

// The player escaping a throw (DevilsThirdPlayerPawn's PlayerThrowBeaten* states, where UpdateThrowEscapeCount counts
// stick movement): the keyboard's stick sweeps then (host::input::SetThrowEscape).
void ThrowEscapeTick(uint32_t engine)
{
    static int ticks = 0;
    if (++ticks % 3)
        return;
    bool escaping = false;
    if (game_ui::Current() != game_ui::State::FrontEnd)
    {
        const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
        const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
        const uint32_t pawn = pc ? ue3::GetObject(pc, "Pawn").value_or(0) : 0;
        if (pawn && ue3::IsA(pawn, "DevilsThirdPlayerPawn"))
        {
            static uint32_t names[5] = {};
            static const char* const states[5] = { "PlayerThrowBeaten", "PlayerThrowBeatenLoop", "PlayerThrowBeatenLudmilla",
                "PlayerThrowBeatenForDSM", "PlayerThrowBeatenNotDead" };
            const uint32_t state = ue3::Call(pawn, "GetStateName").value_or(0);
            for (int i = 0; i < 5; i++)
            {
                if (!names[i])
                    names[i] = ue3::FindNameIndex(states[i]);
                escaping |= state && state == names[i];
            }
        }
    }
    host::input::SetThrowEscape(escaping);
}

// F12 also logs where the player stands (`[pos]`), and DT_TEST_TELEPORT="x,y,z[,yaw]" (testing) puts the player there
// once gameplay has run 100 ticks (a list "x,y,z[,yaw];..." visits each in turn, 480 ticks apart): a spot reached once
// in play can then be reached by a scripted run.
std::atomic<bool> gLogPosition{ false };

void PositionTick(uint32_t engine)
{
    static const char* const teleport = getenv("DT_TEST_TELEPORT");
    static int gameplayTicks = 0;
    static int visited = 0;
    static const std::vector<std::string> spots = [] {
        std::vector<std::string> v;
        if (const char* e = getenv("DT_TEST_TELEPORT"))
            for (std::string s = e; !s.empty();)
            {
                const size_t semi = s.find(';');
                v.push_back(s.substr(0, semi));
                s = semi == std::string::npos ? "" : s.substr(semi + 1);
            }
        return v;
    }();
    const bool teleported = visited >= int(spots.size());
    const bool log = gLogPosition.exchange(false);
    const bool playing = game_ui::Current() == game_ui::State::Gameplay;
    gameplayTicks = playing ? gameplayTicks + 1 : 0;
    const bool move = teleport && !teleported && gameplayTicks > 100 + 480 * visited;
    if (!log && !move)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t pawn = pc ? ue3::GetObject(pc, "Pawn").value_or(0) : 0;
    const auto loc = pawn ? ue3::FindProperty(pawn, "Location") : std::nullopt;
    const auto rot = pc ? ue3::FindProperty(pc, "Rotation") : std::nullopt;
    if (!loc || !rot)
        return;
    auto f = [](uint32_t a) { return std::bit_cast<float>(guest::Read32(a)); };
    if (move)
    {
        const std::string spot = spots[visited++];
        float x = 0, y = 0, z = 0;
        int yaw = 0;
        const int n = sscanf(spot.c_str(), "%f,%f,%f,%d", &x, &y, &z, &yaw);
        if (n >= 3)
        {
            const auto r = ue3::Call(pawn, "SetLocation", { ue3::Arg::MakeVector(x, y, z) });
            dtlog::Important("[pos] SetLocation returned %d (%s)\n", r ? int(*r) : -1, r ? "called" : "not found");
            if (!r || !*r)
            {
                // (refused, e.g. encroaching geometry, or not callable: write the location directly)
                guest::Write32(loc->address, std::bit_cast<uint32_t>(x));
                guest::Write32(loc->address + 4, std::bit_cast<uint32_t>(y));
                guest::Write32(loc->address + 8, std::bit_cast<uint32_t>(z));
            }
            if (n == 4)
                guest::Write32(rot->address + 4, uint32_t(yaw));
            dtlog::Important("[pos] DT_TEST_TELEPORT to %.1f,%.1f,%.1f\n", x, y, z);
        }
    }
    if (log || move)
        dtlog::Important("[pos] player at %.1f,%.1f,%.1f yaw %d pitch %d\n", f(loc->address), f(loc->address + 4),
            f(loc->address + 8), int32_t(guest::Read32(rot->address + 4)), int32_t(guest::Read32(rot->address)));
}

void ShowGuideTick(uint32_t engine)
{
    static const bool on = getenv("DT_SHOW_GUIDE") != nullptr;
    static int ticks = 0;
    if (!on || game_ui::Current() != game_ui::State::Gameplay || ++ticks % 200 != 100)
        return;
    struct Guide
    {
        uint32_t b1, b2;
        const char *text, *left, *right;
    };
    // DevilsThirdPlayerInput.eButtonTypes: 1-4 A B X Y, 5-8 LB RB LT RT, 9-12 D-pad up down left right, 13/14 stick
    // clicks, 15/16 sticks, 21 gyro, 23/24 sticks moving, 41 left stick up/down, 42 START.
    static const Guide guides[] = { { 13, 0, "Dash", "Press", "while running" }, { 2, 13, "Dodge", "", "" },
        { 13, 14, "Slide", "", "" }, { 1, 2, "A B", "", "" }, { 3, 4, "X Y", "", "" }, { 5, 6, "LB RB", "", "" },
        { 7, 8, "LT RT", "", "" }, { 9, 10, "Up Down", "", "" }, { 11, 12, "Left Right", "", "" },
        { 15, 16, "Sticks", "", "" }, { 23, 24, "Sticks moving", "", "" }, { 41, 42, "Stick up/down, START", "", "" },
        { 21, 0, "Gyro", "", "" } };
    // DT_SHOW_GUIDE=b1,b2;b1,b2... shows only those (button names as text); any other value cycles the list above.
    static const std::vector<Guide> custom = [] {
        std::vector<Guide> v;
        static std::vector<std::string> names;
        const std::string spec = getenv("DT_SHOW_GUIDE") ? getenv("DT_SHOW_GUIDE") : "";
        names.reserve(64);
        for (size_t at = 0; at < spec.size();)
        {
            const size_t end = std::min(spec.find(';', at), spec.size());
            unsigned b1 = 0, b2 = 0;
            if (sscanf(spec.substr(at, end - at).c_str(), "%u,%u", &b1, &b2) >= 1)
            {
                names.push_back(std::to_string(b1) + "+" + std::to_string(b2));
                v.push_back({ b1, b2, names.back().c_str(), "", "" });
            }
            at = end + 1;
        }
        return v;
    }();
    const Guide& g = custom.empty() ? guides[size_t(ticks / 200) % std::size(guides)] : custom[size_t(ticks / 200) % custom.size()];
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t hud = pc ? ue3::GetObject(pc, "myHUD").value_or(0) : 0;
    if (!hud)
        return;
    const auto r = ue3::Call(hud, "ShowOperationGuide", { ue3::Arg(g.b1), ue3::Arg(g.b2), ue3::Arg::MakeString(g.text),
        ue3::Arg::MakeString(g.left), ue3::Arg::MakeString(g.right) });
    dtlog::Important("[test] guide %u+%u \"%s\" at frame %llu: %s\n", g.b1, g.b2, g.text, (unsigned long long)cafe::gx2::FrameCount(),
        r ? "shown" : "call failed");
}

// DT_DUMP_HUD (environment, testing): once in gameplay, the HUD movie's canvas objects (class names) and its
// properties whose names suggest the crosshair.
void DumpHudTick(uint32_t engine)
{
    static const bool on = getenv("DT_DUMP_HUD") != nullptr;
    static int ticks = 0;
    if (!on || game_ui::Current() != game_ui::State::Gameplay || ++ticks != 300)
        return;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    const uint32_t hud = pc ? ue3::GetObject(pc, "myHUD").value_or(0) : 0;
    const uint32_t movie = hud ? ue3::GetObject(hud, "m_HudMovie").value_or(0) : 0;
    const uint32_t canvas = movie ? ue3::GetObject(movie, "m_hudCanvas").value_or(0) : 0;
    dtlog::Important("[test] hud %s, movie %s, canvas %s\n", ue3::ClassName(hud).c_str(), ue3::ClassName(movie).c_str(),
        ue3::ClassName(canvas).c_str());
    if (const auto slots = canvas ? ue3::FindProperty(canvas, "m_canvasObject") : std::nullopt)
        for (uint32_t k = 0; k < slots->size / 4 + 16 && k < 40; k++)
            if (const uint32_t o = guest::Read32(slots->address + k * 4); o >= 0x10000000 && o < 0x50000000)
                dtlog::Important("[test]   canvas object %u: %s\n", k, ue3::ClassName(o).c_str());
    for (uint32_t object : { hud, movie, canvas })
        for (uint32_t c = object ? guest::Read32(object + 0x34) : 0; c; c = guest::Read32(c + 0x48))
            for (uint32_t f = guest::Read32(c + 0x4C); f; f = guest::Read32(f + 0x40))
            {
                std::string n = ue3::PathName(f);
                n = n.substr(n.find_last_of(".:") + 1);
                std::string lower = n;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                for (const char* w : { "sight", "reticle", "cross", "aim", "cursor", "guide", "tutorial", "hint", "operation", "action" })
                    if (lower.find(w) != std::string::npos)
                    {
                        std::string params;
                        if (ue3::ClassName(f) == "Function" && lower.find("operationguide") != std::string::npos)
                            for (uint32_t q = guest::Read32(f + 0x4C); q; q = guest::Read32(q + 0x40))
                            {
                                std::string pn = ue3::PathName(q);
                                params += " " + ue3::ClassName(q) + ":" + pn.substr(pn.find_last_of(".:") + 1);
                            }
                        dtlog::Important("[test]   %s.%s (%s)%s\n", ue3::ClassName(object).c_str(), n.c_str(), ue3::ClassName(f).c_str(),
                            params.c_str());
                        break;
                    }
            }
}

// DT_START_CHAPTER=<N> (environment, testing): two seconds into the front end, start chapter N directly, as the game's
// own test traveller does: `start SP_GAME_P?ChapterMap=...` with the map from [ChapterN DevilsThirdUIDataProvider_SpChapter]
// (the table skips 2 and 6: Chapter3 is the second level).
void StartChapterTick(uint32_t engine)
{
    static const char* chapter = getenv("DT_START_CHAPTER");
    static int frontEndTicks = 0;
    if (!chapter || game_ui::Current() != game_ui::State::FrontEnd || ++frontEndTicks != 120)
        return;
    const std::string name = std::string("Chapter") + chapter;
    const std::string map = assets::coalesced::Value("", name + " DevilsThirdUIDataProvider_SpChapter", "MapName");
    if (map.empty())
    {
        dtlog::Important("[test] no map for %s\n", name.c_str());
        return;
    }
    const size_t q = map.find('?');
    const std::string command = "start SP_GAME_P?ChapterMap=" + map.substr(0, q) + (q == std::string::npos ? "" : map.substr(q)) +
                                "?game=DevilsThirdGame.D3GameInfoSingle?ChapterName=" + name;
    const uint32_t world = ue3::Call(engine, "GetCurrentWorldInfo").value_or(0);
    const uint32_t pc = world ? ue3::Call(world, "GetALocalPlayerController").value_or(0) : 0;
    dtlog::Important("[test] %s: %s\n", command.c_str(),
        pc && ue3::Call(pc, "ConsoleCommand", { ue3::Arg::MakeString(command) }) ? "sent" : "no player controller");
}

// Sounds for our settings page: the game's own UI sounds, played on the Game Thread from its per-frame tick.
PPC_FUNC(EngineTick)
{
    gEngineTicks.fetch_add(1, std::memory_order_relaxed);
    {
        const PPCContext saved = ctx;
        game_settings::Tick(saved.r[3].u32);  // (first: a save queued by the closing page runs before the game's
        OptionsTick();                         // Options under it close)
        PromptsTick();
        CinematicTick(saved.r[3].u32);
        MouseSpeedTick(saved.r[3].u32);
        TurnRateTick(saved.r[3].u32);
        StartChapterTick(saved.r[3].u32);
        DumpTitleTick();
        DumpMovieTick();
        ExtraWeaponsTick();
        LogWeaponTick(saved.r[3].u32);
        DumpGameplayTick(saved.r[3].u32);
        GrantXGearTick(saved.r[3].u32);
        DumpSettingsTick(saved.r[3].u32);
        DumpHudTick(saved.r[3].u32);
        CrosshairTick(saved.r[3].u32);
        ShowGuideTick(saved.r[3].u32);
        PointerTick(saved.r[3].u32);
        ThrowEscapeTick(saved.r[3].u32);
        PositionTick(saved.r[3].u32);
        TitleTick();
        ctx = saved;
    }
    host::menu::GameTick();
    if (const uint32_t sounds = host::menu::TakeSounds())
    {
        static const char* const names[] = { "PLAY_UI_CURSOR", "PLAY_UI_OK", "PLAY_UI_CANCEL" };
        static uint32_t fnames[3] = {};  // each an FName (index, number) in guest memory, made on first use
        const PPCContext saved = ctx;
        for (int i = 0; i < 3; i++)
        {
            if (!(sounds & (1u << i)))
                continue;
            if (!fnames[i])
            {
                const uint32_t text = guest::SystemAlloc(32);
                guest::WriteString(text, names[i], 32);
                fnames[i] = guest::SystemAlloc(8);
                guest::Call(FName_FromString, { fnames[i], text, 1 });  // FNAME_Add
            }
            ctx.f[1].ps0 = 0;  // as the game's menus pass
            guest::Call(UWwiseAudio_Static_PlayUISound, { fnames[i] });
        }
        ctx = saved;
    }
    hooks::Original(UGameEngine_Tick)(ctx, base);
}
// The game's Coalesced ini and localisation, decrypted in place at boot: kept in memory for the Controls tab.
PPC_FUNC(DecryptData)
{
    const uint32_t data = ctx.r[3].u32, size = ctx.r[4].u32;
    hooks::Original(appDecryptData)(ctx, base);
    assets::coalesced::Offer(base + data, size);
}
std::u16string ReadWide(uint32_t p, uint32_t max = 256)
{
    std::u16string s;
    for (uint32_t i = 0; p && i < max; i++)
    {
        const char16_t c = char16_t(guest::Read16(p + i * 2));
        if (!c)
            break;
        s += c;
    }
    return s;
}

// Text that names buttons follows the device in use and the player's bindings (host::button_text).
PPC_FUNC(LocalizeText)
{
    const uint32_t result = ctx.r[3].u32;
    const std::u16string section = ReadWide(ctx.r[4].u32), key = ReadWide(ctx.r[5].u32);
    hooks::Original(Localize)(ctx, base);
    uint32_t data = guest::Read32(result);
    const uint32_t count = guest::Read32(result + 4);
    if (count > 4096)
        return;
    std::u16string text;
    for (uint32_t i = 0; data && i + 1 < count; i++)
        text += char16_t(guest::Read16(data + i * 2));
    std::optional<std::u16string> rewritten;
    // The 9K38 Igla's name is empty in the game's text (it was never offered in a menu before the extra Score Attack
    // weapons, gameplay.extra_weapons).
    if (text.empty() && section == u"NormalWeapons" && key == u"ID_54")
        rewritten = u"9K38 Igla";
    // The SOD and USA katanas (extra Score Attack weapons) are both called "Katana", like the game's own (UID_8).
    else if (text == u"Katana" && section == u"Undefined" && key == u"UID_9")
        rewritten = u"SOD Katana";
    else if (text == u"Katana" && section == u"NormalWeapons" && key == u"ID_1")
        rewritten = u"USA Katana";
    else if (!text.empty())
        rewritten = host::button_text::Rewrite(section, key, text);
    if (!rewritten)
        return;
    {
        auto narrow = [](const std::u16string& w) {
            std::string n;
            for (char16_t c : w)
                n += c < 0x80 ? char(c) : '?';
            return n;
        };
        LogOnce("text rewritten", narrow(section) + "." + narrow(key) + ": " + narrow(rewritten->substr(0, 90)));
    }
    const uint32_t need = uint32_t(rewritten->size()) + 1;
    if (need > guest::Read32(result + 8))
    {
        const PPCContext saved = ctx;
        data = guest::Call(appRealloc, { data, need * 2, 8 });
        ctx = saved;
        if (!data)
            return;
        guest::Write32(result, data), guest::Write32(result + 8, need);
    }
    for (uint32_t i = 0; i < need - 1; i++)
        guest::Write16(data + i * 2, (*rewritten)[i]);
    guest::Write16(data + (need - 1) * 2, 0);
    guest::Write32(result + 4, need);
}
} // namespace

namespace game_ui
{
State Current()
{
    const uint64_t frame = cafe::gx2::FrameCount();
    std::lock_guard lock(gMoviesMutex);
    if (gLastBinkFrame && frame < gLastBinkFrame + 10)
        return State::Busy;
    bool hud = false, frontEnd = false, other = false, pause = false;
    for (uint32_t player : gOpen)
    {
        auto it = gMovieOf.find(player);
        const std::string movie = it == gMovieOf.end() ? std::string() : it->second;
        if (IsPauseMenu(movie))
            pause = true;
        else if (movie.rfind("ui_frontend.", 0) == 0)
            frontEnd = true;
        else if (IsPlayMovie(movie))
            hud |= movie.find("hud") != std::string::npos;
        else
            other = true;
    }
    if (pause)
        return State::PauseMenu;
    if (other)
        return State::Busy;
    if (hud)
        return gCinematic ? State::Busy : State::Gameplay;
    return frontEnd ? State::FrontEnd : State::Busy;
}

bool PointerWanted()
{
    const uint64_t frame = cafe::gx2::FrameCount();
    std::lock_guard lock(gMoviesMutex);
    if (gLastBinkFrame && frame < gLastBinkFrame + 10)
        return false;
    bool menu = false, hud = false;
    for (uint32_t player : gOpen)
    {
        auto it = gMovieOf.find(player);
        const std::string movie = it == gMovieOf.end() ? std::string() : it->second;
        if (IsPlayMovie(movie))
            hud |= movie.find("hud") != std::string::npos;
        else
            menu = true;  // the pause menu, front-end menus, the score screen, dialogs
    }
    (void)hud;
    return menu;  // (cinematic mode with only the HUD's movies: a cutscene, no pointer)
}

const char* Name(State s)
{
    switch (s)
    {
    case State::Gameplay: return "gameplay";
    case State::PauseMenu: return "pause menu";
    case State::FrontEnd: return "front end";
    case State::Busy: return "busy";
    }
    return "?";
}
} // namespace game_ui

DT_HOOK(FGFxFileOpener_OpenFile, OpenFile, "FGFxFileOpener::OpenFile");
DT_HOOK(FGFxExternalInterface_Callback, ExternalInterfaceCallback, "FGFxExternalInterface::Callback");
DT_HOOK(FGFxFile_ctor, GFxFileCtor, "FGFxFile::FGFxFile");
DT_HOOK(DrawStringZ, CanvasText, "DrawStringZ");
DT_HOOK(AWorldInfo_execIsConsoleBuild, IsConsoleBuildHook, "AWorldInfo::execIsConsoleBuild");
DT_HOOK(appRequestExit, RequestExitHook, "appRequestExit");
DT_HOOK(UGFxMoviePlayer_Load, MovieLoad, "UGFxMoviePlayer::Load");
DT_HOOK(UGFxMoviePlayer_Start, MovieStart, "UGFxMoviePlayer::Start");
DT_HOOK(UGFxMoviePlayer_Close, MovieClose, "UGFxMoviePlayer::Close");
DT_HOOK(FFullScreenMovieBink_BinkDecodeFrame, BinkDecodeFrame, "FFullScreenMovieBink::BinkDecodeFrame");
DT_HOOK(UGameEngine_Tick, EngineTick, "UGameEngine::Tick");
DT_HOOK(UObject_ProcessEvent, ProcessEvent, "UObject::ProcessEvent");
DT_HOOK(appDecryptData, DecryptData, "appDecryptData");
DT_HOOK(Localize, LocalizeText, "Localize");

namespace game_ui
{
bool CanOpenOriginalOptions()
{
    std::lock_guard lock(gKeptMutex);
    return gTitleUnder || gKept.object != 0;
}

void OpenOriginalOptions()
{
    if (!gTitleUnder.exchange(0))  // (the title's are already open under the page)
        gRunKept = true;
}

void PageClosed()
{
    if (gTitleUnder)
        gCloseTitleOptions = true;
}

void RequestPositionLog() { gLogPosition = true; }

bool CutsceneOrMovie()
{
    const uint64_t frame = cafe::gx2::FrameCount();
    return gCinematic || (gLastBinkFrame && frame < gLastBinkFrame + 10);
}
} // namespace game_ui
