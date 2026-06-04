#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

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

template <typename EntryT, typename = void>
struct WorkerEntryDebugFieldAccess {
    static uint64_t frame_id(const EntryT*)
    {
        return 0;
    }

    static uint64_t recording_frame_id(const EntryT*)
    {
        return 0;
    }

    static uint64_t camera_frame_id(const EntryT*)
    {
        return 0;
    }
};

template <typename EntryT>
struct WorkerEntryDebugFieldAccess<
    EntryT,
    std::void_t<
        decltype(std::declval<const EntryT&>().frame_id),
        decltype(std::declval<const EntryT&>().recording_frame_id),
        decltype(std::declval<const EntryT&>().camera_frame_id)>> {
    static uint64_t frame_id(const EntryT* entry)
    {
        return entry ? static_cast<uint64_t>(entry->frame_id) : 0;
    }

    static uint64_t recording_frame_id(const EntryT* entry)
    {
        return entry ? static_cast<uint64_t>(entry->recording_frame_id) : 0;
    }

    static uint64_t camera_frame_id(const EntryT* entry)
    {
        return entry ? static_cast<uint64_t>(entry->camera_frame_id) : 0;
    }
};

template <typename EntryT>
inline void log_worker_entry_ref_count_issue(
    const char* source,
    const char* kind,
    const uint64_t count,
    const EntryT* entry,
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
              << " frame=" << WorkerEntryDebugFieldAccess<EntryT>::frame_id(entry)
              << " recording_frame=" << WorkerEntryDebugFieldAccess<EntryT>::recording_frame_id(entry)
              << " camera_frame=" << WorkerEntryDebugFieldAccess<EntryT>::camera_frame_id(entry)
              << " current_ref_count=" << current_ref_count
              << std::endl;
}

template <typename EntryT>
inline void log_worker_entry_release_issue(
    const char* kind,
    const uint64_t count,
    const EntryT* entry,
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

template <typename EntryT>
inline void log_worker_entry_retain_issue(
    const char* kind,
    const uint64_t count,
    const EntryT* entry,
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

template <typename EntryT>
inline bool retain_worker_entry_ref(
    EntryT* entry,
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

template <typename EntryT>
inline bool retain_worker_entry_ref(EntryT* entry)
{
    return retain_worker_entry_ref(entry, WorkerEntryReleaseContext{});
}

template <typename EntryT, typename RecycleFn>
inline bool release_worker_entry_ref(
    EntryT* entry,
    const WorkerEntryReleaseContext& context,
    RecycleFn&& recycle_final)
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

    recycle_final(entry);
    return true;
}

template <typename EntryT>
inline bool release_worker_entry_ref(
    EntryT* entry,
    const WorkerEntryReleaseContext& context)
{
    return release_worker_entry_ref(
        entry,
        context,
        [](EntryT*) {});
}

template <typename EntryT>
inline bool release_worker_entry_ref(EntryT* entry)
{
    return release_worker_entry_ref(entry, WorkerEntryReleaseContext{});
}

template <typename EntryT, typename ReleaseFn>
class WorkerEntryRefGuardCore {
public:
    WorkerEntryRefGuardCore(
        EntryT* entry,
        WorkerEntryReleaseContext context,
        ReleaseFn release_fn,
        bool owns_ref)
        : entry_(entry),
          context_(context),
          release_fn_(std::move(release_fn)),
          owns_ref_(owns_ref)
    {
    }

    WorkerEntryRefGuardCore(const WorkerEntryRefGuardCore&) = delete;
    WorkerEntryRefGuardCore& operator=(const WorkerEntryRefGuardCore&) = delete;

    WorkerEntryRefGuardCore(WorkerEntryRefGuardCore&& other) noexcept
        : entry_(other.entry_),
          context_(other.context_),
          release_fn_(std::move(other.release_fn_)),
          owns_ref_(other.owns_ref_)
    {
        other.owns_ref_ = false;
    }

    WorkerEntryRefGuardCore& operator=(WorkerEntryRefGuardCore&& other) noexcept = delete;

    ~WorkerEntryRefGuardCore()
    {
        if (owns_ref_) {
            release_fn_(entry_, context_);
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
    EntryT* entry_ = nullptr;
    WorkerEntryReleaseContext context_{};
    ReleaseFn release_fn_;
    bool owns_ref_ = false;
};

template <typename EntryT, typename ReleaseFn>
inline WorkerEntryRefGuardCore<EntryT, std::decay_t<ReleaseFn>>
make_worker_entry_ref_guard(
    EntryT* entry,
    WorkerEntryReleaseContext context,
    ReleaseFn&& release_fn,
    bool owns_ref)
{
    return WorkerEntryRefGuardCore<EntryT, std::decay_t<ReleaseFn>>(
        entry,
        context,
        std::forward<ReleaseFn>(release_fn),
        owns_ref);
}

template <typename EntryT, typename ReleaseFn>
class WorkerEntryRefLeaseCore {
public:
    WorkerEntryRefLeaseCore(
        EntryT* entry,
        WorkerEntryReleaseContext context,
        ReleaseFn release_fn,
        bool owns_ref)
        : guard_(make_worker_entry_ref_guard(
              entry,
              context,
              std::move(release_fn),
              owns_ref))
    {
    }

    WorkerEntryRefLeaseCore(const WorkerEntryRefLeaseCore&) = delete;
    WorkerEntryRefLeaseCore& operator=(const WorkerEntryRefLeaseCore&) = delete;
    WorkerEntryRefLeaseCore(WorkerEntryRefLeaseCore&& other) noexcept = default;
    WorkerEntryRefLeaseCore& operator=(WorkerEntryRefLeaseCore&& other) noexcept = delete;

    bool active() const
    {
        return guard_.active();
    }

    explicit operator bool() const
    {
        return active();
    }

    void TransferToConsumer()
    {
        guard_.Dismiss();
    }

    void Dismiss()
    {
        TransferToConsumer();
    }

private:
    WorkerEntryRefGuardCore<EntryT, ReleaseFn> guard_;
};

template <typename EntryT, typename ReleaseFn>
inline WorkerEntryRefLeaseCore<EntryT, std::decay_t<ReleaseFn>>
try_retain_worker_entry_ref_lease(
    EntryT* entry,
    WorkerEntryReleaseContext context,
    ReleaseFn&& release_fn)
{
    const bool retained = retain_worker_entry_ref(entry, context);
    return WorkerEntryRefLeaseCore<EntryT, std::decay_t<ReleaseFn>>(
        entry,
        context,
        std::forward<ReleaseFn>(release_fn),
        retained);
}

template <typename WorkerT, typename EntryT, typename ReleaseFn>
inline bool retain_and_enqueue_worker_entry_ref(
    WorkerT* worker,
    EntryT* entry,
    const WorkerEntryReleaseContext& context,
    ReleaseFn&& release_fn,
    bool* enqueue_rejected = nullptr)
{
    if (enqueue_rejected) {
        *enqueue_rejected = false;
    }
    if (!worker || !retain_worker_entry_ref(entry, context)) {
        return false;
    }
    auto retained_ref_guard = make_worker_entry_ref_guard(
        entry,
        context,
        std::forward<ReleaseFn>(release_fn),
        true);
    if (worker->PutObjectToQueueIn(entry)) {
        retained_ref_guard.Dismiss();
        return true;
    }
    if (enqueue_rejected) {
        *enqueue_rejected = true;
    }
    return false;
}
