#include "overlay.h"

#include "../host/input.h"
#include "../host/menu.h"
#include "../host/window.h"
#include "../assets/game_fonts.h"
#include "../log.h"
#include "device.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <SDL3/SDL.h>

#include "overlay_font.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace gpu::overlay
{
namespace
{
std::mutex gEventsMutex;
std::vector<SDL_Event> gEvents;  // from the window thread, handed to ImGui on the presenting thread

bool gInitialised = false;
Fonts gFonts;

// ImGui font loader for the game's SWF fonts: glyphs are rasterized on demand at the size and density ImGui asks for,
// so text stays sharp at any window size.
float LayoutScale(const assets::SwfFont& f, const ImFontBaked* baked) { return baked->Size / (f.ascent + f.descent); }

bool SrcInit(ImFontAtlas*, ImFontConfig*) { return true; }
bool SrcContains(ImFontAtlas*, ImFontConfig* src, ImWchar c)
{
    return static_cast<const assets::SwfFont*>(src->FontLoaderData)->Find(c) != nullptr;
}
bool BakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*)
{
    const auto& f = *static_cast<const assets::SwfFont*>(src->FontLoaderData);
    if (!src->MergeMode)
    {
        baked->Ascent = ImCeil(f.ascent * LayoutScale(f, baked));
        baked->Descent = ImFloor(-f.descent * LayoutScale(f, baked));
    }
    return true;
}
bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar codepoint, ImFontGlyph* out_glyph,
    float* out_advance_x)
{
    const auto& f = *static_cast<const assets::SwfFont*>(src->FontLoaderData);
    const assets::SwfFont::Glyph* g = f.Find(codepoint);
    if (!g)
        return false;
    const float scale = LayoutScale(f, baked), density = src->RasterizerDensity * baked->RasterizerDensity;
    if (out_advance_x)
    {
        *out_advance_x = g->advance * scale;
        return true;
    }
    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = g->advance * scale;
    const assets::GlyphBitmap bm = assets::Rasterize(*g, scale * density);
    if (bm.width > 0 && bm.height > 0)
    {
        const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, bm.width, bm.height);
        if (pack == ImFontAtlasRectId_Invalid)
            return false;
        ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
        const float top = IM_ROUND(baked->Ascent);
        out_glyph->X0 = float(bm.left) / density;
        out_glyph->Y0 = float(bm.top) / density + top;
        out_glyph->X1 = float(bm.left + r->w) / density;
        out_glyph->Y1 = float(bm.top + r->h) / density + top;
        out_glyph->Visible = true;
        out_glyph->PackId = pack;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, bm.alpha.data(), ImTextureFormat_Alpha8, bm.width);
    }
    return true;
}

ImFont* AddGameFont(const char* name, ImFont* fallback)
{
    const assets::SwfFont* f = assets::GameFont(name);
    if (!f)
        return fallback;
    static ImFontLoader loader = [] {
        ImFontLoader l;
        l.Name = "Scaleform DefineFont3";
        l.FontSrcInit = SrcInit;
        l.FontSrcContainsGlyph = SrcContains;
        l.FontBakedInit = BakedInit;
        l.FontBakedLoadGlyph = LoadGlyph;
        return l;
    }();
    ImFontConfig cfg;
    cfg.FontLoader = &loader;
    cfg.FontLoaderData = const_cast<assets::SwfFont*>(f);
    cfg.SizePixels = 20.0f;
    snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    ImFont* font = ImGui::GetIO().Fonts->AddFont(&cfg);
    // Characters the game's font lacks come from Roboto.
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kOverlayFont), int(sizeof(kOverlayFont)), 20.0f, &merge);
    return font;
}
VkFormat gFormat = VK_FORMAT_UNDEFINED;
uint32_t gMinImages = 0;
float gScale = 0;
ImGuiStyle gBaseStyle;

void CheckResult(VkResult r)
{
    if (r != VK_SUCCESS)
        VulkanFatal("overlay (Dear ImGui)", r, __FILE__, __LINE__);
}

void Init(Device& d)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window positions are not kept; settings live in settings.ini
    io.LogFilename = nullptr;
    // Keyboard and controller navigation. The window thread owns the cursor, so ImGui does not change it.
    // The menu does its own navigation, as the game's menus do; ImGui only reports keys, buttons and the mouse. The
    // window thread owns the cursor, so ImGui does not change it.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplSDL3_InitForVulkan(host::Window());
    // This runs on the GPU thread, not SDL's event thread: no mouse capture, no gamepads opened by the backend (the
    // controller is fed in from host::input instead).
    ImGui_ImplSDL3_SetMouseCaptureMode(ImGui_ImplSDL3_MouseCaptureMode_Disabled);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    gFormat = d.SwapFormat();
    gMinImages = d.SwapMinImageCount();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = d.Instance();
    ii.PhysicalDevice = d.Physical();
    ii.Device = d.Handle();
    ii.QueueFamily = d.QueueFamily();
    ii.Queue = d.Queue();
    // One descriptor set per texture: the fonts, and the loadout picker's weapon pictures (one per weapon and variant, about
    // 120; the pool had room for 16 and the Shotguns list ran out: VK_ERROR_OUT_OF_POOL_MEMORY stopped the game).
    ii.DescriptorPoolSize = std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE, 512);
    ii.MinImageCount = std::max(2u, gMinImages);
    ii.ImageCount = std::max(ii.MinImageCount, d.SwapImageCount());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &gFormat;
    ii.CheckVkResultFn = CheckResult;
    ImGui_ImplVulkan_Init(&ii);

    ImFontConfig font;
    font.FontDataOwnedByAtlas = false;  // the embedded font is static data
    ImFont* roboto = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kOverlayFont), int(sizeof(kOverlayFont)), 20.0f, &font);
    gFonts.label = AddGameFont("NewCezProBNTLGD002_3", roboto);
    gFonts.value = AddGameFont("Mechanical-NewRodinUB", roboto);
    gFonts.tab = AddGameFont("OptiB-NewRodinB85-NTLGDB002", roboto);

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6, s.FrameRounding = 4, s.GrabRounding = 4, s.TabRounding = 4;
    s.WindowPadding = { 14, 12 }, s.FramePadding = { 8, 5 }, s.ItemSpacing = { 10, 8 };
    s.Colors[ImGuiCol_WindowBg].w = 0.94f;
    gBaseStyle = s;
    gInitialised = true;
    dtlog::Important("[overlay] Dear ImGui %s ready\n", IMGUI_VERSION);
}

// Sizes follow the window: designed for 1080 lines, scaled to the swapchain's height.
void Scale(VkExtent2D extent)
{
    const float scale = std::clamp(float(extent.height) / 1080.0f, 0.75f, 3.0f);
    if (scale == gScale)
        return;
    gScale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    s = gBaseStyle;
    s.ScaleAllSizes(scale);
    s.FontScaleMain = scale;
}

// The controller drives ImGui's navigation through host::input (the backend does not own the gamepads).
void FeedController()
{
    ImGuiIO& io = ImGui::GetIO();
    const host::input::MenuPad p = host::input::ReadMenuPad();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;  // ImGui discards gamepad keys without it (the SDL3 backend sets
                                                      // it only for its own gamepad navigation, which is off)
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, p.up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, p.down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, p.left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, p.right);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, p.confirm);  // ImGui: activate
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, p.back);    // ImGui: cancel
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, p.reset);
    io.AddKeyEvent(ImGuiKey_GamepadL1, p.prevPage);
    io.AddKeyEvent(ImGuiKey_GamepadR1, p.nextPage);
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, float v) {
        io.AddKeyAnalogEvent(neg, v < -0.3f, std::max(0.0f, -v));
        io.AddKeyAnalogEvent(pos, v > 0.3f, std::max(0.0f, v));
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, p.stickX);
    stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, p.stickY);
}
} // namespace

const Fonts& MenuFonts() { return gFonts; }

void PushEvent(const SDL_Event& e)
{
    std::lock_guard lock(gEventsMutex);
    if (gEvents.size() < 4096)
        gEvents.push_back(e);
}

bool WantsToDraw()
{
    return host::menu::IsOpen() || host::menu::WantsOverlay() || host::menu::SkipGlyphActive() || host::menu::SubtitlesActive() ||
           host::menu::MenuPointerActive();
}

void Record(Device& device, VkCommandBuffer cmd, VkImageView target, VkExtent2D extent)
{
    if (!gInitialised)
        Init(device);
    if (device.SwapMinImageCount() != gMinImages)
        gMinImages = device.SwapMinImageCount(), ImGui_ImplVulkan_SetMinImageCount(std::max(2u, gMinImages));
    Scale(extent);

    std::vector<SDL_Event> events;
    {
        std::lock_guard lock(gEventsMutex);
        events.swap(gEvents);
    }
    for (const SDL_Event& e : events)
        ImGui_ImplSDL3_ProcessEvent(&e);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // (the Windows cursor is hidden over the game: the PC page draws the game's own, host::menu::DrawGameCursor)
    ImGui::GetIO().MouseDrawCursor = false;
    if (host::menu::IsOpen())
        FeedController();
    ImGui::NewFrame();
    host::menu::Draw();
    ImGui::Render();

    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}
} // namespace gpu::overlay
