// src/yolo_runtime_flags.h
//
// Single source of truth for the environment flags that shape the YOLO
// worker's hot path. YoloWorker reads these at start; recording snapshots
// write the resolved values so a run can be interpreted without knowing the
// launch environment. Keep defaults here and nowhere else.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace orange::yolo_flags {

// Returns `default_on` when the variable is unset or empty. Otherwise "0",
// "false", "off", and "no" (any case) mean off; anything else means on.
inline bool EnvFlag(const char* name, bool default_on)
{
    const char* env = std::getenv(name);
    if (!env || !*env) {
        return default_on;
    }
    std::string normalized(env);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized != "0" &&
           normalized != "false" &&
           normalized != "off" &&
           normalized != "no";
}

struct ResolvedFlags {
    // ORANGE_YOLO_SYNC_EVENT: wait for inference with cudaEventSynchronize
    // instead of polling cudaStreamQuery with usleep(100). Default on since
    // 2026-09-03 (about 80 us per frame, verified in the three-camera A/B).
    bool sync_event = true;
    // ORANGE_YOLO_DETACH_INPUT: record an input-ready event after preprocess so
    // the source frame can be released before inference finishes.
    bool detach_input = true;
    // ORANGE_YOLO_READY_EVENT_FASTPATH: skip cudaStreamWaitEvent when the
    // ingress event has already completed.
    bool ready_event_fast_path = true;
    // ORANGE_INLINE_CROP_PRODUCER: run the crop producer on the YOLO thread.
    bool inline_crop_producer = false;
    // ORANGE_YOLO_SKIP_CPU_RESULTS: diagnostic; drop postprocess/IPC/tracking.
    bool skip_cpu_results = false;
    // ORANGE_ANALYTICS_DEVICE_ROI: select the crop origin on the device from
    // the EfficientNMS outputs (src/detect_roi.h) right after the detect
    // graph, and compare it against the CPU crop origin on every frame
    // (device_roi_valid / device_roi_match perf columns). Default off.
    bool device_roi = false;
    // ORANGE_ANALYTICS_DEVICE_CROP: with device_roi, cut the pose crop from
    // the device ROI and queue pose right behind the detect graph on the
    // YOLO thread (no crop-thread hop; step 1 of the fused-graph plan).
    // The crop producer keeps feeding the recorder and preview. Default off.
    bool device_crop = false;
    // ORANGE_ANALYTICS_COPY_AFTER_POSE: with the device crop path, queue the
    // late owned pool copy (lever 2d) on the pose stream right behind the
    // pose graph instead of on the acquisition stream at detect done, so the
    // 20 MB copy does not overlap the pose graph on the die (step 4) and no
    // cross-stream wait is needed. The recorder and display see the owned
    // frame about one pose later. Default on; off restores the copy at
    // detect done for an A/B.
    bool copy_after_pose = true;
    // ORANGE_ANALYTICS_FUSED_FRAME: with the device crop path, capture one
    // CUDA graph per pose slot covering preprocess, detect, ROI, crop, pose,
    // the output copies and the pool copy (step 3). Default off.
    bool fused_frame = false;
    // ORANGE_YOLO_STREAM_PRIORITY: "high" (default), "low", or an integer.
    std::string stream_priority = "high";
    // ORANGE_YOLO_STREAM_NONBLOCKING: create the YOLO stream non-blocking.
    bool stream_nonblocking = false;
    // ORANGE_YOLO_GPU_TIMING: record CUDA events around the ingress wait,
    // preprocess, and the TensorRT graph on the YOLO stream, and around the
    // early-owned copy on the acquisition stream, and report the elapsed
    // GPU times per frame (pre_ms, gap_ms, infer_ms, early_copy_ms). Default
    // on since 2026-09-04 (lever 2d, measured first): the elapsed reads
    // happen after the completion wait the worker already does, so the cost
    // is six event records per frame.
    bool gpu_timing = true;
    // ORANGE_YOLO_PERF_LOG / ORANGE_YOLO_PERF_SAMPLE.
    bool perf_log = false;
    int perf_sample = 0;
};

inline ResolvedFlags Resolve()
{
    ResolvedFlags flags;
    flags.sync_event = EnvFlag("ORANGE_YOLO_SYNC_EVENT", true);
    flags.detach_input = EnvFlag("ORANGE_YOLO_DETACH_INPUT", true);
    flags.ready_event_fast_path = EnvFlag("ORANGE_YOLO_READY_EVENT_FASTPATH", true);
    flags.inline_crop_producer = EnvFlag("ORANGE_INLINE_CROP_PRODUCER", false);
    flags.skip_cpu_results = EnvFlag("ORANGE_YOLO_SKIP_CPU_RESULTS", false);
    flags.device_roi = EnvFlag("ORANGE_ANALYTICS_DEVICE_ROI", false);
    flags.device_crop = EnvFlag("ORANGE_ANALYTICS_DEVICE_CROP", false);
    flags.copy_after_pose = EnvFlag("ORANGE_ANALYTICS_COPY_AFTER_POSE", true);
    flags.fused_frame = EnvFlag("ORANGE_ANALYTICS_FUSED_FRAME", false);
    if (const char* env = std::getenv("ORANGE_YOLO_STREAM_PRIORITY"); env && *env) {
        flags.stream_priority = env;
    }
    flags.stream_nonblocking = EnvFlag("ORANGE_YOLO_STREAM_NONBLOCKING", false);
    flags.gpu_timing = EnvFlag("ORANGE_YOLO_GPU_TIMING", true);
    flags.perf_log = EnvFlag("ORANGE_YOLO_PERF_LOG", false);
    if (const char* env = std::getenv("ORANGE_YOLO_PERF_SAMPLE"); env && *env) {
        flags.perf_sample = std::atoi(env);
    }
    return flags;
}

// Short label written into every Cam*_yolo_perf.csv row.
inline const char* SyncModeLabel(bool sync_event)
{
    return sync_event ? "event" : "poll";
}

}  // namespace orange::yolo_flags
