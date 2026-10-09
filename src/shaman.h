#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <grp.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <vector>
#include <iostream>
#include <sys/stat.h>
#include <signal.h>
#include <chrono>
#include <string>  // Added for std::string support

inline uint64_t get_steady_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

inline uint64_t get_epoch_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Backward-compatible alias for older callers. This is monotonic/steady time,
// not epoch time, and not any camera/acquisition timestamp.
inline uint64_t get_time_us() {
    return get_steady_time_us();
}

namespace shaman {

constexpr const char* SHM_IPC_GROUP = "ipc";
constexpr mode_t SHM_IPC_MODE = 0660;
constexpr size_t MAX_OBJECTS = 100;
constexpr size_t MAX_KEYPOINTS = 32;

struct Rect {
    float x, y, width, height;
};

struct Object {
    Rect rect;
    int label;
    float prob;
    float kps[MAX_KEYPOINTS];
    size_t num_kps;
};

// The SHAMAN v1 shared-memory queue (SharedBoxQueue, /shm_cam_<serial>)
// was retired on 2026-10-08: Citrus reads only the v2 live-state queue
// (shaman_v2.h). This header keeps the detection object type that the v2
// publisher and the analytics workers share.

} // namespace shaman
