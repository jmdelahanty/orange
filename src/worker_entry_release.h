#pragma once

#include <atomic>

#include "video_capture.h"

inline bool release_worker_entry_to_recycle(
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;
    }
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return false;
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
