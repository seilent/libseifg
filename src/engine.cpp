#include "engine.h"
#include <seifg_shaders.h>

namespace seifg {

static void barrier(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static void barrierMulti(VkCommandBuffer cmd, VkImage* images, uint32_t count) {
    VkImageMemoryBarrier barriers[16]{};
    for (uint32_t i = 0; i < count && i < 16; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = images[i];
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, count, barriers);
}

static void externalAcquire(VkCommandBuffer cmd, VkImage image, uint32_t dstFamily) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static void externalRelease(VkCommandBuffer cmd, VkImage image, uint32_t srcFamily) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool initPipeline(Pipeline& p, VkDevice dev, const uint8_t* spv, size_t size,
                          const VkDescriptorType* types, uint32_t count) {
    return p.init(dev, reinterpret_cast<const uint32_t*>(spv), static_cast<uint32_t>(size), types, count);
}

bool Engine::init(uint64_t deviceUUID, float fs) {
    flowScale = fs;

    if (!device.init(deviceUUID)) return false;
    VkDevice dev = device.device;

    if (!samplers.init(dev, device.hasImageProcessing)) return false;
    if (!commands.init(dev, device.computeQueueFamily)) return false;
    if (!descriptorPool.init(dev)) return false;

    VkDescriptorType lumaTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType pyramidTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType blockTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType blockTypesQcom[] = {VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM, VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType refineTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType refineTypesQcom[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM, VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType filterTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType occTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType warpTypes[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorType blendTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};

    if (!initPipeline(lumaConvertPipeline, dev, shaders::seifg_luma_convert_spv, shaders::seifg_luma_convert_spv_size, lumaTypes, 2)) return false;
    if (!initPipeline(pyramidDownsamplePipeline, dev, shaders::seifg_pyramid_downsample_spv, shaders::seifg_pyramid_downsample_spv_size, pyramidTypes, 2)) return false;
    if (useQcom) {
        if (!initPipeline(blockMatchCoarsePipeline, dev, shaders::seifg_block_match_coarse_qcom_spv, shaders::seifg_block_match_coarse_qcom_spv_size, blockTypesQcom, 3)) return false;
        if (!initPipeline(refineLevelPipeline, dev, shaders::seifg_refine_level_qcom_spv, shaders::seifg_refine_level_qcom_spv_size, refineTypesQcom, 4)) return false;
    } else {
        if (!initPipeline(blockMatchCoarsePipeline, dev, shaders::seifg_block_match_coarse_spv, shaders::seifg_block_match_coarse_spv_size, blockTypes, 3)) return false;
        if (!initPipeline(refineLevelPipeline, dev, shaders::seifg_refine_level_spv, shaders::seifg_refine_level_spv_size, refineTypes, 4)) return false;
    }
    if (!initPipeline(flowFilterPipeline, dev, shaders::seifg_flow_filter_spv, shaders::seifg_flow_filter_spv_size, filterTypes, 3)) return false;
    if (!initPipeline(occlusionPipeline, dev, shaders::seifg_occlusion_spv, shaders::seifg_occlusion_spv_size, occTypes, 3)) return false;
    if (!initPipeline(warpPipeline, dev, shaders::seifg_warp_spv, shaders::seifg_warp_spv_size, warpTypes, 3)) return false;
    if (!initPipeline(blendPipeline, dev, shaders::seifg_blend_spv, shaders::seifg_blend_spv_size, blendTypes, 6)) return false;

    return true;
}

bool Engine::createResources(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    VkDevice dev = device.device;
    VkPhysicalDevice phys = device.physicalDevice;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags lumaUsage = usage;
    if (useQcom)
        lumaUsage |= VK_IMAGE_USAGE_SAMPLE_BLOCK_MATCH_BIT_QCOM;

    for (uint32_t i = 0; i < PYRAMID_LEVELS; i++) {
        uint32_t lw = w >> i;
        uint32_t lh = h >> i;
        if (!lumaPrev[i].createInternal(dev, phys, VK_FORMAT_R8_UNORM, lw, lh, lumaUsage)) return false;
        if (!lumaCurr[i].createInternal(dev, phys, VK_FORMAT_R8_UNORM, lw, lh, lumaUsage)) return false;
    }

    uint32_t cw = w >> 4;
    uint32_t ch = h >> 4;
    if (!mvCoarse.createInternal(dev, phys, VK_FORMAT_R16G16_SFLOAT, cw, ch, usage)) return false;

    for (uint32_t i = 0; i < PYRAMID_LEVELS - 1; i++) {
        uint32_t rw = w >> (3 - i);
        uint32_t rh = h >> (3 - i);
        if (!mvRefined[i].createInternal(dev, phys, VK_FORMAT_R16G16_SFLOAT, rw, rh, usage)) return false;
    }

    if (!mvFiltered.createInternal(dev, phys, VK_FORMAT_R16G16_SFLOAT, w, h, usage)) return false;
    if (!mvBackward.createInternal(dev, phys, VK_FORMAT_R16G16_SFLOAT, w, h, usage)) return false;
    if (!confidence.createInternal(dev, phys, VK_FORMAT_R16_SFLOAT, w, h, usage)) return false;
    if (!warpedForward.createInternal(dev, phys, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, usage)) return false;
    if (!warpedBackward.createInternal(dev, phys, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, usage)) return false;

    auto& pool = descriptorPool;
    VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL;

    for (uint32_t i = 0; i < 2; i++) {
        if (!pool.allocate(dev, lumaConvertPipeline.descriptorSetLayout, &dsLumaConvert[i])) return false;
    }

    for (uint32_t i = 0; i < 8; i++) {
        if (!pool.allocate(dev, pyramidDownsamplePipeline.descriptorSetLayout, &dsPyramid[i])) return false;
    }

    if (!pool.allocate(dev, blockMatchCoarsePipeline.descriptorSetLayout, &dsBlockMatch)) return false;
    for (uint32_t i = 0; i < 4; i++) {
        if (!pool.allocate(dev, refineLevelPipeline.descriptorSetLayout, &dsRefine[i])) return false;
    }
    if (!pool.allocate(dev, flowFilterPipeline.descriptorSetLayout, &dsFlowFilter)) return false;
    if (!pool.allocate(dev, occlusionPipeline.descriptorSetLayout, &dsOcclusion)) return false;
    for (uint32_t i = 0; i < 2; i++) {
        if (!pool.allocate(dev, warpPipeline.descriptorSetLayout, &dsWarp[i])) return false;
    }
    if (!pool.allocate(dev, blendPipeline.descriptorSetLayout, &dsBlend)) return false;

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t src = i;
        uint32_t dst = i + 1;
        pool.updateCombinedImageSampler(dev, dsPyramid[i], 0, lumaPrev[src].view, samplers.bilinear, general);
        pool.updateStorageImage(dev, dsPyramid[i], 1, lumaPrev[dst].view, general);
        pool.updateCombinedImageSampler(dev, dsPyramid[i + 4], 0, lumaCurr[src].view, samplers.bilinear, general);
        pool.updateStorageImage(dev, dsPyramid[i + 4], 1, lumaCurr[dst].view, general);
    }

    if (useQcom) {
        pool.updateBlockMatchImage(dev, dsBlockMatch, 0, lumaPrev[4].view, samplers.unnormalized, general);
        pool.updateBlockMatchImage(dev, dsBlockMatch, 1, lumaCurr[4].view, samplers.unnormalized, general);
    } else {
        pool.updateStorageImage(dev, dsBlockMatch, 0, lumaPrev[4].view, general);
        pool.updateStorageImage(dev, dsBlockMatch, 1, lumaCurr[4].view, general);
    }
    pool.updateStorageImage(dev, dsBlockMatch, 2, mvCoarse.view, general);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t prevLevel = (i == 0) ? 4 : 3 - (i - 1);
        Image& prevMv = (i == 0) ? mvCoarse : mvRefined[i - 1];
        uint32_t pyrLevel = 3 - i;
        if (useQcom) {
            pool.updateStorageImage(dev, dsRefine[i], 0, prevMv.view, general);
            pool.updateBlockMatchImage(dev, dsRefine[i], 1, lumaPrev[pyrLevel].view, samplers.unnormalized, general);
            pool.updateBlockMatchImage(dev, dsRefine[i], 2, lumaCurr[pyrLevel].view, samplers.unnormalized, general);
        } else {
            pool.updateCombinedImageSampler(dev, dsRefine[i], 0, prevMv.view, samplers.bilinear, general);
            pool.updateCombinedImageSampler(dev, dsRefine[i], 1, lumaPrev[pyrLevel].view, samplers.bilinear, general);
            pool.updateCombinedImageSampler(dev, dsRefine[i], 2, lumaCurr[pyrLevel].view, samplers.bilinear, general);
        }
        pool.updateStorageImage(dev, dsRefine[i], 3, mvRefined[i].view, general);
    }

    pool.updateStorageImage(dev, dsFlowFilter, 0, mvRefined[3].view, general);
    pool.updateStorageImage(dev, dsFlowFilter, 1, mvFiltered.view, general);
    pool.updateStorageImage(dev, dsFlowFilter, 2, mvBackward.view, general);

    pool.updateStorageImage(dev, dsOcclusion, 0, mvFiltered.view, general);
    pool.updateStorageImage(dev, dsOcclusion, 1, mvBackward.view, general);
    pool.updateStorageImage(dev, dsOcclusion, 2, confidence.view, general);

    pool.updateStorageImage(dev, dsBlend, 0, warpedForward.view, general);
    pool.updateStorageImage(dev, dsBlend, 1, warpedBackward.view, general);
    pool.updateStorageImage(dev, dsBlend, 2, confidence.view, general);

    return true;
}

bool Engine::recordAndSubmit(Image& in0, Image& in1, Image& out, float t) {
    VkDevice dev = device.device;
    VkCommandBuffer cmd = commands.acquire(dev);
    VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t qf = device.computeQueueFamily;

    externalAcquire(cmd, in0.image, qf);
    externalAcquire(cmd, in1.image, qf);
    externalAcquire(cmd, out.image, qf);

    descriptorPool.updateCombinedImageSampler(dev, dsLumaConvert[0], 0, in0.view, samplers.nearest, general);
    descriptorPool.updateStorageImage(dev, dsLumaConvert[0], 1, lumaPrev[0].view, general);
    descriptorPool.updateCombinedImageSampler(dev, dsLumaConvert[1], 0, in1.view, samplers.nearest, general);
    descriptorPool.updateStorageImage(dev, dsLumaConvert[1], 1, lumaCurr[0].view, general);

    descriptorPool.updateCombinedImageSampler(dev, dsWarp[0], 0, in0.view, samplers.bilinear, general);
    descriptorPool.updateStorageImage(dev, dsWarp[0], 1, mvFiltered.view, general);
    descriptorPool.updateStorageImage(dev, dsWarp[0], 2, warpedForward.view, general);
    descriptorPool.updateCombinedImageSampler(dev, dsWarp[1], 0, in1.view, samplers.bilinear, general);
    descriptorPool.updateStorageImage(dev, dsWarp[1], 1, mvBackward.view, general);
    descriptorPool.updateStorageImage(dev, dsWarp[1], 2, warpedBackward.view, general);

    descriptorPool.updateCombinedImageSampler(dev, dsBlend, 3, in0.view, samplers.bilinear, general);
    descriptorPool.updateCombinedImageSampler(dev, dsBlend, 4, in1.view, samplers.bilinear, general);
    descriptorPool.updateStorageImage(dev, dsBlend, 5, out.view, general);

    SeifgPushConstants pc{};
    pc.width = width;
    pc.height = height;
    pc.flowScale = flowScale;
    pc.t = t;
    pc.threshold = 4.0f;
    pc.temperature = 5.0f;

    auto dispatch = [&](Pipeline& p, VkDescriptorSet ds, uint32_t dw, uint32_t dh) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipelineLayout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, p.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dw + 15) / 16, (dh + 15) / 16, 1);
    };

    dispatch(lumaConvertPipeline, dsLumaConvert[0], width, height);
    dispatch(lumaConvertPipeline, dsLumaConvert[1], width, height);

    VkImage lumaL0s[] = {lumaPrev[0].image, lumaCurr[0].image};
    barrierMulti(cmd, lumaL0s, 2);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t lw = width >> (i + 1);
        uint32_t lh = height >> (i + 1);
        pc.width = lw;
        pc.height = lh;
        pc.level = i + 1;
        dispatch(pyramidDownsamplePipeline, dsPyramid[i], lw, lh);
        dispatch(pyramidDownsamplePipeline, dsPyramid[i + 4], lw, lh);
        VkImage levelImgs[] = {lumaPrev[i + 1].image, lumaCurr[i + 1].image};
        barrierMulti(cmd, levelImgs, 2);
    }

    pc.width = width >> 4;
    pc.height = height >> 4;
    pc.level = 4;
    dispatch(blockMatchCoarsePipeline, dsBlockMatch, pc.width, pc.height);
    barrier(cmd, mvCoarse.image);

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t rw = width >> (3 - i);
        uint32_t rh = height >> (3 - i);
        pc.width = rw;
        pc.height = rh;
        pc.level = 3 - i;
        dispatch(refineLevelPipeline, dsRefine[i], rw, rh);
        barrier(cmd, mvRefined[i].image);
    }

    pc.width = width;
    pc.height = height;
    pc.level = 0;
    dispatch(flowFilterPipeline, dsFlowFilter, width, height);

    VkImage filterOut[] = {mvFiltered.image, mvBackward.image};
    barrierMulti(cmd, filterOut, 2);

    dispatch(occlusionPipeline, dsOcclusion, width, height);
    barrier(cmd, confidence.image);

    dispatch(warpPipeline, dsWarp[0], width, height);
    pc.t = 1.0f - t;
    dispatch(warpPipeline, dsWarp[1], width, height);
    pc.t = t;

    VkImage warpOut[] = {warpedForward.image, warpedBackward.image};
    barrierMulti(cmd, warpOut, 2);

    dispatch(blendPipeline, dsBlend, width, height);
    barrier(cmd, out.image);

    externalRelease(cmd, in0.image, qf);
    externalRelease(cmd, in1.image, qf);
    externalRelease(cmd, out.image, qf);

    return commands.submit(dev, device.computeQueue);
}

void Engine::destroyResources() {
    VkDevice dev = device.device;
    for (uint32_t i = 0; i < PYRAMID_LEVELS; i++) {
        lumaPrev[i].destroy(dev);
        lumaCurr[i].destroy(dev);
    }
    mvCoarse.destroy(dev);
    for (uint32_t i = 0; i < PYRAMID_LEVELS - 1; i++)
        mvRefined[i].destroy(dev);
    mvFiltered.destroy(dev);
    mvBackward.destroy(dev);
    confidence.destroy(dev);
    warpedForward.destroy(dev);
    warpedBackward.destroy(dev);
}

void Engine::destroy() {
    VkDevice dev = device.device;
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    destroyResources();
    lumaConvertPipeline.destroy(dev);
    pyramidDownsamplePipeline.destroy(dev);
    blockMatchCoarsePipeline.destroy(dev);
    refineLevelPipeline.destroy(dev);
    flowFilterPipeline.destroy(dev);
    occlusionPipeline.destroy(dev);
    warpPipeline.destroy(dev);
    blendPipeline.destroy(dev);
    descriptorPool.destroy(dev);
    commands.destroy(dev);
    samplers.destroy(dev);
    device.destroy();
}

}
