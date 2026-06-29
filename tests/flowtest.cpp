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

static void fillScene(AHardwareBuffer* ahb, int boxX, int boxW) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            uint8_t bg = pat((int)x, (int)y);
            bool inBox = ((int)x >= boxX && (int)x < boxX + boxW);
            uint8_t v = inBox ? (uint8_t)(255 - bg) : bg;
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void measureScene(AHardwareBuffer* ahb, int boxX, int boxW, int edge,
                         uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    const int m = 32;
    double allE = 0.0; long allN = 0;
    double edgeE = 0.0; long edgeN = 0; double edgeMax = 0.0;
    const int le = boxX, re = boxX + boxW;
    for (uint32_t y = m; y < H - m; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = m; x < W - m; x++) {
            uint8_t bg = pat((int)x, (int)y);
            bool inBox = ((int)x >= boxX && (int)x < boxX + boxW);
            int expected = inBox ? (255 - bg) : bg;
            double e = fabs((double)row[x*4] - expected);
            allE += e; allN++;
            int dl = (int)x - le; if (dl < 0) dl = -dl;
            int dr = (int)x - re; if (dr < 0) dr = -dr;
            if (dl <= edge || dr <= edge) { edgeE += e; edgeN++; if (e > edgeMax) edgeMax = e; }
        }
    }
    printf("%s overall=%.1f  edgeBand=%.1f  edgeMax=%.0f\n",
           label, allE / allN, edgeN ? edgeE / edgeN : 0.0, edgeMax);
    AHardwareBuffer_unlock(ahb, nullptr);
}

int main(int argc, char** argv) {
    const uint32_t W = 1280, H = 720;
    const int SHIFT = (argc > 1) ? atoi(argv[1]) : 12;
    const int M = (argc > 2) ? atoi(argv[2]) : 2;
    const float flowScale = (argc > 3) ? (float)atof(argv[3]) : 0.5f;
    const int N = M - 1;
    if (M < 2 || N > 3) { printf("FAIL: multiplier must be 2..4\n"); return 1; }

    AHardwareBuffer* in0 = allocAhb(W, H);
    AHardwareBuffer* in1 = allocAhb(W, H);
    std::vector<AHardwareBuffer*> outs(N);
    for (int i = 0; i < N; i++) outs[i] = allocAhb(W, H);
    if (!in0 || !in1) { printf("FAIL: ahb alloc\n"); return 1; }
    for (auto* o : outs) if (!o) { printf("FAIL: out alloc\n"); return 1; }

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    printf("=== multiplier %dx, flowScale %.2f, pan +%dpx ===\n", M, flowScale, 2 * SHIFT);

    seifg::initialize(0, false, flowScale, (uint64_t)M, {});
    int32_t ctx = seifg::createContextFromAHB(in0, in1, outs, VkExtent2D{W, H},
                                              VK_FORMAT_R8G8B8A8_UNORM);
    if (ctx < 0) { printf("FAIL: createContext=%d\n", ctx); return 1; }
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    for (int i = 0; i < N; i++) {
        int ideal = (int)lround(2.0 * SHIFT * (i + 1) / (double)M);
        char label[64];
        snprintf(label, sizeof(label), "t=%d/%d ideal+%dpx", i + 1, M, ideal);
        measure(outs[i], ideal, W, H, label);
    }

    fillShifted(in0, 0);
    fillShifted(in1, 0);
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    measure(outs[0], 0, W, H, "static");

    const int BOXW = 200;
    const int BX0 = (int)W / 2 - BOXW / 2 - SHIFT;
    const int BX1 = BX0 + 2 * SHIFT;
    fillScene(in0, BX0, BOXW);
    fillScene(in1, BX1, BOXW);
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    printf("-- occlusion: box %dpx wide moving +%dpx over static bg --\n", BOXW, 2 * SHIFT);
    for (int i = 0; i < N; i++) {
        int boxXt = (int)lround(BX0 + (double)(BX1 - BX0) * (i + 1) / (double)M);
        char label[80];
        snprintf(label, sizeof(label), "  t=%d/%d", i + 1, M);
        measureScene(outs[i], boxXt, BOXW, 12, W, H, label);
    }

    fillShifted(in0, 0);
    fillShifted(in1, 2 * SHIFT);
    const int FRAMES = 120;
    struct timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < FRAMES; i++) { seifg::presentContext(ctx, -1, {}); seifg::waitIdle(); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / FRAMES;
    printf("seifg %dx: %.2f ms/frame at %ux%u (%d interp%s)\n", M, ms, W, H, N, N > 1 ? "s" : "");

    seifg::deleteContext(ctx);
    seifg::finalize();
    AHardwareBuffer_release(in0);
    AHardwareBuffer_release(in1);
    for (auto* o : outs) AHardwareBuffer_release(o);
    printf("DONE\n");
    return 0;
}
