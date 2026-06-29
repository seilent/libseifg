#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

static const int64_t P = 16'666'667;

struct Pacer {
    int64_t scanoutNextPresentNs = 0;

    int64_t nextVsyncSlot(int64_t now, int64_t lastVsync, int64_t period) {
        if (lastVsync <= 0 || period <= 0) return now;
        int64_t slot = lastVsync + ((now - lastVsync) / period + 1) * period;
        int64_t target = scanoutNextPresentNs + period;
        if (target < slot || target > now + period * 4)
            target = slot;
        scanoutNextPresentNs = target;
        return target;
    }
};

static int64_t vsyncBefore(int64_t t) { return (t / P) * P; }

int main(int argc, char** argv) {
    double latMs = (argc > 1) ? atof(argv[1]) : 6.6;
    double jitMs = (argc > 2) ? atof(argv[2]) : 0.5;
    const int64_t latency = (int64_t)(latMs * 1e6);
    const int64_t jitter = (int64_t)(jitMs * 1e6);
    const int N = 240;

    Pacer pacer;
    std::vector<int64_t> presents;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245 + 12345; return (int64_t)((seed >> 16) % 2001) - 1000; };

    int64_t threadFree = 0;
    for (int n = 0; n < N; n++) {
        int64_t arrival = (int64_t)n * 2 * P + (jitter ? rnd() * jitter / 1000 : 0);
        int64_t startProcess = std::max(arrival, threadFree);
        int64_t computeDone = startProcess + latency + (jitter ? rnd() * jitter / 1000 : 0);
        int64_t now = computeDone;
        int64_t lastVsync = vsyncBefore(now);

        int64_t k = pacer.nextVsyncSlot(now, lastVsync, P);
        presents.push_back(k);
        int64_t realT = k + P;
        presents.push_back(realT);
        pacer.scanoutNextPresentNs = k + P;
        threadFree = std::max(now, k);
    }

    std::sort(presents.begin(), presents.end());
    std::vector<int64_t> iv;
    for (size_t i = 1; i < presents.size(); i++) iv.push_back(presents[i] - presents[i-1]);

    double mean = 0; for (int64_t v : iv) mean += v; mean /= iv.size();
    double var = 0, mx = 0, mn = 1e18; int slips = 0;
    for (int64_t v : iv) {
        var += (v - mean) * (v - mean);
        if (v > mx) mx = v; if (v < mn) mn = v;
        if (llabs(v - P) > P / 4) slips++;
    }
    double sd = sqrt(var / iv.size());
    printf("latency=%.1fms jitter=%.1fms  meanInterval=%.2fms  stdDev=%.2fms  min=%.2f max=%.2f  slips(>25%%)=%d/%zu\n",
           latMs, jitMs, mean/1e6, sd/1e6, mn/1e6, mx/1e6, slips, iv.size());
    printf("ideal interval = %.2fms (60Hz). stdDev=judder; slips=hard stutters.\n", P/1e6);
    return 0;
}
