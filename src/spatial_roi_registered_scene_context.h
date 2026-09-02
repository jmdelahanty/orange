#pragma once

#include "json.hpp"
#include "spatial_roi_session_authority_store.h"

#include <cstdint>
#include <memory>
#include <string>

namespace orange::session::spatial_roi {

// This is the first, deliberately small, recording-bound context product.
// It describes one native camera frame captured after the installed dish
// setup and explicitly declared registration authority state are stable. A
// diagnostic declaration remains explicitly non-accepted; this descriptor
// never upgrades it merely because IDs/digests are present. It is not a video
// frame stream and it does not make any claim about subject pixels outside a
// future ROI media product.
inline constexpr const char* kRegisteredSceneContextSchemaId =
    "orange.recording.registered_scene_context";
inline constexpr int kRegisteredSceneContextSchemaVersion = 1;
inline constexpr const char* kRegisteredSceneContextCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kRegisteredSceneContextCaptureRole =
    "registered_scene_context";
inline constexpr const char* kRegisteredSceneContextPixelFormat = "Mono8";

struct RegisteredSceneContextArtifact final {
    // This path is relative to the recording root. The v1 authority store
    // intentionally accepts one flat leaf so publication remains descriptor
    // authoritative and cannot follow an output-directory symlink.
    std::string relative_path;
    std::uint64_t size_bytes = 0;
    std::string sha256;
};

struct RegisteredSceneContextReference final {
    std::string id;
    std::string sha256;
};

struct RegisteredSceneContextRaster final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;
};

// The frame identities are copied from the source acquisition record. A
// context capture normally happens before recording-frame numbering starts;
// recording_frame_id is therefore explicitly zero when no recording frame was
// assigned, and is never synthesized by this module.
struct RegisteredSceneContextSourceFrame final {
    std::uint64_t source_frame_id = 0;
    std::uint64_t local_frame_id = 0;
    std::uint64_t camera_frame_id = 0;
    std::uint64_t recording_frame_id = 0;
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;
};

struct RegisteredSceneContextCaptureInvariants final {
    bool daily_registration_accepted = false;
    // Closed declaration copied from the recording-start configuration. A
    // diagnostic authority is explicit and must never be serialized as
    // accepted physical registration.
    std::string registration_authority_status;
    bool dish_setup_complete = false;
    // Declarative scene state: the registered context may intentionally be
    // captured before or after subject introduction. "unknown" is retained
    // as an explicit operator state rather than being inferred as absent.
    std::string subject_presence;
    bool nir_illumination_fixed = false;
    bool camera_configuration_fixed = false;
    bool rig_fixed = false;
};

// Closed descriptor for one context frame. The frame bytes are passed
// separately to publish_registered_scene_context() and are never embedded in
// the JSON descriptor.
struct RegisteredSceneContextDescriptor final {
    // v1 only publishes complete captures. A failed capture can still be
    // represented and validated as a terminal evidence record, but has no
    // image receipt and is never readable as context bytes.
    std::string status = "complete";
    std::string failure_reason;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;

    int camera_id = -1;
    std::string camera_serial;
    std::string source_camera_stream_id;
    std::string stream_epoch_id;
    std::string camera_configuration_sha256;

    RegisteredSceneContextSourceFrame source_frame;
    RegisteredSceneContextRaster native_raster;
    std::string coordinate_space = "camera_native_px";
    std::string pixel_format = kRegisteredSceneContextPixelFormat;

    RegisteredSceneContextReference layout;
    RegisteredSceneContextReference materialization;
    RegisteredSceneContextReference registration;

    RegisteredSceneContextCaptureInvariants invariants;
    RegisteredSceneContextArtifact artifact;
};

struct RegisteredSceneContextPublication final {
    RegisteredSceneContextDescriptor descriptor;
    SpatialRoiSessionAuthorityReceipt descriptor_receipt;
};

// Validate the complete, immutable descriptor, including the artifact
// receipt. All keys emitted by the JSON builder are required and unknown keys
// are rejected by the parser.
bool validate_registered_scene_context_descriptor(
    const RegisteredSceneContextDescriptor& descriptor,
    std::string* error_out = nullptr);

nlohmann::json registered_scene_context_descriptor_to_json(
    const RegisteredSceneContextDescriptor& descriptor);

bool registered_scene_context_descriptor_from_json(
    const nlohmann::json& value,
    RegisteredSceneContextDescriptor* descriptor_out,
    std::string* error_out = nullptr);

// Publish the exact native Mono8 bytes and then its descriptor through the
// already-open recording authority store. The input descriptor must contain a
// valid relative artifact path with size_bytes == 0 and an empty sha256; the
// returned descriptor contains the read-back receipt for those exact bytes.
// descriptor_relative_path must be a separately allow-listed authority leaf.
// No replacement is possible: identical retries succeed, while changed bytes
// or changed descriptors fail closed.
bool publish_registered_scene_context(
    const SpatialRoiSessionAuthorityStore& authority,
    const std::string& descriptor_relative_path,
    const RegisteredSceneContextDescriptor& descriptor,
    const std::string& mono8_bytes,
    RegisteredSceneContextPublication* publication_out,
    std::string* error_out = nullptr);

// Read and verify the image artifact named by a published descriptor. This
// checks the exact descriptor receipt and the descriptor's geometry-derived
// byte count before returning any bytes.
bool read_registered_scene_context_bytes(
    const SpatialRoiSessionAuthorityStore& authority,
    const RegisteredSceneContextDescriptor& descriptor,
    std::string* mono8_bytes_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
