#include "seifg.h"
#include <volk.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <unistd.h>

static const uint32_t W = 256;
static const uint32_t H = 256;
static const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return 0;
}

static uint8_t patPixel(int x, int y, int shiftX) {
    int sx = x - shiftX;
    int h = sx * 374761393 + y * 668265263;
    h = (h ^ (h >> 13)) * 1274126177;
    return (uint8_t)((h ^ (h >> 16)) & 0xFF);
}

struct ProducerDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
};

struct ExportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;
};

static bool createProducer(ProducerDevice& p) {
    VkResult res = volkInitialize();
    if (res != VK_SUCCESS) return false;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instCI{};
    instCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;

    res = vkCreateInstance(&instCI, nullptr, &p.instance);
    if (res != VK_SUCCESS) return false;
    volkLoadInstance(p.instance);

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(p.instance, &gpuCount, nullptr);
    if (gpuCount == 0) return false;
    VkPhysicalDevice gpus[8];
    gpuCount = gpuCount > 8 ? 8 : gpuCount;
    vkEnumeratePhysicalDevices(p.instance, &gpuCount, gpus);
    p.physDev = gpus[0];

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p.physDev, &qfCount, nullptr);
    VkQueueFamilyProperties qfProps[16];
    qfCount = qfCount > 16 ? 16 : qfCount;
    vkGetPhysicalDeviceQueueFamilyProperties(p.physDev, &qfCount, qfProps);

    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            p.queueFamily = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = p.queueFamily;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &priority;

    const char* exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo devCI{};
    devCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.pNext = &timelineFeatures;
    devCI.queueCreateInfoCount = 1;
    devCI.pQueueCreateInfos = &queueCI;
    devCI.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
    devCI.ppEnabledExtensionNames = exts;

    res = vkCreateDevice(p.physDev, &devCI, nullptr, &p.device);
    if (res != VK_SUCCESS) return false;

    volkLoadDevice(p.device);
    vkGetDeviceQueue(p.device, p.queueFamily, 0, &p.queue);

    VkCommandPoolCreateInfo cpCI{};
    cpCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpCI.queueFamilyIndex = p.queueFamily;
    res = vkCreateCommandPool(p.device, &cpCI, nullptr, &p.pool);
    return res == VK_SUCCESS;
}

static bool createExportedImage(ProducerDevice& p, ExportedImage& ei, VkImageUsageFlags usage) {
    VkExternalMemoryImageCreateInfo extMemCI{};
    extMemCI.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemCI.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.pNext = &extMemCI;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.format = FMT;
    imageCI.extent = {W, H, 1};
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage = usage;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res = vkCreateImage(p.device, &imageCI, nullptr, &ei.image);
    if (res != VK_SUCCESS) { printf("FAIL: vkCreateImage %d\n", res); return false; }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(p.device, ei.image, &memReqs);

    VkExportMemoryAllocateInfo exportMem{};
    exportMem.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportMem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &exportMem;
    dedicatedInfo.image = ei.image;

    uint32_t memIndex = findMemoryType(p.physDev, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memIndex;

    res = vkAllocateMemory(p.device, &allocInfo, nullptr, &ei.memory);
    if (res != VK_SUCCESS) { printf("FAIL: vkAllocateMemory %d\n", res); return false; }

    res = vkBindImageMemory(p.device, ei.image, ei.memory, 0);
    if (res != VK_SUCCESS) { printf("FAIL: vkBindImageMemory %d\n", res); return false; }

    VkMemoryGetFdInfoKHR getFdInfo{};
    getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    getFdInfo.memory = ei.memory;
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    res = vkGetMemoryFdKHR(p.device, &getFdInfo, &ei.fd);
    if (res != VK_SUCCESS) { printf("FAIL: vkGetMemoryFdKHR %d\n", res); return false; }

    return true;
}

static bool uploadFrame(ProducerDevice& p, VkImage dst, int shiftX) {
    VkDeviceSize bufSize = W * H * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkBuffer stagingBuf;
    if (vkCreateBuffer(p.device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements breqs;
    vkGetBufferMemoryRequirements(p.device, stagingBuf, &breqs);

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breqs.size;
    bai.memoryTypeIndex = findMemoryType(p.physDev, breqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory bufMem;
    if (vkAllocateMemory(p.device, &bai, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(p.device, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(p.device, stagingBuf, bufMem, 0);

    void* mapped;
    vkMapMemory(p.device, bufMem, 0, bufSize, 0, &mapped);
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint8_t v = patPixel((int)x, (int)y, shiftX);
            pixels[(y * W + x) * 4 + 0] = v;
            pixels[(y * W + x) * 4 + 1] = v;
            pixels[(y * W + x) * 4 + 2] = v;
            pixels[(y * W + x) * 4 + 3] = 255;
        }
    }
    vkUnmapMemory(p.device, bufMem);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = p.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(p.device, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = dst;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask = 0;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = p.queueFamily;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    toGeneral.image = dst;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(p.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(p.queue);

    vkFreeCommandBuffers(p.device, p.pool, 1, &cmd);
    vkDestroyBuffer(p.device, stagingBuf, nullptr);
    vkFreeMemory(p.device, bufMem, nullptr);
    return true;
}

static bool readbackImage(ProducerDevice& p, VkImage src, std::vector<uint8_t>& out) {
    VkDeviceSize bufSize = W * H * 4;
    out.resize(bufSize);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer buf;
    if (vkCreateBuffer(p.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements breqs;
    vkGetBufferMemoryRequirements(p.device, buf, &breqs);

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breqs.size;
    bai.memoryTypeIndex = findMemoryType(p.physDev, breqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory bufMem;
    if (vkAllocateMemory(p.device, &bai, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(p.device, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(p.device, buf, bufMem, 0);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = p.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(p.device, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = p.queueFamily;
    acquire.image = src;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &acquire);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(p.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(p.queue);

    void* mapped;
    vkMapMemory(p.device, bufMem, 0, bufSize, 0, &mapped);
    memcpy(out.data(), mapped, bufSize);
    vkUnmapMemory(p.device, bufMem);

    vkFreeCommandBuffers(p.device, p.pool, 1, &cmd);
    vkDestroyBuffer(p.device, buf, nullptr);
    vkFreeMemory(p.device, bufMem, nullptr);
    return true;
}

static bool createExportedTimelineSemaphore(ProducerDevice& p, VkSemaphore* outSem, int* outFd) {
    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.pNext = &exportInfo;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semCI.pNext = &typeInfo;

    VkResult res = vkCreateSemaphore(p.device, &semCI, nullptr, outSem);
    if (res != VK_SUCCESS) { printf("FAIL: vkCreateSemaphore timeline %d\n", res); return false; }

    VkSemaphoreGetFdInfoKHR getFdInfo{};
    getFdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    getFdInfo.semaphore = *outSem;
    getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    res = vkGetSemaphoreFdKHR(p.device, &getFdInfo, outFd);
    if (res != VK_SUCCESS) {
        printf("FAIL: vkGetSemaphoreFdKHR %d\n", res);
        vkDestroySemaphore(p.device, *outSem, nullptr);
        *outSem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void signalTimeline(ProducerDevice& p, VkSemaphore sem, uint64_t value) {
    VkSemaphoreSignalInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signalInfo.semaphore = sem;
    signalInfo.value = value;
    vkSignalSemaphore(p.device, &signalInfo);
}

static bool waitTimeline(ProducerDevice& p, VkSemaphore sem, uint64_t value) {
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &sem;
    waitInfo.pValues = &value;
    VkResult res = vkWaitSemaphores(p.device, &waitInfo, 5000000000ULL);
    return res == VK_SUCCESS;
}

int main() {
    printf("=== dmabuf_roundtrip test ===\n");
    printf("Image: %ux%u RGBA8\n", W, H);

    ProducerDevice producer{};
    if (!createProducer(producer)) {
        printf("FAIL: producer device creation\n");
        return 1;
    }
    printf("PASS: producer device created\n");

    VkImageUsageFlags srcUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageUsageFlags dstUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    ExportedImage in0{}, in1{}, out0{};
    if (!createExportedImage(producer, in0, srcUsage)) { printf("FAIL: export in0\n"); return 1; }
    if (!createExportedImage(producer, in1, srcUsage)) { printf("FAIL: export in1\n"); return 1; }
    if (!createExportedImage(producer, out0, dstUsage)) { printf("FAIL: export out0\n"); return 1; }
    printf("PASS: exported images created (in0 fd=%d, in1 fd=%d, out0 fd=%d)\n",
           in0.fd, in1.fd, out0.fd);

    const int SHIFT = 8;
    if (!uploadFrame(producer, in0.image, 0)) { printf("FAIL: upload in0\n"); return 1; }
    if (!uploadFrame(producer, in1.image, SHIFT)) { printf("FAIL: upload in1\n"); return 1; }
    printf("PASS: frames uploaded to producer\n");

    int syncFd = -1;
    VkSemaphore producerSem = VK_NULL_HANDLE;
    if (!createExportedTimelineSemaphore(producer, &producerSem, &syncFd)) {
        printf("FAIL: timeline semaphore export\n");
        return 1;
    }
    printf("PASS: timeline semaphore exported (fd=%d)\n", syncFd);

    int in0FdDup = dup(in0.fd);
    int in1FdDup = dup(in1.fd);
    int out0FdDup = dup(out0.fd);
    int syncFdDup = dup(syncFd);

    seifg::initialize(0, false, 4, 2, {});
    VkDevice seifgDev = seifg::getDevice();
    if (!seifgDev) { printf("FAIL: seifg init\n"); return 1; }
    printf("PASS: seifg engine initialized\n");

    std::vector<int> outFds = {out0FdDup};
    int32_t ctx = seifg::createContextFromFd(in0FdDup, in1FdDup, outFds,
        VkExtent2D{W, H}, FMT);
    if (ctx < 0) { printf("FAIL: createContextFromFd returned %d\n", ctx); return 1; }
    printf("PASS: context created from FDs (id=%d)\n", ctx);

    if (!seifg::importTimelineSemaphore(syncFdDup)) {
        printf("FAIL: importTimelineSemaphore\n");
        return 1;
    }
    printf("PASS: timeline semaphore imported\n");

    signalTimeline(producer, producerSem, 1);
    printf("INFO: producer signaled timeline=1\n");

    seifg::presentContextTimeline(ctx, 1, 2);
    printf("INFO: presentContextTimeline(wait=1, signal=2) dispatched\n");

    if (!waitTimeline(producer, producerSem, 2)) {
        printf("FAIL: timed out waiting for timeline=2 from consumer\n");
        return 1;
    }
    printf("PASS: producer received timeline=2 from consumer\n");

    std::vector<uint8_t> result;
    if (!readbackImage(producer, out0.image, result)) {
        printf("FAIL: readback output\n");
        return 1;
    }

    uint64_t sum = 0;
    uint32_t nonZero = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        uint8_t r = result[i * 4];
        sum += r;
        if (r != 0) nonZero++;
    }

    if (nonZero == 0) {
        printf("FAIL: output is all zeros\n");
        seifg::deleteContext(ctx);
        seifg::finalize();
        return 1;
    }

    double mean = (double)sum / (W * H);
    printf("PASS: output non-trivial (mean=%.1f, nonZero=%u/%u)\n", mean, nonZero, W * H);

    double diffIn0 = 0.0, diffIn1 = 0.0;
    for (uint32_t y = 32; y < H - 32; y++) {
        for (uint32_t x = 32; x < W - 32; x++) {
            uint8_t outV = result[(y * W + x) * 4];
            uint8_t ref0 = patPixel((int)x, (int)y, 0);
            uint8_t ref1 = patPixel((int)x, (int)y, SHIFT);
            diffIn0 += fabs((double)outV - ref0);
            diffIn1 += fabs((double)outV - ref1);
        }
    }
    uint32_t innerPixels = (W - 64) * (H - 64);
    diffIn0 /= innerPixels;
    diffIn1 /= innerPixels;

    printf("INFO: avgDiffFromIn0=%.2f avgDiffFromIn1=%.2f\n", diffIn0, diffIn1);

    seifg::deleteContext(ctx);
    seifg::finalize();

    vkDestroySemaphore(producer.device, producerSem, nullptr);
    vkDestroyImage(producer.device, in0.image, nullptr);
    vkFreeMemory(producer.device, in0.memory, nullptr);
    vkDestroyImage(producer.device, in1.image, nullptr);
    vkFreeMemory(producer.device, in1.memory, nullptr);
    vkDestroyImage(producer.device, out0.image, nullptr);
    vkFreeMemory(producer.device, out0.memory, nullptr);
    vkDestroyCommandPool(producer.device, producer.pool, nullptr);
    vkDestroyDevice(producer.device, nullptr);
    vkDestroyInstance(producer.instance, nullptr);

    if (in0.fd >= 0) close(in0.fd);
    if (in1.fd >= 0) close(in1.fd);
    if (out0.fd >= 0) close(out0.fd);
    if (syncFd >= 0) close(syncFd);

    printf("SUCCESS\n");
    return 0;
}
