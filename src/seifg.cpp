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
    bool, float flowScale, uint64_t,
    const std::function<std::vector<uint8_t>(const std::string&)>&) {
    if (g_engine) return;
    g_engine = new Engine();
    if (!g_engine->init(deviceUUID, flowScale)) {
        delete g_engine;
        g_engine = nullptr;
    }
}

#ifdef __ANDROID__
int32_t createContextFromAHB(
    AHardwareBuffer* in0, AHardwareBuffer* in1,
    const std::vector<AHardwareBuffer*>& outN,
    VkExtent2D extent, VkFormat format) {
    if (!g_engine) return -1;

    VkDevice dev = g_engine->device.device;
    VkPhysicalDevice phys = g_engine->device.physicalDevice;

    g_in0.destroy(dev);
    g_in1.destroy(dev);
    for (auto& img : g_outN) img.destroy(dev);
    g_outN.clear();

    if (!g_in0.createFromAHB(dev, phys, in0, extent, format)) return -1;
    if (!g_in1.createFromAHB(dev, phys, in1, extent, format)) return -1;

    g_outN.resize(outN.size());
    for (size_t i = 0; i < outN.size(); ++i) {
        if (!g_outN[i].createFromAHB(dev, phys, outN[i], extent, format))
            return -1;
    }

    if (!g_engine->createResources(extent.width, extent.height)) return -1;

    g_ctxId = rand();
    return g_ctxId;
}
#endif

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

#ifdef __ANDROID__
void waitIdle() {
    if (!g_engine) return;
    vkDeviceWaitIdle(g_engine->device.device);
}
#endif

}
