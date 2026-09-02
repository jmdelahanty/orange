#pragma once

#include "json.hpp"

#include <map>
#include <string>

namespace orange::session::spatial_roi {

inline constexpr const char* kRecorderRuntimeConfigSchemaId =
    "orange.spatial_roi_recording.recorder_runtime";
inline constexpr int kRecorderRuntimeConfigSchemaVersion = 1;
inline constexpr const char* kRecorderRuntimeModeExplicitPerStream =
    "explicit_per_stream";
inline constexpr int kRecorderRuntimeMaxGpuId = 255;

// Host-local recorder placement is deliberately separate from the immutable
// spatial ROI recording config/plan. The consuming headless runtime must
// compare this map with the selected plan and require exactly one entry for
// every admitted logical stream before arming a recorder.
struct RecorderRuntimeConfig {
    std::string mode = kRecorderRuntimeModeExplicitPerStream;
    std::map<std::string, int> recorder_gpu_by_logical_stream_id;
};

// Validate the closed schema-v1 value independent of any particular recording
// plan. Cross-checking exact stream coverage belongs to the plan consumer.
bool validate_recorder_runtime_config(
    const RecorderRuntimeConfig& config,
    std::string* error_out = nullptr);

// Parse exactly the four schema-v1 fields. Unknown/missing fields, unsafe
// logical stream IDs, empty placement maps, and GPU IDs outside [0,255] fail
// closed. The destination is not modified on failure.
bool parse_recorder_runtime_config(
    const nlohmann::json& value,
    RecorderRuntimeConfig* config_out,
    std::string* error_out = nullptr);

// Serialize only a valid runtime config. The destination is not modified on
// failure.
bool serialize_recorder_runtime_config(
    const RecorderRuntimeConfig& config,
    nlohmann::json* value_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
