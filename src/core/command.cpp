#include "command.h"

namespace seifg {

bool CommandRing::init(VkDevice device, uint32_t queueFamily) {
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = queueFamily;

    lastResult = vkCreateCommandPool(device, &poolCI, nullptr, &pool);
    if (lastResult != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = RING_SIZE;

    lastResult = vkAllocateCommandBuffers(device, &allocInfo, buffers);
    if (lastResult != VK_SUCCESS) return false;

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        lastResult = vkCreateFence(device, &fenceCI, nullptr, &fences[i]);
        if (lastResult != VK_SUCCESS) return false;
    }

    return true;
}

VkCommandBuffer CommandRing::acquire(VkDevice device) {
    vkWaitForFences(device, 1, &fences[index], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fences[index]);
    vkResetCommandBuffer(buffers[index], 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(buffers[index], &beginInfo);
    return buffers[index];
}

bool CommandRing::submit(VkDevice device, VkQueue queue) {
    lastResult = vkEndCommandBuffer(buffers[index]);
    if (lastResult != VK_SUCCESS) return false;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &buffers[index];

    lastResult = vkQueueSubmit(queue, 1, &submitInfo, fences[index]);
    index = (index + 1) % RING_SIZE;
    return lastResult == VK_SUCCESS;
}

void CommandRing::destroy(VkDevice device) {
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        if (fences[i]) { vkDestroyFence(device, fences[i], nullptr); fences[i] = VK_NULL_HANDLE; }
    }
    if (pool) { vkDestroyCommandPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
}

}
