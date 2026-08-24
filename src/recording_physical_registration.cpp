#include "recording_physical_registration.h"

#include "dish_top_rim_observation.h"
#include "gui/spatial_layout/physical_registration_selection.h"
#include "gui/spatial_layout/sha256.h"

#include <cstdlib>
#include <cctype>
#include <fstream>

namespace orange::recording_geometry {
namespace {

namespace fs = std::filesystem;

bool read_json(const fs::path& path, nlohmann::json* value)
{
    if (value == nullptr) return false;
    std::ifstream stream(path);
    if (!stream) return false;
    try {
        stream >> *value;
    } catch (...) {
        return false;
    }
    return value->is_object();
}

std::string safe_component(const std::string& value)
{
    std::string safe;
    safe.reserve(value.size());
    for (const unsigned char character : value) {
        safe.push_back(
            std::isalnum(character) || character == '-' || character == '_'
                ? static_cast<char>(character)
                : '_');
    }
    return safe.empty() ? "unknown" : safe;
}

nlohmann::json source_snapshot(const fs::path& path)
{
    std::string sha256;
    std::string error;
    nlohmann::json snapshot;
    if (path.empty() || !fs::is_regular_file(path) ||
        !orange::gui::spatial_layout::checksum::file_sha256(
            path, &sha256, &error) ||
        !read_json(path, &snapshot)) {
        return nlohmann::json::object();
    }
    return {
        {"source_path", fs::absolute(path).lexically_normal().string()},
        {"sha256", sha256},
        {"snapshot", std::move(snapshot)},
    };
}

nlohmann::json build_recording_snapshot_entry(
    const orange::gui::spatial_layout::PhysicalRegistrationArtifactCandidate& candidate)
{
    const nlohmann::json& observation = candidate.observation;
    const std::string camera_serial = candidate.camera_serial;
    const nlohmann::json arena_context = observation.contains("arena_context") &&
        observation["arena_context"].is_object()
        ? observation["arena_context"]
        : nlohmann::json::object();
    const fs::path destination = fs::path("recording_geometry_assets") /
        "cameras" / ("Cam" + safe_component(camera_serial)) /
        "physical_registration";
    return {
        {"artifact_id", candidate.artifact_id},
        {"artifact_schema_id", observation.value("schema_id", "")},
        {"artifact_schema_version", observation.value("schema_version", 0)},
        {"camera_serial", camera_serial},
        {"arena_id", arena_context.value("arena_id", "")},
        {"calibration_ref", observation.value(
            "calibration_ref", nlohmann::json::object())},
        {"camera", observation.value("camera", nlohmann::json::object())},
        {"physical_target", "dish_top_rim"},
        {"coordinate_space", "camera_native_pixels"},
        {"accepted_inner_rim_boundary", observation.value(
            "accepted_inner_rim_boundary", nlohmann::json::object())},
        {"accepted_experimental_area_boundary", observation.value(
            "accepted_experimental_area_boundary", nlohmann::json::object())},
        {"accepted_mask", observation.value(
            "accepted_mask", nlohmann::json::object())},
        {"valid_detection_region", observation.value(
            "valid_detection_region", nlohmann::json::object())},
        {"boundary_interpretation", observation.value(
            "boundary_interpretation", nlohmann::json::object())},
        {"operator_review", observation.value(
            "operator_review", nlohmann::json::object())},
        {"review_artifacts", observation.value(
            "review_artifacts", nlohmann::json::object())},
        {"runtime_verification", observation.value(
            "runtime_verification", nlohmann::json::object())},
        {"source", {
            {"path", fs::absolute(candidate.observation_path).lexically_normal().string()},
            {"sha256", candidate.observation_sha256},
            {"intended_recording_relative_path",
             (destination / "observation.json").generic_string()},
        }},
        {"selection_authority", "orange_active_physical_registration_pointer"},
        {"available_for_downstream_detection_gating", true},
        {"active_in_orange_live_detection_pipeline", false},
        {"active_in_orange_neural_input_mask", false},
        {"gating_semantics",
         "bounding_box_centroid_inside_valid_detection_region"},
    };
}

nlohmann::json compact_sources(
    const orange::gui::spatial_layout::PhysicalRegistrationArtifactCandidate& candidate)
{
    nlohmann::json result = nlohmann::json::object();
    nlohmann::json manifest;
    if (!read_json(candidate.manifest_path, &manifest)) return result;
    const nlohmann::json files = manifest.value("files", nlohmann::json::object());
    if (!files.is_object()) return result;
    struct Source {
        const char* manifest_key;
        const char* output_key;
    };
    static constexpr Source sources[] = {
        {"image_set_json", "image_set"},
        {"spatial_dish_mask_runtime_v1", "spatial_dish_mask_runtime_v1"},
        {"palette_dish_mask_v2", "palette_dish_mask_v2"},
    };
    const fs::path artifact_dir = candidate.observation_path.parent_path();
    for (const Source& source : sources) {
        const std::string relative = files.value(source.manifest_key, "");
        if (relative.empty()) continue;
        const nlohmann::json snapshot = source_snapshot(artifact_dir / relative);
        if (!snapshot.empty()) result[source.output_key] = snapshot;
    }
    result["manifest"] = {
        {"source_path", fs::absolute(candidate.manifest_path).lexically_normal().string()},
        {"sha256", candidate.manifest_sha256},
        {"snapshot", std::move(manifest)},
    };
    return result;
}

nlohmann::json optional_evidence(
    const orange::gui::spatial_layout::PhysicalRegistrationArtifactCandidate& candidate)
{
    nlohmann::json result = nlohmann::json::object();
    nlohmann::json manifest;
    if (!read_json(candidate.manifest_path, &manifest)) return result;
    const nlohmann::json files = manifest.value("files", nlohmann::json::object());
    const nlohmann::json checksums = manifest.value(
        "checksums", nlohmann::json::object());
    if (!files.is_object()) return result;
    struct Evidence {
        const char* key;
        const char* output_key;
    };
    static constexpr Evidence evidence[] = {
        {"source_frame", "source_frame"},
        {"review_overlay", "review_overlay"},
        {"registration_hough_overlay", "registration_hough_overlay"},
        {"valid_detection_overlay", "valid_detection_overlay"},
    };
    const fs::path artifact_dir = candidate.observation_path.parent_path();
    for (const Evidence& item : evidence) {
        const std::string relative = files.value(item.key, "");
        if (relative.empty()) continue;
        const fs::path path = artifact_dir / relative;
        std::string sha256;
        std::string error;
        if (!fs::is_regular_file(path) ||
            !orange::gui::spatial_layout::checksum::file_sha256(
                path, &sha256, &error)) {
            continue;
        }
        result[item.output_key] = {
            {"source_path", fs::absolute(path).lexically_normal().string()},
            {"sha256", sha256},
            {"declared_checksum", checksums.value(item.key, "")},
        };
    }
    return result;
}

}  // namespace

fs::path resolve_recording_calibration_base_dir()
{
    if (const char* configured = std::getenv("ORANGE_CALIBRATION_BASE_DIR");
        configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    if (const char* home = std::getenv("HOME");
        home != nullptr && home[0] != '\0') {
        return fs::path(home) / "orange_data" / "calibrations";
    }
    return fs::path("orange_data") / "calibrations";
}

void append_recording_physical_registrations(
    nlohmann::json* contract,
    const fs::path& calibration_base_dir,
    const std::vector<RecordingPhysicalCamera>& cameras,
    const std::string& captured_at_utc)
{
    if (contract == nullptr || !contract->is_object()) return;
    nlohmann::json envelope = {
        {"schema_id", "orange.recording.physical_registration_geometry"},
        {"schema_version", 1},
        {"captured_at_utc", captured_at_utc},
        {"status", "not_performed"},
        {"selection_authority", "orange_active_physical_registration_pointer"},
        {"selection_policy", "exact_per_camera_pointer_revalidated_at_recording_prearm"},
        {"recording_blocked", false},
        {"participating_cameras_only", true},
        {"cameras", nlohmann::json::object()},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array()},
    };
    std::size_t selected = 0;
    std::size_t invalid = 0;
    std::size_t available_not_selected = 0;
    for (const RecordingPhysicalCamera& camera : cameras) {
        if (camera.camera_serial.empty()) continue;
        const auto resolution = orange::gui::spatial_layout::
            resolve_active_physical_registration(
                calibration_base_dir, camera.camera_serial,
                camera.width_px, camera.height_px, camera.pixel_format);
        nlohmann::json camera_entry = {
            {"camera_serial", camera.camera_serial},
            {"status", resolution.pointer_exists
                ? (resolution.status == "not_selected"
                       ? "available_not_selected"
                       : resolution.status)
                : "not_performed"},
            {"mode", resolution.selected
                ? "selected_physical_registration"
                : "not_selected"},
            {"recording_blocked", false},
        };
        if (resolution.pointer_exists) {
            camera_entry["active_pointer"] = {
                {"source_path", fs::absolute(
                    resolution.pointer_path).lexically_normal().string()},
                {"sha256", resolution.pointer_sha256},
                {"snapshot", resolution.pointer},
            };
        }
        if (resolution.pointer_exists && !resolution.selected &&
            resolution.status == "not_selected") {
            ++available_not_selected;
        }
        if (resolution.selected && resolution.valid) {
            ++selected;
            camera_entry["status"] = "selected_resolved";
            camera_entry["artifact_id"] = resolution.candidate.artifact_id;
            camera_entry["observation"] = {
                {"source_path", fs::absolute(
                    resolution.candidate.observation_path).lexically_normal().string()},
                {"sha256", resolution.candidate.observation_sha256},
                {"snapshot", resolution.candidate.observation},
            };
            camera_entry["manifest"] = {
                {"source_path", fs::absolute(
                    resolution.candidate.manifest_path).lexically_normal().string()},
                {"sha256", resolution.candidate.manifest_sha256},
            };
            camera_entry["compact_artifacts"] = compact_sources(
                resolution.candidate);
            camera_entry["optional_evidence"] = optional_evidence(
                resolution.candidate);
            camera_entry["recording_snapshot_entry"] =
                build_recording_snapshot_entry(resolution.candidate);
        } else if (resolution.selected || resolution.status == "invalid_pointer") {
            ++invalid;
            camera_entry["status"] = resolution.status == "invalid_pointer"
                ? "invalid_pointer"
                : "invalid_selected";
            camera_entry["error"] = resolution.error;
            envelope["errors"].push_back(
                "Camera " + camera.camera_serial +
                " has an invalid physical-registration selection: " +
                resolution.error);
        }
        envelope["cameras"][camera.camera_serial] = camera_entry;
        (*contract)["cameras"][camera.camera_serial]["physical_registration"] =
            std::move(camera_entry);
        if (!(*contract)["cameras"][camera.camera_serial].contains(
                "projection_registration")) {
            const nlohmann::json projection =
                (*contract)["cameras"][camera.camera_serial].value(
                    "projection_geometry", nlohmann::json::object());
            const nlohmann::json daily =
                (*contract)["cameras"][camera.camera_serial].value(
                    "daily_registration_geometry", nlohmann::json::object());
            if (projection.is_object() &&
                projection.value("status", "") == "resolved") {
                (*contract)["cameras"][camera.camera_serial]
                    ["projection_registration"] = {
                        {"status", "resolved"},
                        {"authority", "citrus"},
                        {"mode", daily.value(
                            "mode", "commissioned_base_geometry")},
                        {"source", "recording_bound_citrus_geometry"},
                    };
            } else {
                (*contract)["cameras"][camera.camera_serial]
                    ["projection_registration"] = {
                        {"status", "not_applicable"},
                        {"reason",
                         "no_projection_registration_selected_for_this_edge"},
                    };
            }
        }
    }
    if (invalid > 0) {
        envelope["status"] = selected > 0
            ? "selected_partial"
            : "invalid_selected";
    } else if (selected == cameras.size() && selected > 0) {
        envelope["status"] = "selected_resolved";
    } else if (selected > 0) {
        envelope["status"] = "selected_partial";
    } else if (available_not_selected > 0) {
        envelope["status"] = "available_not_selected";
    }
    envelope["selected_camera_count"] = selected;
    envelope["invalid_selected_camera_count"] = invalid;
    envelope["available_not_selected_camera_count"] =
        available_not_selected;
    if (selected > 0 && contract->value("status", "not_configured") ==
            "not_configured") {
        (*contract)["status"] = "orange_only";
    }
    (*contract)["physical_registration_geometry"] = std::move(envelope);
}

bool mark_recording_dish_mask_runtime_use(
    nlohmann::json* contract,
    const std::string& camera_serial,
    const std::string& mode,
    const bool centroid_gate_active,
    const bool neural_input_mask_active)
{
    if (contract == nullptr || !contract->is_object()) return false;
    nlohmann::json& camera = (*contract)["cameras"][camera_serial];
    nlohmann::json* entry = nullptr;
    auto physical = camera.find("physical_registration");
    if (physical != camera.end() && physical->is_object() &&
        physical->value("status", "") == "selected_resolved") {
        auto candidate = physical->find("recording_snapshot_entry");
        if (candidate != physical->end() && candidate->is_object()) {
            entry = &(*candidate);
        }
    }
    if (entry == nullptr) {
        auto daily = camera.find("daily_registration_geometry");
        if (daily != camera.end() && daily->is_object()) {
            auto candidate = daily->find("recording_snapshot_entry");
            if (candidate != daily->end() && candidate->is_object()) {
                entry = &(*candidate);
            }
        }
    }
    if (entry == nullptr) return false;
    (*entry)["active_in_orange_live_detection_pipeline"] =
        centroid_gate_active;
    (*entry)["orange_live_detection_pipeline_mode"] = mode;
    (*entry)["active_in_orange_neural_input_mask"] =
        neural_input_mask_active;
    nlohmann::json& envelope_camera = (*contract)
        ["physical_registration_geometry"]["cameras"][camera_serial];
    if (envelope_camera.is_object() &&
        envelope_camera.value("status", "") == "selected_resolved") {
        nlohmann::json& envelope_entry =
            envelope_camera["recording_snapshot_entry"];
        envelope_entry["active_in_orange_live_detection_pipeline"] =
            centroid_gate_active;
        envelope_entry["orange_live_detection_pipeline_mode"] = mode;
        envelope_entry["active_in_orange_neural_input_mask"] =
            neural_input_mask_active;
    }
    return true;
}

}  // namespace orange::recording_geometry
