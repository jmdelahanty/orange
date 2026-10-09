#pragma once

// Shared conventions of the per-frame JSONL event logs (orange.yolo_event and
// orange.pose_event, version 2, 2026-10-09).
//
// Version 2 moves the session-constant blocks (recording id, camera, model
// identity, spatial-mask policy, IPC queue, source raster, skeleton labels)
// into one `session_header` line at the top of each file and rounds the
// per-frame numbers: pixel coordinates to 0.001 px, confidences to 1e-4,
// latencies to 1 us. Frame lines keep schema_id / schema_version /
// event_kind / event_sequence / camera_serial so a line out of context is
// still identifiable; everything else about the session is read from the
// header (or, for the spatial mask, the most recent `spatial_mask_policy`
// line whose policy_generation the frame line names).

#include "json.hpp"

#include <cmath>

namespace event_log_format {

inline constexpr const char* kYoloEventSchemaId = "orange.yolo_event";
inline constexpr int kYoloEventSchemaVersion = 2;
inline constexpr const char* kPoseEventSchemaId = "orange.pose_event";
inline constexpr int kPoseEventSchemaVersion = 2;

inline constexpr int kCoordinateDecimals = 3;
inline constexpr int kConfidenceDecimals = 4;
inline constexpr int kLatencyDecimals = 3;

inline double round_to(double value, int decimals)
{
    if (!std::isfinite(value)) {
        return value;
    }
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i) scale *= 10.0;
    return std::round(value * scale) / scale;
}

inline double round_px(double value) { return round_to(value, kCoordinateDecimals); }
inline double round_confidence(double value) { return round_to(value, kConfidenceDecimals); }
inline double round_ms(double value) { return round_to(value, kLatencyDecimals); }

inline nlohmann::json line_format_json()
{
    return {
        {"coordinate_decimals", kCoordinateDecimals},
        {"confidence_decimals", kConfidenceDecimals},
        {"latency_decimals", kLatencyDecimals}
    };
}

}  // namespace event_log_format
