// Vulkan through volk: no Vulkan SDK or import library is needed; the loader is found at run time.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include <cstdio>

#define VK_CHECK(expr)                                                                       \
    do                                                                                       \
    {                                                                                        \
        const VkResult vkCheckResult_ = (expr);                                              \
        if (vkCheckResult_ != VK_SUCCESS)                                                    \
            ::gpu::VulkanFatal(#expr, vkCheckResult_, __FILE__, __LINE__);                   \
    } while (0)

namespace gpu
{
const char* VkResultName(VkResult r);
[[noreturn]] void VulkanFatal(const char* expr, VkResult r, const char* file, int line);
}
