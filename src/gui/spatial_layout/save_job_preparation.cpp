#include "gui/spatial_layout/save_job_preparation.h"

#include "calibration_image_set.h"
#include "dish_top_rim_observation.h"
#include "gui/spatial_layout/calibration_metadata.h"
#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;
using orange::spatial::ViewRegistration;

bool is_legacy_top_level_calibration_artifact_root(const std::string& artifact_root_dir)
{
    if (artifact_root_dir.empty()) {
        return false;
    }
    const std::filesystem::path root =
        std::filesystem::path(artifact_root_dir).lexically_normal();
    return root.filename() == "artifacts" &&
           root.parent_path().filename() == "calibrations";
}

bool reject_legacy_top_level_calibration_artifact_root(
    const std::string& artifact_root_dir,
    std::string* error_out)
{
    if (!is_legacy_top_level_calibration_artifact_root(artifact_root_dir)) {
        return false;
    }
    if (error_out) {
        *error_out =
            "Spatial Layout artifacts must be saved inside a calibration session, "
            "not the legacy top-level calibrations/artifacts folder. Use "
            "calibrations/sessions/<session_id>/artifacts/.";
    }
    return true;
}

bool runtime_geometry_to_top_rim_circle(
    const RuntimeGeometry& geometry,
    orange::calibration::DishTopRimCircle* circle_out,
    std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim circle destination is null.";
        }
        return false;
    }
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        if (error_out) {
            *error_out = "Top-rim observation requires a circular resolved experimental boundary.";
        }
        return false;
    }
    circle_out->center.x = geometry.circle.cx;
    circle_out->center.y = geometry.circle.cy;
    circle_out->radius_px = geometry.circle.r;
    return true;
}

bool captured_frame_to_gray8(
    const SpatialLayoutUiState& ui_state,
    cv::Mat* image_out,
    std::string* error_out)
{
    if (image_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim source image destination is null.";
        }
        return false;
    }
    if (!ui_state.has_capture ||
        ui_state.captured_texture_width <= 0 ||
        ui_state.captured_texture_height <= 0 ||
        ui_state.captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before saving a top-rim observation.";
        }
        return false;
    }

    const size_t expected_size =
        static_cast<size_t>(ui_state.captured_texture_width) *
        static_cast<size_t>(ui_state.captured_texture_height) *
        4u;
    if (ui_state.captured_rgba.size() < expected_size) {
        if (error_out) {
            *error_out = "Captured RGBA buffer is smaller than the recorded image dimensions.";
        }
        return false;
    }

    const cv::Mat rgba(
        ui_state.captured_texture_height,
        ui_state.captured_texture_width,
        CV_8UC4,
        const_cast<unsigned char*>(ui_state.captured_rgba.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    *image_out = gray.clone();
    return true;
}

void apply_captured_frame_provenance_to_capture(
    const SpatialLayoutUiState& ui_state,
    orange::calibration::CalibrationImageSetCaptureContext* capture)
{
    if (capture == nullptr) {
        return;
    }
    capture->source_frame_count = std::max<uint32_t>(1u, ui_state.captured_source_frame_count);
    capture->has_source_frame_count = true;
    capture->capture_group_id = ui_state.captured_capture_group_id;
    if (capture->source_frame_count > 1 ||
        ui_state.captured_capture_mode == "temporal_mean_stream_frames_v1") {
        capture->temporal_compositing_method = "temporal_mean_stream_frames_v1";
    }
    if (ui_state.captured_first_local_frame_id != 0 ||
        ui_state.captured_last_local_frame_id != 0) {
        capture->first_local_frame_id = ui_state.captured_first_local_frame_id;
        capture->last_local_frame_id = ui_state.captured_last_local_frame_id;
        capture->has_local_frame_range = true;
    }
    if (ui_state.captured_first_camera_frame_id != 0 ||
        ui_state.captured_last_camera_frame_id != 0) {
        capture->first_camera_frame_id = ui_state.captured_first_camera_frame_id;
        capture->last_camera_frame_id = ui_state.captured_last_camera_frame_id;
        capture->has_camera_frame_range = true;
    }
}

void apply_captured_frame_provenance_to_capture(
    const SpatialLayoutUiState& ui_state,
    orange::calibration::DishTopRimCaptureContext* capture)
{
    if (capture == nullptr) {
        return;
    }
    capture->source_frame_count = std::max<uint32_t>(1u, ui_state.captured_source_frame_count);
    capture->has_source_frame_count = true;
    if (capture->source_frame_count > 1 ||
        ui_state.captured_capture_mode == "temporal_mean_stream_frames_v1") {
        capture->temporal_compositing_method = "temporal_mean_stream_frames_v1";
    }
    if (ui_state.captured_first_local_frame_id != 0 ||
        ui_state.captured_last_local_frame_id != 0) {
        capture->first_local_frame_id = ui_state.captured_first_local_frame_id;
        capture->last_local_frame_id = ui_state.captured_last_local_frame_id;
        capture->has_local_frame_range = true;
    }
    if (ui_state.captured_first_camera_frame_id != 0 ||
        ui_state.captured_last_camera_frame_id != 0) {
        capture->first_camera_frame_id = ui_state.captured_first_camera_frame_id;
        capture->last_camera_frame_id = ui_state.captured_last_camera_frame_id;
        capture->has_camera_frame_range = true;
    }
}

orange::calibration::DishTopRimHoughParams make_top_rim_hough_params(
    const SpatialLayoutUiState& ui_state,
    const orange::calibration::DishTopRimCircle& accepted_circle,
    int width,
    int height)
{
    orange::calibration::DishTopRimHoughParams params;
    const double min_dim = static_cast<double>(std::max(1, std::min(width, height)));
    const double max_dim = static_cast<double>(std::max(width, height));
    const double radius = std::max(1.0, accepted_circle.radius_px);
    params.dp = std::clamp(ui_state.hough_dp, 1.0, 3.0);
    params.min_dist_px =
        std::max(1.0, min_dim * std::clamp(ui_state.hough_min_dist_fraction, 0.01, 2.0));
    params.param1 = std::clamp(ui_state.hough_param1, 1.0, 500.0);
    params.param2 = std::clamp(ui_state.hough_param2, 1.0, 500.0);
    params.min_radius_px =
        std::max(
            4,
            static_cast<int>(
                std::floor(min_dim * std::clamp(ui_state.hough_min_radius_fraction, 0.001, 1.0))));
    params.max_radius_px =
        std::max(params.min_radius_px + 1,
                 static_cast<int>(
                     std::ceil(
                         std::min(
                             max_dim,
                             min_dim *
                                 std::clamp(ui_state.hough_max_radius_fraction, 0.001, 1.5)))));
    if (params.min_radius_px > static_cast<int>(std::floor(radius * 1.10)) ||
        params.max_radius_px < static_cast<int>(std::ceil(radius * 0.90))) {
        params.min_radius_px = std::max(4, static_cast<int>(std::floor(radius * 0.75)));
        params.max_radius_px =
            std::max(
                params.min_radius_px + 1,
                static_cast<int>(std::ceil(std::min(max_dim, radius * 1.25))));
    }
    params.max_detection_dimension_px =
        std::clamp(ui_state.hough_max_detection_dimension_px, 256, 8192);
    params.detection_scale =
        max_dim > static_cast<double>(params.max_detection_dimension_px)
            ? static_cast<double>(params.max_detection_dimension_px) / max_dim
            : 1.0;
    params.radius_adjustment_px = ui_state.hough_radius_adjustment_px;
    return params;
}

struct SpatialLayoutCaptureStateBackup {
    bool has_capture = false;
    int captured_texture_width = 0;
    int captured_texture_height = 0;
    std::vector<unsigned char> captured_rgba;
    std::string captured_camera_serial;
    std::string captured_source_array_role;
    std::string captured_capture_mode;
    std::string captured_capture_group_id;
    uint32_t captured_source_frame_count = 1;
    uint64_t captured_first_local_frame_id = 0;
    uint64_t captured_last_local_frame_id = 0;
    uint64_t captured_first_camera_frame_id = 0;
    uint64_t captured_last_camera_frame_id = 0;
    CitrusSpatialTemplateState citrus_template;
    SpatialLayoutCalibrationImageSetMetadata metadata;
    nlohmann::json pending_full_res_snapshot_pre_capture = nlohmann::json::object();
};

SpatialLayoutCaptureStateBackup backup_spatial_layout_capture_state(
    const SpatialLayoutUiState& ui_state)
{
    SpatialLayoutCaptureStateBackup backup;
    backup.has_capture = ui_state.has_capture;
    backup.captured_texture_width = ui_state.captured_texture_width;
    backup.captured_texture_height = ui_state.captured_texture_height;
    backup.captured_rgba = ui_state.captured_rgba;
    backup.captured_camera_serial = ui_state.captured_camera_serial;
    backup.captured_source_array_role = ui_state.captured_source_array_role;
    backup.captured_capture_mode = ui_state.captured_capture_mode;
    backup.captured_capture_group_id = ui_state.captured_capture_group_id;
    backup.captured_source_frame_count = ui_state.captured_source_frame_count;
    backup.captured_first_local_frame_id = ui_state.captured_first_local_frame_id;
    backup.captured_last_local_frame_id = ui_state.captured_last_local_frame_id;
    backup.captured_first_camera_frame_id = ui_state.captured_first_camera_frame_id;
    backup.captured_last_camera_frame_id = ui_state.captured_last_camera_frame_id;
    backup.citrus_template = ui_state.citrus_template;
    backup.metadata = make_calibration_image_set_metadata_from_ui(ui_state);
    backup.pending_full_res_snapshot_pre_capture =
        ui_state.pending_full_res_snapshot_pre_capture;
    return backup;
}

void restore_spatial_layout_capture_state(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutCaptureStateBackup backup)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->has_capture = backup.has_capture;
    ui_state->captured_texture_width = backup.captured_texture_width;
    ui_state->captured_texture_height = backup.captured_texture_height;
    ui_state->captured_rgba = std::move(backup.captured_rgba);
    ui_state->captured_camera_serial = std::move(backup.captured_camera_serial);
    ui_state->captured_source_array_role = std::move(backup.captured_source_array_role);
    ui_state->captured_capture_mode = std::move(backup.captured_capture_mode);
    ui_state->captured_capture_group_id = std::move(backup.captured_capture_group_id);
    ui_state->captured_source_frame_count = backup.captured_source_frame_count;
    ui_state->captured_first_local_frame_id = backup.captured_first_local_frame_id;
    ui_state->captured_last_local_frame_id = backup.captured_last_local_frame_id;
    ui_state->captured_first_camera_frame_id = backup.captured_first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = backup.captured_last_camera_frame_id;
    ui_state->citrus_template = std::move(backup.citrus_template);
    apply_calibration_image_set_metadata_to_ui(ui_state, backup.metadata);
    ui_state->pending_full_res_snapshot_pre_capture =
        std::move(backup.pending_full_res_snapshot_pre_capture);
}

bool prepare_generic_calibration_image_set_save_job_from_group_capture(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    const CameraParams& camera_params,
    const std::string& artifact_root_dir,
    GenericCalibrationImageSetSaveJob* job_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (!capture.valid || capture.width <= 0 || capture.height <= 0 || capture.rgba.empty()) {
        if (error_out) {
            *error_out = "Grouped capture for " + capture.camera_serial + " is empty.";
        }
        return false;
    }
    if (capture.source_array_role != "images_full") {
        if (error_out) {
            *error_out = "Grouped capture for " + capture.camera_serial +
                         " is not in full-resolution camera coordinates.";
        }
        return false;
    }

    SpatialLayoutCaptureStateBackup backup =
        backup_spatial_layout_capture_state(*ui_state);

    ui_state->has_capture = true;
    ui_state->captured_texture_width = capture.width;
    ui_state->captured_texture_height = capture.height;
    ui_state->captured_rgba = capture.rgba;
    ui_state->captured_camera_serial = capture.camera_serial;
    ui_state->captured_source_array_role = capture.source_array_role;
    ui_state->captured_capture_mode = capture.capture_mode;
    ui_state->captured_capture_group_id = capture.capture_group_id;
    ui_state->captured_source_frame_count = std::max<uint32_t>(1u, capture.source_frame_count);
    ui_state->captured_first_local_frame_id = capture.first_local_frame_id;
    ui_state->captured_last_local_frame_id = capture.last_local_frame_id;
    ui_state->captured_first_camera_frame_id = capture.first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = capture.last_camera_frame_id;
    apply_calibration_image_set_metadata_to_ui(ui_state, capture.metadata);

    bool template_ok = true;
    if (!ui_state->citrus_canvas_templates.empty()) {
        const int citrus_index =
            find_citrus_template_index_for_camera(*ui_state, capture.camera_serial);
        if (citrus_index < 0) {
            template_ok = false;
            if (error_out) {
                *error_out = "No loaded Citrus canvas template matches camera " +
                             capture.camera_serial + ".";
            }
        } else {
            ui_state->citrus_template =
                ui_state->citrus_canvas_templates[static_cast<size_t>(citrus_index)];
        }
    } else if (ui_state->citrus_template.available &&
               !ui_state->citrus_template.source_camera_id.empty() &&
               ui_state->citrus_template.source_camera_id != capture.camera_serial) {
        ui_state->citrus_template = CitrusSpatialTemplateState{};
    }

    bool ok = false;
    if (template_ok) {
        ok = prepare_generic_calibration_image_set_save_job_from_spatial_layout(
            ui_state,
            camera_params,
            artifact_root_dir,
            job_out,
            error_out);
        if (ok && job_out != nullptr) {
            if (capture.camera_configured_width > 0) {
                job_out->request.camera.configured_width = capture.camera_configured_width;
            }
            if (capture.camera_configured_height > 0) {
                job_out->request.camera.configured_height = capture.camera_configured_height;
            }
            if (!capture.camera_pixel_format.empty()) {
                job_out->request.camera.pixel_format = capture.camera_pixel_format;
            }
            if (capture.has_camera_exposure_us) {
                job_out->request.capture.exposure_us = capture.camera_exposure_us;
                job_out->request.capture.has_exposure_us = true;
            }
            if (capture.has_camera_frame_rate_hz) {
                job_out->request.capture.frame_rate_hz = capture.camera_frame_rate_hz;
                job_out->request.capture.has_frame_rate_hz = true;
            }
            if (capture.has_camera_gain) {
                job_out->request.capture.gain = capture.camera_gain;
                job_out->request.capture.has_gain = true;
            }
            attach_calibration_domain_observation(
                &job_out->request,
                capture.metadata);
        }
    }
    restore_spatial_layout_capture_state(
        ui_state,
        std::move(backup));
    return ok;
}

}  // namespace

bool prepare_dish_top_rim_observation_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    TopRimObservationSaveJob* job_out,
    std::string* error_out)
{
    if (job_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim save job destination is null.";
        }
        return false;
    }
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root directory is empty.";
        }
        return false;
    }
    if (reject_legacy_top_level_calibration_artifact_root(
            artifact_root_dir,
            error_out)) {
        return false;
    }
    if (!ui_state->dish_mask_runtime.has_geometry) {
        if (error_out) {
            *error_out = "Resolved dish-mask geometry is not available yet.";
        }
        return false;
    }
    const std::string source_array_role =
        ui_state->captured_source_array_role.empty()
            ? "images_full"
            : ui_state->captured_source_array_role;
    if (source_array_role != "images_full") {
        if (error_out) {
            *error_out =
                "Top-rim observations must be saved in full-resolution camera coordinates. "
                "The current capture is a downsampled live preview; recapture with stream downsample=1 "
                "or use a full-resolution capture path.";
        }
        return false;
    }

    orange::calibration::DishTopRimCircle accepted_circle;
    if (!runtime_geometry_to_top_rim_circle(
            ui_state->dish_mask_runtime.geometry.outer_geometry,
            &accepted_circle,
            error_out)) {
        return false;
    }
    if (!ui_state->has_detected_experimental_area_circle ||
        ui_state->detected_experimental_area_geometry.type != RuntimeGeometryType::kCircle) {
        if (error_out) {
            *error_out =
                "Run Hough circle detection before saving a top-rim observation. "
                "The save path persists the displayed full-resolution detection proposal "
                "instead of recomputing Hough on save.";
        }
        return false;
    }
    orange::calibration::DishTopRimCircle detected_circle;
    if (!runtime_geometry_to_top_rim_circle(
            ui_state->detected_experimental_area_geometry,
            &detected_circle,
            error_out)) {
        return false;
    }

    cv::Mat source_gray;
    if (!captured_frame_to_gray8(*ui_state, &source_gray, error_out)) {
        return false;
    }

    TopRimObservationSaveJob job;
    job.artifact_root_dir = artifact_root_dir;
    job.accepted_circle = accepted_circle;
    job.source_gray = std::move(source_gray);

    const std::string timestamp = get_current_utc_timestamp();
    auto& request = job.request;
    request.artifact_id =
        orange::calibration::build_dish_top_rim_observation_artifact_id(
            selected_camera.camera_serial,
            timestamp);
    const std::string associated_image_set_artifact_id =
        build_camera_arena_calibration_image_set_artifact_id(ui_state, selected_camera);
    request.storage_relative_artifact_dir =
        (std::filesystem::path(associated_image_set_artifact_id) /
         "top_rim_observations" /
         request.artifact_id).generic_string();
    request.created_utc = timestamp;
    request.camera.serial = selected_camera.camera_serial;
    request.camera.name = selected_camera.camera_name;
    request.camera.width = ui_state->captured_texture_width;
    request.camera.height = ui_state->captured_texture_height;
    request.camera.pixel_format = selected_camera.pixel_format.empty()
                                      ? "captured_rgba_converted_to_gray8"
                                      : selected_camera.pixel_format;
    request.capture.operation_id = "spatial_layout_top_rim_" + request.artifact_id;
    request.capture.capture_mode = ui_state->captured_capture_mode.empty()
                                       ? "session_local_operator_still"
                                       : ui_state->captured_capture_mode;
    apply_captured_frame_provenance_to_capture(*ui_state, &request.capture);
    const auto metadata_or_unknown = [](const std::string& value) {
        return value.empty() ? std::string("unknown") : value;
    };
    request.capture.filter_state = metadata_or_unknown(ui_state->calibration_filter_state);
    request.capture.runtime_filter_state =
        metadata_or_unknown(ui_state->calibration_runtime_filter_state);
    request.capture.light_handling = metadata_or_unknown(ui_state->calibration_light_handling);
    request.capture.light_state = metadata_or_unknown(ui_state->calibration_light_state);
    request.capture.illumination_spectrum =
        metadata_or_unknown(ui_state->calibration_illumination_spectrum);
    request.capture.illumination_source =
        metadata_or_unknown(ui_state->calibration_illumination_source);
    request.capture.illumination_center_wavelength_nm =
        ui_state->calibration_illumination_center_wavelength_nm;
    request.capture.has_illumination_center_wavelength_nm =
        ui_state->calibration_has_illumination_center_wavelength_nm;
    request.capture.illumination_min_wavelength_nm =
        ui_state->calibration_illumination_min_wavelength_nm;
    request.capture.has_illumination_min_wavelength_nm =
        ui_state->calibration_has_illumination_min_wavelength_nm;
    request.capture.illumination_max_wavelength_nm =
        ui_state->calibration_illumination_max_wavelength_nm;
    request.capture.has_illumination_max_wavelength_nm =
        ui_state->calibration_has_illumination_max_wavelength_nm;
    request.capture.illumination_bandwidth_fwhm_nm =
        ui_state->calibration_illumination_bandwidth_fwhm_nm;
    request.capture.has_illumination_bandwidth_fwhm_nm =
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm;
    request.capture.illumination_wavelength_confidence =
        metadata_or_unknown(ui_state->calibration_illumination_wavelength_confidence);
    request.capture.projector_state = metadata_or_unknown(ui_state->calibration_projector_state);
    request.capture.projector_visible_to_camera =
        ui_state->calibration_projector_visible_to_camera;
    request.capture.dish_fill_state =
        metadata_or_unknown(ui_state->calibration_dish_fill_state);
    request.capture.exposure_us = static_cast<double>(selected_camera.exposure);
    request.capture.frame_rate_hz = static_cast<double>(selected_camera.frame_rate);
    request.capture.requires_filter_reinstalled_repeatably =
        ui_state->calibration_requires_filter_reinstalled_repeatably;
    request.source_array_role = source_array_role;
    request.source_frame_index = 0;
    request.has_detected_circle = true;
    request.detected_circle = detected_circle;
    request.detected_circle_source =
        "orange_spatial_layout_ui_cached_hough_scaled_to_full_resolution";
    request.valid_region_erosion_px = std::max(0.0, ui_state->edge_margin_px);
    request.operator_boundary_target =
        metadata_or_unknown(ui_state->calibration_operator_boundary_target);
    request.boundary_inclusion_policy =
        metadata_or_unknown(ui_state->calibration_boundary_inclusion_policy);
    request.operator_confirmed = true;
    request.operator_status = "orange_spatial_layout_ui_confirmed";
    request.operator_notes = ui_state->calibration_operator_notes;
    request.runtime_verification.status = "unknown";
    request.runtime_verification.reason = "runtime_850nm_rim_not_verified";
    request.write_palette_export = true;
    request.arena_context = {
        {"camera_serial", selected_camera.camera_serial},
        {"associated_image_set_artifact_id", associated_image_set_artifact_id}
    };
    ViewRegistration registration_snapshot = ui_state->registration;
    registration_snapshot.layout_coordinate_space =
        ui_state->layout_artifact.layout.coordinate_space;
    registration_snapshot.layout_to_camera_matrix =
        build_layout_to_camera_matrix(*ui_state);
    registration_snapshot.has_camera_to_layout_matrix =
        invert_affine_3x3(
            registration_snapshot.layout_to_camera_matrix,
            &registration_snapshot.camera_to_layout_matrix);
    request.arena_context["spatial_layout_registration"] = {
        {"schema_id", "orange.spatial_layout.registration_snapshot"},
        {"schema_version", 1},
        {"authority", "orange_review_context_only"},
        {"semantics", "view_registration_used_to_render_accepted_dish_fit_at_save_time"},
        {"view_registration", orange::spatial::view_registration_to_json(registration_snapshot)},
        {"editor_parameters", {
            {"translate_x_px", ui_state->registration_tx_px},
            {"translate_y_px", ui_state->registration_ty_px},
            {"scale_px_per_layout_unit", ui_state->registration_scale},
            {"rotation_deg_clockwise", ui_state->registration_rotation_deg_clockwise},
            {"edge_margin_px", ui_state->edge_margin_px}
        }},
        {"accepted_circle_source", "orange_spatial_layout_runtime.outer_geometry"},
        {"hough_circle_source", request.detected_circle_source}
    };
    if (ui_state->citrus_template.available) {
        nlohmann::json rig_context = nlohmann::json::object();
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            rig_context["rig_id"] = ui_state->citrus_template.source_rig_name;
            request.arena_context["rig_id"] =
                ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            rig_context["canvas_id"] = ui_state->citrus_template.source_canvas_name;
            request.arena_context["canvas_id"] =
                ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            rig_context["arena_id"] = ui_state->citrus_template.source_arena_name;
            request.arena_context["arena_id"] =
                ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            rig_context["camera_id"] = ui_state->citrus_template.source_camera_id;
            request.arena_context["citrus_camera_id"] =
                ui_state->citrus_template.source_camera_id;
        }
        if (!ui_state->citrus_template.source_config_path.empty() ||
            !ui_state->citrus_template.source_config_name.empty()) {
            rig_context["citrus_config_ref"] = {
                {"source", "spatial_layout_import"},
                {"path", ui_state->citrus_template.source_config_path},
                {"config_name", ui_state->citrus_template.source_config_name}
            };
            request.arena_context["citrus_config_ref"] =
                rig_context["citrus_config_ref"];
        }
        if (ui_state->citrus_template.has_camera_to_canvas_homography) {
            rig_context["citrus_homography_ref"] = {
                {"available", true},
                {"source", "citrus_homography_sidecar"},
                {"direction", "camera_view_px_to_final_display_canvas_px"}
            };
            request.arena_context["citrus_homography_ref"] =
                rig_context["citrus_homography_ref"];
        }
        rig_context["associated_image_set_artifact_id"] =
            request.arena_context["associated_image_set_artifact_id"];
        request.image_set_rig_context = rig_context;
    }

    job.hough_params = make_top_rim_hough_params(
        *ui_state,
        accepted_circle,
        ui_state->captured_texture_width,
        ui_state->captured_texture_height);

    *job_out = std::move(job);
    return true;
}

bool prepare_generic_calibration_image_set_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    GenericCalibrationImageSetSaveJob* job_out,
    std::string* error_out)
{
    if (job_out == nullptr) {
        if (error_out) {
            *error_out = "Calibration image-set save job destination is null.";
        }
        return false;
    }
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root directory is empty.";
        }
        return false;
    }
    if (reject_legacy_top_level_calibration_artifact_root(
            artifact_root_dir,
            error_out)) {
        return false;
    }
    const std::string source_array_role =
        ui_state->captured_source_array_role.empty()
            ? "images_full"
            : ui_state->captured_source_array_role;
    if (source_array_role != "images_full") {
        if (error_out) {
            *error_out =
                "Calibration image sets must be saved in full-resolution camera coordinates. "
                "Use Capture Full-Resolution Stream Snapshot before saving this artifact.";
        }
        return false;
    }

    cv::Mat source_gray;
    if (!captured_frame_to_gray8(*ui_state, &source_gray, error_out)) {
        return false;
    }

    const auto metadata_or_unknown = [](const std::string& value) {
        return value.empty() ? std::string("unknown") : value;
    };

    const std::string timestamp = get_current_utc_timestamp();
    GenericCalibrationImageSetSaveJob job;
    job.artifact_root_dir = artifact_root_dir;
    job.image_role =
        ui_state->calibration_image_set_image_role.empty()
            ? "source"
            : ui_state->calibration_image_set_image_role;
    job.image_description =
        "full-resolution source frame for " +
        metadata_or_unknown(ui_state->calibration_image_set_purpose);
    job.capture_filename = build_calibration_capture_filename(
        ui_state->calibration_image_set_purpose,
        timestamp);
    job.source_gray = std::move(source_gray);

    auto& request = job.request;
    request.artifact_id =
        build_camera_arena_calibration_image_set_artifact_id(ui_state, selected_camera);
    request.created_utc = timestamp;
    request.purpose = metadata_or_unknown(ui_state->calibration_image_set_purpose);
    request.target_plane = metadata_or_unknown(ui_state->calibration_image_set_target_plane);
    request.coordinate_space = "camera_native_pixels";
    request.camera.serial = selected_camera.camera_serial;
    request.camera.name = selected_camera.camera_name;
    request.camera.image_shape.height = ui_state->captured_texture_height;
    request.camera.image_shape.width = ui_state->captured_texture_width;
    request.camera.pixel_format = selected_camera.pixel_format.empty()
                                      ? "captured_rgba_converted_to_gray8"
                                      : selected_camera.pixel_format;
    request.camera.configured_height = selected_camera.height;
    request.camera.configured_width = selected_camera.width;

    request.capture.operation_id =
        "spatial_layout_image_set_" + request.artifact_id +
        "_" + sanitize_artifact_component(request.purpose) +
        "_" + sanitize_artifact_component(timestamp);
    request.capture.timestamp_utc = timestamp;
    request.capture.capture_mode = ui_state->captured_capture_mode.empty()
                                       ? "session_local_operator_still"
                                       : ui_state->captured_capture_mode;
    apply_captured_frame_provenance_to_capture(*ui_state, &request.capture);
    request.capture.exposure_us = static_cast<double>(selected_camera.exposure);
    request.capture.has_exposure_us = true;
    request.capture.frame_rate_hz = static_cast<double>(selected_camera.frame_rate);
    request.capture.has_frame_rate_hz = true;
    request.capture.filter_state = metadata_or_unknown(ui_state->calibration_filter_state);
    request.capture.runtime_filter_state =
        metadata_or_unknown(ui_state->calibration_runtime_filter_state);
    request.capture.light_handling = metadata_or_unknown(ui_state->calibration_light_handling);
    request.capture.light_state = metadata_or_unknown(ui_state->calibration_light_state);
    request.capture.illumination_spectrum =
        metadata_or_unknown(ui_state->calibration_illumination_spectrum);
    request.capture.illumination_source =
        metadata_or_unknown(ui_state->calibration_illumination_source);
    request.capture.illumination_center_wavelength_nm =
        ui_state->calibration_illumination_center_wavelength_nm;
    request.capture.has_illumination_center_wavelength_nm =
        ui_state->calibration_has_illumination_center_wavelength_nm;
    request.capture.illumination_min_wavelength_nm =
        ui_state->calibration_illumination_min_wavelength_nm;
    request.capture.has_illumination_min_wavelength_nm =
        ui_state->calibration_has_illumination_min_wavelength_nm;
    request.capture.illumination_max_wavelength_nm =
        ui_state->calibration_illumination_max_wavelength_nm;
    request.capture.has_illumination_max_wavelength_nm =
        ui_state->calibration_has_illumination_max_wavelength_nm;
    request.capture.illumination_bandwidth_fwhm_nm =
        ui_state->calibration_illumination_bandwidth_fwhm_nm;
    request.capture.has_illumination_bandwidth_fwhm_nm =
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm;
    request.capture.illumination_wavelength_confidence =
        metadata_or_unknown(ui_state->calibration_illumination_wavelength_confidence);
    request.capture.projector_state = metadata_or_unknown(ui_state->calibration_projector_state);
    request.capture.projector_visible_to_camera =
        ui_state->calibration_projector_visible_to_camera;
    request.capture.has_projector_visible_to_camera = true;
    request.capture.requires_camera_mount_unchanged = true;
    request.capture.has_requires_camera_mount_unchanged = true;
    request.capture.requires_filter_reinstalled_repeatably =
        ui_state->calibration_requires_filter_reinstalled_repeatably;
    request.capture.has_requires_filter_reinstalled_repeatably = true;

    if (ui_state->citrus_template.available) {
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            request.rig_context["rig_id"] = ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            request.rig_context["canvas_id"] = ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            request.rig_context["arena_id"] = ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            request.rig_context["camera_id"] = ui_state->citrus_template.source_camera_id;
        }
        if (!ui_state->citrus_template.source_config_path.empty() ||
            !ui_state->citrus_template.source_config_name.empty()) {
            request.rig_context["citrus_config_ref"] = {
                {"source", "spatial_layout_import"},
                {"path", ui_state->citrus_template.source_config_path},
                {"config_name", ui_state->citrus_template.source_config_name}
            };
        }
        if (ui_state->citrus_template.has_camera_to_canvas_homography) {
            request.rig_context["citrus_homography_ref"] = {
                {"available", true},
                {"source", "citrus_homography_sidecar"},
                {"direction", "camera_view_px_to_final_display_canvas_px"}
            };
        }
    }

    if (request.purpose == "homography_grid" ||
        request.purpose == "crosshair_alignment" ||
        request.purpose == "verification_dots" ||
        request.purpose == "validation_pattern") {
        request.projected_pattern = {
            {"pattern_id", metadata_or_unknown(ui_state->calibration_image_set_projected_pattern_id)},
            {"type", metadata_or_unknown(ui_state->calibration_image_set_projected_pattern_type)},
            {"source", "operator_entered"},
            {"target_plane", request.target_plane},
            {"pattern_type", metadata_or_unknown(ui_state->calibration_pattern_type)},
            {"pattern_domain", metadata_or_unknown(ui_state->calibration_pattern_domain)}
        };
    }
    if (request.purpose == "scale_image") {
        request.scale_target = {
            {"target_type", metadata_or_unknown(ui_state->calibration_image_set_scale_target_type)},
            {"source", "operator_entered"},
            {"target_plane", request.target_plane}
        };
    }
    const SpatialLayoutCalibrationImageSetMetadata capture_metadata =
        make_calibration_image_set_metadata_from_ui(*ui_state);
    apply_capture_stage_metadata_to_request(&request, capture_metadata);
    request.citrus_projection_snapshot_pre_capture =
        capture_metadata.citrus_projection_snapshot_pre_capture;
    request.citrus_projection_snapshot_post_capture =
        capture_metadata.citrus_projection_snapshot_post_capture;
    request.citrus_projection_epoch_consistency =
        capture_metadata.citrus_projection_epoch_consistency.is_object() &&
                !capture_metadata.citrus_projection_epoch_consistency.empty()
            ? capture_metadata.citrus_projection_epoch_consistency
            : make_citrus_projection_epoch_consistency(request);
    attach_runtime_role_metadata(&request);
    attach_projection_surface_authored_domain_hint(&request);
    attach_calibration_domain_observation(&request, capture_metadata);
    request.citrus_preview = {
        {"available", false},
        {"diagnostic_only", true},
        {"authority", "citrus_recomputes_before_acceptance"}
    };
    request.operator_notes =
        ui_state->calibration_image_set_notes.empty()
            ? ui_state->calibration_operator_notes
            : ui_state->calibration_image_set_notes;

    *job_out = std::move(job);
    return true;
}

bool queue_group_calibration_image_set_save_jobs(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras_params == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Grouped save requires open cameras.";
        }
        return false;
    }
    if (ui_state->group_captures.empty()) {
        if (error_out) {
            *error_out = "Capture grouped full-resolution snapshots before saving grouped image sets.";
        }
        return false;
    }
    if (pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "Grouped capture is still pending.";
        }
        return false;
    }

    std::string session_artifact_root;
    if (!ensure_spatial_calibration_session(
            ui_state,
            selected_camera,
            artifact_root_dir,
            &session_artifact_root,
            error_out)) {
        return false;
    }

    std::deque<GenericCalibrationImageSetSaveJob> jobs;
    for (const SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
        const int camera_index =
            capture.camera_index >= 0
                ? capture.camera_index
                : find_camera_index_by_serial(cameras_params, num_cameras, capture.camera_serial);
        if (camera_index < 0 || camera_index >= num_cameras) {
            if (error_out) {
                *error_out = "Grouped capture camera is no longer open: " +
                             capture.camera_serial;
            }
            return false;
        }
        GenericCalibrationImageSetSaveJob job;
        if (!prepare_generic_calibration_image_set_save_job_from_group_capture(
                ui_state,
                capture,
                cameras_params[camera_index],
                session_artifact_root,
                &job,
                error_out)) {
            return false;
        }
        job.session_dir = ui_state->calibration_session_dir;
        jobs.push_back(std::move(job));
    }

    for (GenericCalibrationImageSetSaveJob& job : jobs) {
        std::string submit_error;
        if (!submit_generic_calibration_image_set_save_job(std::move(job), &submit_error)) {
            if (error_out) {
                *error_out = submit_error.empty()
                                 ? "Failed to queue grouped calibration image-set save job."
                                 : submit_error;
            }
            return false;
        }
    }

    if (status_out) {
        *status_out =
            "Queued " + std::to_string(ui_state->group_captures.size()) +
            " grouped calibration image-set save job(s) in session " +
            ui_state->calibration_session_id + ".";
    }
    return true;
}

}  // namespace orange::gui::spatial_layout
