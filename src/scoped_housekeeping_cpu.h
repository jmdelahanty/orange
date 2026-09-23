#pragma once
#include <pthread.h>
#include <sched.h>
#include <stdexcept>

namespace orange {
// For bounded, explicitly requested off-hot-path work on an existing thread.
// Restores the original mask. Do not use on acquisition or render threads.
class ScopedHousekeepingCpu {
public:
    explicit ScopedHousekeepingCpu(int cpu) {
        if (cpu == -1) return;
        if (cpu < 0 || cpu >= CPU_SETSIZE || pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_) != 0)
            throw std::runtime_error("invalid housekeeping CPU or unreadable thread affinity");
        cpu_set_t requested; CPU_ZERO(&requested); CPU_SET(cpu, &requested);
        if (pthread_setaffinity_np(pthread_self(), sizeof(requested), &requested) != 0)
            throw std::runtime_error("housekeeping CPU affinity could not be applied");
        active_ = true;
    }
    ~ScopedHousekeepingCpu() { if (active_) pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_); }
    ScopedHousekeepingCpu(const ScopedHousekeepingCpu&) = delete;
    ScopedHousekeepingCpu& operator=(const ScopedHousekeepingCpu&) = delete;
    void Restore() {
        if (active_ && pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_) != 0)
            throw std::runtime_error("snapshot worker affinity could not be restored");
        active_ = false;
    }
private:
    cpu_set_t saved_{};
    bool active_ = false;
};
}
