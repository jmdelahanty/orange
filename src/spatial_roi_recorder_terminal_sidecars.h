#pragma once

#include "spatial_roi_recorder_artifact_root.h"
#include "spatial_roi_recorder_evidence.h"
#include "spatial_roi_recorder_video_sanity.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace orange::spatial_roi::recording {

inline constexpr const char* kSpatialRoiRecorderPerfSchemaId =
    "orange.spatial_roi_recorder.perf";
inline constexpr const char* kSpatialRoiRecorderSummarySchemaId =
    "orange.spatial_roi_recorder.summary";
inline constexpr const char* kSpatialRoiRecorderStatusSchemaId =
    "orange.spatial_roi_recorder.status";
inline constexpr const char* kSpatialRoiRecorderVideoSanitySchemaId =
    "orange.spatial_roi_recorder.video_sanity";
inline constexpr const char* kSpatialRoiRecorderLogSchemaId =
    "orange.spatial_roi_recorder.log";
inline constexpr const char* kSpatialRoiRecorderTransportSchemaId =
    "orange.spatial_roi_recorder.transport";
inline constexpr int kSpatialRoiRecorderTerminalCandidateSchemaVersion = 1;
inline constexpr const char* kSpatialRoiRecorderPendingManifestState =
    "pending_manifest";

struct SpatialRoiRecorderTerminalCandidateSidecarConfig {
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root;

    // Exact projection produced from the independently verified plan and
    // recorder contract. It supplies the closed twelve-artifact allow-list,
    // stream identity, geometry, and authenticated limits.
    const SpatialRoiRecorderEvidenceBinding* binding = nullptr;

    // Non-forgeable result returned by the descriptor-bound decoder probe.
    // Root/inode/path/hash/dimensions/cardinality are checked before creating
    // any sidecar.
    const SpatialRoiRecorderVideoSanityResult* video_sanity = nullptr;
};

struct SpatialRoiRecorderTerminalCandidateSidecarResult {
    // Exact kind -> contract-relative path entries to pass to recorder
    // evidence finalization alongside the four encoder-owned artifacts.
    std::map<std::string, std::string> artifacts;
    std::uint64_t candidate_bytes = 0;
};

// Create, fsync, binding-check, and seal six recorder-owned terminal candidate
// sidecars. They deliberately remain non-certifying and identify the finalized
// evidence manifest as their required commit marker. Files are O_EXCL
// contract-authorized artifacts; any partial set is non-resumable residue.
// Aggregate non-video budget certification remains solely in evidence
// finalization, which measures every sidecar plus evidence JSONL/manifest.
bool write_spatial_roi_recorder_terminal_candidate_sidecars(
    const SpatialRoiRecorderTerminalCandidateSidecarConfig& config,
    SpatialRoiRecorderTerminalCandidateSidecarResult* result_out,
    std::string* error_out = nullptr);

}  // namespace orange::spatial_roi::recording
