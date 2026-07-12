#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

struct ALooper;

class VsyncClock {
public:
    VsyncClock() = default;
    ~VsyncClock();

    void start();
    void stop();

    int64_t lastVsyncNs() const { return lastVsync.load(std::memory_order_relaxed); }
    int64_t periodNs() const { return period.load(std::memory_order_relaxed); }

    VsyncClock(const VsyncClock&) = delete;
    VsyncClock& operator=(const VsyncClock&) = delete;

private:
    void threadMain();
    static void frameCallback(int64_t frameTimeNanos, void* data);
    void onVsync(int64_t frameTimeNanos);

    std::thread thread;
    std::mutex lifecycleMutex;
    std::atomic<bool> running{false};
    std::atomic<ALooper*> looper{nullptr};

    std::atomic<int64_t> lastVsync{0};
    std::atomic<int64_t> period{0};
    int64_t prevVsync = 0;
    int64_t vsyncCount = 0;

    void* fnGetInstance = nullptr;
    void* fnPostFrameCallback64 = nullptr;
    void* choreographer = nullptr;
};
