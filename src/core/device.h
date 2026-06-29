#pragma once

#include <volk.h>
#include <cstdint>

namespace seifg {

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    uint32_t computeQueueFamily = 0;
    VkResult lastResult = VK_SUCCESS;

    bool init(uint64_t deviceUUID);
    void destroy();
};

}
