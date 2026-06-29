#pragma once

#include <volk.h>

namespace seifg {

struct Samplers {
    VkSampler bilinear = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    VkResult lastResult = VK_SUCCESS;

    bool init(VkDevice device);
    void destroy(VkDevice device);
};

}
