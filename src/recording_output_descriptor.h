#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::session {

struct RecordingSessionCameraArtifact {
    std::string camera_serial;
    std::string video_path;
    std::string metadata_path;
    std::string keyframe_path;
    uint64_t frame_count = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
    uint64_t packet_count = 0;
    std::string packet_count_source;
};

struct RecordingOutputDescriptor {
    std::string camera_serial;
    std::string output_kind = "full";
    std::string role = "ingest_authoritative";
    std::string backend = "in_process";
    std::string status = "finalized";
    std::string video_path;
    std::string metadata_path;
    std::string keyframe_path;
    std::string perf_path;
    std::string sidecar_perf_path;
    std::string summary_path;
    uint64_t frame_count = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
    uint64_t packet_count = 0;
    std::string packet_count_source;
    int width = 0;
    int height = 0;
    int frame_rate = 0;
    std::string codec;
    std::string container;
    std::string tuning;
    std::string pixel_source_format;
    std::string encoded_format;
    std::string coordinate_space;
    nlohmann::json details = nlohmann::json::object();
};

nlohmann::json build_recording_output_descriptor_json(
    const RecordingOutputDescriptor& output);
nlohmann::json build_recording_outputs_json(
    const std::vector<RecordingOutputDescriptor>& outputs);
RecordingOutputDescriptor build_full_recording_output_descriptor(
    const RecordingSessionCameraArtifact& artifact,
    const std::string& backend,
    const std::string& status);
std::vector<RecordingOutputDescriptor> build_full_recording_output_descriptors(
    const std::vector<RecordingSessionCameraArtifact>& artifacts,
    const std::string& backend,
    const std::string& status);

}  // namespace orange::session
