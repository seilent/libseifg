#include "descriptor.h"

namespace seifg {

bool DescriptorPool::init(VkDevice device) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96},
        {VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM, 24},
    };

    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 64;
    ci.poolSizeCount = 3;
    ci.pPoolSizes = sizes;

    lastResult = vkCreateDescriptorPool(device, &ci, nullptr, &pool);
    return lastResult == VK_SUCCESS;
}

bool DescriptorPool::allocate(VkDevice device, VkDescriptorSetLayout layout, VkDescriptorSet* set) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;

    lastResult = vkAllocateDescriptorSets(device, &ai, set);
    return lastResult == VK_SUCCESS;
}

void DescriptorPool::updateStorageImage(VkDevice device, VkDescriptorSet set, uint32_t binding,
                                         VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView = view;
    imgInfo.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void DescriptorPool::updateCombinedImageSampler(VkDevice device, VkDescriptorSet set, uint32_t binding,
                                                 VkImageView view, VkSampler sampler, VkImageLayout layout) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = sampler;
    imgInfo.imageView = view;
    imgInfo.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void DescriptorPool::updateBlockMatchImage(VkDevice device, VkDescriptorSet set, uint32_t binding,
                                           VkImageView view, VkSampler sampler, VkImageLayout layout) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = sampler;
    imgInfo.imageView = view;
    imgInfo.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void DescriptorPool::destroy(VkDevice device) {
    if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
}

}
