#pragma once

#include <volk.h>
#include <android/hardware_buffer.h>

namespace seifg {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkResult lastResult = VK_SUCCESS;

    bool createFromAHB(VkDevice device, VkPhysicalDevice physDev, AHardwareBuffer* ahb,
                       VkExtent2D ext, VkFormat fmt);
    bool createInternal(VkDevice device, VkPhysicalDevice physDev,
                        VkFormat fmt, uint32_t width, uint32_t height, VkImageUsageFlags usage);
    void destroy(VkDevice device);
};

}
