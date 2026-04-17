#pragma once

#include "encoder_pipeline.h"

#include <functional>
#include <string>
#include <vector>

struct RecordingValidationGpuPathInfo {
    int source_gpu_id = -1;
    int helper_gpu_id = -1;
    std::string topology_class;
    std::string topology_error;
    bool can_access_peer = false;
    bool can_access_peer_known = false;
};

struct RecordingValidationCameraInput {
    int camera_index = -1;
    std::string camera_serial;
    bool record_enabled = false;
    int source_gpu_id = -1;
    RecordingStrategyConfig strategy;
    CameraRecordingConstraintsConfig constraints;
};

struct HelperGpuValidationSummary {
    int helper_gpu_id = -1;
    std::string topology_class;
    std::string topology_error;
    bool can_access_peer = false;
    bool can_access_peer_known = false;
    bool topology_matches_preference = true;
    bool peer_requirement_satisfied = true;
};

struct CameraRecordingValidationSummary {
    int camera_index = -1;
    std::string camera_serial;
    bool record_enabled = false;
    bool split_gop_enabled = false;
    int source_gpu_id = -1;
    std::vector<int> claimed_gpu_ids;
    std::vector<int> helper_gpu_ids;
    std::vector<HelperGpuValidationSummary> helpers;
    std::vector<std::string> local_errors;
    std::vector<std::string> session_errors;
    std::string preferred_topology_class;
    bool require_peer_access = false;
    std::string transfer_mode;
    std::string source_encoder_policy;

    bool valid() const { return local_errors.empty() && session_errors.empty(); }
};

using RecordingValidationGpuPathLookup =
    std::function<RecordingValidationGpuPathInfo(int source_gpu_id, int helper_gpu_id)>;

std::vector<int> build_recording_claimed_gpu_ids(int source_gpu_id,
                                                 const std::vector<int>& encode_gpu_ids);
std::vector<int> build_recording_helper_gpu_ids(int source_gpu_id,
                                                const std::vector<int>& encode_gpu_ids);

std::vector<CameraRecordingValidationSummary> validate_recording_configuration(
    const std::vector<RecordingValidationCameraInput>& cameras,
    const RecordingValidationGpuPathLookup& gpu_path_lookup);
