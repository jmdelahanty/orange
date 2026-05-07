#pragma once

#include "modern_recording_pipeline.h"
#include "recording_config_state.h"
#include "video_capture.h"

#include <memory>
#include <string>
#include <vector>

namespace orange::session {

struct RecordingSessionState {
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::string recording_sink_mode = "real";
};

struct RecordingRunStartResult {
    bool ok = false;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
};

void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* camera_resources,
                                           CameraControl* camera_control);
RecordingRunStartResult begin_recording_run(CameraControl* camera_control,
                                            CameraParams* cameras_params,
                                            int num_cameras,
                                            const std::string& base_folder,
                                            PTPParams* ptp_params,
                                            const std::string& recording_sink_mode = "real");
void request_stop_recording_run(CameraControl* camera_control);
void request_drain_recording_run(RecordingSessionState* state, CameraControl* camera_control);
std::string current_recording_folder(CameraControl* camera_control);
void start_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void request_stop_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void shutdown_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void clear_recording_pipelines(RecordingSessionState* state);
RecordingIngress* recording_ingress_for_camera(const RecordingSessionState& state, int camera_index);

}  // namespace orange::session
