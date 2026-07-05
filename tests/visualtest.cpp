#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "seifg.h"
#include <volk.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

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
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
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

static bool uploadFrame(VkDevice dev, VkPhysicalDevice phys, VkImage dst,
                        VkCommandPool pool, VkQueue queue,
                        const uint8_t* pixels, uint32_t w, uint32_t h) {
    VkDeviceSize bufSize = (VkDeviceSize)w * h * 4;
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
    memcpy(mapped, pixels, bufSize);
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
    region.imageExtent = {w, h, 1};
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
                          VkCommandPool pool, VkQueue queue,
                          std::vector<uint8_t>& out, uint32_t w, uint32_t h) {
    VkDeviceSize bufSize = (VkDeviceSize)w * h * 4;
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
    region.imageExtent = {w, h, 1};
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

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s <in0.png> <in1.png> <out_interp.png> [out_blend.png]\n", argv[0]);
        return 1;
    }

    const char* pathIn0 = argv[1];
    const char* pathIn1 = argv[2];
    const char* pathInterp = argv[3];
    const char* pathBlend = argc > 4 ? argv[4] : nullptr;

    int w0, h0, c0, w1, h1, c1;
    uint8_t* pix0 = stbi_load(pathIn0, &w0, &h0, &c0, 4);
    if (!pix0) {
        printf("FAIL: cannot load %s\n", pathIn0);
        return 1;
    }
    uint8_t* pix1 = stbi_load(pathIn1, &w1, &h1, &c1, 4);
    if (!pix1) {
        printf("FAIL: cannot load %s\n", pathIn1);
        stbi_image_free(pix0);
        return 1;
    }

    if (w0 != w1 || h0 != h1) {
        printf("FAIL: dimension mismatch %dx%d vs %dx%d\n", w0, h0, w1, h1);
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    uint32_t W = (uint32_t)w0;
    uint32_t H = (uint32_t)h0;
    printf("INPUT: %ux%u\n", W, H);

    double interFrameDiff = 0.0;
    size_t totalPixels = (size_t)W * H;
    for (size_t i = 0; i < totalPixels * 4; i++) {
        interFrameDiff += fabs((double)pix0[i] - (double)pix1[i]);
    }
    interFrameDiff /= (totalPixels * 4);
    printf("INTER_FRAME_MEAN_DIFF: %.3f\n", interFrameDiff);

    seifg::initialize(0, false, 4, 2, {});

    VkDevice dev = seifg::getDevice();
    VkPhysicalDevice phys = seifg::getPhysicalDevice();
    if (!dev || !phys) {
        printf("FAIL: engine initialization\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfProps.data());
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
        printf("FAIL: command pool\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags inputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags outputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    AllocatedImage imgIn0 = createImage(dev, phys, W, H, FMT, inputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AllocatedImage imgIn1 = createImage(dev, phys, W, H, FMT, inputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AllocatedImage imgOut = createImage(dev, phys, W, H, FMT, outputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (!imgIn0.image || !imgIn1.image || !imgOut.image) {
        printf("FAIL: image allocation\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    if (!uploadFrame(dev, phys, imgIn0.image, pool, queue, pix0, W, H)) {
        printf("FAIL: upload in0\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }
    if (!uploadFrame(dev, phys, imgIn1.image, pool, queue, pix1, W, H)) {
        printf("FAIL: upload in1\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    VkExtent2D extent{W, H};
    int32_t ctx = seifg::createContextFromImages(imgIn0.image, imgIn1.image, {imgOut.image}, extent, FMT);
    if (ctx < 0) {
        printf("FAIL: createContextFromImages returned %d\n", ctx);
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    seifg::presentContext(ctx, -1, {});
    vkDeviceWaitIdle(dev);

    std::vector<uint8_t> result;
    if (!readbackImage(dev, phys, imgOut.image, pool, queue, result, W, H)) {
        printf("FAIL: readback\n");
        stbi_image_free(pix0);
        stbi_image_free(pix1);
        return 1;
    }

    stbi_write_png(pathInterp, (int)W, (int)H, 4, result.data(), (int)(W * 4));
    printf("WROTE: %s\n", pathInterp);

    double diffOut0 = 0.0, diffOut1 = 0.0;
    for (size_t i = 0; i < totalPixels * 4; i++) {
        diffOut0 += fabs((double)result[i] - (double)pix0[i]);
        diffOut1 += fabs((double)result[i] - (double)pix1[i]);
    }
    diffOut0 /= (totalPixels * 4);
    diffOut1 /= (totalPixels * 4);
    printf("OUTPUT_VS_IN0_MEAN_DIFF: %.3f\n", diffOut0);
    printf("OUTPUT_VS_IN1_MEAN_DIFF: %.3f\n", diffOut1);

    std::vector<uint8_t> blend(totalPixels * 4);
    for (size_t i = 0; i < totalPixels * 4; i++) {
        blend[i] = (uint8_t)(((uint16_t)pix0[i] + (uint16_t)pix1[i]) / 2);
    }

    if (pathBlend) {
        stbi_write_png(pathBlend, (int)W, (int)H, 4, blend.data(), (int)(W * 4));
        printf("WROTE: %s\n", pathBlend);
    } else {
        std::string blendPath = std::string(pathInterp);
        size_t dot = blendPath.rfind('.');
        if (dot != std::string::npos)
            blendPath = blendPath.substr(0, dot) + "_blend.png";
        else
            blendPath += "_blend.png";
        stbi_write_png(blendPath.c_str(), (int)W, (int)H, 4, blend.data(), (int)(W * 4));
        printf("WROTE: %s\n", blendPath.c_str());
    }

    seifg::deleteContext(ctx);
    vkDestroyCommandPool(dev, pool, nullptr);
    seifg::finalize();

    stbi_image_free(pix0);
    stbi_image_free(pix1);

    printf("SUCCESS\n");
    return 0;
}
