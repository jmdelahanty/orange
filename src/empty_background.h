// Empty-dish background (docs/empty_background_capture_design.md; schemas
// docs/schemas/orange_empty_background_capture_v1.schema.json and
// orange_recording_empty_background_v1.schema.json, agreed with Palette as
// agent-contracts orange-palette/empty_background_v1).
//
// CPU only: no camera, GPU or process state. The caller hands in copied Mono8
// frames (the GUI copies them on the snapshot worker; never the acquisition
// thread) and runs these functions on a housekeeping CPU.
//
//   capture:   per camera, the exact per-pixel median of 17 consecutive
//              frames, its levels (orange.image_levels.v1), a PNG of the
//              median, then background.json written last, in
//              <store root>/<capture_id>/ (store root: <data root>/backgrounds).
//   binding:   at record start, the selected capture's descriptor and each
//              recording camera's image copied byte for byte to
//              <recording>/empty_background/, digests verified, checked
//              settings compared (a difference is copied and flagged as
//              settings_changed, never refused), and the start-snapshot block
//              session.empty_background built; every recording gets one.
#pragma once

#include "json.hpp"
#include "orange_outputs/orange_empty_background_capture_v1.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace orange::empty_background {

namespace gen = ::orange::outputs::gen::empty_background_capture_v1;

inline constexpr const char* kCaptureSchemaId = "orange.empty_background_capture";
inline constexpr int kCaptureSchemaVersion = 1;
inline constexpr std::size_t kFrameCount = 17;  // method median_17 v1; odd, so the median is a sample
inline constexpr const char* kDescriptorFileName = "background.json";
inline constexpr const char* kStoreDirectoryName = "backgrounds";            // under the Orange data root
inline constexpr const char* kRecordingDirectoryName = "empty_background";   // at the recording root
inline constexpr const char* kSnapshotSessionKey = "empty_background";       // session.empty_background
inline constexpr std::uint64_t kMaxFileBytes = 64ull * 1024 * 1024;         // schema size_bytes maximum
// Burst admission (the daily context's 64 MiB / 256 MiB limits are for one frame).
inline constexpr std::uint64_t kMaxFrameBytes = 32ull * 1024 * 1024;         // 4512 x 4512 = 19.4 MiB
inline constexpr std::uint64_t kMaxBurstBytesTotal = 1536ull * 1024 * 1024;  // 4 cameras x 17 x 4512 x 4512 = 1.3 GiB

// Camera settings as configured (CameraParams holds unsigned integers, so
// equality is exact). empty_background_camera.h fills it from CameraParams.
struct CameraConfiguration {
    std::uint64_t width = 0, height = 0, offset_x = 0, offset_y = 0;
    std::string pixel_format;
    std::uint64_t frame_rate_hz = 0, exposure_us = 0, gain = 0, focus = 0, iris = 0;
};

struct SourceFrame {
    std::uint64_t stream_frame_id = 0;  // frame_identity_v1 local counter
    std::uint64_t camera_frame_id = 0;  // 1..65535, wraps 65535 -> 1
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;
};

// ---- median and statistics ------------------------------------------------------------------

struct BurstMedian {
    std::vector<std::uint8_t> median;           // pixel_count bytes
    std::uint64_t any_frame_saturated_pixels = 0;  // 255 in at least one frame
};

// Exact per-pixel median of exactly kFrameCount frames of pixel_count bytes
// each (the 9th smallest of 17 samples; one pass over 17 sequential streams).
// Throws std::invalid_argument on a wrong frame
// count or a short frame.
BurstMedian MedianOfBurst(const std::vector<std::vector<std::uint8_t>>& frames, std::size_t pixel_count);

// orange.image_levels.v1 of a whole Mono8 image (src/image_levels.cpp rules).
::orange::outputs::gen::image_levels_v1::OrangeImageLevelsV1 ImageLevels(const std::uint8_t* pixels,
                                                                           std::size_t pixel_count);

// ---- PNG (8-bit greyscale, no alpha, no gAMA/iCCP/sRGB) -------------------------------------

std::string EncodePngGray8(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height);
// Decodes a PNG of exactly that form (any filter type, no interlace); false with
// a reason otherwise, including for gAMA, iCCP or sRGB chunks.
bool DecodePngGray8(const std::string& bytes, std::vector<std::uint8_t>* pixels, std::uint32_t* width,
                    std::uint32_t* height, std::string* error);
// Header only: signature, IHDR (8-bit greyscale, no interlace) and no
// gAMA/iCCP/sRGB chunk anywhere.
bool ReadPngGray8Header(const std::string& bytes, std::uint32_t* width, std::uint32_t* height,
                        std::string* error);

// ---- capture --------------------------------------------------------------------------------

struct CameraBurst {
    std::string camera_serial;
    CameraConfiguration configuration;
    // kFrameCount consecutive frames in stream order, width x height Mono8
    // each. Released by the writer once the median is computed.
    std::vector<SourceFrame> source_frames;
    std::vector<std::vector<std::uint8_t>> frames;
    // Instead of frames: the median already computed from them (the GUI burst
    // computes it on the landing die, src/empty_background_median.cu, so the 17
    // frames never leave the device). Exactly one of frames / median is given.
    std::optional<BurstMedian> median;
    // Non-empty: the burst did not complete; the camera is written as failed.
    std::string failure_reason;
};

struct CapturePlan {
    std::filesystem::path store_root;  // <data root>/backgrounds
    std::string capture_id;            // ^[A-Za-z0-9_-]{1,128}$, the directory name
    std::string captured_at_utc;       // YYYY-MM-DDThh:mm:ss.sssZ
    gen::OrangeEmptyBackgroundCaptureV1SceneDeclaration scene_declaration;
    std::optional<std::string> registration_id;
};

struct CaptureReceipt {
    std::filesystem::path directory;
    std::filesystem::path descriptor_path;
    std::uint64_t descriptor_size_bytes = 0;
    std::string descriptor_sha256;  // sha256:<hex>
    gen::OrangeEmptyBackgroundCaptureV1 descriptor;
};

// Writes <store_root>/<capture_id>/: each captured camera's PNG first
// (atomically, durable), then background.json last. Refuses an existing
// capture directory. A camera with a failure_reason, or whose source frames
// are not consecutive, is written as capture_status failed; the others keep
// their images. Throws std::runtime_error on a malformed plan or a write
// failure (nothing is left that claims to be a capture: no background.json).
// Files belong to the sudo invoker.
CaptureReceipt WriteCapture(const CapturePlan& plan, std::vector<CameraBurst>* bursts);

// "<YYYY_MM_DD_hh_mm_ss>" in local time with a numeric suffix when needed,
// the same style recordings use; utc_iso is the matching captured_at_utc.
std::string NewCaptureId(const std::filesystem::path& store_root, std::string* utc_iso);

// The store root: <Orange data root>/backgrounds (src/orange_data_root.h).
// Empty with *error when the data root cannot be resolved.
std::filesystem::path DefaultStoreRoot(std::string* error = nullptr);

// ---- reading a capture ----------------------------------------------------------------------

struct StoredCapture {
    std::filesystem::path descriptor_path;  // absolute
    std::string descriptor_bytes;           // exactly as on disk
    std::string descriptor_sha256;          // sha256:<hex>
    gen::OrangeEmptyBackgroundCaptureV1 descriptor;
};

// Reads and validates a background.json (schema, plus the checks JSON Schema
// cannot express: unique serials, image names that match their serial, the
// directory named after capture_id). Throws std::runtime_error naming the problem.
StoredCapture ReadCapture(const std::filesystem::path& descriptor_path);

// The capture with the newest captured_at_utc under store_root, or nullopt
// when none is readable. Unreadable directories are skipped.
std::optional<StoredCapture> NewestCapture(const std::filesystem::path& store_root);

// ---- record-start binding -------------------------------------------------------------------

struct RecordingCamera {
    std::string camera_serial;
    CameraConfiguration configuration;  // live, at record start
};

struct BindingSelection {
    bool selected = false;
    std::string selection_source;  // gui_operator | headless_spec
    // GUI: the per-recording "scene unchanged apart from the fish" confirmation.
    // Headless: naming the capture in the spec.
    bool scene_unchanged_confirmed = false;
    std::filesystem::path descriptor_path;  // absolute path of background.json in the store
};

struct BindingResult {
    nlohmann::json block;  // session.empty_background (orange_recording_empty_background_v1)
    // One line per camera that is not attached (settings_changed, missing,
    // copy failure), for the GUI and the log; empty when all attached or none selected.
    std::vector<std::string> warnings;
};

// Builds the block and, when a capture is selected, writes
// <recording>/empty_background/ (descriptor copy and the images of attached
// and settings_changed cameras, each digest-verified after the write; a
// failed copy leaves no image behind and is copy_failed). Per-camera problems
// never throw. Throws std::runtime_error, leaving nothing behind, when the selection
// itself is unusable: no recording cameras, a selected capture without the
// confirmation, a descriptor that cannot be read or validated, or a descriptor
// copy that fails (the callers refuse the start: the operator chose this capture).
BindingResult BindToRecording(const std::filesystem::path& recording_folder, const BindingSelection& selection,
                              const std::vector<RecordingCamera>& cameras);

// Headless experiment spec key fixed.empty_background_capture: the absolute
// path of a capture's background.json. Naming the capture is the
// confirmation (selection_source headless_spec). The capture is read and
// validated here, at spec load; a missing key means no background, so a
// caller only calls this when the key is present. Throws std::runtime_error.
inline constexpr const char* kHeadlessSpecKey = "empty_background_capture";
BindingSelection SelectionFromHeadlessSpec(const nlohmann::json& value);

// Writes the block into the start snapshot (production:
// update_recording_snapshot_session_artifacts). Same throw rules.
using SnapshotSessionUpdate = std::function<bool(const std::string& folder, const nlohmann::json& session)>;
BindingResult WriteRecordingBinding(const std::string& recording_folder, const BindingSelection& selection,
                                    const std::vector<RecordingCamera>& cameras,
                                    const SnapshotSessionUpdate& update_snapshot);

}  // namespace orange::empty_background
