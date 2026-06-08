#include "calibration_image_set.h"

#include <filesystem>
#include <fstream>

namespace orange::calibration {
namespace {

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool is_allowed_purpose(const std::string& value)
{
    return value == "homography_grid" ||
           value == "scale_image" ||
           value == "dish_top_rim" ||
           value == "crosshair_alignment";
}

bool is_allowed_target_plane(const std::string& value)
{
    return value == "projected_surface" ||
           value == "tank_bottom_outer_surface" ||
           value == "tank_bottom_inner_surface" ||
           value == "estimated_fish_plane" ||
           value == "dish_top_rim" ||
           value == "unknown";
}

bool is_allowed_coordinate_space(const std::string& value)
{
    return value == "camera_native_pixels" || value == "camera_view_px";
}

nlohmann::json image_shape_to_json(const CalibrationImageSetShape& shape)
{
    return {{"height", shape.height}, {"width", shape.width}};
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return set_error(error_out, "failed to open image-set JSON for writing: " + path.string());
    }
    out << value.dump(2) << '\n';
    return true;
}

bool validate_request(const CalibrationImageSetRequest& request, std::string* error_out)
{
    if (request.artifact_id.empty()) {
        return set_error(error_out, "image_set artifact_id is empty");
    }
    if (request.created_utc.empty()) {
        return set_error(error_out, "image_set created_utc is empty");
    }
    if (!is_allowed_purpose(request.purpose)) {
        return set_error(error_out, "image_set purpose is not supported: " + request.purpose);
    }
    if (!is_allowed_target_plane(request.target_plane)) {
        return set_error(error_out, "image_set target_plane is not supported: " + request.target_plane);
    }
    if (!is_allowed_coordinate_space(request.coordinate_space)) {
        return set_error(
            error_out,
            "image_set coordinate_space must be camera_native_pixels or camera_view_px");
    }
    if (request.camera.serial.empty()) {
        return set_error(error_out, "image_set camera.serial is empty");
    }
    if (request.camera.image_shape.height <= 0 || request.camera.image_shape.width <= 0) {
        return set_error(error_out, "image_set camera.image_shape must be positive");
    }
    if (request.capture.timestamp_utc.empty()) {
        return set_error(error_out, "image_set capture.timestamp_utc is empty");
    }
    if (request.images.empty()) {
        return set_error(error_out, "image_set images must contain at least one image");
    }
    for (size_t idx = 0; idx < request.images.size(); ++idx) {
        const CalibrationImageSetImageRef& image = request.images[idx];
        const std::string prefix = "image_set images[" + std::to_string(idx) + "]";
        if (image.role.empty()) {
            return set_error(error_out, prefix + ".role is empty");
        }
        if (image.path.empty()) {
            return set_error(error_out, prefix + ".path is empty");
        }
        if (image.checksum_algorithm.empty()) {
            return set_error(error_out, prefix + ".checksum_algorithm is empty");
        }
        if (image.checksum.empty()) {
            return set_error(error_out, prefix + ".checksum is empty");
        }
        if (!is_allowed_coordinate_space(image.coordinate_space)) {
            return set_error(error_out, prefix + ".coordinate_space is not supported");
        }
        if (image.image_shape.height <= 0 || image.image_shape.width <= 0) {
            return set_error(error_out, prefix + ".image_shape must be positive");
        }
    }
    for (size_t idx = 0; idx < request.derived_artifacts.size(); ++idx) {
        const CalibrationImageSetArtifactRef& ref = request.derived_artifacts[idx];
        const std::string prefix = "image_set derived_artifacts[" + std::to_string(idx) + "]";
        if (ref.artifact_id.empty()) {
            return set_error(error_out, prefix + ".artifact_id is empty");
        }
        if (ref.artifact_schema_id.empty()) {
            return set_error(error_out, prefix + ".artifact_schema_id is empty");
        }
        if (ref.artifact_schema_version <= 0) {
            return set_error(error_out, prefix + ".artifact_schema_version must be positive");
        }
    }
    return true;
}

nlohmann::json camera_to_json(const CalibrationImageSetCameraInfo& camera)
{
    nlohmann::json out = {
        {"serial", camera.serial},
        {"image_shape", image_shape_to_json(camera.image_shape)}
    };
    if (!camera.name.empty()) {
        out["name"] = camera.name;
    }
    if (!camera.pixel_format.empty()) {
        out["pixel_format"] = camera.pixel_format;
    }
    if (camera.configured_width > 0) {
        out["configured_width"] = camera.configured_width;
    }
    if (camera.configured_height > 0) {
        out["configured_height"] = camera.configured_height;
    }
    if (camera.has_gpu_direct) {
        out["gpu_direct"] = camera.gpu_direct;
    }
    if (!camera.camera_config_ref.empty()) {
        out["camera_config_ref"] = camera.camera_config_ref;
    }
    return out;
}

nlohmann::json capture_to_json(const CalibrationImageSetCaptureContext& capture)
{
    nlohmann::json out = {{"timestamp_utc", capture.timestamp_utc}};
    if (!capture.operation_id.empty()) {
        out["operation_id"] = capture.operation_id;
    }
    if (capture.has_frame_id) {
        out["frame_id"] = capture.frame_id;
    }
    if (capture.has_recording_frame_id) {
        out["recording_frame_id"] = capture.recording_frame_id;
    }
    if (!capture.capture_mode.empty()) {
        out["capture_mode"] = capture.capture_mode;
    }
    if (capture.has_exposure_us) {
        out["exposure_us"] = capture.exposure_us;
    }
    if (capture.has_frame_rate_hz) {
        out["frame_rate_hz"] = capture.frame_rate_hz;
    }
    if (capture.has_gain) {
        out["gain"] = capture.gain;
    }
    if (!capture.filter_state.empty()) {
        out["filter_state"] = capture.filter_state;
    }
    if (!capture.runtime_filter_state.empty()) {
        out["runtime_filter_state"] = capture.runtime_filter_state;
    }
    if (!capture.light_state.empty()) {
        out["light_state"] = capture.light_state;
    }
    if (!capture.projector_state.empty()) {
        out["projector_state"] = capture.projector_state;
    }
    if (capture.has_projector_visible_to_camera) {
        out["projector_visible_to_camera"] = capture.projector_visible_to_camera;
    }
    if (capture.has_requires_camera_mount_unchanged) {
        out["requires_camera_mount_unchanged"] = capture.requires_camera_mount_unchanged;
    }
    if (capture.has_requires_filter_reinstalled_repeatably) {
        out["requires_filter_reinstalled_repeatably"] =
            capture.requires_filter_reinstalled_repeatably;
    }
    return out;
}

nlohmann::json image_ref_to_json(const CalibrationImageSetImageRef& image)
{
    nlohmann::json out = {
        {"role", image.role},
        {"path", image.path},
        {"checksum_algorithm", image.checksum_algorithm},
        {"checksum", image.checksum},
        {"coordinate_space", image.coordinate_space},
        {"image_shape", image_shape_to_json(image.image_shape)}
    };
    if (!image.description.empty()) {
        out["description"] = image.description;
    }
    return out;
}

nlohmann::json artifact_ref_to_json(const CalibrationImageSetArtifactRef& ref)
{
    nlohmann::json out = {
        {"artifact_id", ref.artifact_id},
        {"artifact_schema_id", ref.artifact_schema_id},
        {"artifact_schema_version", ref.artifact_schema_version}
    };
    if (!ref.fingerprint.empty()) {
        out["fingerprint"] = ref.fingerprint;
    }
    return out;
}

} // namespace

nlohmann::json calibration_image_set_to_json(const CalibrationImageSetRequest& request)
{
    nlohmann::json out = {
        {"schema_id", kCalibrationImageSetSchemaId},
        {"schema_version", kCalibrationImageSetSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"created_utc", request.created_utc},
        {"purpose", request.purpose},
        {"target_plane", request.target_plane},
        {"coordinate_space", request.coordinate_space},
        {"camera", camera_to_json(request.camera)},
        {"capture", capture_to_json(request.capture)},
        {"images", nlohmann::json::array()}
    };

    for (const CalibrationImageSetImageRef& image : request.images) {
        out["images"].push_back(image_ref_to_json(image));
    }
    if (!request.rig_context.empty()) {
        out["rig_context"] = request.rig_context;
    }
    if (!request.derived_artifacts.empty()) {
        out["derived_artifacts"] = nlohmann::json::array();
        for (const CalibrationImageSetArtifactRef& ref : request.derived_artifacts) {
            out["derived_artifacts"].push_back(artifact_ref_to_json(ref));
        }
    }
    if (!request.projected_pattern.empty()) {
        out["projected_pattern"] = request.projected_pattern;
    }
    if (!request.scale_target.empty()) {
        out["scale_target"] = request.scale_target;
    }
    if (!request.observations.empty()) {
        out["observations"] = request.observations;
    }
    if (!request.review_artifacts.empty()) {
        out["review_artifacts"] = request.review_artifacts;
    }
    if (!request.citrus_preview.empty()) {
        out["citrus_preview"] = request.citrus_preview;
    }
    if (!request.operator_notes.empty()) {
        out["operator_notes"] = request.operator_notes;
    }
    return out;
}

bool write_calibration_image_set_json_file(
    const std::string& image_set_json_path,
    const CalibrationImageSetRequest& request,
    CalibrationImageSetWriteResult* result_out,
    std::string* error_out)
{
    if (image_set_json_path.empty()) {
        return set_error(error_out, "image_set_json_path is empty");
    }
    if (!validate_request(request, error_out)) {
        return false;
    }

    const nlohmann::json image_set = calibration_image_set_to_json(request);
    if (!write_json_file(image_set_json_path, image_set, error_out)) {
        return false;
    }

    if (result_out) {
        result_out->image_set_json_path = image_set_json_path;
        result_out->image_set = image_set;
    }
    return true;
}

} // namespace orange::calibration
