#pragma once

#include "json.hpp"

#include <map>
#include <string>

namespace orange::session::spatial_roi {

inline constexpr const char* kSpatialRoiRecorderContractSchemaId =
    "orange.spatial_roi_recording.external_recorder_contract";
inline constexpr int kSpatialRoiRecorderContractSchemaVersion = 1;

// Runtime placement is deliberately separate from the verified plan. The
// plan binds camera/ROI geometry and identity; this mapping binds the live
// process/GPU placement used for the recorder handoff.
struct SpatialRoiRecorderRuntimeGpuMapping {
    // Exactly one entry is required for every camera_serial in the verified
    // plan. Values must be nonnegative CUDA device IDs.
    std::map<std::string, int> analytics_gpu_by_camera_serial;

    // Exactly one entry is required for every logical_stream_id in the
    // verified plan. Values must be nonnegative CUDA device IDs.
    std::map<std::string, int> recorder_gpu_by_logical_stream_id;
};

// Build the strict, non-rolling spatial-ROI recorder contract from a verified
// plan. The plan is verified again (including its canonical digest) before any
// contract is emitted. The output contains one stream for every verified ROI
// and no caller-supplied stream identity is accepted. This schema is the
// versioned input to the forthcoming ROI-aware supervisor/protocol; it must
// not be relabeled as the existing orange.external_recorder.contract or fed to
// that v1 parser before the consumer extension lands.
//
// recording_root must be an absolute, non-root path. It is not created or
// otherwise touched by this function. GPU mappings must exactly cover the
// plan's cameras and logical ROI streams; missing, extra, negative, or
// mismatched entries fail closed.
bool build_spatial_roi_recorder_contract(
    const nlohmann::json& verified_plan,
    const std::string& recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& gpu_mapping,
    nlohmann::json* contract_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
