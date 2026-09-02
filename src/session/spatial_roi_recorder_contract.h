#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace orange::session::spatial_roi {

inline constexpr const char* kSpatialRoiRecorderContractSchemaId =
    "orange.spatial_roi_recording.external_recorder_contract";
inline constexpr int kLegacySpatialRoiRecorderContractSchemaVersion = 4;
inline constexpr int kSpatialRoiRecorderContractSchemaVersion = 5;
inline constexpr const char* kLegacySpatialRoiRecorderContractScope =
    "strict_spatial_roi_external_recorder_v4";
inline constexpr const char* kSpatialRoiRecorderContractScope =
    "strict_spatial_roi_external_recorder_v5";
inline constexpr const char* kLegacySpatialRoiRecorderContractMode =
    "spatial_roi_external_recorder_v4";
inline constexpr const char* kSpatialRoiRecorderContractMode =
    "spatial_roi_external_recorder_v5";

// Storage admission is a separate closed policy schema.  The recorder must
// authenticate these values from the contract before it performs its
// descriptor-bound live filesystem query; there is no zero-reserve fallback.
inline constexpr const char* kSpatialRoiRecorderStoragePreflightPolicySchemaId =
    "orange.spatial_roi_recorder_storage_preflight_policy";
inline constexpr int kSpatialRoiRecorderStoragePreflightPolicySchemaVersion = 1;
inline constexpr const char* kSpatialRoiRecorderStoragePreflightSchemaId =
    "orange.spatial_roi_recording.storage_preflight";
inline constexpr int kSpatialRoiRecorderStoragePreflightSchemaVersion = 1;
inline constexpr std::uint64_t kSpatialRoiRecorderReservedFreeBytes =
    500000000000ULL;

// Closed recorder-side construction policy.  These are authenticated values,
// not parser fallbacks: every emitted stream repeats the applicable values and
// the aggregate contract binds their checked totals.  Keep these policy
// values within SpatialRoiLosslessEncoder's host-side validation ceilings.
inline constexpr std::uint64_t kSpatialRoiRecorderWriterQueueMaxPackets = 512;
inline constexpr std::uint64_t kSpatialRoiRecorderWriterQueueMaxBytes =
    128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kSpatialRoiRecorderOperationTimeoutMs = 2000;

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
