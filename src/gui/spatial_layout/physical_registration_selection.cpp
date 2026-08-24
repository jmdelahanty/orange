#include "gui/spatial_layout/physical_registration_selection.h"

#include "dish_top_rim_observation.h"
#include "fnv1a64_fingerprint.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <unistd.h>

namespace orange::gui::spatial_layout {
namespace {

namespace fs = std::filesystem;

std::string sanitize_component(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        out.push_back(
            (std::isalnum(c) || c == '-' || c == '_')
                ? static_cast<char>(c)
                : '_');
    }
    return out.empty() ? "unknown" : out;
}

bool read_json(const fs::path& path, nlohmann::json* out, std::string* error)
{
    if (out == nullptr) return false;
    std::ifstream stream(path);
    if (!stream) {
        if (error) *error = "Cannot open JSON file: " + path.string();
        return false;
    }
    try {
        stream >> *out;
    } catch (const std::exception& exception) {
        if (error) {
            *error = "Cannot parse JSON file " + path.string() + ": " +
                exception.what();
        }
        return false;
    }
    return true;
}

bool write_json_atomically(
    const fs::path& path,
    const nlohmann::json& value,
    std::string* error)
{
    try {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        fs::create_directories(path.parent_path());
        const fs::path temporary = path.string() + ".tmp." +
            std::to_string(static_cast<long long>(::getpid()));
        {
            std::ofstream stream(temporary, std::ios::trunc);
            if (!stream) {
                if (error) {
                    *error = "Cannot create temporary active pointer: " +
                        temporary.string();
                }
                return false;
            }
            stream << std::setw(2) << value << '\n';
            stream.flush();
            if (!stream) {
                if (error) {
                    *error = "Failed writing temporary active pointer: " +
                        temporary.string();
                }
                return false;
            }
        }
        std::error_code rename_error;
        fs::rename(temporary, path, rename_error);
        if (rename_error) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            if (error) {
                *error = "Failed to publish active pointer " + path.string() +
                    ": " + rename_error.message();
            }
            return false;
        }
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
    return true;
}

bool path_is_within(const fs::path& child, const fs::path& parent)
{
    std::error_code error;
    const fs::path canonical_child = fs::weakly_canonical(child, error);
    if (error) return false;
    const fs::path canonical_parent = fs::weakly_canonical(parent, error);
    if (error) return false;
    auto child_it = canonical_child.begin();
    for (auto parent_it = canonical_parent.begin();
         parent_it != canonical_parent.end(); ++parent_it, ++child_it) {
        if (child_it == canonical_child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return true;
}

bool paths_equal_weakly(const fs::path& left, const fs::path& right)
{
    std::error_code left_error;
    const fs::path canonical_left = fs::weakly_canonical(left, left_error);
    if (left_error) return false;
    std::error_code right_error;
    const fs::path canonical_right = fs::weakly_canonical(right, right_error);
    return !right_error && canonical_left == canonical_right;
}

nlohmann::json object_member(
    const nlohmann::json& parent,
    const char* key)
{
    if (!parent.is_object()) return nlohmann::json::object();
    const auto it = parent.find(key);
    return it != parent.end() && it->is_object()
        ? *it
        : nlohmann::json::object();
}

bool finite_positive(const nlohmann::json& object, const char* key, double* out)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) return false;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value <= 0.0) return false;
    if (out) *out = value;
    return true;
}

bool file_fnv1a64(
    const fs::path& path,
    std::string* fingerprint_out,
    std::string* error_out)
{
    if (fingerprint_out == nullptr) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error_out) *error_out = "cannot_open_artifact_file:" + path.string();
        return false;
    }
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    std::array<char, 1024u * 1024u> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
            hash *= kPrime;
        }
    }
    if (!stream.eof()) {
        if (error_out) *error_out = "cannot_read_artifact_file:" + path.string();
        return false;
    }
    *fingerprint_out = orange::calibration::format_fnv1a64_fingerprint(hash);
    return true;
}

bool validate_completed_artifact(
    const fs::path& sessions_root,
    PhysicalRegistrationArtifactCandidate* candidate,
    std::string* error_out)
{
    if (candidate == nullptr) return false;
    const fs::path artifact_dir = candidate->observation_path.parent_path();
    candidate->manifest_path = artifact_dir / "manifest.json";
    if (!path_is_within(artifact_dir, sessions_root) ||
        !fs::is_regular_file(candidate->manifest_path)) {
        if (error_out) *error_out = "artifact_completion_manifest_missing";
        return false;
    }
    nlohmann::json manifest;
    if (!read_json(candidate->manifest_path, &manifest, error_out)) return false;
    if (!manifest.is_object()) {
        if (error_out) *error_out = "artifact_completion_manifest_not_object";
        return false;
    }
    if (manifest.value("schema_id", std::string()) !=
            orange::calibration::kCalibrationManifestSchemaId ||
        manifest.value("schema_version", 0) !=
            orange::calibration::kCalibrationManifestSchemaVersion ||
        manifest.value("artifact_id", std::string()) != candidate->artifact_id ||
        manifest.value("artifact_schema_id", std::string()) !=
            orange::calibration::kDishTopRimObservationSchemaId ||
        manifest.value("artifact_schema_version", 0) !=
            orange::calibration::kDishTopRimObservationSchemaVersion) {
        if (error_out) *error_out = "artifact_completion_manifest_identity_mismatch";
        return false;
    }
    const nlohmann::json files = object_member(manifest, "files");
    const nlohmann::json checksums = object_member(manifest, "checksums");
    if (checksums.value("algorithm", std::string()) !=
            orange::calibration::kCalibrationFingerprintAlgorithm) {
        if (error_out) *error_out = "artifact_checksum_algorithm_mismatch";
        return false;
    }
    const std::array<std::pair<const char*, const char*>, 4> required = {{
        {"source_frame", "source_frame"},
        {"review_overlay", "review_overlay"},
        {"registration_hough_overlay", "registration_hough_overlay"},
        {"valid_detection_overlay", "valid_detection_overlay"},
    }};
    for (const auto& [file_key, checksum_key] : required) {
        const std::string relative_path = files.value(file_key, std::string());
        const std::string expected_checksum = checksums.value(
            checksum_key, std::string());
        if (relative_path.empty() || expected_checksum.empty()) {
            if (error_out) {
                *error_out = std::string("artifact_required_file_or_checksum_missing:") +
                    file_key;
            }
            return false;
        }
        const fs::path resolved = artifact_dir / relative_path;
        if (!path_is_within(resolved, artifact_dir) ||
            !fs::is_regular_file(resolved)) {
            if (error_out) {
                *error_out = std::string("artifact_required_file_missing:") + file_key;
            }
            return false;
        }
        std::string actual_checksum;
        if (!file_fnv1a64(resolved, &actual_checksum, error_out) ||
            actual_checksum != expected_checksum) {
            if (error_out && error_out->empty()) {
                *error_out = std::string("artifact_required_file_checksum_mismatch:") +
                    file_key;
            } else if (error_out && actual_checksum != expected_checksum) {
                *error_out = std::string("artifact_required_file_checksum_mismatch:") +
                    file_key;
            }
            return false;
        }
    }
    static constexpr const char* compact_required[] = {
        "image_set_json",
        "spatial_dish_mask_runtime_v1",
        "palette_dish_mask_v2",
    };
    for (const char* file_key : compact_required) {
        const std::string relative_path = files.value(
            file_key, std::string());
        const fs::path resolved = artifact_dir / relative_path;
        if (relative_path.empty() || !path_is_within(resolved, artifact_dir) ||
            !fs::is_regular_file(resolved)) {
            if (error_out) {
                *error_out = std::string(
                    "artifact_required_compact_file_missing:") + file_key;
            }
            return false;
        }
    }
    const std::string observation_relative = files.value(
        "observation_json", std::string());
    if (observation_relative.empty() ||
        !paths_equal_weakly(
            artifact_dir / observation_relative,
            candidate->observation_path)) {
        if (error_out) *error_out = "artifact_observation_manifest_path_mismatch";
        return false;
    }
    if (!checksum::file_sha256(
            candidate->manifest_path, &candidate->manifest_sha256, error_out)) {
        return false;
    }
    return true;
}

PhysicalRegistrationArtifactCandidate parse_candidate_unchecked(
    const fs::path& sessions_root,
    const fs::path& path,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format)
{
    PhysicalRegistrationArtifactCandidate candidate;
    candidate.observation_path = path;
    if (!path_is_within(path, sessions_root)) {
        candidate.compatibility_reason = "observation_path_outside_sessions_root";
        return candidate;
    }
    std::string error;
    if (!read_json(path, &candidate.observation, &error)) {
        candidate.compatibility_reason = error;
        return candidate;
    }
    const auto& observation = candidate.observation;
    if (!observation.is_object()) {
        candidate.compatibility_reason = "observation_not_object";
        return candidate;
    }
    if (observation.value("schema_id", std::string()) !=
            orange::calibration::kDishTopRimObservationSchemaId ||
        observation.value("schema_version", 0) !=
            orange::calibration::kDishTopRimObservationSchemaVersion) {
        candidate.compatibility_reason = "unsupported_observation_schema";
        return candidate;
    }
    const nlohmann::json physical = object_member(
        object_member(observation, "registration_products"),
        "physical_registration");
    if (physical.value("product_id", std::string()) !=
            orange::calibration::kDailyPhysicalDishRegistrationProductId ||
        physical.value("status", std::string()) != "accepted" ||
        physical.value("coordinate_space", std::string()) !=
            "camera_native_pixels") {
        candidate.compatibility_reason = "physical_registration_not_accepted";
        return candidate;
    }
    const nlohmann::json camera = object_member(observation, "camera");
    candidate.camera_serial = camera.value("serial", std::string());
    candidate.width_px = camera.value("width", 0);
    candidate.height_px = camera.value("height", 0);
    candidate.pixel_format = camera.value("pixel_format", std::string());
    candidate.artifact_id = observation.value("artifact_id", std::string());
    candidate.created_utc = observation.value("created_utc", std::string());
    const nlohmann::json capture = object_member(observation, "capture");
    candidate.dish_fill_state = capture.value("dish_fill_state", "unknown");
    candidate.physical_state_summary =
        "fill=" + candidate.dish_fill_state +
        ", projector=" + capture.value("projector_state", "unknown") +
        ", filter=" + capture.value("filter_state", "unknown");
    const nlohmann::json accepted = object_member(
        observation, "accepted_inner_rim_boundary");
    candidate.operator_confirmed = accepted.value("operator_confirmed", false);
    const nlohmann::json geometry = object_member(accepted, "geometry");
    const nlohmann::json center = object_member(geometry, "center_px");
    candidate.accepted_center_x_px = center.value("x", 0.0);
    candidate.accepted_center_y_px = center.value("y", 0.0);
    finite_positive(geometry, "radius_px", &candidate.accepted_radius_px);
    candidate.centroid_gate_outset_px = object_member(
        observation, "valid_detection_region").value(
            "centroid_gate_outset_px", 0.0);
    const nlohmann::json accepted_mask = object_member(
        observation, "accepted_mask");
    const nlohmann::json accepted_mask_center = object_member(
        accepted_mask, "center_px");
    const nlohmann::json valid_region = object_member(
        observation, "valid_detection_region");
    const nlohmann::json valid_geometry = object_member(
        valid_region, "geometry");
    const nlohmann::json valid_center = object_member(
        valid_geometry, "center_px");
    const auto finite_value = [](const nlohmann::json& value,
                                 const char* key,
                                 double* out) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_number()) return false;
        const double parsed = it->get<double>();
        if (!std::isfinite(parsed)) return false;
        if (out) *out = parsed;
        return true;
    };
    double mask_x = 0.0;
    double mask_y = 0.0;
    double mask_radius = 0.0;
    double valid_x = 0.0;
    double valid_y = 0.0;
    double valid_radius = 0.0;
    const bool derived_geometry_valid =
        accepted.value("target_plane", std::string()) == "dish_top_rim" &&
        accepted_mask.value("shape", std::string()) == "circle" &&
        accepted_mask.value("coordinate_space", std::string()) ==
            "camera_native_pixels" &&
        finite_value(accepted_mask_center, "x", &mask_x) &&
        finite_value(accepted_mask_center, "y", &mask_y) &&
        finite_value(accepted_mask, "radius_px", &mask_radius) &&
        valid_region.value("coordinate_space", std::string()) ==
            "camera_native_pixels" &&
        valid_region.value("purpose", std::string()) ==
            "bounding_box_centroid_detection_gating" &&
        (valid_region.value("offset_direction", std::string()) == "outward" ||
         valid_region.value("offset_direction", std::string()) == "none") &&
        valid_geometry.value("type", std::string()) == "circle" &&
        finite_value(valid_center, "x", &valid_x) &&
        finite_value(valid_center, "y", &valid_y) &&
        finite_value(valid_geometry, "radius_px", &valid_radius) &&
        object_member(observation, "operator_review").value(
            "accepted", false);
    const auto close = [](const double left, const double right) {
        return std::isfinite(left) && std::isfinite(right) &&
            std::abs(left - right) <= 1e-6;
    };
    if (!checksum::file_sha256(path, &candidate.observation_sha256, &error)) {
        candidate.compatibility_reason = error;
        return candidate;
    }
    if (!validate_completed_artifact(sessions_root, &candidate, &error)) {
        candidate.compatibility_reason = error;
        return candidate;
    }

    if (candidate.artifact_id.empty() || candidate.camera_serial.empty()) {
        candidate.compatibility_reason = "observation_identity_missing";
    } else if (candidate.camera_serial != camera_serial) {
        candidate.compatibility_reason = "camera_serial_mismatch";
    } else if (candidate.width_px <= 0 || candidate.height_px <= 0) {
        candidate.compatibility_reason = "native_raster_missing";
    } else if ((expected_width_px > 0 &&
                candidate.width_px != expected_width_px) ||
               (expected_height_px > 0 &&
                candidate.height_px != expected_height_px)) {
        candidate.compatibility_reason = "native_raster_mismatch";
    } else if (!expected_pixel_format.empty() &&
               candidate.pixel_format != expected_pixel_format) {
        candidate.compatibility_reason = "pixel_format_mismatch";
    } else if (!candidate.operator_confirmed ||
               accepted.value("coordinate_space", std::string()) !=
                   "camera_native_pixels" ||
               geometry.value("type", std::string()) != "circle" ||
               !std::isfinite(candidate.accepted_center_x_px) ||
               !std::isfinite(candidate.accepted_center_y_px) ||
               !std::isfinite(candidate.accepted_radius_px) ||
               candidate.accepted_center_x_px < 0.0 ||
               candidate.accepted_center_x_px >= candidate.width_px ||
               candidate.accepted_center_y_px < 0.0 ||
               candidate.accepted_center_y_px >= candidate.height_px ||
               candidate.accepted_radius_px <= 0.0 ||
               !std::isfinite(candidate.centroid_gate_outset_px) ||
               candidate.centroid_gate_outset_px < 0.0 ||
               !derived_geometry_valid || mask_radius <= 0.0 ||
               !close(mask_x, candidate.accepted_center_x_px) ||
               !close(mask_y, candidate.accepted_center_y_px) ||
               !close(valid_x, candidate.accepted_center_x_px) ||
               !close(valid_y, candidate.accepted_center_y_px) ||
               !close(mask_radius, valid_radius) ||
               !close(mask_radius,
                      candidate.accepted_radius_px +
                          candidate.centroid_gate_outset_px)) {
        candidate.compatibility_reason = "accepted_inner_rim_invalid";
    } else {
        candidate.compatible = true;
        candidate.compatibility_reason = "compatible";
    }
    return candidate;
}

PhysicalRegistrationArtifactCandidate parse_candidate(
    const fs::path& sessions_root,
    const fs::path& path,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format)
{
    try {
        return parse_candidate_unchecked(
            sessions_root,
            path,
            camera_serial,
            expected_width_px,
            expected_height_px,
            expected_pixel_format);
    } catch (const std::exception& exception) {
        PhysicalRegistrationArtifactCandidate candidate;
        candidate.observation_path = path;
        candidate.compatibility_reason =
            "malformed_observation_or_manifest:" +
            std::string(exception.what());
        return candidate;
    }
}

}  // namespace

fs::path active_physical_registration_pointer_path(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial)
{
    return calibration_base_dir / "active" /
        "physical_dish_registration" /
        ("Cam" + sanitize_component(camera_serial) + ".json");
}

std::vector<PhysicalRegistrationArtifactCandidate>
discover_physical_registration_artifacts(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format,
    std::vector<std::string>* warnings_out)
{
    std::vector<PhysicalRegistrationArtifactCandidate> candidates;
    const fs::path sessions_root = calibration_base_dir / "sessions";
    std::error_code error;
    if (!fs::is_directory(sessions_root, error)) return candidates;
    fs::recursive_directory_iterator iterator(
        sessions_root,
        fs::directory_options::skip_permission_denied,
        error);
    const fs::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const fs::directory_entry entry = *iterator;
        iterator.increment(error);
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error ||
            entry.path().filename() != "observation.json") {
            continue;
        }
        PhysicalRegistrationArtifactCandidate candidate = parse_candidate(
            sessions_root, entry.path(), camera_serial,
            expected_width_px, expected_height_px, expected_pixel_format);
        if (candidate.camera_serial == camera_serial &&
            !candidate.artifact_id.empty()) {
            candidates.push_back(std::move(candidate));
        } else if (warnings_out != nullptr &&
                   !candidate.compatibility_reason.empty() &&
                   candidate.compatibility_reason !=
                       "unsupported_observation_schema") {
            warnings_out->push_back(
                entry.path().string() + ": " +
                candidate.compatibility_reason);
        }
    }
    if (error && warnings_out != nullptr) {
        warnings_out->push_back(
            "Calibration discovery stopped early: " + error.message());
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& left, const auto& right) {
            if (left.created_utc != right.created_utc) {
                return left.created_utc > right.created_utc;
            }
            return left.artifact_id > right.artifact_id;
        });
    return candidates;
}

PhysicalRegistrationArtifactCandidate validate_physical_registration_artifact(
    const fs::path& calibration_base_dir,
    const fs::path& observation_path,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format)
{
    return parse_candidate(
        calibration_base_dir / "sessions",
        observation_path,
        camera_serial,
        expected_width_px,
        expected_height_px,
        expected_pixel_format);
}

PhysicalRegistrationSelectionResolution resolve_active_physical_registration(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format)
{
    PhysicalRegistrationSelectionResolution result;
    const fs::path pointer_path = active_physical_registration_pointer_path(
        calibration_base_dir, camera_serial);
    result.pointer_path = pointer_path;
    std::error_code exists_error;
    result.pointer_exists = fs::is_regular_file(pointer_path, exists_error);
    if (!result.pointer_exists) return result;
    if (!checksum::file_sha256(
            pointer_path, &result.pointer_sha256, &result.error)) {
        result.status = "invalid_pointer";
        return result;
    }
    if (!read_json(pointer_path, &result.pointer, &result.error)) {
        result.status = "invalid_pointer";
        return result;
    }
    if (!result.pointer.is_object()) {
        result.status = "invalid_pointer";
        result.error = "active_pointer_not_object";
        return result;
    }
    try {
        if (result.pointer.value("schema_id", std::string()) !=
                kActivePhysicalRegistrationPointerSchemaId ||
            result.pointer.value("schema_version", 0) !=
                kActivePhysicalRegistrationPointerSchemaVersion ||
            result.pointer.value("camera_serial", std::string()) !=
                camera_serial) {
            result.status = "invalid_pointer";
            result.error = "active_pointer_identity_or_schema_mismatch";
            return result;
        }
        if (result.pointer.value("status", std::string()) == "cleared") {
            result.status = "not_selected";
            return result;
        }
        if (result.pointer.value("status", std::string()) != "selected") {
            result.status = "invalid_pointer";
            result.error = "active_pointer_status_invalid";
            return result;
        }
        const auto selection_it = result.pointer.find("selection");
        if (selection_it == result.pointer.end() ||
            !selection_it->is_object()) {
            result.status = "invalid_pointer";
            result.error =
                "active_pointer_selection_missing_or_not_object";
            return result;
        }
        result.selected = true;
        const nlohmann::json selection = *selection_it;
        const fs::path observation_path = selection.value(
            "observation_path", std::string());
        const std::string expected_sha = selection.value(
            "observation_sha256", std::string());
        const fs::path expected_manifest_path = selection.value(
            "manifest_path", std::string());
        const std::string expected_manifest_sha = selection.value(
            "manifest_sha256", std::string());
        result.candidate = parse_candidate(
            calibration_base_dir / "sessions", observation_path,
            camera_serial, expected_width_px, expected_height_px,
            expected_pixel_format);
        if (!result.candidate.compatible) {
            result.status = "invalid_selected";
            result.error = result.candidate.compatibility_reason;
            return result;
        }
        if (expected_sha.empty() ||
            expected_sha != result.candidate.observation_sha256 ||
            selection.value("artifact_id", std::string()) !=
                result.candidate.artifact_id ||
            expected_manifest_path.empty() || expected_manifest_sha.empty() ||
            !paths_equal_weakly(
                expected_manifest_path,
                result.candidate.manifest_path) ||
            expected_manifest_sha != result.candidate.manifest_sha256) {
            result.status = "invalid_selected";
            result.error =
                "selected_observation_or_manifest_digest_or_identity_mismatch";
            return result;
        }
        result.valid = true;
        result.status = "selected";
        return result;
    } catch (const std::exception& exception) {
        result.selected = false;
        result.valid = false;
        result.status = "invalid_pointer";
        result.error = "malformed_active_pointer:" +
            std::string(exception.what());
        return result;
    }
}

bool select_physical_registration_artifact(
    const fs::path& calibration_base_dir,
    const PhysicalRegistrationArtifactCandidate& candidate,
    const std::string& selected_at_utc,
    std::string* error_out)
{
    if (!candidate.compatible || candidate.camera_serial.empty() ||
        candidate.artifact_id.empty() ||
        candidate.observation_sha256.empty()) {
        if (error_out) {
            *error_out = "Only a validated compatible physical registration may be selected.";
        }
        return false;
    }
    const PhysicalRegistrationArtifactCandidate validated = parse_candidate(
        calibration_base_dir / "sessions",
        candidate.observation_path,
        candidate.camera_serial,
        candidate.width_px,
        candidate.height_px,
        candidate.pixel_format);
    if (!validated.compatible ||
        validated.artifact_id != candidate.artifact_id ||
        validated.observation_sha256 != candidate.observation_sha256) {
        if (error_out) {
            *error_out = validated.compatible
                ? "The physical registration changed after review; refresh before selecting."
                : "Physical registration revalidation failed: " +
                    validated.compatibility_reason;
        }
        return false;
    }
    const nlohmann::json pointer = {
        {"schema_id", kActivePhysicalRegistrationPointerSchemaId},
        {"schema_version", kActivePhysicalRegistrationPointerSchemaVersion},
        {"status", "selected"},
        {"camera_serial", validated.camera_serial},
        {"selected_at_utc", selected_at_utc},
        {"selection", {
            {"artifact_id", validated.artifact_id},
            {"artifact_schema_id",
             orange::calibration::kDishTopRimObservationSchemaId},
            {"artifact_schema_version",
             orange::calibration::kDishTopRimObservationSchemaVersion},
            {"observation_path",
             fs::absolute(validated.observation_path).lexically_normal().string()},
            {"observation_sha256", validated.observation_sha256},
            {"manifest_path",
             fs::absolute(validated.manifest_path).lexically_normal().string()},
            {"manifest_sha256", validated.manifest_sha256},
        }},
        {"compatibility", {
            {"camera_serial", validated.camera_serial},
            {"width_px", validated.width_px},
            {"height_px", validated.height_px},
            {"pixel_format", validated.pixel_format},
            {"coordinate_space", "camera_native_pixels"},
            {"physical_target", "dish_top_rim"},
        }},
        {"selection_policy", "explicit_operator_selection"},
    };
    return write_json_atomically(
        active_physical_registration_pointer_path(
            calibration_base_dir, validated.camera_serial),
        pointer, error_out);
}

bool clear_active_physical_registration(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial,
    const std::string& cleared_at_utc,
    std::string* error_out)
{
    if (camera_serial.empty()) {
        if (error_out) *error_out = "Camera serial is required to clear selection.";
        return false;
    }
    const nlohmann::json pointer = {
        {"schema_id", kActivePhysicalRegistrationPointerSchemaId},
        {"schema_version", kActivePhysicalRegistrationPointerSchemaVersion},
        {"status", "cleared"},
        {"camera_serial", camera_serial},
        {"cleared_at_utc", cleared_at_utc},
        {"selection", nullptr},
        {"selection_policy", "explicit_operator_clear"},
    };
    return write_json_atomically(
        active_physical_registration_pointer_path(
            calibration_base_dir, camera_serial),
        pointer, error_out);
}

}  // namespace orange::gui::spatial_layout
