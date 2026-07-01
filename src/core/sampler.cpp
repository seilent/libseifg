#include "sampler.h"

namespace seifg {

bool Samplers::init(VkDevice device, bool imageProcessing, bool filterCubic) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    lastResult = vkCreateSampler(device, &ci, nullptr, &bilinear);
    if (lastResult != VK_SUCCESS) return false;

    ci.magFilter = VK_FILTER_NEAREST;
    ci.minFilter = VK_FILTER_NEAREST;

    lastResult = vkCreateSampler(device, &ci, nullptr, &nearest);
    if (lastResult != VK_SUCCESS) return false;

    if (imageProcessing) {
        ci.flags = VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM;
        ci.unnormalizedCoordinates = VK_TRUE;
        lastResult = vkCreateSampler(device, &ci, nullptr, &unnormalized);
        if (lastResult != VK_SUCCESS) return false;
    }

    if (filterCubic) {
        VkSamplerCreateInfo cubicCI{};
        cubicCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        cubicCI.magFilter = VK_FILTER_CUBIC_EXT;
        cubicCI.minFilter = VK_FILTER_CUBIC_EXT;
        cubicCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        cubicCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        cubicCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        lastResult = vkCreateSampler(device, &cubicCI, nullptr, &cubic);
        if (lastResult != VK_SUCCESS) {
            cubic = VK_NULL_HANDLE;
            lastResult = VK_SUCCESS;
        }
    }

    return true;
}

void Samplers::destroy(VkDevice device) {
    if (bilinear) { vkDestroySampler(device, bilinear, nullptr); bilinear = VK_NULL_HANDLE; }
    if (nearest) { vkDestroySampler(device, nearest, nullptr); nearest = VK_NULL_HANDLE; }
    if (unnormalized) { vkDestroySampler(device, unnormalized, nullptr); unnormalized = VK_NULL_HANDLE; }
    if (cubic) { vkDestroySampler(device, cubic, nullptr); cubic = VK_NULL_HANDLE; }
}

}
