#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include "present/vsync_clock.hh"

class ScPresenter {
public:
    ScPresenter() = default;
    ~ScPresenter();

    bool init(ANativeWindow* window);
    void destroy();

    void presentOne(AHardwareBuffer* ahb, int64_t desiredPresentTime, int acquireFence = -1);
    int64_t nextVsyncSlot();
    void commitBurst(int64_t lastSlotNs);

    void busyWaitUntil(int64_t targetNs);

    void setBufferTransform(int32_t transform);

    bool ready() const { return sc != nullptr; }

    VsyncClock& clock() { return vsyncClock; }

    ScPresenter(const ScPresenter&) = delete;
    ScPresenter& operator=(const ScPresenter&) = delete;

private:
    bool loadApi();

    VsyncClock vsyncClock;
    int64_t scanoutNextPresentNs = 0;
    int32_t bufTransform = 0;

    ANativeWindow* window = nullptr;
    void* sc = nullptr;
    bool apiLoaded = false;

    void* fnSCCreateFromWin = nullptr;
    void* fnSCRelease = nullptr;
    void* fnSTCreate = nullptr;
    void* fnSTDelete = nullptr;
    void* fnSTApply = nullptr;
    void* fnSTSetBuffer = nullptr;
    void* fnSTSetDesiredPresentTime = nullptr;
    void* fnSTSetBackPressure = nullptr;
    void* fnSTSetBufferTransparency = nullptr;
    void* fnSTSetBufferTransform = nullptr;
};
