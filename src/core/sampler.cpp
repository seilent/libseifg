#include "sampler.h"

namespace seifg {

bool Samplers::init(VkDevice device) {
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
    return lastResult == VK_SUCCESS;
}

void Samplers::destroy(VkDevice device) {
    if (bilinear) { vkDestroySampler(device, bilinear, nullptr); bilinear = VK_NULL_HANDLE; }
    if (nearest) { vkDestroySampler(device, nearest, nullptr); nearest = VK_NULL_HANDLE; }
}

}
