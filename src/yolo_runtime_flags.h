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
    // instead of polling cudaStreamQuery with usleep(100).
    bool sync_event = false;
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
    // ORANGE_YOLO_STREAM_PRIORITY: "high" (default), "low", or an integer.
    std::string stream_priority = "high";
    // ORANGE_YOLO_STREAM_NONBLOCKING: create the YOLO stream non-blocking.
    bool stream_nonblocking = false;
    // ORANGE_YOLO_PERF_LOG / ORANGE_YOLO_PERF_SAMPLE.
    bool perf_log = false;
    int perf_sample = 0;
};

inline ResolvedFlags Resolve()
{
    ResolvedFlags flags;
    flags.sync_event = EnvFlag("ORANGE_YOLO_SYNC_EVENT", false);
    flags.detach_input = EnvFlag("ORANGE_YOLO_DETACH_INPUT", true);
    flags.ready_event_fast_path = EnvFlag("ORANGE_YOLO_READY_EVENT_FASTPATH", true);
    flags.inline_crop_producer = EnvFlag("ORANGE_INLINE_CROP_PRODUCER", false);
    flags.skip_cpu_results = EnvFlag("ORANGE_YOLO_SKIP_CPU_RESULTS", false);
    if (const char* env = std::getenv("ORANGE_YOLO_STREAM_PRIORITY"); env && *env) {
        flags.stream_priority = env;
    }
    flags.stream_nonblocking = EnvFlag("ORANGE_YOLO_STREAM_NONBLOCKING", false);
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
