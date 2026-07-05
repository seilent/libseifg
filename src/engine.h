#pragma once

#include "core/device.h"
#include "core/image.h"
#include "core/pipeline.h"
#include "core/descriptor.h"
#include "core/command.h"
#include "core/sampler.h"

namespace seifg {

static constexpr uint32_t PYRAMID_LEVELS = 6;
static constexpr uint32_t MAX_OUTPUTS = 3;

struct Engine {
    Device device;
    Samplers samplers;
    CommandRing commands;
    DescriptorPool descriptorPool;

    Pipeline lumaConvertPipeline;
    Pipeline pyramidDownsamplePipeline;
    Pipeline gradProductPipeline;
    Pipeline blockMatchCoarsePipeline;
    Pipeline refineLevelPipeline;
    Pipeline flowFilterPipeline;
    Pipeline occlusionPipeline;
    Pipeline warpPipeline;
    Pipeline blendPipeline;

    Image lumaPrev[PYRAMID_LEVELS];
    Image lumaCurr[PYRAMID_LEVELS];
    Image gradImg[PYRAMID_LEVELS];
    Image mvCoarse;
    Image mvRefined[PYRAMID_LEVELS - 1];
    Image mvFiltered;
    Image mvBackward;
    Image confidence;
    Image warpedForward;
    Image warpedBackward;

    VkDescriptorSet dsLumaConvert[2]{};
    VkDescriptorSet dsPyramid[2 * (PYRAMID_LEVELS - 1)]{};
    VkDescriptorSet dsGradProduct[PYRAMID_LEVELS]{};
    VkDescriptorSet dsBlockMatch{};
    VkDescriptorSet dsRefine[PYRAMID_LEVELS - 1]{};
    VkDescriptorSet dsFlowFilter{};
    VkDescriptorSet dsOcclusion{};
    VkDescriptorSet dsWarp[2]{};
    VkDescriptorSet dsBlend[MAX_OUTPUTS]{};

    uint32_t width = 0;
    uint32_t height = 0;
    int quality = 2;
    bool useQcom = false;
    bool useCubicWarp = false;

    bool init(uint64_t deviceUUID, uint32_t quality);
    void destroy();
    bool createResources(uint32_t w, uint32_t h, VkFormat frameFormat);
    void destroyResources();
    bool recordAndSubmit(Image& in0, Image& in1, Image* outs, uint32_t numOut);
    bool recordAndSubmit(Image& in0, Image& in1, Image* outs, uint32_t numOut,
                         VkSemaphore timelineSem, uint64_t waitValue, uint64_t signalValue);

#if defined(__linux__) && !defined(__ANDROID__)
    VkSemaphore importedTimeline = VK_NULL_HANDLE;
    uint64_t timelineValue = 0;
    bool importTimelineSemaphore(int fd);
    void destroyTimelineSemaphore();
#endif
};

}
