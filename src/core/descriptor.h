#pragma once

#include <volk.h>
#include <cstdint>

namespace seifg {

struct DescriptorPool {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult lastResult = VK_SUCCESS;

    bool init(VkDevice device);
    bool allocate(VkDevice device, VkDescriptorSetLayout layout, VkDescriptorSet* set);
    void updateStorageImage(VkDevice device, VkDescriptorSet set, uint32_t binding,
                            VkImageView view, VkImageLayout layout);
    void updateCombinedImageSampler(VkDevice device, VkDescriptorSet set, uint32_t binding,
                                     VkImageView view, VkSampler sampler, VkImageLayout layout);
    void destroy(VkDevice device);
};

}
