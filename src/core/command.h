#pragma once

#include <volk.h>
#include <cstdint>

namespace seifg {

struct CommandRing {
    static constexpr uint32_t RING_SIZE = 4;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffers[RING_SIZE]{};
    VkFence fences[RING_SIZE]{};
    uint32_t index = 0;
    VkResult lastResult = VK_SUCCESS;

    bool init(VkDevice device, uint32_t queueFamily);
    VkCommandBuffer acquire(VkDevice device);
    bool submit(VkDevice device, VkQueue queue);
    bool submit(VkDevice device, VkQueue queue,
                VkSemaphore timelineSem, uint64_t waitValue, uint64_t signalValue);
    void destroy(VkDevice device);
};

}
