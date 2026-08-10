#include "gui/spatial_layout/group_capture_controller.h"

#include "camera_preview_utils.h"
#include "gui/spatial_layout/calibration_metadata.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/preview_capture.h"
#include "gui/spatial_layout/projector_intensity_authority.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/session_review.h"
#include "project.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

const CitrusSpatialTemplateState* template_for_camera(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial)
{
    for (const CitrusSpatialTemplateState& candidate :
         ui_state.citrus_canvas_templates) {
        if (candidate.available &&
            candidate.source_camera_id == camera_serial) {
            return &candidate;
        }
    }
    if (ui_state.citrus_template.available &&
        ui_state.citrus_template.source_camera_id == camera_serial) {
        return &ui_state.citrus_template;
    }
    return nullptr;
}

bool apply_holder_projector_intensity_authority(
    SpatialLayoutUiState* ui_state,
    const std::vector<std::string>& camera_serials,
    std::string* error_out)
{
    if (ui_state == nullptr ||
        ui_state->calibration_capture_stage !=
            "projected_surface_holder_installed") {
        return true;
    }
    const std::string recipe = resolve_group_capture_scene_recipe(*ui_state);
    if (recipe != "homography_grid" && recipe != "homography_rings") {
        return true;
    }

    std::vector<ProjectorIntensityCameraAuthorityRef> refs;
    refs.reserve(camera_serials.size());
    for (const std::string& camera_serial : camera_serials) {
        const CitrusSpatialTemplateState* template_state =
            template_for_camera(*ui_state, camera_serial);
        if (template_state == nullptr) {
            if (error_out != nullptr) {
                *error_out = "No imported Citrus arena template is available for "
                    "selected camera " + camera_serial + ".";
            }
            return false;
        }
        refs.push_back({
            camera_serial,
            template_state->source_config_name,
            template_state->source_rig_name,
            template_state->source_canvas_name,
            template_state->source_config_path,
            template_state->homography_projector_intensity_report_path,
            template_state->homography_projector_intensity_report_sha256,
        });
    }

    const ProjectorIntensityAuthorityResult authority =
        resolve_projector_intensity_authority(refs);
    if (!authority.ok) {
        if (error_out != nullptr) {
            *error_out = "Holder homography capture requires a valid commissioned "
                "projector intensity: " + authority.error;
        }
        return false;
    }
    if (!ui_state->group_capture_scene_options.is_object()) {
        ui_state->group_capture_scene_options = nlohmann::json::object();
    }
    ui_state->group_capture_scene_options["foreground_gray_u8"] =
        authority.foreground_gray_u8;
    ui_state->group_capture_scene_options[
        "projector_intensity_commissioning"] = authority.provenance;
    return true;
}

SpatialLayoutGroupCaptureFrame make_group_capture_from_snapshot(
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& capture_group_id,
    const std::string& capture_mode,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    SpatialLayoutGroupCaptureFrame capture;
    capture.valid = result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty();
    capture.capture_group_id = capture_group_id;
    capture.metadata = metadata;
    capture.camera_serial = result.camera_serial;
    capture.camera_index =
        find_camera_index_by_serial(cameras_params, num_cameras, result.camera_serial);
    if (capture.camera_index >= 0) {
        const CameraParams& camera_params = cameras_params[capture.camera_index];
        capture.camera_name = camera_params.camera_name;
        capture.camera_configured_width = camera_params.width;
        capture.camera_configured_height = camera_params.height;
        capture.camera_pixel_format = camera_params.pixel_format;
        capture.camera_exposure_us = static_cast<double>(camera_params.exposure);
        capture.has_camera_exposure_us = true;
        capture.camera_frame_rate_hz = static_cast<double>(camera_params.frame_rate);
        capture.has_camera_frame_rate_hz = true;
        capture.camera_gain = static_cast<double>(camera_params.gain);
        capture.has_camera_gain = true;
    }
    capture.width = result.width;
    capture.height = result.height;
    capture.rgba = result.rgba;
    capture.source_array_role =
        result.source_array_role.empty() ? "images_full" : result.source_array_role;
    capture.capture_mode = capture_mode.empty() ? "operator_group_next_frame" : capture_mode;
    capture.source_frame_count = std::max<uint32_t>(1u, result.completed_frame_count);
    capture.first_local_frame_id = result.first_local_frame_id;
    capture.last_local_frame_id = result.last_local_frame_id;
    capture.first_camera_frame_id = result.first_camera_frame_id;
    capture.last_camera_frame_id = result.last_camera_frame_id;
    capture.camera_timestamp_ns = result.camera_timestamp_ns;
    capture.timestamp_sys_ns = result.timestamp_sys_ns;
    return capture;
}

void upsert_group_capture(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutGroupCaptureFrame capture)
{
    if (ui_state == nullptr || capture.camera_serial.empty()) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& existing : ui_state->group_captures) {
        if (existing.camera_serial == capture.camera_serial) {
            orange::preview::clear_texture(
                &existing.texture,
                &existing.texture_width,
                &existing.texture_height);
            existing = std::move(capture);
            return;
        }
    }
    ui_state->group_captures.push_back(std::move(capture));
}

std::string build_group_capture_id(
    const SpatialLayoutUiState& ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    const std::string& timestamp)
{
    static std::atomic<uint64_t> group_sequence{1};
    std::ostringstream oss;
    oss << "calgrp_" << sanitize_artifact_component(timestamp);
    if (ui_state.citrus_template.available &&
        !ui_state.citrus_template.source_canvas_name.empty()) {
        oss << "_" << sanitize_artifact_component(ui_state.citrus_template.source_canvas_name);
    }
    if (!metadata.image_set_purpose.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_purpose);
    }
    if (!metadata.image_set_target_plane.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_target_plane);
    }
    oss << "_g" << group_sequence.fetch_add(1, std::memory_order_relaxed);
    return oss.str();
}

bool camera_is_group_capture_eligible(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int camera_index)
{
    return cameras_select != nullptr &&
           spatial_snapshot_workers != nullptr &&
           camera_index >= 0 &&
           cameras_select[camera_index].stream_on &&
           spatial_snapshot_workers[camera_index] != nullptr;
}

double monotonic_seconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

double calibration_scene_timeout_seconds()
{
    const char* value = std::getenv("ORANGE_CITRUS_CALIBRATION_SCENE_TIMEOUT_SECONDS");
    if (value == nullptr || value[0] == '\0') {
        return 10.0;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || parsed <= 0.0) {
        return 10.0;
    }
    return std::clamp(parsed, 1.0, 120.0);
}

int post_presentation_settle_milliseconds()
{
    const char* value = std::getenv(
        "ORANGE_GUI_GROUP_CAPTURE_POST_PRESENTATION_SETTLE_MS");
    if (value == nullptr || *value == '\0') return 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return 0;
    return static_cast<int>(std::clamp(parsed, 0L, 30000L));
}

bool string_list_contains(
    const std::vector<std::string>& values,
    const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> normalized_expected_camera_serials(
    const SpatialLayoutUiState& ui_state,
    const CameraParams* cameras_params,
    int num_cameras)
{
    std::vector<std::string> out;
    if (cameras_params == nullptr) {
        return out;
    }
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        const std::string& serial = cameras_params[camera_index].camera_serial;
        if (!serial.empty() &&
            string_list_contains(ui_state.group_capture_selected_camera_serials, serial)) {
            out.push_back(serial);
        }
    }
    return out;
}

bool resolve_group_capture_arena_ids(
    const SpatialLayoutUiState& ui_state,
    const std::vector<std::string>& camera_serials,
    std::vector<std::string>* arena_ids_out,
    std::string* error_out)
{
    if (arena_ids_out == nullptr) {
        if (error_out) {
            *error_out = "Grouped Citrus scene arena destination is null.";
        }
        return false;
    }
    arena_ids_out->clear();
    for (const std::string& camera_serial : camera_serials) {
        std::string arena_id;
        for (const CitrusSpatialTemplateState& candidate : ui_state.citrus_canvas_templates) {
            if (!candidate.available ||
                candidate.source_camera_id != camera_serial ||
                candidate.source_arena_name.empty()) {
                continue;
            }
            arena_id = candidate.source_arena_name;
            break;
        }
        if (arena_id.empty() &&
            ui_state.citrus_template.available &&
            ui_state.citrus_template.source_camera_id == camera_serial) {
            arena_id = ui_state.citrus_template.source_arena_name;
        }
        if (arena_id.empty()) {
            if (error_out) {
                *error_out =
                    "No loaded Citrus arena mapping matches expected camera " +
                    camera_serial + ". Load the Citrus canvas before guided capture.";
            }
            return false;
        }
        if (!string_list_contains(*arena_ids_out, arena_id)) {
            arena_ids_out->push_back(arena_id);
        }
    }
    if (arena_ids_out->empty()) {
        if (error_out) {
            *error_out = "Guided grouped capture requires at least one Citrus arena target.";
        }
        return false;
    }
    return true;
}

bool scene_targets_match_group(
    const nlohmann::json& scene,
    const SpatialLayoutUiState& ui_state,
    std::string* error_out)
{
    if (!scene.contains("resolved_targets") ||
        !scene["resolved_targets"].is_array()) {
        if (error_out) {
            *error_out = "Citrus scene status lacks resolved_targets.";
        }
        return false;
    }
    std::set<std::string> resolved_arenas;
    std::set<std::string> resolved_cameras;
    for (const nlohmann::json& target : scene["resolved_targets"]) {
        if (!target.is_object()) {
            continue;
        }
        const std::string arena_id = target.value("arena_id", std::string());
        if (!arena_id.empty()) {
            resolved_arenas.insert(arena_id);
        }
        if (target.contains("associated_camera_ids") &&
            target["associated_camera_ids"].is_array()) {
            for (const nlohmann::json& camera_id : target["associated_camera_ids"]) {
                if (camera_id.is_string()) {
                    resolved_cameras.insert(camera_id.get<std::string>());
                }
            }
        }
    }
    for (const std::string& arena_id : ui_state.group_capture_arena_ids) {
        if (resolved_arenas.count(arena_id) == 0) {
            if (error_out) {
                *error_out = "Citrus scene did not resolve expected arena " + arena_id + ".";
            }
            return false;
        }
    }
    for (const std::string& camera_serial : ui_state.group_capture_expected_camera_serials) {
        if (resolved_cameras.count(camera_serial) == 0) {
            if (error_out) {
                *error_out =
                    "Citrus scene did not associate expected camera " + camera_serial + ".";
            }
            return false;
        }
    }
    return true;
}

bool scene_presented_for_group_command(
    const nlohmann::json& scene,
    const SpatialLayoutUiState& ui_state,
    std::string* error_out)
{
    if (!scene.is_object() || scene.empty()) {
        if (error_out) {
            *error_out = "Citrus calibration scene status is empty.";
        }
        return false;
    }
    if (scene.value("transaction_id", std::string()) !=
            ui_state.group_capture_transaction_id ||
        scene.value("recipe_id", std::string()) !=
            ui_state.group_capture_resolved_scene_recipe ||
        scene.value("operation_id", std::string()) !=
            ui_state.group_capture_scene_operation_id ||
        scene.value("request_id", std::string()) !=
            ui_state.group_capture_scene_request_id) {
        if (error_out) {
            *error_out = "Citrus scene status does not identify this grouped capture command.";
        }
        return false;
    }
    const nlohmann::json last_command =
        scene.value("last_command", nlohmann::json::object());
    if (last_command.value("status", std::string()) != "applied" ||
        last_command.value("operation_id", std::string()) !=
            ui_state.group_capture_scene_operation_id ||
        last_command.value("request_id", std::string()) !=
            ui_state.group_capture_scene_request_id) {
        if (error_out) {
            *error_out = "Citrus has not applied this grouped capture scene command.";
        }
        return false;
    }
    if (scene.value("state", std::string()) != "presented" ||
        !scene.value("presented", false) ||
        !scene.value("active", false) ||
        scene.value("scene_revision", uint64_t{0}) == 0 ||
        scene.value("content_fingerprint", std::string()).empty()) {
        if (error_out) {
            *error_out = "Citrus scene has not crossed its presentation fence.";
        }
        return false;
    }
    return scene_targets_match_group(scene, ui_state, error_out);
}

bool arena_centering_presented_for_group_command(
    const nlohmann::json& centering,
    const SpatialLayoutUiState& ui_state,
    nlohmann::json* scene_out,
    std::string* error_out)
{
    if (!centering.is_object() || centering.empty()) {
        if (error_out) {
            *error_out = "Citrus arena-centering status is empty.";
        }
        return false;
    }
    if (centering.value("transaction_id", std::string()) !=
            ui_state.group_capture_transaction_id ||
        centering.value("stage_id", std::string()) !=
            ui_state.group_capture_expected_stage_id ||
        centering.value("operation_id", std::string()) !=
            ui_state.group_capture_scene_operation_id ||
        centering.value("state", std::string()) != "presented" ||
        !centering.value("presented", false) ||
        !centering.value("active", false)) {
        if (error_out) {
            *error_out =
                "Citrus has not presented the expected arena-centering stage.";
        }
        return false;
    }
    const nlohmann::json last_command =
        centering.value("last_command", nlohmann::json::object());
    if (last_command.value("status", std::string()) != "applied" ||
        last_command.value("operation_id", std::string()) !=
            ui_state.group_capture_scene_operation_id) {
        if (error_out) {
            *error_out = "Citrus arena-centering command is not applied.";
        }
        return false;
    }
    const nlohmann::json scene = centering.value(
        "calibration_scene", nlohmann::json::object());
    if (!scene.is_object() ||
        scene.value("transaction_id", std::string()) !=
            ui_state.group_capture_transaction_id ||
        scene.value("operation_id", std::string()) !=
            ui_state.group_capture_scene_operation_id ||
        scene.value("state", std::string()) != "presented" ||
        !scene.value("presented", false) ||
        !scene.value("active", false) ||
        scene.value("recipe_id", std::string()) != "arena_outline") {
        if (error_out) {
            *error_out =
                "Embedded Citrus arena-outline scene has not crossed its presentation fence.";
        }
        return false;
    }
    if (!scene_targets_match_group(scene, ui_state, error_out)) {
        return false;
    }
    if (scene_out != nullptr) {
        *scene_out = scene;
    }
    return true;
}

nlohmann::json make_arena_centering_consistency(
    const SpatialLayoutUiState& ui_state)
{
    const nlohmann::json& pre =
        ui_state.group_capture_metadata.citrus_arena_centering_pre_capture;
    const nlohmann::json& post =
        ui_state.group_capture_metadata.citrus_arena_centering_post_capture;
    nlohmann::json result = {
        {"schema_id", "orange.citrus_arena_centering_capture_consistency"},
        {"schema_version", 1},
        {"status", "unavailable"},
        {"transaction_id", ui_state.group_capture_transaction_id},
        {"stage_id", ui_state.group_capture_expected_stage_id},
        {"operation_id", ui_state.group_capture_scene_operation_id},
    };
    if (!pre.is_object() || pre.empty() || !post.is_object() || post.empty()) {
        result["reason"] = "pre_or_post_arena_centering_status_missing";
        return result;
    }
    const nlohmann::json pre_scene = pre.value(
        "calibration_scene", nlohmann::json::object());
    const nlohmann::json post_scene = post.value(
        "calibration_scene", nlohmann::json::object());
    const bool same =
        pre.value("transaction_id", std::string()) ==
            ui_state.group_capture_transaction_id &&
        post.value("transaction_id", std::string()) ==
            ui_state.group_capture_transaction_id &&
        pre.value("stage_id", std::string()) ==
            ui_state.group_capture_expected_stage_id &&
        post.value("stage_id", std::string()) ==
            ui_state.group_capture_expected_stage_id &&
        pre.value("operation_id", std::string()) ==
            ui_state.group_capture_scene_operation_id &&
        post.value("operation_id", std::string()) ==
            ui_state.group_capture_scene_operation_id &&
        pre_scene.value("scene_revision", uint64_t{0}) > 0 &&
        pre_scene.value("scene_revision", uint64_t{0}) ==
            post_scene.value("scene_revision", uint64_t{0}) &&
        !pre_scene.value("content_fingerprint", std::string()).empty() &&
        pre_scene.value("content_fingerprint", std::string()) ==
            post_scene.value("content_fingerprint", std::string()) &&
        pre.value("presented", false) && post.value("presented", false);
    result["status"] = same ? "same_stage" : "changed_stage";
    result["reason"] = same ? "" :
        "arena_centering_stage_or_embedded_scene_changed_across_capture";
    result["scene_revision"] =
        pre_scene.value("scene_revision", uint64_t{0});
    result["content_fingerprint"] =
        pre_scene.value("content_fingerprint", std::string());
    return result;
}

bool daily_registration_preview_presented_for_group_capture(
    const nlohmann::json& daily,
    const SpatialLayoutUiState& ui_state,
    std::string* error_out)
{
    const bool ready =
        daily.is_object() &&
        daily.value("active", false) &&
        daily.value("preview_active", false) &&
        daily.value("visible", false) &&
        daily.value("transaction_id", std::string()) ==
            ui_state.group_capture_transaction_id &&
        daily.value("candidate_sha256", std::string()) ==
            ui_state.group_capture_expected_stage_id &&
        daily.value("operation_id", std::string()) ==
            ui_state.group_capture_scene_operation_id &&
        daily.value("transition", std::string()) == "candidate_previewed" &&
        !daily.value("content_fingerprint", std::string()).empty();
    if (!ready && error_out != nullptr) {
        *error_out =
            "Citrus has not presented the expected daily-registration candidate preview.";
    }
    return ready;
}

nlohmann::json make_daily_registration_consistency(
    const SpatialLayoutUiState& ui_state)
{
    const nlohmann::json& pre =
        ui_state.group_capture_metadata.citrus_daily_registration_pre_capture;
    const nlohmann::json& post =
        ui_state.group_capture_metadata.citrus_daily_registration_post_capture;
    nlohmann::json result = {
        {"schema_id", "orange.citrus_daily_registration_capture_consistency"},
        {"schema_version", 1},
        {"status", "unavailable"},
        {"transaction_id", ui_state.group_capture_transaction_id},
        {"candidate_sha256", ui_state.group_capture_expected_stage_id},
        {"preview_operation_id", ui_state.group_capture_scene_operation_id},
    };
    if (!pre.is_object() || pre.empty() || !post.is_object() || post.empty()) {
        result["reason"] = "pre_or_post_daily_registration_status_missing";
        return result;
    }
    const bool same =
        pre.value("transaction_id", std::string()) ==
            ui_state.group_capture_transaction_id &&
        post.value("transaction_id", std::string()) ==
            ui_state.group_capture_transaction_id &&
        pre.value("candidate_sha256", std::string()) ==
            ui_state.group_capture_expected_stage_id &&
        post.value("candidate_sha256", std::string()) ==
            ui_state.group_capture_expected_stage_id &&
        pre.value("operation_id", std::string()) ==
            ui_state.group_capture_scene_operation_id &&
        post.value("operation_id", std::string()) ==
            ui_state.group_capture_scene_operation_id &&
        !pre.value("content_fingerprint", std::string()).empty() &&
        pre.value("content_fingerprint", std::string()) ==
            post.value("content_fingerprint", std::string()) &&
        pre.value("preview_active", false) &&
        post.value("preview_active", false) &&
        pre.value("visible", false) && post.value("visible", false);
    result["status"] = same ? "same_candidate_preview" : "changed_candidate_preview";
    result["reason"] = same ? "" :
        "daily_registration_candidate_or_preview_changed_across_capture";
    result["content_fingerprint"] =
        pre.value("content_fingerprint", std::string());
    return result;
}

bool restore_presented_for_group_command(
    const nlohmann::json& scene,
    const SpatialLayoutUiState& ui_state)
{
    if (!scene.is_object() ||
        scene.value("transaction_id", std::string()) !=
            ui_state.group_capture_transaction_id ||
        scene.value("operation_id", std::string()) !=
            ui_state.group_capture_restore_operation_id ||
        scene.value("request_id", std::string()) !=
            ui_state.group_capture_restore_request_id ||
        scene.value("state", std::string()) != "restored" ||
        !scene.value("presented", false) ||
        scene.value("active", true)) {
        return false;
    }
    const nlohmann::json last_command =
        scene.value("last_command", nlohmann::json::object());
    return last_command.value("status", std::string()) == "applied" &&
           last_command.value("operation_id", std::string()) ==
               ui_state.group_capture_restore_operation_id &&
           last_command.value("request_id", std::string()) ==
               ui_state.group_capture_restore_request_id;
}

bool citrus_stimulus_display_matches_contract(
    const nlohmann::json& response,
    std::string* error_out)
{
    const char* configured =
        std::getenv("ORANGE_CITRUS_EXPECTED_STIMULUS_MONITOR");
    if (configured == nullptr || *configured == '\0') {
        return true;
    }
    const nlohmann::json status = response.value(
        "status", nlohmann::json::object());
    const nlohmann::json display = status.value(
        "display", nlohmann::json::object());
    const std::string selected = display.value(
        "stimulus_monitor_name", std::string());
    const int width = display.value("stimulus_width_px", 0);
    const int height = display.value("stimulus_height_px", 0);
    const int framebuffer_width = display.value("framebuffer_width_px", 0);
    const int framebuffer_height = display.value("framebuffer_height_px", 0);
    const int observed_width = display.value(
        "observed_framebuffer_width_px", 0);
    const int observed_height = display.value(
        "observed_framebuffer_height_px", 0);
    const int viewport_x = display.value("viewport_x_px", -1);
    const int viewport_y = display.value("viewport_y_px", -1);
    const int viewport_width = display.value("viewport_width_px", 0);
    const int viewport_height = display.value("viewport_height_px", 0);
    const bool valid =
        display.value("stimulus_window_active", false) &&
        display.value("stimulus_render_loop_active", false) &&
        display.value("stimulus_rendered_frames", std::uint64_t{0}) > 0 &&
        !display.value("framebuffer_invariant_violated", true) &&
        display.value("viewport_matches_framebuffer", false) &&
        selected == configured && width > 0 && height > 0 &&
        framebuffer_width == width && framebuffer_height == height &&
        observed_width == width && observed_height == height &&
        viewport_x == 0 && viewport_y == 0 &&
        viewport_width == width && viewport_height == height;
    if (!valid && error_out != nullptr) {
        std::ostringstream error;
        error << "Citrus stimulus display ownership check failed"
              << " (expected_monitor=" << configured
              << ", selected_monitor="
              << (selected.empty() ? "missing" : selected)
              << ", window_active="
              << display.value("stimulus_window_active", false)
              << ", render_loop_active="
              << display.value("stimulus_render_loop_active", false)
              << ", configured_size=" << width << "x" << height
              << ", framebuffer=" << framebuffer_width << "x"
              << framebuffer_height << ", observed=" << observed_width
              << "x" << observed_height << ", viewport=" << viewport_x
              << "," << viewport_y << " " << viewport_width << "x"
              << viewport_height << ", viewport_matches_framebuffer="
              << display.value("viewport_matches_framebuffer", false)
              << ")";
        *error_out = error.str();
    }
    return valid;
}

nlohmann::json make_group_scene_consistency(
    const SpatialLayoutUiState& ui_state)
{
    nlohmann::json out = {
        {"status", "unavailable"},
        {"policy", "required_same_presented_scene_v1"},
        {"transaction_id", ui_state.group_capture_transaction_id},
        {"recipe_id", ui_state.group_capture_resolved_scene_recipe},
        {"blocking_or_warning_reason", "Citrus scene status is unavailable before or after capture."}
    };
    const nlohmann::json& pre = ui_state.group_capture_scene_pre_capture;
    const nlohmann::json& post = ui_state.group_capture_scene_post_capture;
    if (!pre.is_object() || pre.empty() || !post.is_object() || post.empty()) {
        return out;
    }
    const uint64_t pre_revision = pre.value("scene_revision", uint64_t{0});
    const uint64_t post_revision = post.value("scene_revision", uint64_t{0});
    const std::string pre_fingerprint =
        pre.value("content_fingerprint", std::string());
    const std::string post_fingerprint =
        post.value("content_fingerprint", std::string());
    out["pre_scene_revision"] = pre_revision;
    out["post_scene_revision"] = post_revision;
    out["pre_content_fingerprint"] = pre_fingerprint;
    out["post_content_fingerprint"] = post_fingerprint;
    if (pre_revision == 0 || pre_fingerprint.empty() ||
        pre_revision != post_revision || pre_fingerprint != post_fingerprint ||
        post.value("transaction_id", std::string()) !=
            ui_state.group_capture_transaction_id ||
        post.value("recipe_id", std::string()) !=
            ui_state.group_capture_resolved_scene_recipe ||
        post.value("state", std::string()) != "presented" ||
        !post.value("presented", false)) {
        out["status"] = "changed_scene";
        out["blocking_or_warning_reason"] =
            "Citrus scene identity changed or was no longer presented across grouped capture.";
        return out;
    }
    out["status"] = "same_scene";
    out["scene_revision"] = pre_revision;
    out["content_fingerprint"] = pre_fingerprint;
    out["blocking_or_warning_reason"] = "";
    return out;
}

nlohmann::json make_group_membership(const SpatialLayoutUiState& ui_state)
{
    nlohmann::json completed = nlohmann::json::array();
    for (const SpatialLayoutGroupCaptureFrame& capture : ui_state.group_captures) {
        completed.push_back(capture.camera_serial);
    }
    nlohmann::json failed = nlohmann::json::array();
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (request.failed) {
            failed.push_back({
                {"camera_serial", request.camera_serial},
                {"error", request.error}
            });
        }
    }
    const size_t expected_count = ui_state.group_capture_expected_camera_serials.size();
    const bool complete = completed.size() == expected_count && failed.empty();
    const std::string scene_status =
        ui_state.group_capture_scene_authority == "daily_registration"
            ? (ui_state.group_capture_metadata.citrus_daily_registration_consistency.value(
                   "status", std::string("unavailable")) ==
                       "same_candidate_preview"
                   ? "same_scene"
                   : "unavailable")
            : ui_state.group_capture_metadata.citrus_calibration_scene_consistency.value(
                  "status", std::string("unavailable"));
    const std::string centering_status =
        ui_state.group_capture_scene_authority == "arena_centering"
            ? ui_state.group_capture_metadata.citrus_arena_centering_consistency.value(
                  "status", std::string("unavailable"))
            : "not_applicable";
    const std::string daily_status =
        ui_state.group_capture_scene_authority == "daily_registration"
            ? ui_state.group_capture_metadata.citrus_daily_registration_consistency.value(
                  "status", std::string("unavailable"))
            : "not_applicable";
    std::string status = complete ? "complete" : (completed.empty() ? "failed" : "partial");
    if (scene_status != "same_scene" ||
        (ui_state.group_capture_scene_authority == "arena_centering" &&
         centering_status != "same_stage") ||
        (ui_state.group_capture_scene_authority == "daily_registration" &&
         daily_status != "same_candidate_preview")) {
        status = completed.empty() ? "failed" : "invalid_scene";
    }
    nlohmann::json membership = {
        {"schema_id", "orange.calibration.capture_group_membership"},
        {"schema_version", 1},
        {"capture_group_id", ui_state.group_capture_id},
        {"expected_camera_serials", ui_state.group_capture_expected_camera_serials},
        {"completed_camera_serials", completed},
        {"failed_cameras", failed},
        {"expected_count", expected_count},
        {"completed_count", completed.size()},
        {"failed_count", failed.size()},
        {"status", status},
        {"intentional_camera_scope", true},
        {"scene_consistency_status", scene_status},
        {"scene_authority", ui_state.group_capture_scene_authority},
        {"arena_centering_consistency_status", centering_status},
        {"daily_registration_consistency_status", daily_status}
    };
    membership["scene_options"] = ui_state.group_capture_scene_options;
    if (ui_state.group_capture_scene_options.is_object() &&
        ui_state.group_capture_scene_options.contains(
            "projector_intensity_commissioning")) {
        membership["projector_intensity_commissioning"] =
            ui_state.group_capture_scene_options.at(
                "projector_intensity_commissioning");
    }
    return membership;
}

void append_group_capture_error(SpatialLayoutUiState* ui_state, const std::string& error)
{
    if (ui_state == nullptr || error.empty()) {
        return;
    }
    if (!ui_state->group_capture_error.empty()) {
        ui_state->group_capture_error += " ";
    }
    ui_state->group_capture_error += error;
}

bool ensure_group_capture_transaction(
    SpatialLayoutUiState* ui_state,
    const std::string& owner_id,
    const std::vector<std::string>& camera_serials,
    const std::string& parent_transaction_owner_kind,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial calibration transaction state is unavailable.";
        }
        return false;
    }
    if (ui_state->calibration_transaction_lease) {
        ui_state->group_capture_owns_calibration_transaction = false;
        if (parent_transaction_owner_kind.empty() ||
            !spatial_calibration_transaction_owned_by(
                *ui_state, parent_transaction_owner_kind)) {
            if (error_out) {
                *error_out = parent_transaction_owner_kind.empty()
                    ? "A parent calibration transaction is active; a manual grouped capture cannot borrow it."
                    : "The grouped capture does not match its declared parent calibration transaction.";
            }
            return false;
        }
        return require_spatial_calibration_transaction(
            *ui_state,
            camera_serials,
            orange::calibration::Mutation::kCitrusScene,
            error_out);
    }

    const bool acquired = acquire_spatial_calibration_transaction(
        ui_state,
        kManualGroupTransactionOwner,
        owner_id,
        orange::calibration::WorkflowKind::kSpatialGroupedCapture,
        camera_serials,
        orange::calibration::mutation_set(
            orange::calibration::Mutation::kCitrusScene),
        "Present, capture, verify, and restore one grouped calibration scene.",
        error_out);
    ui_state->group_capture_owns_calibration_transaction = acquired;
    return acquired;
}

void release_group_capture_transaction_if_owned(
    SpatialLayoutUiState* ui_state,
    const std::string& terminal_status,
    const std::string& terminal_reason)
{
    if (ui_state == nullptr ||
        !ui_state->group_capture_owns_calibration_transaction ||
        !spatial_calibration_transaction_owned_by(
            *ui_state, kManualGroupTransactionOwner)) {
        return;
    }
    release_spatial_calibration_transaction(
        ui_state, terminal_status, terminal_reason);
}

bool begin_group_scene_restore(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->group_capture_restore_required) {
        return false;
    }
    ui_state->group_capture_restore_operation_id =
        ui_state->group_capture_id + "_restore_scene";
    const CitrusCalibrationSceneControlResult restore =
        restore_citrus_calibration_scene(
            ui_state->group_capture_transaction_id,
            ui_state->group_capture_restore_operation_id);
    if (!restore.ok) {
        ui_state->group_capture_workflow_state = "failed";
        append_group_capture_error(
            ui_state,
            "Citrus scene restore request failed: " + restore.reason +
                " Manual Citrus recovery may be required before starting an experiment.");
        return false;
    }
    ui_state->group_capture_restore_request_id =
        restore.response.value("request_id", std::string());
    ui_state->group_capture_workflow_state = "waiting_restore";
    ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
    ui_state->group_capture_scene_deadline_at_seconds =
        monotonic_seconds() + calibration_scene_timeout_seconds();
    ui_state->group_capture_status =
        "Grouped capture finished acquiring; waiting for Citrus to restore the prior scene.";
    return true;
}

void fail_group_workflow_and_restore(
    SpatialLayoutUiState* ui_state,
    const std::string& error)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->group_capture_terminal_outcome = "failed";
    append_group_capture_error(ui_state, error);
    if (!ui_state->group_capture_restore_required ||
        !begin_group_scene_restore(ui_state)) {
        if (!ui_state->group_capture_restore_required) {
            ui_state->group_capture_workflow_state = "failed";
            release_group_capture_transaction_if_owned(
                ui_state,
                "failed",
                ui_state->group_capture_error);
        }
    }
}

}  // namespace

int pending_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (!request.completed && !request.failed) {
            ++count;
        }
    }
    return count;
}

int failed_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (request.failed) {
            ++count;
        }
    }
    return count;
}

bool group_capture_workflow_active(const SpatialLayoutUiState& ui_state)
{
    return ui_state.group_capture_restore_required ||
           ui_state.group_capture_workflow_state == "waiting_scene" ||
           ui_state.group_capture_workflow_state == "capturing" ||
           ui_state.group_capture_workflow_state == "waiting_post_scene" ||
           ui_state.group_capture_workflow_state == "waiting_restore";
}

std::string resolve_group_capture_scene_recipe(const SpatialLayoutUiState& ui_state)
{
    if (!ui_state.group_capture_scene_recipe.empty() &&
        ui_state.group_capture_scene_recipe != "auto") {
        return ui_state.group_capture_scene_recipe;
    }
    const std::string& purpose = ui_state.calibration_image_set_purpose;
    if (purpose == "homography_grid") {
        if (ui_state.calibration_workflow_profile_id ==
                "unobstructed_canvas_commissioning" ||
            ui_state.calibration_capture_stage ==
                "projected_surface_dry_reference") {
            return "homography_grid";
        }
        return "homography_rings";
    }
    if (purpose == "verification_dots" || purpose == "validation_pattern") {
        return "verification_dots";
    }
    if (purpose == "crosshair_alignment") {
        return "experimental_area_center_and_outline";
    }
    if (purpose == "arena_projection") {
        return "arena_outline";
    }
    return "black_reference";
}

void initialize_group_capture_camera_scope(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras)
{
    if (ui_state == nullptr || ui_state->group_capture_camera_scope_initialized ||
        cameras_params == nullptr || num_cameras <= 0) {
        return;
    }
    std::vector<std::string> eligible;
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        if (camera_is_group_capture_eligible(
                cameras_select,
                spatial_snapshot_workers,
                camera_index) &&
            !cameras_params[camera_index].camera_serial.empty()) {
            eligible.push_back(cameras_params[camera_index].camera_serial);
        }
    }
    if (!eligible.empty()) {
        ui_state->group_capture_selected_camera_serials = std::move(eligible);
        ui_state->group_capture_camera_scope_initialized = true;
    }
}

int find_camera_index_by_serial(
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& camera_serial)
{
    if (cameras_params == nullptr || camera_serial.empty()) {
        return -1;
    }
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_params[i].camera_serial == camera_serial) {
            return i;
        }
    }
    return -1;
}

void clear_group_captures(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
        orange::preview::clear_texture(
            &capture.texture,
            &capture.texture_width,
            &capture.texture_height);
    }
    ui_state->group_captures.clear();
    ui_state->pending_group_snapshot_requests.clear();
}

bool apply_group_capture_to_active_preview(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    std::string* error_out)
{
    SpatialSnapshotResult result;
    result.ok = capture.valid;
    result.camera_serial = capture.camera_serial;
    result.capture_mode = capture.capture_mode;
    result.source_array_role = capture.source_array_role;
    result.width = capture.width;
    result.height = capture.height;
    result.completed_frame_count = std::max<uint32_t>(1u, capture.source_frame_count);
    result.first_local_frame_id = capture.first_local_frame_id;
    result.last_local_frame_id = capture.last_local_frame_id;
    result.first_camera_frame_id = capture.first_camera_frame_id;
    result.last_camera_frame_id = capture.last_camera_frame_id;
    result.local_frame_id = capture.last_local_frame_id;
    result.camera_frame_id = capture.last_camera_frame_id;
    result.camera_timestamp_ns = capture.camera_timestamp_ns;
    result.timestamp_sys_ns = capture.timestamp_sys_ns;
    result.rgba = capture.rgba;
    const bool ok = apply_full_resolution_stream_snapshot(ui_state, result, error_out);
    if (ok) {
        ui_state->captured_capture_group_id = capture.capture_group_id;
        set_captured_citrus_projection_snapshots(
            ui_state,
            capture.metadata.citrus_projection_snapshot_pre_capture,
            capture.metadata.citrus_projection_snapshot_post_capture);
        ui_state->captured_citrus_projection_epoch_consistency =
            capture.metadata.citrus_projection_epoch_consistency.is_object()
                ? capture.metadata.citrus_projection_epoch_consistency
                : nlohmann::json::object();
        ui_state->captured_citrus_calibration_scene_pre_capture =
            capture.metadata.citrus_calibration_scene_pre_capture;
        ui_state->captured_citrus_calibration_scene_post_capture =
            capture.metadata.citrus_calibration_scene_post_capture;
        ui_state->captured_citrus_calibration_scene_consistency =
            capture.metadata.citrus_calibration_scene_consistency;
        ui_state->captured_citrus_calibration_scene_restore_status =
            capture.metadata.citrus_calibration_scene_restore_status;
        ui_state->captured_citrus_arena_centering_pre_capture =
            capture.metadata.citrus_arena_centering_pre_capture;
        ui_state->captured_citrus_arena_centering_post_capture =
            capture.metadata.citrus_arena_centering_post_capture;
        ui_state->captured_citrus_arena_centering_consistency =
            capture.metadata.citrus_arena_centering_consistency;
        ui_state->captured_citrus_daily_registration_pre_capture =
            capture.metadata.citrus_daily_registration_pre_capture;
        ui_state->captured_citrus_daily_registration_post_capture =
            capture.metadata.citrus_daily_registration_post_capture;
        ui_state->captured_citrus_daily_registration_consistency =
            capture.metadata.citrus_daily_registration_consistency;
        ui_state->captured_group_membership =
            capture.metadata.capture_group_membership;
        ui_state->preview_status =
            "Showing grouped full-resolution capture from " + capture.camera_serial +
            " (" + capture.capture_group_id + ").";
    }
    return ok;
}

bool apply_session_review_image_to_active_preview(
    SpatialLayoutUiState* ui_state,
    CameraParams* cameras_params,
    int num_cameras,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->selected_session_review_image < 0 ||
        ui_state->selected_session_review_image >=
            static_cast<int>(ui_state->session_review_images.size())) {
        if (error_out) {
            *error_out = "No calibration session image is selected.";
        }
        return false;
    }
    const SpatialLayoutSessionReviewImage& entry =
        ui_state->session_review_images[
            static_cast<size_t>(ui_state->selected_session_review_image)];
    if (!entry.valid || entry.image_path.empty()) {
        if (error_out) {
            *error_out = "Selected calibration session image is not loadable.";
        }
        return false;
    }

    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    if (!load_rgba_image_from_path(entry.image_path, &rgba, &width, &height, error_out)) {
        return false;
    }

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            rgba,
            width,
            height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    const int camera_index =
        find_camera_index_by_serial(cameras_params, num_cameras, entry.camera_serial);
    if (camera_index >= 0) {
        ui_state->selected_camera = camera_index;
        ui_state->configured_camera_index = camera_index;
    }

    ui_state->captured_rgba = std::move(rgba);
    ui_state->captured_camera_serial = entry.camera_serial;
    ui_state->captured_source_array_role =
        entry.source_array_role.empty() ? "images_full" : entry.source_array_role;
    ui_state->captured_capture_mode =
        entry.capture_mode.empty() ? "loaded_calibration_session_image" : entry.capture_mode;
    ui_state->captured_capture_group_id = entry.capture_group_id;
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->has_capture = true;
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);
    apply_calibration_image_set_metadata_to_ui(ui_state, entry.metadata);

    if (entry.has_accepted_circle && entry.accepted_circle_r > 0.0) {
        ui_state->has_detected_experimental_area_circle = true;
        ui_state->detected_experimental_area_geometry =
            runtime_circle(
                entry.accepted_circle_cx,
                entry.accepted_circle_cy,
                entry.accepted_circle_r);
        ui_state->detection_status =
            "Loaded accepted top-rim circle from " + entry.artifact_id + ".";
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Loaded calibration session image " << width << "x" << height
           << " from " << entry.image_path;
    if (!entry.camera_serial.empty()) {
        status << " for Cam" << entry.camera_serial;
    }
    ui_state->preview_status = status.str();
    return true;
}

bool consume_group_snapshot_result(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    int selected_camera_index)
{
    if (ui_state == nullptr || ui_state->pending_group_snapshot_requests.empty()) {
        return false;
    }

    for (SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state->pending_group_snapshot_requests) {
        if (request.camera_serial != result.camera_serial ||
            request.request_id != result.request_id ||
            request.completed ||
            request.failed) {
            continue;
        }

        if (result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty()) {
            SpatialLayoutCalibrationImageSetMetadata capture_metadata =
                ui_state->group_capture_metadata;
            SpatialLayoutGroupCaptureFrame capture =
                make_group_capture_from_snapshot(
                    result,
                    cameras_params,
                    num_cameras,
                    ui_state->group_capture_id,
                    ui_state->group_capture_mode,
                    capture_metadata);
            std::string texture_error;
            if (!orange::preview::update_rgba_texture(
                    &capture.texture,
                    &capture.texture_width,
                    &capture.texture_height,
                    capture.rgba,
                    capture.width,
                    capture.height,
                    &texture_error)) {
                request.failed = true;
                request.completed = false;
                request.error = texture_error.empty()
                                    ? "Grouped capture texture upload failed."
                                    : texture_error;
            } else {
                request.completed = true;
                upsert_group_capture(ui_state, capture);
                if (selected_camera_index >= 0 &&
                    selected_camera_index < num_cameras &&
                    cameras_params[selected_camera_index].camera_serial == result.camera_serial) {
                    std::string preview_error;
                    if (!apply_group_capture_to_active_preview(ui_state, capture, &preview_error)) {
                        ui_state->preview_error = preview_error;
                    }
                }
            }
        } else {
            request.failed = true;
            request.error = result.error.empty()
                                ? "Grouped full-resolution snapshot failed."
                                : result.error;
        }

        const int pending = pending_group_snapshot_count(*ui_state);
        const int failed = failed_group_snapshot_count(*ui_state);
        std::ostringstream status;
        status << "Grouped capture " << ui_state->group_capture_id
               << ": completed=" << ui_state->group_captures.size()
               << " pending=" << pending
               << " failed=" << failed << ".";
        ui_state->group_capture_status = status.str();
        if (failed > 0) {
            std::ostringstream error;
            for (const SpatialLayoutPendingGroupSnapshotRequest& pending_request :
                 ui_state->pending_group_snapshot_requests) {
                if (!pending_request.failed) {
                    continue;
                }
                if (error.tellp() > 0) {
                    error << " ";
                }
                error << pending_request.camera_serial << ": "
                      << (pending_request.error.empty()
                              ? "capture failed"
                              : pending_request.error);
            }
            ui_state->group_capture_error = error.str();
        } else {
            ui_state->group_capture_error.clear();
        }
        if (pending == 0 && failed == 0) {
            ui_state->group_capture_status =
                "Grouped camera frames acquired; verifying the shared Citrus scene.";
        }
        if (pending == 0 &&
            ui_state->group_capture_workflow_state == "capturing") {
            ui_state->group_capture_workflow_state = "waiting_post_scene";
            ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
            ui_state->group_capture_scene_deadline_at_seconds =
                monotonic_seconds() + calibration_scene_timeout_seconds();
        }
        return true;
    }
    return false;
}

int eligible_group_capture_camera_count(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras)
{
    int count = 0;
    for (int i = 0; i < num_cameras; ++i) {
        if (camera_is_group_capture_eligible(cameras_select, spatial_snapshot_workers, i)) {
            ++count;
        }
    }
    return count;
}

bool request_group_full_resolution_snapshots(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    std::string* error_out,
    const std::string& transaction_id_override,
    const std::string& operation_id_override,
    const std::string& parent_transaction_owner_kind)
{
    if (ui_state == nullptr || cameras_params == nullptr || cameras_select == nullptr ||
        spatial_snapshot_workers == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Grouped capture requires open cameras and snapshot workers.";
        }
        return false;
    }
    if (group_capture_workflow_active(*ui_state) ||
        pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "A guided grouped capture is already active.";
        }
        return false;
    }

    const std::vector<std::string> expected_camera_serials =
        normalized_expected_camera_serials(*ui_state, cameras_params, num_cameras);
    if (expected_camera_serials.empty()) {
        if (error_out) {
            *error_out =
                "Select at least one expected camera for guided grouped capture.";
        }
        return false;
    }
    for (const std::string& camera_serial : expected_camera_serials) {
        const int camera_index =
            find_camera_index_by_serial(cameras_params, num_cameras, camera_serial);
        if (camera_index < 0 ||
            !camera_is_group_capture_eligible(
                cameras_select,
                spatial_snapshot_workers,
                camera_index)) {
            if (error_out) {
                *error_out =
                    "Expected camera " + camera_serial +
                    " is not streaming or lacks a spatial snapshot worker.";
            }
            return false;
        }
    }

    if (!apply_holder_projector_intensity_authority(
            ui_state, expected_camera_serials, error_out)) {
        return false;
    }

    std::vector<std::string> arena_ids;
    if (!resolve_group_capture_arena_ids(
            *ui_state,
            expected_camera_serials,
            &arena_ids,
            error_out)) {
        return false;
    }

    clear_group_captures(ui_state);
    ui_state->group_capture_error.clear();
    ui_state->group_capture_terminal_outcome.clear();
    ui_state->group_capture_scene_pre_capture = nlohmann::json::object();
    ui_state->group_capture_scene_post_capture = nlohmann::json::object();
    ui_state->group_capture_scene_restore_status = nlohmann::json::object();
    ui_state->group_capture_restore_required = false;
    ui_state->group_capture_presented_not_before_seconds = 0.0;

    const std::string timestamp = get_current_utc_timestamp();
    ui_state->group_capture_metadata =
        make_calibration_image_set_metadata_from_ui(*ui_state);
    ui_state->group_capture_id =
        build_group_capture_id(*ui_state, ui_state->group_capture_metadata, timestamp);
    ui_state->group_capture_mode =
        target_frame_count > 1 ? "operator_group_temporal_mean" : "operator_group_next_frame";
    ui_state->group_capture_target_frame_count = std::max<uint32_t>(1u, target_frame_count);
    ui_state->group_capture_expected_camera_serials = expected_camera_serials;
    ui_state->group_capture_arena_ids = std::move(arena_ids);
    ui_state->group_capture_resolved_scene_recipe =
        resolve_group_capture_scene_recipe(*ui_state);
    ui_state->group_capture_scene_authority = "calibration_scene";
    ui_state->group_capture_expected_stage_id.clear();
    ui_state->group_capture_transaction_id = transaction_id_override.empty()
        ? ui_state->group_capture_id
        : transaction_id_override;
    ui_state->group_capture_scene_operation_id = operation_id_override.empty()
        ? ui_state->group_capture_id + "_set_" +
              ui_state->group_capture_resolved_scene_recipe
        : operation_id_override;
    ui_state->group_capture_scene_request_id.clear();
    ui_state->group_capture_restore_operation_id.clear();
    ui_state->group_capture_restore_request_id.clear();
    ui_state->group_capture_metadata.citrus_projection_snapshot_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_snapshot_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_epoch_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_restore_status =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.capture_group_membership =
        nlohmann::json::object();

    std::string transaction_error;
    if (!ensure_group_capture_transaction(
            ui_state,
            ui_state->group_capture_transaction_id,
            expected_camera_serials,
            parent_transaction_owner_kind,
            &transaction_error)) {
        ui_state->group_capture_workflow_state = "failed";
        ui_state->group_capture_terminal_outcome = "failed";
        ui_state->group_capture_error = transaction_error;
        if (error_out) {
            *error_out = transaction_error;
        }
        return false;
    }

    const CitrusCalibrationSceneControlResult scene_request =
        set_citrus_calibration_scene(
            ui_state->group_capture_transaction_id,
            ui_state->group_capture_resolved_scene_recipe,
            ui_state->group_capture_arena_ids,
            ui_state->group_capture_scene_operation_id,
            ui_state->group_capture_scene_options);
    if (!scene_request.ok) {
        ui_state->group_capture_workflow_state = "failed";
        ui_state->group_capture_terminal_outcome = "failed";
        ui_state->group_capture_error =
            "Citrus calibration scene request failed: " + scene_request.reason;
        if (error_out) {
            *error_out = ui_state->group_capture_error;
        }
        release_group_capture_transaction_if_owned(
            ui_state,
            "failed",
            ui_state->group_capture_error);
        return false;
    }

    ui_state->group_capture_scene_request_id =
        scene_request.response.value("request_id", std::string());
    ui_state->group_capture_restore_required = true;
    ui_state->group_capture_workflow_state = "waiting_scene";
    ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
    ui_state->group_capture_scene_deadline_at_seconds =
        monotonic_seconds() + calibration_scene_timeout_seconds();
    std::ostringstream status;
    status << "Citrus accepted " << ui_state->group_capture_resolved_scene_recipe
           << " for " << ui_state->group_capture_arena_ids.size()
           << " arena(s); waiting for the common presentation fence before capturing "
           << ui_state->group_capture_expected_camera_serials.size()
           << " expected camera(s).";
    ui_state->group_capture_status = status.str();
    return true;
}

bool request_group_full_resolution_snapshots_for_arena_centering(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    const std::string& centering_transaction_id,
    const std::string& centering_stage_id,
    const std::string& centering_operation_id,
    std::string* error_out,
    const std::string& parent_transaction_owner_kind)
{
    if (ui_state == nullptr || cameras_params == nullptr || cameras_select == nullptr ||
        spatial_snapshot_workers == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Arena-centering grouped capture requires open cameras and snapshot workers.";
        }
        return false;
    }
    if (centering_transaction_id.empty() || centering_stage_id.empty() ||
        centering_operation_id.empty()) {
        if (error_out) {
            *error_out = "Arena-centering grouped capture requires transaction, stage, and operation identities.";
        }
        return false;
    }
    if (group_capture_workflow_active(*ui_state) ||
        pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "A guided grouped capture is already active.";
        }
        return false;
    }

    const std::vector<std::string> expected_camera_serials =
        normalized_expected_camera_serials(*ui_state, cameras_params, num_cameras);
    if (expected_camera_serials.empty()) {
        if (error_out) {
            *error_out = "Select at least one expected camera for arena-centering capture.";
        }
        return false;
    }
    for (const std::string& camera_serial : expected_camera_serials) {
        const int camera_index =
            find_camera_index_by_serial(cameras_params, num_cameras, camera_serial);
        if (camera_index < 0 ||
            !camera_is_group_capture_eligible(
                cameras_select, spatial_snapshot_workers, camera_index)) {
            if (error_out) {
                *error_out = "Expected camera " + camera_serial +
                    " is not streaming or lacks a spatial snapshot worker.";
            }
            return false;
        }
    }
    std::vector<std::string> arena_ids;
    if (!resolve_group_capture_arena_ids(
            *ui_state, expected_camera_serials, &arena_ids, error_out)) {
        return false;
    }

    std::string transaction_error;
    if (!spatial_calibration_transaction_owned_by(
            *ui_state, parent_transaction_owner_kind) ||
        !require_spatial_calibration_transaction(
            *ui_state,
            expected_camera_serials,
            orange::calibration::Mutation::kCitrusScene,
            &transaction_error)) {
        if (transaction_error.empty()) {
            transaction_error =
                "The grouped capture does not match its declared parent calibration transaction.";
        }
        if (error_out) {
            *error_out = transaction_error;
        }
        return false;
    }

    clear_group_captures(ui_state);
    ui_state->group_capture_error.clear();
    ui_state->group_capture_terminal_outcome.clear();
    ui_state->group_capture_scene_pre_capture = nlohmann::json::object();
    ui_state->group_capture_scene_post_capture = nlohmann::json::object();
    ui_state->group_capture_scene_restore_status = nlohmann::json::object();
    ui_state->group_capture_restore_required = false;
    ui_state->group_capture_presented_not_before_seconds = 0.0;
    const std::string timestamp = get_current_utc_timestamp();
    ui_state->group_capture_metadata =
        make_calibration_image_set_metadata_from_ui(*ui_state);
    ui_state->group_capture_id =
        build_group_capture_id(*ui_state, ui_state->group_capture_metadata, timestamp);
    ui_state->group_capture_mode = target_frame_count > 1
        ? "arena_centering_group_temporal_mean"
        : "arena_centering_group_next_frame";
    ui_state->group_capture_target_frame_count =
        std::max<uint32_t>(1u, target_frame_count);
    ui_state->group_capture_expected_camera_serials = expected_camera_serials;
    ui_state->group_capture_arena_ids = std::move(arena_ids);
    ui_state->group_capture_resolved_scene_recipe = "arena_outline";
    ui_state->group_capture_scene_authority = "arena_centering";
    ui_state->group_capture_expected_stage_id = centering_stage_id;
    ui_state->group_capture_transaction_id = centering_transaction_id;
    ui_state->group_capture_scene_operation_id = centering_operation_id;
    ui_state->group_capture_scene_request_id.clear();
    ui_state->group_capture_restore_operation_id.clear();
    ui_state->group_capture_restore_request_id.clear();
    ui_state->group_capture_metadata.citrus_projection_snapshot_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_snapshot_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_epoch_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_calibration_scene_restore_status =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_arena_centering_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_arena_centering_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_arena_centering_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_pre_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_daily_registration_consistency =
        nlohmann::json::object();
    ui_state->group_capture_metadata.capture_group_membership =
        nlohmann::json::object();
    ui_state->group_capture_workflow_state = "waiting_scene";
    ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
    ui_state->group_capture_scene_deadline_at_seconds =
        monotonic_seconds() + calibration_scene_timeout_seconds();
    ui_state->group_capture_status =
        "Waiting for Citrus arena-centering stage " + centering_stage_id +
        " to cross its common presentation fence before grouped PTP capture.";
    return true;
}

bool request_group_full_resolution_snapshots_for_daily_registration_preview(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    const std::string& daily_transaction_id,
    const std::string& candidate_sha256,
    const std::string& preview_operation_id,
    std::string* error_out)
{
    if (daily_transaction_id.empty() || candidate_sha256.empty() ||
        preview_operation_id.empty()) {
        if (error_out != nullptr) {
            *error_out =
                "Daily preview capture requires transaction, candidate checksum, and preview operation identities.";
        }
        return false;
    }
    // The arena-centering capture initializer already provides the exact
    // selected-camera/arena validation and a no-set/no-restore presented-scene
    // capture. Daily preview is another authority over that same capture
    // mechanism, with different readiness and consistency predicates below.
    if (!request_group_full_resolution_snapshots_for_arena_centering(
            ui_state,
            cameras_params,
            cameras_select,
            num_cameras,
            spatial_snapshot_workers,
            target_frame_count,
            daily_transaction_id,
            candidate_sha256,
            preview_operation_id,
            error_out,
            "daily_registration")) {
        return false;
    }
    ui_state->group_capture_mode = target_frame_count > 1
        ? "daily_registration_preview_group_temporal_mean"
        : "daily_registration_preview_group_next_frame";
    ui_state->group_capture_resolved_scene_recipe =
        "candidate_center_and_experimental_outline";
    ui_state->group_capture_scene_authority = "daily_registration";
    ui_state->group_capture_status =
        "Waiting for the Citrus daily-registration candidate preview to cross "
        "its common presentation fence before grouped PTP capture.";
    return true;
}

void advance_group_capture_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers)
{
    if (ui_state == nullptr || !group_capture_workflow_active(*ui_state)) {
        return;
    }
    if (ui_state->group_capture_workflow_state == "capturing") {
        if (pending_group_snapshot_count(*ui_state) == 0) {
            ui_state->group_capture_workflow_state = "waiting_post_scene";
            ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
            ui_state->group_capture_scene_deadline_at_seconds =
                monotonic_seconds() + calibration_scene_timeout_seconds();
        } else {
            return;
        }
    }

    const double now = monotonic_seconds();
    if (now < ui_state->group_capture_next_scene_poll_at_seconds) {
        return;
    }
    ui_state->group_capture_next_scene_poll_at_seconds = now + 0.05;

    if (ui_state->group_capture_workflow_state == "waiting_scene") {
        nlohmann::json scene = nlohmann::json::object();
        std::string status_reason;
        bool status_ok = false;
        if (ui_state->group_capture_scene_authority == "arena_centering") {
            const CitrusArenaCenteringControlResult status =
                query_citrus_arena_centering_status(
                    ui_state->group_capture_transaction_id,
                    "pre_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                std::string display_error;
                if (!citrus_stimulus_display_matches_contract(
                        status.response, &display_error)) {
                    fail_group_workflow_and_restore(ui_state, display_error);
                    return;
                }
                std::string readiness_error;
                if (arena_centering_presented_for_group_command(
                        status.centering,
                        *ui_state,
                        &scene,
                        &readiness_error)) {
                    status_ok = true;
                    ui_state->group_capture_metadata
                        .citrus_arena_centering_pre_capture = status.centering;
                } else {
                    status_reason = readiness_error;
                }
            }
        } else if (ui_state->group_capture_scene_authority ==
                   "daily_registration") {
            const CitrusDailyRegistrationControlResult status =
                query_citrus_daily_registration_status("pre_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                std::string display_error;
                if (!citrus_stimulus_display_matches_contract(
                        status.response, &display_error)) {
                    fail_group_workflow_and_restore(ui_state, display_error);
                    return;
                }
                std::string readiness_error;
                if (daily_registration_preview_presented_for_group_capture(
                        status.daily_registration,
                        *ui_state,
                        &readiness_error)) {
                    status_ok = true;
                    scene = status.daily_registration;
                    ui_state->group_capture_metadata
                        .citrus_daily_registration_pre_capture =
                        status.daily_registration;
                } else {
                    status_reason = readiness_error;
                }
            }
        } else {
            const CitrusCalibrationSceneControlResult status =
                query_citrus_calibration_scene_status(
                    ui_state->group_capture_transaction_id,
                    "pre_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                std::string display_error;
                if (!citrus_stimulus_display_matches_contract(
                        status.response, &display_error)) {
                    fail_group_workflow_and_restore(ui_state, display_error);
                    return;
                }
                scene = status.scene;
                const nlohmann::json last_command =
                    scene.value("last_command", nlohmann::json::object());
                if (last_command.value("operation_id", std::string()) ==
                        ui_state->group_capture_scene_operation_id &&
                    last_command.value("status", std::string()) == "rejected") {
                    fail_group_workflow_and_restore(
                        ui_state,
                        "Citrus rejected the calibration scene: " +
                            last_command.value(
                                "error", std::string("unknown error")));
                    return;
                }
                std::string readiness_error;
                if (scene_presented_for_group_command(
                        scene, *ui_state, &readiness_error)) {
                    status_ok = true;
                } else {
                    status_reason = readiness_error;
                }
            }
        }
        if (!status_ok) {
            if (now >= ui_state->group_capture_scene_deadline_at_seconds) {
                fail_group_workflow_and_restore(
                    ui_state,
                    "Timed out waiting for the Citrus scene presentation fence: " +
                        status_reason);
            } else {
                ui_state->group_capture_status =
                    "Waiting for Citrus scene status before grouped capture: " +
                    status_reason;
            }
            return;
        }

        const int settle_ms = post_presentation_settle_milliseconds();
        if (settle_ms > 0) {
            if (ui_state->group_capture_presented_not_before_seconds <= 0.0) {
                ui_state->group_capture_presented_not_before_seconds =
                    now + static_cast<double>(settle_ms) / 1000.0;
            }
            if (now < ui_state->group_capture_presented_not_before_seconds) {
                std::ostringstream waiting;
                waiting << "Citrus presentation fence is valid; waiting "
                        << settle_ms
                        << " ms before requesting fresh camera frames.";
                ui_state->group_capture_status = waiting.str();
                return;
            }
        }

        ui_state->group_capture_scene_pre_capture = scene;
        if (ui_state->group_capture_scene_authority == "calibration_scene") {
            ui_state->group_capture_metadata.citrus_calibration_scene_pre_capture =
                scene;
        }
        const CitrusProjectionSnapshotQueryResult projection_snapshot =
            query_citrus_active_projection_snapshot(
                "pre_group_capture",
                ui_state->group_capture_id);
        ui_state->group_capture_metadata.citrus_projection_snapshot_pre_capture =
            projection_snapshot.ok
                ? projection_snapshot.snapshot
                : nlohmann::json::object();

        int requested = 0;
        for (const std::string& camera_serial :
             ui_state->group_capture_expected_camera_serials) {
            const int camera_index =
                find_camera_index_by_serial(cameras_params, num_cameras, camera_serial);
            SpatialLayoutPendingGroupSnapshotRequest pending_request;
            pending_request.camera_serial = camera_serial;
            if (camera_index < 0 ||
                !camera_is_group_capture_eligible(
                    cameras_select,
                    spatial_snapshot_workers,
                    camera_index)) {
                pending_request.failed = true;
                pending_request.error =
                    "camera stopped streaming before the presentation fence completed";
                ui_state->pending_group_snapshot_requests.push_back(
                    std::move(pending_request));
                continue;
            }

            uint64_t request_id = 0;
            std::string request_error;
            const std::string operation_id =
                ui_state->group_capture_id + "_Cam" + camera_serial;
            if (!spatial_snapshot_workers[camera_index]->RequestSnapshot(
                    operation_id,
                    &request_id,
                    &request_error,
                    ui_state->group_capture_target_frame_count)) {
                pending_request.failed = true;
                pending_request.error =
                    request_error.empty() ? "snapshot request rejected" : request_error;
            } else {
                pending_request.request_id = request_id;
                ++requested;
            }
            ui_state->pending_group_snapshot_requests.push_back(
                std::move(pending_request));
        }

        if (requested == 0) {
            ui_state->group_capture_workflow_state = "waiting_post_scene";
            ui_state->group_capture_status =
                "No expected camera accepted its snapshot request; recording the partial group.";
        } else {
            ui_state->group_capture_workflow_state = "capturing";
            std::ostringstream capture_status;
            capture_status << "Citrus scene revision "
                           << scene.value("scene_revision", uint64_t{0})
                           << " is presented; collecting fresh frame(s) from "
                           << requested << "/"
                           << ui_state->group_capture_expected_camera_serials.size()
                           << " expected camera(s).";
            ui_state->group_capture_status = capture_status.str();
        }
        ui_state->group_capture_next_scene_poll_at_seconds = 0.0;
        ui_state->group_capture_scene_deadline_at_seconds =
            monotonic_seconds() + calibration_scene_timeout_seconds();
        return;
    }

    if (ui_state->group_capture_workflow_state == "waiting_post_scene") {
        bool status_ok = false;
        std::string status_reason;
        if (ui_state->group_capture_scene_authority == "arena_centering") {
            const CitrusArenaCenteringControlResult status =
                query_citrus_arena_centering_status(
                    ui_state->group_capture_transaction_id,
                    "post_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                nlohmann::json scene = nlohmann::json::object();
                std::string readiness_error;
                if (arena_centering_presented_for_group_command(
                        status.centering,
                        *ui_state,
                        &scene,
                        &readiness_error)) {
                    status_ok = true;
                    ui_state->group_capture_scene_post_capture = scene;
                    ui_state->group_capture_metadata
                        .citrus_arena_centering_post_capture = status.centering;
                } else {
                    status_reason = readiness_error;
                }
            }
        } else if (ui_state->group_capture_scene_authority ==
                   "daily_registration") {
            const CitrusDailyRegistrationControlResult status =
                query_citrus_daily_registration_status("post_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                std::string readiness_error;
                if (daily_registration_preview_presented_for_group_capture(
                        status.daily_registration,
                        *ui_state,
                        &readiness_error)) {
                    status_ok = true;
                    ui_state->group_capture_scene_post_capture =
                        status.daily_registration;
                    ui_state->group_capture_metadata
                        .citrus_daily_registration_post_capture =
                        status.daily_registration;
                } else {
                    status_reason = readiness_error;
                }
            }
        } else {
            const CitrusCalibrationSceneControlResult status =
                query_citrus_calibration_scene_status(
                    ui_state->group_capture_transaction_id,
                    "post_capture_fence");
            status_reason = status.reason;
            if (status.ok) {
                status_ok = true;
                ui_state->group_capture_scene_post_capture = status.scene;
            }
        }
        if (!status_ok) {
            if (now >= ui_state->group_capture_scene_deadline_at_seconds) {
                ui_state->group_capture_scene_post_capture = nlohmann::json::object();
            } else {
                ui_state->group_capture_status =
                    "Camera frames acquired; waiting for post-capture Citrus scene status: " +
                    status_reason;
                return;
            }
        }

        const CitrusProjectionSnapshotQueryResult projection_snapshot =
            query_citrus_active_projection_snapshot(
                "post_group_capture",
                ui_state->group_capture_id);
        ui_state->group_capture_metadata.citrus_projection_snapshot_post_capture =
            projection_snapshot.ok
                ? projection_snapshot.snapshot
                : nlohmann::json::object();
        ui_state->group_capture_metadata.citrus_projection_epoch_consistency =
            nlohmann::json::object();
        if (ui_state->group_capture_scene_authority == "calibration_scene") {
            ui_state->group_capture_metadata.citrus_calibration_scene_post_capture =
                ui_state->group_capture_scene_post_capture;
            ui_state->group_capture_metadata.citrus_calibration_scene_consistency =
                make_group_scene_consistency(*ui_state);
        } else if (ui_state->group_capture_scene_authority == "arena_centering") {
            ui_state->group_capture_metadata.citrus_arena_centering_consistency =
                make_arena_centering_consistency(*ui_state);
        } else if (ui_state->group_capture_scene_authority ==
                   "daily_registration") {
            ui_state->group_capture_metadata.citrus_daily_registration_consistency =
                make_daily_registration_consistency(*ui_state);
        }
        ui_state->group_capture_metadata.capture_group_membership =
            make_group_membership(*ui_state);
        for (SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
            capture.metadata = ui_state->group_capture_metadata;
        }
        if (ui_state->captured_capture_group_id == ui_state->group_capture_id) {
            ui_state->captured_citrus_projection_snapshot_pre_capture =
                ui_state->group_capture_metadata.citrus_projection_snapshot_pre_capture;
            ui_state->captured_citrus_projection_snapshot_post_capture =
                ui_state->group_capture_metadata.citrus_projection_snapshot_post_capture;
            ui_state->captured_citrus_projection_epoch_consistency =
                ui_state->group_capture_metadata.citrus_projection_epoch_consistency;
            ui_state->captured_citrus_calibration_scene_pre_capture =
                ui_state->group_capture_metadata.citrus_calibration_scene_pre_capture;
            ui_state->captured_citrus_calibration_scene_post_capture =
                ui_state->group_capture_metadata.citrus_calibration_scene_post_capture;
            ui_state->captured_citrus_calibration_scene_consistency =
                ui_state->group_capture_metadata.citrus_calibration_scene_consistency;
            ui_state->captured_citrus_calibration_scene_restore_status =
                ui_state->group_capture_metadata.citrus_calibration_scene_restore_status;
            ui_state->captured_citrus_arena_centering_pre_capture =
                ui_state->group_capture_metadata.citrus_arena_centering_pre_capture;
            ui_state->captured_citrus_arena_centering_post_capture =
                ui_state->group_capture_metadata.citrus_arena_centering_post_capture;
            ui_state->captured_citrus_arena_centering_consistency =
                ui_state->group_capture_metadata.citrus_arena_centering_consistency;
            ui_state->captured_citrus_daily_registration_pre_capture =
                ui_state->group_capture_metadata.citrus_daily_registration_pre_capture;
            ui_state->captured_citrus_daily_registration_post_capture =
                ui_state->group_capture_metadata.citrus_daily_registration_post_capture;
            ui_state->captured_citrus_daily_registration_consistency =
                ui_state->group_capture_metadata.citrus_daily_registration_consistency;
            ui_state->captured_group_membership =
                ui_state->group_capture_metadata.capture_group_membership;
        }

        ui_state->group_capture_terminal_outcome =
            ui_state->group_capture_metadata.capture_group_membership.value(
                "status", std::string("failed"));
        if (ui_state->group_capture_terminal_outcome == "invalid_scene") {
            append_group_capture_error(
                ui_state,
                "The captured frames do not share one unchanged Citrus scene identity; the saved group will be marked invalid_scene.");
        }
        if (ui_state->group_capture_scene_authority == "arena_centering" ||
            ui_state->group_capture_scene_authority == "daily_registration") {
            ui_state->group_capture_restore_required = false;
            ui_state->group_capture_scene_restore_status = {
                {"state", "not_requested"},
                {"reason", ui_state->group_capture_scene_authority ==
                               "daily_registration"
                           ? "daily_registration_preview_lifecycle_remains_active"
                           : "arena_centering_transaction_remains_active"},
                {"transaction_id", ui_state->group_capture_transaction_id},
                {"stage_id", ui_state->group_capture_expected_stage_id},
            };
            ui_state->group_capture_metadata.citrus_calibration_scene_restore_status =
                ui_state->group_capture_scene_restore_status;
            for (SpatialLayoutGroupCaptureFrame& capture :
                 ui_state->group_captures) {
                capture.metadata.citrus_calibration_scene_restore_status =
                    ui_state->group_capture_scene_restore_status;
            }
            ui_state->captured_citrus_calibration_scene_restore_status =
                ui_state->group_capture_scene_restore_status;
            const bool failed =
                ui_state->group_capture_terminal_outcome == "failed";
            ui_state->group_capture_workflow_state = failed ? "failed" : "complete";
            ui_state->group_capture_status =
                (ui_state->group_capture_scene_authority == "daily_registration"
                     ? "Daily-registration preview grouped capture "
                     : "Arena-centering grouped capture ") +
                ui_state->group_capture_id +
                " finished with status " +
                ui_state->group_capture_terminal_outcome +
                "; the Citrus transaction remains active for the next probe.";
            return;
        }
        begin_group_scene_restore(ui_state);
        return;
    }

    if (ui_state->group_capture_workflow_state == "waiting_restore") {
        const CitrusCalibrationSceneControlResult status =
            query_citrus_calibration_scene_status(
                ui_state->group_capture_transaction_id,
                "restore_fence");
        if (!status.ok) {
            if (now >= ui_state->group_capture_scene_deadline_at_seconds) {
                ui_state->group_capture_workflow_state = "failed";
                append_group_capture_error(
                    ui_state,
                    "Timed out verifying that Citrus restored the prior projection scene. Manual recovery is required before experiment start.");
            }
            return;
        }
        const nlohmann::json last_command =
            status.scene.value("last_command", nlohmann::json::object());
        if (last_command.value("operation_id", std::string()) ==
                ui_state->group_capture_restore_operation_id &&
            last_command.value("status", std::string()) == "rejected") {
            ui_state->group_capture_workflow_state = "failed";
            append_group_capture_error(
                ui_state,
                "Citrus rejected scene restoration: " +
                    last_command.value("error", std::string("unknown error")));
            return;
        }
        if (!restore_presented_for_group_command(status.scene, *ui_state)) {
            if (now >= ui_state->group_capture_scene_deadline_at_seconds) {
                ui_state->group_capture_workflow_state = "failed";
                append_group_capture_error(
                    ui_state,
                    "Timed out waiting for the Citrus restore presentation fence. Manual recovery is required before experiment start.");
            }
            return;
        }

        ui_state->group_capture_scene_restore_status = status.scene;
        ui_state->group_capture_metadata.citrus_calibration_scene_restore_status =
            status.scene;
        for (SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
            capture.metadata.citrus_calibration_scene_restore_status = status.scene;
        }
        if (ui_state->captured_capture_group_id == ui_state->group_capture_id) {
            ui_state->captured_citrus_calibration_scene_restore_status = status.scene;
        }
        ui_state->group_capture_restore_required = false;
        const bool failed = ui_state->group_capture_terminal_outcome == "failed";
        ui_state->group_capture_workflow_state = failed ? "failed" : "complete";
        const std::string membership_status =
            ui_state->group_capture_metadata.capture_group_membership.value(
                "status", ui_state->group_capture_terminal_outcome);
        std::ostringstream complete_status;
        complete_status << "Guided grouped capture " << ui_state->group_capture_id
                        << " finished with status "
                        << (membership_status.empty() ? "failed" : membership_status)
                        << ": " << ui_state->group_captures.size() << "/"
                        << ui_state->group_capture_expected_camera_serials.size()
                        << " expected camera(s) captured; Citrus prior scene restored.";
        ui_state->group_capture_status = complete_status.str();
        release_group_capture_transaction_if_owned(
            ui_state,
            failed ? "failed" : "complete",
            ui_state->group_capture_status);
    }
}

}  // namespace orange::gui::spatial_layout
