#include "gui/spatial_layout/daily_registration_preview.h"

#include "gui/daily_registration_geometry.h"

#include <cmath>

namespace orange::gui::spatial_layout {
namespace {

namespace daily_geometry = orange::gui::daily_registration;

orange::spatial::RuntimeGeometry circle_geometry(
    double cx,
    double cy,
    double radius)
{
    orange::spatial::RuntimeGeometry geometry;
    geometry.type = orange::spatial::RuntimeGeometryType::kCircle;
    geometry.circle.cx = cx;
    geometry.circle.cy = cy;
    geometry.circle.r = radius;
    return geometry;
}

const nlohmann::json* object_member(
    const nlohmann::json& value,
    const char* key)
{
    if (!value.is_object()) return nullptr;
    const auto it = value.find(key);
    return it != value.end() && it->is_object() ? &*it : nullptr;
}

bool json_number(
    const nlohmann::json& value,
    const char* key,
    double* output)
{
    if (output == nullptr || !value.is_object()) return false;
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return false;
    *output = it->get<double>();
    return std::isfinite(*output);
}

std::string json_string(
    const nlohmann::json& value,
    const char* key)
{
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string()
        ? it->get<std::string>()
        : std::string();
}

const CitrusSpatialTemplateState* find_citrus_template_for_daily_overlay(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial,
    const std::string& arena_id)
{
    for (const auto& candidate : ui_state.citrus_canvas_templates) {
        if (candidate.available &&
            candidate.source_camera_id == camera_serial &&
            (arena_id.empty() || candidate.source_config_name == arena_id)) {
            return &candidate;
        }
    }
    if (ui_state.citrus_template.available &&
        ui_state.citrus_template.source_camera_id == camera_serial &&
        (arena_id.empty() ||
         ui_state.citrus_template.source_config_name == arena_id)) {
        return &ui_state.citrus_template;
    }
    return nullptr;
}

bool workflow_stage_has_daily_geometry(const std::string& stage)
{
    return stage == "review_geometry_candidate" ||
        stage == "waiting_accept" ||
        stage == "waiting_runtime_selection" || stage == "complete";
}

bool resolve_from_workflow(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial,
    DailyRegistrationPreviewOverlay* overlay)
{
    if (overlay == nullptr ||
        !workflow_stage_has_daily_geometry(
            ui_state.daily_registration_workflow.stage)) {
        return false;
    }
    const auto& workflow = ui_state.daily_registration_workflow;
    for (const auto& target : workflow.targets) {
        if (target.camera_serial != camera_serial ||
            target.accepted_rim_radius_camera_px <= 0.0 ||
            target.geometry_outline_camera_px.size() < 3) {
            continue;
        }
        overlay->available = true;
        overlay->selected_for_runtime = workflow.stage == "complete" &&
            !workflow.accepted_registration_path.empty();
        overlay->has_raw_hough_circle =
            target.detected_rim_radius_camera_px > 0.0;
        overlay->raw_hough_circle = circle_geometry(
            target.detected_rim_center_x_camera_px,
            target.detected_rim_center_y_camera_px,
            target.detected_rim_radius_camera_px);
        overlay->accepted_rim_circle = circle_geometry(
            target.accepted_rim_center_x_camera_px,
            target.accepted_rim_center_y_camera_px,
            target.accepted_rim_radius_camera_px);
        overlay->registered_center_camera_px = {
            target.geometry_corrected_center_x_camera_px,
            target.geometry_corrected_center_y_camera_px};
        overlay->registered_outline_camera_px =
            target.geometry_outline_camera_px;
        overlay->transaction_id = workflow.transaction_id;
        overlay->rim_artifact_id = target.rim_observation_artifact_id;
        const auto& runtime = workflow.citrus_runtime_selection_status;
        const auto targets_it = runtime.is_object()
            ? runtime.find("targets") : runtime.end();
        if (targets_it != runtime.end() && targets_it->is_array()) {
            for (const auto& runtime_target : *targets_it) {
                if (runtime_target.is_object() &&
                    json_string(runtime_target, "camera_id") ==
                        camera_serial) {
                    overlay->registration_id =
                        json_string(runtime_target, "registration_id");
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

const nlohmann::json* selected_runtime_from_status(
    const nlohmann::json& status)
{
    if (!status.is_object()) return nullptr;
    if (json_string(status, "schema_id") ==
            "citrus.runtime_daily_registration_compatibility") {
        return &status;
    }
    return object_member(status, "runtime");
}

bool resolve_from_runtime(
    const SpatialLayoutUiState& ui_state,
    const nlohmann::json& status,
    const std::string& camera_serial,
    DailyRegistrationPreviewOverlay* overlay)
{
    if (overlay == nullptr) return false;
    const nlohmann::json* runtime = selected_runtime_from_status(status);
    if (runtime == nullptr ||
        json_string(*runtime, "mode") != "selected_daily_registration") {
        return false;
    }
    const auto targets_it = runtime->find("targets");
    if (targets_it == runtime->end() || !targets_it->is_array()) return false;
    for (const auto& runtime_target : *targets_it) {
        if (!runtime_target.is_object() ||
            json_string(runtime_target, "camera_id") != camera_serial ||
            json_string(runtime_target, "state") != "selected_valid" ||
            json_string(runtime_target, "registration_id").empty() ||
            json_string(runtime_target, "registration_path").empty() ||
            json_string(runtime_target, "registration_sha256").empty() ||
            !runtime_target.value("applied", false)) {
            continue;
        }
        const nlohmann::json* target = object_member(runtime_target, "target");
        const nlohmann::json* rim_center = target == nullptr
            ? nullptr : object_member(*target, "rim_center_camera_px");
        const nlohmann::json* desired_center = target == nullptr
            ? nullptr
            : object_member(
                *target, "desired_experimental_area_center_canvas_px");
        const nlohmann::json* effective_center = target == nullptr
            ? nullptr
            : object_member(
                *target, "effective_experimental_area_center_canvas_px");
        const nlohmann::json* rim_observation = target == nullptr
            ? nullptr : object_member(*target, "rim_observation");
        double rim_x = 0.0;
        double rim_y = 0.0;
        double rim_radius = 0.0;
        double desired_x = 0.0;
        double desired_y = 0.0;
        double effective_x = 0.0;
        double effective_y = 0.0;
        if (target == nullptr || rim_center == nullptr ||
            desired_center == nullptr || effective_center == nullptr ||
            rim_observation == nullptr ||
            json_string(*rim_observation, "artifact_id").empty() ||
            json_string(*rim_observation, "path").empty() ||
            json_string(*rim_observation, "sha256").empty() ||
            !json_number(*rim_center, "x", &rim_x) ||
            !json_number(*rim_center, "y", &rim_y) ||
            !json_number(*target, "observed_rim_radius_camera_px", &rim_radius) ||
            !json_number(*desired_center, "x", &desired_x) ||
            !json_number(*desired_center, "y", &desired_y) ||
            !json_number(*effective_center, "x", &effective_x) ||
            !json_number(*effective_center, "y", &effective_y) ||
            rim_radius <= 0.0) {
            continue;
        }
        const std::string arena_id = json_string(runtime_target, "arena_id");
        const auto* template_state = find_citrus_template_for_daily_overlay(
            ui_state, camera_serial, arena_id);
        const nlohmann::json* homography =
            object_member(*target, "homography");
        if (template_state == nullptr ||
            !template_state->has_canvas_to_camera_homography ||
            !template_state->has_authoritative_camera_to_canvas_homography ||
            template_state->homography_authority_status !=
                "accepted_compatible" ||
            template_state->homography_target_plane != "projected_surface" ||
            homography == nullptr ||
            json_string(*homography, "candidate_id") !=
                template_state->homography_candidate_id ||
            json_string(*homography, "selected_canvas_name") !=
                template_state->source_canvas_name ||
            template_state->experimental_area_radius_px <= 0.0) {
            continue;
        }
        daily_geometry::GeometryReviewInput input;
        input.canvas_to_camera_homography =
            template_state->canvas_to_camera_homography;
        input.desired_experimental_center_canvas_x_px = desired_x;
        input.desired_experimental_center_canvas_y_px = desired_y;
        input.effective_experimental_center_canvas_x_px = effective_x;
        input.effective_experimental_center_canvas_y_px = effective_y;
        input.canonical_experimental_radius_canvas_px =
            template_state->experimental_area_radius_px;
        input.accepted_rim_center_camera_x_px = rim_x;
        input.accepted_rim_center_camera_y_px = rim_y;
        input.accepted_rim_radius_camera_px = rim_radius;
        const auto review = daily_geometry::ComputeGeometryReview(input);
        if (!review.ok || review.canonical_outline_camera_px.size() < 3) {
            continue;
        }
        overlay->available = true;
        overlay->selected_for_runtime = true;
        overlay->accepted_rim_circle = circle_geometry(
            rim_x, rim_y, rim_radius);
        overlay->registered_center_camera_px = {
            review.corrected_center_camera_px.x,
            review.corrected_center_camera_px.y};
        overlay->registered_outline_camera_px.reserve(
            review.canonical_outline_camera_px.size());
        for (const auto& point : review.canonical_outline_camera_px) {
            overlay->registered_outline_camera_px.push_back(
                {point.x, point.y});
        }
        overlay->transaction_id = json_string(status, "transaction_id");
        overlay->registration_id =
            json_string(runtime_target, "registration_id");
        overlay->rim_artifact_id =
            json_string(*rim_observation, "artifact_id");
        return true;
    }
    return false;
}

}  // namespace

bool resolve_daily_registration_preview_overlay(
    const SpatialLayoutUiState& ui_state,
    DailyRegistrationPreviewOverlay* overlay)
{
    if (overlay == nullptr) return false;
    *overlay = DailyRegistrationPreviewOverlay{};
    const std::string camera_serial = !ui_state.captured_camera_serial.empty()
        ? ui_state.captured_camera_serial
        : ui_state.citrus_template.source_camera_id;
    if (camera_serial.empty()) return false;

    if (resolve_from_workflow(ui_state, camera_serial, overlay)) return true;
    if (resolve_from_runtime(
            ui_state, ui_state.daily_registration_status,
            camera_serial, overlay)) {
        return true;
    }
    if (resolve_from_runtime(
            ui_state,
            ui_state.daily_registration_workflow
                .citrus_runtime_selection_status,
            camera_serial, overlay)) {
        return true;
    }
    if (resolve_from_runtime(
            ui_state,
            ui_state.captured_citrus_daily_registration_post_capture,
            camera_serial, overlay)) {
        return true;
    }
    return resolve_from_runtime(
        ui_state,
        ui_state.captured_citrus_daily_registration_pre_capture,
        camera_serial, overlay);
}

}  // namespace orange::gui::spatial_layout
