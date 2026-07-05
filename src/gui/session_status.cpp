// Session-timing and external-recorder status extracted verbatim from
// src/orange.cpp. The function bodies below are byte-identical to their
// previous definitions in orange.cpp's anonymous namespace, except that
// the two nlohmann::json stop_control default arguments moved to the
// declarations in gui/session_status.h; helpers that are only called
// from this translation unit stay in an anonymous namespace here.

#include "gui/session_status.h"

#include "gui/recording_finalizer.h"

#include "external_recorder_lifecycle.h"
#include "global.h"
#include "project.h"
#include "session/recording_session.h"
#include "video_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

std::chrono::seconds gui_elapsed_since(
    const std::chrono::steady_clock::time_point& started_at,
    const std::chrono::steady_clock::time_point& now)
{
    if (!has_gui_timepoint(started_at) || now < started_at) {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(now - started_at);
}

void gui_note_recording_started(GuiRecordingRunState* run,
                                CameraControl* camera_control,
                                const std::string& recording_folder,
                                const std::string& recording_sink_mode)
{
    if (!run) {
        return;
    }
    run->active = true;
    run->finalizing = false;
    run->finalized = false;
    run->recording_folder = recording_folder;
    run->recording_sink_mode = recording_sink_mode.empty() ? "real" : recording_sink_mode;
    run->recording_started_at = std::chrono::steady_clock::now();
    run->recording_started_at_utc = get_current_utc_timestamp();
    run->recording_stop_requested_at = {};
    run->recording_stop_requested_at_utc.clear();
    run->recording_drained_at = {};
    run->recording_drained_at_utc.clear();
    run->stop_reason = "manual_stop";
    run->stop_control = nlohmann::json::object();
    run->diagnostic_finalize_stall_reported = false;
    if (camera_control) {
        camera_control->preserve_recording_session_state = true;
    }
}

void gui_note_recording_stop_requested(GuiRecordingRunState* run,
                                       const std::string& stop_reason,
                                       nlohmann::json stop_control)
{
    if (!run || !run->active) {
        return;
    }
    if (!run->finalizing) {
        run->recording_stop_requested_at = std::chrono::steady_clock::now();
        run->recording_stop_requested_at_utc = get_current_utc_timestamp();
        run->stop_control =
            stop_control.is_object() ? std::move(stop_control) : nlohmann::json::object();
    }
    run->finalizing = true;
    run->stop_reason = stop_reason.empty() ? "manual_stop" : stop_reason;
}

void gui_mark_stream_started(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    timing->stream_running = true;
    timing->stream_started_at = now;
    timing->recording_starting = false;
    timing->recording_running = false;
    timing->recording_finalizing = false;
    timing->recording_start_pending_at = {};
    timing->recording_started_at = {};
    timing->finalizing_started_at = {};
    timing->last_recording_elapsed = std::chrono::seconds{0};
}

void gui_mark_stream_stopped(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    *timing = GuiSessionTimingState{};
}

void gui_mark_recording_started(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    timing->recording_starting = false;
    timing->recording_start_pending_at = {};
    timing->recording_running = true;
    timing->recording_finalizing = false;
    timing->recording_started_at = std::chrono::steady_clock::now();
    timing->finalizing_started_at = {};
    timing->last_recording_elapsed = std::chrono::seconds{0};
}

void gui_mark_recording_finalizing(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (timing->recording_running) {
        timing->last_recording_elapsed =
            gui_elapsed_since(timing->recording_started_at, now);
    }
    timing->recording_running = false;
    if (!timing->recording_finalizing) {
        timing->finalizing_started_at = now;
    }
    timing->recording_finalizing = true;
}

void gui_mark_recording_finished(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    timing->recording_running = false;
    timing->recording_finalizing = false;
    timing->recording_started_at = {};
    timing->finalizing_started_at = {};
}

void gui_request_recording_stop_through_operator_path(
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    const std::string& stop_reason,
    nlohmann::json stop_control)
{
    gui_note_recording_stop_requested(
        recording_run,
        stop_reason,
        std::move(stop_control));
    orange::session::request_drain_recording_run(recording_session, camera_control);
    gui_mark_recording_finalizing(timing);
    try_stop_timer();
    if (camera_control && !camera_control->recording_draining) {
        gui_mark_recording_finished(timing);
    }
}

GuiSessionTimingSnapshot gui_session_timing_snapshot(
    GuiSessionTimingState* timing,
    const CameraControl* camera_control,
    const bool recording_start_pending)
{
    GuiSessionTimingSnapshot snapshot;
    if (!timing) {
        return snapshot;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool stream_active = camera_control && camera_control->subscribe;
    const bool recording_active = camera_control && camera_control->record_video;
    const bool recording_draining = camera_control && camera_control->recording_draining;

    if (stream_active && !timing->stream_running) {
        timing->stream_running = true;
        timing->stream_started_at = now;
    } else if (!stream_active && timing->stream_running) {
        gui_mark_stream_stopped(timing);
    }

    // Pending-start reconciliation. CameraControl stays authoritative: an
    // active recording always wins over a stale pending marker (the pending
    // window ends exactly when record_video flips true at completion, or
    // when the start fails/cancels and the caller stops reporting pending).
    if (recording_start_pending && !recording_active && !timing->recording_starting) {
        timing->recording_starting = true;
        timing->recording_start_pending_at = now;
    } else if ((!recording_start_pending || recording_active) &&
               timing->recording_starting) {
        timing->recording_starting = false;
        timing->recording_start_pending_at = {};
    }

    if (recording_active && !timing->recording_running) {
        gui_mark_recording_started(timing);
    } else if (!recording_active && recording_draining && !timing->recording_finalizing) {
        gui_mark_recording_finalizing(timing);
    } else if (!recording_active && !recording_draining &&
               (timing->recording_running || timing->recording_finalizing)) {
        gui_mark_recording_finished(timing);
    }

    snapshot.stream_running = timing->stream_running;
    snapshot.recording_starting = timing->recording_starting;
    snapshot.recording_running = timing->recording_running;
    snapshot.recording_finalizing = timing->recording_finalizing;
    snapshot.has_recording_elapsed =
        timing->recording_running ||
        timing->recording_finalizing ||
        timing->last_recording_elapsed.count() > 0;

    if (timing->stream_running) {
        snapshot.stream_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->stream_started_at, now));
    }
    if (timing->recording_starting) {
        snapshot.starting_elapsed =
            format_elapsed_time(
                gui_elapsed_since(timing->recording_start_pending_at, now));
    }
    if (timing->recording_running) {
        snapshot.recording_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->recording_started_at, now));
    } else if (snapshot.has_recording_elapsed) {
        snapshot.recording_elapsed = format_elapsed_time(timing->last_recording_elapsed);
    }
    if (timing->recording_finalizing) {
        snapshot.finalizing_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->finalizing_started_at, now));
    }
    return snapshot;
}

std::string gui_format_storage_bytes(uint64_t bytes)
{
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);
    while (value >= 1024.0 && unit_index + 1 < kUnitCount) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream out;
    if (unit_index == 0 || value >= 100.0) {
        out << std::fixed << std::setprecision(0);
    } else if (value >= 10.0) {
        out << std::fixed << std::setprecision(1);
    } else {
        out << std::fixed << std::setprecision(2);
    }
    out << value << ' ' << kUnits[unit_index];
    return out.str();
}

GuiExternalRecorderStatusLine gui_external_recorder_status_line(
    const char* label,
    const orange::external_recorder::SupervisedRecorderLifecycleState& state,
    const std::string& last_error)
{
    GuiExternalRecorderStatusLine line;
    line.label = label ? label : "External recorder";

    for (const orange::external_recorder::RecorderProcessState& process :
         state.runtime.processes) {
        ++line.process_count;
        if (process.active) {
            ++line.active_count;
        }
        if (process.socket_ready) {
            ++line.socket_ready_count;
        }
        const auto& recorder_status = process.recorder_status;
        if (recorder_status.present) {
            ++line.recorder_status_present_count;
            if (recorder_status.valid) {
                ++line.recorder_status_valid_count;
                line.frames_received += recorder_status.frames_received;
                line.frames_encoded += recorder_status.frames_encoded;
                if (recorder_status.storage_checked) {
                    ++line.storage_checked_count;
                    const bool storage_healthy =
                        recorder_status.storage_ok &&
                        !recorder_status.storage_low_space;
                    if (storage_healthy) {
                        ++line.storage_healthy_count;
                    }
                    if (recorder_status.storage_low_space) {
                        ++line.storage_low_space_count;
                    }
                    line.storage_path_count += recorder_status.storage_path_count;
                    line.storage_paths_ok_count +=
                        recorder_status.storage_paths_ok_count;
                    if (recorder_status.storage_has_min_available_bytes &&
                        (!line.storage_has_min_available_bytes ||
                         recorder_status.storage_min_available_bytes <
                             line.storage_min_available_bytes)) {
                        line.storage_min_available_bytes =
                            recorder_status.storage_min_available_bytes;
                        line.storage_has_min_available_bytes = true;
                    }
                    if (line.storage_status_detail.empty()) {
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        std::ostringstream detail;
                        detail
                            << camera << " storage="
                            << (storage_healthy
                                    ? "ok"
                                    : (recorder_status.storage_low_space
                                           ? "low_space"
                                           : "failed"))
                            << " paths=" << recorder_status.storage_paths_ok_count
                            << "/" << recorder_status.storage_path_count;
                        if (recorder_status.storage_has_min_available_bytes) {
                            detail << " min_avail="
                                   << gui_format_storage_bytes(
                                          recorder_status
                                              .storage_min_available_bytes);
                        }
                        if (recorder_status.storage_min_free_bytes > 0) {
                            detail << " min_required="
                                   << gui_format_storage_bytes(
                                          recorder_status.storage_min_free_bytes);
                        }
                        if (recorder_status.storage_low_space_warning_bytes > 0) {
                            detail << " warn_below="
                                   << gui_format_storage_bytes(
                                          recorder_status
                                              .storage_low_space_warning_bytes);
                        }
                        line.storage_status_detail = detail.str();
                    }
                    if (!storage_healthy) {
                        ++line.error_count;
                        if (line.error.empty()) {
                            const std::string camera =
                                process.camera_serial.empty()
                                    ? process.stream_id
                                    : ("Cam" + process.camera_serial);
                            line.error = camera + ": recorder storage ";
                            line.error += recorder_status.storage_low_space
                                ? "below low-space threshold"
                                : "preflight failed";
                        }
                    }
                }
                if (recorder_status.rolling_enabled) {
                    ++line.rolling_process_count;
                    if (line.rolling_status_detail.empty()) {
                        line.rolling_current_clip_index =
                            recorder_status.rolling_current_clip_index;
                        line.rolling_clip_seconds =
                            recorder_status.rolling_clip_seconds;
                        line.rolling_frames_until_next_rollover =
                            recorder_status.rolling_frames_until_next_rollover;
                        line.rolling_next_rollover_at_recording_frame_id =
                            recorder_status.rolling_next_rollover_at_recording_frame_id;
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        std::ostringstream detail;
                        detail << camera
                               << " clip=" << recorder_status.rolling_current_clip_index
                               << " clip_s=" << recorder_status.rolling_clip_seconds
                               << " next_frame="
                               << recorder_status.rolling_next_rollover_at_recording_frame_id
                               << " frames_left="
                               << recorder_status.rolling_frames_until_next_rollover;
                        line.rolling_status_detail = detail.str();
                        if (recorder_status.rolling_completed_clip_count > 0) {
                            std::ostringstream last_detail;
                            last_detail
                                << camera
                                << " completed_clip="
                                << recorder_status.rolling_last_completed_clip_index
                                << " status="
                                << recorder_status.rolling_last_rollover_status
                                << " last_frame="
                                << recorder_status
                                       .rolling_last_completed_clip_last_recording_frame_id
                                << " frames="
                                << recorder_status.rolling_last_completed_clip_frame_count;
                            line.rolling_last_rollover_detail = last_detail.str();
                        }
                    }
                }
                if (line.recorder_status_detail.empty()) {
                    const std::string camera =
                        process.camera_serial.empty()
                            ? process.stream_id
                            : ("Cam" + process.camera_serial);
                    std::ostringstream detail;
                    detail << camera
                           << " recorder=" << recorder_status.status
                           << " seq=" << recorder_status.heartbeat_sequence
                           << " rx=" << recorder_status.frames_received
                           << " enc=" << recorder_status.frames_encoded;
                    line.recorder_status_detail = detail.str();
                }
                if (recorder_status.status == "failed" ||
                    recorder_status.worker_failed ||
                    !recorder_status.error.empty()) {
                    ++line.error_count;
                    if (line.error.empty()) {
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        line.error =
                            camera + ": recorder status " + recorder_status.status;
                        if (!recorder_status.error.empty()) {
                            line.error += ": " + recorder_status.error;
                        }
                    }
                }
            } else {
                ++line.error_count;
                if (line.error.empty()) {
                    const std::string camera =
                        process.camera_serial.empty()
                            ? process.stream_id
                            : ("Cam" + process.camera_serial);
                    line.error =
                        camera + ": invalid recorder status sidecar";
                    if (!recorder_status.error.empty()) {
                        line.error += ": " + recorder_status.error;
                    }
                }
            }
        }
        if (!process.error.empty()) {
            ++line.error_count;
            if (line.error.empty()) {
                line.error = process.camera_serial.empty()
                    ? process.error
                    : ("Cam" + process.camera_serial + ": " + process.error);
            }
        }
    }

    if (line.error.empty() && !state.last_artifact_error.empty()) {
        line.error = state.last_artifact_error;
        line.error_count = std::max(line.error_count, 1);
    }
    if (line.error.empty() && !state.last_runtime_error.empty()) {
        line.error = state.last_runtime_error;
        line.error_count = std::max(line.error_count, 1);
    }
    if (line.error.empty() && !last_error.empty()) {
        line.error = last_error;
        line.error_count = std::max(line.error_count, 1);
    }

    line.visible = state.started || !line.error.empty();
    if (!line.visible) {
        return line;
    }

    if (!line.error.empty() || line.error_count > 0) {
        line.status = "error";
    } else if (state.started &&
               line.process_count > 0 &&
               line.active_count == line.process_count &&
               line.socket_ready_count == line.process_count) {
        line.status = "running";
    } else if (state.started) {
        line.status = "degraded";
    } else {
        line.status = "stopped";
    }
    return line;
}

namespace {

void gui_refresh_external_recorder_lifecycle(
    orange::external_recorder::SupervisedRecorderLifecycleState* lifecycle,
    std::string* last_error)
{
    if (!lifecycle || !lifecycle->started) {
        return;
    }

    std::string error;
    if (!orange::external_recorder::RefreshSupervisedRecorderLifecycle(
            lifecycle,
            &error)) {
        if (last_error && last_error->empty()) {
            *last_error = error.empty()
                ? "external recorder supervisor process health check failed"
                : error;
        }
    }
}

}  // namespace

void gui_refresh_external_recorder_lifecycles(
    orange::session::RecordingSessionState* recording_session,
    const CameraControl* camera_control)
{
    if (!recording_session || !camera_control) {
        return;
    }
    if (!camera_control->record_video && !camera_control->recording_draining) {
        return;
    }

    gui_refresh_external_recorder_lifecycle(
        &recording_session->external_recorder_lifecycle,
        &recording_session->external_recorder_last_error);
    gui_refresh_external_recorder_lifecycle(
        &recording_session->external_crop_recorder_lifecycle,
        &recording_session->external_crop_recorder_last_error);
}
