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
    Pipeline sgsrPipeline;

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
    Image interpResult[MAX_OUTPUTS];

    VkDescriptorSet dsLumaConvert[2]{};
    VkDescriptorSet dsPyramid[2 * (PYRAMID_LEVELS - 1)]{};
    VkDescriptorSet dsGradProduct[PYRAMID_LEVELS]{};
    VkDescriptorSet dsBlockMatch{};
    VkDescriptorSet dsRefine[PYRAMID_LEVELS - 1]{};
    VkDescriptorSet dsFlowFilter{};
    VkDescriptorSet dsOcclusion{};
    VkDescriptorSet dsWarp[2]{};
    VkDescriptorSet dsBlend[MAX_OUTPUTS]{};
    VkDescriptorSet dsSgsr[MAX_OUTPUTS]{};

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    float sgsrSharpness = 0.5f;
    int quality = 2;
    uint32_t upscaleOnlyLevels = 1;
    bool useQcom = false;
    bool useCubicWarp = false;

    bool upscaling() const { return outWidth > width && outHeight > height; }

    bool init(uint64_t deviceUUID, uint32_t quality);
#if defined(__linux__) && !defined(__ANDROID__)
    bool initWithPicker(const std::function<bool(const std::string& name, uint32_t vendorID, uint32_t deviceID)>& picker, uint32_t quality);
#endif
    void destroy();
    bool createResources(uint32_t w, uint32_t h, VkFormat frameFormat, uint32_t outW = 0, uint32_t outH = 0);
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
