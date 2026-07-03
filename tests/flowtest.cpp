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

static float patf(float x, float y) {
    return 0.5f * vnoise(x * 0.05f, y * 0.05f)
         + 0.3f * vnoise(x * 0.12f, y * 0.12f)
         + 0.2f * vnoise(x * 0.25f, y * 0.25f);
}

static uint8_t toByte(float v) {
    int iv = (int)(v * 255.0f);
    return (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

static uint8_t pat(int x, int y) {
    return toByte(patf((float)x, (float)y));
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
    double bvar = 0.0;
    for (double e : blockErr) bvar += (e - bmean) * (e - bmean);
    double bstd = sqrt(bvar / blockErr.size());

    double lapEx = 0.0; long lapN = 0;
    for (uint32_t y = m + 1; y + m + 1 < H; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        uint8_t* up = base + (size_t)(y - 1) * d.stride * 4;
        uint8_t* dn = base + (size_t)(y + 1) * d.stride * 4;
        for (uint32_t x = m + 1; x + m + 1 < W; x++) {
            double lapO = fabs(4.0 * row[x*4] - row[(x-1)*4] - row[(x+1)*4] - up[x*4] - dn[x*4]);
            int rx = (int)x - refShift, ry = (int)y;
            double lapR = fabs(4.0 * pat(rx, ry) - pat(rx-1, ry) - pat(rx+1, ry) - pat(rx, ry-1) - pat(rx, ry+1));
            double ex = lapO - lapR; if (ex > 0.0) { lapEx += ex; }
            lapN++;
        }
    }
    double discont = lapN ? lapEx / lapN : 0.0;
    printf("%s: meanErr=%.1f  blockStd=%.1f  discontinuity=%.2f  (discontinuity=spurious edges)\n",
           label, mean, bstd, discont);
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

static const int GRID_SP = 24;
static const int GRID_TH = 3;

static uint8_t gridPixel(int srcX, int y) {
    int gx = ((srcX % GRID_SP) + GRID_SP) % GRID_SP;
    int gy = ((y % GRID_SP) + GRID_SP) % GRID_SP;
    if (gx < GRID_TH || gy < GRID_TH) return 0;
    return pat(srcX, y);
}

static void fillGrid(AHardwareBuffer* ahb, int shiftX) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            uint8_t v = gridPixel((int)x - shiftX, (int)y);
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void measureGrid(AHardwareBuffer* ahb, int refShift, uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    const int B = 16;
    const int m = 32;
    double globalErr = 0.0; long gN = 0; double gMax = 0.0;
    std::vector<double> blockErr;
    for (uint32_t by = m; by + B < H - m; by += B) {
        for (uint32_t bx = m; bx + B < W - m; bx += B) {
            double be = 0.0; int bn = 0;
            for (int yy = 0; yy < B; yy++) {
                uint8_t* row = base + (size_t)(by + yy) * d.stride * 4;
                for (int xx = 0; xx < B; xx++) {
                    int outv = row[(bx + xx) * 4];
                    int ref = gridPixel((int)(bx + xx) - refShift, (int)(by + yy));
                    double e = fabs((double)outv - ref);
                    be += e; bn++; globalErr += e; gN++;
                    if (e > gMax) gMax = e;
                }
            }
            blockErr.push_back(be / bn);
        }
    }
    double mean = globalErr / gN;
    double bmean = 0.0; for (double e : blockErr) bmean += e; bmean /= blockErr.size();
    double bvar = 0.0;
    for (double e : blockErr) bvar += (e - bmean) * (e - bmean);
    double bstd = sqrt(bvar / blockErr.size());
    printf("%s meanErr=%.1f  blockStdErr=%.1f  maxErr=%.0f\n", label, mean, bstd, gMax);
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void affineSrc(float px, float py, float s, float a, float tx, float ty,
                      float cx, float cy, float& sx, float& sy) {
    float dx = px - tx - cx;
    float dy = py - ty - cy;
    float ca = cosf(a), sn = sinf(a);
    sx = cx + (ca * dx + sn * dy) / s;
    sy = cy + (-sn * dx + ca * dy) / s;
}

static void fillAffine(AHardwareBuffer* ahb, float s, float a, float tx, float ty) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    float cx = d.width * 0.5f, cy = d.height * 0.5f;
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            float sx, sy;
            affineSrc((float)x, (float)y, s, a, tx, ty, cx, cy, sx, sy);
            uint8_t v = toByte(patf(sx, sy));
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void measureAffine(AHardwareBuffer* ahb, float s, float a, float tx, float ty,
                          uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    float cx = W * 0.5f, cy = H * 0.5f;
    const int B = 16, m = 32;
    double gErr = 0.0; long gN = 0; double gMax = 0.0;
    std::vector<double> blockErr;
    for (uint32_t by = m; by + B < H - m; by += B) {
        for (uint32_t bx = m; bx + B < W - m; bx += B) {
            double be = 0.0; int bn = 0;
            for (int yy = 0; yy < B; yy++) {
                uint8_t* row = base + (size_t)(by + yy) * d.stride * 4;
                for (int xx = 0; xx < B; xx++) {
                    float sx, sy;
                    affineSrc((float)(bx + xx), (float)(by + yy), s, a, tx, ty, cx, cy, sx, sy);
                    int ref = toByte(patf(sx, sy));
                    double e = fabs((double)row[(bx + xx) * 4] - ref);
                    be += e; bn++; gErr += e; gN++; if (e > gMax) gMax = e;
                }
            }
            blockErr.push_back(be / bn);
        }
    }
    double mean = gErr / gN, bmean = 0.0;
    for (double e : blockErr) bmean += e; bmean /= blockErr.size();
    double bvar = 0.0; for (double e : blockErr) bvar += (e - bmean) * (e - bmean);
    printf("%s meanErr=%.1f  blockStdErr=%.1f  maxErr=%.0f\n", label, mean, sqrt(bvar / blockErr.size()), gMax);
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void fillFade(AHardwareBuffer* ahb, float bright) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            uint8_t v = toByte(patf((float)x, (float)y) * bright);
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void measureFade(AHardwareBuffer* ahb, float bright, uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    const int m = 32;
    double e = 0.0; long n = 0; double mx = 0.0;
    for (uint32_t y = m; y < H - m; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = m; x < W - m; x++) {
            int ref = toByte(patf((float)x, (float)y) * bright);
            double er = fabs((double)row[x*4] - ref);
            e += er; n++; if (er > mx) mx = er;
        }
    }
    printf("%s meanErr=%.1f  maxErr=%.0f\n", label, e / n, mx);
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void fillUiBox(AHardwareBuffer* ahb, int boxX, int boxW, int boxH) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    int cy = (int)d.height / 2;
    int y0 = cy - boxH / 2, y1 = cy + boxH / 2;
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            uint8_t bg = pat((int)x, (int)y);
            bool inBox = ((int)x >= boxX && (int)x < boxX + boxW && (int)y >= y0 && (int)y < y1);
            uint8_t v = inBox ? 250 : bg;
            row[x*4+0] = v; row[x*4+1] = v; row[x*4+2] = v; row[x*4+3] = 255;
        }
    }
    AHardwareBuffer_unlock(ahb, nullptr);
}

static void measureUiSnap(AHardwareBuffer* ahb, int snapX, int vacatedX, int boxW, int boxH,
                          uint32_t W, uint32_t H, const char* label) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(ahb, &d);
    void* ptr = nullptr;
    AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    auto* base = static_cast<uint8_t*>(ptr);
    int cy = (int)H / 2, y0 = cy - boxH / 2, y1 = cy + boxH / 2;
    double snapErr = 0.0; long snapN = 0;
    double vacGhost = 0.0; long vacN = 0; double vacMax = 0.0;
    for (int y = y0; y < y1; y++) {
        uint8_t* row = base + (size_t)y * d.stride * 4;
        for (int x = 0; x < (int)W; x++) {
            bool inSnap = (x >= snapX && x < snapX + boxW);
            bool inVac = (x >= vacatedX && x < vacatedX + boxW);
            if (!inSnap && !inVac) continue;
            int out = row[x * 4];
            int ideal = inSnap ? 250 : (int)pat(x, y);
            snapErr += fabs((double)out - ideal); snapN++;
            if (inVac) {
                double g = fabs((double)out - (double)pat(x, y));
                vacGhost += g; vacN++; if (g > vacMax) vacMax = g;
            }
        }
    }
    printf("%s snapErr=%.1f  vacatedGhost=%.1f  ghostMax=%.0f\n",
           label, snapN ? snapErr / snapN : 0.0, vacN ? vacGhost / vacN : 0.0, vacMax);
    AHardwareBuffer_unlock(ahb, nullptr);
}

int main(int argc, char** argv) {    const uint32_t W = 1280, H = 720;
    const int SHIFT = (argc > 1) ? atoi(argv[1]) : 12;
    const int M = (argc > 2) ? atoi(argv[2]) : 2;
    const uint32_t quality = (argc > 3) ? (uint32_t)atoi(argv[3]) : 2;
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
    printf("=== multiplier %dx, quality %u, pan +%dpx ===\n", M, quality, 2 * SHIFT);

    seifg::initialize(0, false, quality, (uint64_t)M, {});
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

    {
        const int UBW = 150, UBH = 150, JUMP = 220;
        const int UAX = (int)W / 2 - JUMP / 2 - UBW / 2;
        const int UBX = UAX + JUMP;
        fillUiBox(in0, UAX, UBW, UBH);
        fillUiBox(in1, UBX, UBW, UBH);
        seifg::presentContext(ctx, -1, {});
        seifg::waitIdle();
        printf("-- ui highlight teleport A->B (%dpx box, +%dpx jump, static bg) --\n", UBW, JUMP);
        for (int i = 0; i < N; i++) {
            double f = (double)(i + 1) / (double)M;
            int snapX = (f < 0.5) ? UAX : UBX;
            int vacX = (f < 0.5) ? UBX : UAX;
            char label[80];
            snprintf(label, sizeof(label), "  t=%d/%d", i + 1, M);
            measureUiSnap(outs[i], snapX, vacX, UBW, UBH, W, H, label);
        }
    }

    fillGrid(in0, 0);
    fillGrid(in1, 2 * SHIFT);
    seifg::presentContext(ctx, -1, {});
    seifg::waitIdle();
    printf("-- grid/fence pan +%dpx (%dpx spacing, %dpx lines over noise) --\n", 2 * SHIFT, GRID_SP, GRID_TH);
    for (int i = 0; i < N; i++) {
        int gShift = (int)lround(2.0 * SHIFT * (i + 1) / (double)M);
        char label[80];
        snprintf(label, sizeof(label), "  t=%d/%d +%dpx", i + 1, M, gShift);
        measureGrid(outs[i], gShift, W, H, label);
    }

    {
        float TX = 2.0f * SHIFT, TY = (float)SHIFT;
        fillAffine(in0, 1.0f, 0.0f, 0.0f, 0.0f);
        fillAffine(in1, 1.0f, 0.0f, TX, TY);
        seifg::presentContext(ctx, -1, {}); seifg::waitIdle();
        printf("-- diagonal pan (+%.0f,+%.0f px) --\n", TX, TY);
        for (int i = 0; i < N; i++) {
            float ti = (float)(i + 1) / (float)M;
            char l[48]; snprintf(l, sizeof(l), "  t=%d/%d", i + 1, M);
            measureAffine(outs[i], 1.0f, 0.0f, TX * ti, TY * ti, W, H, l);
        }

        float S = 1.0f + 0.01f * SHIFT;
        fillAffine(in0, 1.0f, 0.0f, 0.0f, 0.0f);
        fillAffine(in1, S, 0.0f, 0.0f, 0.0f);
        seifg::presentContext(ctx, -1, {}); seifg::waitIdle();
        printf("-- zoom (scale %.3f) --\n", S);
        for (int i = 0; i < N; i++) {
            float ti = (float)(i + 1) / (float)M;
            char l[48]; snprintf(l, sizeof(l), "  t=%d/%d", i + 1, M);
            measureAffine(outs[i], 1.0f + (S - 1.0f) * ti, 0.0f, 0.0f, 0.0f, W, H, l);
        }

        float ANG = 0.004f * SHIFT;
        fillAffine(in0, 1.0f, 0.0f, 0.0f, 0.0f);
        fillAffine(in1, 1.0f, ANG, 0.0f, 0.0f);
        seifg::presentContext(ctx, -1, {}); seifg::waitIdle();
        printf("-- rotation (%.1f deg) --\n", ANG * 57.2958f);
        for (int i = 0; i < N; i++) {
            float ti = (float)(i + 1) / (float)M;
            char l[48]; snprintf(l, sizeof(l), "  t=%d/%d", i + 1, M);
            measureAffine(outs[i], 1.0f, ANG * ti, 0.0f, 0.0f, W, H, l);
        }

        fillFade(in0, 1.0f);
        fillFade(in1, 0.5f);
        seifg::presentContext(ctx, -1, {}); seifg::waitIdle();
        printf("-- brightness fade (1.0 -> 0.5, no motion) --\n");
        for (int i = 0; i < N; i++) {
            float ti = (float)(i + 1) / (float)M;
            char l[48]; snprintf(l, sizeof(l), "  t=%d/%d", i + 1, M);
            measureFade(outs[i], 1.0f - 0.5f * ti, W, H, l);
        }
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
