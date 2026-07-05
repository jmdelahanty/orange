// Recording-session finalizer extracted verbatim from src/orange.cpp.
// The function bodies below are byte-identical to their previous
// definitions in orange.cpp's anonymous namespace; helpers that are only
// called from this translation unit stay in an anonymous namespace here.

#include "gui/recording_finalizer.h"

#include "gui/env_util.h"

#include "crop_and_encode_worker.h"
#include "external_recorder_contract_utils.h"
#include "project.h"
#include "recording_ingress.h"
#include "session/crop_rolling_sidecars.h"
#include "session/recording_session.h"
#include "video_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>


namespace {

int gui_local_control_diagnostic_finalize_stall_seconds()
{
    if (const char* raw =
            std::getenv("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
        raw && *raw) {
        return gui_env_int(
            "ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS",
            0,
            0);
    }
    return gui_env_int(
        "ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS",
        0,
        0);
}

std::string gui_crop_stream_camera_serial(const orange::external_recorder::RecorderStreamPlan& stream)
{
    const std::string suffix = "_crop";
    auto strip_suffix = [&](std::string serial) {
        if (serial.size() > suffix.size() &&
            serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
            serial.resize(serial.size() - suffix.size());
        }
        return serial;
    };
    if (stream.output_kind == "crop" && !stream.camera_serial.empty()) {
        return strip_suffix(stream.camera_serial);
    }
    std::string serial = stream.stream_id;
    if (serial.size() > suffix.size() &&
        serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
        serial.resize(serial.size() - suffix.size());
        return serial;
    }
    serial = stream.camera_serial;
    if (serial.size() > suffix.size() &&
        serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
        serial.resize(serial.size() - suffix.size());
    }
    return serial;
}

}  // namespace

bool gui_external_stream_is_full(
    const orange::external_recorder::RecorderStreamPlan& stream)
{
    return stream.output_kind.empty() || stream.output_kind == "full";
}

bool gui_external_stream_is_crop(
    const orange::external_recorder::RecorderStreamPlan& stream)
{
    const std::string suffix = "_crop";
    auto has_crop_suffix = [&](const std::string& value) {
        return value.size() > suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return stream.output_kind == "crop" ||
           has_crop_suffix(stream.stream_id) ||
           has_crop_suffix(stream.camera_serial);
}

namespace {

bool gui_read_json_file(const std::string& path, nlohmann::json* out, std::string* error_out)
{
    if (!out) {
        if (error_out) {
            *error_out = "internal error: null JSON destination";
        }
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        if (error_out) {
            *error_out = "missing JSON file: " + path;
        }
        return false;
    }
    try {
        input >> *out;
        return true;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "invalid JSON file " + path + ": " + ex.what();
        }
        return false;
    }
}

double gui_elapsed_seconds_between(const std::chrono::steady_clock::time_point& start,
                                   const std::chrono::steady_clock::time_point& finish);

}  // namespace

std::string gui_external_recorder_clip_id(const int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

std::string gui_json_string_or(const nlohmann::json& object,
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

namespace {

uint64_t gui_json_u64_or(const nlohmann::json& object,
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

bool gui_external_recorder_plan_requests_rolling(
    const orange::external_recorder::SupervisorPlan& plan)
{
    for (const auto& stream : plan.streams) {
        if (stream.clip_seconds > 0) {
            return true;
        }
    }
    return false;
}

bool gui_external_recorder_recording_control_from_plan(
    const orange::external_recorder::SupervisorPlan& plan,
    orange::session::RecordingControlConfig* recording_control_out,
    std::string* error_out)
{
    orange::session::RecordingControlConfig resolved;
    bool found = false;
    for (const auto& stream : plan.streams) {
        if (stream.record_for_seconds <= 0 && stream.clip_seconds <= 0) {
            continue;
        }
        if (stream.clip_seconds > 0 && stream.record_for_seconds <= 0) {
            if (error_out) {
                *error_out =
                    "GUI external rolling requires record_for_seconds when clip_seconds is set";
            }
            return false;
        }
        if (!found) {
            resolved.record_for_seconds = stream.record_for_seconds;
            resolved.clip_seconds = stream.clip_seconds;
            found = true;
            continue;
        }
        if (resolved.record_for_seconds != stream.record_for_seconds ||
            resolved.clip_seconds != stream.clip_seconds) {
            if (error_out) {
                *error_out =
                    "GUI external rolling requires consistent recording_control across streams";
            }
            return false;
        }
    }
    if (recording_control_out) {
        *recording_control_out = resolved;
    }
    return true;
}

void gui_append_error_message(std::string& target, const std::string& message)
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

}  // namespace

nlohmann::json gui_crop_rollover_json(
    const orange::session::RecordingControlConfig& recording_control,
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

bool gui_attach_crop_rolling_outputs_to_clips(
    const nlohmann::json& recording_backend,
    std::map<int, orange::session::RollingClipManifestOptions>* clips_by_index,
    std::string* error_out)
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
                    gui_append_error_message(
                        *error_out,
                        "external crop rolling clip " +
                            std::to_string(clip_index) +
                            " for camera " + serial +
                            " does not match a full-frame rolling clip");
                }
                continue;
            }

            orange::session::RecordingOutputDescriptor output;
            output.camera_serial = serial;
            output.output_kind = "crop";
            output.role = "sidecar";
            output.backend = "external_ipc";
            output.status = clip.value("status", std::string("completed"));
            output.video_path = clip.value("video", std::string());
            output.metadata_path = clip.value("metadata", std::string());
            output.keyframe_path = clip.value("keyframes", std::string());
            output.perf_path = clip.value("perf", std::string());
            output.summary_path = clip.value("summary", std::string());
            output.frame_count = gui_json_u64_or(clip, "frame_count", 0ULL);
            output.first_recording_frame_id =
                gui_json_u64_or(clip, "first_recording_frame_id", 0ULL);
            output.last_recording_frame_id =
                gui_json_u64_or(clip, "last_recording_frame_id", 0ULL);
            output.recording_frame_id_gaps =
                gui_json_u64_or(clip, "recording_frame_id_gaps", 0ULL);
            output.packet_count = gui_json_u64_or(clip, "packet_count", 0ULL);
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
            output.coordinate_space = "full_frame_pixels";
            output.details = {
                {"clip_index", clip_index},
                {"clip_id", clip.value("clip_id", std::string())},
                {"stream_id", clip.value("stream_id", std::string())},
                {"video_backend", "external_ipc"},
                {"metadata_backend", "orange_gui_split_crop_csv"},
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

nlohmann::json gui_build_rolling_recording_session_snapshot_update(
    const std::string& recording_folder,
    const nlohmann::json& manifest,
    const orange::session::RecordingSessionIndexArtifacts& index_artifacts,
    const nlohmann::json& gui_display_frame_rate)
{
    nlohmann::json indexes =
        manifest.value("indexes", nlohmann::json::object());
    if (indexes.is_object()) {
        if (!index_artifacts.clip_index_json_path.empty()) {
            indexes["clip_index_json_path"] = index_artifacts.clip_index_json_path;
        }
        if (!index_artifacts.clip_index_csv_path.empty()) {
            indexes["clip_index_csv_path"] = index_artifacts.clip_index_csv_path;
        }
    }

    nlohmann::json update = {
        {"recording_mode", manifest.value("mode", std::string())},
        {"recording_session_manifest_path",
         (std::filesystem::path(recording_folder) / "recording_session.json").string()},
        {"recording_session_status", manifest.value("status", std::string("incomplete"))},
        {"recording_session_camera_count",
         manifest.value("cameras", nlohmann::json::array()).size()},
        {"recording_session_index", indexes},
        {"rolling_clip_count", manifest.value("clips", nlohmann::json::array()).size()},
        {"rolling_index_row_count", indexes.value("row_count", 0)},
        {"gui_display_frame_rate", gui_display_frame_rate}
    };
    if (manifest.contains("recording_backend")) {
        update["recording_backend"] = manifest["recording_backend"];
    }
    return update;
}

bool gui_write_external_rolling_recording_session_manifest(
    const GuiRecordingRunState& run,
    const orange::external_recorder::SupervisorPlan& plan,
    const nlohmann::json& recording_backend,
    const std::vector<orange::session::RecordingOutputDescriptor>& session_recording_outputs,
    const bool recording_session_ok,
    const nlohmann::json& gui_display_frame_rate,
    nlohmann::json* manifest_out,
    nlohmann::json* bridge_out,
    std::string* error_out,
    GuiRecordingFinalizeProgress* progress)
{
    orange::session::RecordingControlConfig recording_control;
    if (!gui_external_recorder_recording_control_from_plan(
            plan,
            &recording_control,
            error_out)) {
        return false;
    }
    if (recording_control.clip_seconds <= 0) {
        if (error_out) {
            *error_out = "GUI external rolling manifest requested without clip_seconds";
        }
        return false;
    }

    const std::string session_id =
        std::filesystem::path(run.recording_folder).filename().string();
    std::map<int, orange::session::RollingClipManifestOptions> clips_by_index;
    std::vector<std::string> camera_serials;
    nlohmann::json summary_paths = nlohmann::json::object();
    bool full_frame_clips_ok = true;

    for (const auto& stream : plan.streams) {
        if (!gui_external_stream_is_full(stream)) {
            continue;
        }
        const std::string serial = stream.camera_serial.empty()
            ? stream.stream_id
            : stream.camera_serial;
        if (serial.empty() || stream.summary_json.empty()) {
            if (error_out) {
                *error_out =
                    "GUI external rolling manifest bridge missing camera serial or summary_json";
            }
            return false;
        }

        nlohmann::json summary;
        std::string read_error;
        if (!gui_read_json_file(stream.summary_json, &summary, &read_error)) {
            if (error_out) {
                *error_out = read_error;
            }
            return false;
        }
        const nlohmann::json rolling =
            summary.value("rolling_output", nlohmann::json::object());
        if (!rolling.is_object() || !rolling.value("enabled", false)) {
            if (error_out) {
                *error_out =
                    "external recorder summary missing enabled rolling_output for camera " +
                    serial;
            }
            return false;
        }
        const nlohmann::json summary_clips =
            rolling.value("clips", nlohmann::json::array());
        if (!summary_clips.is_array() || summary_clips.empty()) {
            if (error_out) {
                *error_out =
                    "external recorder rolling_output has no clips for camera " + serial;
            }
            return false;
        }

        camera_serials.push_back(serial);
        summary_paths[serial] = stream.summary_json;
        const int fps = std::max(
            1,
            summary.value("fps", stream.encode_fps > 0 ? stream.encode_fps : 100));

        for (const nlohmann::json& clip : summary_clips) {
            if (!clip.is_object()) {
                continue;
            }
            const int clip_index = clip.value("clip_index", -1);
            if (clip_index < 0) {
                if (error_out) {
                    *error_out =
                        "external recorder rolling clip missing clip_index for camera " +
                        serial;
                }
                return false;
            }
            orange::session::RollingClipManifestOptions& manifest_clip =
                clips_by_index[clip_index];
            if (manifest_clip.clip_id.empty()) {
                manifest_clip.producer = "orange_gui_external_ipc";
                manifest_clip.session_id = session_id;
                manifest_clip.clip_index = clip_index;
                manifest_clip.clip_id =
                    gui_json_string_or(
                        clip,
                        "clip_id",
                        gui_external_recorder_clip_id(clip_index));
                manifest_clip.directory =
                    clip.value("directory", std::string());
                manifest_clip.recording_folder = manifest_clip.directory;
                manifest_clip.status = "completed";
                manifest_clip.drain_completed = true;
            }

            const bool clip_failed = clip.value("failed", false);
            full_frame_clips_ok = full_frame_clips_ok && !clip_failed;
            if (clip_failed) {
                manifest_clip.status = "incomplete";
                manifest_clip.drain_completed = false;
            }

            const uint64_t frame_count =
                gui_json_u64_or(clip, "frame_count", 0ULL);
            const uint64_t first_frame =
                gui_json_u64_or(clip, "first_recording_frame_id", 0ULL);
            const uint64_t last_frame =
                gui_json_u64_or(clip, "last_recording_frame_id", 0ULL);
            const uint64_t packets_written =
                gui_json_u64_or(clip, "packets_written", 0ULL);
            const std::string mp4 =
                clip.value("mp4", std::string());
            const std::string metadata =
                clip.value("metadata", std::string());
            const std::string keyframes =
                clip.value("keyframes", std::string());
            const bool clip_artifacts_ok =
                frame_count > 0 &&
                packets_written > 0 &&
                !mp4.empty() &&
                std::filesystem::exists(mp4) &&
                !metadata.empty() &&
                std::filesystem::exists(metadata) &&
                !keyframes.empty() &&
                std::filesystem::exists(keyframes);
            if (!clip_artifacts_ok) {
                full_frame_clips_ok = false;
                manifest_clip.status = "incomplete";
                manifest_clip.drain_completed = false;
            }

            if (first_frame > 0 &&
                (manifest_clip.first_recording_frame_id == 0 ||
                 first_frame < manifest_clip.first_recording_frame_id)) {
                manifest_clip.first_recording_frame_id = first_frame;
            }
            if (last_frame > manifest_clip.last_recording_frame_id) {
                manifest_clip.last_recording_frame_id = last_frame;
            }
            manifest_clip.actual_duration_s =
                std::max(
                    manifest_clip.actual_duration_s,
                    static_cast<double>(frame_count) / static_cast<double>(fps));

            orange::session::RecordingSessionCameraArtifact camera_artifact;
            camera_artifact.camera_serial = serial;
            camera_artifact.video_path = mp4;
            camera_artifact.metadata_path = metadata;
            camera_artifact.keyframe_path = keyframes;
            camera_artifact.frame_count = frame_count;
            camera_artifact.first_recording_frame_id = first_frame;
            camera_artifact.last_recording_frame_id = last_frame;
            camera_artifact.recording_frame_id_gaps = 0;
            camera_artifact.packet_count = packets_written;
            camera_artifact.packet_count_source =
                "external_recorder_summary.packets_written";
            manifest_clip.cameras.push_back(camera_artifact);

            orange::session::RecordingOutputDescriptor output;
            output.camera_serial = serial;
            output.output_kind = "full";
            output.role = "ingest_authoritative";
            output.backend = "external_ipc";
            output.status = clip_artifacts_ok && !clip_failed
                ? "completed"
                : "incomplete";
            output.video_path = mp4;
            output.metadata_path = metadata;
            output.keyframe_path = keyframes;
            output.summary_path = stream.summary_json;
            output.frame_count = frame_count;
            output.first_recording_frame_id = first_frame;
            output.last_recording_frame_id = last_frame;
            output.recording_frame_id_gaps = 0;
            output.packet_count = packets_written;
            output.packet_count_source =
                "external_recorder_summary.packets_written";
            output.frame_rate = fps;
            output.codec = stream.codec;
            output.container = "mp4";
            output.tuning = stream.tuning;
            output.pixel_source_format = "mono8";
            output.encoded_format = "nv12";
            output.coordinate_space = "full_frame_pixels";
            output.details = {
                {"clip_index", clip_index},
                {"clip_id", manifest_clip.clip_id},
                {"stream_id", stream.stream_id},
                {"analytics_gpu_id", stream.analytics_gpu_id},
                {"recorder_gpu_id", stream.recorder_gpu_id},
                {"recording_control",
                 orange::session::build_recording_control_json(recording_control)},
                {"rollover",
                 {
                     {"requested", true},
                     {"status", "completed"},
                     {"implementation",
                      orange::external_recorder::kExternalRecorderRollingImplementation},
                     {"seamless_writer_switch", true},
                     {"records_during_rollover", true},
                     {"boundary", "gop_first_frame_id"}
                 }}
            };
            manifest_clip.recording_outputs.push_back(std::move(output));
        }
    }

    if (clips_by_index.empty()) {
        if (error_out) {
            *error_out = "GUI external rolling manifest bridge found no clips";
        }
        return false;
    }
    std::sort(camera_serials.begin(), camera_serials.end());
    camera_serials.erase(
        std::unique(camera_serials.begin(), camera_serials.end()),
        camera_serials.end());

    std::vector<orange::session::RollingClipManifestOptions> clip_options;
    clip_options.reserve(clips_by_index.size());
    double sum_clip_actual_duration_s = 0.0;
    std::string crop_output_attachment_error;
    const bool crop_output_attachments_ok =
        gui_attach_crop_rolling_outputs_to_clips(
            recording_backend,
            &clips_by_index,
            &crop_output_attachment_error);
    if (!crop_output_attachments_ok) {
        full_frame_clips_ok = false;
    }
    if (progress) {
        progress->clips_total.store(
            static_cast<int>(clips_by_index.size()),
            std::memory_order_relaxed);
        progress->clips_done.store(0, std::memory_order_relaxed);
    }
    for (auto it = clips_by_index.begin(); it != clips_by_index.end(); ++it) {
        orange::session::RollingClipManifestOptions clip = std::move(it->second);
        if (clip.recording_folder.empty()) {
            if (error_out) {
                *error_out =
                    "GUI external rolling clip missing output directory for clip " +
                    std::to_string(clip.clip_index);
            }
            return false;
        }
        if (clip.cameras.size() != camera_serials.size()) {
            full_frame_clips_ok = false;
            clip.status = "incomplete";
            clip.drain_completed = false;
        }
        const bool final_clip = std::next(it) == clips_by_index.end();
        clip.start_reason = it == clips_by_index.begin() ? "recording_start" : "rollover";
        clip.stop_reason = final_clip ? run.stop_reason : "clip_seconds_elapsed";
        clip.final_clip = final_clip;
        clip.timed_stop_hit =
            final_clip && clip.stop_reason == "record_for_seconds_elapsed";
        clip.requested_duration_s =
            final_clip
                ? clip.actual_duration_s
                : static_cast<double>(recording_control.clip_seconds);
        clip.rollover_request_id = 0;
        if (!final_clip) {
            clip.rollover_at_recording_frame_id =
                std::next(it)->second.first_recording_frame_id;
        } else if (it != clips_by_index.begin()) {
            clip.rollover_at_recording_frame_id = clip.first_recording_frame_id;
        }
        clip.pending_next_clip = false;
        if (it == clips_by_index.begin()) {
            clip.started_at_utc = run.recording_started_at_utc;
            clip.started_at_elapsed_s = 0.0;
        }
        if (final_clip) {
            clip.stop_requested_at_utc = run.recording_stop_requested_at_utc;
            clip.stop_requested_at_elapsed_s =
                gui_elapsed_seconds_between(
                    run.recording_started_at,
                    run.recording_stop_requested_at);
        }
        clip.finalized_at_utc = run.recording_drained_at_utc;
        clip.finalized_at_elapsed_s =
            gui_elapsed_seconds_between(
                run.recording_started_at,
                run.recording_drained_at);
        clip.drain_duration_s =
            final_clip
                ? gui_elapsed_seconds_between(
                      run.recording_stop_requested_at,
                      run.recording_drained_at)
                : 0.0;
        sum_clip_actual_duration_s += clip.actual_duration_s;

        std::string clip_manifest_error;
        if (!orange::session::write_recording_session_manifest(
                (std::filesystem::path(clip.recording_folder) /
                 "clip_manifest.json").string(),
                orange::session::build_recording_clip_manifest(clip),
                &clip_manifest_error)) {
            if (error_out) {
                *error_out = clip_manifest_error;
            }
            return false;
        }
        clip_options.push_back(std::move(clip));
        if (progress) {
            progress->clips_done.fetch_add(1, std::memory_order_relaxed);
        }
    }

    nlohmann::json rolling_recording_backend = recording_backend;
    if (!rolling_recording_backend.is_object()) {
        rolling_recording_backend = nlohmann::json::object();
    }
    if (!full_frame_clips_ok) {
        rolling_recording_backend["status"] = "incomplete";
    }
    if (!crop_output_attachments_ok) {
        rolling_recording_backend["status"] = "incomplete";
        if (rolling_recording_backend.contains("crop_recording") &&
            rolling_recording_backend["crop_recording"].is_object()) {
            rolling_recording_backend["crop_recording"]["status"] = "incomplete";
            if (!crop_output_attachment_error.empty()) {
                std::string combined_error =
                    rolling_recording_backend["crop_recording"].value(
                        "error",
                        std::string());
                gui_append_error_message(combined_error, crop_output_attachment_error);
                rolling_recording_backend["crop_recording"]["error"] = combined_error;
            }
        }
    }
    rolling_recording_backend["recording_control"] =
        orange::session::build_recording_control_json(recording_control);
    rolling_recording_backend["rollover"] = {
        {"requested", true},
        {"status", full_frame_clips_ok ? "completed" : "incomplete"},
        {"implementation",
         orange::external_recorder::kExternalRecorderRollingImplementation},
        {"seamless_writer_switch", true},
        {"records_during_rollover", true},
        {"boundary", "gop_first_frame_id"},
        {"next_writer_preopened", false}
    };
    rolling_recording_backend["summary_json"] = summary_paths;

    orange::session::RollingRecordingSessionManifestOptions manifest_options;
    manifest_options.producer = "orange_gui_external_ipc";
    manifest_options.session_id = session_id;
    manifest_options.created_at_utc = run.recording_started_at_utc;
    manifest_options.updated_at_utc = run.recording_drained_at_utc;
    manifest_options.recording_folder = run.recording_folder;
    manifest_options.status =
        recording_session_ok && full_frame_clips_ok ? "completed" : "incomplete";
    manifest_options.requested_stream_duration_seconds =
        recording_control.record_for_seconds;
    manifest_options.stream_started_at_utc = run.recording_started_at_utc;
    manifest_options.stream_finished_at_utc = run.recording_drained_at_utc;
    manifest_options.stream_actual_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_drained_at);
    manifest_options.recording_control = recording_control;
    manifest_options.recording_started = true;
    manifest_options.recording_started_at_utc = run.recording_started_at_utc;
    manifest_options.recording_started_at_elapsed_s = 0.0;
    manifest_options.recording_stop_requested = true;
    manifest_options.recording_stop_requested_at_utc =
        run.recording_stop_requested_at_utc;
    manifest_options.recording_stop_requested_at_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_stop_requested_at);
    manifest_options.recording_stop_reason = run.stop_reason;
    manifest_options.recording_drain_completed = true;
    manifest_options.recording_drained_at_utc = run.recording_drained_at_utc;
    manifest_options.recording_drained_at_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_drained_at);
    manifest_options.actual_recording_duration_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_stop_requested_at);
    manifest_options.drain_duration_s =
        gui_elapsed_seconds_between(
            run.recording_stop_requested_at,
            run.recording_drained_at);
    manifest_options.sum_clip_actual_duration_s = sum_clip_actual_duration_s;
    manifest_options.rollover_implementation =
        orange::external_recorder::kExternalRecorderRollingImplementation;
    manifest_options.rollover_next_writer_preopened = false;
    manifest_options.recording_stop_control = run.stop_control;
    manifest_options.recording_backend = std::move(rolling_recording_backend);
    manifest_options.recording_outputs = session_recording_outputs;
    manifest_options.camera_serials = std::move(camera_serials);
    manifest_options.clips = std::move(clip_options);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(
            manifest_options);
    const std::filesystem::path manifest_path =
        std::filesystem::path(run.recording_folder) / "recording_session.json";
    orange::session::RecordingSessionIndexArtifacts index_artifacts;
    if (!orange::session::write_rolling_clip_index_artifacts(
            run.recording_folder,
            manifest,
            &index_artifacts,
            error_out)) {
        return false;
    }
    std::string manifest_error;
    if (!orange::session::write_recording_session_manifest(
            manifest_path.string(),
            manifest,
            &manifest_error)) {
        if (error_out) {
            *error_out = manifest_error;
        }
        return false;
    }

    const nlohmann::json snapshot_update =
        gui_build_rolling_recording_session_snapshot_update(
            run.recording_folder,
            manifest,
            index_artifacts,
            gui_display_frame_rate);
    if (!update_recording_snapshot_session_artifacts(
            run.recording_folder,
            snapshot_update)) {
        if (error_out) {
            *error_out =
                "failed to update recording_snapshot.json with GUI external IPC rolling index";
        }
        return false;
    }
    if (manifest.contains("recording_outputs") &&
        manifest["recording_outputs"].is_object() &&
        !update_recording_snapshot_recording_outputs(
            run.recording_folder,
            manifest["recording_outputs"])) {
        if (error_out) {
            *error_out =
                "failed to update recording_snapshot.json with GUI external IPC recording outputs";
        }
        return false;
    }

    if (manifest_out) {
        *manifest_out = manifest;
    }
    if (bridge_out) {
        *bridge_out = {
            {"pass", recording_session_ok && full_frame_clips_ok},
            {"full_frame_pass", full_frame_clips_ok},
            {"path", manifest_path.string()},
            {"mode", "rolling_clips"},
            {"producer", "orange_gui_external_ipc"},
            {"clip_count", manifest_options.clips.size()},
            {"camera_count", manifest_options.camera_serials.size()},
            {"indexes",
             {
                 {"clip_index_json", index_artifacts.clip_index_json_path},
                 {"clip_index_csv", index_artifacts.clip_index_csv_path}
             }},
            {"summary_json", summary_paths}
        };
    }
    return true;
}

bool has_gui_timepoint(const std::chrono::steady_clock::time_point& timepoint)
{
    return timepoint.time_since_epoch() != std::chrono::steady_clock::duration::zero();
}

namespace {

double gui_elapsed_seconds_between(const std::chrono::steady_clock::time_point& start,
                                   const std::chrono::steady_clock::time_point& finish)
{
    if (!has_gui_timepoint(start) || !has_gui_timepoint(finish) || finish < start) {
        return 0.0;
    }
    return std::chrono::duration<double>(finish - start).count();
}

void gui_update_local_control_stop_manifest_for_finalized_drain(
    GuiRecordingRunState* run)
{
    if (!run ||
        !run->stop_control.is_object() ||
        run->stop_control.empty()) {
        return;
    }

    const bool drain_timed_out =
        run->stop_control.value("drain_timed_out", false);
    if (!run->stop_control.contains("forced_finalize_requested")) {
        run->stop_control["forced_finalize_requested"] = false;
    }
    if (!run->stop_control.contains("forced_finalize_stream_stop_requested")) {
        run->stop_control["forced_finalize_stream_stop_requested"] = false;
    }
    if (!run->stop_control.contains("forced_finalize_requested_at_utc")) {
        run->stop_control["forced_finalize_requested_at_utc"] = "";
    }
    run->stop_control["drain_completed"] = true;
    run->stop_control["drain_timed_out"] = drain_timed_out;
    run->stop_control["drain_completed_at_utc"] =
        run->recording_drained_at_utc;
    run->stop_control["health"] = drain_timed_out ? "warning" : "ok";
    run->stop_control["error_code"] =
        drain_timed_out ? "drain_timeout" : "";
    run->stop_control["ack_state"] =
        drain_timed_out ? "failed_timeout" : "executed";
    run->stop_control["last_event"] =
        drain_timed_out ? "finalized_after_drain_timeout" : "finalized";
    run->stop_control["last_event_at_utc"] =
        run->recording_drained_at_utc;
}

}  // namespace


// --- Phased finalize ---------------------------------------------------------
//
// The monolithic gui_finalize_recording_session_if_ready body was split into
// the gate / prepare (F1) / run (F2) / complete (F3) phases declared in
// gui/recording_finalizer.h. The transplanted code below is otherwise
// verbatim; every mutation of live GUI-owned state (CameraControl,
// RecordingSessionState, the run flags) stays on the GUI thread in the gate,
// F1, and F3, while F2 touches only its inputs/outcome structs and the
// filesystem so it can run on a background thread.

const char* gui_recording_finalize_stage_label(const int stage)
{
    switch (static_cast<GuiRecordingFinalizeStage>(stage)) {
        case GuiRecordingFinalizeStage::kStoppingRecorders:
            return "stopping recorders";
        case GuiRecordingFinalizeStage::kReadingSummaries:
            return "reading recorder summaries";
        case GuiRecordingFinalizeStage::kWritingClipManifests:
            return "writing clip manifests";
        case GuiRecordingFinalizeStage::kUpdatingSnapshot:
            return "updating session snapshot";
        case GuiRecordingFinalizeStage::kIdle:
        default:
            return "preparing";
    }
}

bool gui_recording_finalize_gate_ready(
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control)
{
    if (!run || !run->active || !run->finalizing || run->recording_folder.empty()) {
        return false;
    }
    const bool external_ipc = run->recording_sink_mode == "external_ipc";
    if (external_ipc) {
        if (camera_control && camera_control->record_video) {
            return false;
        }
        if (camera_control &&
            camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
            camera_control->recording_draining = true;
            camera_control->stop_record = true;
            return false;
        }
        if (recording_session &&
            orange::session::should_reassert_recording_drain_flags(
                run->recording_sink_mode,
                orange::session::recording_pipelines_drained(recording_session))) {
            if (camera_control) {
                camera_control->recording_draining = true;
                camera_control->stop_record = true;
            }
            return false;
        }
        if (camera_control) {
            camera_control->recording_draining = false;
            camera_control->stop_record = false;
        }
    } else if (camera_control &&
               (camera_control->record_video ||
                camera_control->recording_draining ||
                camera_control->active_recorders.load(std::memory_order_relaxed) > 0)) {
        return false;
    }

    const int diagnostic_finalize_stall_seconds =
        gui_local_control_diagnostic_finalize_stall_seconds();
    if (diagnostic_finalize_stall_seconds > 0 &&
        run->stop_control.is_object() &&
        !run->stop_control.empty() &&
        has_gui_timepoint(run->recording_stop_requested_at)) {
        const double elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                run->recording_stop_requested_at)
                .count();
        if (elapsed < static_cast<double>(diagnostic_finalize_stall_seconds)) {
            if (!run->diagnostic_finalize_stall_reported) {
                run->diagnostic_finalize_stall_reported = true;
                run->stop_control["diagnostic_finalize_stall_seconds"] =
                    diagnostic_finalize_stall_seconds;
                run->stop_control["diagnostic_finalize_stall_active"] = true;
                std::cerr
                    << "[GUI][local_control] diagnostic finalize stall active"
                    << " seconds=" << diagnostic_finalize_stall_seconds
                    << " request_id="
                    << run->stop_control.value("request_id", std::string())
                    << " operation_id="
                    << run->stop_control.value("operation_id", std::string())
                    << std::endl;
            }
            return false;
        }
        if (run->diagnostic_finalize_stall_reported) {
            run->stop_control["diagnostic_finalize_stall_active"] = false;
        }
    }
    return true;
}

GuiRecordingFinalizeInputs gui_prepare_recording_finalize(
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const int crop_size_px,
    const nlohmann::json& gui_display_frame_rate)
{
    GuiRecordingFinalizeInputs inputs;
    if (!run) {
        return inputs;
    }

    run->recording_drained_at = std::chrono::steady_clock::now();
    run->recording_drained_at_utc = get_current_utc_timestamp();
    if (!has_gui_timepoint(run->recording_stop_requested_at)) {
        run->recording_stop_requested_at = run->recording_drained_at;
        run->recording_stop_requested_at_utc = run->recording_drained_at_utc;
    }
    gui_update_local_control_stop_manifest_for_finalized_drain(run);

    inputs.valid = true;
    inputs.run = *run;
    inputs.external_ipc = run->recording_sink_mode == "external_ipc";
    inputs.recording_session_available = recording_session != nullptr;
    inputs.crop_external_recorder_active =
        recording_session &&
        recording_session->crop_recording_sink_mode == "external_ipc" &&
        !recording_session->external_crop_recorder_lifecycle.plan.streams.empty();
    inputs.crop_size_px = crop_size_px;
    inputs.gui_display_frame_rate = gui_display_frame_rate;

    if (cameras_params && cameras_select && num_cameras > 0) {
        inputs.cameras.reserve(static_cast<size_t>(num_cameras));
        for (int i = 0; i < num_cameras; ++i) {
            GuiRecordingFinalizeCameraSnapshot camera;
            camera.camera_serial = cameras_params[i].camera_serial;
            camera.camera_id = cameras_params[i].camera_id;
            camera.frame_rate = cameras_params[i].frame_rate;
            camera.record = cameras_select[i].record;
            camera.crop_and_encode = cameras_select[i].crop_and_encode;
            inputs.cameras.push_back(std::move(camera));
        }
    }

    if (recording_session) {
        if (inputs.external_ipc) {
            // Both of these touch live pipeline objects, so they must stay
            // on the GUI thread: reset the IPC connections before the
            // recorders are stopped (as the synchronous body did), and
            // snapshot the per-pipeline ingress stats for the background
            // phase (the drain gate guarantees they are final).
            orange::session::reset_external_ipc_connections(recording_session);
            const size_t pipeline_count = std::min(
                recording_session->recording_pipelines.size(),
                static_cast<size_t>(std::max(0, num_cameras)));
            for (size_t i = 0; i < pipeline_count; ++i) {
                const auto& pipeline = recording_session->recording_pipelines[i];
                if (!pipeline || !pipeline->recording_ingress()) {
                    continue;
                }
                if (cameras_select && !cameras_select[i].record) {
                    continue;
                }
                const std::string serial =
                    cameras_params && !cameras_params[i].camera_serial.empty()
                        ? cameras_params[i].camera_serial
                        : std::to_string(i);
                const RecordingIngressStats stats =
                    pipeline->recording_ingress()->GetStats();
                inputs.total_ingress_submitted += stats.submitted_frames;
                inputs.total_ingress_acked += stats.external_ipc_frames_acked;
                inputs.total_ingress_failures += stats.external_ipc_failures;
                inputs.total_ingress_ack_timeouts += stats.external_ipc_ack_timeouts;
                inputs.ingress_stats[serial] = {
                    {"submitted_frames", stats.submitted_frames},
                    {"external_ipc_frames_acked", stats.external_ipc_frames_acked},
                    {"external_ipc_failures", stats.external_ipc_failures},
                    {"external_ipc_ack_timeouts", stats.external_ipc_ack_timeouts}
                };
            }
        }

        // MOVE ownership of the external recorder lifecycle states into the
        // finalize inputs so a concurrent start (or anything else on the GUI
        // thread) can never touch the objects the background phase stops and
        // reads. The moved-from members are reset to fresh states explicitly
        // (the defaulted move leaves scalar fields unspecified).
        inputs.external_recorder_lifecycle =
            std::move(recording_session->external_recorder_lifecycle);
        recording_session->external_recorder_lifecycle =
            orange::external_recorder::SupervisedRecorderLifecycleState{};
        inputs.external_crop_recorder_lifecycle =
            std::move(recording_session->external_crop_recorder_lifecycle);
        recording_session->external_crop_recorder_lifecycle =
            orange::external_recorder::SupervisedRecorderLifecycleState{};
        inputs.external_recorder_last_error =
            recording_session->external_recorder_last_error;
        inputs.external_crop_recorder_last_error =
            recording_session->external_crop_recorder_last_error;
        inputs.external_recorder_contract_path =
            recording_session->external_recorder_contract_path;
        inputs.external_recorder_supervisor_plan_path =
            recording_session->external_recorder_supervisor_plan_path;
        inputs.external_crop_recorder_contract_path =
            recording_session->external_crop_recorder_contract_path;
        inputs.external_crop_recorder_supervisor_plan_path =
            recording_session->external_crop_recorder_supervisor_plan_path;
    }
    return inputs;
}

GuiRecordingFinalizeOutcome gui_run_recording_finalize(
    GuiRecordingFinalizeInputs* inputs,
    GuiRecordingFinalizeProgress* progress)
{
    GuiRecordingFinalizeOutcome outcome;
    if (!inputs || !inputs->valid) {
        outcome.error_message = "internal error: finalize inputs are unavailable";
        return outcome;
    }
    auto publish_stage = [&](const GuiRecordingFinalizeStage stage) {
        if (progress) {
            progress->stage.store(
                static_cast<int>(stage), std::memory_order_relaxed);
        }
    };
    publish_stage(GuiRecordingFinalizeStage::kStoppingRecorders);

    const GuiRecordingRunState& run = inputs->run;
    const bool external_ipc = inputs->external_ipc;
    const int crop_size_px = inputs->crop_size_px;
    const nlohmann::json& gui_display_frame_rate = inputs->gui_display_frame_rate;

    std::vector<std::string> camera_serials;
    for (const GuiRecordingFinalizeCameraSnapshot& camera : inputs->cameras) {
        if (camera.record && !camera.camera_serial.empty()) {
            camera_serials.push_back(camera.camera_serial);
        }
    }
    std::vector<orange::session::RecordingSessionCameraArtifact> camera_artifacts;
    nlohmann::json recording_backend = nlohmann::json::object();
    bool external_recorder_ok = true;
    std::string external_recorder_error;
    std::vector<orange::session::RecordingOutputDescriptor> external_crop_outputs;
    const bool crop_external_recorder_active = inputs->crop_external_recorder_active;
    bool crop_external_recorder_lifecycle_ok = true;
    bool crop_external_recorder_ok = true;
    std::string crop_external_recorder_error;
    auto json_string_or =
        [](const nlohmann::json& object,
           const char* key,
           const std::string& fallback) -> std::string {
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
        };
    auto json_u64_or =
        [](const nlohmann::json& object,
           const char* key,
           const uint64_t fallback) -> uint64_t {
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
        };
    auto json_double_or =
        [](const nlohmann::json& object,
           const char* key,
           const double fallback) -> double {
            if (object.is_object()) {
                const auto it = object.find(key);
                if (it != object.end() && it->is_number()) {
                    return it->get<double>();
                }
            }
            return fallback;
        };
    auto append_error_message =
        [](std::string& target, const std::string& message) {
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
        };

    // Crop recorder env overrides are stacked after full-frame recorder
    // overrides. Stop crop first so scoped environment restoration unwinds in
    // the reverse order of startup.
    if (crop_external_recorder_active &&
        inputs->external_crop_recorder_lifecycle.started) {
        std::string stop_error;
        if (!orange::external_recorder::StopSupervisedRecorderLifecycle(
                &inputs->external_crop_recorder_lifecycle,
                &stop_error)) {
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
            append_error_message(
                crop_external_recorder_error,
                stop_error.empty()
                    ? "external crop recorder supervisor shutdown failed"
                    : stop_error);
        }
    }

    if (external_ipc) {
        if (!inputs->recording_session_available) {
            external_recorder_ok = false;
            external_recorder_error = "external IPC recording session state is unavailable";
        } else {
            // reset_external_ipc_connections ran in the prepare phase (it
            // touches live pipeline objects).
            std::string stop_error;
            if (!orange::external_recorder::StopSupervisedRecorderLifecycle(
                    &inputs->external_recorder_lifecycle,
                    &stop_error)) {
                external_recorder_ok = false;
                append_error_message(
                    external_recorder_error,
                    stop_error.empty()
                        ? "external recorder supervisor shutdown failed"
                        : stop_error);
            }
            if (!inputs->external_recorder_lifecycle.last_artifact_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    inputs->external_recorder_lifecycle.last_artifact_error);
                inputs->external_recorder_lifecycle.last_artifact_error.clear();
                external_recorder_ok = false;
            }
            if (!inputs->external_recorder_lifecycle.last_runtime_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    inputs->external_recorder_lifecycle.last_runtime_error);
                inputs->external_recorder_lifecycle.last_runtime_error.clear();
                external_recorder_ok = false;
            }
            if (!inputs->external_recorder_last_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    inputs->external_recorder_last_error);
                external_recorder_ok = false;
            }

            // Ingress stats were snapshotted from the live pipelines in the
            // prepare phase; the drain gate guarantees they are final.
            const nlohmann::json& ingress_stats = inputs->ingress_stats;
            const uint64_t total_ingress_submitted = inputs->total_ingress_submitted;
            const uint64_t total_ingress_acked = inputs->total_ingress_acked;
            const uint64_t total_ingress_failures = inputs->total_ingress_failures;
            const uint64_t total_ingress_ack_timeouts = inputs->total_ingress_ack_timeouts;
            if (total_ingress_failures > 0 ||
                total_ingress_ack_timeouts > 0 ||
                (total_ingress_submitted > 0 &&
                 total_ingress_acked < total_ingress_submitted)) {
                external_recorder_ok = false;
                if (!external_recorder_error.empty()) {
                    external_recorder_error += "; ";
                }
                external_recorder_error +=
                    "external IPC ingress incomplete: submitted=" +
                    std::to_string(total_ingress_submitted) +
                    " acked=" +
                    std::to_string(total_ingress_acked) +
                    " failures=" +
                    std::to_string(total_ingress_failures) +
                    " ack_timeouts=" +
                    std::to_string(total_ingress_ack_timeouts);
            }

            publish_stage(GuiRecordingFinalizeStage::kReadingSummaries);
            nlohmann::json summary_paths = nlohmann::json::object();
            nlohmann::json merged_mp4s = nlohmann::json::object();
            nlohmann::json keyframe_paths = nlohmann::json::object();
            nlohmann::json gop_routing_paths = nlohmann::json::object();

            for (const auto& stream : inputs->external_recorder_lifecycle.plan.streams) {
                if (!gui_external_stream_is_full(stream)) {
                    continue;
                }
                const std::string serial = stream.camera_serial.empty()
                    ? stream.stream_id
                    : stream.camera_serial;
                if (serial.empty()) {
                    continue;
                }
                if (std::find(camera_serials.begin(), camera_serials.end(), serial) ==
                    camera_serials.end()) {
                    camera_serials.push_back(serial);
                }

                nlohmann::json summary;
                std::ifstream input(stream.summary_json);
                if (!input) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "missing external recorder summary for camera " + serial +
                        ": " + stream.summary_json;
                    continue;
                }
                try {
                    input >> summary;
                } catch (const std::exception& ex) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "invalid external recorder summary for camera " + serial +
                        ": " + ex.what();
                    continue;
                }

                const nlohmann::json merged =
                    summary.value("merged_output", nlohmann::json::object());
                const nlohmann::json outputs =
                    summary.value("outputs", nlohmann::json::object());
                const nlohmann::json external_encode =
                    summary.value("external_encode", nlohmann::json::object());
                const bool worker_failed = summary.value("worker_failed", false);
                const bool merged_enabled =
                    merged.is_object() && merged.value("enabled", false);
                const bool merged_failed =
                    merged_enabled && merged.value("failed", false);
                const uint64_t frames_received =
                    summary.value("frames_received", 0ULL);
                const uint64_t frames_encoded =
                    summary.value("frames_encoded", frames_received);
                const uint64_t external_packets =
                    json_u64_or(external_encode, "mp4_packets", 0ULL);
                const uint64_t packets_written = merged_enabled
                    ? json_u64_or(merged, "packets_written", external_packets)
                    : external_packets;
                const std::string output_mp4 =
                    json_string_or(outputs, "mp4", stream.mp4);
                const std::string output_keyframes =
                    json_string_or(outputs, "mp4_keyframe", stream.mp4_keyframe);
                const std::string mp4 = merged_enabled
                    ? json_string_or(merged, "mp4", output_mp4)
                    : output_mp4;
                const std::string keyframes = merged_enabled
                    ? json_string_or(merged, "mp4_keyframe", output_keyframes)
                    : output_keyframes;

                if (worker_failed || merged_failed || frames_received == 0 ||
                    frames_encoded == 0 || frames_encoded != frames_received ||
                    packets_written == 0 || mp4.empty() ||
                    !std::filesystem::exists(mp4)) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "external recorder output incomplete for camera " + serial;
                }

                orange::session::RecordingSessionCameraArtifact artifact;
                artifact.camera_serial = serial;
                artifact.video_path = mp4;
                artifact.metadata_path = stream.summary_json;
                artifact.keyframe_path = keyframes;
                artifact.frame_count = frames_encoded;
                artifact.first_recording_frame_id = frames_encoded > 0 ? 1 : 0;
                artifact.last_recording_frame_id = frames_encoded;
                artifact.recording_frame_id_gaps = 0;
                artifact.packet_count = packets_written;
                artifact.packet_count_source = "external_recorder_summary.packets_written";
                camera_artifacts.push_back(std::move(artifact));

                summary_paths[serial] = stream.summary_json;
                merged_mp4s[serial] = mp4;
                keyframe_paths[serial] = keyframes;
                gop_routing_paths[serial] = stream.gop_routing_csv;
            }

            recording_backend = {
                {"mode", "external_ipc"},
                {"status", external_recorder_ok ? "completed" : "incomplete"},
                {"artifact_root", inputs->external_recorder_lifecycle.plan.artifact_root},
                {"source", "external_recorder_summary"},
                {"ingress_stats", ingress_stats},
                {"ingress_totals",
                 {
                     {"submitted_frames", total_ingress_submitted},
                     {"external_ipc_frames_acked", total_ingress_acked},
                     {"external_ipc_failures", total_ingress_failures},
                     {"external_ipc_ack_timeouts", total_ingress_ack_timeouts}
                 }},
                {"summary_json", summary_paths},
                {"merged_mp4", merged_mp4s},
                {"keyframes", keyframe_paths},
                {"gop_routing_csv", gop_routing_paths},
                {"external_recorder_contract_path", inputs->external_recorder_contract_path},
                {"external_recorder_supervisor_plan_path",
                 inputs->external_recorder_supervisor_plan_path},
                {"external_recorder_session_json",
                 (std::filesystem::path(inputs->external_recorder_lifecycle.plan.artifact_root) /
                  "external_recorder_session.json").string()},
                {"external_recorder_finalization_json",
                 (std::filesystem::path(inputs->external_recorder_lifecycle.plan.artifact_root) /
                  "external_recorder_finalization.json").string()}
            };
            if (!external_recorder_error.empty()) {
                recording_backend["error"] = external_recorder_error;
            }
        }
    } else {
        camera_artifacts =
            orange::session::build_recording_camera_artifacts(
                camera_serials,
                run.recording_folder,
                true);
    }

    publish_stage(GuiRecordingFinalizeStage::kReadingSummaries);
    if (crop_external_recorder_active) {
        if (!inputs->external_crop_recorder_lifecycle.last_artifact_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                inputs->external_crop_recorder_lifecycle.last_artifact_error);
            inputs->external_crop_recorder_lifecycle.last_artifact_error.clear();
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }
        if (!inputs->external_crop_recorder_lifecycle.last_runtime_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                inputs->external_crop_recorder_lifecycle.last_runtime_error);
            inputs->external_crop_recorder_lifecycle.last_runtime_error.clear();
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }
        if (!inputs->external_crop_recorder_last_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                inputs->external_crop_recorder_last_error);
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }

        nlohmann::json crop_summary_paths = nlohmann::json::object();
        nlohmann::json crop_mp4_paths = nlohmann::json::object();
        nlohmann::json crop_keyframe_paths = nlohmann::json::object();
        nlohmann::json crop_gop_routing_paths = nlohmann::json::object();
        nlohmann::json crop_stream_config = nlohmann::json::object();
        nlohmann::json crop_frames_received = nlohmann::json::object();
        nlohmann::json crop_frames_encoded = nlohmann::json::object();
        nlohmann::json crop_encode_dropped = nlohmann::json::object();
        nlohmann::json crop_external_frames_dropped = nlohmann::json::object();
        nlohmann::json crop_encode_queue_depth = nlohmann::json::object();
        nlohmann::json crop_encode_queue_high_water = nlohmann::json::object();
        nlohmann::json crop_enqueue_age_p95_ms = nlohmann::json::object();
        nlohmann::json crop_rolling_clips = nlohmann::json::object();
        orange::session::RecordingControlConfig crop_recording_control_config;
        std::string crop_recording_control_error;
        if (!gui_external_recorder_recording_control_from_plan(
                inputs->external_crop_recorder_lifecycle.plan,
                &crop_recording_control_config,
                &crop_recording_control_error)) {
            append_error_message(crop_external_recorder_error, crop_recording_control_error);
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
            crop_recording_control_config = orange::session::RecordingControlConfig{};
        }
        const nlohmann::json crop_recording_control =
            orange::session::build_recording_control_json(crop_recording_control_config);
        const bool crop_rolling_requested =
            crop_recording_control_config.clip_seconds > 0;

        auto append_external_crop_output =
            [&](const auto& stream,
                const std::string& serial,
                const std::string& mp4,
                const std::string& keyframes,
                const uint64_t frames_encoded,
                const uint64_t packets_written,
                const bool stream_ok,
                const std::string& stream_error) {
                orange::session::RecordingOutputDescriptor output;
                output.camera_serial = serial;
                output.output_kind = "crop";
                output.role = "sidecar";
                output.backend = "external_ipc";
                output.status = stream_ok ? "completed" : "incomplete";
                output.video_path = mp4;
                output.metadata_path = "Cam" + serial + "_crop_meta.csv";
                output.keyframe_path = keyframes;
                output.perf_path = "Cam" + serial + "_crop_perf.csv";
                output.sidecar_perf_path = "Cam" + serial + "_crop_sidecar_perf.csv";
                output.summary_path = stream.summary_json;
                output.frame_count = frames_encoded;
                output.first_recording_frame_id = frames_encoded > 0 ? 1 : 0;
                output.last_recording_frame_id = frames_encoded;
                output.recording_frame_id_gaps = 0;
                output.packet_count = packets_written;
                output.packet_count_source = "external_crop_recorder_summary.packets_written";
                const int resolved_crop_size =
                    CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
                const nlohmann::json stream_crop_rollover =
                    gui_crop_rollover_json(
                        crop_recording_control_config,
                        stream_ok ? "completed" : "incomplete");
                output.width = resolved_crop_size;
                output.height = resolved_crop_size;
                output.frame_rate = stream.encode_fps;
                output.codec = stream.codec;
                output.container = "mp4";
                output.tuning = stream.tuning;
                output.pixel_source_format = "mono8";
                output.encoded_format = "nv12";
                output.coordinate_space = "full_frame_pixels";
                output.details = {
                    {"stream_id", stream.stream_id},
                    {"stream_kind", stream.stream_kind},
                    {"output_kind", stream.output_kind},
                    {"camera_serial", stream.camera_serial},
                    {"env_key", stream.env_key},
                    {"scope", crop_rolling_requested ? "session_aggregate" : "single_clip"},
                    {"video_backend", "external_ipc"},
                    {"metadata_backend", "orange_gui"},
                    {"analytics_gpu_id", stream.analytics_gpu_id},
                    {"recorder_gpu_id", stream.recorder_gpu_id},
                    {"encode_queue_depth", stream.encode_queue_depth},
                    {"socket_path", stream.socket_path},
                    {"summary_json", stream.summary_json},
                    {"status_json", stream.status_json},
                    {"recording_control", crop_recording_control},
                    {"rollover", stream_crop_rollover},
                    {"selection_policy", "largest_detection_by_confidence"},
                    {"blank_frame_policy", "encode_black_frame_when_no_detection"}
                };
                if (!stream_ok && !stream_error.empty()) {
                    output.details["status_reason"] = stream_error;
                }
                external_crop_outputs.push_back(std::move(output));

                crop_summary_paths[serial] = stream.summary_json;
                crop_mp4_paths[serial] = mp4;
                crop_keyframe_paths[serial] = keyframes;
                crop_gop_routing_paths[serial] = stream.gop_routing_csv;
                crop_stream_config[serial] = {
                    {"stream_id", stream.stream_id},
                    {"stream_kind", stream.stream_kind},
                    {"output_kind", stream.output_kind},
                    {"camera_serial", stream.camera_serial},
                    {"env_key", stream.env_key},
                    {"analytics_gpu_id", stream.analytics_gpu_id},
                    {"recorder_gpu_id", stream.recorder_gpu_id},
                    {"encode_queue_depth", stream.encode_queue_depth},
                    {"socket_path", stream.socket_path},
                    {"summary_json", stream.summary_json},
                    {"status_json", stream.status_json},
                    {"encode_fps", stream.encode_fps},
                    {"encode_max_fps", stream.encode_max_fps},
                    {"gop", stream.gop},
                    {"terminal_tail_coalesce_frames",
                     stream.terminal_tail_coalesce_frames},
                    {"codec", stream.codec},
                    {"tuning", stream.tuning},
                    {"recording_control", crop_recording_control},
                    {"rollover", stream_crop_rollover}
                };
            };

        for (const auto& stream : inputs->external_crop_recorder_lifecycle.plan.streams) {
            if (!gui_external_stream_is_crop(stream)) {
                continue;
            }
            const std::string serial = gui_crop_stream_camera_serial(stream);
            if (serial.empty()) {
                continue;
            }

            bool stream_ok = crop_external_recorder_lifecycle_ok;
            std::string stream_error;
            nlohmann::json summary;
            std::string summary_error;
            if (!gui_read_json_file(stream.summary_json, &summary, &summary_error)) {
                stream_ok = false;
                crop_external_recorder_ok = false;
                stream_error =
                    "external crop recorder summary unavailable for camera " +
                    serial + ": " + summary_error;
                if (!crop_external_recorder_error.empty()) {
                    crop_external_recorder_error += "; ";
                }
                crop_external_recorder_error += stream_error;
                append_external_crop_output(
                    stream,
                    serial,
                    stream.mp4,
                    stream.mp4_keyframe,
                    0,
                    0,
                    false,
                    stream_error);
                continue;
            }

            const nlohmann::json merged =
                summary.value("merged_output", nlohmann::json::object());
            const nlohmann::json outputs =
                summary.value("outputs", nlohmann::json::object());
            const nlohmann::json external_encode =
                summary.value("external_encode", nlohmann::json::object());
            const bool worker_failed = summary.value("worker_failed", false);
            const bool merged_enabled =
                merged.is_object() && merged.value("enabled", false);
            const bool merged_failed =
                merged_enabled && merged.value("failed", false);
            const uint64_t frames_received =
                summary.value("frames_received", 0ULL);
            const uint64_t frames_encoded =
                summary.value("frames_encoded", frames_received);
            const uint64_t summary_encode_dropped =
                json_u64_or(summary, "encode_dropped", 0ULL);
            const uint64_t summary_external_frames_dropped =
                json_u64_or(external_encode, "frames_dropped", 0ULL);
            const uint64_t summary_encode_queue_depth =
                json_u64_or(
                    summary,
                    "encode_queue_depth",
                    stream.encode_queue_depth > 0
                        ? static_cast<uint64_t>(stream.encode_queue_depth)
                        : 0ULL);
            const uint64_t summary_encode_queue_high_water =
                json_u64_or(summary, "encode_queue_high_water", 0ULL);
            const double summary_enqueue_age_p95_ms =
                json_double_or(external_encode, "enqueue_age_p95_ms", -1.0);
            const uint64_t external_packets =
                json_u64_or(external_encode, "mp4_packets", 0ULL);
            const uint64_t packets_written = merged_enabled
                ? json_u64_or(merged, "packets_written", external_packets)
                : external_packets;
            const std::string output_mp4 =
                json_string_or(outputs, "mp4", stream.mp4);
            const std::string output_keyframes =
                json_string_or(outputs, "mp4_keyframe", stream.mp4_keyframe);
            const std::string mp4 = merged_enabled
                ? json_string_or(merged, "mp4", output_mp4)
                : output_mp4;
            const std::string keyframes = merged_enabled
                ? json_string_or(merged, "mp4_keyframe", output_keyframes)
                : output_keyframes;
            const nlohmann::json rolling =
                summary.value("rolling_output", nlohmann::json::object());
            const bool summary_rolling_enabled =
                rolling.is_object() && rolling.value("enabled", false);

            if (worker_failed || merged_failed || frames_received == 0 ||
                frames_encoded == 0 || frames_encoded != frames_received ||
                packets_written == 0 || mp4.empty() ||
                !std::filesystem::exists(mp4)) {
                stream_ok = false;
                crop_external_recorder_ok = false;
                stream_error =
                    "external crop recorder output incomplete for camera " + serial;
                if (!crop_external_recorder_error.empty()) {
                    crop_external_recorder_error += "; ";
                }
                crop_external_recorder_error += stream_error;
            }

            if (summary_rolling_enabled) {
                if (!crop_rolling_requested) {
                    stream_ok = false;
                    crop_external_recorder_ok = false;
                    stream_error =
                        "external crop recorder produced rolling output without a crop rolling request";
                    append_error_message(crop_external_recorder_error, stream_error);
                }

                const nlohmann::json summary_clips =
                    rolling.value("clips", nlohmann::json::array());
                if (!summary_clips.is_array() || summary_clips.empty()) {
                    stream_ok = false;
                    crop_external_recorder_ok = false;
                    stream_error =
                        "external crop recorder rolling output has no clips for camera " + serial;
                    append_error_message(crop_external_recorder_error, stream_error);
                } else {
                    std::vector<orange::session::RecordingFrameCsvRange> metadata_ranges;
                    std::vector<orange::session::RecordingFrameCsvRange> perf_ranges;
                    std::vector<nlohmann::json> clip_records;
                    metadata_ranges.reserve(summary_clips.size());
                    perf_ranges.reserve(summary_clips.size());
                    clip_records.reserve(summary_clips.size());
                    const int resolved_crop_size =
                        CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
                    for (const nlohmann::json& clip : summary_clips) {
                        if (!clip.is_object()) {
                            continue;
                        }
                        const int clip_index = clip.value("clip_index", -1);
                        const uint64_t frame_count =
                            json_u64_or(clip, "frame_count", 0ULL);
                        const uint64_t first_frame =
                            json_u64_or(clip, "first_recording_frame_id", 0ULL);
                        const uint64_t last_frame =
                            json_u64_or(clip, "last_recording_frame_id", 0ULL);
                        const uint64_t packets =
                            json_u64_or(clip, "packets_written", 0ULL);
                        const std::string clip_mp4 =
                            json_string_or(clip, "mp4", std::string());
                        const std::string clip_keyframes =
                            json_string_or(clip, "keyframes", std::string());
                        std::filesystem::path clip_dir =
                            clip.value("directory", std::string());
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
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            stream_error =
                                "external crop recorder rolling clip incomplete for camera " +
                                serial;
                            append_error_message(crop_external_recorder_error, stream_error);
                            continue;
                        }

                        metadata_ranges.push_back({
                            first_frame,
                            last_frame,
                            clip_metadata,
                            0,
                        });
                        perf_ranges.push_back({
                            first_frame,
                            last_frame,
                            clip_perf,
                            0,
                        });
                        clip_records.push_back({
                            {"clip_index", clip_index},
                            {"clip_id",
                             clip.value("clip_id", gui_external_recorder_clip_id(clip_index))},
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
                            {"packet_count_source",
                             "external_crop_recorder_summary.packets_written"},
                            {"width", resolved_crop_size},
                            {"height", resolved_crop_size},
                            {"frame_rate", stream.encode_fps},
                            {"codec", stream.codec},
                            {"container", "mp4"},
                            {"tuning", stream.tuning}
                        });
                    }

                    if (!metadata_ranges.empty()) {
                        std::string split_error;
                        const std::string root_metadata =
                            (std::filesystem::path(run.recording_folder) /
                             ("Cam" + serial + "_crop_meta.csv")).string();
                        // Crop metadata rows exist only for frames accepted
                        // for encoding, so a row outside every clip range is
                        // a hole in the recorded data: fail loudly.
                        if (!orange::session::split_recording_frame_csv_by_ranges(
                                root_metadata,
                                &metadata_ranges,
                                &split_error,
                                orange::session::RecordingFrameCsvOrphanRowPolicy::kFail)) {
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            append_error_message(
                                crop_external_recorder_error,
                                "failed to split crop metadata for camera " +
                                    serial + ": " + split_error);
                        }
                        split_error.clear();
                        const std::string root_perf =
                            (std::filesystem::path(run.recording_folder) /
                             ("Cam" + serial + "_crop_perf.csv")).string();
                        // Crop perf rows are also written for frames dropped
                        // before external submission (drop_reason set), so
                        // head/tail rows can legitimately match no clip
                        // range: count and report them without failing.
                        orange::session::RecordingFrameCsvSplitStats perf_split_stats;
                        if (!orange::session::split_recording_frame_csv_by_ranges(
                                root_perf,
                                &perf_ranges,
                                &split_error,
                                orange::session::RecordingFrameCsvOrphanRowPolicy::kCountAndSkip,
                                &perf_split_stats)) {
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            append_error_message(
                                crop_external_recorder_error,
                                "failed to split crop perf for camera " +
                                    serial + ": " + split_error);
                        } else if (perf_split_stats.orphan_rows > 0) {
                            std::cerr
                                << "[GUI][recording] WARNING: "
                                << perf_split_stats.orphan_rows
                                << " crop perf row(s) for camera " << serial
                                << " matched no rolling clip range (first orphaned"
                                   " recording_frame_id "
                                << perf_split_stats.first_orphan_recording_frame_id
                                << "); these rows describe frames dropped before"
                                   " external submission and were left out of the"
                                   " per-clip crop perf sidecars."
                                << std::endl;
                        }
                        nlohmann::json stream_clips = nlohmann::json::array();
                        for (size_t i = 0; i < clip_records.size(); ++i) {
                            const uint64_t frame_count =
                                clip_records[i].value("frame_count", 0ULL);
                            clip_records[i]["metadata_rows"] =
                                i < metadata_ranges.size()
                                    ? metadata_ranges[i].rows_written
                                    : 0ULL;
                            clip_records[i]["perf_rows"] =
                                i < perf_ranges.size()
                                    ? perf_ranges[i].rows_written
                                    : 0ULL;
                            if (clip_records[i].value("metadata_rows", 0ULL) != frame_count ||
                                clip_records[i].value("perf_rows", 0ULL) != frame_count) {
                                stream_ok = false;
                                crop_external_recorder_ok = false;
                                append_error_message(
                                    crop_external_recorder_error,
                                    "external crop rolling sidecar row mismatch for camera " +
                                        serial);
                            }
                            stream_clips.push_back(clip_records[i]);
                        }
                        crop_rolling_clips[serial] = std::move(stream_clips);
                    }
                }
            }

            append_external_crop_output(
                stream,
                serial,
                mp4,
                keyframes,
                frames_encoded,
                packets_written,
                stream_ok,
                stream_error);

            crop_frames_received[serial] = frames_received;
            crop_frames_encoded[serial] = frames_encoded;
            crop_encode_dropped[serial] = summary_encode_dropped;
            crop_external_frames_dropped[serial] = summary_external_frames_dropped;
            crop_encode_queue_depth[serial] = summary_encode_queue_depth;
            crop_encode_queue_high_water[serial] = summary_encode_queue_high_water;
            if (summary_enqueue_age_p95_ms >= 0.0) {
                crop_enqueue_age_p95_ms[serial] = summary_enqueue_age_p95_ms;
            }
        }

        if (!recording_backend.is_object() || recording_backend.empty()) {
            recording_backend = {
                {"mode", "real"},
                {"status", "completed"}
            };
        }
        const nlohmann::json crop_rollover_backend =
            gui_crop_rollover_json(
                crop_recording_control_config,
                crop_external_recorder_ok ? "completed" : "incomplete");
        recording_backend["crop_recording"] = {
            {"mode", "external_ipc"},
            {"status", crop_external_recorder_ok ? "completed" : "incomplete"},
            {"artifact_root", inputs->external_crop_recorder_lifecycle.plan.artifact_root},
            {"source", "external_crop_recorder_summary"},
            {"summary_json", crop_summary_paths},
            {"merged_mp4", crop_mp4_paths},
            {"keyframes", crop_keyframe_paths},
            {"gop_routing_csv", crop_gop_routing_paths},
            {"stream_config", crop_stream_config},
            {"recording_control", crop_recording_control},
            {"rollover", crop_rollover_backend},
            {"frames_received", crop_frames_received},
            {"frames_encoded", crop_frames_encoded},
            {"encode_dropped", crop_encode_dropped},
            {"external_frames_dropped", crop_external_frames_dropped},
            {"encode_queue_depth", crop_encode_queue_depth},
            {"encode_queue_high_water", crop_encode_queue_high_water},
            {"enqueue_age_p95_ms", crop_enqueue_age_p95_ms},
            {"external_crop_recorder_contract_path", inputs->external_crop_recorder_contract_path},
            {"external_crop_recorder_supervisor_plan_path",
             inputs->external_crop_recorder_supervisor_plan_path},
            {"external_crop_recorder_session_json",
             (std::filesystem::path(inputs->external_crop_recorder_lifecycle.plan.artifact_root) /
              "external_recorder_session.json").string()},
            {"external_crop_recorder_finalization_json",
             (std::filesystem::path(inputs->external_crop_recorder_lifecycle.plan.artifact_root) /
              "external_recorder_finalization.json").string()}
        };
        if (!crop_rolling_clips.empty()) {
            recording_backend["crop_recording"]["rolling_clips"] =
                std::move(crop_rolling_clips);
        }
        if (!crop_external_recorder_error.empty()) {
            recording_backend["crop_recording"]["error"] = crop_external_recorder_error;
        }
    }

    const bool recording_session_ok =
        external_recorder_ok &&
        (!crop_external_recorder_active || crop_external_recorder_ok);

    publish_stage(GuiRecordingFinalizeStage::kWritingClipManifests);
    const std::filesystem::path manifest_path =
        std::filesystem::path(run.recording_folder) / "recording_session.json";
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json recording_session_bridge = nlohmann::json::object();
    std::string manifest_mode = "single_clip";
    const std::string manifest_producer =
        external_ipc ? "orange_gui_external_ipc" : "orange_gui";
    const bool external_rolling_requested =
        external_ipc &&
        inputs->recording_session_available &&
        gui_external_recorder_plan_requests_rolling(
            inputs->external_recorder_lifecycle.plan);

    if (external_rolling_requested) {
        std::string rolling_error;
        if (!gui_write_external_rolling_recording_session_manifest(
                run,
                inputs->external_recorder_lifecycle.plan,
                recording_backend,
                external_crop_outputs,
                recording_session_ok,
                gui_display_frame_rate,
                &manifest,
                &recording_session_bridge,
                &rolling_error,
                progress)) {
            std::cerr << "[GUI][recording] Failed to write GUI external IPC rolling manifest: "
                      << rolling_error << std::endl;
            outcome.error_message =
                "failed to write GUI external IPC rolling manifest: " + rolling_error;
            outcome.external_recorder_error = external_recorder_error;
            outcome.crop_external_recorder_error = crop_external_recorder_error;
            return outcome;
        }
        if (!recording_session_bridge.value("full_frame_pass", true)) {
            external_recorder_ok = false;
            append_error_message(
                external_recorder_error,
                "GUI external rolling full-frame clip manifest marked incomplete");
        }
        manifest_mode = "rolling_clips";
    } else {
        orange::session::SingleClipRecordingSessionManifestOptions manifest_options;
        manifest_options.producer = manifest_producer;
        manifest_options.session_id =
            std::filesystem::path(run.recording_folder).filename().string();
        manifest_options.created_at_utc = run.recording_started_at_utc;
        manifest_options.updated_at_utc = run.recording_drained_at_utc;
        manifest_options.recording_folder = run.recording_folder;
        manifest_options.status = recording_session_ok ? "completed" : "incomplete";
        manifest_options.stream_started_at_utc = run.recording_started_at_utc;
        manifest_options.stream_finished_at_utc = run.recording_drained_at_utc;
        manifest_options.stream_actual_elapsed_s =
            gui_elapsed_seconds_between(run.recording_started_at, run.recording_drained_at);
        manifest_options.recording_started = true;
        manifest_options.recording_started_at_utc = run.recording_started_at_utc;
        manifest_options.recording_started_at_elapsed_s = 0.0;
        manifest_options.recording_stop_requested = true;
        manifest_options.recording_stop_requested_at_utc = run.recording_stop_requested_at_utc;
        manifest_options.recording_stop_requested_at_elapsed_s =
            gui_elapsed_seconds_between(run.recording_started_at, run.recording_stop_requested_at);
        manifest_options.recording_stop_reason = run.stop_reason;
        manifest_options.recording_drain_completed = true;
        manifest_options.recording_drained_at_utc = run.recording_drained_at_utc;
        manifest_options.recording_drained_at_elapsed_s =
            gui_elapsed_seconds_between(run.recording_started_at, run.recording_drained_at);
        manifest_options.actual_recording_duration_s =
            gui_elapsed_seconds_between(run.recording_started_at, run.recording_stop_requested_at);
        manifest_options.drain_duration_s =
            gui_elapsed_seconds_between(run.recording_stop_requested_at, run.recording_drained_at);
        manifest_options.timed_stop_hit = false;
        manifest_options.recording_stop_control = run.stop_control;
        manifest_options.recording_backend = recording_backend;
        manifest_options.cameras = std::move(camera_artifacts);
        if (!inputs->cameras.empty()) {
            const int resolved_crop_size =
                CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
            for (const GuiRecordingFinalizeCameraSnapshot& camera : inputs->cameras) {
                if (!camera.crop_and_encode) {
                    continue;
                }
                std::string camera_serial = camera.camera_serial;
                if (camera_serial.empty()) {
                    camera_serial = std::to_string(camera.camera_id);
                }
                auto external_crop_it = std::find_if(
                    external_crop_outputs.begin(),
                    external_crop_outputs.end(),
                    [&](const orange::session::RecordingOutputDescriptor& output) {
                        return output.camera_serial == camera_serial;
                    });
                if (external_crop_it != external_crop_outputs.end()) {
                    manifest_options.recording_outputs.push_back(*external_crop_it);
                } else if (crop_external_recorder_active) {
                    const std::filesystem::path external_crop_root =
                        !inputs->external_crop_recorder_lifecycle.plan.artifact_root.empty()
                            ? std::filesystem::path(
                                  inputs->external_crop_recorder_lifecycle.plan.artifact_root)
                            : std::filesystem::path(run.recording_folder) / "external_crop_recorder";
                    const std::string external_crop_prefix =
                        (external_crop_root / ("Cam" + camera_serial + "_crop_external")).string();
                    orange::session::RecordingOutputDescriptor output;
                    output.camera_serial = camera_serial;
                    output.output_kind = "crop";
                    output.role = "sidecar";
                    output.backend = "external_ipc";
                    output.status = "incomplete";
                    output.video_path = external_crop_prefix + ".mp4";
                    output.metadata_path = "Cam" + camera_serial + "_crop_meta.csv";
                    output.keyframe_path = external_crop_prefix + "_keyframe.json";
                    output.perf_path = "Cam" + camera_serial + "_crop_perf.csv";
                    output.sidecar_perf_path =
                        "Cam" + camera_serial + "_crop_sidecar_perf.csv";
                    output.width = resolved_crop_size;
                    output.height = resolved_crop_size;
                    output.frame_rate = camera.frame_rate;
                    output.codec = "hevc";
                    output.container = "mp4";
                    output.tuning = "lossless";
                    output.pixel_source_format = "mono8";
                    output.encoded_format = "nv12";
                    output.coordinate_space = "full_frame_pixels";
                    output.details = {
                        {"video_backend", "external_ipc"},
                        {"metadata_backend", "orange_gui"},
                        {"status_reason", "external crop recorder output missing from supervisor plan"},
                        {"recording_control", {
                            {"record_for_seconds", 0},
                            {"clip_seconds", 0}
                        }},
                        {"rollover", {
                            {"requested", false},
                            {"status", "not_requested"},
                            {"implementation", "none"},
                            {"seamless_writer_switch", false},
                            {"records_during_rollover", false},
                            {"output_kind", "crop"},
                            {"supported_mode", "single_clip"},
                            {"rolling_supported", false}
                        }},
                        {"selection_policy", "largest_detection_by_confidence"},
                        {"blank_frame_policy", "encode_black_frame_when_no_detection"}
                    };
                    manifest_options.recording_outputs.push_back(std::move(output));
                } else {
                    manifest_options.recording_outputs.push_back(
                        orange::session::build_crop_recording_output_descriptor(
                            camera_serial,
                            run.recording_folder,
                            true,
                            resolved_crop_size,
                            camera.frame_rate,
                            manifest_options.status));
                }
            }
        }

        manifest =
            orange::session::build_single_clip_recording_session_manifest(manifest_options);
        std::string manifest_error;
        if (!orange::session::write_recording_session_manifest(
                manifest_path.string(),
                manifest,
                &manifest_error)) {
            std::cerr << "[GUI][recording] Failed to write recording_session.json: "
                      << manifest_error << std::endl;
            outcome.error_message =
                "failed to write recording_session.json: " + manifest_error;
            outcome.external_recorder_error = external_recorder_error;
            outcome.crop_external_recorder_error = crop_external_recorder_error;
            return outcome;
        }
        recording_session_bridge = {
            {"pass", recording_session_ok},
            {"path", manifest_path.string()},
            {"mode", manifest_mode},
            {"producer", manifest_producer},
            {"camera_count", camera_serials.size()}
        };
    }

    publish_stage(GuiRecordingFinalizeStage::kUpdatingSnapshot);
    if (external_ipc && inputs->recording_session_available) {
        orange::external_recorder::FinalizationManifestOptions finalization_options;
        finalization_options.experiment_root = run.recording_folder;
        finalization_options.artifact_root =
            inputs->external_recorder_lifecycle.plan.artifact_root;
        finalization_options.run_id =
            std::filesystem::path(run.recording_folder).filename().string();
        finalization_options.status = external_recorder_ok ? "pass" : "fail";
        finalization_options.started_at_utc = run.recording_started_at_utc;
        finalization_options.finished_at_utc = run.recording_drained_at_utc;
        finalization_options.error = external_recorder_error;
        nlohmann::json finalization =
            orange::external_recorder::BuildExternalRecorderFinalizationManifest(
                finalization_options);
        finalization["recording_session_manifest"] = recording_session_bridge;
        const orange::external_recorder::ArtifactWriteResult finalization_write =
            orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
                inputs->external_recorder_lifecycle.plan.artifact_root,
                finalization);
        if (!finalization_write.ok) {
            std::cerr << "[GUI][recording] Failed to write external recorder finalization: "
                      << finalization_write.error_message << std::endl;
        }
    }
    if (crop_external_recorder_active) {
        orange::external_recorder::FinalizationManifestOptions finalization_options;
        finalization_options.experiment_root = run.recording_folder;
        finalization_options.artifact_root =
            inputs->external_crop_recorder_lifecycle.plan.artifact_root;
        finalization_options.run_id =
            std::filesystem::path(run.recording_folder).filename().string();
        finalization_options.status = crop_external_recorder_ok ? "pass" : "fail";
        finalization_options.started_at_utc = run.recording_started_at_utc;
        finalization_options.finished_at_utc = run.recording_drained_at_utc;
        finalization_options.error = crop_external_recorder_error;
        nlohmann::json finalization =
            orange::external_recorder::BuildExternalRecorderFinalizationManifest(
                finalization_options);
        finalization["recording_session_manifest"] = {
            {"pass", crop_external_recorder_ok},
            {"path", manifest_path.string()},
            {"mode", manifest_mode},
            {"producer", manifest_producer},
            {"output_kind", "crop"},
            {"crop_mode", "single_clip"},
            {"camera_count", external_crop_outputs.size()}
        };
        const orange::external_recorder::ArtifactWriteResult finalization_write =
            orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
                inputs->external_crop_recorder_lifecycle.plan.artifact_root,
                finalization);
        if (!finalization_write.ok) {
            std::cerr << "[GUI][recording] Failed to write external crop recorder finalization: "
                      << finalization_write.error_message << std::endl;
        }
    }

    if (!external_rolling_requested) {
        const nlohmann::json snapshot_update = {
            {"recording_mode", "single_clip"},
            {"recording_session_manifest_path", manifest_path.string()},
            {"recording_session_status", recording_session_ok ? "completed" : "incomplete"},
            {"recording_session_camera_count", camera_serials.size()},
            {"gui_display_frame_rate", gui_display_frame_rate}
        };
        if (!update_recording_snapshot_session_artifacts(
                run.recording_folder,
                snapshot_update)) {
            std::cerr << "[GUI][recording] Failed to update recording_snapshot.json session pointers."
                      << std::endl;
            outcome.error_message =
                "failed to update recording_snapshot.json session pointers";
            outcome.external_recorder_error = external_recorder_error;
            outcome.crop_external_recorder_error = crop_external_recorder_error;
            return outcome;
        }
        if (manifest.contains("recording_outputs") &&
            manifest["recording_outputs"].is_object() &&
            !update_recording_snapshot_recording_outputs(
                run.recording_folder,
                manifest["recording_outputs"])) {
            std::cerr << "[GUI][recording] Failed to update recording_snapshot.json recording outputs."
                      << std::endl;
            outcome.error_message =
                "failed to update recording_snapshot.json recording outputs";
            outcome.external_recorder_error = external_recorder_error;
            outcome.crop_external_recorder_error = crop_external_recorder_error;
            return outcome;
        }
    }

    outcome.ok = true;
    outcome.external_recorder_error = external_recorder_error;
    outcome.crop_external_recorder_error = crop_external_recorder_error;
    return outcome;
}

bool gui_complete_recording_finalize(
    GuiRecordingFinalizeInputs* inputs,
    GuiRecordingFinalizeOutcome&& outcome,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control)
{
    if (!inputs || !inputs->valid || !run) {
        return false;
    }

    // Return lifecycle ownership to the session first (the states are
    // stopped by the run phase on both outcomes). On failure this restores
    // the exact pre-phased retry surface: the per-frame gate reopens
    // (finalizing stays latched below) and the retried finalize re-moves and
    // re-stops the lifecycles, which no-ops because started is false; the
    // manifest writes are trunc-overwrites so re-running them is safe.
    if (recording_session) {
        recording_session->external_recorder_lifecycle =
            std::move(inputs->external_recorder_lifecycle);
        recording_session->external_crop_recorder_lifecycle =
            std::move(inputs->external_crop_recorder_lifecycle);
    }

    if (!outcome.ok) {
        std::cerr << "[GUI][recording] Recording session finalize failed"
                  << " (will retry): " << outcome.error_message << std::endl;
        if (recording_session) {
            // Loud failure for the GUI status line. finalizing stays latched
            // and the folder stays claimed, so no new recording can start
            // into the unfinalized folder and the finalize retries.
            recording_session->external_recorder_last_error = outcome.error_message;
        }
        inputs->valid = false;
        return false;
    }

    const std::filesystem::path manifest_path =
        std::filesystem::path(run->recording_folder) / "recording_session.json";
    if (camera_control) {
        camera_control->preserve_recording_session_state = false;
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        if (camera_control->recording_folder == run->recording_folder) {
            camera_control->recording_folder.clear();
        }
        if (camera_control->recording_output_folder == run->recording_folder) {
            camera_control->recording_output_folder.clear();
        }
    }
    std::cout << "[GUI][recording] Wrote recording session manifest: "
              << manifest_path.string() << std::endl;
    if (inputs->external_ipc && recording_session) {
        recording_session->active_external_recorder_contract = nlohmann::json::object();
        recording_session->external_recorder_contract_path.clear();
        recording_session->external_recorder_supervisor_plan_path.clear();
        recording_session->external_recorder_last_error = outcome.external_recorder_error;
    }
    if (inputs->crop_external_recorder_active && recording_session) {
        recording_session->active_external_crop_recorder_contract = nlohmann::json::object();
        recording_session->external_crop_recorder_contract_path.clear();
        recording_session->external_crop_recorder_supervisor_plan_path.clear();
        recording_session->external_crop_recorder_last_error = outcome.crop_external_recorder_error;
    }
    run->active = false;
    run->finalizing = false;
    run->finalized = true;
    inputs->valid = false;
    return true;
}

namespace {

// Joins the worker and runs F3 on the calling (GUI) thread. The caller must
// have observed done == true or accept blocking until the worker finishes.
bool gui_complete_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control)
{
    if (state->worker.joinable()) {
        state->worker.join();
    }
    state->active = false;
    const bool finalized = gui_complete_recording_finalize(
        &state->inputs,
        std::move(state->outcome),
        run,
        recording_session,
        camera_control);
    state->inputs = GuiRecordingFinalizeInputs{};
    state->outcome = GuiRecordingFinalizeOutcome{};
    state->progress.stage.store(
        static_cast<int>(GuiRecordingFinalizeStage::kIdle),
        std::memory_order_relaxed);
    state->progress.clips_done.store(0, std::memory_order_relaxed);
    state->progress.clips_total.store(0, std::memory_order_relaxed);
    return finalized;
}

void gui_launch_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const int crop_size_px,
    const nlohmann::json& gui_display_frame_rate)
{
    state->inputs = gui_prepare_recording_finalize(
        run,
        recording_session,
        cameras_params,
        cameras_select,
        num_cameras,
        crop_size_px,
        gui_display_frame_rate);
    if (!state->inputs.valid) {
        return;
    }
    state->outcome = GuiRecordingFinalizeOutcome{};
    state->progress.stage.store(
        static_cast<int>(GuiRecordingFinalizeStage::kStoppingRecorders),
        std::memory_order_relaxed);
    state->progress.clips_done.store(0, std::memory_order_relaxed);
    state->progress.clips_total.store(0, std::memory_order_relaxed);
    state->started_at = std::chrono::steady_clock::now();
    state->done.store(false, std::memory_order_release);
    state->active = true;
    GuiAsyncRecordingFinalizeState* worker_state = state;
    state->worker = std::thread([worker_state]() {
        // Thread boundary (docs/error_handling_convention.md): an exception
        // escaping a raw std::thread would std::terminate the whole process.
        // Catch, report through the outcome slot, and let the GUI poll
        // surface the failure loudly (finalizing stays latched; the gate
        // retries).
        try {
            worker_state->outcome = gui_run_recording_finalize(
                &worker_state->inputs,
                &worker_state->progress);
        } catch (const std::exception& ex) {
            worker_state->outcome = GuiRecordingFinalizeOutcome{};
            worker_state->outcome.error_message =
                std::string("recording finalize threw: ") + ex.what();
        } catch (...) {
            worker_state->outcome = GuiRecordingFinalizeOutcome{};
            worker_state->outcome.error_message =
                "recording finalize threw a non-std exception";
        }
        worker_state->done.store(true, std::memory_order_release);
    });
    std::cout << "[GUI][recording] Recording finalize pending"
              << " folder=" << state->inputs.run.recording_folder
              << " sink_mode=" << state->inputs.run.recording_sink_mode
              << std::endl;
}

}  // namespace

bool gui_poll_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const int crop_size_px,
    const nlohmann::json& gui_display_frame_rate)
{
    if (!state || !run) {
        return false;
    }
    if (state->active) {
        if (!state->done.load(std::memory_order_acquire)) {
            return false;
        }
        return gui_complete_async_recording_finalize(
            state, run, recording_session, camera_control);
    }
    if (!gui_recording_finalize_gate_ready(run, recording_session, camera_control)) {
        return false;
    }
    gui_launch_async_recording_finalize(
        state,
        run,
        recording_session,
        cameras_params,
        cameras_select,
        num_cameras,
        crop_size_px,
        gui_display_frame_rate);
    return false;
}

bool gui_join_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control)
{
    if (!state || !run) {
        return false;
    }
    if (!state->active) {
        if (state->worker.joinable()) {
            state->worker.join();
        }
        return false;
    }
    std::cout << "[GUI][recording] Waiting for the in-flight recording finalize"
                 " (recorder shutdown is bounded by its graceful timeout)..."
              << std::endl;
    return gui_complete_async_recording_finalize(
        state, run, recording_session, camera_control);
}

GuiRecordingFinalizeProgressView gui_async_recording_finalize_progress_view(
    const GuiAsyncRecordingFinalizeState& state)
{
    GuiRecordingFinalizeProgressView view;
    view.pending = state.active;
    view.stage = state.progress.stage.load(std::memory_order_relaxed);
    view.clips_done = state.progress.clips_done.load(std::memory_order_relaxed);
    view.clips_total = state.progress.clips_total.load(std::memory_order_relaxed);
    return view;
}

bool gui_finalize_recording_session_if_ready(GuiRecordingRunState* run,
                                             orange::session::RecordingSessionState* recording_session,
                                             CameraControl* camera_control,
                                             const CameraParams* cameras_params,
                                             const CameraEachSelect* cameras_select,
                                             const int num_cameras,
                                             const int crop_size_px,
                                             const nlohmann::json& gui_display_frame_rate)
{
    if (!gui_recording_finalize_gate_ready(run, recording_session, camera_control)) {
        return false;
    }
    GuiRecordingFinalizeInputs inputs = gui_prepare_recording_finalize(
        run,
        recording_session,
        cameras_params,
        cameras_select,
        num_cameras,
        crop_size_px,
        gui_display_frame_rate);
    if (!inputs.valid) {
        return false;
    }
    GuiRecordingFinalizeOutcome outcome =
        gui_run_recording_finalize(&inputs, nullptr);
    return gui_complete_recording_finalize(
        &inputs,
        std::move(outcome),
        run,
        recording_session,
        camera_control);
}
