#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>

#include "video_capture.h"

inline std::atomic<uint64_t>& worker_entry_release_underflow_count()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline std::atomic<uint64_t>& worker_entry_release_double_release_count()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline void reset_worker_entry_release_diagnostics_for_tests()
{
    worker_entry_release_underflow_count().store(0, std::memory_order_release);
    worker_entry_release_double_release_count().store(0, std::memory_order_release);
}

inline bool should_log_worker_entry_release_issue(const uint64_t count)
{
    return count <= 16 || (count & (count - 1)) == 0;
}

inline void log_worker_entry_release_issue(
    const char* kind,
    const uint64_t count,
    const WORKER_ENTRY* entry,
    const int current_ref_count)
{
    if (!should_log_worker_entry_release_issue(count)) {
        return;
    }

    std::cerr << "[WORKER_ENTRY_RELEASE] " << kind
              << " count=" << count
              << " cam=unknown"
              << " frame=" << (entry ? entry->frame_id : 0)
              << " recording_frame=" << (entry ? entry->recording_frame_id : 0)
              << " camera_frame=" << (entry ? entry->camera_frame_id : 0)
              << " current_ref_count=" << current_ref_count
              << std::endl;
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;
    }

    int current = entry->ref_count.load(std::memory_order_acquire);
    while (true) {
        if (current <= 0) {
            if (current == 0) {
                const uint64_t count =
                    worker_entry_release_double_release_count().fetch_add(
                        1,
                        std::memory_order_acq_rel) + 1;
                log_worker_entry_release_issue(
                    "double_release",
                    count,
                    entry,
                    current);
            } else {
                const uint64_t count =
                    worker_entry_release_underflow_count().fetch_add(
                        1,
                        std::memory_order_acq_rel) + 1;
                log_worker_entry_release_issue(
                    "ref_count_underflow",
                    count,
                    entry,
                    current);
            }
            return false;
        }

        const int desired = current - 1;
        if (entry->ref_count.compare_exchange_weak(
                current,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (desired != 0) {
                return false;
            }
            break;
        }
    }

    if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
        EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
    }
    if (recycle_queue) {
        recycle_queue->push(entry);
    }
    return true;
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    WORKER_ENTRY* entry)
{
    return release_worker_entry_to_recycle(&recycle_queue, entry);
}
