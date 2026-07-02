#include "gui/spatial_layout/layout_artifact_persistence.h"

#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <filesystem>
#include <utility>

namespace orange::gui::spatial_layout {
namespace {

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

nlohmann::json make_arena_layout_manifest_json(
    const orange::spatial::ArenaLayoutArtifact& artifact,
    const CameraParams& camera_params,
    const SpatialLayoutPersistedFiles& files)
{
    nlohmann::json manifest = {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", artifact.artifact_id},
        {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
        {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion},
        {"created_utc", artifact.created_utc},
        {"producer", {
            {"name", "orange"},
            {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
            {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion}
        }},
        {"calibration_ref", orange::spatial::calibration_ref_to_json(artifact.calibration_ref)},
        {"compatibility", {
            {"camera_serial", camera_params.camera_serial},
            {"focus", camera_params.focus},
            {"iris", camera_params.iris},
            {"exposure", camera_params.exposure},
            {"gain", camera_params.gain},
            {"pixel_format", camera_params.pixel_format},
            {"width", camera_params.width},
            {"height", camera_params.height}
        }},
        {"summary", {
            {"layout_id", artifact.layout_id},
            {"camera_serial", camera_params.camera_serial},
            {"zone_count", static_cast<int>(artifact.layout.zones.size())},
            {"coordinate_space", orange::spatial::coordinate_space_to_string(artifact.layout.coordinate_space)},
            {"outer_geometry_type", orange::spatial::layout_geometry_type_to_string(artifact.layout.outer_geometry.type)}
        }},
        {"files", {
            {"manifest", files.manifest_path.filename().string()},
            {"measurement_json", files.measurement_path.filename().string()},
            {"arena_layout_runtime_json", files.arena_layout_runtime_path.filename().string()},
            {"dish_mask_runtime_json", files.dish_mask_runtime_path.filename().string()}
        }}
    };

    if (!artifact.context.canvas_id.empty()) {
        manifest["summary"]["canvas_id"] = artifact.context.canvas_id;
    }
    if (!artifact.context.arena_id.empty()) {
        manifest["summary"]["arena_id"] = artifact.context.arena_id;
    }
    if (!artifact.context.dish_design_id.empty()) {
        manifest["summary"]["dish_design_id"] = artifact.context.dish_design_id;
    }
    return manifest;
}

}  // namespace

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

bool persist_spatial_layout_artifact_bundle(
    orange::spatial::ArenaLayoutArtifact artifact,
    const orange::spatial::DishMaskRuntime& dish_mask_runtime,
    const orange::spatial::ArenaLayoutRuntime& arena_layout_runtime,
    const CameraParams& camera_params,
    const std::string& artifact_root_dir,
    const std::string& session_dir,
    orange::spatial::ArenaLayoutArtifact* saved_artifact_out,
    std::string* saved_artifact_dir_out,
    std::string* error_out)
{
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
    if (!ensure_directory_exists(artifact_root_dir, error_out)) {
        return false;
    }

    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id =
        orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version =
        orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "pending";

    std::string validation_error;
    if (!orange::spatial::validate_arena_layout_artifact(artifact, &validation_error)) {
        if (error_out) {
            *error_out = "Arena layout artifact is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_dish_mask_runtime(dish_mask_runtime, &validation_error)) {
        if (error_out) {
            *error_out = "Dish-mask runtime is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_arena_layout_runtime_against_artifact(
            arena_layout_runtime,
            artifact,
            &validation_error)) {
        if (error_out) {
            *error_out = "Arena-layout runtime is invalid: " + validation_error;
        }
        return false;
    }

    nlohmann::json measurement_json =
        orange::spatial::arena_layout_artifact_to_json(artifact);
    artifact.calibration_ref.fingerprint = compute_json_fingerprint(measurement_json);
    measurement_json = orange::spatial::arena_layout_artifact_to_json(artifact);

    const SpatialLayoutPersistedFiles files =
        make_spatial_layout_persisted_files(artifact_root_dir, artifact.artifact_id);
    if (!ensure_directory_exists(files.artifact_dir.string(), error_out)) {
        return false;
    }

    const nlohmann::json manifest_json =
        make_arena_layout_manifest_json(artifact, camera_params, files);
    const nlohmann::json arena_layout_runtime_json =
        orange::spatial::arena_layout_runtime_to_json(arena_layout_runtime);
    const nlohmann::json dish_mask_runtime_json =
        orange::spatial::dish_mask_runtime_to_json(dish_mask_runtime);

    if (!write_json_file(files.measurement_path, measurement_json, error_out) ||
        !write_json_file(files.arena_layout_runtime_path, arena_layout_runtime_json, error_out) ||
        !write_json_file(files.dish_mask_runtime_path, dish_mask_runtime_json, error_out) ||
        !write_json_file(files.manifest_path, manifest_json, error_out)) {
        return false;
    }
    if (!update_spatial_calibration_session_index(
            session_dir,
            artifact_root_dir,
            manifest_json,
            error_out)) {
        return false;
    }
    if (!update_calibration_artifact_registry(artifact_root_dir, manifest_json, error_out)) {
        return false;
    }

    if (saved_artifact_out != nullptr) {
        *saved_artifact_out = std::move(artifact);
    }
    if (saved_artifact_dir_out != nullptr) {
        *saved_artifact_dir_out = files.artifact_dir.string();
    }
    return true;
}

bool save_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    const std::string& session_dir,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    orange::spatial::ArenaLayoutArtifact artifact = ui_state->layout_artifact;
    if (artifact.artifact_id.empty() || artifact.artifact_id.rfind("preview.", 0) == 0) {
        artifact.artifact_id = build_arena_layout_artifact_id(
            artifact.layout_id.empty() ? "layout" : artifact.layout_id,
            selected_camera,
            get_current_utc_timestamp());
    }
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "pending";

    std::string saved_artifact_dir;
    orange::spatial::ArenaLayoutArtifact saved_artifact;
    if (!persist_spatial_layout_artifact_bundle(
            artifact,
            ui_state->dish_mask_runtime,
            ui_state->arena_layout_runtime,
            selected_camera,
            artifact_root_dir,
            session_dir,
            &saved_artifact,
            &saved_artifact_dir,
            error_out)) {
        return false;
    }

    ui_state->layout_artifact = std::move(saved_artifact);
    if (status_out) {
        *status_out = "Saved arena layout artifact to " + saved_artifact_dir;
        if (!session_dir.empty()) {
            *status_out += " in calibration session " +
                           std::filesystem::path(session_dir).filename().generic_string();
        }
    }
    return true;
}

}  // namespace orange::gui::spatial_layout
