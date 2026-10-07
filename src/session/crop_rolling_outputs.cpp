#include "session/crop_rolling_outputs.h"

#include "external_recorder_contract_utils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace orange::session {

namespace {

std::string json_string_or(const nlohmann::json& object,
                           const char* key,
                           const std::string& fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            const std::string value = it->get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
    }
    return fallback;
}

uint64_t json_u64_or(const nlohmann::json& object,
                     const char* key,
                     const uint64_t fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_number_unsigned()) {
            return it->get<uint64_t>();
        }
        if (it != object.end() && it->is_number_integer()) {
            const int64_t value = it->get<int64_t>();
            return value > 0 ? static_cast<uint64_t>(value) : 0;
        }
    }
    return fallback;
}

double json_double_or(const nlohmann::json& object,
                      const char* key,
                      const double fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_number()) {
            return it->get<double>();
        }
    }
    return fallback;
}

int json_int_or(const nlohmann::json& object, const char* key, const int fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_number_integer()) {
            return it->get<int>();
        }
    }
    return fallback;
}

bool read_json_file(const std::string& path, nlohmann::json* out, std::string* error_out)
{
    std::ifstream input(path);
    if (!input) {
        if (error_out) {
            *error_out = "failed to open " + path;
        }
        return false;
    }
    try {
        input >> *out;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "failed to parse " + path + ": " + ex.what();
        }
        return false;
    }
    return true;
}

bool has_crop_suffix(const std::string& value)
{
    static const std::string suffix = "_crop";
    return value.size() > suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string strip_crop_suffix(std::string value)
{
    if (has_crop_suffix(value)) {
        value.resize(value.size() - 5);
    }
    return value;
}

bool contract_stream_is_crop(const nlohmann::json& stream, const std::string& key)
{
    return stream.value("output_kind", std::string()) == "crop" ||
           stream.value("stream_kind", std::string()) == "crop" ||
           has_crop_suffix(stream.value("stream_id", std::string())) ||
           has_crop_suffix(stream.value("camera_serial", std::string())) ||
           has_crop_suffix(key);
}

std::string contract_stream_camera_serial(const nlohmann::json& stream, const std::string& key)
{
    const std::string serial = stream.value("camera_serial", std::string());
    if (!serial.empty()) {
        return strip_crop_suffix(serial);
    }
    const std::string stream_id = stream.value("stream_id", std::string());
    if (!stream_id.empty()) {
        return strip_crop_suffix(stream_id);
    }
    return strip_crop_suffix(key);
}

// Encoded crop raster the recorder reports, for runs whose spec left the
// crop size to the camera configuration.
int summary_crop_size_px(const nlohmann::json& summary)
{
    const nlohmann::json encoder =
        summary.value("video_metadata", nlohmann::json::object())
            .value("encoder", nlohmann::json::object());
    const int output_width = json_int_or(encoder, "output_width", 0);
    if (output_width > 0) {
        return output_width;
    }
    const nlohmann::json geometry =
        summary.value("encoding_budget", nlohmann::json::object())
            .value("geometry", nlohmann::json::object());
    return json_int_or(geometry, "width_px", 0);
}

}  // namespace

void append_unique_error_message(std::string& target, const std::string& message)
{
    if (message.empty()) {
        return;
    }
    if (target.find(message) != std::string::npos) {
        return;
    }
    if (!target.empty()) {
        target += "; ";
    }
    target += message;
}

std::string external_recorder_clip_id(const int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

nlohmann::json build_crop_rollover_json(
    const RecordingControlConfig& recording_control,
    const std::string& status)
{
    if (recording_control.clip_seconds > 0) {
        return {
            {"requested", true},
            {"status", status.empty() ? "completed" : status},
            {"implementation",
             orange::external_recorder::kExternalRecorderRollingImplementation},
            {"seamless_writer_switch", true},
            {"records_during_rollover", true},
            {"boundary", "recording_frame_id"},
            {"output_kind", "crop"},
            {"supported_mode", "rolling_clips"},
            {"rolling_supported", true},
            {"next_writer_preopened", false}
        };
    }
    return {
        {"requested", false},
        {"status", "not_requested"},
        {"implementation", "none"},
        {"seamless_writer_switch", false},
        {"records_during_rollover", false},
        {"output_kind", "crop"},
        {"supported_mode", "single_clip"},
        {"rolling_supported", true}
    };
}

CropRollingClipRecordsResult build_crop_rolling_clip_records(
    const nlohmann::json& summary_clips,
    const CropRollingClipStream& stream,
    const std::string& recording_folder,
    const std::function<void(const std::string&)>& report_error)
{
    CropRollingClipRecordsResult result;
    auto fail = [&](const std::string& message) {
        result.ok = false;
        if (report_error) {
            report_error(message);
        }
    };
    const std::string& serial = stream.camera_serial;

    std::vector<RecordingFrameCsvRange> metadata_ranges;
    std::vector<RecordingFrameCsvRange> perf_ranges;
    std::vector<nlohmann::json> clip_records;
    if (summary_clips.is_array()) {
        metadata_ranges.reserve(summary_clips.size());
        perf_ranges.reserve(summary_clips.size());
        clip_records.reserve(summary_clips.size());
    }
    for (const nlohmann::json& clip : summary_clips) {
        if (!clip.is_object()) {
            continue;
        }
        const int clip_index = clip.value("clip_index", -1);
        const uint64_t frame_count = json_u64_or(clip, "frame_count", 0ULL);
        const uint64_t first_frame = json_u64_or(clip, "first_recording_frame_id", 0ULL);
        const uint64_t last_frame = json_u64_or(clip, "last_recording_frame_id", 0ULL);
        const uint64_t packets = json_u64_or(clip, "packets_written", 0ULL);
        const std::string clip_mp4 = json_string_or(clip, "mp4", std::string());
        const std::string clip_keyframes = json_string_or(clip, "keyframes", std::string());
        std::filesystem::path clip_dir = clip.value("directory", std::string());
        if (clip_dir.empty() && !clip_mp4.empty()) {
            clip_dir = std::filesystem::path(clip_mp4).parent_path();
        }
        const std::string clip_metadata =
            (clip_dir / ("Cam" + serial + "_crop_meta.csv")).string();
        const std::string clip_perf =
            (clip_dir / ("Cam" + serial + "_crop_perf.csv")).string();
        if (clip_index < 0 || frame_count == 0 ||
            first_frame == 0 || last_frame < first_frame ||
            clip_mp4.empty() || clip_keyframes.empty() ||
            clip_dir.empty()) {
            result.incomplete_clip_error =
                "external crop recorder rolling clip incomplete for camera " + serial;
            fail(result.incomplete_clip_error);
            continue;
        }

        metadata_ranges.push_back({first_frame, last_frame, clip_metadata, 0});
        perf_ranges.push_back({first_frame, last_frame, clip_perf, 0});
        clip_records.push_back({
            {"clip_index", clip_index},
            {"clip_id", clip.value("clip_id", external_recorder_clip_id(clip_index))},
            {"status", clip.value("failed", false) ? "incomplete" : "completed"},
            {"stream_id", stream.stream_id},
            {"video", clip_mp4},
            {"metadata", clip_metadata},
            {"perf", clip_perf},
            {"keyframes", clip_keyframes},
            {"summary", stream.summary_json},
            {"frame_count", frame_count},
            {"first_recording_frame_id", first_frame},
            {"last_recording_frame_id", last_frame},
            {"recording_frame_id_gaps", 0},
            {"packet_count", packets},
            {"packet_count_source", "external_crop_recorder_summary.packets_written"},
            {"width", stream.crop_size_px},
            {"height", stream.crop_size_px},
            {"frame_rate", stream.frame_rate},
            {"codec", stream.codec},
            {"container", "mp4"},
            {"tuning", stream.tuning},
            {"encoding_budget", clip.value("encoding_budget", nlohmann::json::object())}
        });
    }

    if (metadata_ranges.empty()) {
        return result;
    }

    std::string split_error;
    const std::string root_metadata =
        (std::filesystem::path(recording_folder) /
         ("Cam" + serial + "_crop_meta.csv")).string();
    // Crop metadata rows exist only for frames accepted for encoding, so a
    // row outside every clip range is a hole in the recorded data: fail
    // loudly.
    if (!split_recording_frame_csv_by_ranges(
            root_metadata,
            &metadata_ranges,
            &split_error,
            RecordingFrameCsvOrphanRowPolicy::kFail)) {
        fail("failed to split crop metadata for camera " + serial + ": " + split_error);
    } else {
        for (const auto& range : metadata_ranges) {
            result.split_csv_paths.push_back(range.output_path);
        }
    }
    split_error.clear();
    const std::string root_perf =
        (std::filesystem::path(recording_folder) /
         ("Cam" + serial + "_crop_perf.csv")).string();
    // Crop perf rows are also written for frames dropped before external
    // submission (drop_reason set), so head/tail rows can legitimately match
    // no clip range: count and report them without failing.
    if (!split_recording_frame_csv_by_ranges(
            root_perf,
            &perf_ranges,
            &split_error,
            RecordingFrameCsvOrphanRowPolicy::kCountAndSkip,
            &result.perf_split_stats)) {
        fail("failed to split crop perf for camera " + serial + ": " + split_error);
    } else {
        for (const auto& range : perf_ranges) {
            result.split_csv_paths.push_back(range.output_path);
        }
    }

    nlohmann::json stream_clips = nlohmann::json::array();
    for (size_t i = 0; i < clip_records.size(); ++i) {
        const uint64_t frame_count = clip_records[i].value("frame_count", 0ULL);
        clip_records[i]["metadata_rows"] =
            i < metadata_ranges.size() ? metadata_ranges[i].rows_written : 0ULL;
        clip_records[i]["perf_rows"] =
            i < perf_ranges.size() ? perf_ranges[i].rows_written : 0ULL;
        if (clip_records[i].value("metadata_rows", 0ULL) != frame_count ||
            clip_records[i].value("perf_rows", 0ULL) != frame_count) {
            fail("external crop rolling sidecar row mismatch for camera " + serial);
        }
        stream_clips.push_back(clip_records[i]);
    }
    result.clip_records = std::move(stream_clips);
    return result;
}

bool attach_crop_rolling_outputs_to_clips(
    const nlohmann::json& recording_backend,
    std::map<int, RollingClipManifestOptions>* clips_by_index,
    std::string* error_out,
    const std::string& metadata_backend)
{
    if (!clips_by_index) {
        return true;
    }
    const nlohmann::json crop_recording =
        recording_backend.value("crop_recording", nlohmann::json::object());
    if (!crop_recording.is_object()) {
        return true;
    }
    const nlohmann::json rolling_clips =
        crop_recording.value("rolling_clips", nlohmann::json::object());
    if (!rolling_clips.is_object() || rolling_clips.empty()) {
        return true;
    }

    bool all_attached = true;
    for (auto it = rolling_clips.begin(); it != rolling_clips.end(); ++it) {
        const std::string serial = it.key();
        if (serial.empty() || !it.value().is_array()) {
            continue;
        }
        for (const nlohmann::json& clip : it.value()) {
            if (!clip.is_object()) {
                continue;
            }
            const int clip_index = clip.value("clip_index", -1);
            if (clip_index < 0) {
                continue;
            }
            auto clip_it = clips_by_index->find(clip_index);
            if (clip_it == clips_by_index->end()) {
                all_attached = false;
                if (error_out) {
                    append_unique_error_message(
                        *error_out,
                        "external crop rolling clip " +
                            std::to_string(clip_index) +
                            " for camera " + serial +
                            " does not match a full-frame rolling clip");
                }
                continue;
            }

            RecordingOutputDescriptor output;
            output.camera_serial = serial;
            apply_crop_recording_output_media_contract(&output);
            output.backend = "external_ipc";
            output.status = clip.value("status", std::string("completed"));
            output.video_path = clip.value("video", std::string());
            output.metadata_path = clip.value("metadata", std::string());
            output.keyframe_path = clip.value("keyframes", std::string());
            output.perf_path = clip.value("perf", std::string());
            output.summary_path = clip.value("summary", std::string());
            output.frame_count = json_u64_or(clip, "frame_count", 0ULL);
            output.first_recording_frame_id =
                json_u64_or(clip, "first_recording_frame_id", 0ULL);
            output.last_recording_frame_id =
                json_u64_or(clip, "last_recording_frame_id", 0ULL);
            output.recording_frame_id_gaps =
                json_u64_or(clip, "recording_frame_id_gaps", 0ULL);
            output.packet_count = json_u64_or(clip, "packet_count", 0ULL);
            output.packet_count_source =
                clip.value("packet_count_source", std::string());
            output.width = clip.value("width", 0);
            output.height = clip.value("height", 0);
            output.frame_rate = clip.value("frame_rate", 0);
            output.codec = clip.value("codec", std::string("hevc"));
            output.container = clip.value("container", std::string("mp4"));
            output.tuning = clip.value("tuning", std::string("lossless"));
            output.pixel_source_format = "mono8";
            output.encoded_format = "nv12";
            output.encoding_budget = clip.value(
                "encoding_budget", nlohmann::json::object());
            output.details = {
                {"clip_index", clip_index},
                {"clip_id", clip.value("clip_id", std::string())},
                {"stream_id", clip.value("stream_id", std::string())},
                {"video_backend", "external_ipc"},
                {"metadata_backend", metadata_backend},
                {"summary_json", clip.value("summary", std::string())},
                {"selection_policy", "largest_detection_by_confidence"},
                {"blank_frame_policy", "encode_black_frame_when_no_detection"},
                {"recording_control",
                 crop_recording.value("recording_control", nlohmann::json::object())},
                {"rollover",
                 crop_recording.value("rollover", nlohmann::json::object())}
            };
            clip_it->second.recording_outputs.push_back(std::move(output));
        }
    }
    return all_attached;
}

ExternalCropRecordingBackendResult build_external_crop_recording_backend(
    const ExternalCropRecordingBackendInputs& inputs)
{
    ExternalCropRecordingBackendResult result;
    auto fail = [&](const std::string& message) {
        result.ok = false;
        append_unique_error_message(result.error, message);
    };

    const std::filesystem::path recording_folder(inputs.recording_folder);
    const std::string contract_path =
        !inputs.contract_path.empty()
            ? inputs.contract_path
            : (recording_folder / "external_crop_recorder_contract.json").string();
    nlohmann::json contract;
    std::string contract_error;
    const bool contract_ok = read_json_file(contract_path, &contract, &contract_error);
    if (!contract_ok || !contract.is_object()) {
        fail("external crop recorder contract unavailable: " +
             (contract_ok ? "not a JSON object" : contract_error));
        contract = nlohmann::json::object();
    }
    const std::string artifact_root = json_string_or(
        contract, "artifact_root",
        (recording_folder / "external_crop_recorder").string());
    const std::filesystem::path artifact_root_path(artifact_root);

    RecordingControlConfig recording_control = inputs.recording_control;
    const nlohmann::json contract_control =
        contract.value("recording_control", nlohmann::json::object());
    if (contract_control.is_object() && !contract_control.empty()) {
        recording_control.record_for_seconds = json_int_or(
            contract_control, "record_for_seconds", recording_control.record_for_seconds);
        recording_control.clip_seconds = json_int_or(
            contract_control, "clip_seconds", recording_control.clip_seconds);
    }
    const nlohmann::json recording_control_json =
        build_recording_control_json(recording_control);
    result.rolling_requested = recording_control.clip_seconds > 0;
    const bool require_storage_preflight =
        contract.value("require_storage_preflight", false);

    nlohmann::json summary_paths = nlohmann::json::object();
    nlohmann::json mp4_paths = nlohmann::json::object();
    nlohmann::json keyframe_paths = nlohmann::json::object();
    nlohmann::json gop_routing_paths = nlohmann::json::object();
    nlohmann::json stream_config = nlohmann::json::object();
    nlohmann::json frames_received_json = nlohmann::json::object();
    nlohmann::json frames_encoded_json = nlohmann::json::object();
    nlohmann::json encode_dropped_json = nlohmann::json::object();
    nlohmann::json external_frames_dropped_json = nlohmann::json::object();
    nlohmann::json encode_queue_depth_json = nlohmann::json::object();
    nlohmann::json encode_queue_high_water_json = nlohmann::json::object();
    nlohmann::json enqueue_age_p95_ms_json = nlohmann::json::object();
    nlohmann::json rolling_clips = nlohmann::json::object();

    auto append_session_output =
        [&](const nlohmann::json& stream,
            const std::string& serial,
            const std::string& mp4,
            const std::string& keyframes,
            const uint64_t frames_encoded,
            const uint64_t packets_written,
            const nlohmann::json& encoding_budget,
            const int crop_size_px,
            const bool stream_ok,
            const std::string& stream_error) {
            const std::string stream_id = stream.value("stream_id", serial + "_crop");
            const std::string summary_json = stream.value("summary_json", std::string());
            const std::string codec = stream.value("codec", std::string("hevc"));
            const std::string tuning = stream.value("tuning", std::string());
            const int encode_fps = json_int_or(stream, "encode_fps", 0);
            const nlohmann::json stream_rollover =
                build_crop_rollover_json(
                    recording_control, stream_ok ? "completed" : "incomplete");

            RecordingOutputDescriptor output;
            output.camera_serial = serial;
            apply_crop_recording_output_media_contract(&output);
            output.backend = "external_ipc";
            output.status = stream_ok ? "completed" : "incomplete";
            output.video_path = mp4;
            output.metadata_path = "Cam" + serial + "_crop_meta.csv";
            output.keyframe_path = keyframes;
            output.perf_path = "Cam" + serial + "_crop_perf.csv";
            output.sidecar_perf_path = "Cam" + serial + "_crop_sidecar_perf.csv";
            output.summary_path = summary_json;
            output.frame_count = frames_encoded;
            output.first_recording_frame_id = frames_encoded > 0 ? 1 : 0;
            output.last_recording_frame_id = frames_encoded;
            output.recording_frame_id_gaps = 0;
            output.packet_count = packets_written;
            output.packet_count_source = "external_crop_recorder_summary.packets_written";
            output.width = crop_size_px;
            output.height = crop_size_px;
            output.frame_rate = encode_fps;
            output.codec = codec;
            output.container = "mp4";
            output.tuning = tuning;
            output.pixel_source_format = "mono8";
            output.encoded_format = "nv12";
            output.encoding_budget = encoding_budget;
            output.details = {
                {"stream_id", stream_id},
                {"stream_kind", stream.value("stream_kind", std::string("crop"))},
                {"output_kind", stream.value("output_kind", std::string("crop"))},
                {"camera_serial", stream.value("camera_serial", serial)},
                {"env_key", stream.value("env_key", std::string())},
                {"scope", result.rolling_requested ? "session_aggregate" : "single_clip"},
                {"video_backend", "external_ipc"},
                {"metadata_backend", inputs.session_metadata_backend},
                {"analytics_gpu_id", json_int_or(stream, "analytics_gpu_id", -1)},
                {"recorder_gpu_id", json_int_or(stream, "recorder_gpu_id", -1)},
                {"encode_queue_depth", json_int_or(stream, "encode_queue_depth", 0)},
                {"socket_path", stream.value("socket_path", std::string())},
                {"summary_json", summary_json},
                {"status_json", stream.value("status_json", std::string())},
                {"recording_control", recording_control_json},
                {"rollover", stream_rollover},
                {"selection_policy", "largest_detection_by_confidence"},
                {"blank_frame_policy", "encode_black_frame_when_no_detection"}
            };
            if (!stream_ok && !stream_error.empty()) {
                output.details["status_reason"] = stream_error;
            }
            result.session_outputs.push_back(std::move(output));

            summary_paths[serial] = summary_json;
            mp4_paths[serial] = mp4;
            keyframe_paths[serial] = keyframes;
            gop_routing_paths[serial] = stream.value("gop_routing_csv", std::string());
            stream_config[serial] = {
                {"stream_id", stream_id},
                {"stream_kind", stream.value("stream_kind", std::string("crop"))},
                {"output_kind", stream.value("output_kind", std::string("crop"))},
                {"camera_serial", stream.value("camera_serial", serial)},
                {"env_key", stream.value("env_key", std::string())},
                {"analytics_gpu_id", json_int_or(stream, "analytics_gpu_id", -1)},
                {"recorder_gpu_id", json_int_or(stream, "recorder_gpu_id", -1)},
                {"encode_queue_depth", json_int_or(stream, "encode_queue_depth", 0)},
                {"socket_path", stream.value("socket_path", std::string())},
                {"summary_json", summary_json},
                {"status_json", stream.value("status_json", std::string())},
                {"encode_fps", encode_fps},
                {"encode_max_fps", json_int_or(stream, "encode_max_fps", 0)},
                {"gop", json_int_or(stream, "gop", 0)},
                {"terminal_tail_coalesce_frames",
                 json_u64_or(stream, "terminal_tail_coalesce_frames", 0ULL)},
                {"codec", codec},
                {"tuning", tuning},
                {"recording_control", recording_control_json},
                {"rollover", stream_rollover}
            };
        };

    const nlohmann::json streams = contract.value("streams", nlohmann::json::object());
    size_t crop_stream_count = 0;
    if (streams.is_object()) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            const nlohmann::json& stream = it.value();
            if (!stream.is_object() || !contract_stream_is_crop(stream, it.key())) {
                continue;
            }
            const std::string serial = contract_stream_camera_serial(stream, it.key());
            if (serial.empty()) {
                continue;
            }
            ++crop_stream_count;

            bool stream_ok = true;
            std::string stream_error;
            const std::string summary_path = stream.value("summary_json", std::string());
            nlohmann::json summary;
            std::string summary_error;
            if (summary_path.empty() ||
                !read_json_file(summary_path, &summary, &summary_error) ||
                !summary.is_object()) {
                stream_error =
                    "external crop recorder summary unavailable for camera " + serial +
                    ": " + (summary_path.empty() ? "no summary_json in the contract"
                                                 : summary_error);
                fail(stream_error);
                append_session_output(
                    stream, serial,
                    stream.value("mp4", std::string()),
                    stream.value("mp4_keyframe", std::string()),
                    0, 0, nlohmann::json::object(),
                    inputs.crop_size_px, false, stream_error);
                continue;
            }

            const nlohmann::json merged =
                summary.value("merged_output", nlohmann::json::object());
            const nlohmann::json outputs =
                summary.value("outputs", nlohmann::json::object());
            const nlohmann::json external_encode =
                summary.value("external_encode", nlohmann::json::object());
            const nlohmann::json rolling =
                summary.value("rolling_output", nlohmann::json::object());
            const bool summary_rolling_enabled =
                rolling.is_object() && rolling.value("enabled", false);
            const nlohmann::json summary_clips =
                summary_rolling_enabled
                    ? rolling.value("clips", nlohmann::json::array())
                    : nlohmann::json::array();
            const bool worker_failed = summary.value("worker_failed", false);
            const bool merged_enabled =
                merged.is_object() && merged.value("enabled", false);
            const bool merged_failed =
                merged_enabled && merged.value("failed", false);
            const uint64_t frames_received = json_u64_or(summary, "frames_received", 0ULL);
            const uint64_t frames_encoded =
                json_u64_or(summary, "frames_encoded", frames_received);
            const uint64_t summary_encode_dropped =
                json_u64_or(summary, "encode_dropped", 0ULL);
            const uint64_t summary_external_frames_dropped =
                json_u64_or(external_encode, "frames_dropped", 0ULL);
            const uint64_t summary_encode_queue_depth =
                json_u64_or(summary, "encode_queue_depth",
                            json_u64_or(stream, "encode_queue_depth", 0ULL));
            const uint64_t summary_encode_queue_high_water =
                json_u64_or(summary, "encode_queue_high_water", 0ULL);
            const double summary_enqueue_age_p95_ms =
                json_double_or(external_encode, "enqueue_age_p95_ms", -1.0);
            const uint64_t external_packets =
                json_u64_or(external_encode, "mp4_packets", 0ULL);
            uint64_t rolling_packets = 0;
            if (summary_clips.is_array()) {
                for (const nlohmann::json& clip : summary_clips) {
                    rolling_packets += json_u64_or(clip, "packets_written", 0ULL);
                }
            }
            const uint64_t packets_written = summary_rolling_enabled
                ? rolling_packets
                : (merged_enabled
                       ? json_u64_or(merged, "packets_written", external_packets)
                       : external_packets);
            const std::string output_mp4 =
                json_string_or(outputs, "mp4", stream.value("mp4", std::string()));
            const std::string output_keyframes =
                json_string_or(outputs, "mp4_keyframe",
                               stream.value("mp4_keyframe", std::string()));
            const nlohmann::json first_rolling_clip =
                summary_clips.is_array() && !summary_clips.empty() &&
                        summary_clips.front().is_object()
                    ? summary_clips.front()
                    : nlohmann::json::object();
            const std::string mp4 = summary_rolling_enabled
                ? json_string_or(first_rolling_clip, "mp4", std::string())
                : (merged_enabled
                       ? json_string_or(merged, "mp4", output_mp4)
                       : output_mp4);
            const std::string keyframes = summary_rolling_enabled
                ? json_string_or(first_rolling_clip, "keyframes", std::string())
                : (merged_enabled
                       ? json_string_or(merged, "mp4_keyframe", output_keyframes)
                       : output_keyframes);
            const int crop_size_px =
                inputs.crop_size_px > 0 ? inputs.crop_size_px : summary_crop_size_px(summary);

            std::string runtime_contract_error;
            if (require_storage_preflight) {
                const nlohmann::json storage =
                    summary.value("storage_preflight", nlohmann::json::object());
                if (!storage.is_object() || !storage.value("checked", false)) {
                    runtime_contract_error =
                        "final recorder storage preflight is missing or unchecked";
                } else if (!storage.value("ok", false)) {
                    runtime_contract_error =
                        "final recorder storage preflight violates min_free_bytes";
                }
            }
            if (runtime_contract_error.empty() && recording_control.record_for_seconds > 0) {
                const nlohmann::json ipc =
                    summary.value("ipc_protocol", nlohmann::json::object());
                if (!ipc.is_object() ||
                    ipc.value("duration_safety_ceiling_exceeded", false) ||
                    !ipc.value("descriptor_intake_completed_cleanly", false)) {
                    runtime_contract_error =
                        "recorder duration backstop or descriptor intake failed";
                }
            }

            if (!runtime_contract_error.empty() || worker_failed || merged_failed ||
                frames_received == 0 ||
                frames_encoded == 0 || frames_encoded != frames_received ||
                packets_written != frames_encoded || mp4.empty() ||
                !std::filesystem::exists(mp4)) {
                stream_ok = false;
                stream_error =
                    "external crop recorder output incomplete for camera " + serial;
                if (!runtime_contract_error.empty()) {
                    stream_error += ": " + runtime_contract_error;
                }
                fail(stream_error);
            }

            if (summary_rolling_enabled) {
                if (!result.rolling_requested) {
                    stream_ok = false;
                    stream_error =
                        "external crop recorder produced rolling output without a crop rolling request";
                    fail(stream_error);
                }
                if (!summary_clips.is_array() || summary_clips.empty()) {
                    stream_ok = false;
                    stream_error =
                        "external crop recorder rolling output has no clips for camera " + serial;
                    fail(stream_error);
                } else {
                    CropRollingClipStream clip_stream;
                    clip_stream.camera_serial = serial;
                    clip_stream.stream_id = stream.value("stream_id", serial + "_crop");
                    clip_stream.summary_json = summary_path;
                    clip_stream.crop_size_px = crop_size_px;
                    clip_stream.frame_rate = json_int_or(stream, "encode_fps", 0);
                    clip_stream.codec = stream.value("codec", std::string("hevc"));
                    clip_stream.tuning = stream.value("tuning", std::string());
                    const CropRollingClipRecordsResult clip_records =
                        build_crop_rolling_clip_records(
                            summary_clips, clip_stream, inputs.recording_folder, fail);
                    if (!clip_records.ok) {
                        stream_ok = false;
                    }
                    if (!clip_records.incomplete_clip_error.empty()) {
                        stream_error = clip_records.incomplete_clip_error;
                    }
                    if (clip_records.perf_split_stats.orphan_rows > 0) {
                        result.warnings.push_back(
                            std::to_string(clip_records.perf_split_stats.orphan_rows) +
                            " crop perf row(s) for camera " + serial +
                            " matched no rolling clip range (first orphaned"
                            " recording_frame_id " +
                            std::to_string(
                                clip_records.perf_split_stats.first_orphan_recording_frame_id) +
                            "); these rows describe frames dropped before external"
                            " submission and were left out of the per-clip crop perf"
                            " sidecars.");
                    }
                    if (!clip_records.clip_records.empty()) {
                        rolling_clips[serial] = clip_records.clip_records;
                    }
                }
            } else if (inputs.rolling_expected) {
                stream_ok = false;
                stream_error =
                    "external crop recorder produced single-clip output in a rolling session for camera " +
                    serial;
                fail(stream_error);
            }

            append_session_output(
                stream, serial, mp4, keyframes, frames_encoded, packets_written,
                summary.value("encoding_budget", nlohmann::json::object()),
                crop_size_px, stream_ok, stream_error);

            frames_received_json[serial] = frames_received;
            frames_encoded_json[serial] = frames_encoded;
            encode_dropped_json[serial] = summary_encode_dropped;
            external_frames_dropped_json[serial] = summary_external_frames_dropped;
            encode_queue_depth_json[serial] = summary_encode_queue_depth;
            encode_queue_high_water_json[serial] = summary_encode_queue_high_water;
            if (summary_enqueue_age_p95_ms >= 0.0) {
                enqueue_age_p95_ms_json[serial] = summary_enqueue_age_p95_ms;
            }
        }
    }
    if (crop_stream_count == 0) {
        fail("external crop recorder contract declares no crop streams");
    }

    result.crop_recording = {
        {"mode", "external_ipc"},
        {"status", result.ok ? "completed" : "incomplete"},
        {"artifact_root", artifact_root},
        {"source", "external_crop_recorder_summary"},
        {"summary_json", summary_paths},
        {"merged_mp4", mp4_paths},
        {"keyframes", keyframe_paths},
        {"gop_routing_csv", gop_routing_paths},
        {"stream_config", stream_config},
        {"recording_control", recording_control_json},
        {"rollover",
         build_crop_rollover_json(
             recording_control, result.ok ? "completed" : "incomplete")},
        {"frames_received", frames_received_json},
        {"frames_encoded", frames_encoded_json},
        {"encode_dropped", encode_dropped_json},
        {"external_frames_dropped", external_frames_dropped_json},
        {"encode_queue_depth", encode_queue_depth_json},
        {"encode_queue_high_water", encode_queue_high_water_json},
        {"enqueue_age_p95_ms", enqueue_age_p95_ms_json},
        {"external_crop_recorder_contract_path", contract_path},
        {"external_crop_recorder_supervisor_plan_path",
         (artifact_root_path / "external_recorder_supervisor_plan.json").string()},
        {"duration_aware_storage_preflight_path",
         (artifact_root_path / "duration_aware_storage_preflight.json").string()},
        {"external_crop_recorder_session_json",
         (artifact_root_path / "external_recorder_session.json").string()},
        {"external_crop_recorder_finalization_json",
         (artifact_root_path / "external_recorder_finalization.json").string()}
    };
    if (!rolling_clips.empty()) {
        result.crop_recording["rolling_clips"] = std::move(rolling_clips);
    }
    if (!result.error.empty()) {
        result.crop_recording["error"] = result.error;
    }
    return result;
}

}  // namespace orange::session
