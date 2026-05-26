#include "recording_output_descriptor.h"

namespace orange::session {

namespace {

void add_string_if_nonempty(nlohmann::json& out,
                            const char* key,
                            const std::string& value)
{
    if (!value.empty()) {
        out[key] = value;
    }
}

}  // namespace

nlohmann::json build_recording_output_descriptor_json(
    const RecordingOutputDescriptor& output)
{
    nlohmann::json out = {
        {"schema_version", 1},
        {"camera_serial", output.camera_serial},
        {"output_kind", output.output_kind.empty() ? "full" : output.output_kind},
        {"role", output.role.empty() ? "sidecar" : output.role},
        {"backend", output.backend.empty() ? "unknown" : output.backend},
        {"status", output.status.empty() ? "unknown" : output.status}
    };

    add_string_if_nonempty(out, "video", output.video_path);
    add_string_if_nonempty(out, "metadata", output.metadata_path);
    add_string_if_nonempty(out, "keyframes", output.keyframe_path);
    add_string_if_nonempty(out, "perf", output.perf_path);
    add_string_if_nonempty(out, "sidecar_perf", output.sidecar_perf_path);
    add_string_if_nonempty(out, "summary", output.summary_path);
    add_string_if_nonempty(out, "codec", output.codec);
    add_string_if_nonempty(out, "container", output.container);
    add_string_if_nonempty(out, "tuning", output.tuning);
    add_string_if_nonempty(out, "pixel_source_format", output.pixel_source_format);
    add_string_if_nonempty(out, "encoded_format", output.encoded_format);
    add_string_if_nonempty(out, "coordinate_space", output.coordinate_space);

    if (output.width > 0) {
        out["width"] = output.width;
    }
    if (output.height > 0) {
        out["height"] = output.height;
    }
    if (output.frame_rate > 0) {
        out["frame_rate"] = output.frame_rate;
    }
    if (output.frame_count > 0 ||
        output.first_recording_frame_id > 0 ||
        output.last_recording_frame_id > 0) {
        out["frame_count"] = output.frame_count;
        out["first_recording_frame_id"] = output.first_recording_frame_id;
        out["last_recording_frame_id"] = output.last_recording_frame_id;
        out["recording_frame_id_gaps"] = output.recording_frame_id_gaps;
    }
    if (!output.packet_count_source.empty()) {
        out["packet_count"] = output.packet_count;
        out["packet_count_source"] = output.packet_count_source;
    }
    if (output.details.is_object() && !output.details.empty()) {
        out["details"] = output.details;
    }
    return out;
}

nlohmann::json build_recording_outputs_json(
    const std::vector<RecordingOutputDescriptor>& outputs)
{
    nlohmann::json grouped = nlohmann::json::object();
    for (const RecordingOutputDescriptor& output : outputs) {
        if (output.camera_serial.empty()) {
            continue;
        }
        const std::string output_kind =
            output.output_kind.empty() ? "full" : output.output_kind;
        if (!grouped.contains(output.camera_serial) ||
            !grouped[output.camera_serial].is_object()) {
            grouped[output.camera_serial] = nlohmann::json::object();
        }
        grouped[output.camera_serial][output_kind] =
            build_recording_output_descriptor_json(output);
    }
    return grouped;
}

RecordingOutputDescriptor build_full_recording_output_descriptor(
    const RecordingSessionCameraArtifact& artifact,
    const std::string& backend,
    const std::string& status)
{
    RecordingOutputDescriptor output;
    output.camera_serial = artifact.camera_serial;
    output.output_kind = "full";
    output.role = "ingest_authoritative";
    output.backend = backend.empty() ? "in_process" : backend;
    output.status = status.empty() ? "finalized" : status;
    output.video_path = artifact.video_path;
    output.metadata_path = artifact.metadata_path;
    output.keyframe_path = artifact.keyframe_path;
    output.frame_count = artifact.frame_count;
    output.first_recording_frame_id = artifact.first_recording_frame_id;
    output.last_recording_frame_id = artifact.last_recording_frame_id;
    output.recording_frame_id_gaps = artifact.recording_frame_id_gaps;
    output.packet_count = artifact.packet_count;
    output.packet_count_source = artifact.packet_count_source;
    output.container = "mp4";
    output.coordinate_space = "full_frame_pixels";
    return output;
}

std::vector<RecordingOutputDescriptor> build_full_recording_output_descriptors(
    const std::vector<RecordingSessionCameraArtifact>& artifacts,
    const std::string& backend,
    const std::string& status)
{
    std::vector<RecordingOutputDescriptor> outputs;
    outputs.reserve(artifacts.size());
    for (const RecordingSessionCameraArtifact& artifact : artifacts) {
        if (artifact.camera_serial.empty()) {
            continue;
        }
        outputs.push_back(build_full_recording_output_descriptor(
            artifact,
            backend,
            status));
    }
    return outputs;
}

}  // namespace orange::session
