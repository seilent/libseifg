#pragma once

#include "core/device.h"
#include "core/image.h"
#include "core/pipeline.h"
#include "core/descriptor.h"
#include "core/command.h"
#include "core/sampler.h"

namespace seifg {

static constexpr uint32_t PYRAMID_LEVELS = 5;
static constexpr uint32_t MAX_OUTPUTS = 3;

struct Engine {
    Device device;
    Samplers samplers;
    CommandRing commands;
    DescriptorPool descriptorPool;

    Pipeline lumaConvertPipeline;
    Pipeline pyramidDownsamplePipeline;
    Pipeline blockMatchCoarsePipeline;
    Pipeline refineLevelPipeline;
    Pipeline flowFilterPipeline;
    Pipeline occlusionPipeline;
    Pipeline warpPipeline;
    Pipeline blendPipeline;

    Image lumaPrev[PYRAMID_LEVELS];
    Image lumaCurr[PYRAMID_LEVELS];
    Image mvCoarse;
    Image mvRefined[PYRAMID_LEVELS - 1];
    Image mvFiltered;
    Image mvBackward;
    Image confidence;
    Image warpedForward;
    Image warpedBackward;

    VkDescriptorSet dsLumaConvert[2]{};
    VkDescriptorSet dsPyramid[8]{};
    VkDescriptorSet dsBlockMatch{};
    VkDescriptorSet dsRefine[4]{};
    VkDescriptorSet dsFlowFilter{};
    VkDescriptorSet dsOcclusion{};
    VkDescriptorSet dsWarp[2]{};
    VkDescriptorSet dsBlend[MAX_OUTPUTS]{};

    uint32_t width = 0;
    uint32_t height = 0;
    float flowScale = 0.5f;
    int quality = 2;
    bool useQcom = false;

    bool init(uint64_t deviceUUID, float flowScale);
    void destroy();
    bool createResources(uint32_t w, uint32_t h);
    void destroyResources();
    bool recordAndSubmit(Image& in0, Image& in1, Image* outs, uint32_t numOut);
};

}
