#include "image.h"

namespace seifg {

static bool createView(VkDevice device, VkImage image, VkFormat format, VkImageView* view) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.layerCount = 1;
    return vkCreateImageView(device, &ci, nullptr, view) == VK_SUCCESS;
}

bool Image::createFromAHB(VkDevice device, VkPhysicalDevice physDev, AHardwareBuffer* ahb,
                           VkExtent2D ext, VkFormat fmt) {
    extent = ext;
    format = fmt;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{};
    fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID ahbProps{};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahbProps.pNext = &fmtProps;

    lastResult = vkGetAndroidHardwareBufferPropertiesANDROID(device, ahb, &ahbProps);
    if (lastResult != VK_SUCCESS) return false;

    VkExternalMemoryImageCreateInfo extMemCI{};
    extMemCI.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.pNext = &extMemCI;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.format = fmt;
    imageCI.extent = {ext.width, ext.height, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    lastResult = vkCreateImage(device, &imageCI, nullptr, &image);
    if (lastResult != VK_SUCCESS) return false;

    VkImportAndroidHardwareBufferInfoANDROID importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importInfo.buffer = ahb;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = image;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = ahbProps.allocationSize;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (ahbProps.memoryTypeBits & (1u << i)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    lastResult = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
    if (lastResult != VK_SUCCESS) return false;

    lastResult = vkBindImageMemory(device, image, memory, 0);
    if (lastResult != VK_SUCCESS) return false;

    return createView(device, image, format, &view);
}

bool Image::createInternal(VkDevice device, VkPhysicalDevice physDev,
                            VkFormat fmt, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
    extent = {width, height};
    format = fmt;

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.format = fmt;
    imageCI.extent = {width, height, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage = usage;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    lastResult = vkCreateImage(device, &imageCI, nullptr, &image);
    if (lastResult != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    uint32_t memIndex = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memIndex = i;
            break;
        }
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memIndex;

    lastResult = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
    if (lastResult != VK_SUCCESS) return false;

    lastResult = vkBindImageMemory(device, image, memory, 0);
    if (lastResult != VK_SUCCESS) return false;

    return createView(device, image, format, &view);
}

void Image::destroy(VkDevice device) {
    if (view) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    if (image) { vkDestroyImage(device, image, nullptr); image = VK_NULL_HANDLE; }
    if (memory) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
}

}
