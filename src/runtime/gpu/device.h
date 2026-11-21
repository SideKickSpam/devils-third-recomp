// Vulkan device, swapchain and per-frame command recording.
#pragma once

#include "exclusive.h"
#include "vulkan.h"

#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <vector>

struct SDL_Window;

namespace gpu
{
// A Vulkan image plus the layout it is currently in (we track layouts ourselves and transition on demand).
struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset = 0, memorySize = 0;
    uint32_t memoryBlock = ~0u;  // the Device's image memory block it is suballocated from, or ~0u (own allocation)
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mipLevels = 1, layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Everything CreateImage needs. The view covers mips [viewFirstMip, viewFirstMip + viewMips).
struct ImageDesc
{
    uint32_t width = 1, height = 1, depth = 1, mipLevels = 1, layers = 1;  // depth > 1 makes a 3D image
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool createView = true;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    VkComponentMapping swizzle = {};
    uint32_t viewFirstMip = 0, viewMips = VK_REMAINING_MIP_LEVELS;
};

bool IsDepthFormat(VkFormat format);

class Device
{
public:
    // Create everything for the window. Returns false (with a log message) if Vulkan is unavailable.
    bool Init(SDL_Window* window);
    bool Ready() const { return device != VK_NULL_HANDLE; }

    VkDevice Handle() const { return device; }
    bool PreciseOcclusion() const { return enabledFeatures.occlusionQueryPrecise; }
    VkInstance Instance() const { return instance; }
    uint32_t QueueFamily() const { return queueFamily; }
    VkQueue Queue() const { return queue; }
    VkFormat SwapFormat() const { return swapFormat; }
    // --screenshot-window: screenshots of the presented image (the overlay included), PNG at `path` after the next present.
    void EnableWindowShots() { windowShots = true, swapchainStale = true; }
    void RequestWindowShot(const std::string& path) { windowShot = path; }
    // --record: copies the presented window images (overlay included) of frames first .. first+count-1 without
    // stalling (each lands in its frame slot's buffer and is collected when that slot's fence has passed), then
    // writes them as PNGs into `dir` on a thread of their own. Screenshots stall the game about 400 ms each, so
    // anything that moves looked like flicker in them.
    void StartRecording(uint64_t first, uint32_t count, const std::string& dir)
    {
        recFirst = first, recCount = count, recDir = dir;
        EnableWindowShots();
    }
    uint32_t SwapImageCount() const { return uint32_t(swapImages.size()); }
    uint32_t SwapMinImageCount() const { return swapMinImages; }
    VkPhysicalDevice Physical() const { return physical; }
    const VkPhysicalDeviceProperties& Properties() const { return properties; }
    const VkPhysicalDeviceFeatures& Features() const { return enabledFeatures; }  // optional features enabled
    VkFormatFeatureFlags FormatFeatures(VkFormat format) const;
    // Instance-rate vertex divisors above 1 (VK_KHR_vertex_attribute_divisor).
    bool AttributeDivisor() const { return hasAttributeDivisor; }

    // The command buffer recording the current frame. Begun automatically.
    VkCommandBuffer Cmd();

    // Finish the frame: blit `scan` into the next swapchain image (scaled to the window), submit and present.
    // `scan` may be null (presents black). Afterwards the next frame's command buffer is ready.
    void Present(Image* scan);

    // Submit the current frame's work and wait for the GPU to finish it (for CPU readbacks).
    void SubmitAndWait();
    // Submit the work recorded so far without waiting, and go on recording the frame in a new command buffer (so the
    // GPU starts on it early: occlusion queries the game reads at the start of the next frame).
    void SubmitNoWait();
    // Called before the current command buffer is ended (by Present or SubmitAndWait), so open render passes
    // can be closed.
    void SetBeforeSubmit(void (*fn)()) { beforeSubmit = fn; }

    // Resources.
    uint32_t MemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    Image CreateImage(const ImageDesc& desc);
    Image CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
        VkImageCreateFlags flags = 0, VkComponentMapping swizzle = {});
    void DestroyImage(Image& img);
    // Record a layout transition with a full barrier (simple and always correct).
    void Transition(VkCommandBuffer cmd, Image& img, VkImageLayout layout);

    // Host-visible upload memory for the current frame (reset when the frame's fence signals).
    struct Upload
    {
        VkBuffer buffer;
        VkDeviceSize offset;
        uint8_t* ptr;
    };
    Upload Allocate(VkDeviceSize size, VkDeviceSize align = 256);

    uint64_t FrameIndex() const { return frameCounter; }
    // Changes whenever a command buffer is begun (each frame, and after SubmitAndWait): state recorded under
    // another value is gone.
    uint64_t CommandEpoch() const { return commandEpoch; }
    // Time spent in Present's steps, in seconds since start (for --gpu-stats): acquiring the swapchain image,
    // submitting, presenting, and waiting for the next frame slot's previous work.
    struct PresentTimes
    {
        double acquire = 0, submit = 0, present = 0, fence = 0;
    };
    const PresentTimes& Times() const { return times; }
    // Device-local memory in use by this process and the budget the OS gives it (VK_EXT_memory_budget).
    bool MemoryBudget(uint64_t& usage, uint64_t& budget) const;
    // Where an image of this size goes in the swapchain image: scaled to fit with its aspect ratio, centred.
    VkRect2D FitRect(uint32_t width, uint32_t height) const;
    VkExtent2D SwapExtent() const { return swapExtent; }
    // The frame-in-flight slot being recorded (0 .. FramesInFlight-1); its previous work has finished.
    uint32_t Slot() const { return frame; }
    static constexpr uint32_t SlotCount() { return FramesInFlight; }

private:
    static constexpr uint32_t FramesInFlight = 2;
    static constexpr VkDeviceSize UploadSize = 64ull << 20;

    struct Frame
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;      // the one being recorded
        VkCommandBuffer primary = VK_NULL_HANDLE;  // the frame's first
        std::vector<VkCommandBuffer> spare;        // more, after SubmitNoWait (from `pool`)
        size_t spareUsed = 0;
        std::vector<VkFence> midFences;            // one per SubmitNoWait this frame
        size_t midUsed = 0;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        bool recording = false;
        VkBuffer upload = VK_NULL_HANDLE;
        VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
        uint8_t* uploadPtr = nullptr;
        VkDeviceSize uploadUsed = 0;
    };

    bool CreateSwapchain();
    void DestroySwapchain();
    void BeginFrame(bool restart = false);

    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures enabledFeatures{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageLayout> swapLayouts;
    std::vector<VkSemaphore> renderDone;  // per swapchain image
    std::vector<VkImageView> swapViews;   // per swapchain image, for the overlay
    uint32_t swapMinImages = 0;
    bool swapchainStale = false;
    uint64_t displayGeneration = 0, presentGeneration = 0;  // host::DisplayGeneration / settings at creation
    std::string presentModeSetting;
    bool windowShots = false;  // swapchain images can be copied from (EnableWindowShots)
    VkBuffer shotBuffer = VK_NULL_HANDLE;
    VkDeviceMemory shotMemory = VK_NULL_HANDLE;
    VkBuffer CreateHostBuffer(VkDeviceSize size, VkDeviceMemory& memory);
    void SaveWindowShot();
    struct RecSlot
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint64_t frame = ~0ull;  // the frame copied into it, not yet collected
        uint32_t width = 0, height = 0;
    };
    struct RecFrame
    {
        uint64_t frame;
        uint32_t width, height;
        std::vector<uint8_t> pixels;
    };
    uint64_t recFirst = 0;
    uint32_t recCount = 0;
    std::string recDir;
    RecSlot recSlots[2];
    std::vector<RecFrame> recFrames;
    void CollectRecording(RecSlot& slot);
    std::string windowShot;    // where the next presented image goes
    bool hasExclusive = false;  // VK_EXT_full_screen_exclusive
    bool hasAttributeDivisor = false;
    exclusive::Swapchain exclusiveSwapchain;

    Frame frames[FramesInFlight];
    uint32_t frame = 0;
    PresentTimes times;
    uint64_t commandEpoch = 0;

    // Image memory: suballocated from large blocks per memory type (one vkAllocateMemory per image cost about
    // 0.1 ms on Windows, 60 ms for a burst of streamed textures); very large images get their own allocation.
    struct MemoryBlock
    {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t type = 0;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size, coalesced
    };
    std::vector<MemoryBlock> imageBlocks;
    std::mutex imageBlocksMutex;
    bool AllocateImageMemory(const VkMemoryRequirements& req, Image& img);
    void FreeImageMemory(Image& img);
    bool hasMemoryBudget = false;
    // VK_NV_low_latency2's Low Latency Boost (graphics.gpu_boost): keeps an NVIDIA GPU's clocks up while it has little to
    // do (movies, loading, menus), so the first heavy frames after them do not run at idle clocks.
    bool hasLowLatency = false;
    bool boostApplied = false, boostOn = false;
    uint64_t boostGeneration = ~0ull;
    void ApplyGpuBoost();
    uint64_t frameCounter = 0;
    void (*beforeSubmit)() = nullptr;
};

Device& GetDevice();
// Enable the Khronos validation layer (--validate). Call before Device::Init.
void SetValidation(bool enable);
} // namespace gpu
