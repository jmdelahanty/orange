#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>

#include "video_capture.h"

struct WorkerEntryReleaseContext {
    const char* camera_serial = nullptr;
    const char* worker_name = nullptr;
};

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

inline std::atomic<uint64_t>& worker_entry_retain_after_release_count()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

inline void reset_worker_entry_release_diagnostics_for_tests()
{
    worker_entry_release_underflow_count().store(0, std::memory_order_release);
    worker_entry_release_double_release_count().store(0, std::memory_order_release);
    worker_entry_retain_after_release_count().store(0, std::memory_order_release);
}

inline bool should_log_worker_entry_release_issue(const uint64_t count)
{
    return count <= 16 || (count & (count - 1)) == 0;
}

inline void log_worker_entry_release_issue(
    const char* kind,
    const uint64_t count,
    const WORKER_ENTRY* entry,
    const int current_ref_count,
    const WorkerEntryReleaseContext& context);

inline void log_worker_entry_ref_count_issue(
    const char* source,
    const char* kind,
    const uint64_t count,
    const WORKER_ENTRY* entry,
    const int current_ref_count,
    const WorkerEntryReleaseContext& context)
{
    if (!should_log_worker_entry_release_issue(count)) {
        return;
    }

    const char* camera_serial =
        (context.camera_serial && context.camera_serial[0] != '\0')
            ? context.camera_serial
            : "unknown";
    const char* worker_name =
        (context.worker_name && context.worker_name[0] != '\0')
            ? context.worker_name
            : "unknown";

    std::cerr << "[" << source << "] " << kind
              << " count=" << count
              << " cam=" << camera_serial
              << " worker=" << worker_name
              << " frame=" << (entry ? entry->frame_id : 0)
              << " recording_frame=" << (entry ? entry->recording_frame_id : 0)
              << " camera_frame=" << (entry ? entry->camera_frame_id : 0)
              << " current_ref_count=" << current_ref_count
              << std::endl;
}

inline void log_worker_entry_release_issue(
    const char* kind,
    const uint64_t count,
    const WORKER_ENTRY* entry,
    const int current_ref_count,
    const WorkerEntryReleaseContext& context)
{
    log_worker_entry_ref_count_issue(
        "WORKER_ENTRY_RELEASE",
        kind,
        count,
        entry,
        current_ref_count,
        context);
}

inline void log_worker_entry_retain_issue(
    const char* kind,
    const uint64_t count,
    const WORKER_ENTRY* entry,
    const int current_ref_count,
    const WorkerEntryReleaseContext& context)
{
    log_worker_entry_ref_count_issue(
        "WORKER_ENTRY_RETAIN",
        kind,
        count,
        entry,
        current_ref_count,
        context);
}

inline bool retain_worker_entry(
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context)
{
    if (!entry) {
        return false;
    }

    int current = entry->ref_count.load(std::memory_order_acquire);
    while (true) {
        if (current <= 0) {
            const uint64_t count =
                worker_entry_retain_after_release_count().fetch_add(
                    1,
                    std::memory_order_acq_rel) + 1;
            log_worker_entry_retain_issue(
                "retain_after_release",
                count,
                entry,
                current,
                context);
            return false;
        }

        if (entry->ref_count.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

inline bool retain_worker_entry(WORKER_ENTRY* entry)
{
    return retain_worker_entry(entry, WorkerEntryReleaseContext{});
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context)
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
                    current,
                    context);
            } else {
                const uint64_t count =
                    worker_entry_release_underflow_count().fetch_add(
                        1,
                        std::memory_order_acq_rel) + 1;
                log_worker_entry_release_issue(
                    "ref_count_underflow",
                    count,
                    entry,
                    current,
                    context);
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
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry)
{
    return release_worker_entry_to_recycle(
        recycle_queue,
        entry,
        WorkerEntryReleaseContext{});
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context)
{
    return release_worker_entry_to_recycle(&recycle_queue, entry, context);
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    WORKER_ENTRY* entry)
{
    return release_worker_entry_to_recycle(&recycle_queue, entry);
}

template <typename WorkerT>
inline bool retain_and_enqueue_worker_entry(
    WorkerT* worker,
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context,
    bool* enqueue_rejected = nullptr)
{
    if (enqueue_rejected) {
        *enqueue_rejected = false;
    }
    if (!worker || !retain_worker_entry(entry, context)) {
        return false;
    }
    if (worker->PutObjectToQueueIn(entry)) {
        return true;
    }
    if (enqueue_rejected) {
        *enqueue_rejected = true;
    }
    release_worker_entry_to_recycle(recycle_queue, entry, context);
    return false;
}

template <typename WorkerT>
inline bool retain_and_enqueue_worker_entry(
    WorkerT* worker,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context,
    bool* enqueue_rejected = nullptr)
{
    return retain_and_enqueue_worker_entry(
        worker,
        &recycle_queue,
        entry,
        context,
        enqueue_rejected);
}
