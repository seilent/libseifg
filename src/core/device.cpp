#include "device.h"
#include <cstring>

namespace seifg {

bool Device::init(uint64_t deviceUUID) {
    lastResult = volkInitialize();
    if (lastResult != VK_SUCCESS) return false;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceCI{};
    instanceCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCI.pApplicationInfo = &appInfo;

    lastResult = vkCreateInstance(&instanceCI, nullptr, &instance);
    if (lastResult != VK_SUCCESS) return false;

    volkLoadInstance(instance);

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    VkPhysicalDevice gpus[16];
    gpuCount = gpuCount > 16 ? 16 : gpuCount;
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus);

    for (uint32_t i = 0; i < gpuCount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        uint64_t id = (static_cast<uint64_t>(props.vendorID) << 32) | props.deviceID;
        if (id == deviceUUID) {
            physicalDevice = gpus[i];
            break;
        }
    }
    if (physicalDevice == VK_NULL_HANDLE && gpuCount > 0)
        physicalDevice = gpus[0];
    if (physicalDevice == VK_NULL_HANDLE) return false;

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, nullptr);
    VkQueueFamilyProperties qfProps[16];
    qfCount = qfCount > 16 ? 16 : qfCount;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, qfProps);

    bool found = false;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeQueueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = computeQueueFamily;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &priority;

    const char* requiredExts[] = {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    uint32_t extCount = 8;

    const char* allExts[16];
    memcpy(allExts, requiredExts, sizeof(requiredExts));

    uint32_t availExtCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availExtCount, nullptr);
    VkExtensionProperties* availExts = new VkExtensionProperties[availExtCount];
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availExtCount, availExts);

    for (uint32_t i = 0; i < availExtCount; i++) {
        if (strcmp(availExts[i].extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) == 0)
            allExts[extCount++] = VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME;
        else if (strcmp(availExts[i].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
            allExts[extCount++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
        else if (strcmp(availExts[i].extensionName, "VK_QCOM_image_processing") == 0)
            hasImageProcessing = true;
        else if (strcmp(availExts[i].extensionName, "VK_EXT_filter_cubic") == 0 && !hasFilterCubic) {
            hasFilterCubic = true;
            allExts[extCount++] = "VK_EXT_filter_cubic";
        } else if (strcmp(availExts[i].extensionName, "VK_IMG_filter_cubic") == 0 && !hasFilterCubic) {
            hasFilterCubic = true;
            allExts[extCount++] = "VK_IMG_filter_cubic";
        }
    }
    delete[] availExts;

    if (hasImageProcessing) {
        auto supportsBlockMatch = [&](VkFormat fmt) {
            VkFormatProperties3 p3{};
            p3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
            VkFormatProperties2 p2{};
            p2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            p2.pNext = &p3;
            vkGetPhysicalDeviceFormatProperties2(physicalDevice, fmt, &p2);
            return (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_BLOCK_MATCHING_BIT_QCOM) != 0;
        };
        if (supportsBlockMatch(VK_FORMAT_R16_SFLOAT)) {
            lumaFormat = VK_FORMAT_R16_SFLOAT;
        } else if (supportsBlockMatch(VK_FORMAT_R8_UNORM)) {
            lumaFormat = VK_FORMAT_R8_UNORM;
        } else {
            hasImageProcessing = false;
        }
    }
    if (hasImageProcessing) {
        allExts[extCount++] = "VK_QCOM_image_processing";
    }

    VkPhysicalDeviceShaderFloat16Int8Features fp16Features{};
    fp16Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    fp16Features.shaderFloat16 = VK_TRUE;

    VkPhysicalDeviceImageProcessingFeaturesQCOM ipFeatures{};
    ipFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM;
    ipFeatures.textureBlockMatch = VK_TRUE;
    if (hasImageProcessing)
        fp16Features.pNext = &ipFeatures;

    VkDeviceCreateInfo deviceCI{};
    deviceCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCI.pNext = &fp16Features;
    deviceCI.queueCreateInfoCount = 1;
    deviceCI.pQueueCreateInfos = &queueCI;
    deviceCI.enabledExtensionCount = extCount;
    deviceCI.ppEnabledExtensionNames = allExts;

    lastResult = vkCreateDevice(physicalDevice, &deviceCI, nullptr, &device);
    if (lastResult != VK_SUCCESS) return false;

    volkLoadDevice(device);
    vkGetDeviceQueue(device, computeQueueFamily, 0, &computeQueue);
    return true;
}

void Device::destroy() {
    if (device) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

}
