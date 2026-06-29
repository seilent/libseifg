#include "pipeline.h"

namespace seifg {

bool Pipeline::init(VkDevice device, const uint32_t* spirv, uint32_t spirvSize,
                    const VkDescriptorType* bindingTypes, uint32_t bindingCount) {
    VkShaderModuleCreateInfo smCI{};
    smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirvSize;
    smCI.pCode = spirv;

    lastResult = vkCreateShaderModule(device, &smCI, nullptr, &shaderModule);
    if (lastResult != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding bindings[16];
    for (uint32_t i = 0; i < bindingCount && i < 16; i++) {
        bindings[i] = {};
        bindings[i].binding = i;
        bindings[i].descriptorType = bindingTypes[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = bindingCount;
    dslCI.pBindings = bindings;

    lastResult = vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &descriptorSetLayout);
    if (lastResult != VK_SUCCESS) return false;

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(SeifgPushConstants);

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &descriptorSetLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pcRange;

    lastResult = vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout);
    if (lastResult != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo cpCI{};
    cpCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module = shaderModule;
    cpCI.stage.pName = "main";
    cpCI.layout = pipelineLayout;

    lastResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &pipeline);
    return lastResult == VK_SUCCESS;
}

void Pipeline::destroy(VkDevice device) {
    if (pipeline) { vkDestroyPipeline(device, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
    if (pipelineLayout) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); pipelineLayout = VK_NULL_HANDLE; }
    if (descriptorSetLayout) { vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr); descriptorSetLayout = VK_NULL_HANDLE; }
    if (shaderModule) { vkDestroyShaderModule(device, shaderModule, nullptr); shaderModule = VK_NULL_HANDLE; }
}

}
