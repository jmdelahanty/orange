// src/late_owned_copy.h
//
// Lever 2d (2026-09-04): the owned analytics copy of a GPUDirect frame is
// issued by the YOLO worker after detection completes instead of by the
// acquisition thread at t=0. Preprocess then reads the camera buffer alone
// (0.076 ms instead of 0.29 under contention with the copy) and the graph
// runs alone (issuing the copy after preprocess, overlapping the graph,
// slowed it from 2.00 to 2.26 ms). The copy runs in the idle part of the
// frame period. Enabled by ORANGE_ANALYTICS_LATE_OWNED_COPY (default off
// until the A/B).
//
// Every path that can end a frame's YOLO consumption must call
// issue_late_owned_copy() exactly once while entry->late_owned_copy_pending
// is set, so delayed consumers (recorder, display) always see a recorded
// analytics_ready_event. They must wait for
// entry->analytics_ready_event_recorded before synchronizing on it.
#pragma once

#include <cuda_runtime_api.h>

#include "video_capture.h"
#include "yolo_runtime_flags.h"

inline bool late_owned_copy_enabled()
{
    static const bool enabled =
        orange::yolo_flags::EnvFlag("ORANGE_ANALYTICS_LATE_OWNED_COPY", false);
    return enabled;
}

// Enqueue the pool copy on the entry's acquisition stream, after `after` if
// given (the YOLO input-ready event), then record analytics_ready_event and
// publish it. Returns false if there was nothing pending.
inline bool issue_late_owned_copy(WORKER_ENTRY* entry, cudaEvent_t* after, const bool timing)
{
    if (!entry || !entry->late_owned_copy_pending) {
        return false;
    }
    cudaStream_t stream = entry->late_owned_copy_stream;
    if (after) {
        ck(cudaStreamWaitEvent(stream, *after, 0));
    }
    entry->analytics_copy_timed = false;
    if (timing && entry->analytics_copy_timing_start) {
        ck(cudaEventRecord(entry->analytics_copy_timing_start, stream));
    }
    ck(cudaMemcpyAsync(
        entry->d_analytics_image,
        entry->d_image,
        entry->late_owned_copy_bytes,
        cudaMemcpyDeviceToDevice,
        stream));
    if (timing && entry->analytics_copy_timing_end) {
        ck(cudaEventRecord(entry->analytics_copy_timing_end, stream));
        entry->analytics_copy_timed = true;
    }
    ck(cudaEventRecord(entry->analytics_ready_event, stream));
    entry->late_owned_copy_pending = false;
    entry->analytics_ready_event_recorded.store(true, std::memory_order_release);
    return true;
}
