#include "present/sc_present.hh"

#include <android/log.h>
#include <dlfcn.h>
#include <time.h>

#define SC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "seifg_sc", __VA_ARGS__)

namespace {

using pfn_SCCreateFromWin = void* (*)(ANativeWindow*, const char*);
using pfn_SCRelease = void (*)(void*);
using pfn_STCreate = void* (*)();
using pfn_STDelete = void (*)(void*);
using pfn_STApply = int (*)(void*);
using pfn_STSetBuffer = void (*)(void*, void*, AHardwareBuffer*, int);
using pfn_STSetDesiredPresentTime = void (*)(void*, int64_t);
using pfn_STSetBackPressure = void (*)(void*, void*, bool);
using pfn_STSetBufferTransparency = void (*)(void*, void*, int32_t);
using pfn_STSetBufferTransform = void (*)(void*, void*, int32_t);

inline int64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

}

ScPresenter::~ScPresenter() {
    destroy();
}

bool ScPresenter::loadApi() {
    if (apiLoaded) return fnSCCreateFromWin != nullptr;
    apiLoaded = true;

    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) { SC_LOG("dlopen libandroid.so failed"); return false; }

    fnSCCreateFromWin = dlsym(lib, "ASurfaceControl_createFromWindow");
    fnSCRelease = dlsym(lib, "ASurfaceControl_release");
    fnSTCreate = dlsym(lib, "ASurfaceTransaction_create");
    fnSTDelete = dlsym(lib, "ASurfaceTransaction_delete");
    fnSTApply = dlsym(lib, "ASurfaceTransaction_apply");
    fnSTSetBuffer = dlsym(lib, "ASurfaceTransaction_setBuffer");
    fnSTSetDesiredPresentTime = dlsym(lib, "ASurfaceTransaction_setDesiredPresentTime");
    fnSTSetBackPressure = dlsym(lib, "ASurfaceTransaction_setEnableBackPressure");
    fnSTSetBufferTransparency = dlsym(lib, "ASurfaceTransaction_setBufferTransparency");
    fnSTSetBufferTransform = dlsym(lib, "ASurfaceTransaction_setBufferTransform");

    bool ok = fnSCCreateFromWin && fnSCRelease && fnSTCreate && fnSTDelete && fnSTApply && fnSTSetBuffer;
    if (!ok) { SC_LOG("ASurfaceControl symbols missing"); return false; }
    SC_LOG("API loaded");
    return true;
}

bool ScPresenter::init(ANativeWindow* win) {
    if (!win) return false;
    if (!loadApi()) return false;

    window = win;
    sc = ((pfn_SCCreateFromWin)fnSCCreateFromWin)(win, "seifg");
    if (!sc) { SC_LOG("ASurfaceControl_createFromWindow failed"); return false; }

    vsyncClock.start();
    SC_LOG("init ok sc=%p transform=%d", sc, bufTransform);
    return true;
}

void ScPresenter::destroy() {
    vsyncClock.stop();
    if (sc && fnSCRelease) {
        ((pfn_SCRelease)fnSCRelease)(sc);
        sc = nullptr;
    }
    window = nullptr;
    scanoutNextPresentNs = 0;
}

void ScPresenter::setBufferTransform(int32_t transform) {
    bufTransform = transform;
}

void ScPresenter::presentOne(AHardwareBuffer* ahb, int64_t desiredPresentTime, int acquireFence) {
    if (!sc || !fnSTCreate) return;

    void* tx = ((pfn_STCreate)fnSTCreate)();
    ((pfn_STSetBuffer)fnSTSetBuffer)(tx, sc, ahb, acquireFence);
    if (fnSTSetBufferTransform && bufTransform != 0)
        ((pfn_STSetBufferTransform)fnSTSetBufferTransform)(tx, sc, bufTransform);
    if (fnSTSetBufferTransparency)
        ((pfn_STSetBufferTransparency)fnSTSetBufferTransparency)(tx, sc, 2);
    if (fnSTSetBackPressure)
        ((pfn_STSetBackPressure)fnSTSetBackPressure)(tx, sc, true);
    if (fnSTSetDesiredPresentTime && desiredPresentTime > 0)
        ((pfn_STSetDesiredPresentTime)fnSTSetDesiredPresentTime)(tx, desiredPresentTime);
    ((pfn_STApply)fnSTApply)(tx);
    ((pfn_STDelete)fnSTDelete)(tx);
}

int64_t ScPresenter::nextVsyncSlot() {
    const int64_t now = nowNs();
    const int64_t vsync = vsyncClock.lastVsyncNs();
    const int64_t period = vsyncClock.periodNs();
    if (vsync <= 0 || period <= 0) return now + 1'000'000;

    if (scanoutNextPresentNs == 0)
        scanoutNextPresentNs = vsync;

    int64_t target = scanoutNextPresentNs + period;
    if (target < now - period / 2 || target > now + period * 4)
        target = vsync + ((now - vsync) / period + 1) * period;

    int64_t phase = (target - vsync) % period;
    if (phase < 0) phase += period;
    if (phase > period / 2) target += period - phase;
    else target -= phase;

    if (target <= now)
        target += period;

    scanoutNextPresentNs = target;
    return target;
}

void ScPresenter::commitBurst(int64_t lastSlotNs) {
    scanoutNextPresentNs = lastSlotNs;
}

void ScPresenter::busyWaitUntil(int64_t t) {
    const int64_t spinTail = 250'000;
    for (int64_t rem = t - nowNs(); rem > spinTail; rem = t - nowNs()) {
        const int64_t s = rem - spinTail;
        struct timespec ts{};
        ts.tv_sec = s / 1'000'000'000LL;
        ts.tv_nsec = s % 1'000'000'000LL;
        nanosleep(&ts, nullptr);
    }
    while (nowNs() < t) { }
}
