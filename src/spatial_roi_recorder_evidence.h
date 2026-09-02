#pragma once

#include "json.hpp"
#include "session/spatial_roi_recorder_contract.h"
#include "spatial_roi_frame_contract.h"
#include "spatial_roi_lossless_encoder.h"
#include "spatial_roi_recorder_artifact_root.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orange::spatial_roi::recording {

struct SpatialRoiRecorderEvidenceMetadataDigest;
class SpatialRoiRecorderVideoSanityResult;

// This is deliberately a recorder-side artifact contract.  It is not the
// established full-frame external-recorder CSV protocol, and it is not a
// replacement for the ROI IPC-v2 wire grammar.
inline constexpr const char* kSpatialRoiRecorderEvidenceSchemaId =
    "orange.spatial_roi_recorder.evidence";
inline constexpr int kSpatialRoiRecorderEvidenceSchemaVersion = 2;
inline constexpr const char* kSpatialRoiRecorderManifestSchemaId =
    "orange.spatial_roi_recorder.finalized_manifest";
inline constexpr int kSpatialRoiRecorderManifestSchemaVersion = 2;
inline constexpr const char* kSpatialRoiRecorderCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kSpatialRoiRecorderFixedRegionKind =
    "fixed_region";

inline constexpr std::size_t kSpatialRoiRecorderEvidenceMaxPathBytes = 1024;
inline constexpr std::size_t kSpatialRoiRecorderEvidenceMaxLineBytes =
    1024 * 1024;
// Evidence has a fixed implementation ceiling as well as the stricter exact
// per-stream ceiling authenticated by contract v3. The authenticated value is
// one aggregate budget for every non-video artifact, the JSONL, and its final
// manifest; it is not independently reusable by each sidecar. Validation
// streams JSONL rather than making a second multi-gigabyte in-memory copy.
inline constexpr std::size_t kSpatialRoiRecorderEvidenceMaxFileBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
// Manifests and JSON sidecars have a separate small schema bound. Video uses
// the authenticated per-stream media ceiling instead of a universal cap.
inline constexpr std::size_t kSpatialRoiRecorderManifestMaxFileBytes =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kSpatialRoiRecorderEvidenceMaxFrames =
    4ULL * 1000ULL * 1000ULL;

// The binding is a compact, self-describing projection of the independently
// verified recorder contract and plan.  The contract/plan digests bind the
// complete source documents; the repeated fields make each evidence file
// directly inspectable without requiring those documents to be present.
struct SpatialRoiRecorderEvidenceBinding {
    std::string contract_schema_id;
    int contract_schema_version = 0;
    std::string contract_sha256;
    std::string contract_mode;

    std::string plan_schema_id;
    int plan_schema_version = 0;
    std::string plan_sha256;

    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;

    int camera_id = -1;
    std::string camera_serial;
    int analytics_gpu_id = -1;
    int source_gpu_id = -1;

    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    bool has_arena_id = false;
    std::string arena_id;
    std::string logical_stream_id;

    // These objects are copied from the closed contract stream and validated
    // as closed geometry/profile objects before the writer is armed.
    nlohmann::json geometry_identity = nlohmann::json::object();
    nlohmann::json encode_profile = nlohmann::json::object();

    int recorder_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = -1;
    std::string routing_policy;

    // These are copied only from the authoritative contract parser view.  The
    // writer root and every output path must match this map exactly; callers
    // cannot substitute a different path during finalization.
    std::string recording_root;
    std::string artifact_root;
    std::map<std::string, std::string> expected_artifacts;

    // Exact per-stream admission ceilings copied from the authoritative v3
    // contract parser. They bound recorder allocation/publication; they are
    // not caller estimates derived from duration or compression. The evidence
    // ceiling is aggregate across all non-video products for this stream.
    std::uint64_t max_frames_per_stream = 0;
    std::uint64_t max_media_bytes_per_stream = 0;
    std::uint64_t max_evidence_bytes_per_stream = 0;

};

// Rebuild one binding from the exact strict contract and its independently
// verified plan.  The plan digest is recomputed from plan.plan and the
// contract is checked for the active IPC-v2 feature set.  There is no legacy
// parser fallback and no selector fallback: exactly logical_stream_id must
// exist in the contract.
bool make_spatial_roi_recorder_evidence_binding(
    const nlohmann::json& contract,
    const nlohmann::json& verified_plan,
    const std::string& expected_recording_root,
    const orange::session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping&
        expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderEvidenceBinding* binding_out,
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_recorder_evidence_binding_to_json(
    const SpatialRoiRecorderEvidenceBinding& binding);

bool validate_spatial_roi_recorder_evidence_binding(
    const SpatialRoiRecorderEvidenceBinding& binding,
    std::string* error_out = nullptr);

struct SpatialRoiRecorderFrameEvidence {
    SpatialRoiFrameDescriptor frame;

    // `detach_status` is the recorder detach result. A status other than
    // "detached" is retained as evidence and makes the frame unsuccessful.
    // Empty until the detach seam records an explicit outcome.  A default
    // construction must never imply successful ownership transfer.
    std::string detach_status;
    bool source_release_safe = false;

    // Dispatch admission is recorder-internal truth and is never inferred from
    // ACK state. An admitted frame can have an ACK write failure.
    bool dispatch_admitted = false;
    std::string dispatch_reason;

    // ACK fields describe both the local attempt and the payload. The accepted
    // bit is retained even if the attempted write fails.
    bool ack_attempted = false;
    bool ack_sent = false;
    bool ack_accepted = false;
    std::string ack_reason;
    std::string ack_error;

    // A successful WriteLine is release_sent; no peer receipt/success bit is
    // inferable by the recorder. release_reason is the exact wire reason.
    bool release_attempted = false;
    bool release_sent = false;
    std::string release_reason;
    std::string release_error;

    // encoded is the only successful encode state.  failed and not_attempted
    // retain terminal/nonterminal recorder outcomes without inventing packets.
    std::string encode_status = "not_attempted";
    std::uint64_t output_frame_index = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t encoded_bytes = 0;
    bool keyframe = false;
};

struct SpatialRoiRecorderArtifactInput {
    std::string kind;
    std::string relative_path;
};

struct SpatialRoiRecorderFinalizeRequest {
    // complete requires successful evidence plus the contract's video,
    // keyframes, and finalization sidecars; failed may describe a recorder
    // that stopped before any media output.
    std::string terminal_state = "complete";
    std::string terminal_reason;
    // The caller-controlled packet-write boolean formerly present here was
    // intentionally removed: it was not authoritative provenance. Complete
    // finalization consumes the exact immutable snapshot returned by the
    // encoder only after its Finalize has joined both owner/writer threads.
    std::shared_ptr<const orange::spatial_roi::encoder::
                        SpatialRoiLosslessEncoderTerminalSnapshot>
        encoder_terminal_snapshot;
    // Complete first publication additionally requires the exact
    // descriptor-retaining result minted by the bounded decoder probe.  A
    // caller-authored JSON sidecar is never sufficient to certify media.
    // This capability is consumed synchronously and its fields are persisted
    // in the committed video-sanity sidecar; it is not part of request hash
    // canonicalization because the pointer itself has no wire identity.
    std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>
        video_sanity_result;
    std::vector<SpatialRoiRecorderArtifactInput> artifacts;
};

struct SpatialRoiRecorderEvidenceWriterConfig {
    // Retained descriptor-relative authority created from the authoritative
    // recorder contract. The writer never reopens binding paths and never
    // accepts a caller path as output authority.
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root;
    std::string evidence_relative_path;
    std::string manifest_relative_path;
    // Must equal the authenticated per-stream frame admission in `binding`.
    std::size_t max_frames = kSpatialRoiRecorderEvidenceMaxFrames;
};

// Host-side fixed-region evidence writer.  It writes a header and one closed
// JSON object per frame to a staging file, then appends a terminal record and
// publishes both JSONL and the finalized manifest with fsync plus no-replace
// atomic publication.  It never creates or updates legacy CSV artifacts.
class SpatialRoiRecorderEvidenceWriter final {
public:
    static bool Open(
        SpatialRoiRecorderEvidenceWriterConfig config,
        SpatialRoiRecorderEvidenceBinding binding,
        std::unique_ptr<SpatialRoiRecorderEvidenceWriter>* writer_out,
        std::string* error_out = nullptr);

    ~SpatialRoiRecorderEvidenceWriter();

    SpatialRoiRecorderEvidenceWriter(
        const SpatialRoiRecorderEvidenceWriter&) = delete;
    SpatialRoiRecorderEvidenceWriter& operator=(
        const SpatialRoiRecorderEvidenceWriter&) = delete;

    bool AppendFrame(const SpatialRoiRecorderFrameEvidence& frame,
                     std::string* error_out = nullptr);

    bool Finalize(const SpatialRoiRecorderFinalizeRequest& request,
                  nlohmann::json* manifest_out = nullptr,
                  std::string* error_out = nullptr);

    bool finalized() const noexcept { return finalized_; }
    std::size_t frame_count() const noexcept { return frame_count_; }
    const std::string& error() const noexcept { return error_; }
    const SpatialRoiRecorderEvidenceBinding& binding() const noexcept
    {
        return binding_;
    }

private:
    SpatialRoiRecorderEvidenceWriter() = default;

    bool append_json_line(const nlohmann::json& value,
                          std::string* error_out);
    bool latch_failure(std::string message, std::string* error_out);

    SpatialRoiRecorderEvidenceWriterConfig config_;
    SpatialRoiRecorderEvidenceBinding binding_;
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root_;
    int root_fd_ = -1;
    int staging_dir_fd_ = -1;
    std::string staging_name_;
    int staging_fd_ = -1;
    std::string evidence_leaf_;
    std::string manifest_leaf_;
    std::uint64_t last_recording_frame_id_ = 0;
    std::uint64_t last_roi_stream_frame_index_ = 0;
    std::uint64_t last_output_frame_index_ = 0;
    std::uint64_t first_recording_frame_id_ = 0;
    std::uint64_t first_roi_stream_frame_index_ = 0;
    std::size_t frame_count_ = 0;
    bool has_frames_ = false;
    bool fatal_ = false;
    bool finalized_ = false;
    std::uint64_t detach_successes_ = 0;
    std::uint64_t dispatch_admitted_ = 0;
    std::uint64_t dispatch_rejected_ = 0;
    std::uint64_t ack_attempted_ = 0;
    std::uint64_t ack_sent_ = 0;
    std::uint64_t ack_accepted_ = 0;
    std::uint64_t release_attempted_ = 0;
    std::uint64_t release_sent_ = 0;
    std::uint64_t encoded_frames_ = 0;
    std::uint64_t failed_frames_ = 0;
    std::uint64_t packet_count_ = 0;
    std::uint64_t encoded_bytes_ = 0;
    std::uint64_t keyframes_ = 0;
    std::uint64_t ack_write_failures_ = 0;
    std::uint64_t release_write_failures_ = 0;
    std::uint64_t lifecycle_failures_ = 0;
    std::uint64_t evidence_bytes_written_ = 0;
    // Incremental O(1)-memory digest of the exact encoder metadata CSV bytes
    // projected from admitted frame evidence. Complete finalization compares
    // it with the descriptor-hashed metadata artifact.
    std::unique_ptr<SpatialRoiRecorderEvidenceMetadataDigest> metadata_digest_;
    bool evidence_published_ = false;
    bool terminal_written_ = false;
    std::string finalized_request_digest_;
    std::string terminal_state_;
    std::string terminal_reason_;
    nlohmann::json terminal_artifact_receipts_ = nlohmann::json::object();
    nlohmann::json terminal_encoder_snapshot_ = nullptr;
    nlohmann::json evidence_reference_ = nlohmann::json::object();
    nlohmann::json finalized_manifest_ = nlohmann::json::object();
    std::string error_;
};

// Revalidate a finalized manifest and all referenced artifacts beneath the
// retained, contract-derived authority. This recomputes the receipt digest,
// JSONL evidence sequence, exact file sizes, and SHA-256 values from retained
// no-symlink file handles.
bool validate_spatial_roi_recorder_finalized_manifest(
    const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& artifact_root,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    const nlohmann::json& manifest,
    std::string* error_out = nullptr);

bool read_and_validate_spatial_roi_recorder_finalized_manifest(
    const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& artifact_root,
    const std::string& manifest_relative_path,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    nlohmann::json* manifest_out,
    std::string* error_out = nullptr);

}  // namespace orange::spatial_roi::recording
