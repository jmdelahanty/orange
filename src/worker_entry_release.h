#pragma once

#include "worker_entry_ownership_core.h"
#include "video_capture.h"

inline bool retain_worker_entry(
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context)
{
    return retain_worker_entry_ref(entry, context);
}

inline bool retain_worker_entry(WORKER_ENTRY* entry)
{
    return retain_worker_entry(entry, WorkerEntryReleaseContext{});
}

// The camera callback is a deliberately narrow seam: production passes the
// SDK's EVT_CameraQueueFrame, while host tests can inject its return value
// without constructing a live camera.  The callback must return the SDK-style
// EVT_ERROR result so non-success values are handled exactly like production.
template <typename CameraQueueFn>
inline bool release_worker_entry_to_recycle_with_camera_queue(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context,
    CameraQueueFn&& camera_queue) noexcept
{
    try {
        return release_worker_entry_ref(
            entry,
            context,
            [recycle_queue, context, &camera_queue](WORKER_ENTRY* final_entry) -> bool {
                if (final_entry->gpu_direct_mode &&
                    final_entry->camera_instance &&
                    final_entry->camera_frame_struct) {
                    try {
                        const EVT_ERROR queue_result = camera_queue(
                            final_entry->camera_instance,
                            final_entry->camera_frame_struct);
                        if (queue_result != EVT_SUCCESS) {
                            record_worker_entry_recycle_failure_detail(
                                context,
                                WorkerEntryRefCountIssueKind::CameraRequeueFailure);
                            return false;
                        }
                    } catch (...) {
                        record_worker_entry_recycle_failure_detail(
                            context,
                            WorkerEntryRefCountIssueKind::CameraRequeueFailure);
                        return false;
                    }
                }
                if (recycle_queue) {
                    try {
                        recycle_queue->push(final_entry);
                    } catch (...) {
                        record_worker_entry_recycle_failure_detail(
                            context,
                            WorkerEntryRefCountIssueKind::RecycleQueueFailure);
                        return false;
                    }
                }
                return true;
            });
    } catch (...) {
        // The ordinary finalization path catches recycle callback failures.
        // This outer boundary also protects against failures in diagnostic
        // handling (for example, context-bucket allocation) so guards remain
        // safe during exception unwinding.
        record_worker_entry_recycle_failure(entry, context);
        return false;
    }
}

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context) noexcept
{
    return release_worker_entry_to_recycle_with_camera_queue(
        recycle_queue,
        entry,
        context,
        [](Emergent::CEmergentCamera* camera,
           Emergent::CEmergentFrame* frame) noexcept -> EVT_ERROR {
            return EVT_CameraQueueFrame(camera, frame);
        });
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

struct WorkerEntryRecycleReleaseFn {
    SafeQueue<WORKER_ENTRY*>* recycle_queue = nullptr;

    bool operator()(
        WORKER_ENTRY* release_entry,
        const WorkerEntryReleaseContext& release_context) const noexcept
    {
        return release_worker_entry_to_recycle(
            recycle_queue,
            release_entry,
            release_context);
    }
};

class WorkerEntryRefGuard {
public:
    WorkerEntryRefGuard(
        SafeQueue<WORKER_ENTRY*>* recycle_queue,
        WORKER_ENTRY* entry,
        WorkerEntryReleaseContext context,
        bool owns_ref)
        : guard_(make_worker_entry_ref_guard(
              entry,
              context,
              WorkerEntryRecycleReleaseFn{recycle_queue},
              owns_ref))
    {
    }

    WorkerEntryRefGuard(const WorkerEntryRefGuard&) = delete;
    WorkerEntryRefGuard& operator=(const WorkerEntryRefGuard&) = delete;
    WorkerEntryRefGuard(WorkerEntryRefGuard&& other) noexcept = default;
    WorkerEntryRefGuard& operator=(WorkerEntryRefGuard&& other) noexcept = delete;

    void Dismiss()
    {
        guard_.Dismiss();
    }

    bool active() const
    {
        return guard_.active();
    }

private:
    WorkerEntryRefGuardCore<WORKER_ENTRY, WorkerEntryRecycleReleaseFn> guard_;
};

using WorkerEntryRetainedRefGuard = WorkerEntryRefGuard;

class WorkerEntryLease {
public:
    WorkerEntryLease(
        SafeQueue<WORKER_ENTRY*>* recycle_queue,
        WORKER_ENTRY* entry,
        WorkerEntryReleaseContext context)
        : lease_(try_retain_worker_entry_ref_lease(
              entry,
              context,
              WorkerEntryRecycleReleaseFn{recycle_queue}))
    {
    }

    WorkerEntryLease(const WorkerEntryLease&) = delete;
    WorkerEntryLease& operator=(const WorkerEntryLease&) = delete;
    WorkerEntryLease(WorkerEntryLease&& other) noexcept = default;
    WorkerEntryLease& operator=(WorkerEntryLease&& other) noexcept = delete;

    bool active() const
    {
        return lease_.active();
    }

    explicit operator bool() const
    {
        return active();
    }

    void TransferToConsumer()
    {
        lease_.TransferToConsumer();
    }

    void Dismiss()
    {
        TransferToConsumer();
    }

private:
    WorkerEntryRefLeaseCore<WORKER_ENTRY, WorkerEntryRecycleReleaseFn> lease_;
};

inline WorkerEntryLease TryRetainWorkerEntryLease(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    WorkerEntryReleaseContext context)
{
    return WorkerEntryLease(recycle_queue, entry, context);
}

inline WorkerEntryLease TryRetainWorkerEntryLease(
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    WORKER_ENTRY* entry,
    WorkerEntryReleaseContext context)
{
    return TryRetainWorkerEntryLease(&recycle_queue, entry, context);
}

template <typename WorkerT>
inline bool retain_and_enqueue_worker_entry(
    WorkerT* worker,
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context,
    bool* enqueue_rejected = nullptr)
{
    return retain_and_enqueue_worker_entry_ref(
        worker,
        entry,
        context,
        WorkerEntryRecycleReleaseFn{recycle_queue},
        enqueue_rejected);
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
