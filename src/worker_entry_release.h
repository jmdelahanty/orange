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

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry,
    const WorkerEntryReleaseContext& context)
{
    return release_worker_entry_ref(
        entry,
        context,
        [recycle_queue](WORKER_ENTRY* final_entry) {
            if (final_entry->gpu_direct_mode &&
                final_entry->camera_instance &&
                final_entry->camera_frame_struct) {
                EVT_CameraQueueFrame(
                    final_entry->camera_instance,
                    final_entry->camera_frame_struct);
            }
            if (recycle_queue) {
                recycle_queue->push(final_entry);
            }
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

    void operator()(
        WORKER_ENTRY* release_entry,
        const WorkerEntryReleaseContext& release_context) const
    {
        release_worker_entry_to_recycle(
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
