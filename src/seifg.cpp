#include "seifg.h"
#include "engine.h"
#include <cstdlib>

namespace seifg {

static Engine* g_engine = nullptr;
static Image g_in0;
static Image g_in1;
static std::vector<Image> g_outN;
static int32_t g_ctxId = 0;

void initialize(uint64_t deviceUUID,
    bool, uint32_t quality, uint64_t,
    const std::function<std::vector<uint8_t>(const std::string&)>&) {
    if (g_engine) return;
    g_engine = new Engine();
    if (!g_engine->init(deviceUUID, quality)) {
        delete g_engine;
        g_engine = nullptr;
    }
}

#ifdef __ANDROID__
int32_t createContextFromAHB(
    AHardwareBuffer* in0, AHardwareBuffer* in1,
    const std::vector<AHardwareBuffer*>& outN,
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent) {
    if (!g_engine) return -1;

    VkDevice dev = g_engine->device.device;
    VkPhysicalDevice phys = g_engine->device.physicalDevice;

    g_in0.destroy(dev);
    g_in1.destroy(dev);
    for (auto& img : g_outN) img.destroy(dev);
    g_outN.clear();

    if (!g_in0.createFromAHB(dev, phys, in0, extent, format)) return -1;
    if (!g_in1.createFromAHB(dev, phys, in1, extent, format)) return -1;

    bool upscale = (outExtent.width > extent.width && outExtent.height > extent.height);
    VkExtent2D outE = upscale ? outExtent : extent;

    g_outN.resize(outN.size());
    for (size_t i = 0; i < outN.size(); ++i) {
        if (!g_outN[i].createFromAHB(dev, phys, outN[i], outE, format))
            return -1;
    }

    if (!g_engine->createResources(extent.width, extent.height, format, upscale ? outExtent.width : 0, upscale ? outExtent.height : 0)) return -1;

    g_ctxId = rand();
    return g_ctxId;
}
#endif

int32_t createContextFromImages(
    VkImage in0, VkImage in1,
    const std::vector<VkImage>& outN,
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent) {
    if (!g_engine) return -1;

    VkDevice dev = g_engine->device.device;

    g_in0.destroy(dev);
    g_in1.destroy(dev);
    for (auto& img : g_outN) img.destroy(dev);
    g_outN.clear();

    if (!g_in0.wrapExternal(dev, in0, extent, format)) return -1;
    if (!g_in1.wrapExternal(dev, in1, extent, format)) return -1;

    bool upscale = (outExtent.width > extent.width && outExtent.height > extent.height);
    VkExtent2D outE = upscale ? outExtent : extent;

    g_outN.resize(outN.size());
    for (size_t i = 0; i < outN.size(); ++i) {
        if (!g_outN[i].wrapExternal(dev, outN[i], outE, format))
            return -1;
    }

    if (!g_engine->createResources(extent.width, extent.height, format, upscale ? outExtent.width : 0, upscale ? outExtent.height : 0)) return -1;

    g_ctxId = rand();
    return g_ctxId;
}

void presentContext(int32_t, int, const std::vector<int>&) {
    if (!g_engine || g_outN.empty()) return;
    g_engine->recordAndSubmit(g_in0, g_in1, g_outN.data(), static_cast<uint32_t>(g_outN.size()));
}

void deleteContext(int32_t) {
    g_in0.destroy(g_engine ? g_engine->device.device : VK_NULL_HANDLE);
    g_in1.destroy(g_engine ? g_engine->device.device : VK_NULL_HANDLE);
    for (auto& img : g_outN)
        img.destroy(g_engine ? g_engine->device.device : VK_NULL_HANDLE);
    g_outN.clear();
    if (g_engine) g_engine->destroyResources();
}

void finalize() {
    if (!g_engine) return;
    g_engine->destroy();
    delete g_engine;
    g_engine = nullptr;
}

void setFlowDownscale(uint32_t levels) {
    if (!g_engine) return;
    if (levels < 1) levels = 1;
    if (levels > PYRAMID_LEVELS - 2) levels = PYRAMID_LEVELS - 2;
    g_engine->upscaleOnlyLevels = levels;
}

VkDevice getDevice() {
    return g_engine ? g_engine->device.device : VK_NULL_HANDLE;
}

VkPhysicalDevice getPhysicalDevice() {
    return g_engine ? g_engine->device.physicalDevice : VK_NULL_HANDLE;
}

#ifdef __ANDROID__
void waitIdle() {
    if (!g_engine) return;
    vkDeviceWaitIdle(g_engine->device.device);
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
int32_t createContextFromFd(int in0Fd, int in1Fd,
    const std::vector<int>& outFds,
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent) {
    if (!g_engine) return -1;

    VkDevice dev = g_engine->device.device;
    VkPhysicalDevice phys = g_engine->device.physicalDevice;
    VkImageUsageFlags srcUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags dstUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    g_in0.destroy(dev);
    g_in1.destroy(dev);
    for (auto& img : g_outN) img.destroy(dev);
    g_outN.clear();

    if (!g_in0.createFromFd(dev, phys, in0Fd, extent, format, srcUsage)) return -1;
    if (!g_in1.createFromFd(dev, phys, in1Fd, extent, format, srcUsage)) {
        g_in0.destroy(dev);
        return -1;
    }

    bool upscale = (outExtent.width > extent.width && outExtent.height > extent.height);
    VkExtent2D outE = upscale ? outExtent : extent;

    g_outN.resize(outFds.size());
    for (size_t i = 0; i < outFds.size(); ++i) {
        if (!g_outN[i].createFromFd(dev, phys, outFds[i], outE, format, dstUsage)) {
            g_in0.destroy(dev);
            g_in1.destroy(dev);
            for (size_t j = 0; j < i; ++j) g_outN[j].destroy(dev);
            g_outN.clear();
            return -1;
        }
    }

    if (!g_engine->createResources(extent.width, extent.height, format, upscale ? outExtent.width : 0, upscale ? outExtent.height : 0)) return -1;

    g_ctxId = rand();
    return g_ctxId;
}

bool importTimelineSemaphore(int syncFd) {
    if (!g_engine) return false;
    return g_engine->importTimelineSemaphore(syncFd);
}

void presentContextTimeline(int32_t, uint64_t waitValue, uint64_t signalValue) {
    if (!g_engine || g_outN.empty()) return;
    g_engine->recordAndSubmit(g_in0, g_in1, g_outN.data(), static_cast<uint32_t>(g_outN.size()),
                              g_engine->importedTimeline, waitValue, signalValue);
}

bool initializeWithPicker(
    const std::function<bool(const std::string& name, uint32_t vendorID, uint32_t deviceID)>& picker,
    bool, uint32_t quality, uint64_t,
    const std::function<std::vector<uint8_t>(const std::string&)>&) {
    if (g_engine) return true;
    g_engine = new Engine();
    if (!g_engine->initWithPicker(picker, quality)) {
        delete g_engine;
        g_engine = nullptr;
        return false;
    }
    return true;
}
#endif

}
