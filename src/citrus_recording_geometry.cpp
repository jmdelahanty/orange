#include "citrus_recording_geometry.h"

#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace orange::recording_geometry {
namespace {

namespace checksum = orange::gui::spatial_layout::checksum;

struct JsonFile {
    std::filesystem::path path;
    std::string bytes;
    std::string sha256;
    nlohmann::json value = nlohmann::json::object();
};

struct ArenaBinding {
    std::string arena_id;
    nlohmann::json arena = nlohmann::json::object();
    nlohmann::json camera = nlohmann::json::object();
};

std::filesystem::path normalized_path(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return normalized;
    }
    normalized = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : normalized.lexically_normal();
}

bool path_is_inside(const std::filesystem::path& child,
                    const std::filesystem::path& parent)
{
    const std::filesystem::path normalized_child = normalized_path(child);
    const std::filesystem::path normalized_parent = normalized_path(parent);
    auto child_it = normalized_child.begin();
    for (auto parent_it = normalized_parent.begin();
         parent_it != normalized_parent.end(); ++parent_it, ++child_it) {
        if (child_it == normalized_child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return true;
}

void append_message(nlohmann::json* messages, const std::string& message)
{
    if (messages == nullptr || message.empty()) {
        return;
    }
    if (!messages->is_array()) {
        *messages = nlohmann::json::array();
    }
    messages->push_back(message);
}

bool read_json_file(const std::filesystem::path& path,
                    JsonFile* out,
                    std::string* error_out)
{
    if (out == nullptr) {
        if (error_out) *error_out = "null JSON file destination";
        return false;
    }
    std::string bytes;
    std::string error;
    if (!checksum::read_file(path, &bytes, &error)) {
        if (error_out) *error_out = error;
        return false;
    }
    nlohmann::json value = nlohmann::json::parse(bytes, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        if (error_out) *error_out = "invalid JSON object: " + path.string();
        return false;
    }
    out->path = normalized_path(path);
    out->bytes = std::move(bytes);
    out->sha256 = "sha256:" + checksum::sha256_hex(out->bytes);
    out->value = std::move(value);
    return true;
}

nlohmann::json json_file_snapshot(const JsonFile& file)
{
    return {
        {"source_path", file.path.string()},
        {"sha256", file.sha256},
        {"snapshot", file.value},
    };
}

std::map<std::string, ArenaBinding> camera_bindings(
    const nlohmann::json& canvas,
    nlohmann::json* errors,
    const std::string& label)
{
    std::map<std::string, ArenaBinding> bindings;
    const auto arenas_it = canvas.find("arenas");
    if (arenas_it == canvas.end() || !arenas_it->is_object()) {
        append_message(errors, label + " has no arenas object");
        return bindings;
    }
    for (auto arena_it = arenas_it->begin(); arena_it != arenas_it->end(); ++arena_it) {
        if (!arena_it.value().is_object()) {
            continue;
        }
        const auto cameras_it = arena_it.value().find("camera_calibrations");
        if (cameras_it == arena_it.value().end() || !cameras_it->is_array()) {
            continue;
        }
        for (const auto& camera : *cameras_it) {
            if (!camera.is_object()) {
                continue;
            }
            const std::string serial = camera.value("camera_id", "");
            if (serial.empty()) {
                continue;
            }
            if (bindings.count(serial) != 0) {
                append_message(
                    errors,
                    label + " maps camera " + serial + " to more than one arena");
                continue;
            }
            bindings.emplace(
                serial,
                ArenaBinding{arena_it.key(), arena_it.value(), camera});
        }
    }
    return bindings;
}

bool same_positive_integer_field(const nlohmann::json& lhs,
                                 const nlohmann::json& rhs,
                                 const char* field)
{
    return lhs.value(field, 0) > 0 &&
        lhs.value(field, 0) == rhs.value(field, 0);
}

const nlohmann::json* find_release_member(const nlohmann::json& release,
                                          const std::string& arena_id,
                                          const std::string& camera_id)
{
    const auto members_it = release.find("members");
    if (members_it == release.end() || !members_it->is_array()) {
        return nullptr;
    }
    for (const auto& member : *members_it) {
        if (member.is_object() &&
            member.value("arena_id", "") == arena_id &&
            member.value("camera_id", "") == camera_id) {
            return &member;
        }
    }
    return nullptr;
}

bool read_checksums_member_file(const nlohmann::json& member_product,
                                const char* path_field,
                                const char* checksum_field,
                                const std::filesystem::path& artifact_root,
                                JsonFile* file_out,
                                std::string* error_out)
{
    const std::filesystem::path path = member_product.value(path_field, "");
    const std::string expected_checksum = member_product.value(checksum_field, "");
    if (path.empty() || expected_checksum.empty()) {
        if (error_out) {
            *error_out = std::string("commissioning member is missing ") +
                path_field + " or " + checksum_field;
        }
        return false;
    }
    if (!path_is_inside(path, artifact_root)) {
        if (error_out) {
            *error_out = "commissioning member path is outside authority artifact root: " +
                path.string();
        }
        return false;
    }
    std::string error;
    if (!read_json_file(path, file_out, &error)) {
        if (error_out) *error_out = error;
        return false;
    }
    if (file_out->sha256 != expected_checksum) {
        if (error_out) {
            *error_out = "commissioning member checksum mismatch: " + path.string();
        }
        return false;
    }
    return true;
}

nlohmann::json selected_experimental_area(const nlohmann::json& arena)
{
    nlohmann::json area = nlohmann::json::object();
    bool has_physical_dimensions = false;
    static constexpr const char* fields[] = {
        "experimental_area_shape",
        "experimental_area_center_x_px",
        "experimental_area_center_y_px",
        "experimental_area_radius_px",
        "experimental_area_radius_mm",
        "experimental_area_width_px",
        "experimental_area_height_px",
        "experimental_area_width_mm",
        "experimental_area_height_mm",
        "experimental_area_corner_radius_px",
        "experimental_area_corner_radius_mm",
    };
    for (const char* field : fields) {
        const auto it = arena.find(field);
        if (it != arena.end()) {
            area[field] = *it;
            const std::string field_name(field);
            if (field_name.size() >= 3 &&
                field_name.compare(field_name.size() - 3, 3, "_mm") == 0) {
                has_physical_dimensions = true;
            }
        }
    }
    area["owner"] = "selected_canvas";
    area["physical_dimensions_are_authoritative"] = has_physical_dimensions;
    return area;
}

nlohmann::json arena_placement(const nlohmann::json& camera)
{
    nlohmann::json placement = nlohmann::json::object();
    static constexpr const char* fields[] = {
        "arena_center_x_px",
        "arena_center_y_px",
        "arena_width_px",
        "arena_height_px",
        "native_width_px",
        "native_height_px",
    };
    for (const char* field : fields) {
        const auto it = camera.find(field);
        if (it != camera.end()) {
            placement[field] = *it;
        }
    }
    placement["coordinate_space"] = "final_display_canvas_px";
    placement["origin"] = "top_left";
    placement["positive_x"] = "right";
    placement["positive_y"] = "down";
    return placement;
}

nlohmann::json make_contract_base(const CitrusGeometryResolveRequest& request)
{
    nlohmann::json cameras = nlohmann::json::object();
    for (const std::string& serial : request.camera_serials) {
        if (!serial.empty()) {
            cameras[serial] = {
                {"camera_serial", serial},
                {"status", "not_configured"},
                {"orange_spatial_calibration", {{"status", "not_configured"}}},
            };
        }
    }
    return {
        {"schema_id", kRecordingGeometryContractSchemaId},
        {"schema_version", kRecordingGeometryContractSchemaVersion},
        {"captured_at_utc", request.captured_at_utc},
        {"status", "not_configured"},
        {"recording_policy", {
            {"citrus_metadata_optional", true},
            {"citrus_runtime_optional", true},
            {"recording_blocked", false},
            {"missing_or_invalid_metadata_action", "record_status_and_continue"},
        }},
        {"runtime_participation", {
            {"status", "captured_separately"},
            {"recording_snapshot_field", "citrus_runtime_geometry"},
        }},
        {"selection", {
            {"source", request.selection_source.empty() ? "none" : request.selection_source},
            {"selected_canvas_config_path", request.selected_canvas_config_path},
        }},
        {"sources", nlohmann::json::object()},
        {"tank_designs", nlohmann::json::object()},
        {"cameras", std::move(cameras)},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array()},
    };
}

bool validate_active_homography(const JsonFile& file,
                                const std::string& rig_id,
                                const std::string& canvas_name,
                                const std::string& arena_id,
                                const std::string& camera_id,
                                std::string* error_out)
{
    const nlohmann::json& value = file.value;
    if (value.value("schema_id", "") != "citrus.calibration.active_homography" ||
        value.value("schema_version", 0) != 1 ||
        value.value("status", "") != "accepted" ||
        value.value("rig_id", "") != rig_id ||
        value.value("canvas_name", "") != canvas_name ||
        value.value("arena_id", "") != arena_id ||
        value.value("camera_id", "") != camera_id ||
        value.value("target_plane", "") != "projected_surface" ||
        value.value("homography_direction", "") !=
            "camera_native_px_to_final_display_canvas_px" ||
        !value.contains("homography_matrix")) {
        if (error_out) {
            *error_out = "active homography identity or coordinate contract is invalid";
        }
        return false;
    }
    return true;
}

bool validate_active_scale(const JsonFile& file,
                           const std::string& rig_id,
                           const std::string& canvas_name,
                           const std::string& arena_id,
                           const std::string& camera_id,
                           const std::string& homography_candidate_id,
                           std::string* error_out)
{
    const nlohmann::json& value = file.value;
    const nlohmann::json active_homography =
        value.value("active_homography", nlohmann::json::object());
    const nlohmann::json scale = value.value("scale", nlohmann::json::object());
    if (value.value("schema_id", "") !=
            "citrus.calibration.active_projected_surface_scale" ||
        value.value("schema_version", 0) != 1 ||
        value.value("status", "") != "accepted" ||
        value.value("rig_id", "") != rig_id ||
        value.value("canvas_name", "") != canvas_name ||
        value.value("arena_id", "") != arena_id ||
        value.value("camera_id", "") != camera_id ||
        value.value("target_plane", "") != "projected_surface" ||
        value.value("direction", "") !=
            "physical_target_mm_to_final_display_canvas_px" ||
        active_homography.value("candidate_id", "") != homography_candidate_id ||
        !scale.contains("camera_pixels_per_mm") ||
        !scale.contains("canvas_pixels_per_mm")) {
        if (error_out) {
            *error_out = "active projected-surface scale identity, units, or homography binding is invalid";
        }
        return false;
    }
    return true;
}

std::string safe_path_component(const std::string& value)
{
    std::string safe;
    safe.reserve(std::min<std::size_t>(value.size(), 96));
    for (const unsigned char character : value) {
        if (safe.size() >= 96) {
            break;
        }
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.') {
            safe.push_back(static_cast<char>(character));
        } else {
            safe.push_back('_');
        }
    }
    while (!safe.empty() && safe.front() == '.') {
        safe.front() = '_';
    }
    return safe.empty() ? "unnamed" : safe;
}

const nlohmann::json* find_daily_registration_target(
    const nlohmann::json& registration,
    const std::string& arena_id,
    const std::string& camera_id)
{
    const auto targets_it = registration.find("targets");
    if (targets_it == registration.end() || !targets_it->is_array()) {
        return nullptr;
    }
    for (const auto& target : *targets_it) {
        if (target.is_object() && target.value("arena_id", "") == arena_id &&
            target.value("camera_id", "") == camera_id) {
            return &target;
        }
    }
    return nullptr;
}

bool read_relative_json_file(const std::filesystem::path& root,
                             const std::filesystem::path& relative,
                             JsonFile* out,
                             std::string* error_out)
{
    const std::filesystem::path path = root / relative;
    if (relative.empty() || relative.is_absolute() ||
        !path_is_inside(path, root)) {
        if (error_out) {
            *error_out = "daily-registration member path escapes its observation root";
        }
        return false;
    }
    return read_json_file(path, out, error_out);
}

bool finite_circle(const nlohmann::json& value)
{
    if (!value.is_object() || value.value("type", "") != "circle") {
        return false;
    }
    const nlohmann::json center = value.value(
        "center_px", nlohmann::json::object());
    if (!center.is_object() || !center.contains("x") ||
        !center.contains("y") || !value.contains("radius_px") ||
        !center["x"].is_number() || !center["y"].is_number() ||
        !value["radius_px"].is_number()) {
        return false;
    }
    const double x = center["x"].get<double>();
    const double y = center["y"].get<double>();
    const double radius = value["radius_px"].get<double>();
    return std::isfinite(x) && std::isfinite(y) &&
        std::isfinite(radius) && radius > 0.0;
}

bool canonical_utc_second_timestamp(const std::string& value)
{
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 ||
            index == 16 || index == 19) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

nlohmann::json build_registered_dish_mask_snapshot(
    const JsonFile& observation,
    const std::string& camera_id,
    const std::string& arena_id)
{
    const nlohmann::json& value = observation.value;
    nlohmann::json entry = {
        {"artifact_id", value.value("artifact_id", "")},
        {"artifact_schema_id", value.value("schema_id", "")},
        {"artifact_schema_version", value.value("schema_version", 0)},
        {"camera_serial", camera_id},
        {"arena_id", arena_id},
        {"calibration_ref", value.value(
            "calibration_ref", nlohmann::json::object())},
        {"camera", value.value("camera", nlohmann::json::object())},
        {"physical_target", value.value("physical_target", "dish_top_rim")},
        {"coordinate_space", value.value(
            "accepted_mask", nlohmann::json::object()).value(
                "coordinate_space", "camera_native_pixels")},
        {"accepted_inner_rim_boundary", value.value(
            "accepted_inner_rim_boundary",
            value.value(
                "accepted_experimental_area_boundary",
                nlohmann::json::object()))},
        {"accepted_experimental_area_boundary", value.value(
            "accepted_experimental_area_boundary",
            nlohmann::json::object())},
        {"accepted_mask", value.value(
            "accepted_mask", nlohmann::json::object())},
        {"valid_detection_region", value.value(
            "valid_detection_region", nlohmann::json::object())},
        {"boundary_interpretation", value.value(
            "boundary_interpretation", nlohmann::json::object())},
        {"operator_review", value.value(
            "operator_review", nlohmann::json::object())},
        {"review_artifacts", value.value(
            "review_artifacts", nlohmann::json::object())},
        {"runtime_verification", value.value(
            "runtime_verification", nlohmann::json::object())},
        {"source", {
            {"path", observation.path.string()},
            {"sha256", observation.sha256},
            {"intended_recording_relative_path",
             (std::filesystem::path("recording_geometry_assets") /
              "cameras" / ("Cam" + safe_path_component(camera_id)) /
              "daily_registration" / "rim_observation" /
              "observation.json").generic_string()},
        }},
        {"available_for_downstream_detection_gating", true},
        {"active_in_orange_live_detection_pipeline", false},
        {"gating_semantics", "bounding_box_centroid_inside_valid_detection_region"},
    };
    return entry;
}

bool validate_daily_rim_observation(
    const JsonFile& observation,
    const nlohmann::json& target,
    const ArenaBinding& selected_binding,
    const std::string& rig_id,
    const std::string& canvas_name,
    const std::string& camera_id,
    std::string* error_out)
{
    const nlohmann::json& value = observation.value;
    const nlohmann::json camera = value.value(
        "camera", nlohmann::json::object());
    const nlohmann::json arena_context = value.value(
        "arena_context", nlohmann::json::object());
    const nlohmann::json inner = value.value(
        "accepted_inner_rim_boundary", nlohmann::json::object());
    const nlohmann::json inner_geometry = inner.value(
        "geometry", nlohmann::json::object());
    const nlohmann::json accepted_mask = value.value(
        "accepted_mask", nlohmann::json::object());
    const nlohmann::json mask_center = accepted_mask.value(
        "center_px", nlohmann::json::object());
    const nlohmann::json valid_region = value.value(
        "valid_detection_region", nlohmann::json::object());
    const nlohmann::json valid_geometry = valid_region.value(
        "geometry", nlohmann::json::object());
    const nlohmann::json target_center = target.value(
        "rim_center_camera_px", nlohmann::json::object());
    const int native_width = selected_binding.camera.value("native_width_px", 0);
    const int native_height = selected_binding.camera.value("native_height_px", 0);

    if (value.value("schema_id", "") !=
            "orange.calibration.dish_top_rim_observation" ||
        value.value("schema_version", 0) != 2 ||
        value.value("artifact_id", "") !=
            target.value("rim_observation", nlohmann::json::object()).value(
                "artifact_id", "") ||
        camera.value("serial", "") != camera_id ||
        camera.value("width", 0) != native_width ||
        camera.value("height", 0) != native_height ||
        arena_context.value("camera_serial", "") != camera_id ||
        arena_context.value("arena_id", "") != selected_binding.arena_id ||
        (!arena_context.value("rig_id", "").empty() &&
         arena_context.value("rig_id", "") != rig_id) ||
        (!arena_context.value("canvas_id", "").empty() &&
         arena_context.value("canvas_id", "") != canvas_name) ||
        inner.value("coordinate_space", "") != "camera_native_pixels" ||
        inner.value("target_plane", "") != "dish_top_rim" ||
        !finite_circle(inner_geometry) ||
        accepted_mask.value("coordinate_space", "") !=
            "camera_native_pixels" ||
        accepted_mask.value("shape", "") != "circle" ||
        !mask_center.is_object() || !mask_center.contains("x") ||
        !mask_center.contains("y") || !mask_center["x"].is_number() ||
        !mask_center["y"].is_number() ||
        !accepted_mask.contains("radius_px") ||
        !accepted_mask["radius_px"].is_number() ||
        valid_region.value("coordinate_space", "") !=
            "camera_native_pixels" ||
        valid_region.value("purpose", "") !=
            "bounding_box_centroid_detection_gating" ||
        valid_region.value("offset_direction", "") != "outward" ||
        !finite_circle(valid_geometry) ||
        !value.value("operator_review", nlohmann::json::object()).value(
            "accepted", false)) {
        if (error_out) {
            *error_out =
                "daily rim observation identity, raster, plane, or schema-v2 mask contract is invalid";
        }
        return false;
    }

    const nlohmann::json inner_center = inner_geometry["center_px"];
    const nlohmann::json valid_center = valid_geometry["center_px"];
    const double inner_radius = inner_geometry["radius_px"].get<double>();
    const double mask_radius = accepted_mask["radius_px"].get<double>();
    const double valid_radius = valid_geometry["radius_px"].get<double>();
    const auto close = [](const double lhs, const double rhs) {
        return std::isfinite(lhs) && std::isfinite(rhs) &&
            std::abs(lhs - rhs) <= 1e-6;
    };
    if (!target_center.is_object() || !target_center.contains("x") ||
        !target_center.contains("y") ||
        !target_center["x"].is_number() ||
        !target_center["y"].is_number() ||
        !target.contains("observed_rim_radius_camera_px") ||
        !target["observed_rim_radius_camera_px"].is_number() ||
        !close(inner_center["x"].get<double>(), target_center["x"].get<double>()) ||
        !close(inner_center["y"].get<double>(), target_center["y"].get<double>()) ||
        !close(inner_radius,
               target["observed_rim_radius_camera_px"].get<double>()) ||
        !close(inner_center["x"].get<double>(), mask_center["x"].get<double>()) ||
        !close(inner_center["y"].get<double>(), mask_center["y"].get<double>()) ||
        !close(inner_center["x"].get<double>(), valid_center["x"].get<double>()) ||
        !close(inner_center["y"].get<double>(), valid_center["y"].get<double>()) ||
        !close(mask_radius, valid_radius) || mask_radius <= 0.0 ||
        mask_radius < inner_radius) {
        if (error_out) {
            *error_out =
                "daily rim registration and schema-v2 boundary/mask geometry disagree";
        }
        return false;
    }
    return true;
}

void resolve_daily_registration_geometry(
    const std::filesystem::path& selected_artifact_root,
    const std::string& rig_id,
    const std::string& canvas_name,
    const std::string& captured_at_utc,
    const std::vector<std::string>& requested_cameras,
    const std::map<std::string, ArenaBinding>& selected_bindings,
    const bool commissioning_valid,
    const JsonFile& commissioning_release,
    nlohmann::json* contract)
{
    if (contract == nullptr) {
        return;
    }
    nlohmann::json daily = {
        {"schema_id", "orange.recording.daily_registration_geometry"},
        {"schema_version", 1},
        {"status", "not_configured"},
        {"mode", "base_only"},
        {"recording_blocked", false},
        {"participating_cameras_only", true},
        {"cameras", nlohmann::json::object()},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array()},
    };

    const std::filesystem::path selection_path =
        selected_artifact_root / "daily_registration_runtime_selection.json";
    JsonFile selection;
    std::string error;
    if (!read_json_file(selection_path, &selection, &error)) {
        daily["reason"] = "runtime_selection_pointer_missing";
        daily["warnings"].push_back(
            "No readable daily-registration runtime selection; commissioned base geometry remains valid.");
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }
    daily["runtime_selection"] = json_file_snapshot(selection);
    const std::string mode = selection.value.value("mode", "");
    daily["mode"] = mode;
    if (selection.value.value("schema_id", "") !=
            "citrus.calibration.daily_registration_runtime_selection" ||
        selection.value.value("schema_version", 0) != 1 ||
        selection.value.value("status", "") != "accepted" ||
        selection.value.value("rig_id", "") != rig_id ||
        selection.value.value("canvas_name", "") != canvas_name ||
        (mode != "base_only" && mode != "selected_daily_registration")) {
        daily["status"] = "invalid";
        daily["errors"].push_back(
            "Daily-registration runtime selection identity or mode is invalid.");
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }
    if (mode == "base_only") {
        daily["status"] = "base_only";
        daily["reason"] = "operator_selected_commissioned_base_geometry";
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }

    const nlohmann::json registration_reference = selection.value.value(
        "registration", nlohmann::json::object());
    const std::filesystem::path registration_path =
        registration_reference.value("path", "");
    const std::string registration_sha256 =
        registration_reference.value("sha256", "");
    const std::filesystem::path daily_root =
        selected_artifact_root / "daily_registration";
    JsonFile registration;
    if (registration_path.empty() || registration_sha256.empty() ||
        !path_is_inside(registration_path, daily_root) ||
        !read_json_file(registration_path, &registration, &error) ||
        registration.sha256 != registration_sha256) {
        daily["status"] = "invalid";
        daily["errors"].push_back(
            "Selected daily-registration artifact is missing, outside its authority root, or checksum-mismatched.");
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }
    daily["registration"] = json_file_snapshot(registration);
    const std::string registration_id = registration.value.value(
        "registration_id", "");
    const std::string valid_until_utc = registration.value.value(
        "valid_until_utc", "");
    const nlohmann::json selection_commissioning = selection.value.value(
        "commissioning_base", nlohmann::json::object());
    const nlohmann::json registration_commissioning = registration.value.value(
        "commissioning_base", nlohmann::json::object());
    const bool commissioning_matches = commissioning_valid &&
        registration_commissioning.value("release_id", "") ==
            commissioning_release.value.value("release_id", "") &&
        registration_commissioning.value("manifest_sha256", "") ==
            commissioning_release.sha256 &&
        selection_commissioning.value("release_id", "") ==
            registration_commissioning.value("release_id", "") &&
        selection_commissioning.value("manifest_sha256", "") ==
            registration_commissioning.value("manifest_sha256", "");
    if (registration.value.value("schema_id", "") !=
            "citrus.calibration.daily_registration" ||
        registration.value.value("schema_version", 0) != 1 ||
        registration.value.value("status", "") != "accepted" ||
        registration.value.value("rig_id", "") != rig_id ||
        registration.value.value("canvas_name", "") != canvas_name ||
        registration_id.empty() ||
        registration_id != registration_reference.value("registration_id", "") ||
        !commissioning_matches) {
        daily["status"] = "invalid";
        daily["errors"].push_back(
            "Selected daily registration identity or commissioning binding is invalid.");
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }
    if (!canonical_utc_second_timestamp(valid_until_utc) ||
        registration_reference.value("valid_until_utc", "") != valid_until_utc ||
        (canonical_utc_second_timestamp(captured_at_utc) &&
         valid_until_utc <= captured_at_utc)) {
        daily["status"] = "invalid";
        daily["reason"] = "selected_daily_registration_expired_or_invalid";
        daily["errors"].push_back(
            "Selected daily registration has an invalid, mismatched, or expired validity interval.");
        (*contract)["daily_registration_geometry"] = std::move(daily);
        return;
    }

    const std::filesystem::path candidate_path =
        registration.value.value("candidate_path", "");
    const std::string candidate_sha256 =
        registration.value.value("candidate_sha256", "");
    JsonFile candidate;
    const std::string registration_transaction_id = registration.value.value(
        "transaction_id", "");
    if (candidate_path.empty() || candidate_sha256.empty() ||
        !path_is_inside(candidate_path, daily_root) ||
        !read_json_file(candidate_path, &candidate, &error) ||
        candidate.sha256 != candidate_sha256 ||
        candidate.value.value("schema_id", "") !=
            "citrus.calibration.daily_registration_candidate" ||
        candidate.value.value("schema_version", 0) != 1 ||
        candidate.value.value("status", "") != "candidate" ||
        candidate.value.value("candidate_id", "") != registration_id ||
        registration_transaction_id.empty() ||
        candidate.value.value("transaction_id", "") !=
            registration_transaction_id ||
        candidate.value.value("rig_id", "") != rig_id ||
        candidate.value.value("canvas_name", "") != canvas_name) {
        daily["warnings"].push_back(
            "Accepted daily-registration candidate could not be identity- and checksum-resolved.");
    } else {
        daily["candidate"] = json_file_snapshot(candidate);
    }

    std::size_t requested_count = 0;
    std::size_t resolved_count = 0;
    for (const std::string& camera_id : requested_cameras) {
        if (camera_id.empty()) {
            continue;
        }
        ++requested_count;
        nlohmann::json camera_daily = {
            {"camera_serial", camera_id},
            {"status", "unavailable"},
            {"errors", nlohmann::json::array()},
        };
        const auto binding_it = selected_bindings.find(camera_id);
        if (binding_it == selected_bindings.end()) {
            camera_daily["errors"].push_back(
                "Camera has no selected-canvas arena binding.");
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }
        const ArenaBinding& binding = binding_it->second;
        camera_daily["arena_id"] = binding.arena_id;
        const nlohmann::json* target = find_daily_registration_target(
            registration.value, binding.arena_id, camera_id);
        if (target == nullptr) {
            camera_daily["errors"].push_back(
                "Selected registration has no matching camera/arena target.");
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }
        camera_daily["registration_target"] = *target;
        const nlohmann::json camera_contract =
            contract->value("cameras", nlohmann::json::object()).value(
                camera_id, nlohmann::json::object());
        const nlohmann::json projection = camera_contract.value(
            "projection_geometry", nlohmann::json::object());
        const nlohmann::json active_homography = projection.value(
            "homography", nlohmann::json::object()).value(
                "active_pointer_snapshot", nlohmann::json::object());
        const nlohmann::json target_homography = target->value(
            "homography", nlohmann::json::object());
        const nlohmann::json invariants = target->value(
            "invariants", nlohmann::json::object());
        if (projection.value("status", "") != "resolved" ||
            target->value("target_plane", "") != "projected_surface" ||
            target_homography.value("candidate_id", "").empty() ||
            target_homography.value("candidate_id", "") !=
                active_homography.value("candidate_id", "") ||
            target_homography.value("candidate_path", "") !=
                active_homography.value("candidate_json_path", "") ||
            target_homography.value("candidate_sha256", "") !=
                active_homography.value("candidate_json_checksum", "") ||
            !invariants.value("arena_size_unchanged", false) ||
            !invariants.value("canvas_geometry_unchanged", false) ||
            !invariants.value("experimental_area_local_geometry_unchanged", false) ||
            !invariants.value("homography_unchanged", false) ||
            !invariants.value("scale_unchanged", false)) {
            camera_daily["errors"].push_back(
                "Daily registration is not bound to the resolved commissioned homography and translation-only invariants.");
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }
        const nlohmann::json rim_reference = target->value(
            "rim_observation", nlohmann::json::object());
        const std::filesystem::path observation_path =
            rim_reference.value("path", "");
        const std::string observation_sha256 =
            rim_reference.value("sha256", "");
        JsonFile observation;
        if (observation_path.empty() || observation_sha256.empty() ||
            !read_json_file(observation_path, &observation, &error) ||
            observation.sha256 != observation_sha256 ||
            !validate_daily_rim_observation(
                observation, *target, binding, rig_id, canvas_name,
                camera_id, &error)) {
            camera_daily["errors"].push_back(
                error.empty()
                    ? "Daily rim observation could not be checksum-resolved."
                    : error);
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }

        const std::filesystem::path observation_root =
            observation.path.parent_path();
        JsonFile manifest;
        if (!read_relative_json_file(
                observation_root, "manifest.json", &manifest, &error) ||
            manifest.value.value("schema_id", "") !=
                "orange.calibration.manifest" ||
            manifest.value.value("artifact_id", "") !=
                observation.value.value("artifact_id", "")) {
            camera_daily["errors"].push_back(
                "Daily rim observation manifest is missing or has the wrong identity.");
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }

        nlohmann::json compact_artifacts = nlohmann::json::object();
        compact_artifacts["manifest"] = json_file_snapshot(manifest);
        const nlohmann::json manifest_files = manifest.value.value(
            "files", nlohmann::json::object());
        struct CompactMember {
            const char* key;
            const char* manifest_field;
        };
        static constexpr CompactMember compact_members[] = {
            {"image_set", "image_set_json"},
            {"spatial_dish_mask_runtime_v1", "spatial_dish_mask_runtime_v1"},
            {"palette_dish_mask_v2", "palette_dish_mask_v2"},
        };
        bool compact_valid = true;
        for (const CompactMember& member : compact_members) {
            JsonFile member_file;
            const std::filesystem::path member_relative =
                manifest_files.value(member.manifest_field, "");
            if (!read_relative_json_file(
                    observation_root, member_relative, &member_file, &error)) {
                compact_valid = false;
                camera_daily["errors"].push_back(
                    std::string("Missing daily rim compact member: ") +
                    member.manifest_field + ".");
                continue;
            }
            compact_artifacts[member.key] = json_file_snapshot(member_file);
        }
        if (compact_valid) {
            const nlohmann::json image_set = compact_artifacts.value(
                "image_set", nlohmann::json::object()).value(
                    "snapshot", nlohmann::json::object());
            const nlohmann::json image_set_camera = image_set.value(
                "camera", nlohmann::json::object());
            const nlohmann::json image_shape = image_set_camera.value(
                "image_shape", nlohmann::json::object());
            const nlohmann::json spatial_export = compact_artifacts.value(
                "spatial_dish_mask_runtime_v1",
                nlohmann::json::object()).value(
                    "snapshot", nlohmann::json::object());
            const nlohmann::json spatial_source = spatial_export.value(
                "source_observation", nlohmann::json::object());
            const nlohmann::json spatial_geometry = spatial_export.value(
                "geometry", nlohmann::json::object());
            const nlohmann::json outer_geometry = spatial_geometry.value(
                "outer_geometry", nlohmann::json::object());
            const nlohmann::json exported_valid_geometry =
                spatial_geometry.value(
                    "valid_geometry", nlohmann::json::object());
            const nlohmann::json palette_export = compact_artifacts.value(
                "palette_dish_mask_v2",
                nlohmann::json::object()).value(
                    "snapshot", nlohmann::json::object());
            const nlohmann::json inner_geometry = observation.value.value(
                "accepted_inner_rim_boundary",
                nlohmann::json::object()).value(
                    "geometry", nlohmann::json::object());
            const nlohmann::json valid_geometry = observation.value.value(
                "valid_detection_region",
                nlohmann::json::object()).value(
                    "geometry", nlohmann::json::object());
            const nlohmann::json inner_center = inner_geometry.value(
                "center_px", nlohmann::json::object());
            const nlohmann::json valid_center = valid_geometry.value(
                "center_px", nlohmann::json::object());
            const auto exported_number_matches = [](
                const nlohmann::json& value,
                const char* key,
                const double expected) {
                const auto it = value.find(key);
                return it != value.end() && it->is_number() &&
                    std::abs(it->get<double>() - expected) <= 1e-6;
            };
            const bool compact_identity_valid =
                image_set.value("schema_id", "") ==
                    "orange.calibration.image_set" &&
                image_set.value("schema_version", 0) == 1 &&
                image_set.value("artifact_id", "") ==
                    observation.value.value("artifact_id", "") &&
                image_set.value("purpose", "") == "dish_top_rim" &&
                image_set.value("coordinate_space", "") ==
                    "camera_native_pixels" &&
                image_set.value("target_plane", "") == "dish_top_rim" &&
                image_set_camera.value("serial", "") == camera_id &&
                image_shape.value("width", 0) ==
                    binding.camera.value("native_width_px", 0) &&
                image_shape.value("height", 0) ==
                    binding.camera.value("native_height_px", 0) &&
                spatial_export.value("schema_version", 0) == 1 &&
                spatial_export.value("enabled", false) &&
                spatial_geometry.value("coordinate_space", "") ==
                    "camera_native_pixels" &&
                spatial_source.value("artifact_id", "") ==
                    observation.value.value("artifact_id", "") &&
                spatial_source.value("artifact_schema_id", "") ==
                    "orange.calibration.dish_top_rim_observation" &&
                spatial_source.value("artifact_schema_version", 0) == 2 &&
                outer_geometry.value("type", "") == "circle" &&
                exported_number_matches(
                    outer_geometry, "cx", inner_center.value("x", -1.0)) &&
                exported_number_matches(
                    outer_geometry, "cy", inner_center.value("y", -1.0)) &&
                exported_number_matches(
                    outer_geometry, "r", inner_geometry.value("radius_px", -1.0)) &&
                exported_valid_geometry.value("type", "") == "circle" &&
                exported_number_matches(
                    exported_valid_geometry, "cx",
                    valid_center.value("x", -1.0)) &&
                exported_number_matches(
                    exported_valid_geometry, "cy",
                    valid_center.value("y", -1.0)) &&
                exported_number_matches(
                    exported_valid_geometry, "r",
                    valid_geometry.value("radius_px", -1.0)) &&
                palette_export.value("version", "") == "2.0" &&
                palette_export.value("shape", "") == "circle" &&
                palette_export.value("orange_artifact_id", "") ==
                    observation.value.value("artifact_id", "") &&
                palette_export.value("orange_artifact_schema_id", "") ==
                    "orange.calibration.dish_top_rim_observation" &&
                palette_export.value("orange_artifact_schema_version", 0) == 2;
            if (!compact_identity_valid) {
                compact_valid = false;
                camera_daily["errors"].push_back(
                    "Daily rim manifest members disagree with the accepted schema-v2 observation or native raster.");
            }
        }
        if (!compact_valid) {
            daily["cameras"][camera_id] = std::move(camera_daily);
            continue;
        }

        nlohmann::json optional_evidence = nlohmann::json::object();
        const nlohmann::json declared_checksums = manifest.value.value(
            "checksums", nlohmann::json::object());
        struct EvidenceMember {
            const char* key;
            const char* manifest_field;
            const char* checksum_field;
        };
        static constexpr EvidenceMember evidence_members[] = {
            {"review_overlay", "review_overlay", "review_overlay"},
            {"valid_detection_overlay", "valid_detection_overlay",
             "valid_detection_overlay"},
            {"registration_hough_overlay", "registration_hough_overlay",
             "registration_hough_overlay"},
            {"source_frame", "source_frame", "source_frame"},
        };
        for (const EvidenceMember& member : evidence_members) {
            const std::filesystem::path relative =
                manifest_files.value(member.manifest_field, "");
            const std::filesystem::path path = observation_root / relative;
            if (relative.empty() || relative.is_absolute() ||
                !path_is_inside(path, observation_root)) {
                continue;
            }
            optional_evidence[member.key] = {
                {"source_path", normalized_path(path).string()},
                {"declared_checksum", declared_checksums.value(
                    member.checksum_field, "")},
                {"declared_checksum_algorithm", declared_checksums.value(
                    "algorithm", "")},
            };
        }

        const nlohmann::json recording_snapshot_entry =
            build_registered_dish_mask_snapshot(
                observation, camera_id, binding.arena_id);
        camera_daily.update({
            {"status", "resolved"},
            {"registration_id", registration_id},
            {"rim_observation", json_file_snapshot(observation)},
            {"compact_artifacts", std::move(compact_artifacts)},
            {"optional_evidence", std::move(optional_evidence)},
            {"recording_snapshot_entry", recording_snapshot_entry},
        });
        (*contract)["cameras"][camera_id]["daily_registration_geometry"] = {
            {"schema_id", "orange.recording.daily_registration_camera_geometry"},
            {"schema_version", 1},
            {"status", "resolved"},
            {"mode", "selected_daily_registration"},
            {"registration_id", registration_id},
            {"registration", {
                {"source_path", registration.path.string()},
                {"sha256", registration.sha256}}},
            {"recording_snapshot_entry", recording_snapshot_entry},
        };
        daily["cameras"][camera_id] = std::move(camera_daily);
        ++resolved_count;
    }

    daily["registration_id"] = registration_id;
    daily["requested_camera_count"] = requested_count;
    daily["resolved_camera_count"] = resolved_count;
    daily["status"] = requested_count > 0 && resolved_count == requested_count
        ? "selected_resolved"
        : "selected_partial";
    if (!daily.contains("candidate")) {
        daily["status"] = "selected_partial";
    }
    (*contract)["daily_registration_geometry"] = std::move(daily);
}

}  // namespace

CitrusGeometryResolveResult resolve_citrus_recording_geometry(
    const CitrusGeometryResolveRequest& request)
{
    CitrusGeometryResolveResult result;
    result.contract = make_contract_base(request);
    if (request.selected_canvas_config_path.empty()) {
        result.contract["warnings"].push_back(
            "No Citrus rig/canvas was selected; Orange-only metadata remains valid.");
        return result;
    }
    result.configured = true;

    try {
        nlohmann::json& contract = result.contract;
        nlohmann::json& errors = contract["errors"];
        nlohmann::json& warnings = contract["warnings"];
        const std::filesystem::path selected_path =
            normalized_path(request.selected_canvas_config_path);
        const std::filesystem::path selected_dir = selected_path.parent_path();
        const std::filesystem::path rig_dir = selected_dir.parent_path();
        const std::string selected_canvas_name = selected_dir.filename().string();
        const std::string rig_dir_name = rig_dir.filename().string();

        JsonFile selected_canvas;
        std::string error;
        if (!read_json_file(selected_path, &selected_canvas, &error)) {
            append_message(&errors, error);
            contract["status"] = "invalid";
            return result;
        }
        if (selected_canvas.value.value("canvas_name", "") !=
                selected_canvas_name) {
            append_message(
                &errors,
                "selected canvas identity does not match its Citrus folder name");
        }
        contract["sources"]["selected_canvas"] =
            json_file_snapshot(selected_canvas);

        const std::filesystem::path rig_config_path =
            rig_dir / (rig_dir_name + "_config.json");
        JsonFile rig_config;
        if (!read_json_file(rig_config_path, &rig_config, &error)) {
            append_message(&errors, error);
        } else {
            contract["sources"]["rig"] = json_file_snapshot(rig_config);
        }
        const std::string rig_id = rig_config.value.value("rig_id", rig_dir_name);
        if (!rig_config.value.empty() && rig_id != rig_dir_name) {
            append_message(&errors, "rig identity does not match its Citrus folder name");
        }

        std::string authority_canvas_name = selected_canvas_name;
        std::string authority_mode = "local_canvas";
        nlohmann::json authority_contract = nlohmann::json::object();
        const auto authority_it = selected_canvas.value.find(
            "projection_geometry_authority");
        if (authority_it != selected_canvas.value.end()) {
            if (!authority_it->is_object()) {
                append_message(&errors, "projection_geometry_authority must be an object");
            } else {
                authority_contract = *authority_it;
                authority_mode = authority_contract.value("mode", "");
                authority_canvas_name =
                    authority_contract.value("source_canvas_name", "");
                if (authority_contract.value("schema_version", 0) != 1 ||
                    authority_mode != "inherit_active_commissioning" ||
                    authority_canvas_name.empty() ||
                    authority_canvas_name == selected_canvas_name ||
                    authority_contract.value("geometry_scope", "") !=
                        "arena_placement_homography_and_projected_surface_scale" ||
                    authority_contract.value("experimental_area_owner", "") !=
                        "selected_canvas") {
                    append_message(
                        &errors,
                        "unsupported or invalid projection_geometry_authority contract");
                }
            }
        }

        const std::filesystem::path authority_path = authority_mode == "local_canvas"
            ? selected_path
            : rig_dir / authority_canvas_name / (authority_canvas_name + ".json");
        JsonFile authority_canvas;
        if (authority_path == selected_path) {
            authority_canvas = selected_canvas;
        } else if (!read_json_file(authority_path, &authority_canvas, &error)) {
            append_message(&errors, error);
        }
        if (!authority_canvas.value.empty() &&
            authority_canvas.value.value("canvas_name", "") !=
                authority_canvas_name) {
            append_message(&errors, "authority canvas identity mismatch");
        }
        if (!authority_canvas.value.empty()) {
            contract["sources"]["projection_authority_canvas"] =
                json_file_snapshot(authority_canvas);
        }
        if (authority_mode != "local_canvas" && !authority_canvas.value.empty()) {
            if (!same_positive_integer_field(
                    selected_canvas.value, authority_canvas.value, "canvas_width_px") ||
                !same_positive_integer_field(
                    selected_canvas.value, authority_canvas.value, "canvas_height_px")) {
                append_message(
                    &errors,
                    "selected and projection-authority canvas dimensions do not match");
            }
        }

        contract["selection"].update({
            {"configured", true},
            {"rig_id", rig_id},
            {"selected_canvas_name", selected_canvas_name},
            {"projection_geometry_authority_mode", authority_mode},
            {"projection_geometry_authority_canvas_name", authority_canvas_name},
            {"experimental_area_owner", "selected_canvas"},
        });
        if (!authority_contract.empty()) {
            contract["selection"]["projection_geometry_authority_contract"] =
                authority_contract;
        }

        const auto selected_bindings = camera_bindings(
            selected_canvas.value, &errors, "selected canvas");
        const auto authority_bindings = camera_bindings(
            authority_canvas.value, &errors, "projection-authority canvas");

        const std::filesystem::path authority_artifact_root =
            authority_path.parent_path() / "calibration_artifacts";
        JsonFile commissioning_pointer;
        JsonFile commissioning_release;
        bool commissioning_valid = false;
        const std::filesystem::path commissioning_pointer_path =
            authority_artifact_root / "commissioning_active.json";
        if (!read_json_file(
                commissioning_pointer_path, &commissioning_pointer, &error)) {
            append_message(
                &warnings,
                "Projection authority has no readable active commissioning pointer: " +
                    error);
        } else if (
            commissioning_pointer.value.value("schema_id", "") !=
                "citrus.calibration.active_rig_canvas_commissioning" ||
            commissioning_pointer.value.value("schema_version", 0) != 1 ||
            commissioning_pointer.value.value("status", "") != "accepted" ||
            commissioning_pointer.value.value("rig_id", "") != rig_id ||
            commissioning_pointer.value.value("canvas_name", "") !=
                authority_canvas_name) {
            append_message(&errors, "active commissioning pointer identity is invalid");
        } else {
            const std::filesystem::path manifest_path =
                commissioning_pointer.value.value("manifest_path", "");
            const std::string manifest_sha256 =
                commissioning_pointer.value.value("manifest_sha256", "");
            if (manifest_path.empty() ||
                !path_is_inside(manifest_path, authority_artifact_root / "commissioning")) {
                append_message(
                    &errors,
                    "active commissioning manifest path is outside the authority root");
            } else if (!read_json_file(manifest_path, &commissioning_release, &error)) {
                append_message(&errors, error);
            } else if (commissioning_release.sha256 != manifest_sha256) {
                append_message(&errors, "active commissioning manifest checksum mismatch");
            } else if (
                commissioning_release.value.value("schema_id", "") !=
                    "citrus.calibration.rig_canvas_commissioning_release" ||
                commissioning_release.value.value("schema_version", 0) != 1 ||
                commissioning_release.value.value("status", "") != "accepted" ||
                commissioning_release.value.value("rig_id", "") != rig_id ||
                commissioning_release.value.value("canvas_name", "") !=
                    authority_canvas_name ||
                commissioning_release.value.value("release_id", "") !=
                    commissioning_pointer.value.value("release_id", "")) {
                append_message(&errors, "active commissioning release identity is invalid");
            } else {
                commissioning_valid = true;
            }
            contract["sources"]["commissioning_active"] =
                json_file_snapshot(commissioning_pointer);
            if (!commissioning_release.value.empty()) {
                contract["sources"]["commissioning_release"] =
                    json_file_snapshot(commissioning_release);
            }
        }

        std::set<std::string> loaded_tank_designs;
        std::size_t resolved_camera_count = 0;
        for (const std::string& camera_id : request.camera_serials) {
            if (camera_id.empty()) {
                continue;
            }
            nlohmann::json& camera_contract = contract["cameras"][camera_id];
            camera_contract["camera_serial"] = camera_id;
            camera_contract["status"] = "unavailable";
            camera_contract["errors"] = nlohmann::json::array();
            camera_contract["warnings"] = nlohmann::json::array();

            const auto selected_it = selected_bindings.find(camera_id);
            if (selected_it == selected_bindings.end()) {
                append_message(
                    &camera_contract["errors"],
                    "camera is not mapped by the selected Citrus canvas");
                continue;
            }
            const ArenaBinding& selected_binding = selected_it->second;
            camera_contract["arena_id"] = selected_binding.arena_id;
            camera_contract["selected_canvas"] = {
                {"canvas_name", selected_canvas_name},
                {"experimental_area", selected_experimental_area(selected_binding.arena)},
                {"configured_arena_placement", arena_placement(selected_binding.camera)},
                {"camera_calibration_snapshot", selected_binding.camera},
            };

            const std::string tank_design_id =
                selected_binding.arena.value("selected_dish_type_name", "");
            camera_contract["tank_design"] = {
                {"tank_design_id", tank_design_id},
                {"owner", "selected_canvas"},
                {"status", tank_design_id.empty() ? "not_configured" : "referenced"},
            };
            if (!tank_design_id.empty() && loaded_tank_designs.insert(tank_design_id).second) {
                const bool safe_name = tank_design_id.find('/') == std::string::npos &&
                    tank_design_id.find('\\') == std::string::npos &&
                    tank_design_id != "." && tank_design_id != "..";
                const std::filesystem::path targets_root =
                    rig_dir.parent_path().parent_path();
                const std::filesystem::path tank_path =
                    targets_root / "tank_designs" / (tank_design_id + ".json");
                JsonFile tank_design;
                if (!safe_name || !read_json_file(tank_path, &tank_design, &error)) {
                    append_message(
                        &warnings,
                        "Could not snapshot selected tank design " + tank_design_id +
                            (error.empty() ? std::string() : ": " + error));
                    contract["tank_designs"][tank_design_id] = {
                        {"status", "unavailable"},
                        {"source_path", tank_path.string()},
                    };
                } else if (tank_design.value.value("tank_design_id", "") !=
                               tank_design_id) {
                    append_message(
                        &errors,
                        "tank design identity mismatch for " + tank_design_id);
                    contract["tank_designs"][tank_design_id] = {
                        {"status", "invalid"},
                        {"artifact", json_file_snapshot(tank_design)},
                    };
                } else {
                    contract["tank_designs"][tank_design_id] = {
                        {"status", "resolved"},
                        {"artifact", json_file_snapshot(tank_design)},
                    };
                }
            }

            const auto authority_it_binding = authority_bindings.find(camera_id);
            if (authority_it_binding == authority_bindings.end() ||
                authority_it_binding->second.arena_id != selected_binding.arena_id) {
                append_message(
                    &camera_contract["errors"],
                    "camera/arena mapping does not match the projection-authority canvas");
                continue;
            }
            const ArenaBinding& authority_binding = authority_it_binding->second;
            if (!same_positive_integer_field(
                    selected_binding.camera, authority_binding.camera, "native_width_px") ||
                !same_positive_integer_field(
                    selected_binding.camera, authority_binding.camera, "native_height_px")) {
                append_message(
                    &camera_contract["errors"],
                    "camera native raster does not match the projection-authority canvas");
                continue;
            }
            camera_contract["projection_geometry"] = {
                {"status", commissioning_valid ? "resolving" : "unavailable"},
                {"authority_canvas_name", authority_canvas_name},
                {"arena_placement", arena_placement(authority_binding.camera)},
                {"target_plane", "projected_surface"},
            };
            if (!commissioning_valid) {
                append_message(
                    &camera_contract["warnings"],
                    "No validated active commissioning release is available");
                continue;
            }

            const nlohmann::json* member = find_release_member(
                commissioning_release.value,
                selected_binding.arena_id,
                camera_id);
            if (member == nullptr) {
                append_message(
                    &camera_contract["errors"],
                    "camera/arena is absent from the active commissioning release");
                continue;
            }
            const nlohmann::json requirements =
                member->value("requirements", nlohmann::json::object());
            if (!requirements.value("active_homography_compatible", false) ||
                !requirements.value("active_scale_compatible", false) ||
                !requirements.value("scale_bound_to_active_homography", false) ||
                !requirements.value("acceptance_receipts_valid", false)) {
                append_message(
                    &camera_contract["errors"],
                    "commissioning member requirements are not all satisfied");
                continue;
            }

            JsonFile homography_pointer;
            JsonFile scale_pointer;
            const nlohmann::json homography_member =
                member->value("homography", nlohmann::json::object());
            const nlohmann::json scale_member =
                member->value("projected_surface_scale", nlohmann::json::object());
            if (!read_checksums_member_file(
                    homography_member,
                    "active_pointer_path",
                    "active_pointer_sha256",
                    authority_artifact_root,
                    &homography_pointer,
                    &error) ||
                !validate_active_homography(
                    homography_pointer,
                    rig_id,
                    authority_canvas_name,
                    selected_binding.arena_id,
                    camera_id,
                    &error)) {
                append_message(&camera_contract["errors"], error);
                continue;
            }
            if (!read_checksums_member_file(
                    scale_member,
                    "active_pointer_path",
                    "active_pointer_sha256",
                    authority_artifact_root,
                    &scale_pointer,
                    &error) ||
                !validate_active_scale(
                    scale_pointer,
                    rig_id,
                    authority_canvas_name,
                    selected_binding.arena_id,
                    camera_id,
                    homography_pointer.value.value("candidate_id", ""),
                    &error)) {
                append_message(&camera_contract["errors"], error);
                continue;
            }

            camera_contract["projection_geometry"].update({
                {"status", "resolved"},
                {"commissioning_release_id",
                 commissioning_release.value.value("release_id", "")},
                {"homography", {
                    {"source_path", homography_pointer.path.string()},
                    {"source_sha256", homography_pointer.sha256},
                    {"active_pointer_snapshot", homography_pointer.value},
                }},
                {"scale_models", {
                    {"projected_surface", {
                        {"source_path", scale_pointer.path.string()},
                        {"source_sha256", scale_pointer.sha256},
                        {"active_pointer_snapshot", scale_pointer.value},
                    }},
                }},
            });
            camera_contract["status"] = "resolved";
            ++resolved_camera_count;
        }

        // Daily registration belongs to the selected canvas because it moves
        // only that canvas's per-arena placement.  Its commissioning binding
        // is nevertheless checked against the resolved projection authority
        // release above.  Keep failures local to this optional metadata block:
        // a user may intentionally record from commissioned base geometry.
        resolve_daily_registration_geometry(
            selected_dir / "calibration_artifacts",
            rig_id,
            selected_canvas_name,
            request.captured_at_utc,
            request.camera_serials,
            selected_bindings,
            commissioning_valid,
            commissioning_release,
            &contract);

        const std::size_t requested_camera_count = std::count_if(
            request.camera_serials.begin(),
            request.camera_serials.end(),
            [](const std::string& serial) { return !serial.empty(); });
        bool camera_validation_failed = false;
        for (auto it = contract["cameras"].begin();
             it != contract["cameras"].end(); ++it) {
            if (it.value().is_object() &&
                it.value().value("errors", nlohmann::json::array()).is_array() &&
                !it.value().value("errors", nlohmann::json::array()).empty()) {
                camera_validation_failed = true;
                break;
            }
        }
        if (!errors.empty() || camera_validation_failed) {
            contract["status"] = "invalid";
        } else if (requested_camera_count == 0 ||
                   resolved_camera_count != requested_camera_count) {
            contract["status"] = "partial";
        } else {
            contract["status"] = "resolved";
            result.fully_resolved = true;
        }
    } catch (const std::exception& ex) {
        result.contract["status"] = "invalid";
        append_message(
            &result.contract["errors"],
            std::string("Citrus geometry resolution threw: ") + ex.what());
    }
    return result;
}

}  // namespace orange::recording_geometry
