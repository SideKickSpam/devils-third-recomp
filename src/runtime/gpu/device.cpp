#include "device.h"
#include "png.h"
#include "exclusive.h"
#include "overlay.h"
#include "../host/window.h"
#include "../settings.h"

#include "../log.h"
#include "../trace.h"

#include <chrono>
#include <string_view>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace gpu
{
const char* VkResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult(?)";
    }
}

void VulkanFatal(const char* expr, VkResult r, const char* file, int line)
{
    trace::Stop(9, "Vulkan call failed: %s -> %s (%d) at %s:%d", expr, VkResultName(r), int(r), file, line);
}

namespace
{
bool gValidate = false;

// Validation messages, each message ID logged for its first few occurrences only (a bad draw repeats every frame).
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    static std::mutex mutex;
    static std::unordered_map<int32_t, uint32_t> seen;
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;
    std::lock_guard lock(mutex);
    const uint32_t n = ++seen[data->messageIdNumber];
    if (n <= 3)
        dtlog::Important("[vulkan] %s%s\n", data->pMessage, n == 3 ? " (further repeats not logged)" : "");
    return VK_FALSE;
}

bool HasInstanceExtension(const char* name)
{
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    for (auto& e : exts)
        if (!strcmp(e.extensionName, name))
            return true;
    return false;
}

bool HasLayer(const char* name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (auto& l : layers)
        if (!strcmp(l.layerName, name))
            return true;
    return false;
}
} // namespace

void SetValidation(bool enable) { gValidate = enable; }

Device& GetDevice()
{
    static Device d;
    return d;
}

bool Device::Init(SDL_Window* w)
{
    window = w;
    if (volkInitialize() != VK_SUCCESS)
    {
        dtlog::Important("[dt-port] no Vulkan loader found; rendering disabled\n");
        return false;
    }

    // ---- instance ----
    Uint32 extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    std::vector<const char*> exts(sdlExts, sdlExts + extCount);
    std::vector<const char*> layers;
    // Exclusive fullscreen (Windows) needs VK_KHR_get_surface_capabilities2 on the instance.
    const char* exclusiveInstanceExt = nullptr;
    const bool exclusiveInstance = exclusive::InstanceExtension(exclusiveInstanceExt) && HasInstanceExtension(exclusiveInstanceExt);
    if (exclusiveInstance)
        exts.push_back(exclusiveInstanceExt);
    const bool validation = gValidate && HasLayer("VK_LAYER_KHRONOS_validation");
    if (validation)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    else if (gValidate)
        dtlog::Important("[dt-port] --validate: VK_LAYER_KHRONOS_validation is not installed (install the Vulkan SDK)\n");

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "dt-port";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    // Core validation plus synchronization validation (missing or wrong barriers).
    const VkValidationFeatureEnableEXT validationFeatures[] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
    VkValidationFeaturesEXT vf{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = validationFeatures;
    if (validation)
        ici.pNext = &vf;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
    {
        dtlog::Important("[dt-port] vkCreateInstance failed; rendering disabled\n");
        return false;
    }
    volkLoadInstance(instance);
    if (validation)
    {
        VkDebugUtilsMessengerCreateInfoEXT mci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger);
    }
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
    {
        dtlog::Important("[dt-port] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }

    // ---- physical device: Vulkan 1.3 with graphics + present; prefer discrete, then integrated GPUs ----
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    int bestScore = -1;
    for (VkPhysicalDevice pd : devices)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU        ? 2
                                                                                : 1;
            if (score > bestScore)
            {
                bestScore = score;
                physical = pd;
                properties = p;
                queueFamily = q;
            }
            break;
        }
    }
    if (!physical)
    {
        dtlog::Important("[dt-port] no Vulkan 1.3 device can present to the window; rendering disabled\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

    // ---- device ----
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &f13;
    // Optional core features the renderer uses when present (it checks Features() before relying on them).
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    enabledFeatures.independentBlend = supported.independentBlend;
    enabledFeatures.depthClamp = supported.depthClamp;
    enabledFeatures.depthBiasClamp = supported.depthBiasClamp;
    enabledFeatures.samplerAnisotropy = supported.samplerAnisotropy;
    enabledFeatures.shaderClipDistance = supported.shaderClipDistance;
    enabledFeatures.shaderCullDistance = supported.shaderCullDistance;
    enabledFeatures.imageCubeArray = supported.imageCubeArray;
    enabledFeatures.fillModeNonSolid = supported.fillModeNonSolid;
    enabledFeatures.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    features.features = enabledFeatures;
    std::vector<const char*> devExts{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    {
        // Optional: VK_EXT_memory_budget, to report how much of the GPU's memory budget the renderer uses.
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> available(n);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data());
        const char* exclusiveExt = nullptr;
        const bool exclusivePlatform = exclusiveInstance && exclusive::DeviceExtension(exclusiveExt);
        for (const VkExtensionProperties& e : available)
            if (std::string_view(e.extensionName) == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)
                devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME), hasMemoryBudget = true;
            else if (std::string_view(e.extensionName) == VK_NV_LOW_LATENCY_2_EXTENSION_NAME)
                devExts.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME), hasLowLatency = true;
            else if (exclusivePlatform && std::string_view(e.extensionName) == exclusiveExt)
                devExts.push_back(exclusiveExt), hasExclusive = true;
            else if (std::string_view(e.extensionName) == VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)
            {
                VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR df{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
                VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
                f2.pNext = &df;
                vkGetPhysicalDeviceFeatures2(physical, &f2);
                if (df.vertexAttributeInstanceRateDivisor)
                    devExts.push_back(VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME), hasAttributeDivisor = true;
            }
    }
    // Instance divisors above 1 (and 0) are a feature of the extension.
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divisorFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
    divisorFeatures.vertexAttributeInstanceRateDivisor = VK_TRUE;
    if (hasAttributeDivisor)
        divisorFeatures.pNext = f13.pNext, f13.pNext = &divisorFeatures;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // ---- per-frame resources ----
    for (Frame& f : frames)
    {
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &f.cmd));
        f.primary = f.cmd;
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &f.acquired));

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = UploadSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &f.upload));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, f.upload, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &f.uploadMemory));
        VK_CHECK(vkBindBufferMemory(device, f.upload, f.uploadMemory, 0));
        VK_CHECK(vkMapMemory(device, f.uploadMemory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&f.uploadPtr)));
    }
    if (!CreateSwapchain())
        return false;
    dtlog::Important("[dt-port] Vulkan: %s (API %u.%u.%u)%s\n", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
        validation ? ", validation on" : "");
    BeginFrame();
    return true;
}

bool Device::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
    // Scan buffers hold display-ready values, so present them unconverted through a UNORM format.
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            chosen = f;
            break;
        }
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, modes.data());
    // display.vsync: fifo (default) waits for the display; with VRR (G-Sync, FreeSync) each present starts a
    // refresh, so the display follows the game. mailbox never blocks and shows the newest frame at each refresh
    // (fixed-refresh monitors); immediate tears. GX2SwapScanBuffers paces frames either way. FIFO is always
    // supported.
    const std::string vsync = settings::Get("display.vsync", "fifo");
    VkPresentModeKHR mode = vsync == "mailbox" ? VK_PRESENT_MODE_MAILBOX_KHR
        : vsync == "immediate"                 ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                               : VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
        mode = VK_PRESENT_MODE_FIFO_KHR;
    auto modeName = [](VkPresentModeKHR m) {
        return m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
            : m == VK_PRESENT_MODE_MAILBOX_KHR    ? "mailbox"
            : m == VK_PRESENT_MODE_FIFO_KHR       ? "fifo"
                                                  : "other";
    };
    static VkPresentModeKHR logged = VkPresentModeKHR(-1);
    if (logged != mode)
    {
        std::string offered;
        for (VkPresentModeKHR m : modes)
            offered += std::string(" ") + modeName(m);
        logged = mode, dtlog::Important("[dt-port] present mode %s (display.vsync = %s; offered:%s)\n", modeName(mode),
            vsync.c_str(), offered.c_str());
    }
    displayGeneration = host::DisplayGeneration();
    presentGeneration = settings::Generation();
    presentModeSetting = vsync;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    swapExtent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{ uint32_t(w), uint32_t(h) };
    if (swapExtent.width == 0 || swapExtent.height == 0)
        return false;  // minimised

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = surface;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount)
        sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (windowShots && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain;
    const bool exclusiveMode = host::CurrentDisplayMode() == host::DisplayMode::Exclusive && host::HasFocus();
    VkSwapchainLatencyCreateInfoNV latency{ VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV };
    latency.latencyModeEnable = VK_TRUE;
    sci.pNext = exclusiveSwapchain.Chain(window, hasExclusive, exclusiveMode, hasLowLatency ? &latency : nullptr);
    VkSwapchainKHR newSwapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &newSwapchain));
    DestroySwapchain();
    swapchain = newSwapchain;
    exclusiveSwapchain.Acquire(device, swapchain);
    boostApplied = false;
    ApplyGpuBoost();
    dtlog::Important("[dt-port] swapchain %ux%u, %u images, %s\n", swapExtent.width, swapExtent.height, sci.minImageCount,
        host::DisplayModeName(host::CurrentDisplayMode()));
    swapFormat = chosen.format;
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    swapImages.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data());
    swapLayouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
    // Views for drawing the overlay (settings menu) on top of the scan buffer.
    swapViews.resize(n);
    for (uint32_t i = 0; i < n; i++)
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &swapViews[i]));
    }
    swapMinImages = sci.minImageCount;
    renderDone.resize(n);
    for (auto& s : renderDone)
    {
        VkSemaphoreCreateInfo sci2{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(device, &sci2, nullptr, &s));
    }
    swapchainStale = false;
    return true;
}

void Device::DestroySwapchain()
{
    if (!swapchain)
        return;
    vkDeviceWaitIdle(device);
    for (auto s : renderDone)
        vkDestroySemaphore(device, s, nullptr);
    renderDone.clear();
    for (VkImageView v : swapViews)
        vkDestroyImageView(device, v, nullptr);
    swapViews.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void Device::BeginFrame(bool restart)
{
    Frame& f = frames[frame];
    VK_CHECK(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    if (f.midUsed)
        VK_CHECK(vkWaitForFences(device, uint32_t(f.midUsed), f.midFences.data(), VK_TRUE, UINT64_MAX));
    f.midUsed = 0;
    // A restart (SubmitAndWait, from the middle of a draw that holds the command buffer) begins the same buffer again;
    // a new frame starts with the first.
    if (!restart)
        f.spareUsed = 0, f.cmd = f.primary;
    if (recSlots[frame].frame != ~0ull)
        CollectRecording(recSlots[frame]);
    VK_CHECK(vkResetCommandPool(device, f.pool, 0));
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    f.recording = true;
    f.uploadUsed = 0;
    commandEpoch++;
}

VkCommandBuffer Device::Cmd() { return frames[frame].cmd; }

Device::Upload Device::Allocate(VkDeviceSize size, VkDeviceSize align)
{
    Frame& f = frames[frame];
    VkDeviceSize offset = (f.uploadUsed + align - 1) & ~(align - 1);
    if (offset + size > UploadSize)
    {
        // Out of upload space for this frame: flush the work so far and start over.
        SubmitAndWait();
        offset = 0;
        if (size > UploadSize)
            trace::Stop(9, "upload of %llu bytes exceeds the %llu-byte frame upload buffer", (unsigned long long)size,
                (unsigned long long)UploadSize);
    }
    Frame& cur = frames[frame];
    cur.uploadUsed = offset + size;
    return { cur.upload, offset, cur.uploadPtr + offset };
}

void Device::SubmitNoWait()
{
    if (beforeSubmit)
        beforeSubmit();
    Frame& f = frames[frame];
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    if (f.midUsed == f.midFences.size())
    {
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
        f.midFences.push_back(fence);
    }
    VkFence fence = f.midFences[f.midUsed++];
    VK_CHECK(vkResetFences(device, 1, &fence));
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    if (f.spareUsed == f.spare.size())
    {
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
        f.spare.push_back(cmd);
    }
    f.cmd = f.spare[f.spareUsed++];
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    commandEpoch++;
}

void Device::SubmitAndWait()
{
    if (beforeSubmit)
        beforeSubmit();
    Frame& f = frames[frame];
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    VK_CHECK(vkResetFences(device, 1, &f.fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &si, f.fence));
    VK_CHECK(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    BeginFrame(true);
}

void Device::ApplyGpuBoost()
{
    if (!hasLowLatency || !swapchain)
        return;
    const bool boost = settings::Get("graphics.gpu_boost", "on") != "off";
    if (boostApplied && boost == boostOn)
        return;
    static const auto setMode = PFN_vkSetLatencySleepModeNV(vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV"));
    if (!setMode)
        return;
    // Low latency mode only for the boost: the game is never asked to sleep (vkLatencySleepNV), so pacing is unchanged.
    VkLatencySleepModeInfoNV mode{ VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV };
    mode.lowLatencyMode = boost ? VK_TRUE : VK_FALSE;
    mode.lowLatencyBoost = boost ? VK_TRUE : VK_FALSE;
    mode.minimumIntervalUs = 0;
    const VkResult r = setMode(device, swapchain, &mode);
    if (!boostApplied || boost != boostOn)
        dtlog::Important("[dt-port] GPU clock boost (VK_NV_low_latency2): %s%s\n", boost ? "on" : "off",
            r == VK_SUCCESS ? "" : " (failed)");
    boostApplied = true, boostOn = boost;
}

void Device::Present(Image* scan)
{
    if (settings::Generation() != boostGeneration)
        boostGeneration = settings::Generation(), ApplyGpuBoost();
    Frame& f = frames[frame];
    // Rebuild the swapchain after a resize, a display mode change, regained focus or a new display.vsync.
    if (host::DisplayGeneration() != displayGeneration ||
        (settings::Generation() != presentGeneration && settings::Get("display.vsync", "fifo") != presentModeSetting))
        swapchainStale = true;
    if (swapchainStale)
        CreateSwapchain();
    uint32_t index = 0;
    using Clock = std::chrono::steady_clock;
    auto lap = [t = Clock::now()](double& total) mutable {
        const auto now = Clock::now();
        total += std::chrono::duration<double>(now - t).count();
        t = now;
    };
    VkResult acquire = swapchain ? vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index)
                                 : VK_ERROR_OUT_OF_DATE_KHR;
    const bool haveImage = acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR;
    lap(times.acquire);
    if (!haveImage)
    {
        if (acquire != VK_ERROR_OUT_OF_DATE_KHR && acquire != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
            acquire != VK_ERROR_SURFACE_LOST_KHR)
            VulkanFatal("vkAcquireNextImageKHR", acquire, __FILE__, __LINE__);
        swapchainStale = true;
    }

    VkCommandBuffer cmd = f.cmd;
    if (haveImage)
    {
        auto barrier = [&](VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = swapImages[index];
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                0, nullptr, 1, &b);
        };
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // The scan buffer keeps its aspect ratio: scaled to fit, centred, with black bars.
        const VkRect2D dst = scan && scan->image ? FitRect(scan->width, scan->height) : VkRect2D{};
        if (dst.extent.width != swapExtent.width || dst.extent.height != swapExtent.height)
        {
            VkClearColorValue black{};
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            if (dst.extent.width)
                barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        if (dst.extent.width)
        {
            Transition(cmd, *scan, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = { int32_t(scan->width), int32_t(scan->height), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0] = { dst.offset.x, dst.offset.y, 0 };
            blit.dstOffsets[1] = { dst.offset.x + int32_t(dst.extent.width), dst.offset.y + int32_t(dst.extent.height), 1 };
            vkCmdBlitImage(cmd, scan->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages[index],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (overlay::WantsToDraw())
        {
            barrier(layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            overlay::Record(*this, cmd, swapViews[index], swapExtent);
            layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        if (!windowShot.empty() && windowShots)
        {
            barrier(layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            shotBuffer = CreateHostBuffer(VkDeviceSize(swapExtent.width) * swapExtent.height * 4, shotMemory);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { swapExtent.width, swapExtent.height, 1 };
            vkCmdCopyImageToBuffer(cmd, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuffer, 1, &copy);
        }
        if (recCount && windowShots && frameCounter >= recFirst && frameCounter < recFirst + recCount)
        {
            RecSlot& r = recSlots[frame];
            const VkDeviceSize size = VkDeviceSize(swapExtent.width) * swapExtent.height * 4;
            if (r.size < size)
            {
                if (r.buffer)
                    vkDestroyBuffer(device, r.buffer, nullptr), vkFreeMemory(device, r.memory, nullptr);
                r.buffer = CreateHostBuffer(size, r.memory);
                r.size = size;
            }
            if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                barrier(layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { swapExtent.width, swapExtent.height, 1 };
            vkCmdCopyImageToBuffer(cmd, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &copy);
            r.frame = frameCounter, r.width = swapExtent.width, r.height = swapExtent.height;
        }
        barrier(layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (haveImage)
    {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone[index];
    }
    VK_CHECK(vkResetFences(device, 1, &f.fence));
    double recording = 0;
    lap(recording);  // the blit
    VK_CHECK(vkQueueSubmit(queue, 1, &si, f.fence));
    lap(times.submit);
    if (haveImage)
    {
        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        const VkResult r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
            r == VK_ERROR_SURFACE_LOST_KHR)
            swapchainStale = true;
        else if (r != VK_SUCCESS)
            VulkanFatal("vkQueuePresentKHR", r, __FILE__, __LINE__);
    }
    lap(times.present);
    if (shotBuffer)
        SaveWindowShot();
    frame = (frame + 1) % FramesInFlight;
    frameCounter++;
    BeginFrame();
    lap(times.fence);
}

VkBuffer Device::CreateHostBuffer(VkDeviceSize size, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    return buffer;
}

void Device::CollectRecording(RecSlot& r)
{
    const size_t bytes = size_t(r.width) * r.height * 4;
    void* data;
    VK_CHECK(vkMapMemory(device, r.memory, 0, bytes, 0, &data));
    recFrames.push_back({ r.frame, r.width, r.height, std::vector<uint8_t>(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes) });
    vkUnmapMemory(device, r.memory);
    r.frame = ~0ull;
    if (recFrames.size() < recCount)
        return;
    dtlog::Important("[gpu] recorded %zu frames from %llu; writing them to %s\n", recFrames.size(),
        (unsigned long long)recFrames.front().frame, recDir.c_str());
    recCount = 0;
    const bool bgra = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    // Written by 8 threads (one PNG of 1280x720 takes about 50 ms; a run's end does not wait for them).
    auto frames = std::make_shared<std::vector<RecFrame>>(std::move(recFrames));
    auto left = std::make_shared<std::atomic<int>>(8);
    std::filesystem::create_directories(recDir);
    for (int t = 0; t < 8; t++)
        std::thread([frames, left, t, dir = recDir, bgra] {
            for (size_t n = t; n < frames->size(); n += 8)
            {
                RecFrame& f = (*frames)[n];
                for (size_t i = 0; i < f.pixels.size(); i += 4)
                {
                    if (bgra)
                        std::swap(f.pixels[i], f.pixels[i + 2]);
                    f.pixels[i + 3] = 255;
                }
                char name[64];
                snprintf(name, sizeof(name), "/rec_%06llu.png", (unsigned long long)f.frame);
                WritePng(dir + name, f.pixels.data(), f.width, f.height, f.width * 4);
                std::vector<uint8_t>().swap(f.pixels);
            }
            if (--*left == 0)
                dtlog::Important("[gpu] recording written (%zu frames)\n", frames->size());
        }).detach();
    recFrames.clear();
}

void Device::SaveWindowShot()
{
    VK_CHECK(vkWaitForFences(device, 1, &frames[frame].fence, VK_TRUE, UINT64_MAX));
    const uint32_t w = swapExtent.width, h = swapExtent.height;
    void* data;
    VK_CHECK(vkMapMemory(device, shotMemory, 0, VkDeviceSize(w) * h * 4, 0, &data));
    std::vector<uint8_t> pixels(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size_t(w) * h * 4);
    vkUnmapMemory(device, shotMemory);
    vkDestroyBuffer(device, shotBuffer, nullptr);
    vkFreeMemory(device, shotMemory, nullptr);
    shotBuffer = VK_NULL_HANDLE, shotMemory = VK_NULL_HANDLE;
    const bool bgra = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        if (bgra)
            std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 255;
    }
    if (WritePng(windowShot, pixels.data(), w, h, w * 4))
        dtlog::Important("[gpu] screenshot %s (window)\n", windowShot.c_str());
    windowShot.clear();
}

VkRect2D Device::FitRect(uint32_t width, uint32_t height) const
{
    const uint32_t W = swapExtent.width, H = swapExtent.height;
    if (!width || !height || !W || !H)
        return { { 0, 0 }, { W, H } };
    // Width-limited or height-limited, whichever fits; rounded to whole pixels.
    uint32_t w = W, h = uint32_t((uint64_t(W) * height + width / 2) / width);
    if (h > H)
        h = H, w = uint32_t((uint64_t(H) * width + height / 2) / height);
    w = std::min(std::max(w, 1u), W), h = std::min(std::max(h, 1u), H);
    return { { int32_t((W - w) / 2), int32_t((H - h) / 2) }, { w, h } };
}

bool Device::MemoryBudget(uint64_t& usage, uint64_t& budget) const
{
    usage = budget = 0;
    if (!hasMemoryBudget)
        return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 mp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
    mp.pNext = &mb;
    vkGetPhysicalDeviceMemoryProperties2(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += mb.heapUsage[i], budget += mb.heapBudget[i];
    return true;
}

uint32_t Device::MemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    trace::Stop(9, "no Vulkan memory type with flags 0x%X", flags);
}

bool IsDepthFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return true;
    default: return false;
    }
}

Image Device::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkComponentMapping swizzle)
{
    ImageDesc d;
    d.width = width, d.height = height, d.format = format, d.usage = usage, d.flags = flags, d.swizzle = swizzle;
    return CreateImage(d);
}

VkFormatFeatureFlags Device::FormatFeatures(VkFormat format) const
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical, format, &p);
    return p.optimalTilingFeatures;
}

Image Device::CreateImage(const ImageDesc& d)
{
    Image img;
    img.width = d.width;
    img.height = d.height;
    img.format = d.format;
    img.mipLevels = d.mipLevels;
    img.layers = d.layers;
    if (IsDepthFormat(d.format))
    {
        img.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (d.format == VK_FORMAT_D24_UNORM_S8_UINT || d.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            img.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.flags = d.flags;
    ici.imageType = d.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = d.format;
    ici.extent = { d.width, d.height, d.depth };
    ici.mipLevels = d.mipLevels;
    ici.arrayLayers = d.layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = d.usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device, &ici, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    if (!AllocateImageMemory(req, img))
    {
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &img.memory));
    }
    VK_CHECK(vkBindImageMemory(device, img.image, img.memory, img.memoryOffset));
    // Transfer-only images (the TV scan image) have no view, and callers that make their own views ask for none.
    if (!d.createView || !(d.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return img;
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = d.format;
    vci.components = d.swizzle;
    vci.subresourceRange = { img.aspect, std::min(d.viewFirstMip, d.mipLevels - 1), d.viewMips, 0, 1 };
    // A sampled view of a depth/stencil image reads depth only.
    if ((d.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VK_CHECK(vkCreateImageView(device, &vci, nullptr, &img.view));
    return img;
}

void Device::DestroyImage(Image& img)
{
    if (img.view)
        vkDestroyImageView(device, img.view, nullptr);
    if (img.image)
        vkDestroyImage(device, img.image, nullptr);
    if (img.memoryBlock != ~0u)
        FreeImageMemory(img);
    else if (img.memory)
        vkFreeMemory(device, img.memory, nullptr);
    img = {};
}

bool Device::AllocateImageMemory(const VkMemoryRequirements& req, Image& img)
{
    constexpr VkDeviceSize BlockSize = 256ull << 20, MaxSuballocation = 64ull << 20;
    if (req.size > MaxSuballocation)
        return false;
    const uint32_t type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    std::lock_guard lock(imageBlocksMutex);
    auto tryBlock = [&](uint32_t b) {
        MemoryBlock& block = imageBlocks[b];
        for (auto it = block.free.begin(); it != block.free.end(); ++it)
        {
            const VkDeviceSize start = (it->first + req.alignment - 1) / req.alignment * req.alignment;
            const VkDeviceSize end = it->first + it->second;
            if (start + req.size > end)
                continue;
            // Split the free range around [start, start + size).
            const VkDeviceSize before = start - it->first, after = end - (start + req.size), rangeStart = it->first;
            block.free.erase(it);
            if (before)
                block.free[rangeStart] = before;
            if (after)
                block.free[start + req.size] = after;
            img.memory = block.memory, img.memoryOffset = start, img.memorySize = req.size, img.memoryBlock = b;
            return true;
        }
        return false;
    };
    for (uint32_t b = 0; b < imageBlocks.size(); b++)
        if (imageBlocks[b].memory && imageBlocks[b].type == type && tryBlock(b))
            return true;
    MemoryBlock block;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = BlockSize;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &mai, nullptr, &block.memory) != VK_SUCCESS)
        return false;  // out of device memory for a whole block: fall back to a dedicated allocation
    block.type = type;
    block.free[0] = BlockSize;
    imageBlocks.push_back(std::move(block));
    return tryBlock(uint32_t(imageBlocks.size() - 1));
}

void Device::FreeImageMemory(Image& img)
{
    std::lock_guard lock(imageBlocksMutex);
    auto& free = imageBlocks[img.memoryBlock].free;
    VkDeviceSize start = img.memoryOffset, size = img.memorySize;
    // Coalesce with the free ranges on either side.
    auto next = free.lower_bound(start);
    if (next != free.end() && next->first == start + size)
        size += next->second, next = free.erase(next);
    if (next != free.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start)
            start = prev->first, size += prev->second, free.erase(prev);
    }
    free[start] = size;
}

void Device::Transition(VkCommandBuffer cmd, Image& img, VkImageLayout layout)
{
    // Read-only to read-only needs nothing; any other same-layout pair (two clears, two passes on one
    // attachment) still needs the write ordered before what follows.
    if (img.layout == layout && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = { img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &b);
    img.layout = layout;
}
} // namespace gpu
