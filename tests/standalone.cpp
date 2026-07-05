#include "seifg.h"
#include <volk.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

static const uint32_t W = 256;
static const uint32_t H = 256;
static const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct AllocatedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return 0;
}

static AllocatedImage createImage(VkDevice dev, VkPhysicalDevice phys,
                                   uint32_t w, uint32_t h, VkFormat fmt,
                                   VkImageUsageFlags usage, VkMemoryPropertyFlags memFlags) {
    AllocatedImage ai{};
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev, &ci, nullptr, &ai.image) != VK_SUCCESS) return ai;

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(dev, ai.image, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = findMemoryType(phys, reqs.memoryTypeBits, memFlags);

    if (vkAllocateMemory(dev, &alloc, nullptr, &ai.memory) != VK_SUCCESS) return ai;
    vkBindImageMemory(dev, ai.image, ai.memory, 0);
    return ai;
}

static void destroyImage(VkDevice dev, AllocatedImage& ai) {
    if (ai.image) vkDestroyImage(dev, ai.image, nullptr);
    if (ai.memory) vkFreeMemory(dev, ai.memory, nullptr);
    ai = {};
}

static uint8_t patPixel(int x, int y, int shiftX) {
    int sx = x - shiftX;
    int h = sx * 374761393 + y * 668265263;
    h = (h ^ (h >> 13)) * 1274126177;
    return (uint8_t)((h ^ (h >> 16)) & 0xFF);
}

static bool uploadFrame(VkDevice dev, VkPhysicalDevice phys, VkImage dst,
                        VkCommandPool pool, VkQueue queue, int shiftX) {
    VkDeviceSize bufSize = W * H * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkBuffer stagingBuf;
    if (vkCreateBuffer(dev, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements breqs;
    vkGetBufferMemoryRequirements(dev, stagingBuf, &breqs);

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breqs.size;
    bai.memoryTypeIndex = findMemoryType(phys, breqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory bufMem;
    if (vkAllocateMemory(dev, &bai, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, stagingBuf, nullptr);
        return false;
    }
    vkBindBufferMemory(dev, stagingBuf, bufMem, 0);

    void* mapped;
    vkMapMemory(dev, bufMem, 0, bufSize, 0, &mapped);
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
    vkUnmapMemory(dev, bufMem);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);

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
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = dst;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyBuffer(dev, stagingBuf, nullptr);
    vkFreeMemory(dev, bufMem, nullptr);
    return true;
}

static bool readbackImage(VkDevice dev, VkPhysicalDevice phys, VkImage src,
                          VkCommandPool pool, VkQueue queue, std::vector<uint8_t>& out) {
    VkDeviceSize bufSize = W * H * 4;
    out.resize(bufSize);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements breqs;
    vkGetBufferMemoryRequirements(dev, buf, &breqs);

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = breqs.size;
    bai.memoryTypeIndex = findMemoryType(phys, breqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory bufMem;
    if (vkAllocateMemory(dev, &bai, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(dev, buf, bufMem, 0);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = src;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    void* mapped;
    vkMapMemory(dev, bufMem, 0, bufSize, 0, &mapped);
    memcpy(out.data(), mapped, bufSize);
    vkUnmapMemory(dev, bufMem);

    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, bufMem, nullptr);
    return true;
}

int main() {
    seifg::initialize(0, false, 2, 2, {});

    VkDevice dev = seifg::getDevice();
    VkPhysicalDevice phys = seifg::getPhysicalDevice();
    if (!dev || !phys) {
        printf("FAIL: engine initialization (no Vulkan device?)\n");
        return 1;
    }
    printf("PASS: engine initialized\n");

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    VkQueueFamilyProperties qfProps[16];
    qfCount = qfCount > 16 ? 16 : qfCount;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfProps);
    uint32_t qf = 0;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    }

    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = qf;
    VkCommandPool pool;
    if (vkCreateCommandPool(dev, &cpci, nullptr, &pool) != VK_SUCCESS) {
        printf("FAIL: command pool creation\n");
        return 1;
    }

    const VkImageUsageFlags inputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags outputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    AllocatedImage in0 = createImage(dev, phys, W, H, FMT, inputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AllocatedImage in1 = createImage(dev, phys, W, H, FMT, inputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AllocatedImage out0 = createImage(dev, phys, W, H, FMT, outputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (!in0.image || !in1.image || !out0.image) {
        printf("FAIL: image allocation\n");
        return 1;
    }
    printf("PASS: images allocated\n");

    const int SHIFT = 8;
    if (!uploadFrame(dev, phys, in0.image, pool, queue, 0)) {
        printf("FAIL: upload in0\n");
        return 1;
    }
    if (!uploadFrame(dev, phys, in1.image, pool, queue, SHIFT)) {
        printf("FAIL: upload in1\n");
        return 1;
    }
    printf("PASS: frames uploaded\n");

    int32_t ctx = seifg::createContextFromImages(in0.image, in1.image, {out0.image},
        VkExtent2D{W, H}, FMT);
    if (ctx < 0) {
        printf("FAIL: createContextFromImages returned %d\n", ctx);
        return 1;
    }
    printf("PASS: context created id=%d\n", ctx);

    seifg::presentContext(ctx, -1, {});
    vkDeviceWaitIdle(dev);
    printf("PASS: presentContext completed\n");

    std::vector<uint8_t> result;
    if (!readbackImage(dev, phys, out0.image, pool, queue, result)) {
        printf("FAIL: readback\n");
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

    printf("INFO: avgDiffFromIn0=%.1f avgDiffFromIn1=%.1f\n", diffIn0, diffIn1);

    if (diffIn0 < 0.5 && diffIn1 < 0.5) {
        printf("WARN: output may be trivially identical to both inputs (static scene?)\n");
    }

    seifg::deleteContext(ctx);
    vkDestroyCommandPool(dev, pool, nullptr);
    destroyImage(dev, in0);
    destroyImage(dev, in1);
    destroyImage(dev, out0);
    seifg::finalize();

    printf("SUCCESS\n");
    return 0;
}
