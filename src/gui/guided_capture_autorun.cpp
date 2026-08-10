#include "gui/guided_capture_autorun.h"

#include "gui/env_util.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/citrus_template_workflow.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/preflight.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/save_job_preparation.h"
#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/sha256.h"
#include "gui/projected_surface_scale_artifact.h"
#include "fsuid_guard.h"
#include "project.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

#include <unistd.h>

namespace orange::gui {
namespace {

using orange::gui::spatial_layout::apply_calibration_image_set_purpose_defaults;
using orange::gui::spatial_layout::apply_calibration_workflow_profile_defaults;
using orange::gui::spatial_layout::generic_calibration_image_set_save_worker_is_busy;
using orange::gui::spatial_layout::group_capture_workflow_active;
using orange::gui::spatial_layout::import_citrus_canvas_templates;
using orange::gui::spatial_layout::initialize_spatial_layout_defaults;
using orange::gui::spatial_layout::queue_group_calibration_image_set_save_jobs;
using orange::gui::spatial_layout::queued_generic_calibration_image_set_save_job_count;
using orange::gui::spatial_layout::camera_has_exposed_mapped_nir_strobe;
using orange::gui::spatial_layout::CalibrationCaptureTiming;
using orange::gui::spatial_layout::prepare_calibration_capture_preflight_camera_serials;
using orange::gui::spatial_layout::request_group_full_resolution_snapshots;
using orange::gui::spatial_layout::restore_calibration_capture_preflight_all_cameras;
using orange::gui::spatial_layout::set_calibration_preflight_result;

bool recipe_supported(const std::string& recipe)
{
    return recipe == "black_reference" ||
           recipe == "uniform_gray" ||
           recipe == "arena_outline" ||
           recipe == "experimental_area_center_and_outline" ||
           recipe == "homography_grid" ||
           recipe == "homography_rings" ||
           recipe == "verification_dots";
}

bool purpose_is_schema_saveable(const std::string& purpose)
{
    static const std::set<std::string> allowed = {
        "camera_arena_calibration_set",
        "arena_projection",
        "homography_grid",
        "verification_dots",
        "validation_pattern",
        "dry_physical_target_height_parallax_diagnostic",
        "camera_only_physical_target_calibration",
        "projected_surface_scale_calibration",
        "scale_image",
        "dish_top_rim",
        "crosshair_alignment",
    };
    return allowed.count(purpose) != 0;
}

bool recipe_supports_foreground_gray(const std::string& recipe)
{
    return recipe == "homography_grid" ||
           recipe == "homography_rings" ||
           recipe == "verification_dots" ||
           recipe == "uniform_gray" ||
           recipe == "arena_outline";
}

std::string purpose_for_recipe(const std::string& recipe)
{
    if (recipe == "black_reference" || recipe == "uniform_gray") {
        return "validation_pattern";
    }
    if (recipe == "arena_outline") {
        return "arena_projection";
    }
    if (recipe == "experimental_area_center_and_outline") {
        return "crosshair_alignment";
    }
    if (recipe == "homography_grid" || recipe == "homography_rings") {
        return "homography_grid";
    }
    if (recipe == "verification_dots") {
        return "verification_dots";
    }
    return "validation_pattern";
}

bool should_poll_scale_status(GuidedCaptureAutorunState* state)
{
    const auto now = std::chrono::steady_clock::now();
    if (state->projected_surface_scale_last_poll_at.time_since_epoch().count() != 0 &&
        now - state->projected_surface_scale_last_poll_at <
            std::chrono::milliseconds(250)) {
        return false;
    }
    state->projected_surface_scale_last_poll_at = now;
    return true;
}

bool should_poll_homography_status(GuidedCaptureAutorunState* state)
{
    const auto now = std::chrono::steady_clock::now();
    if (state->homography_last_poll_at.time_since_epoch().count() != 0 &&
        now - state->homography_last_poll_at < std::chrono::milliseconds(250)) {
        return false;
    }
    state->homography_last_poll_at = now;
    return true;
}

const nlohmann::json* completed_sample_for_recipe(
    const GuidedCaptureAutorunState& state,
    const std::string& recipe)
{
    for (const auto& sample : state.completed_samples) {
        if (sample.is_object() && sample.value("recipe", "") == recipe) {
            return &sample;
        }
    }
    return nullptr;
}

bool prepare_homography_fit_request(
    GuidedCaptureAutorunState* state,
    const GuidedCaptureAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    std::string* error_out)
{
    const std::string recipe = config.fixture_aperture_shape == "rectangle"
        ? "homography_grid"
        : "homography_rings";
    const nlohmann::json* sample = completed_sample_for_recipe(*state, recipe);
    if (sample == nullptr) {
        if (error_out) *error_out = "saved homography support sample is missing";
        return false;
    }
    state->homography_capture_group_id = sample->value(
        "workflow", nlohmann::json::object()).value("capture_group_id", "");
    const auto scene = sample->value(
        "workflow", nlohmann::json::object()).value(
            "citrus_scene_pre_capture", nlohmann::json::object());
    nlohmann::json targets = nlohmann::json::array();
    std::set<std::string> requested_cameras(
        config.camera_serials.begin(), config.camera_serials.end());
    for (const auto& target : scene.value(
             "resolved_targets", nlohmann::json::array())) {
        const std::string arena_id = target.value("arena_id", "");
        for (const auto& camera_value : target.value(
                 "associated_camera_ids", nlohmann::json::array())) {
            if (!camera_value.is_string()) continue;
            const std::string camera = camera_value.get<std::string>();
            if (requested_cameras.erase(camera) == 0) continue;
            targets.push_back({
                {"arena_id", arena_id},
                {"camera_id", camera},
                {"orange_artifact_id",
                 "Cam" + spatial_layout::sanitize_artifact_component(camera) +
                     "_" + spatial_layout::sanitize_artifact_component(arena_id)},
            });
        }
    }
    if (state->homography_capture_group_id.empty() ||
        !requested_cameras.empty() || targets.empty() ||
        spatial_state.calibration_session_dir.empty()) {
        if (error_out) {
            *error_out = "homography support sample is missing a complete target/session identity";
        }
        return false;
    }
    if (!spatial_layout::checksum::file_sha256(
            config.citrus_config_path,
            &state->homography_canvas_sha256,
            error_out)) {
        return false;
    }
    state->homography_targets = std::move(targets);
    state->homography_transaction_id =
        "holder_operational_" + spatial_layout::sanitize_artifact_component(
            state->homography_capture_group_id);
    return true;
}

nlohmann::json holder_homography_quality_thresholds(
    const GuidedCaptureAutorunConfig& config,
    const GuidedCaptureAutorunState& state)
{
    nlohmann::json thresholds = {
        {"maximum_rms_reprojection_error_canvas_px", 0.5},
        {"maximum_point_reprojection_error_canvas_px", 1.5},
        {"minimum_inlier_ratio", 0.95},
        {"maximum_holdout_rms_error_canvas_px", 0.75},
        {"maximum_holdout_error_canvas_px", 2.0},
        {"saturation_pixel_threshold_u8", 250},
        {"maximum_dot_core_saturation_fraction", 0.005},
        {"minimum_dot_background_contrast_u8", 20.0},
        {"commissioned_foreground_gray_u8", config.foreground_gray_u8},
    };
    const std::string support_recipe = config.fixture_aperture_shape == "rectangle"
        ? "homography_grid"
        : "homography_rings";
    const nlohmann::json* sample = completed_sample_for_recipe(
        state, support_recipe);
    if (sample == nullptr) return thresholds;
    const auto commissioning = sample->value(
        "workflow", nlohmann::json::object()).value(
            "capture_group_membership", nlohmann::json::object()).value(
                "projector_intensity_commissioning",
                nlohmann::json::object());
    const std::string report_path = commissioning.value("report_path", "");
    const std::string report_sha256 = commissioning.value("report_sha256", "");
    if (!report_path.empty() && !report_sha256.empty()) {
        thresholds["projector_intensity_report_path"] = report_path;
        thresholds["projector_intensity_report_sha256"] = report_sha256;
    }
    return thresholds;
}

int find_camera_index(const CameraParams* cameras_params,
                      int num_cameras,
                      const std::string& serial)
{
    if (cameras_params == nullptr) {
        return -1;
    }
    for (int index = 0; index < num_cameras; ++index) {
        if (cameras_params[index].camera_serial == serial) {
            return index;
        }
    }
    return -1;
}

double stage_elapsed_seconds(const GuidedCaptureAutorunState& state)
{
    if (state.stage_started_at.time_since_epoch().count() == 0) {
        return 0.0;
    }
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.stage_started_at).count();
}

void enter_stage(GuidedCaptureAutorunState* state, GuidedCaptureAutorunStage stage)
{
    if (state == nullptr) {
        return;
    }
    state->stage = stage;
    state->stage_started_at = std::chrono::steady_clock::now();
    state->action_requested = false;
    std::cout << "[GUI][guided_capture_autorun] stage="
              << guided_capture_autorun_stage_name(stage) << std::endl;
}

void fail(GuidedCaptureAutorunState* state, const std::string& error)
{
    if (state == nullptr) {
        return;
    }
    state->run_passed = false;
    state->error_message = error.empty() ? "guided capture autorun failed" : error;
    std::cerr << "[GUI][guided_capture_autorun] failed: "
              << state->error_message << std::endl;
    enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
}

void apply_diagnostic_black_reference_metadata(SpatialLayoutUiState* spatial_state)
{
    spatial_state->calibration_image_set_purpose = "diagnostic_black_reference";
    spatial_state->calibration_image_set_target_plane = "projected_surface";
    spatial_state->calibration_image_set_image_role = "black_reference";
    spatial_state->calibration_image_set_projected_pattern_id =
        "citrus_black_reference";
    spatial_state->calibration_image_set_projected_pattern_type = "black_reference";
    spatial_state->calibration_pattern_type = "none";
    spatial_state->calibration_pattern_domain = "full_projected_surface";
    spatial_state->calibration_projector_state = "black_reference";
    spatial_state->calibration_projector_visible_to_camera = false;
    spatial_state->calibration_reference_only = true;
    spatial_state->calibration_projected_pattern_used_as_coordinate_target = false;
    spatial_state->calibration_operator_notes =
        "Automated GUI grouped-capture smoke; black-reference scene; not a persisted calibration image set.";
}

std::string current_recipe(const GuidedCaptureAutorunState& state,
                           const GuidedCaptureAutorunConfig& config)
{
    if (!config.recipe_sequence.empty()) {
        return config.recipe_sequence[std::min(
            state.recipe_sequence_index, config.recipe_sequence.size() - 1)];
    }
    return state.current_sample_is_arena_outline ? "arena_outline" : config.recipe;
}

std::string current_purpose(const GuidedCaptureAutorunState& state,
                            const GuidedCaptureAutorunConfig& config)
{
    if (!config.recipe_sequence.empty()) {
        return purpose_for_recipe(current_recipe(state, config));
    }
    return state.current_sample_is_arena_outline ? "arena_projection" : config.purpose;
}

void apply_sequence_recipe_metadata(const std::string& recipe,
                                    const GuidedCaptureAutorunConfig& config,
                                    SpatialLayoutUiState* spatial_state)
{
    if (spatial_state == nullptr || config.recipe_sequence.empty()) {
        return;
    }
    const bool is_holder_homography_candidate =
        config.workflow_profile_id == "holder_installed_projected_surface" &&
        (recipe == "homography_grid" || recipe == "homography_rings");
    spatial_state->calibration_homography_role =
        is_holder_homography_candidate
            ? "operational_candidate"
            : "validation_only";
    spatial_state->calibration_reference_only = true;
    spatial_state->calibration_physical_target_used = false;
    spatial_state->calibration_visibility_domain_shape =
        config.fixture_aperture_shape;

    if (recipe == "black_reference") {
        spatial_state->calibration_image_set_image_role = "black_reference";
        spatial_state->calibration_image_set_projected_pattern_id =
            "citrus_black_reference";
        spatial_state->calibration_image_set_projected_pattern_type =
            "black_reference";
        spatial_state->calibration_pattern_type = "none";
        spatial_state->calibration_pattern_domain = "full_projected_surface";
        spatial_state->calibration_projector_state = "black_reference";
        spatial_state->calibration_projector_visible_to_camera = false;
        spatial_state->calibration_target_method = "not_applicable";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = false;
    } else if (recipe == "uniform_gray") {
        spatial_state->calibration_image_set_image_role = "uniform_gray_reference";
        spatial_state->calibration_image_set_projected_pattern_id =
            "citrus_uniform_gray_fixture_aperture";
        spatial_state->calibration_image_set_projected_pattern_type = "uniform_gray";
        spatial_state->calibration_pattern_type = "validation_pattern";
        spatial_state->calibration_pattern_domain = "full_projected_surface";
        spatial_state->calibration_projector_state = "uniform_gray_illumination";
        spatial_state->calibration_projector_visible_to_camera = true;
        spatial_state->calibration_target_method = "projected_pattern_on_diffuser";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = false;
    } else if (recipe == "arena_outline") {
        spatial_state->calibration_pattern_domain = "full_projected_surface";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = false;
    } else if (recipe == "homography_grid") {
        spatial_state->calibration_reference_only = false;
        spatial_state->calibration_image_set_image_role = "grid_on";
        spatial_state->calibration_image_set_projected_pattern_id =
            "citrus_holder_installed_rectangular_grid_validation_v1";
        spatial_state->calibration_image_set_projected_pattern_type = "dot_grid";
        spatial_state->calibration_pattern_type = "rectangular_grid";
        spatial_state->calibration_pattern_domain = "full_projected_surface";
        spatial_state->calibration_projector_state =
            "holder_installed_rectangular_grid_validation_on";
        spatial_state->calibration_target_method = "projected_pattern_on_diffuser";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = true;
    } else if (recipe == "homography_rings") {
        spatial_state->calibration_reference_only = false;
        spatial_state->calibration_pattern_domain = "circular_experimental_domain";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = true;
    } else if (recipe == "verification_dots") {
        spatial_state->calibration_pattern_domain = "circular_experimental_domain";
        spatial_state->calibration_target_method = "projected_pattern_on_diffuser";
        spatial_state->calibration_projected_pattern_used_as_coordinate_target = true;
    }
    spatial_state->calibration_operator_notes = is_holder_homography_candidate
        ? "Automated holder-installed, dish-absent operational projected-surface "
          "homography candidate capture. The fitted candidate remains subject to "
          "independent holder-aperture and verification-dot review and explicit "
          "promotion."
        : "Automated holder-installed, dish-absent fixture-aperture validation "
          "evidence; this image does not itself define a replacement transform.";
}

nlohmann::json capture_json(const SpatialLayoutGroupCaptureFrame& capture)
{
    return {
        {"camera_serial", capture.camera_serial},
        {"camera_name", capture.camera_name},
        {"valid", capture.valid},
        {"width", capture.width},
        {"height", capture.height},
        {"capture_mode", capture.capture_mode},
        {"source_array_role", capture.source_array_role},
        {"source_frame_count", capture.source_frame_count},
        {"first_local_frame_id", capture.first_local_frame_id},
        {"last_local_frame_id", capture.last_local_frame_id},
        {"first_camera_frame_id", capture.first_camera_frame_id},
        {"last_camera_frame_id", capture.last_camera_frame_id},
        {"camera_timestamp_ns", capture.camera_timestamp_ns},
        {"timestamp_sys_ns", capture.timestamp_sys_ns},
    };
}

nlohmann::json ptp_alignment_json(const SpatialLayoutUiState& spatial_state)
{
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0;
    std::size_t count = 0;
    for (const SpatialLayoutGroupCaptureFrame& capture : spatial_state.group_captures) {
        if (!capture.valid || capture.camera_timestamp_ns == 0) {
            continue;
        }
        minimum = std::min(minimum, capture.camera_timestamp_ns);
        maximum = std::max(maximum, capture.camera_timestamp_ns);
        ++count;
    }
    return {
        {"clock_domain", "camera_ptp_timestamp_ns"},
        {"camera_count", count},
        {"minimum_timestamp_ns", count == 0 ? 0 : minimum},
        {"maximum_timestamp_ns", count == 0 ? 0 : maximum},
        {"span_ns", count == 0 ? 0 : maximum - minimum},
    };
}

nlohmann::json captures_json(const SpatialLayoutUiState& spatial_state)
{
    nlohmann::json captures = nlohmann::json::array();
    for (const SpatialLayoutGroupCaptureFrame& capture : spatial_state.group_captures) {
        captures.push_back(capture_json(capture));
    }
    return captures;
}

nlohmann::json workflow_json(const SpatialLayoutUiState& spatial_state)
{
    return {
        {"state", spatial_state.group_capture_workflow_state},
        {"terminal_outcome", spatial_state.group_capture_terminal_outcome},
        {"capture_group_id", spatial_state.group_capture_id},
        {"transaction_id", spatial_state.group_capture_transaction_id},
        {"expected_camera_serials",
         spatial_state.group_capture_expected_camera_serials},
        {"arena_ids", spatial_state.group_capture_arena_ids},
        {"status_text", spatial_state.group_capture_status},
        {"error_text", spatial_state.group_capture_error},
        {"capture_group_membership",
         spatial_state.group_capture_metadata.capture_group_membership},
        {"citrus_scene_pre_capture",
         spatial_state.group_capture_scene_pre_capture},
        {"citrus_scene_post_capture",
         spatial_state.group_capture_scene_post_capture},
        {"citrus_scene_consistency",
         spatial_state.group_capture_metadata.citrus_calibration_scene_consistency},
        {"citrus_scene_restore_status",
         spatial_state.group_capture_scene_restore_status},
    };
}

nlohmann::json persistence_json(const GuidedCaptureAutorunConfig& config,
                                const SpatialLayoutUiState& spatial_state)
{
    return {
        {"requested", config.save_captures},
        {"session_id", spatial_state.calibration_session_id},
        {"session_dir", spatial_state.calibration_session_dir},
        {"status", spatial_state.persistence_status},
        {"error", spatial_state.persistence_error},
    };
}

std::vector<std::uint8_t> effective_sweep_levels(
    const GuidedCaptureAutorunConfig& config)
{
    if (!config.sweep_foreground_grays_u8.empty()) {
        return config.sweep_foreground_grays_u8;
    }
    return {config.foreground_gray_u8};
}

std::uint8_t current_sweep_gray(const GuidedCaptureAutorunState& state,
                                const GuidedCaptureAutorunConfig& config)
{
    const std::vector<std::uint8_t> levels = effective_sweep_levels(config);
    return levels[std::min(state.sweep_level_index, levels.size() - 1)];
}

void apply_current_sweep_scene(GuidedCaptureAutorunState* state,
                               const GuidedCaptureAutorunConfig& config,
                               SpatialLayoutUiState* spatial_state)
{
    const std::string recipe = current_recipe(*state, config);
    const std::string purpose = current_purpose(*state, config);
    spatial_state->group_capture_scene_recipe = recipe;
    if (purpose == "diagnostic_black_reference") {
        apply_diagnostic_black_reference_metadata(spatial_state);
    } else {
        apply_calibration_image_set_purpose_defaults(spatial_state, purpose);
    }
    apply_sequence_recipe_metadata(recipe, config, spatial_state);
    spatial_state->group_capture_scene_options = nlohmann::json::object();
    if (recipe_supports_foreground_gray(recipe)) {
        spatial_state->group_capture_scene_options["foreground_gray_u8"] =
            current_sweep_gray(*state, config);
    }
}

void append_completed_sample(GuidedCaptureAutorunState* state,
                             const GuidedCaptureAutorunConfig& config,
                             const SpatialLayoutUiState& spatial_state)
{
    const std::string recipe = current_recipe(*state, config);
    state->completed_samples.push_back({
        {"sample_index", state->completed_samples.size()},
        {"recipe", recipe},
        {"purpose", current_purpose(*state, config)},
        {"foreground_gray_u8", recipe_supports_foreground_gray(recipe)
                                   ? nlohmann::json(current_sweep_gray(*state, config))
                                   : nlohmann::json(nullptr)},
        {"repeat_index", config.recipe_sequence.empty()
                             ? state->sweep_repeat_index
                             : 1},
        {"workflow", workflow_json(spatial_state)},
        {"captures", captures_json(spatial_state)},
        {"ptp_capture_alignment", ptp_alignment_json(spatial_state)},
        {"persistence", persistence_json(config, spatial_state)},
    });
}

bool advance_sweep(GuidedCaptureAutorunState* state,
                   const GuidedCaptureAutorunConfig& config)
{
    if (!config.recipe_sequence.empty()) {
        ++state->recipe_sequence_index;
        return state->recipe_sequence_index < config.recipe_sequence.size();
    }
    if (state->current_sample_is_arena_outline) {
        state->current_sample_is_arena_outline = false;
        state->sweep_level_index = 0;
        state->sweep_repeat_index = 1;
        return true;
    }
    if (state->sweep_repeat_index < config.sweep_repeats) {
        ++state->sweep_repeat_index;
        return true;
    }
    state->sweep_repeat_index = 1;
    ++state->sweep_level_index;
    return state->sweep_level_index < effective_sweep_levels(config).size();
}

bool request_current_sweep_sample(
    GuidedCaptureAutorunState* state,
    const GuidedCaptureAutorunConfig& config,
    SpatialLayoutUiState* spatial_state,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    const int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    std::string* error_out)
{
    apply_current_sweep_scene(state, config, spatial_state);
    const std::string recipe = current_recipe(*state, config);
    if (recipe == "arena_outline") {
        std::cout << "[GUI][guided_capture_autorun] requesting arena outline"
                  << " with center fiducial" << std::endl;
    } else {
        std::cout << "[GUI][guided_capture_autorun] requesting recipe=" << recipe;
        if (recipe_supports_foreground_gray(recipe)) {
            std::cout << " gray="
                      << static_cast<unsigned int>(current_sweep_gray(*state, config));
        }
        std::cout << " repeat=" << state->sweep_repeat_index
                  << "/" << config.sweep_repeats << std::endl;
    }
    return request_group_full_resolution_snapshots(
        spatial_state,
        cameras_params,
        cameras_select,
        num_cameras,
        spatial_snapshot_workers,
        config.frame_count,
        error_out,
        std::string(),
        std::string(),
        spatial_layout::kGuidedCommissioningTransactionOwner);
}

bool write_json_atomically(const std::string& path,
                           const nlohmann::json& payload,
                           std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (path.empty()) {
        if (error_out) {
            *error_out = "guided capture result JSON path is empty";
        }
        return false;
    }
    const std::filesystem::path destination(path);
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            if (error_out) {
                *error_out = "could not create result directory: " + ec.message();
            }
            return false;
        }
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error_out) {
                *error_out = "could not open temporary result JSON: " + temporary.string();
            }
            return false;
        }
        output << payload.dump(2) << '\n';
        output.flush();
        if (!output) {
            if (error_out) {
                *error_out = "could not write temporary result JSON: " + temporary.string();
            }
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(destination, ec);
        ec.clear();
        std::filesystem::rename(temporary, destination, ec);
    }
    if (ec) {
        if (error_out) {
            *error_out = "could not publish result JSON: " + ec.message();
        }
        return false;
    }
    return true;
}

void finish_result(GuidedCaptureAutorunState* state,
                   const GuidedCaptureAutorunConfig& config,
                   const SpatialLayoutUiState& spatial_state,
                   bool stream_stopped)
{
    if (state == nullptr || state->result_written) {
        return;
    }
    const nlohmann::json result =
        guided_capture_autorun_result_json(*state, config, spatial_state, stream_stopped);
    state->result_written = write_json_atomically(
        config.result_json_path, result, &state->result_write_error);
    if (!state->result_written) {
        state->run_passed = false;
        if (state->error_message.empty()) {
            state->error_message = state->result_write_error;
        }
        std::cerr << "[GUI][guided_capture_autorun] result write failed: "
                  << state->result_write_error << std::endl;
        return;
    }
    std::cout << "[GUI][guided_capture_autorun] result="
              << config.result_json_path
              << " status=" << (state->run_passed ? "pass" : "fail")
              << std::endl;
}

}  // namespace

GuidedCaptureAutorunRequests guided_capture_autorun_update(
    GuidedCaptureAutorunState* state,
    const GuidedCaptureAutorunConfig& config,
    SpatialLayoutUiState* spatial_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& calibration_sessions_root)
{
    GuidedCaptureAutorunRequests requests;
    if (state == nullptr || spatial_state == nullptr || camera_control == nullptr ||
        !config.enabled || state->stage == GuidedCaptureAutorunStage::kDisabled ||
        state->stage == GuidedCaptureAutorunStage::kDone ||
        state->stage == GuidedCaptureAutorunStage::kFailed) {
        return requests;
    }

    switch (state->stage) {
    case GuidedCaptureAutorunStage::kWaitForStream:
        if (camera_control->subscribe && ecams != nullptr && cameras_params != nullptr &&
            cameras_select != nullptr && spatial_snapshot_workers != nullptr &&
            num_cameras > 0) {
            enter_stage(state, GuidedCaptureAutorunStage::kPrepare);
        } else if (stage_elapsed_seconds(*state) >= config.startup_timeout_seconds) {
            fail(state, "timed out waiting for Orange cameras and streaming");
        }
        break;

    case GuidedCaptureAutorunStage::kPrepare: {
        if (!recipe_supported(config.recipe)) {
            fail(state, "unsupported Citrus calibration scene recipe: " + config.recipe);
            break;
        }
        for (const std::string& recipe : config.recipe_sequence) {
            if (!recipe_supported(recipe)) {
                fail(state, "unsupported Citrus recipe in capture sequence: " + recipe);
                break;
            }
        }
        if (state->stage == GuidedCaptureAutorunStage::kStopStream) {
            break;
        }
        if (!config.recipe_sequence.empty() &&
            (!config.sweep_foreground_grays_u8.empty() ||
             config.sweep_repeats != 1 ||
             config.include_arena_outline_reference)) {
            fail(state,
                 "an explicit recipe sequence cannot be combined with an intensity "
                 "sweep or the legacy arena-outline prefix");
            break;
        }
        if (config.citrus_config_path.empty() ||
            !std::filesystem::is_regular_file(config.citrus_config_path)) {
            fail(state, "Citrus canvas config is missing: " + config.citrus_config_path);
            break;
        }
        std::vector<std::string> requested_cameras = config.camera_serials;
        if (requested_cameras.empty()) {
            for (int index = 0; index < num_cameras; ++index) {
                if (!cameras_params[index].camera_serial.empty()) {
                    requested_cameras.push_back(cameras_params[index].camera_serial);
                }
            }
        }
        if (requested_cameras.empty()) {
            fail(state, "guided capture autorun has no camera scope");
            break;
        }
        const int selected_camera_index =
            find_camera_index(cameras_params, num_cameras, requested_cameras.front());
        if (selected_camera_index < 0) {
            fail(state, "requested camera is not open: " + requested_cameras.front());
            break;
        }
        for (const std::string& serial : requested_cameras) {
            if (find_camera_index(cameras_params, num_cameras, serial) < 0) {
                fail(state, "requested camera is not open: " + serial);
                break;
            }
        }
        if (state->stage == GuidedCaptureAutorunStage::kStopStream) {
            break;
        }

        initialize_spatial_layout_defaults(spatial_state);
        spatial_state->show_window = true;
        spatial_state->selected_camera = selected_camera_index;
        spatial_state->configured_camera_index = selected_camera_index;
        std::string import_status;
        std::string import_error;
        if (!import_citrus_canvas_templates(
                spatial_state,
                cameras_params[selected_camera_index],
                config.citrus_config_path,
                &import_status,
                &import_error)) {
            fail(state, "Citrus canvas import failed: " + import_error);
            break;
        }
        spatial_state->citrus_import_status = import_status;
        spatial_state->citrus_import_error.clear();
        if (!config.workflow_profile_id.empty()) {
            std::string profile_error;
            if (!apply_calibration_workflow_profile_defaults(
                    spatial_state,
                    config.workflow_profile_id,
                    &profile_error)) {
                fail(state, profile_error);
                break;
            }
            if (spatial_state->calibration_image_set_purpose != config.purpose) {
                apply_calibration_image_set_purpose_defaults(
                    spatial_state, config.purpose);
            }
        } else if (config.purpose == "diagnostic_black_reference") {
            apply_diagnostic_black_reference_metadata(spatial_state);
        } else {
            apply_calibration_image_set_purpose_defaults(spatial_state, config.purpose);
        }
        if (config.workflow_profile_id == "holder_installed_projected_surface") {
            spatial_state->calibration_visibility_domain_shape =
                config.fixture_aperture_shape;
        }
        bool sequence_purposes_saveable = true;
        for (const std::string& recipe : config.recipe_sequence) {
            sequence_purposes_saveable = sequence_purposes_saveable &&
                                         purpose_is_schema_saveable(
                                             purpose_for_recipe(recipe));
        }
        if (config.save_captures &&
            (!purpose_is_schema_saveable(config.purpose) ||
             !sequence_purposes_saveable)) {
            fail(state,
                 "capture saving requires a schema-supported purpose; got " +
                     config.purpose);
            break;
        }
        if (config.purpose == "projected_surface_scale_calibration") {
            if (!config.save_captures) {
                fail(state, "projected-surface scale commissioning requires --save");
                break;
            }
            if (!config.projected_surface_targets_ready_confirmed) {
                fail(state,
                     "projected-surface scale commissioning requires explicit confirmation "
                     "that one 3 mm target is placed in every camera view");
                break;
            }
            if (effective_sweep_levels(config).size() != 1 ||
                config.sweep_repeats != 1 ||
                config.include_arena_outline_reference ||
                !config.recipe_sequence.empty()) {
                fail(state,
                     "projected-surface scale commissioning accepts exactly one uniform-gray "
                     "PTP-grouped sample and no arena-outline reference");
                break;
            }
        }
        if (config.fit_homographies_after_capture) {
            const std::string required_support =
                config.fixture_aperture_shape == "rectangle"
                    ? "homography_grid"
                    : "homography_rings";
            const bool has_support = std::find(
                config.recipe_sequence.begin(),
                config.recipe_sequence.end(),
                required_support) != config.recipe_sequence.end();
            const bool has_verification = std::find(
                config.recipe_sequence.begin(),
                config.recipe_sequence.end(),
                "verification_dots") != config.recipe_sequence.end();
            if (config.workflow_profile_id !=
                    "holder_installed_projected_surface" ||
                !config.save_captures || !has_support || !has_verification) {
                fail(state,
                     "holder operational homography fitting requires the "
                     "holder-installed profile, saved support capture, and "
                     "independent verification-dots capture");
                break;
            }
        }
        spatial_state->group_capture_scene_recipe = config.recipe;
        state->recipe_sequence_index = 0;
        state->sweep_level_index = 0;
        state->sweep_repeat_index = 1;
        state->current_sample_is_arena_outline =
            config.include_arena_outline_reference;
        apply_current_sweep_scene(state, config, spatial_state);
        spatial_state->group_capture_selected_camera_serials = requested_cameras;
        spatial_state->group_capture_camera_scope_initialized = true;
        state->prepared_camera_serials = requested_cameras;

        state->calibration_transaction_id =
            "guided_commissioning_" + get_current_utc_timestamp() + "_" +
            std::to_string(static_cast<long long>(::getpid()));
        std::string transaction_error;
        if (!spatial_layout::acquire_spatial_calibration_transaction(
                spatial_state,
                spatial_layout::kGuidedCommissioningTransactionOwner,
                state->calibration_transaction_id,
                orange::calibration::WorkflowKind::kGuidedCommissioning,
                requested_cameras,
                orange::calibration::Mutation::kCameraParameters |
                    orange::calibration::Mutation::kCameraStreamLifecycle |
                    orange::calibration::Mutation::kCitrusScene,
                "Run one guided commissioning capture sequence with stable camera and projector state.",
                &transaction_error)) {
            fail(state, transaction_error);
            break;
        }

        if (config.apply_calibration_preflight) {
            int light_camera_index = -1;
            for (int index = 0; index < num_cameras; ++index) {
                if (camera_has_exposed_mapped_nir_strobe(cameras_params[index])) {
                    light_camera_index = index;
                    break;
                }
            }
            const bool mapped_strobe_available = light_camera_index >= 0;
            CameraEmergent* light_ecam =
                mapped_strobe_available ? &ecams[light_camera_index] : nullptr;
            const CameraParams* light_params =
                mapped_strobe_available ? &cameras_params[light_camera_index] : nullptr;
            const CalibrationCaptureTiming timing{
                config.calibration_frame_rate_hz,
                config.calibration_exposure_us};
            const bool preflight_ok =
                prepare_calibration_capture_preflight_camera_serials(
                    spatial_state,
                    ecams,
                    cameras_params,
                    num_cameras,
                    requested_cameras,
                    light_ecam,
                    light_params,
                    mapped_strobe_available,
                    camera_control->record_video || camera_control->recording_draining,
                    "suppress_mapped_strobe",
                    &state->preflight_prepare_status,
                    timing);
            set_calibration_preflight_result(
                spatial_state, preflight_ok, state->preflight_prepare_status);
            if (!preflight_ok) {
                fail(state, "guided capture calibration preflight failed: " +
                                state->preflight_prepare_status);
                break;
            }
            state->preflight_applied = true;
            state->capture_camera_settings = nlohmann::json::array();
            for (const std::string& serial : requested_cameras) {
                const int index = find_camera_index(cameras_params, num_cameras, serial);
                if (index < 0) {
                    continue;
                }
                state->capture_camera_settings.push_back({
                    {"camera_serial", serial},
                    {"frame_rate_hz", cameras_params[index].frame_rate},
                    {"exposure_us", cameras_params[index].exposure},
                    {"focus", cameras_params[index].focus},
                    {"iris", cameras_params[index].iris},
                    {"sync_mode", cameras_params[index].sync_mode},
                    {"ptp_mode", cameras_params[index].ptp_mode},
                    {"ptp_gate_offset_ns", cameras_params[index].ptp_gate_offset_ns},
                });
            }
            enter_stage(state, GuidedCaptureAutorunStage::kWaitForPreflightSettle);
            break;
        }
        std::string request_error;
        if (!request_current_sweep_sample(
                state,
                config,
                spatial_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                &request_error)) {
            fail(state, "guided grouped capture request failed: " + request_error);
            break;
        }
        enter_stage(state, GuidedCaptureAutorunStage::kWaitForCapture);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForPreflightSettle: {
        const double settle_seconds =
            static_cast<double>(config.preflight_settle_milliseconds) / 1000.0;
        if (stage_elapsed_seconds(*state) < settle_seconds) {
            break;
        }
        std::string request_error;
        if (!request_current_sweep_sample(
                state,
                config,
                spatial_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                &request_error)) {
            fail(state, "guided grouped capture request failed after preflight settle: " +
                            request_error);
            break;
        }
        enter_stage(state, GuidedCaptureAutorunStage::kWaitForCapture);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForCapture:
        if (spatial_state->group_capture_workflow_state == "complete") {
            const std::string membership =
                spatial_state->group_capture_metadata.capture_group_membership.value(
                    "status", std::string());
            const std::string consistency =
                spatial_state->group_capture_metadata.citrus_calibration_scene_consistency.value(
                    "status", std::string());
            const std::string restore_state =
                spatial_state->group_capture_scene_restore_status.value(
                    "state", std::string());
            if (membership != "complete" || consistency != "same_scene" ||
                restore_state != "restored") {
                fail(state,
                     "guided capture completed without a complete, consistent, restored group"
                     " (membership=" + membership + ", consistency=" + consistency +
                     ", restore=" + restore_state + ")");
            } else if (config.save_captures) {
                state->run_passed = true;
                enter_stage(state, GuidedCaptureAutorunStage::kQueueSave);
            } else {
                append_completed_sample(state, config, *spatial_state);
                if (advance_sweep(state, config)) {
                    std::string request_error;
                    if (!request_current_sweep_sample(
                            state,
                            config,
                            spatial_state,
                            cameras_params,
                            cameras_select,
                            num_cameras,
                            spatial_snapshot_workers,
                            &request_error)) {
                        fail(state, "guided sweep request failed: " + request_error);
                    } else {
                        enter_stage(state, GuidedCaptureAutorunStage::kWaitForCapture);
                    }
                } else {
                    state->run_passed = true;
                    enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
                }
            }
        } else if (spatial_state->group_capture_workflow_state == "failed") {
            fail(state,
                 spatial_state->group_capture_error.empty()
                     ? "guided grouped capture failed"
                     : spatial_state->group_capture_error);
        } else if (stage_elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            fail(state, "timed out waiting for guided grouped capture completion");
        }
        break;

    case GuidedCaptureAutorunStage::kQueueSave: {
        std::string save_status;
        std::string save_error;
        const int selected_index = std::clamp(
            spatial_state->selected_camera, 0, std::max(0, num_cameras - 1));
        if (!queue_group_calibration_image_set_save_jobs(
                spatial_state,
                cameras_params,
                num_cameras,
                cameras_params[selected_index],
                calibration_sessions_root,
                &save_status,
                &save_error)) {
            fail(state, "grouped calibration image-set save failed: " + save_error);
            break;
        }
        spatial_state->persistence_status = save_status;
        spatial_state->persistence_error.clear();
        enter_stage(state, GuidedCaptureAutorunStage::kWaitForSave);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForSave:
        if (!spatial_state->persistence_error.empty()) {
            fail(state, "grouped calibration image-set save failed: " +
                            spatial_state->persistence_error);
        } else if (!generic_calibration_image_set_save_worker_is_busy() &&
                   queued_generic_calibration_image_set_save_job_count() == 0) {
            append_completed_sample(state, config, *spatial_state);
            if (advance_sweep(state, config)) {
                std::string request_error;
                if (!request_current_sweep_sample(
                        state,
                        config,
                        spatial_state,
                        cameras_params,
                        cameras_select,
                        num_cameras,
                        spatial_snapshot_workers,
                        &request_error)) {
                    fail(state, "guided sweep request failed after save: " + request_error);
                } else {
                    enter_stage(state, GuidedCaptureAutorunStage::kWaitForCapture);
                }
            } else {
                if (config.purpose == "projected_surface_scale_calibration") {
                    enter_stage(
                        state,
                        GuidedCaptureAutorunStage::kAnalyzeProjectedSurfaceScale);
                } else if (config.fit_homographies_after_capture) {
                    std::string request_error;
                    if (!prepare_homography_fit_request(
                            state,
                            config,
                            *spatial_state,
                            &request_error)) {
                        fail(state, "could not prepare holder homography fit: " +
                                        request_error);
                    } else {
                        enter_stage(
                            state,
                            GuidedCaptureAutorunStage::kRequestHomographyFit);
                    }
                } else {
                    state->run_passed = true;
                    enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
                }
            }
        } else if (stage_elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            fail(state, "timed out waiting for grouped calibration image-set saves");
        }
        break;

    case GuidedCaptureAutorunStage::kRequestHomographyFit: {
        const auto fit = spatial_layout::fit_citrus_homography_candidates(
            state->homography_transaction_id,
            config.citrus_config_path,
            state->homography_canvas_sha256,
            spatial_state->calibration_session_dir,
            state->homography_capture_group_id,
            state->homography_targets,
            holder_homography_quality_thresholds(config, *state),
            state->homography_transaction_id + "_fit");
        state->homography_candidate_status = fit.candidate;
        state->homography_fit_requested = fit.ok;
        if (!fit.ok) {
            fail(state, "Citrus rejected holder operational homography fit: " +
                            fit.reason);
            break;
        }
        state->homography_last_poll_at = {};
        enter_stage(state, GuidedCaptureAutorunStage::kWaitForHomographyFit);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForHomographyFit: {
        if (!should_poll_homography_status(state)) break;
        const auto status =
            spatial_layout::query_citrus_homography_candidate_status(
                state->homography_transaction_id, "holder-fit-wait");
        if (!status.ok) {
            fail(state, "could not query Citrus holder homography fit: " +
                            status.reason);
            break;
        }
        state->homography_candidate_status = status.candidate;
        const std::string candidate_state = status.candidate.value("state", "");
        if (candidate_state == "ready_for_review") {
            enter_stage(
                state, GuidedCaptureAutorunStage::kReleaseHomographyCandidate);
        } else if (candidate_state == "fit_failed") {
            state->run_passed = false;
            state->error_message =
                "holder operational homography candidate fit failed: " +
                status.candidate.value("error", "unknown error");
            enter_stage(
                state, GuidedCaptureAutorunStage::kReleaseHomographyCandidate);
        } else if (stage_elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            state->error_message =
                "timed out waiting for Citrus holder homography fit";
            enter_stage(
                state, GuidedCaptureAutorunStage::kReleaseHomographyCandidate);
        }
        break;
    }

    case GuidedCaptureAutorunStage::kReleaseHomographyCandidate: {
        const auto release = spatial_layout::reject_citrus_homography_candidates(
            state->homography_transaction_id,
            state->error_message.empty()
                ? "persisted_for_external_holder_evidence_review"
                : state->error_message,
            state->homography_transaction_id + "_release_for_review");
        if (!release.ok) {
            fail(state, "could not release persisted holder homography candidate: " +
                            release.reason);
            break;
        }
        state->homography_candidate_status = release.candidate;
        state->homography_candidate_released = true;
        state->homography_last_poll_at = {};
        enter_stage(
            state, GuidedCaptureAutorunStage::kWaitForHomographyRelease);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForHomographyRelease: {
        if (!should_poll_homography_status(state)) break;
        const auto status =
            spatial_layout::query_citrus_homography_candidate_status(
                state->homography_transaction_id, "holder-release-receipt");
        if (status.ok) {
            state->homography_candidate_status = status.candidate;
            if (!status.candidate.value("active", true) &&
                status.candidate.value("state", "") == "rejected" &&
                status.candidate.value(
                    "receipt", nlohmann::json::object()).value(
                        "outcome", "") == "rejected") {
                if (state->error_message.empty()) state->run_passed = true;
                enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
                break;
            }
        }
        if (stage_elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            fail(state, "timed out waiting for holder homography release receipt");
        }
        break;
    }

    case GuidedCaptureAutorunStage::kAnalyzeProjectedSurfaceScale: {
        const auto artifact = projected_surface_scale::analyze_and_write_group(
            *spatial_state,
            spatial_state->calibration_session_dir,
            config.citrus_config_path);
        state->projected_surface_scale_manifest_path = artifact.manifest_path.string();
        state->projected_surface_scale_canvas_sha256 = artifact.canvas_sha256;
        state->projected_surface_scale_observations = artifact.observations;
        state->projected_surface_scale_verification = artifact.verification;
        spatial_state->projected_surface_scale_review_manifest_path =
            state->projected_surface_scale_manifest_path;
        spatial_state->projected_surface_scale_review_verification =
            artifact.verification;
        spatial_state->projected_surface_scale_review_canvas_sha256 =
            artifact.canvas_sha256;
        spatial_state->projected_surface_scale_candidate_manifest_path.clear();
        spatial_state->projected_surface_scale_candidate_manifest =
            nlohmann::json::object();
        spatial_state->projected_surface_scale_review_revalidated = false;
        if (!artifact.ok) {
            fail(state, "projected-surface scale artifact analysis failed: " + artifact.error);
            break;
        }
        nlohmann::json manifest;
        std::ifstream manifest_input(artifact.manifest_path, std::ios::binary);
        manifest = nlohmann::json::parse(manifest_input, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            fail(state, "could not reload projected-surface scale observation-set manifest");
            break;
        }
        spatial_state->projected_surface_scale_review_manifest = std::move(manifest);
        if (!artifact.quality_pass) {
            fail(state, "one or more projected-surface scale quality gates failed");
            break;
        }
        state->projected_surface_scale_transaction_id =
            "projected_surface_scale_" +
            spatial_layout::sanitize_artifact_component(spatial_state->group_capture_id);
        spatial_state->projected_surface_scale_review_transaction_id =
            state->projected_surface_scale_transaction_id;
        enter_stage(
            state,
            GuidedCaptureAutorunStage::kRequestProjectedSurfaceScaleFit);
        break;
    }

    case GuidedCaptureAutorunStage::kRequestProjectedSurfaceScaleFit: {
        const auto fit = spatial_layout::fit_citrus_projected_surface_scale_candidates(
            state->projected_surface_scale_transaction_id,
            config.citrus_config_path,
            state->projected_surface_scale_canvas_sha256,
            state->projected_surface_scale_observations,
            state->projected_surface_scale_transaction_id + "_fit");
        if (!fit.ok) {
            fail(state, "Citrus rejected projected-surface scale fit request: " + fit.reason);
            break;
        }
        state->projected_surface_scale_candidate_status = fit.candidate;
        spatial_state->projected_surface_scale_review_status = fit.candidate;
        state->projected_surface_scale_last_poll_at = {};
        enter_stage(
            state,
            GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleFit);
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleFit: {
        if (!should_poll_scale_status(state)) break;
        const auto status =
            spatial_layout::query_citrus_projected_surface_scale_candidate_status(
                state->projected_surface_scale_transaction_id, "fit-wait");
        if (!status.ok) {
            fail(state, "could not query Citrus projected-surface scale fit: " +
                            status.reason);
            break;
        }
        state->projected_surface_scale_candidate_status = status.candidate;
        spatial_state->projected_surface_scale_review_status = status.candidate;
        const std::string candidate_state = status.candidate.value("state", "");
        if (candidate_state == "failed") {
            fail(state, "Citrus projected-surface scale refit failed: " +
                            status.candidate.value("error", "unknown error"));
        } else if (candidate_state == "ready_for_review") {
            spatial_state->projected_surface_scale_review_message =
                "Orange QC and Citrus refit passed. Review all four overlays and "
                "independent checks before arming scale promotion.";
            spatial_state->projected_surface_scale_review_error.clear();
            if (config.accept_projected_surface_scales_armed) {
                const auto promotion =
                    spatial_layout::promote_citrus_projected_surface_scale_candidates(
                        state->projected_surface_scale_transaction_id,
                        state->projected_surface_scale_canvas_sha256,
                        state->projected_surface_scale_verification,
                        true,
                        state->projected_surface_scale_transaction_id + "_promote");
                if (!promotion.ok) {
                    fail(state, "Citrus rejected projected-surface scale promotion: " +
                                    promotion.reason);
                    break;
                }
                enter_stage(
                    state,
                    GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScalePromotion);
            } else {
                enter_stage(
                    state,
                    GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleReview);
            }
        }
        break;
    }

    case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleReview:
    case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScalePromotion: {
        if (!should_poll_scale_status(state)) break;
        const auto status =
            spatial_layout::query_citrus_projected_surface_scale_candidate_status(
                state->projected_surface_scale_transaction_id, "operator-review-wait");
        if (!status.ok) {
            spatial_state->projected_surface_scale_review_error = status.reason;
            break;
        }
        state->projected_surface_scale_candidate_status = status.candidate;
        spatial_state->projected_surface_scale_review_status = status.candidate;
        const std::string candidate_state = status.candidate.value("state", "");
        if (candidate_state == "committed") {
            state->run_passed = true;
            spatial_state->projected_surface_scale_review_message =
                "Citrus committed the reviewed scales as the active canvas authority.";
            enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
        } else if (candidate_state == "rejected") {
            state->run_passed = true;
            spatial_state->projected_surface_scale_review_message =
                "Scale candidate released without changing active runtime scale.";
            enter_stage(state, GuidedCaptureAutorunStage::kStopStream);
        } else if (candidate_state == "failed") {
            fail(state, "Citrus projected-surface scale transaction failed: " +
                            status.candidate.value("error", "unknown error"));
        }
        break;
    }

    case GuidedCaptureAutorunStage::kStopStream:
        if (state->preflight_applied && !state->preflight_restore_attempted) {
            state->preflight_restore_attempted = true;
            int light_camera_index = -1;
            for (int index = 0; index < num_cameras; ++index) {
                if (camera_has_exposed_mapped_nir_strobe(cameras_params[index])) {
                    light_camera_index = index;
                    break;
                }
            }
            const bool mapped_strobe_available = light_camera_index >= 0;
            state->preflight_restore_ok =
                restore_calibration_capture_preflight_all_cameras(
                    spatial_state,
                    ecams,
                    cameras_params,
                    num_cameras,
                    mapped_strobe_available ? &ecams[light_camera_index] : nullptr,
                    mapped_strobe_available ? &cameras_params[light_camera_index] : nullptr,
                    mapped_strobe_available,
                    camera_control->record_video || camera_control->recording_draining,
                    &state->preflight_restore_status);
            set_calibration_preflight_result(
                spatial_state,
                state->preflight_restore_ok,
                state->preflight_restore_status);
            if (!state->preflight_restore_ok) {
                state->run_passed = false;
                if (state->error_message.empty()) {
                    state->error_message =
                        "guided capture calibration restore failed: " +
                        state->preflight_restore_status;
                }
            }
        }
        if (camera_control->subscribe &&
            orange::calibration::global_transaction_coordinator().active() &&
            !spatial_layout::spatial_calibration_transaction_owned_by(
                *spatial_state,
                spatial_layout::kGuidedCommissioningTransactionOwner)) {
            if (state->error_message.empty()) {
                state->error_message =
                    orange::calibration::global_transaction_coordinator()
                        .rejection_message("Guided commissioning stream stop");
            }
            state->run_passed = false;
            enter_stage(state, GuidedCaptureAutorunStage::kWriteResult);
        } else if (camera_control->subscribe) {
            if (!state->action_requested) {
                requests.toggle_streaming = true;
                state->action_requested = true;
                std::cout << "[GUI][guided_capture_autorun] requesting stream stop"
                          << std::endl;
            }
            enter_stage(state, GuidedCaptureAutorunStage::kWaitForStreamStop);
        } else {
            enter_stage(state, GuidedCaptureAutorunStage::kWriteResult);
        }
        break;

    case GuidedCaptureAutorunStage::kWaitForStreamStop:
        if (!camera_control->subscribe) {
            enter_stage(state, GuidedCaptureAutorunStage::kWriteResult);
        } else if (stage_elapsed_seconds(*state) >= 60.0) {
            state->run_passed = false;
            if (state->error_message.empty()) {
                state->error_message = "timed out stopping Orange stream";
            }
            enter_stage(state, GuidedCaptureAutorunStage::kWriteResult);
        }
        break;

    case GuidedCaptureAutorunStage::kWriteResult:
        finish_result(state, config, *spatial_state, !camera_control->subscribe);
        if (spatial_layout::spatial_calibration_transaction_owned_by(
                *spatial_state,
                spatial_layout::kGuidedCommissioningTransactionOwner)) {
            spatial_layout::release_spatial_calibration_transaction(
                spatial_state,
                state->run_passed ? "complete" : "failed",
                state->error_message.empty()
                    ? "Guided commissioning finished."
                    : state->error_message);
        }
        if (state->result_written) {
            enter_stage(
                state,
                state->run_passed
                    ? GuidedCaptureAutorunStage::kDone
                    : GuidedCaptureAutorunStage::kFailed);
        } else {
            enter_stage(state, GuidedCaptureAutorunStage::kFailed);
        }
        if (config.exit_after_completion) {
            requests.close_window = true;
        }
        break;

    case GuidedCaptureAutorunStage::kDisabled:
    case GuidedCaptureAutorunStage::kDone:
    case GuidedCaptureAutorunStage::kFailed:
        break;
    }
    return requests;
}

nlohmann::json guided_capture_autorun_result_json(
    const GuidedCaptureAutorunState& state,
    const GuidedCaptureAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    bool stream_stopped)
{
    const double elapsed_seconds =
        state.run_started_at.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - state.run_started_at).count();
    return {
        {"schema_id", "orange.gui_guided_capture_smoke_result"},
        {"schema_version", 4},
        {"created_utc", get_current_utc_timestamp()},
        {"calibration_transaction_id", state.calibration_transaction_id},
        {"status", state.run_passed ? "pass" : "fail"},
        {"stage", guided_capture_autorun_stage_name(state.stage)},
        {"elapsed_seconds", elapsed_seconds},
        {"error", state.error_message},
        {"config",
         {
             {"citrus_config_path", config.citrus_config_path},
             {"workflow_profile_id", config.workflow_profile_id},
             {"recipe", config.recipe},
             {"purpose", config.purpose},
             {"camera_serials", config.camera_serials},
             {"frame_count", config.frame_count},
             {"save_captures", config.save_captures},
             {"apply_calibration_preflight", config.apply_calibration_preflight},
             {"calibration_frame_rate_hz", config.calibration_frame_rate_hz},
             {"calibration_exposure_us", config.calibration_exposure_us},
             {"foreground_gray_u8", config.foreground_gray_u8},
             {"recipe_sequence", config.recipe_sequence},
             {"fixture_aperture_shape", config.fixture_aperture_shape},
             {"sweep_foreground_grays_u8", effective_sweep_levels(config)},
             {"sweep_repeats", config.sweep_repeats},
             {"include_arena_outline_reference",
              config.include_arena_outline_reference},
             {"projected_surface_targets_ready_confirmed",
              config.projected_surface_targets_ready_confirmed},
             {"accept_projected_surface_scales_armed",
              config.accept_projected_surface_scales_armed},
             {"fit_homographies_after_capture",
              config.fit_homographies_after_capture},
             {"preflight_settle_milliseconds", config.preflight_settle_milliseconds},
         }},
        {"calibration_preflight",
         {
             {"applied", state.preflight_applied},
             {"prepare_status", state.preflight_prepare_status},
             {"capture_camera_settings", state.capture_camera_settings},
             {"restore_attempted", state.preflight_restore_attempted},
             {"restore_ok", state.preflight_restore_ok},
             {"restore_status", state.preflight_restore_status},
         }},
        {"workflow", workflow_json(spatial_state)},
        {"captures", captures_json(spatial_state)},
        {"ptp_capture_alignment", ptp_alignment_json(spatial_state)},
        {"persistence", persistence_json(config, spatial_state)},
        {"samples", state.completed_samples},
        {"projected_surface_scale", {
            {"transaction_id", state.projected_surface_scale_transaction_id},
            {"canvas_sha256", state.projected_surface_scale_canvas_sha256},
            {"manifest_path", state.projected_surface_scale_manifest_path},
            {"observations", state.projected_surface_scale_observations},
            {"verification", state.projected_surface_scale_verification},
            {"citrus_candidate_status", state.projected_surface_scale_candidate_status},
        }},
        {"homography", {
            {"transaction_id", state.homography_transaction_id},
            {"capture_group_id", state.homography_capture_group_id},
            {"canvas_sha256", state.homography_canvas_sha256},
            {"targets", state.homography_targets},
            {"fit_requested", state.homography_fit_requested},
            {"candidate_released_for_external_review",
             state.homography_candidate_released},
            {"citrus_candidate_status", state.homography_candidate_status},
            {"promotion_requested", false},
            {"runtime_authority_changed", false},
        }},
        {"sample_count", state.completed_samples.size()},
        {"session_policy", config.recipe_sequence.empty()
                               ? "one_session_per_sweep"
                               : "one_session_per_recipe_sequence"},
        {"shutdown", {{"stream_stopped", stream_stopped}}},
    };
}

}  // namespace orange::gui
