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

static float hashf(int x, int y) {
    int h = x * 374761393 + y * 668265263;
    h = (h ^ (h >> 13)) * 1274126177;
    return float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float vnoise(float x, float y) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = hashf(x0, y0), b = hashf(x0 + 1, y0);
    float c = hashf(x0, y0 + 1), d = hashf(x0 + 1, y0 + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

static uint8_t pat(int x, int y) {
    float v = 0.5f * vnoise(x * 0.05f, y * 0.05f)
            + 0.3f * vnoise(x * 0.12f, y * 0.12f)
            + 0.2f * vnoise(x * 0.25f, y * 0.25f);
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

static void measure(AHardwareBuffer* ahb, int refShift, uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    const int B = 16;
    const int m = 32;
    double globalErr = 0.0; long gN = 0;
    std::vector<double> blockErr;
    for (uint32_t by = m; by + B < H - m; by += B) {
        for (uint32_t bx = m; bx + B < W - m; bx += B) {
            double be = 0.0; int bn = 0;
            for (int yy = 0; yy < B; yy++) {
                uint8_t* row = base + (size_t)(by + yy) * d.stride * 4;
                for (int xx = 0; xx < B; xx++) {
                    int outv = row[(bx + xx) * 4];
                    int ref = pat((int)(bx + xx) - refShift, (int)(by + yy));
                    double e = fabs((double)outv - ref);
                    be += e; bn++;
                    globalErr += e; gN++;
                }
            }
            blockErr.push_back(be / bn);
        }
    }
    double mean = globalErr / gN;
    double bmean = 0.0; for (double e : blockErr) bmean += e; bmean /= blockErr.size();
    double bvar = 0.0, bmax = 0.0;
    for (double e : blockErr) { bvar += (e - bmean) * (e - bmean); if (e > bmax) bmax = e; }
    double bstd = sqrt(bvar / blockErr.size());
    printf("%s: meanErr=%.1f  blockStdErr=%.1f  blockMaxErr=%.1f  (blockStd=blockiness)\n",
           label, mean, bstd, bmax);
    AHardwareBuffer_unlock(ahb, nullptr);
}

int main() {
    const uint32_t W = 1280, H = 720;
    const int SHIFT = 10;
    AHardwareBuffer* in0 = allocAhb(W, H);
    AHardwareBuffer* in1 = allocAhb(W, H);
    AHardwareBuffer* out = allocAhb(W, H);
    if (!in0 || !in1 || !out) { printf("FAIL: ahb alloc\n"); return 1; }

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    printf("uniform pan +%dpx, ideal interp +%dpx\n", 2 * SHIFT, SHIFT);

    seifg::initialize(0, false, 0.5f, 1, {});
    int32_t ctx = seifg::createContextFromAHB(in0, in1, {out}, VkExtent2D{W, H},
                                              VK_FORMAT_R8G8B8A8_UNORM);
    if (ctx < 0) { printf("FAIL: createContext=%d\n", ctx); return 1; }
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    measure(out, SHIFT, W, H, "pan ");

    fillShifted(in0, 0);
    fillShifted(in1, 0);
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    measure(out, 0, W, H, "static");

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    const int N = 120;
    struct timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) { seifg::presentContext(ctx, -1, {}); seifg::waitIdle(); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / N;
    printf("seifg: %.2f ms/frame at %ux%u\n", ms, W, H);

    seifg::deleteContext(ctx);
    seifg::finalize();
    AHardwareBuffer_release(in0);
    AHardwareBuffer_release(in1);
    AHardwareBuffer_release(out);
    printf("DONE\n");
    return 0;
}
