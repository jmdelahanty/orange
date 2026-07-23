#pragma once

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace orange::gui::spatial_layout::canvas_compatibility {

namespace fs = std::filesystem;

inline constexpr const char* kIdentitySchemaId =
    "citrus.calibration.canvas_projection_geometry_identity";
inline constexpr int kIdentitySchemaVersion = 1;

struct Result {
    bool compatible = false;
    bool exact_file_checksum_match = false;
    bool projection_geometry_match = false;
    std::string basis;
    std::string error;
    std::string warning;
    std::string current_canvas_sha256;
    std::string accepted_canvas_sha256;
    std::string current_geometry_fingerprint;
    std::string accepted_geometry_fingerprint;
    std::string commissioning_release_id;
};

inline std::string sha256(const std::string& bytes)
{
    return "sha256:" + checksum::sha256_hex(bytes);
}

inline bool read_file(const fs::path& path,
                      std::string* bytes,
                      std::string* error)
{
    if (bytes == nullptr) {
        if (error) *error = "canvas_compatibility_output_missing";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "could_not_open_file:" + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        if (error) *error = "could_not_read_file:" + path.string();
        return false;
    }
    *bytes = buffer.str();
    return true;
}

inline bool has_prefix(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

// This is the same projection-geometry identity v1 rule used by Citrus.
// These controls change calibration presentation/evidence, not the accepted
// physical camera-to-projector mapping.
inline bool is_calibration_presentation_field(std::string_view key)
{
    return key == "calibration_pattern_mode" ||
        key == "calibration_pattern_mask_policy" ||
        key == "dot_radius_px" || key == "grid_cols" || key == "grid_rows" ||
        has_prefix(key, "calibration_ring_") ||
        has_prefix(key, "calibration_verification_");
}

inline bool has_positive_finite_number(const nlohmann::json& object,
                                       std::string_view key)
{
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->is_number()) return false;
    const double value = it->get<double>();
    return std::isfinite(value) && value > 0.0;
}

inline void remove_runtime_projected_surface_scale_cache(nlohmann::json* camera)
{
    if (camera == nullptr || !camera->is_object()) return;
    for (const char* key : {
             "scale_image_path",
             "real_world_ref_mm",
             "pixels_per_mm_camera",
             "pixels_per_mm_projector",
         }) {
        camera->erase(key);
    }
    const auto models = camera->find("scale_models");
    if (models == camera->end() || !models->is_array()) return;
    nlohmann::json retained = nlohmann::json::array();
    for (const auto& model : *models) {
        if (model.is_object() &&
            model.value("target_plane", "") == "projected_surface") {
            continue;
        }
        retained.push_back(model);
    }
    *models = std::move(retained);
}

inline void remove_scale_derived_experimental_area_pixel_caches(
    nlohmann::json* arena)
{
    if (arena == nullptr || !arena->is_object()) return;
    for (const auto& keys : {
             std::pair{"experimental_area_width_mm", "experimental_area_width_px"},
             std::pair{"experimental_area_height_mm", "experimental_area_height_px"},
             std::pair{"experimental_area_radius_mm", "experimental_area_radius_px"},
             std::pair{"experimental_area_corner_radius_mm",
                       "experimental_area_corner_radius_px"},
         }) {
        if (has_positive_finite_number(*arena, keys.first)) {
            arena->erase(keys.second);
        }
    }
}

inline bool build_projection_geometry_identity(const nlohmann::json& canvas,
                                               nlohmann::json* identity,
                                               std::string* error = nullptr)
{
    if (identity == nullptr) {
        if (error) *error = "canvas_geometry_identity_output_missing";
        return false;
    }
    if (!canvas.is_object() || !canvas.contains("canvas_name") ||
        !canvas.contains("canvas_width_px") ||
        !canvas.contains("canvas_height_px") ||
        !canvas.contains("arenas") || !canvas.at("arenas").is_object()) {
        if (error) *error = "canvas_geometry_identity_invalid";
        return false;
    }
    nlohmann::json geometry = canvas;
    for (auto& arena_entry : geometry["arenas"].items()) {
        auto& arena = arena_entry.value();
        if (!arena.is_object()) {
            if (error) *error = "canvas_arena_geometry_invalid";
            return false;
        }
        for (auto it = arena.begin(); it != arena.end();) {
            if (is_calibration_presentation_field(it.key())) {
                it = arena.erase(it);
            } else {
                ++it;
            }
        }
        remove_scale_derived_experimental_area_pixel_caches(&arena);
        const auto cameras = arena.find("camera_calibrations");
        if (cameras == arena.end()) continue;
        if (!cameras->is_array()) {
            if (error) *error = "canvas_camera_calibrations_invalid";
            return false;
        }
        for (auto& camera : *cameras) {
            if (!camera.is_object()) {
                if (error) *error = "canvas_camera_calibration_invalid";
                return false;
            }
            remove_runtime_projected_surface_scale_cache(&camera);
        }
    }
    *identity = {
        {"schema_id", kIdentitySchemaId},
        {"schema_version", kIdentitySchemaVersion},
        {"canvas", std::move(geometry)},
    };
    return true;
}

inline bool projection_geometry_fingerprint(const std::string& canvas_bytes,
                                            std::string* fingerprint,
                                            std::string* error = nullptr)
{
    if (fingerprint == nullptr) {
        if (error) *error = "canvas_geometry_fingerprint_output_missing";
        return false;
    }
    const nlohmann::json canvas = nlohmann::json::parse(
        canvas_bytes, nullptr, false);
    if (canvas.is_discarded()) {
        if (error) *error = "canvas_json_invalid";
        return false;
    }
    nlohmann::json identity;
    if (!build_projection_geometry_identity(canvas, &identity, error)) return false;
    *fingerprint = sha256(identity.dump());
    return true;
}

inline Result compare_canvas_bytes(const std::string& current_canvas_bytes,
                                   const std::string& accepted_canvas_bytes)
{
    Result result;
    result.current_canvas_sha256 = sha256(current_canvas_bytes);
    result.accepted_canvas_sha256 = sha256(accepted_canvas_bytes);
    if (!projection_geometry_fingerprint(
            current_canvas_bytes, &result.current_geometry_fingerprint,
            &result.error) ||
        !projection_geometry_fingerprint(
            accepted_canvas_bytes, &result.accepted_geometry_fingerprint,
            &result.error)) {
        return result;
    }
    result.exact_file_checksum_match =
        result.current_canvas_sha256 == result.accepted_canvas_sha256;
    result.projection_geometry_match =
        result.current_geometry_fingerprint == result.accepted_geometry_fingerprint;
    result.compatible = result.projection_geometry_match;
    result.basis = result.exact_file_checksum_match
        ? "exact_full_file_sha256"
        : "projection_geometry_identity_v1";
    if (!result.exact_file_checksum_match && result.projection_geometry_match) {
        result.warning = "canvas_non_geometry_calibration_state_only_change";
    } else if (!result.projection_geometry_match) {
        result.error = "canvas_projection_geometry_changed";
    }
    return result;
}

inline bool paths_equivalent(const fs::path& left, const fs::path& right)
{
    std::error_code left_error;
    std::error_code right_error;
    const fs::path canonical_left = fs::weakly_canonical(left, left_error);
    const fs::path canonical_right = fs::weakly_canonical(right, right_error);
    return !left_error && !right_error && canonical_left == canonical_right;
}

// A checksum mismatch is relaxed only through an immutable active
// commissioning release that checksum-binds this exact artifact pointer and
// accepted canvas snapshot. There is no unanchored semantic fallback.
inline Result validate_active_artifact_canvas(
    const std::string& rig_id,
    const std::string& canvas_name,
    const std::string& arena_id,
    const std::string& camera_id,
    const std::string& product_key,
    const fs::path& current_canvas_path,
    const std::string& accepted_canvas_sha256,
    const fs::path& artifact_pointer_path,
    const fs::path& commissioning_pointer_path)
{
    Result result;
    result.accepted_canvas_sha256 = accepted_canvas_sha256;
    std::string current_bytes;
    if (!read_file(current_canvas_path, &current_bytes, &result.error)) return result;
    result.current_canvas_sha256 = sha256(current_bytes);
    if (result.current_canvas_sha256 == accepted_canvas_sha256) {
        result = compare_canvas_bytes(current_bytes, current_bytes);
        result.accepted_canvas_sha256 = accepted_canvas_sha256;
        return result;
    }

    std::string pointer_bytes;
    if (!read_file(commissioning_pointer_path, &pointer_bytes, &result.error)) {
        result.error = "active_commissioning_pointer_required_for_canvas_change";
        return result;
    }
    const nlohmann::json commissioning_pointer = nlohmann::json::parse(
        pointer_bytes, nullptr, false);
    if (commissioning_pointer.is_discarded() ||
        !commissioning_pointer.is_object() ||
        commissioning_pointer.value("schema_id", "") !=
            "citrus.calibration.active_rig_canvas_commissioning" ||
        commissioning_pointer.value("schema_version", 0) != 1 ||
        commissioning_pointer.value("status", "") != "accepted" ||
        commissioning_pointer.value("rig_id", "") != rig_id ||
        commissioning_pointer.value("canvas_name", "") != canvas_name) {
        result.error = "active_commissioning_pointer_invalid_for_canvas_change";
        return result;
    }

    const fs::path manifest_path = commissioning_pointer.value(
        "manifest_path", std::string());
    std::string manifest_bytes;
    if (!read_file(manifest_path, &manifest_bytes, &result.error) ||
        sha256(manifest_bytes) !=
            commissioning_pointer.value("manifest_sha256", "")) {
        result.error = "active_commissioning_manifest_invalid_for_canvas_change";
        return result;
    }
    const nlohmann::json manifest = nlohmann::json::parse(
        manifest_bytes, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object() ||
        manifest.value("schema_id", "") !=
            "citrus.calibration.rig_canvas_commissioning_release" ||
        manifest.value("schema_version", 0) != 1 ||
        manifest.value("status", "") != "accepted" ||
        manifest.value("release_id", "") !=
            commissioning_pointer.value("release_id", "") ||
        manifest.value("rig_id", "") != rig_id ||
        manifest.value("canvas_name", "") != canvas_name) {
        result.error = "active_commissioning_manifest_identity_invalid";
        return result;
    }
    result.commissioning_release_id = manifest.value("release_id", "");
    if (!manifest.contains("members") || !manifest.at("members").is_array()) {
        result.error = "commissioning_artifact_members_invalid";
        return result;
    }
    const nlohmann::json* matching_product = nullptr;
    for (const auto& member : manifest.at("members")) {
        if (!member.is_object()) {
            result.error = "commissioning_artifact_member_invalid";
            return result;
        }
        if (member.value("arena_id", "") == arena_id &&
            member.value("camera_id", "") == camera_id &&
            member.contains(product_key) && member.at(product_key).is_object()) {
            if (matching_product != nullptr) {
                result.error = "commissioning_artifact_member_duplicated";
                return result;
            }
            matching_product = &member.at(product_key);
        }
    }
    if (matching_product == nullptr) {
        result.error = "commissioning_artifact_member_missing";
        return result;
    }
    const fs::path recorded_pointer = matching_product->value(
        "active_pointer_path", std::string());
    if (!paths_equivalent(recorded_pointer, artifact_pointer_path)) {
        result.error = "commissioning_artifact_pointer_identity_mismatch";
        return result;
    }
    std::string artifact_pointer_bytes;
    if (!read_file(artifact_pointer_path, &artifact_pointer_bytes, &result.error) ||
        sha256(artifact_pointer_bytes) !=
            matching_product->value("active_pointer_sha256", "")) {
        result.error = "commissioning_artifact_pointer_checksum_mismatch";
        return result;
    }
    if (!manifest.contains("canvas_configuration") ||
        !manifest.at("canvas_configuration").is_object()) {
        result.error = "commissioning_canvas_configuration_invalid";
        return result;
    }
    const auto& canvas = manifest.at("canvas_configuration");
    const fs::path snapshot_path = canvas.value("snapshot_path", std::string());
    std::string snapshot_bytes;
    if (!read_file(snapshot_path, &snapshot_bytes, &result.error) ||
        sha256(snapshot_bytes) != canvas.value("snapshot_sha256", "") ||
        canvas.value("snapshot_sha256", "") != accepted_canvas_sha256) {
        result.error = "commissioning_canvas_snapshot_invalid_for_canvas_change";
        return result;
    }
    result = compare_canvas_bytes(current_bytes, snapshot_bytes);
    result.accepted_canvas_sha256 = accepted_canvas_sha256;
    result.commissioning_release_id = manifest.value("release_id", "");
    return result;
}

}  // namespace orange::gui::spatial_layout::canvas_compatibility
