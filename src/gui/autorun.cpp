#include "gui/autorun.h"
#include "gui/env_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string gui_normalized_path_string(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = std::filesystem::absolute(path, ec);
    }
    if (ec) {
        normalized = std::filesystem::path(path);
    }
    return normalized.lexically_normal().string();
}

int gui_find_local_config_folder(const std::vector<std::string>& folders,
                                 const std::string& requested_config_dir)
{
    if (requested_config_dir.empty()) {
        return -1;
    }
    const std::string requested = gui_normalized_path_string(requested_config_dir);
    for (std::size_t i = 0; i < folders.size(); ++i) {
        if (gui_normalized_path_string(folders[i]) == requested) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

namespace orange::gui {

GuiAutorunConfig resolve_gui_autorun_config()
{
    GuiAutorunConfig config;
    config.enabled = gui_env_flag_enabled("ORANGE_GUI_AUTORUN", false);
    config.stream_warmup_seconds =
        gui_env_int("ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS", 3, 0);
    config.record_seconds =
        gui_env_int("ORANGE_GUI_AUTORUN_RECORD_SECONDS", 10, 1);
    config.exit_after_finalize =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE", false);
    config.hide_crop_preview =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW", false);
    config.enable_stream =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_STREAM", true);
    config.enable_record =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_RECORD", true);
    config.enable_yolo =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_YOLO", true);
    config.enable_crop =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_CROP", true);
    config.start_recording =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_START_RECORDING", true);
    const char* config_dir = std::getenv("ORANGE_GUI_CONFIG_DIR");
    if (config_dir && *config_dir) {
        config.config_dir = config_dir;
    }
    return config;
}

const char* gui_autorun_stage_name(const GuiAutorunStage stage)
{
    switch (stage) {
        case GuiAutorunStage::kDisabled: return "disabled";
        case GuiAutorunStage::kSelectConfig: return "select_config";
        case GuiAutorunStage::kOpenCameras: return "open_cameras";
        case GuiAutorunStage::kStartStreaming: return "start_streaming";
        case GuiAutorunStage::kStreamWarmup: return "stream_warmup";
        case GuiAutorunStage::kStartRecording: return "start_recording";
        case GuiAutorunStage::kRecording: return "recording";
        case GuiAutorunStage::kStopRecording: return "stop_recording";
        case GuiAutorunStage::kWaitFinalize: return "wait_finalize";
        case GuiAutorunStage::kStopStreaming: return "stop_streaming";
        case GuiAutorunStage::kDone: return "done";
        case GuiAutorunStage::kFailed: return "failed";
    }
    return "unknown";
}

double gui_autorun_stage_elapsed_s(const GuiAutorunState& state)
{
    if (state.stage_started_at.time_since_epoch().count() == 0) {
        return 0.0;
    }
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.stage_started_at).count();
}

void gui_autorun_enter_stage(GuiAutorunState* state, const GuiAutorunStage stage)
{
    if (!state) {
        return;
    }
    state->stage = stage;
    state->stage_started_at = std::chrono::steady_clock::now();
    state->action_requested = false;
    std::cout << "[GUI][autorun] stage=" << gui_autorun_stage_name(stage)
              << std::endl;
}

void gui_autorun_fail(GuiAutorunState* state, const std::string& message)
{
    if (!state) {
        return;
    }
    state->error_message = message;
    std::cerr << "[GUI][autorun] " << message << std::endl;
    gui_autorun_enter_stage(state, GuiAutorunStage::kFailed);
}

GuiAutorunRequests gui_autorun_update(
    GuiAutorunState* state,
    const GuiAutorunConfig& config,
    const std::vector<std::string>& local_config_folders,
    int* local_config_select,
    const CameraControl* camera_control,
    const GuiRecordingRunState* recording_run,
    const bool calibration_tool_busy)
{
    GuiAutorunRequests requests;
    if (!state || !config.enabled || !camera_control) {
        return requests;
    }

    switch (state->stage) {
        case GuiAutorunStage::kDisabled:
            gui_autorun_enter_stage(state, GuiAutorunStage::kSelectConfig);
            break;

        case GuiAutorunStage::kSelectConfig: {
            const int folder_index =
                gui_find_local_config_folder(local_config_folders, config.config_dir);
            if (folder_index < 0) {
                gui_autorun_fail(
                    state,
                    "ORANGE_GUI_CONFIG_DIR was not found in local config folders: " +
                    config.config_dir);
                break;
            }
            if (local_config_select) {
                *local_config_select = folder_index;
            }
            std::cout << "[GUI][autorun] selected config folder: "
                      << local_config_folders[folder_index] << std::endl;
            gui_autorun_enter_stage(state, GuiAutorunStage::kOpenCameras);
            break;
        }

        case GuiAutorunStage::kOpenCameras:
            if (camera_control->open) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStartStreaming);
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.open_cameras = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting camera open" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 30.0) {
                gui_autorun_fail(state, "timed out opening cameras");
            }
            break;

        case GuiAutorunStage::kStartStreaming:
            if (camera_control->subscribe) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStreamWarmup);
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.toggle_streaming = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting stream start" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out starting stream");
            }
            break;

        case GuiAutorunStage::kStreamWarmup:
            if (!camera_control->subscribe) {
                gui_autorun_fail(state, "stream stopped during warmup");
            } else if (gui_autorun_stage_elapsed_s(*state) >=
                       static_cast<double>(config.stream_warmup_seconds)) {
                gui_autorun_enter_stage(
                    state,
                    config.start_recording
                        ? GuiAutorunStage::kStartRecording
                        : GuiAutorunStage::kDone);
            }
            break;

        case GuiAutorunStage::kStartRecording:
            if (camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kRecording);
            } else if (camera_control->recording_draining) {
                if (gui_autorun_stage_elapsed_s(*state) > 60.0) {
                    gui_autorun_fail(state, "recording was still draining before autorun start");
                }
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.toggle_recording = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting recording start" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out starting recording");
            }
            break;

        case GuiAutorunStage::kRecording:
            if (!camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (gui_autorun_stage_elapsed_s(*state) >=
                       static_cast<double>(config.record_seconds)) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopRecording);
            }
            break;

        case GuiAutorunStage::kStopRecording:
            if (!camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (!state->action_requested) {
                requests.toggle_recording = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting recording stop" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out stopping recording");
            }
            break;

        case GuiAutorunStage::kWaitFinalize: {
            const bool run_active = recording_run &&
                                    (recording_run->active || recording_run->finalizing);
            if (!camera_control->record_video &&
                !camera_control->recording_draining &&
                !run_active) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (gui_autorun_stage_elapsed_s(*state) > 300.0) {
                gui_autorun_fail(state, "timed out finalizing recording");
            }
            break;
        }

        case GuiAutorunStage::kStopStreaming:
            if (!camera_control->subscribe) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kDone);
            } else if (!state->action_requested) {
                requests.toggle_streaming = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting stream stop" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 90.0) {
                gui_autorun_fail(state, "timed out stopping stream");
            }
            break;

        case GuiAutorunStage::kDone:
            if (config.exit_after_finalize && !state->close_requested) {
                requests.close_window = true;
                state->close_requested = true;
                std::cout << "[GUI][autorun] requesting GUI exit after finalize" << std::endl;
            }
            break;

        case GuiAutorunStage::kFailed:
            if (config.exit_after_finalize && !state->close_requested) {
                requests.close_window = true;
                state->close_requested = true;
            }
            break;
    }

    return requests;
}

void apply_gui_autorun_camera_selection(const GuiAutorunConfig& config,
                                        CameraEachSelect* cameras_select,
                                        const int num_cameras)
{
    if (!config.enabled || !cameras_select || num_cameras <= 0) {
        return;
    }
    for (int i = 0; i < num_cameras; ++i) {
        cameras_select[i].stream_on = config.enable_stream;
        cameras_select[i].record = config.enable_record;
        cameras_select[i].yolo = config.enable_yolo;
        cameras_select[i].crop_and_encode = config.enable_crop;
        if (cameras_select[i].crop_and_encode) {
            cameras_select[i].record = true;
            cameras_select[i].yolo = true;
        }
    }
    std::cout << "[GUI][autorun] camera selection"
              << " stream=" << (config.enable_stream ? 1 : 0)
              << " record=" << (config.enable_record ? 1 : 0)
              << " yolo=" << (config.enable_yolo ? 1 : 0)
              << " crop=" << (config.enable_crop ? 1 : 0)
              << " cameras=" << num_cameras
              << std::endl;
}

}  // namespace orange::gui
