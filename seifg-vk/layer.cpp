#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <seifg.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
};

struct ExportedImage {
    VkImage image;
    VkDeviceMemory memory;
    int fd;
};

struct RenderPass {
    VkCommandBuffer commandBuffer;
    VkSemaphore acquireSemaphore;
};

struct SwapchainData {
    VkDevice device;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;
    VkFormat format;
    uint32_t multiplier;

    std::vector<ExportedImage> sourceImages;
    std::vector<ExportedImage> destImages;

    VkSemaphore timelineSemaphore;
    int timelineFd;

    int32_t contextId;
    uint64_t idx;
    uint64_t fidx;

    VkCommandPool cmdPool;
    VkCommandBuffer renderCmd;
    VkFence renderFence;
    bool fenceSubmitted;

    std::vector<RenderPass> passes;

    struct PresentSemaphores {
        VkSemaphore present;
        VkSemaphore chain;
    };
    std::vector<PresentSemaphores> presentSemaphores;
};

struct DeviceData {
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    DeviceDispatch dispatch;
    uint32_t queueFamilyIndex;

    std::string deviceName;
    uint32_t vendorID;
    uint32_t deviceID;
    bool layerActive;
};

static std::mutex g_lock;
static PFN_vkGetInstanceProcAddr g_nextGIPA;
static std::unordered_map<void*, InstanceDispatch> g_instanceDispatch;
static std::unordered_map<void*, DeviceData> g_deviceData;
static std::unordered_map<VkSwapchainKHR, SwapchainData> g_swapchains;
static bool g_engineInitialized = false;

static uint32_t getMultiplier() {
    const char* env = std::getenv("SEIFG_MULTIPLIER");
    if (!env) return 2;
    int val = std::atoi(env);
    if (val < 2) return 2;
    if (val > 4) return 4;
    return static_cast<uint32_t>(val);
}

static uint32_t getQuality() {
    const char* env = std::getenv("SEIFG_QUALITY");
    if (!env) return 3;
    int val = std::atoi(env);
    if (val < 1) return 1;
    if (val > 5) return 5;
    return static_cast<uint32_t>(val);
}

static void* getDispatchKey(void* object) {
    return *reinterpret_cast<void**>(object);
}

static uint32_t findMemoryType(DeviceData& dd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    dd.dispatch.GetPhysicalDeviceMemoryProperties(dd.physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (typeBits & (1u << i))
            return i;
    }
    return 0;
}

static ExportedImage createExportableImage(DeviceData& dd, VkExtent2D extent,
        VkFormat format, VkImageUsageFlags usage) {
    if (!dd.dispatch.GetMemoryFdKHR) {
        std::cerr << "seifg-vk: vkGetMemoryFdKHR is NULL, cannot create exportable image\n";
        return { VK_NULL_HANDLE, VK_NULL_HANDLE, -1 };
    }

    VkExternalMemoryImageCreateInfo extMemInfo{};
    extMemInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &extMemInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { extent.width, extent.height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image{};
    VkResult res = dd.dispatch.CreateImage(dd.device, &imageInfo, nullptr, &image);
    if (res != VK_SUCCESS) {
        std::cerr << "seifg-vk: vkCreateImage failed: " << res << "\n";
        return { VK_NULL_HANDLE, VK_NULL_HANDLE, -1 };
    }

    VkMemoryRequirements reqs{};
    dd.dispatch.GetImageMemoryRequirements(dd.device, image, &reqs);

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = image;

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.pNext = &dedicatedInfo;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(dd, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory{};
    res = dd.dispatch.AllocateMemory(dd.device, &allocInfo, nullptr, &memory);
    if (res != VK_SUCCESS) {
        std::cerr << "seifg-vk: vkAllocateMemory failed: " << res << "\n";
        dd.dispatch.DestroyImage(dd.device, image, nullptr);
        return { VK_NULL_HANDLE, VK_NULL_HANDLE, -1 };
    }

    dd.dispatch.BindImageMemory(dd.device, image, memory, 0);

    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    int fd = -1;
    res = dd.dispatch.GetMemoryFdKHR(dd.device, &fdInfo, &fd);
    if (res != VK_SUCCESS) {
        std::cerr << "seifg-vk: vkGetMemoryFdKHR failed: " << res << "\n";
        dd.dispatch.FreeMemory(dd.device, memory, nullptr);
        dd.dispatch.DestroyImage(dd.device, image, nullptr);
        return { VK_NULL_HANDLE, VK_NULL_HANDLE, -1 };
    }

    return { image, memory, fd };
}

static VkImageMemoryBarrier makeBarrier(VkImage image, VkAccessFlags srcAccess,
        VkAccessFlags dstAccess, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}

static VkResult VKAPI_CALL seifg_CreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance) {
    auto* layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(
        const_cast<void*>(pCreateInfo->pNext));
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO))
        layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(
            const_cast<void*>(layerInfo->pNext));

    if (!layerInfo || !layerInfo->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto nextGIPA = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        nextGIPA(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = createInstance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceDispatch id{};
    id.GetInstanceProcAddr = nextGIPA;
    id.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        nextGIPA(*pInstance, "vkDestroyInstance"));
    id.EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        nextGIPA(*pInstance, "vkEnumeratePhysicalDevices"));
    id.GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        nextGIPA(*pInstance, "vkGetPhysicalDeviceProperties"));
    id.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        nextGIPA(*pInstance, "vkGetPhysicalDeviceMemoryProperties"));
    id.CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        nextGIPA(*pInstance, "vkCreateDevice"));
    id.EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        nextGIPA(*pInstance, "vkEnumerateDeviceExtensionProperties"));

    std::lock_guard<std::mutex> lock(g_lock);
    g_nextGIPA = nextGIPA;
    g_instanceDispatch[getDispatchKey(*pInstance)] = id;
    return VK_SUCCESS;
}

static std::vector<const char*> buildExtensionList(const char* const* existing, uint32_t count,
        const std::vector<const char*>& required) {
    std::vector<const char*> result(existing, existing + count);
    for (const char* ext : required) {
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            if (std::strcmp(existing[i], ext) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            result.push_back(ext);
    }
    return result;
}

static VkResult VKAPI_CALL seifg_CreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice) {
    auto* layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(
        const_cast<void*>(pCreateInfo->pNext));
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO))
        layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(
            const_cast<void*>(layerInfo->pNext));

    if (!layerInfo || !layerInfo->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto nextGDPA = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto nextGIPA = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;

    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(
        nextGIPA(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!createDevice)
        return VK_ERROR_INITIALIZATION_FAILED;

    static constexpr const char* kRequiredExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };

    std::vector<const char*> extensions = buildExtensionList(
        pCreateInfo->ppEnabledExtensionNames,
        pCreateInfo->enabledExtensionCount,
        { std::begin(kRequiredExtensions), std::end(kRequiredExtensions) }
    );

    VkDeviceCreateInfo modifiedInfo = *pCreateInfo;
    modifiedInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedInfo.ppEnabledExtensionNames = extensions.data();

    bool timelineFeatureEnabled = false;
    auto* chain = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(modifiedInfo.pNext));
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(chain);
            f->timelineSemaphore = VK_TRUE;
            timelineFeatureEnabled = true;
        } else if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(chain);
            f->timelineSemaphore = VK_TRUE;
            timelineFeatureEnabled = true;
        }
        chain = chain->pNext;
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.timelineSemaphore = VK_TRUE;

    if (!timelineFeatureEnabled) {
        timelineFeatures.pNext = const_cast<void*>(modifiedInfo.pNext);
        modifiedInfo.pNext = &timelineFeatures;
    }

    VkResult res = createDevice(physicalDevice, &modifiedInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    DeviceDispatch dd{};
    dd.GetDeviceProcAddr = nextGDPA;

#define LOAD_DEVICE(fn) dd.fn = reinterpret_cast<PFN_vk##fn>(nextGDPA(*pDevice, "vk" #fn))
    LOAD_DEVICE(DestroyDevice);
    LOAD_DEVICE(CreateSwapchainKHR);
    LOAD_DEVICE(DestroySwapchainKHR);
    LOAD_DEVICE(GetSwapchainImagesKHR);
    LOAD_DEVICE(AcquireNextImageKHR);
    LOAD_DEVICE(QueuePresentKHR);
    LOAD_DEVICE(QueueSubmit);
    LOAD_DEVICE(CreateImage);
    LOAD_DEVICE(DestroyImage);
    LOAD_DEVICE(GetImageMemoryRequirements);
    LOAD_DEVICE(AllocateMemory);
    LOAD_DEVICE(FreeMemory);
    LOAD_DEVICE(BindImageMemory);
    LOAD_DEVICE(GetMemoryFdKHR);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(DestroySemaphore);
    LOAD_DEVICE(GetSemaphoreFdKHR);
    LOAD_DEVICE(CreateCommandPool);
    LOAD_DEVICE(DestroyCommandPool);
    LOAD_DEVICE(AllocateCommandBuffers);
    LOAD_DEVICE(FreeCommandBuffers);
    LOAD_DEVICE(BeginCommandBuffer);
    LOAD_DEVICE(EndCommandBuffer);
    LOAD_DEVICE(ResetCommandBuffer);
    LOAD_DEVICE(CmdPipelineBarrier);
    LOAD_DEVICE(CmdBlitImage);
    LOAD_DEVICE(CreateFence);
    LOAD_DEVICE(DestroyFence);
    LOAD_DEVICE(WaitForFences);
    LOAD_DEVICE(ResetFences);
    LOAD_DEVICE(DeviceWaitIdle);
#undef LOAD_DEVICE

    dd.GetPhysicalDeviceMemoryProperties = nullptr;

    bool layerActive = true;
    if (!dd.GetMemoryFdKHR || !dd.GetSemaphoreFdKHR) {
        std::cerr << "seifg-vk: critical function pointers missing (GetMemoryFdKHR="
            << (void*)dd.GetMemoryFdKHR << " GetSemaphoreFdKHR="
            << (void*)dd.GetSemaphoreFdKHR << "), layer disabled for this device\n";
        layerActive = false;
    }

    VkPhysicalDeviceProperties props{};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto instIt = g_instanceDispatch.find(getDispatchKey(physicalDevice));
        if (instIt != g_instanceDispatch.end()) {
            instIt->second.GetPhysicalDeviceProperties(physicalDevice, &props);
            dd.GetPhysicalDeviceMemoryProperties = instIt->second.GetPhysicalDeviceMemoryProperties;
        }
    }

    if (!dd.GetPhysicalDeviceMemoryProperties) {
        std::cerr << "seifg-vk: vkGetPhysicalDeviceMemoryProperties not available, layer disabled\n";
        layerActive = false;
    }

    DeviceData devData{};
    devData.device = *pDevice;
    devData.physicalDevice = physicalDevice;
    devData.dispatch = dd;
    devData.queueFamilyIndex = 0;
    devData.deviceName = props.deviceName;
    devData.vendorID = props.vendorID;
    devData.deviceID = props.deviceID;
    devData.layerActive = layerActive;

    std::lock_guard<std::mutex> lock(g_lock);
    g_deviceData[getDispatchKey(*pDevice)] = devData;
    return VK_SUCCESS;
}

static void VKAPI_CALL seifg_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    std::unique_lock<std::mutex> lock(g_lock);
    auto it = g_deviceData.find(getDispatchKey(device));
    if (it == g_deviceData.end()) return;

    auto dd = it->second.dispatch;
    g_deviceData.erase(it);
    lock.unlock();

    dd.DestroyDevice(device, pAllocator);
}

static void VKAPI_CALL seifg_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    std::unique_lock<std::mutex> lock(g_lock);
    auto it = g_instanceDispatch.find(getDispatchKey(instance));
    if (it == g_instanceDispatch.end()) return;

    auto id = it->second;
    g_instanceDispatch.erase(it);
    lock.unlock();

    id.DestroyInstance(instance, pAllocator);
}

static void destroySwapchainResources(DeviceDispatch& disp, SwapchainData& sc) {
    seifg::deleteContext(sc.contextId);
    disp.DeviceWaitIdle(sc.device);

    for (auto& pass : sc.passes) {
        disp.DestroySemaphore(sc.device, pass.acquireSemaphore, nullptr);
        disp.FreeCommandBuffers(sc.device, sc.cmdPool, 1, &pass.commandBuffer);
    }
    for (auto& ps : sc.presentSemaphores) {
        disp.DestroySemaphore(sc.device, ps.present, nullptr);
        disp.DestroySemaphore(sc.device, ps.chain, nullptr);
    }
    disp.FreeCommandBuffers(sc.device, sc.cmdPool, 1, &sc.renderCmd);

    for (auto& img : sc.sourceImages) {
        disp.DestroyImage(sc.device, img.image, nullptr);
        disp.FreeMemory(sc.device, img.memory, nullptr);
    }
    for (auto& img : sc.destImages) {
        disp.DestroyImage(sc.device, img.image, nullptr);
        disp.FreeMemory(sc.device, img.memory, nullptr);
    }
    disp.DestroySemaphore(sc.device, sc.timelineSemaphore, nullptr);
    disp.DestroyFence(sc.device, sc.renderFence, nullptr);
    disp.DestroyCommandPool(sc.device, sc.cmdPool, nullptr);
}

static VkResult VKAPI_CALL seifg_CreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain) {
    std::unique_lock<std::mutex> lock(g_lock);
    auto devIt = g_deviceData.find(getDispatchKey(device));
    if (devIt == g_deviceData.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    DeviceData& devData = devIt->second;
    auto& disp = devData.dispatch;

    if (!devData.layerActive) {
        lock.unlock();
        return disp.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
        auto scIt = g_swapchains.find(pCreateInfo->oldSwapchain);
        if (scIt != g_swapchains.end()) {
            destroySwapchainResources(disp, scIt->second);
            g_swapchains.erase(scIt);
        }
    }

    lock.unlock();

    uint32_t multiplier = getMultiplier();

    VkSwapchainCreateInfoKHR modInfo = *pCreateInfo;
    modInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    modInfo.minImageCount += multiplier;

    VkSurfaceCapabilitiesKHR surfCaps{};
    auto getCapsFn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        g_nextGIPA(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    if (getCapsFn) {
        getCapsFn(devData.physicalDevice, pCreateInfo->surface, &surfCaps);
        if (surfCaps.maxImageCount > 0 && modInfo.minImageCount > surfCaps.maxImageCount)
            modInfo.minImageCount = surfCaps.maxImageCount;
    }
    modInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;

    VkResult res = disp.CreateSwapchainKHR(device, &modInfo, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) return res;

    uint32_t imageCount = 0;
    disp.GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    disp.GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, images.data());

    VkExtent2D extent = pCreateInfo->imageExtent;
    VkFormat swapFormat = pCreateInfo->imageFormat;
    bool isHdr = (static_cast<int>(swapFormat) > 57);
    VkFormat shareFormat = isHdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    std::vector<ExportedImage> sourceImgs(2);
    for (auto& src : sourceImgs) {
        src = createExportableImage(devData, extent, shareFormat,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (src.fd < 0) {
            std::cerr << "seifg-vk: failed to create source image, disabling layer\n";
            devData.layerActive = false;
            return VK_SUCCESS;
        }
    }

    uint32_t destCount = multiplier - 1;
    std::vector<ExportedImage> destImgs(destCount);
    for (auto& dst : destImgs) {
        dst = createExportableImage(devData, extent, shareFormat,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (dst.fd < 0) {
            std::cerr << "seifg-vk: failed to create dest image, disabling layer\n";
            devData.layerActive = false;
            return VK_SUCCESS;
        }
    }

    VkSemaphoreTypeCreateInfo timelineTypeInfo{};
    timelineTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineTypeInfo.initialValue = 0;

    VkExportSemaphoreCreateInfo exportSemInfo{};
    exportSemInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportSemInfo.pNext = &timelineTypeInfo;
    exportSemInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &exportSemInfo;

    VkSemaphore timelineSem{};
    res = disp.CreateSemaphore(device, &semInfo, nullptr, &timelineSem);
    if (res != VK_SUCCESS) {
        std::cerr << "seifg-vk: failed to create timeline semaphore: " << res << "\n";
        devData.layerActive = false;
        return VK_SUCCESS;
    }

    VkSemaphoreGetFdInfoKHR semFdInfo{};
    semFdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    semFdInfo.semaphore = timelineSem;
    semFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int syncFd = -1;
    res = disp.GetSemaphoreFdKHR(device, &semFdInfo, &syncFd);
    if (res != VK_SUCCESS || syncFd < 0) {
        std::cerr << "seifg-vk: vkGetSemaphoreFdKHR failed: " << res << "\n";
        disp.DestroySemaphore(device, timelineSem, nullptr);
        devData.layerActive = false;
        return VK_SUCCESS;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = devData.queueFamilyIndex;

    VkCommandPool cmdPool{};
    disp.CreateCommandPool(device, &poolInfo, nullptr, &cmdPool);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence renderFence{};
    disp.CreateFence(device, &fenceInfo, nullptr, &renderFence);

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer renderCmd{};
    disp.AllocateCommandBuffers(device, &cmdAllocInfo, &renderCmd);

    std::vector<RenderPass> passes(destCount);
    for (auto& pass : passes) {
        disp.AllocateCommandBuffers(device, &cmdAllocInfo, &pass.commandBuffer);

        VkSemaphoreCreateInfo binSemInfo{};
        binSemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        disp.CreateSemaphore(device, &binSemInfo, nullptr, &pass.acquireSemaphore);
    }

    size_t semPoolSize = std::max(images.size(), static_cast<size_t>(destCount + 2));
    std::vector<SwapchainData::PresentSemaphores> presentSemaphores(semPoolSize);
    for (auto& ps : presentSemaphores) {
        VkSemaphoreCreateInfo binSemInfo{};
        binSemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        disp.CreateSemaphore(device, &binSemInfo, nullptr, &ps.present);
        disp.CreateSemaphore(device, &binSemInfo, nullptr, &ps.chain);
    }

    if (!g_engineInitialized) {
        std::string targetName = devData.deviceName;
        uint32_t targetVendor = devData.vendorID;
        uint32_t targetDevice = devData.deviceID;

        setenv("SEIFG_DISABLE", "1", 1);
        bool ok = seifg::initializeWithPicker(
            [&](const std::string& name, uint32_t vendorID, uint32_t deviceID) {
                if (vendorID != targetVendor || deviceID != targetDevice)
                    return false;
                return name == targetName;
            },
            isHdr,
            getQuality(),
            multiplier,
            [](const std::string&) -> std::vector<uint8_t> { return {}; }
        );
        unsetenv("SEIFG_DISABLE");

        if (!ok) {
            std::cerr << "seifg-vk: failed to initialize engine\n";
            disp.DestroySemaphore(device, timelineSem, nullptr);
            disp.DestroyFence(device, renderFence, nullptr);
            disp.FreeCommandBuffers(device, cmdPool, 1, &renderCmd);
            for (auto& pass : passes) {
                disp.DestroySemaphore(device, pass.acquireSemaphore, nullptr);
                disp.FreeCommandBuffers(device, cmdPool, 1, &pass.commandBuffer);
            }
            for (auto& ps : presentSemaphores) {
                disp.DestroySemaphore(device, ps.present, nullptr);
                disp.DestroySemaphore(device, ps.chain, nullptr);
            }
            disp.DestroyCommandPool(device, cmdPool, nullptr);
            for (auto& img : sourceImgs) {
                disp.DestroyImage(device, img.image, nullptr);
                disp.FreeMemory(device, img.memory, nullptr);
            }
            for (auto& img : destImgs) {
                disp.DestroyImage(device, img.image, nullptr);
                disp.FreeMemory(device, img.memory, nullptr);
            }
            return VK_SUCCESS;
        }
        g_engineInitialized = true;
    }

    std::vector<int> destFds;
    destFds.reserve(destCount);
    for (auto& d : destImgs)
        destFds.push_back(d.fd);

    int32_t ctxId = seifg::createContextFromFd(
        sourceImgs[0].fd, sourceImgs[1].fd,
        destFds, extent, shareFormat);

    if (ctxId < 0) {
        std::cerr << "seifg-vk: failed to create context\n";
        disp.DestroySemaphore(device, timelineSem, nullptr);
        disp.DestroyFence(device, renderFence, nullptr);
        disp.FreeCommandBuffers(device, cmdPool, 1, &renderCmd);
        for (auto& pass : passes) {
            disp.DestroySemaphore(device, pass.acquireSemaphore, nullptr);
            disp.FreeCommandBuffers(device, cmdPool, 1, &pass.commandBuffer);
        }
        for (auto& ps : presentSemaphores) {
            disp.DestroySemaphore(device, ps.present, nullptr);
            disp.DestroySemaphore(device, ps.chain, nullptr);
        }
        disp.DestroyCommandPool(device, cmdPool, nullptr);
        for (auto& img : sourceImgs) {
            disp.DestroyImage(device, img.image, nullptr);
            disp.FreeMemory(device, img.memory, nullptr);
        }
        for (auto& img : destImgs) {
            disp.DestroyImage(device, img.image, nullptr);
            disp.FreeMemory(device, img.memory, nullptr);
        }
        return VK_SUCCESS;
    }

    seifg::importTimelineSemaphore(syncFd);

    SwapchainData sc{};
    sc.device = device;
    sc.swapchainImages = std::move(images);
    sc.extent = extent;
    sc.format = shareFormat;
    sc.multiplier = multiplier;
    sc.sourceImages = std::move(sourceImgs);
    sc.destImages = std::move(destImgs);
    sc.timelineSemaphore = timelineSem;
    sc.timelineFd = syncFd;
    sc.contextId = ctxId;
    sc.idx = 1;
    sc.fidx = 0;
    sc.cmdPool = cmdPool;
    sc.renderCmd = renderCmd;
    sc.renderFence = renderFence;
    sc.fenceSubmitted = false;
    sc.passes = std::move(passes);
    sc.presentSemaphores = std::move(presentSemaphores);

    std::lock_guard<std::mutex> lk(g_lock);
    g_swapchains[*pSwapchain] = std::move(sc);

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL seifg_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    std::unique_lock<std::mutex> lock(g_lock);
    auto devIt = g_deviceData.find(getDispatchKey(queue));
    if (devIt == g_deviceData.end()) {
        lock.unlock();
        return VK_ERROR_DEVICE_LOST;
    }

    DeviceData& devData = devIt->second;
    auto& disp = devData.dispatch;

    if (!devData.layerActive) {
        lock.unlock();
        return disp.QueuePresentKHR(queue, pPresentInfo);
    }

    for (uint32_t si = 0; si < pPresentInfo->swapchainCount; si++) {
        VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[si];
        auto scIt = g_swapchains.find(swapchain);
        if (scIt == g_swapchains.end()) {
            lock.unlock();
            return disp.QueuePresentKHR(queue, pPresentInfo);
        }

        SwapchainData& sc = scIt->second;
        uint32_t imageIdx = pPresentInfo->pImageIndices[si];
        lock.unlock();

        void* next_chain = (pPresentInfo->swapchainCount == 1)
            ? const_cast<void*>(pPresentInfo->pNext) : nullptr;
        {
            auto* pm = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(next_chain);
            while (pm) {
                if (pm->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                    for (uint32_t m = 0; m < pm->swapchainCount; m++)
                        const_cast<VkPresentModeKHR*>(pm->pPresentModes)[m] = VK_PRESENT_MODE_FIFO_KHR;
                }
                pm = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(const_cast<void*>(pm->pNext));
            }
        }

        if (sc.fenceSubmitted) {
            VkResult fenceRes = disp.WaitForFences(sc.device, 1, &sc.renderFence, VK_TRUE, 150000000ULL);
            if (fenceRes == VK_TIMEOUT) {
                VkPresentInfoKHR passInfo = *pPresentInfo;
                return disp.QueuePresentKHR(queue, &passInfo);
            }
            disp.ResetFences(sc.device, 1, &sc.renderFence);
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        disp.ResetCommandBuffer(sc.renderCmd, 0);
        disp.BeginCommandBuffer(sc.renderCmd, &beginInfo);

        VkImage swapImg = sc.swapchainImages[imageIdx];
        VkImage srcImg = sc.sourceImages[sc.fidx % 2].image;

        VkImageMemoryBarrier preBarriers[2] = {
            makeBarrier(swapImg, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            makeBarrier(srcImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        };

        disp.CmdPipelineBarrier(sc.renderCmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, preBarriers);

        VkImageBlit blitRegion{};
        blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.srcOffsets[0] = { 0, 0, 0 };
        blitRegion.srcOffsets[1] = { static_cast<int32_t>(sc.extent.width),
            static_cast<int32_t>(sc.extent.height), 1 };
        blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blitRegion.dstOffsets[0] = { 0, 0, 0 };
        blitRegion.dstOffsets[1] = { static_cast<int32_t>(sc.extent.width),
            static_cast<int32_t>(sc.extent.height), 1 };

        disp.CmdBlitImage(sc.renderCmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            srcImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blitRegion, VK_FILTER_NEAREST);

        VkImageMemoryBarrier postBarriers[2] = {
            makeBarrier(swapImg,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
            makeBarrier(srcImg,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
        };

        disp.CmdPipelineBarrier(sc.renderCmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 2, postBarriers);

        disp.EndCommandBuffer(sc.renderCmd);

        uint64_t E = sc.idx;

        std::vector<VkSemaphore> waitSems;
        std::vector<uint64_t> waitVals;
        std::vector<VkPipelineStageFlags> waitStages;
        for (uint32_t w = 0; w < pPresentInfo->waitSemaphoreCount; w++) {
            waitSems.push_back(pPresentInfo->pWaitSemaphores[w]);
            waitVals.push_back(0);
            waitStages.push_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        }

        VkSemaphore signalSems[] = { sc.timelineSemaphore };
        uint64_t signalVals[] = { E };

        VkTimelineSemaphoreSubmitInfo tsInfo{};
        tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitVals.size());
        tsInfo.pWaitSemaphoreValues = waitVals.data();
        tsInfo.signalSemaphoreValueCount = 1;
        tsInfo.pSignalSemaphoreValues = signalVals;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = &tsInfo;
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
        submitInfo.pWaitSemaphores = waitSems.data();
        submitInfo.pWaitDstStageMask = waitStages.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &sc.renderCmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSems;

        disp.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);

        uint32_t K = static_cast<uint32_t>(sc.destImages.size());
        seifg::presentContextTimeline(sc.contextId, E, E + K);

        sc.idx++;

        VkResult lastRes = VK_SUCCESS;
        for (uint32_t i = 0; i < K; i++) {
            auto& pass = sc.passes[i];
            auto& pcs = sc.presentSemaphores[sc.idx % sc.presentSemaphores.size()];

            uint32_t aqIdx = 0;
            VkResult aqRes = disp.AcquireNextImageKHR(sc.device, swapchain,
                UINT64_MAX, pass.acquireSemaphore, VK_NULL_HANDLE, &aqIdx);

            if (aqRes == VK_ERROR_OUT_OF_DATE_KHR) {
                std::lock_guard<std::mutex> lk(g_lock);
                sc.idx = E + K + 1;
                sc.fidx++;
                sc.fenceSubmitted = false;
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            VkImage destImg = sc.destImages[i].image;
            VkImage aqSwapImg = sc.swapchainImages[aqIdx];

            disp.ResetCommandBuffer(pass.commandBuffer, 0);
            disp.BeginCommandBuffer(pass.commandBuffer, &beginInfo);

            VkImageMemoryBarrier destPreBarriers[2] = {
                makeBarrier(destImg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
                makeBarrier(aqSwapImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            };
            disp.CmdPipelineBarrier(pass.commandBuffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 2, destPreBarriers);

            disp.CmdBlitImage(pass.commandBuffer, destImg, VK_IMAGE_LAYOUT_GENERAL,
                aqSwapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blitRegion, VK_FILTER_NEAREST);

            VkImageMemoryBarrier destPostBarrier = makeBarrier(aqSwapImg,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            disp.CmdPipelineBarrier(pass.commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &destPostBarrier);

            disp.EndCommandBuffer(pass.commandBuffer);

            uint64_t destWaitVal = E + 1 + i;
            std::vector<VkSemaphore> destWaitSems = { pass.acquireSemaphore, sc.timelineSemaphore };
            uint64_t destWaitVals[] = { 0, destWaitVal };
            VkPipelineStageFlags destWaitStages[] = {
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };

            if (i > 0) {
                auto& prevPcs = sc.presentSemaphores[(sc.idx - 1) % sc.presentSemaphores.size()];
                destWaitSems.push_back(prevPcs.chain);
                destWaitVals[1] = destWaitVal;
            }

            std::vector<VkSemaphore> destSignalSems = { pcs.present, pcs.chain };
            std::vector<uint64_t> destSignalVals = { 0, 0 };

            std::vector<uint64_t> destWaitValsVec(destWaitSems.size(), 0);
            destWaitValsVec[1] = destWaitVal;

            std::vector<VkPipelineStageFlags> destWaitStagesVec(destWaitSems.size(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

            VkTimelineSemaphoreSubmitInfo destTsInfo{};
            destTsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            destTsInfo.waitSemaphoreValueCount = static_cast<uint32_t>(destWaitValsVec.size());
            destTsInfo.pWaitSemaphoreValues = destWaitValsVec.data();
            destTsInfo.signalSemaphoreValueCount = static_cast<uint32_t>(destSignalVals.size());
            destTsInfo.pSignalSemaphoreValues = destSignalVals.data();

            VkSubmitInfo destSubmitInfo{};
            destSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            destSubmitInfo.pNext = &destTsInfo;
            destSubmitInfo.waitSemaphoreCount = static_cast<uint32_t>(destWaitSems.size());
            destSubmitInfo.pWaitSemaphores = destWaitSems.data();
            destSubmitInfo.pWaitDstStageMask = destWaitStagesVec.data();
            destSubmitInfo.commandBufferCount = 1;
            destSubmitInfo.pCommandBuffers = &pass.commandBuffer;
            destSubmitInfo.signalSemaphoreCount = static_cast<uint32_t>(destSignalSems.size());
            destSubmitInfo.pSignalSemaphores = destSignalSems.data();

            VkFence submitFence = (i == K - 1) ? sc.renderFence : VK_NULL_HANDLE;
            disp.QueueSubmit(queue, 1, &destSubmitInfo, submitFence);

            VkPresentInfoKHR destPresent{};
            destPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            destPresent.pNext = (i == 0) ? next_chain : nullptr;
            destPresent.waitSemaphoreCount = 1;
            destPresent.pWaitSemaphores = &pcs.present;
            destPresent.swapchainCount = 1;
            destPresent.pSwapchains = &swapchain;
            destPresent.pImageIndices = &aqIdx;

            lastRes = disp.QueuePresentKHR(queue, &destPresent);

            sc.idx++;

            if (lastRes == VK_ERROR_OUT_OF_DATE_KHR) {
                std::lock_guard<std::mutex> lk(g_lock);
                sc.idx = E + K + 1;
                sc.fidx++;
                sc.fenceSubmitted = true;
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }

        auto& lastPcs = sc.presentSemaphores[(sc.idx - 1) % sc.presentSemaphores.size()];

        VkPresentInfoKHR origPresent{};
        origPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        origPresent.waitSemaphoreCount = 1;
        origPresent.pWaitSemaphores = &lastPcs.chain;
        origPresent.swapchainCount = 1;
        origPresent.pSwapchains = &swapchain;
        origPresent.pImageIndices = &imageIdx;

        lastRes = disp.QueuePresentKHR(queue, &origPresent);

        std::lock_guard<std::mutex> lk(g_lock);
        sc.fidx++;
        sc.fenceSubmitted = true;

        return lastRes;
    }

    return disp.QueuePresentKHR(queue, pPresentInfo);
}

static void VKAPI_CALL seifg_DestroySwapchainKHR(
        VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    std::unique_lock<std::mutex> lock(g_lock);
    auto devIt = g_deviceData.find(getDispatchKey(device));
    if (devIt == g_deviceData.end()) return;

    auto& disp = devIt->second.dispatch;

    auto scIt = g_swapchains.find(swapchain);
    if (scIt != g_swapchains.end()) {
        destroySwapchainResources(disp, scIt->second);
        g_swapchains.erase(scIt);
    }
    lock.unlock();

    disp.DestroySwapchainKHR(device, swapchain, pAllocator);
}

static PFN_vkVoidFunction VKAPI_CALL seifg_GetDeviceProcAddr(VkDevice device, const char* pName);
static PFN_vkVoidFunction VKAPI_CALL seifg_GetInstanceProcAddr(VkInstance instance, const char* pName);

static PFN_vkVoidFunction interceptFunction(const char* name) {
    if (!name) return nullptr;

    if (!std::strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_CreateInstance);
    if (!std::strcmp(name, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_CreateDevice);
    if (!std::strcmp(name, "vkDestroyDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_DestroyDevice);
    if (!std::strcmp(name, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_DestroyInstance);
    if (!std::strcmp(name, "vkCreateSwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_CreateSwapchainKHR);
    if (!std::strcmp(name, "vkQueuePresentKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_QueuePresentKHR);
    if (!std::strcmp(name, "vkDestroySwapchainKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_DestroySwapchainKHR);
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(seifg_GetInstanceProcAddr);
    return nullptr;
}

static PFN_vkVoidFunction VKAPI_CALL seifg_GetDeviceProcAddr(VkDevice device, const char* pName) {
    auto fn = interceptFunction(pName);
    if (fn) return fn;

    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_deviceData.find(getDispatchKey(device));
    if (it == g_deviceData.end()) return nullptr;
    return it->second.dispatch.GetDeviceProcAddr(device, pName);
}

static PFN_vkVoidFunction VKAPI_CALL seifg_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    auto fn = interceptFunction(pName);
    if (fn) return fn;

    if (!g_nextGIPA) return nullptr;
    return g_nextGIPA(instance, pName);
}

extern "C" __attribute__((visibility("default")))
VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = seifg_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = seifg_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
