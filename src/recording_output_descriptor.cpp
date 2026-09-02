#include "recording_output_descriptor.h"

#include <set>
#include <utility>

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
    const std::string output_kind =
        output.output_kind.empty() ? "full" : output.output_kind;
    const int schema_version =
        output_kind == "crop"
            ? 2
            : (output_kind == kSpatialRoiRecordingOutputKind ? 3 : 1);
    nlohmann::json out = {
        {"schema_version", schema_version},
        {"camera_serial", output.camera_serial},
        {"output_kind", output_kind},
        {"role", output.role.empty() ? "sidecar" : output.role},
        {"backend", output.backend.empty() ? "unknown" : output.backend},
        {"status", output.status.empty() ? "unknown" : output.status}
    };

    add_string_if_nonempty(out, "logical_stream_id", output.logical_stream_id);
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
    add_string_if_nonempty(
        out,
        "video_pixel_coordinate_space",
        output.video_pixel_coordinate_space);
    add_string_if_nonempty(
        out,
        "source_geometry_coordinate_space",
        output.source_geometry_coordinate_space);

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
    if (output.encoding_budget.is_object() &&
        !output.encoding_budget.empty()) {
        out["encoding_budget"] = output.encoding_budget;
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
        // The schema-2 index has scalar output-kind slots. Keep it a
        // compatibility view for full/crop consumers and do not let a new
        // collection-valued spatial ROI stream replace either slot (or the
        // previous ROI stream) by accident.
        if (output_kind == kSpatialRoiRecordingOutputKind) {
            continue;
        }
        if (!grouped.contains(output.camera_serial) ||
            !grouped[output.camera_serial].is_object()) {
            grouped[output.camera_serial] = nlohmann::json::object();
        }
        grouped[output.camera_serial][output_kind] =
            build_recording_output_descriptor_json(output);
    }
    return grouped;
}

nlohmann::json build_recording_outputs_v3_json(
    const std::vector<RecordingOutputDescriptor>& outputs)
{
    nlohmann::json versioned;
    if (!build_recording_outputs_v3_json(outputs, &versioned, nullptr)) {
        // The convenience overload cannot expose a diagnostic. Returning
        // null is an explicit fail-closed signal; callers that need to
        // report the reason should use the bool/error overload.
        return nlohmann::json();
    }
    return versioned;
}

bool build_recording_outputs_v3_json(
    const std::vector<RecordingOutputDescriptor>& outputs,
    nlohmann::json* output_out,
    std::string* error_out)
{
    if (!output_out) {
        if (error_out) {
            *error_out = "output_out is null";
        }
        return false;
    }

    nlohmann::json versioned = {
        {"schema_id", kRecordingOutputsV3SchemaId},
        {"schema_version", kRecordingOutputsV3SchemaVersion},
        {"cameras", nlohmann::json::object()}
    };
    std::set<std::pair<std::string, std::string>> spatial_roi_keys;

    for (const RecordingOutputDescriptor& output : outputs) {
        if (output.camera_serial.empty()) {
            if (output.output_kind == kSpatialRoiRecordingOutputKind) {
                if (error_out) {
                    *error_out =
                        "spatial ROI descriptor has an empty camera_serial";
                }
                return false;
            }
            continue;
        }

        const std::string output_kind =
            output.output_kind.empty() ? "full" : output.output_kind;
        nlohmann::json& camera_outputs =
            versioned["cameras"][output.camera_serial];
        if (!camera_outputs.is_object()) {
            camera_outputs = nlohmann::json::object();
        }

        if (output_kind == kSpatialRoiRecordingOutputKind) {
            if (output.logical_stream_id.empty()) {
                if (error_out) {
                    *error_out =
                        "spatial ROI descriptor has an empty logical_stream_id";
                }
                return false;
            }
            if (!spatial_roi_keys.emplace(
                     output.camera_serial,
                     output.logical_stream_id)
                     .second) {
                if (error_out) {
                    *error_out =
                        "duplicate spatial ROI logical_stream_id for camera " +
                        output.camera_serial + ": " +
                        output.logical_stream_id;
                }
                return false;
            }

            const nlohmann::json descriptor =
                build_recording_output_descriptor_json(output);
            if (descriptor.value("camera_serial", std::string()) !=
                    output.camera_serial ||
                descriptor.value("output_kind", std::string()) !=
                    kSpatialRoiRecordingOutputKind ||
                descriptor.value("logical_stream_id", std::string()) !=
                    output.logical_stream_id) {
                if (error_out) {
                    *error_out =
                        "spatial ROI descriptor identity fields disagree with "
                        "its collection key";
                }
                return false;
            }
            if (!camera_outputs.contains(kSpatialRoiRecordingOutputKind) ||
                !camera_outputs[kSpatialRoiRecordingOutputKind].is_object()) {
                camera_outputs[kSpatialRoiRecordingOutputKind] =
                    nlohmann::json::object();
            }
            camera_outputs[kSpatialRoiRecordingOutputKind]
                         [output.logical_stream_id] =
                descriptor;
            continue;
        }

        // Full-frame remains an ordinary first-class scalar output. Crop is
        // also kept scalar for compatibility; only spatial_roi is a keyed
        // collection in v3.
        camera_outputs[output_kind] =
            build_recording_output_descriptor_json(output);
    }
    *output_out = std::move(versioned);
    return true;
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
    output.coordinate_space = kFullFramePixelCoordinateSpace;
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

void apply_crop_recording_output_media_contract(
    RecordingOutputDescriptor* output)
{
    if (!output) {
        return;
    }
    output->output_kind = "crop";
    output->role = kRuntimeDerivedAcquisitionInputRole;
    // Keep the historical alias so existing consumers continue to locate the
    // full-frame placement geometry. The two explicit fields are authoritative.
    output->coordinate_space = kFullFramePixelCoordinateSpace;
    output->video_pixel_coordinate_space = kCropFramePixelCoordinateSpace;
    output->source_geometry_coordinate_space = kFullFramePixelCoordinateSpace;
}

}  // namespace orange::session
