// ImGui renderers for the session-status module, extracted verbatim from
// src/orange.cpp. Kept in a separate translation unit from
// gui/session_status.cpp so the session_status_tests executable links the
// pure builders without ImGui objects or the CUDA-linked YoloWorker;
// render_gui_external_recorder_status_line is only called from this
// translation unit, so it stays in an anonymous namespace here. All
// renderers are text-only (Text/TextColored/TextDisabled/TextWrapped,
// Separator, SameLine, PushStyleColor) - no interactive widgets, so no
// ImGui ID-stack concerns.

#include "gui/session_status.h"

#include "imgui.h"
#include "session/recording_session.h"
#include "yolo_worker.h"

void render_gui_session_timing_status(
    const GuiSessionTimingSnapshot& timing,
    double streaming_fps_value,
    YoloWorker* yolo_worker)
{
    if (timing.stream_running) {
        ImGui::Text("Stream: %s", timing.stream_elapsed.c_str());
    } else {
        ImGui::TextDisabled("Stream idle");
    }

    ImGui::SameLine();
    if (timing.recording_running) {
        ImGui::TextColored(
            ImVec4{0.0f, 1.0f, 0.0f, 1.0f},
            "Recording: %s",
            timing.recording_elapsed.c_str());
    } else if (timing.recording_starting) {
        ImGui::TextColored(
            ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
            "Starting external recorder: %s",
            timing.starting_elapsed.c_str());
    } else if (timing.recording_finalizing) {
        ImGui::TextColored(
            ImVec4{1.0f, 1.0f, 0.0f, 1.0f},
            "Finalizing: %s",
            timing.finalizing_elapsed.c_str());
        if (timing.finalize_progress_visible) {
            // Same styling family as the recording-starting line above.
            if (timing.finalize_clips_total > 0) {
                ImGui::TextColored(
                    ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                    "Finalize: %s (clip %d of %d)",
                    timing.finalize_stage_label.c_str(),
                    timing.finalize_clips_done,
                    timing.finalize_clips_total);
            } else {
                ImGui::TextColored(
                    ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                    "Finalize: %s",
                    timing.finalize_stage_label.c_str());
            }
        }
        ImGui::TextDisabled("Recorded: %s", timing.recording_elapsed.c_str());
    } else if (timing.has_recording_elapsed) {
        ImGui::TextDisabled("Last recording: %s", timing.recording_elapsed.c_str());
    } else {
        ImGui::TextDisabled("Recording idle");
    }

    ImGui::Text("Streaming FPS: %.1f", streaming_fps_value);
    if (yolo_worker) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.0f, 1.0f),
            "YOLO FPS: %.1f",
            yolo_worker->get_fps());
    }
}

namespace {

void render_gui_external_recorder_status_line(
    const GuiExternalRecorderStatusLine& line)
{
    if (!line.visible) {
        return;
    }

    ImVec4 color{0.7f, 0.7f, 0.7f, 1.0f};
    if (line.status == "running") {
        color = ImVec4{0.25f, 0.85f, 0.35f, 1.0f};
    } else if (line.status == "degraded") {
        color = ImVec4{1.0f, 0.78f, 0.15f, 1.0f};
    } else if (line.status == "error") {
        color = ImVec4{1.0f, 0.25f, 0.20f, 1.0f};
    }

    ImGui::TextColored(
        color,
        "%s: %s (%d/%d running, %d/%d sockets, %d/%d status)",
        line.label.c_str(),
        line.status.c_str(),
        line.active_count,
        line.process_count,
        line.socket_ready_count,
        line.process_count,
        line.recorder_status_valid_count,
        line.process_count);
    if (!line.recorder_status_detail.empty()) {
        ImGui::TextDisabled(
            "%s, total rx=%llu enc=%llu",
            line.recorder_status_detail.c_str(),
            static_cast<unsigned long long>(line.frames_received),
            static_cast<unsigned long long>(line.frames_encoded));
    }
    if (line.storage_checked_count > 0) {
        const bool storage_healthy =
            line.storage_healthy_count == line.storage_checked_count &&
            line.storage_low_space_count == 0;
        ImVec4 storage_color = storage_healthy
            ? ImVec4{0.25f, 0.85f, 0.35f, 1.0f}
            : ImVec4{1.0f, 0.25f, 0.20f, 1.0f};
        const std::string min_available =
            line.storage_has_min_available_bytes
                ? gui_format_storage_bytes(line.storage_min_available_bytes)
                : "unknown";
        ImGui::TextColored(
            storage_color,
            "Storage: %d/%d healthy, low=%d",
            line.storage_healthy_count,
            line.storage_checked_count,
            line.storage_low_space_count);
        ImGui::TextDisabled(
            "Storage paths: %llu/%llu ok, min available=%s",
            static_cast<unsigned long long>(line.storage_paths_ok_count),
            static_cast<unsigned long long>(line.storage_path_count),
            min_available.c_str());
        if (!line.storage_status_detail.empty()) {
            ImGui::TextDisabled("%s", line.storage_status_detail.c_str());
        }
    }
    if (!line.rolling_status_detail.empty()) {
        ImGui::TextDisabled(
            "Rolling: %s (%d/%d streams)",
            line.rolling_status_detail.c_str(),
            line.rolling_process_count,
            line.process_count);
    }
    if (!line.rolling_last_rollover_detail.empty()) {
        ImGui::TextDisabled(
            "Last rollover: %s",
            line.rolling_last_rollover_detail.c_str());
    }
    if (!line.error.empty()) {
        ImGui::TextWrapped("%s", line.error.c_str());
    }
}

}  // namespace

void render_gui_external_recorder_status(
    const orange::session::RecordingSessionState& recording_session)
{
    render_gui_external_recorder_status_line(
        gui_external_recorder_status_line(
            "External recorder",
            recording_session.external_recorder_lifecycle,
            recording_session.external_recorder_last_error));
    render_gui_external_recorder_status_line(
        gui_external_recorder_status_line(
            "Crop recorder",
            recording_session.external_crop_recorder_lifecycle,
            recording_session.external_crop_recorder_last_error));
}
