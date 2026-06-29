#pragma once

#include "seifg.h"

namespace LSFG_3_1P {

inline void initialize(uint64_t deviceUUID,
    bool isHdr, float flowScale, uint64_t generationCount,
    const std::function<std::vector<uint8_t>(const std::string&)>& loader) {
    seifg::initialize(deviceUUID, isHdr, flowScale, generationCount, loader);
}

#ifdef __ANDROID__
inline int32_t createContextFromAHB(
    AHardwareBuffer* in0, AHardwareBuffer* in1,
    const std::vector<AHardwareBuffer*>& outN,
    VkExtent2D extent, VkFormat format) {
    return seifg::createContextFromAHB(in0, in1, outN, extent, format);
}
#endif

inline int32_t createContext(int in0, int in1, const std::vector<int>& outN,
    VkExtent2D extent, VkFormat format) {
    (void)in0; (void)in1; (void)outN; (void)extent; (void)format;
    return -1;
}

inline void presentContext(int32_t id, int inSem, const std::vector<int>& outSem) {
    seifg::presentContext(id, inSem, outSem);
}

inline void deleteContext(int32_t id) {
    seifg::deleteContext(id);
}

inline void finalize() {
    seifg::finalize();
}

#ifdef __ANDROID__
inline void waitIdle() {
    seifg::waitIdle();
}
#endif

}
