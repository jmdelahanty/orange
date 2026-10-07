#pragma once

// External crop recorder outputs in the recording_session manifest.
//
// The external crop recorder writes one summary per camera
// (Cam<serial>_crop_external_summary.json). In a rolling-clips recording its
// rolling_output.clips[] describe one crop MP4 per clip under
// <artifact_root>/clips/clip_NNNNNN/, while the crop worker's per-frame
// sidecars (Cam<serial>_crop_meta.csv, Cam<serial>_crop_perf.csv) stay
// session-global in the recording folder. The helpers here turn that into
// what the manifest consumers (Palette intake, scripts/recording_output_
// validation.py) read:
//   * one clip record per crop clip, with the session sidecars split into
//     per-clip CSVs inside the clip directory (build_crop_rolling_clip_records);
//   * a `crop` RecordingOutputDescriptor attached to every full-frame rolling
//     clip (attach_crop_rolling_outputs_to_clips);
//   * for the headless client, the whole recording_backend.crop_recording
//     block plus the session-level crop output descriptors, built from the
//     materialized external_crop_recorder_contract.json and the summaries
//     (build_external_crop_recording_backend).
// The GUI finalizer (src/gui/recording_finalizer.cpp) and the headless client
// (src/orange_headless_client.cpp) share the first two so both producers
// declare the crop clips identically.

#include "session/crop_rolling_sidecars.h"
#include "session/recording_session.h"
#include "json.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace orange::session {

// metadata_backend labels for the per-clip crop CSV sidecars written by the
// split below, by producer.
inline constexpr const char* kGuiSplitCropCsvMetadataBackend =
    "orange_gui_split_crop_csv";
inline constexpr const char* kHeadlessSplitCropCsvMetadataBackend =
    "orange_headless_split_crop_csv";

// Appends `message` to `target` ("; "-joined) unless it is empty or already
// contained. Same semantics as the GUI finalizer's append_error_message.
void append_unique_error_message(std::string& target, const std::string& message);

// "clip_000012" style identifier used by the external recorders.
std::string external_recorder_clip_id(int clip_index);

// Encoded frame dimensions an external recorder summary reports
// (video_metadata.encoder.output_width/height, else
// encoding_budget.geometry.width_px/height_px). Returns false and leaves the
// outputs untouched when the summary carries neither.
bool summary_encoded_dimensions(const nlohmann::json& summary, int* width_out, int* height_out);

// recording_backend.crop_recording.rollover (and the per-stream copy).
nlohmann::json build_crop_rollover_json(
    const RecordingControlConfig& recording_control,
    const std::string& status);

// What one camera's external crop stream contributes to its clip records
// beyond the recorder summary's rolling_output.clips[] entries.
struct CropRollingClipStream {
    std::string camera_serial;
    std::string stream_id;
    std::string summary_json;
    int crop_size_px = 0;    // resolved (sanitized) crop raster size
    int frame_rate = 0;      // stream encode_fps
    std::string codec;
    std::string tuning;
};

struct CropRollingClipRecordsResult {
    bool ok = true;
    // One record per valid clip (fields read by attach_crop_rolling_outputs_
    // to_clips), each with metadata_rows / perf_rows added after the split.
    // Empty when no clip was valid.
    nlohmann::json clip_records = nlohmann::json::array();
    // Per-clip CSVs written by the split, metadata ranges first then perf.
    std::vector<std::string> split_csv_paths;
    // "external crop recorder rolling clip incomplete for camera X" when at
    // least one summary clip was skipped; empty otherwise.
    std::string incomplete_clip_error;
    // Crop perf rows that matched no clip range (frames dropped before
    // external submission); reported, never a failure.
    RecordingFrameCsvSplitStats perf_split_stats;
};

// Builds the clip records for one camera from the recorder summary's
// rolling_output.clips[] and splits the session crop sidecars in
// `recording_folder` into Cam<serial>_crop_meta.csv / _crop_perf.csv inside
// every clip directory. Every failure is reported through `report_error`
// (in the order the GUI finalizer reported them) and clears `ok`.
CropRollingClipRecordsResult build_crop_rolling_clip_records(
    const nlohmann::json& summary_clips,
    const CropRollingClipStream& stream,
    const std::string& recording_folder,
    const std::function<void(const std::string&)>& report_error);

// Turns recording_backend.crop_recording.rolling_clips ({serial: [clip
// records]}) into a `crop` RecordingOutputDescriptor on the matching
// full-frame clip in `clips_by_index`. Returns false (and appends to
// `error_out`) when a crop clip has no full-frame clip with its clip_index;
// the other clips are still attached.
bool attach_crop_rolling_outputs_to_clips(
    const nlohmann::json& recording_backend,
    std::map<int, RollingClipManifestOptions>* clips_by_index,
    std::string* error_out,
    const std::string& metadata_backend = kGuiSplitCropCsvMetadataBackend);

// Headless: recording_backend.crop_recording and the session-level crop
// outputs from the materialized contract and the recorder summaries.
struct ExternalCropRecordingBackendInputs {
    std::string recording_folder;
    // <recording_folder>/external_crop_recorder_contract.json
    std::string contract_path;
    // Fallback recording control when the contract carries none.
    RecordingControlConfig recording_control;
    // Resolved crop size; 0 = take the recorder summary's encoded raster.
    int crop_size_px = 0;
    // The full-frame session is rolling: a single-clip crop summary is then
    // an error (the clip outputs could not be declared).
    bool rolling_expected = false;
    // Labels for the session-level and the per-clip crop metadata.
    std::string session_metadata_backend = "orange_headless";
    std::string clip_metadata_backend = kHeadlessSplitCropCsvMetadataBackend;
};

struct ExternalCropRecordingBackendResult {
    bool ok = true;
    bool rolling_requested = false;
    std::string error;
    std::vector<std::string> warnings;
    nlohmann::json crop_recording = nlohmann::json::object();
    // One per crop stream: scope session_aggregate (rolling) or single_clip.
    std::vector<RecordingOutputDescriptor> session_outputs;
};

ExternalCropRecordingBackendResult build_external_crop_recording_backend(
    const ExternalCropRecordingBackendInputs& inputs);

}  // namespace orange::session
