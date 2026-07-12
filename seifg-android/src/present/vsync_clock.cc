#include "present/vsync_clock.hh"

#include <android/log.h>
#include <android/looper.h>
#include <dlfcn.h>

#define VLOG(...) __android_log_print(ANDROID_LOG_INFO, "seifg_vsync", __VA_ARGS__)

namespace {
    using pfn_getInstance = void* (*)();
    using frame_cb64 = void (*)(int64_t, void*);
    using pfn_postFrameCallback64 = void (*)(void*, frame_cb64, void*);

    constexpr int64_t kMinPeriodNs = 1'000'000;
    constexpr int64_t kMaxPeriodNs = 100'000'000;
}

VsyncClock::~VsyncClock() {
    stop();
}

void VsyncClock::start() {
    std::lock_guard<std::mutex> lk(lifecycleMutex);
    if (running.load()) return;
    if (thread.joinable()) thread.join();
    running.store(true);
    thread = std::thread(&VsyncClock::threadMain, this);
}

void VsyncClock::stop() {
    std::lock_guard<std::mutex> lk(lifecycleMutex);
    running.store(false);
    if (ALooper* lp = looper.load()) ALooper_wake(lp);
    if (thread.joinable()) thread.join();
    looper.store(nullptr);
    lastVsync.store(0);
    period.store(0);
    prevVsync = 0;
    choreographer = nullptr;
}

void VsyncClock::frameCallback(int64_t frameTimeNanos, void* data) {
    static_cast<VsyncClock*>(data)->onVsync(frameTimeNanos);
}

void VsyncClock::onVsync(int64_t frameTimeNanos) {
    if (prevVsync != 0) {
        const int64_t delta = frameTimeNanos - prevVsync;
        if (delta > kMinPeriodNs && delta < kMaxPeriodNs) {
            const int64_t cur = period.load(std::memory_order_relaxed);
            period.store(cur == 0 ? delta : (cur * 7 + delta) / 8, std::memory_order_relaxed);
        }
    }
    prevVsync = frameTimeNanos;
    lastVsync.store(frameTimeNanos, std::memory_order_relaxed);

    if ((++vsyncCount % 120) == 1)
        VLOG("vsync tick #%lld period=%lld us", (long long)vsyncCount,
             (long long)(period.load(std::memory_order_relaxed) / 1000));

    if (running.load() && choreographer && fnPostFrameCallback64)
        ((pfn_postFrameCallback64)fnPostFrameCallback64)(choreographer, &VsyncClock::frameCallback, this);
}

void VsyncClock::threadMain() {
    ALooper* lp = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!lp) {
        VLOG("ALooper_prepare failed");
        running.store(false);
        return;
    }
    looper.store(lp);

    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib) {
        fnGetInstance = dlsym(lib, "AChoreographer_getInstance");
        fnPostFrameCallback64 = dlsym(lib, "AChoreographer_postFrameCallback64");
    }
    if (!fnGetInstance || !fnPostFrameCallback64) {
        VLOG("AChoreographer symbols missing");
        looper.store(nullptr);
        running.store(false);
        return;
    }

    choreographer = ((pfn_getInstance)fnGetInstance)();
    if (!choreographer) {
        VLOG("AChoreographer_getInstance returned null");
        looper.store(nullptr);
        running.store(false);
        return;
    }

    ((pfn_postFrameCallback64)fnPostFrameCallback64)(choreographer, &VsyncClock::frameCallback, this);
    VLOG("vsync clock started");

    while (running.load())
        ALooper_pollOnce(250, nullptr, nullptr, nullptr);

    looper.store(nullptr);
}
