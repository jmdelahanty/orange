#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "video_capture.h"

struct WorkerEntryReleaseContext {
    const char* camera_serial = nullptr;
    const char* worker_name = nullptr;
};

struct WorkerEntryRefCountDiagnosticCounts {
    uint64_t release_underflows = 0;
    uint64_t double_releases = 0;
    uint64_t retain_after_release = 0;
};

struct WorkerEntryRefCountDiagnosticBucket {
    std::string camera_serial;
    std::string worker_name;
    WorkerEntryRefCountDiagnosticCounts counts;
};

enum class WorkerEntryRefCountIssueKind {
    ReleaseUnderflow,
    DoubleRelease,
    RetainAfterRelease,
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

inline std::mutex& worker_entry_context_diagnostic_mutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::string, WorkerEntryRefCountDiagnosticBucket>&
worker_entry_context_diagnostic_buckets()
{
    static std::unordered_map<std::string, WorkerEntryRefCountDiagnosticBucket> buckets;
    return buckets;
}

inline std::string worker_entry_context_part(const char* value)
{
    if (!value || value[0] == '\0') {
        return {};
    }
    std::string part(value);
    if (part == "unknown") {
        return {};
    }
    return part;
}

inline std::string worker_entry_context_key(
    const std::string& camera_serial,
    const std::string& worker_name)
{
    return camera_serial + '\x1f' + worker_name;
}

inline WorkerEntryRefCountDiagnosticCounts worker_entry_ref_count_diagnostic_counts_for_context(
    const WorkerEntryReleaseContext& context)
{
    const std::string camera_serial = worker_entry_context_part(context.camera_serial);
    const std::string worker_name = worker_entry_context_part(context.worker_name);
    if (camera_serial.empty() && worker_name.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(worker_entry_context_diagnostic_mutex());
    const auto& buckets = worker_entry_context_diagnostic_buckets();
    const auto it = buckets.find(worker_entry_context_key(camera_serial, worker_name));
    if (it == buckets.end()) {
        return {};
    }
    return it->second.counts;
}

inline WorkerEntryRefCountDiagnosticCounts worker_entry_ref_count_diagnostic_counts_for_camera(
    const std::string& camera_serial)
{
    if (camera_serial.empty() || camera_serial == "unknown") {
        return {};
    }

    WorkerEntryRefCountDiagnosticCounts total;
    std::lock_guard<std::mutex> lock(worker_entry_context_diagnostic_mutex());
    const auto& buckets = worker_entry_context_diagnostic_buckets();
    for (const auto& [key, bucket] : buckets) {
        (void)key;
        if (bucket.camera_serial != camera_serial) {
            continue;
        }
        total.release_underflows += bucket.counts.release_underflows;
        total.double_releases += bucket.counts.double_releases;
        total.retain_after_release += bucket.counts.retain_after_release;
    }
    return total;
}

inline void record_worker_entry_context_diagnostic(
    const WorkerEntryReleaseContext& context,
    const WorkerEntryRefCountIssueKind kind)
{
    const std::string camera_serial = worker_entry_context_part(context.camera_serial);
    const std::string worker_name = worker_entry_context_part(context.worker_name);
    if (camera_serial.empty() && worker_name.empty()) {
        return;
    }

    constexpr size_t kMaxWorkerEntryDiagnosticContexts = 128;
    std::lock_guard<std::mutex> lock(worker_entry_context_diagnostic_mutex());
    auto& buckets = worker_entry_context_diagnostic_buckets();
    const std::string key = worker_entry_context_key(camera_serial, worker_name);
    auto it = buckets.find(key);
    if (it == buckets.end()) {
        if (buckets.size() >= kMaxWorkerEntryDiagnosticContexts) {
            return;
        }
        auto inserted = buckets.emplace(
            key,
            WorkerEntryRefCountDiagnosticBucket{camera_serial, worker_name, {}});
        it = inserted.first;
    }

    switch (kind) {
    case WorkerEntryRefCountIssueKind::ReleaseUnderflow:
        it->second.counts.release_underflows++;
        break;
    case WorkerEntryRefCountIssueKind::DoubleRelease:
        it->second.counts.double_releases++;
        break;
    case WorkerEntryRefCountIssueKind::RetainAfterRelease:
        it->second.counts.retain_after_release++;
        break;
    }
}

inline void reset_worker_entry_release_diagnostics_for_tests()
{
    worker_entry_release_underflow_count().store(0, std::memory_order_release);
    worker_entry_release_double_release_count().store(0, std::memory_order_release);
    worker_entry_retain_after_release_count().store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(worker_entry_context_diagnostic_mutex());
    worker_entry_context_diagnostic_buckets().clear();
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
            record_worker_entry_context_diagnostic(
                context,
                WorkerEntryRefCountIssueKind::RetainAfterRelease);
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
                record_worker_entry_context_diagnostic(
                    context,
                    WorkerEntryRefCountIssueKind::DoubleRelease);
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
                record_worker_entry_context_diagnostic(
                    context,
                    WorkerEntryRefCountIssueKind::ReleaseUnderflow);
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

class WorkerEntryRefGuard {
public:
    WorkerEntryRefGuard(
        SafeQueue<WORKER_ENTRY*>* recycle_queue,
        WORKER_ENTRY* entry,
        WorkerEntryReleaseContext context,
        bool owns_ref)
        : recycle_queue_(recycle_queue),
          entry_(entry),
          context_(context),
          owns_ref_(owns_ref)
    {
    }

    WorkerEntryRefGuard(const WorkerEntryRefGuard&) = delete;
    WorkerEntryRefGuard& operator=(const WorkerEntryRefGuard&) = delete;

    WorkerEntryRefGuard(WorkerEntryRefGuard&& other) noexcept
        : recycle_queue_(other.recycle_queue_),
          entry_(other.entry_),
          context_(other.context_),
          owns_ref_(other.owns_ref_)
    {
        other.owns_ref_ = false;
    }

    WorkerEntryRefGuard& operator=(WorkerEntryRefGuard&& other) noexcept = delete;

    ~WorkerEntryRefGuard()
    {
        if (owns_ref_) {
            release_worker_entry_to_recycle(recycle_queue_, entry_, context_);
        }
    }

    void Dismiss()
    {
        owns_ref_ = false;
    }

    bool active() const
    {
        return owns_ref_;
    }

private:
    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;
    WORKER_ENTRY* entry_ = nullptr;
    WorkerEntryReleaseContext context_{};
    bool owns_ref_ = false;
};

using WorkerEntryRetainedRefGuard = WorkerEntryRefGuard;

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
