#pragma once

#include <volk.h>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __ANDROID__
struct AHardwareBuffer;
#endif

namespace seifg {

void initialize(uint64_t deviceUUID,
    bool isHdr, uint32_t quality, uint64_t generationCount,
    const std::function<std::vector<uint8_t>(const std::string&)>& loader);

#ifdef __ANDROID__
int32_t createContextFromAHB(
    AHardwareBuffer* in0, AHardwareBuffer* in1,
    const std::vector<AHardwareBuffer*>& outN,
    VkExtent2D extent, VkFormat format);
#endif

void presentContext(int32_t id, int inSem, const std::vector<int>& outSem);

void deleteContext(int32_t id);

void finalize();

#ifdef __ANDROID__
void waitIdle();
#endif

}
