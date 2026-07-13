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
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent = {0, 0});
#endif

int32_t createContextFromImages(
    VkImage in0, VkImage in1,
    const std::vector<VkImage>& outN,
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent = {0, 0});

void presentContext(int32_t id, int inSem, const std::vector<int>& outSem);

void deleteContext(int32_t id);

void finalize();

void setFlowDownscale(uint32_t upscaleOnlyLevels);

VkDevice getDevice();
VkPhysicalDevice getPhysicalDevice();

#ifdef __ANDROID__
void waitIdle();
#endif

#if defined(__linux__) && !defined(__ANDROID__)
int32_t createContextFromFd(int in0Fd, int in1Fd,
    const std::vector<int>& outFds,
    VkExtent2D extent, VkFormat format, VkExtent2D outExtent = {0, 0});

bool importTimelineSemaphore(int syncFd);

void presentContextTimeline(int32_t id, uint64_t waitValue, uint64_t signalValue);

bool initializeWithPicker(
    const std::function<bool(const std::string& name, uint32_t vendorID, uint32_t deviceID)>& picker,
    bool isHdr, uint32_t quality, uint64_t generationCount,
    const std::function<std::vector<uint8_t>(const std::string&)>& loader);
#endif

}
