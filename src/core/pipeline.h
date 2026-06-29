#pragma once

#include <volk.h>
#include <cstdint>

namespace seifg {

struct SeifgPushConstants {
    uint32_t width;
    uint32_t height;
    float flowScale;
    float t;
    uint32_t level;
    float threshold;
    float temperature;
    uint32_t _pad;
};

struct Pipeline {
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult lastResult = VK_SUCCESS;

    bool init(VkDevice device, const uint32_t* spirv, uint32_t spirvSize,
              const VkDescriptorType* bindingTypes, uint32_t bindingCount,
              const VkSpecializationInfo* specInfo = nullptr);
    void destroy(VkDevice device);
};

}
