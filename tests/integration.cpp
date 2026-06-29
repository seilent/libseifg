#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#define VK_NO_PROTOTYPES
#include "seifg.h"
#include <android/hardware_buffer.h>
#include <volk.h>
#include <cstdio>
#include <cstdint>

static AHardwareBuffer* allocAHB(uint32_t w, uint32_t h) {
    AHardwareBuffer_Desc desc{};
    desc.width = w; desc.height = h; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
               | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
               | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0) return nullptr;
    return ahb;
}

static void fillAHB(AHardwareBuffer* ahb, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    AHardwareBuffer_Desc desc; AHardwareBuffer_describe(ahb, &desc);
    auto* pixels = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < desc.height; y++) {
        auto* row = pixels + y * desc.stride * 4;
        for (uint32_t x = 0; x < desc.width; x++) {
            row[x*4+0] = r; row[x*4+1] = g; row[x*4+2] = b; row[x*4+3] = a;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

int main() {
    constexpr uint32_t W = 128, H = 128;
    bool pass = true;

    AHardwareBuffer* in0 = allocAHB(W, H);
    AHardwareBuffer* in1 = allocAHB(W, H);
    AHardwareBuffer* out = allocAHB(W, H);
    if (!in0 || !in1 || !out) { printf("FAIL: AHB allocation\n"); return 1; }

    fillAHB(in0, 255, 0, 0, 255);
    fillAHB(in1, 0, 0, 255, 255);
    fillAHB(out, 0, 0, 0, 0);

    seifg::initialize(0, false, 0.5f, 1, {});

    int32_t ctxId = seifg::createContextFromAHB(in0, in1, {out}, {W, H}, VK_FORMAT_R8G8B8A8_UNORM);
    if (ctxId < 0) {
        printf("FAIL: createContextFromAHB returned %d\n", ctxId);
        pass = false;
    } else {
        printf("PASS: createContext id=%d\n", ctxId);
        seifg::presentContext(ctxId, -1, {});
        seifg::waitIdle();
    }

    void* ptr = nullptr;
    AHardwareBuffer_lock(out, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    AHardwareBuffer_Desc odesc; AHardwareBuffer_describe(out, &odesc);
    auto* pixels = static_cast<uint8_t*>(ptr);

    auto* center = pixels + 64 * odesc.stride * 4 + 64 * 4;
    uint8_t cr = center[0], cg = center[1], cb = center[2], ca = center[3];
    printf("center pixel: R=%u G=%u B=%u A=%u\n", cr, cg, cb, ca);

    uint64_t avgR = 0, avgG = 0, avgB = 0;
    uint32_t samples = 0;
    for (uint32_t y = 0; y < H; y += 8) {
        auto* row = pixels + y * odesc.stride * 4;
        for (uint32_t x = 0; x < W; x += 8) {
            avgR += row[x*4+0]; avgG += row[x*4+1]; avgB += row[x*4+2];
            samples++;
        }
    }
    avgR /= samples; avgG /= samples; avgB /= samples;
    printf("average RGB: %lu %lu %lu (samples=%u)\n", avgR, avgG, avgB, samples);
    AHardwareBuffer_unlock(out, nullptr);

    if (cr == 0 && cg == 0 && cb == 0) { printf("FAIL: output is all zeros\n"); pass = false; }
    if (cr == 255 && cg == 0 && cb == 0) { printf("FAIL: output identical to in0\n"); pass = false; }
    if (cr == 0 && cg == 0 && cb == 255) { printf("FAIL: output identical to in1\n"); pass = false; }

    if (ctxId >= 0) seifg::deleteContext(ctxId);
    seifg::finalize();
    AHardwareBuffer_release(in0);
    AHardwareBuffer_release(in1);
    AHardwareBuffer_release(out);

    printf(pass ? "PASS: integration test\n" : "FAIL: integration test\n");
    return pass ? 0 : 1;
}
