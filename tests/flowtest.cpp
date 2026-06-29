#include "seifg.h"
#include <android/hardware_buffer.h>
#include <volk.h>
#include <cstdio>
#include <cmath>
#include <vector>

static AHardwareBuffer* allocAhb(uint32_t w, uint32_t h) {
    AHardwareBuffer_Desc d{};
    d.width = w; d.height = h; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
            | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* a = nullptr;
    if (AHardwareBuffer_allocate(&d, &a) != 0) return nullptr;
    return a;
}

static uint8_t pat(int x, int y) {
    float v = 0.5f + 0.25f * sinf(x * 0.20f) + 0.25f * sinf(y * 0.13f + x * 0.05f);
    int iv = (int)(v * 255.0f);
    return (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

static void fillShifted(AHardwareBuffer* ahb, int shiftX) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            uint8_t v = pat((int)x - shiftX, (int)y);
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void compareToRef(AHardwareBuffer* ahb, int refShift, uint32_t W, uint32_t H) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    double errInterp = 0.0, errLerp = 0.0;
    long n = 0;
    int m = 24;
    for (uint32_t y = m; y < H - m; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = m; x < W - m; x++) {
            int outv = row[x*4+0];
            int ref = pat((int)x - refShift, (int)y);
            int f0 = pat((int)x - 0, (int)y);
            int f1 = pat((int)x - 2*refShift, (int)y);
            int lerp = (f0 + f1) / 2;
            errInterp += fabs((double)outv - ref);
            errLerp += fabs((double)lerp - ref);
            n++;
        }
    }
    printf("mean|interp-ref|=%.1f   mean|lerp-ref|=%.1f   (lower interp = flow working)\n",
           errInterp / n, errLerp / n);
    AHardwareBuffer_unlock(ahb, nullptr);
}

int main() {
    const uint32_t W = 1280, H = 720;
    const int SHIFT = 20;
    AHardwareBuffer* in0 = allocAhb(W, H);
    AHardwareBuffer* in1 = allocAhb(W, H);
    AHardwareBuffer* out = allocAhb(W, H);
    if (!in0 || !in1 || !out) { printf("FAIL: ahb alloc\n"); return 1; }

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    printf("textured pattern, frame1 shifted +%dpx, ideal interp shift=+%dpx\n", 2*SHIFT, SHIFT);

    seifg::initialize(0, false, 0.5f, 1, {});
    int32_t ctx = seifg::createContextFromAHB(in0, in1, {out}, VkExtent2D{W, H},
                                              VK_FORMAT_R8G8B8A8_UNORM);
    if (ctx < 0) { printf("FAIL: createContext=%d\n", ctx); return 1; }
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();

    compareToRef(out, SHIFT, W, H);

    fillShifted(in0, 0);
    fillShifted(in1, 0);
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    compareToRef(out, 0, W, H);
    printf("(static test: identical frames -> interp should match exactly, error near 0)\n");

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    const int N = 120;
    struct timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) {
        seifg::presentContext(ctx, -1, {});
        seifg::waitIdle();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / N;
    printf("seifg presentContext+waitIdle: %.2f ms/frame at %ux%u\n", ms, W, H);

    seifg::deleteContext(ctx);
    seifg::finalize();
    AHardwareBuffer_release(in0);
    AHardwareBuffer_release(in1);
    AHardwareBuffer_release(out);
    printf("DONE\n");
    return 0;
}
