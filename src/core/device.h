#pragma once

#include <volk.h>
#include <cstdint>
#include <functional>
#include <string>

namespace seifg {

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    uint32_t computeQueueFamily = 0;
    VkResult lastResult = VK_SUCCESS;
    bool hasImageProcessing = false;
    bool hasFilterCubic = false;
    VkFormat lumaFormat = VK_FORMAT_R16_SFLOAT;

    bool init(uint64_t deviceUUID);
#if defined(__linux__) && !defined(__ANDROID__)
    bool initWithPicker(const std::function<bool(const std::string& name, uint32_t vendorID, uint32_t deviceID)>& picker);
#endif
    void destroy();
};

}
