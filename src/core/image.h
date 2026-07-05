#pragma once

#include <volk.h>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif

namespace seifg {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkResult lastResult = VK_SUCCESS;
    bool ownsImage = true;

#ifdef __ANDROID__
    bool createFromAHB(VkDevice device, VkPhysicalDevice physDev, AHardwareBuffer* ahb,
                       VkExtent2D ext, VkFormat fmt);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    bool createFromFd(VkDevice device, VkPhysicalDevice physDev, int fd,
                      VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage);
#endif
    bool createInternal(VkDevice device, VkPhysicalDevice physDev,
                        VkFormat fmt, uint32_t width, uint32_t height, VkImageUsageFlags usage);
    bool wrapExternal(VkDevice device, VkImage img, VkExtent2D ext, VkFormat fmt);
    void destroy(VkDevice device);
};

}
