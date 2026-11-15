#include "gui/spatial_layout/physical_registration_selection.h"
#include "outputs/common/atomic_file.h"

#include "dish_top_rim_observation.h"
#include "fnv1a64_fingerprint.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
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
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return orange::outputs::write_json_atomic(path, value, {}, error);
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

bool known_selection_policy(const std::string& policy)
{
    return policy == kSelectionPolicyAcceptedWithGroupFit ||
        policy == kSelectionPolicyExplicitOperatorSelection ||
        policy == kSelectionPolicyExplicitOperatorReuse;
}

std::string current_utc_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    return format_utc_timestamp_seconds(static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count()));
}

// Evaluates a revalidated selection's freshness; returns false (with the
// reason in *error_out) when the pointer's own record is inconsistent.
bool evaluate_freshness(
    const nlohmann::json& pointer,
    const PhysicalRegistrationArtifactCandidate& candidate,
    const std::string& evaluated_at_utc,
    nlohmann::json* freshness_out,
    bool* fresh_out,
    std::string* error_out)
{
    const int version = pointer.value("schema_version", 0);
    // Version 1 pointers predate reuse: their window starts at the fit.
    const std::string policy = version == kActivePhysicalRegistrationPointerReadableV1
        ? std::string(kSelectionPolicyExplicitOperatorSelection)
        : pointer.value("selection_policy", std::string());
    if (!known_selection_policy(policy)) {
        if (error_out) *error_out = "active_pointer_selection_policy_unknown:" + policy;
        return false;
    }
    const std::string selected_at = pointer.value("selected_at_utc", std::string());
    std::string valid_from;
    std::string valid_until;
    std::string window_error;
    if (!physical_registration_freshness_window(
            candidate.created_utc, policy, selected_at,
            &valid_from, &valid_until, &window_error)) {
        if (error_out) *error_out = window_error;
        return false;
    }
    if (version != kActivePhysicalRegistrationPointerReadableV1) {
        const nlohmann::json recorded = pointer.value("freshness", nlohmann::json::object());
        if (!recorded.is_object() ||
            recorded.value("rule", std::string()) != kPhysicalRegistrationFreshnessRule ||
            recorded.value("fit_created_utc", std::string()) != candidate.created_utc ||
            recorded.value("valid_from_utc", std::string()) != valid_from ||
            recorded.value("valid_until_utc", std::string()) != valid_until) {
            if (error_out) *error_out = "active_pointer_freshness_record_mismatch";
            return false;
        }
    }
    long long until_s = 0;
    long long evaluated_s = 0;
    parse_utc_timestamp_seconds(valid_until, &until_s);
    if (!parse_utc_timestamp_seconds(evaluated_at_utc, &evaluated_s)) {
        if (error_out) *error_out = "freshness_evaluation_time_unparseable:" + evaluated_at_utc;
        return false;
    }
    const bool fresh = evaluated_s < until_s;
    *fresh_out = fresh;
    *freshness_out = {
        {"rule", kPhysicalRegistrationFreshnessRule},
        {"selection_policy", policy},
        {"fit_created_utc", candidate.created_utc},
        {"valid_from_utc", valid_from},
        {"valid_until_utc", valid_until},
        {"evaluated_at_utc", evaluated_at_utc},
        {"state", fresh ? "fresh" : "expired"},
    };
    return true;
}

}  // namespace

bool parse_utc_timestamp_seconds(const std::string& utc, long long* seconds_out)
{
    // YYYY-MM-DDTHH:MM:SS, optional .fraction, then Z.
    if (utc.size() < 20 || utc.back() != 'Z') return false;
    const auto digits = [&](std::size_t at, std::size_t count, int* out) {
        if (at + count > utc.size()) return false;
        int value = 0;
        for (std::size_t index = at; index < at + count; ++index) {
            if (!std::isdigit(static_cast<unsigned char>(utc[index]))) return false;
            value = value * 10 + (utc[index] - '0');
        }
        *out = value;
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!digits(0, 4, &year) || utc[4] != '-' || !digits(5, 2, &month) ||
        utc[7] != '-' || !digits(8, 2, &day) || utc[10] != 'T' ||
        !digits(11, 2, &hour) || utc[13] != ':' || !digits(14, 2, &minute) ||
        utc[16] != ':' || !digits(17, 2, &second)) {
        return false;
    }
    if (utc.size() != 20) {
        if (utc[19] != '.' || utc.size() < 22) return false;
        for (std::size_t index = 20; index + 1 < utc.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(utc[index]))) return false;
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const time_t seconds = ::timegm(&tm);
    if (seconds == static_cast<time_t>(-1)) return false;
    if (seconds_out) *seconds_out = static_cast<long long>(seconds);
    return true;
}

std::string format_utc_timestamp_seconds(long long seconds)
{
    const time_t value = static_cast<time_t>(seconds);
    std::tm tm{};
    ::gmtime_r(&value, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool physical_registration_freshness_window(
    const std::string& fit_created_utc,
    const std::string& policy,
    const std::string& selected_at_utc,
    std::string* valid_from_utc_out,
    std::string* valid_until_utc_out,
    std::string* error_out)
{
    if (!known_selection_policy(policy)) {
        if (error_out) *error_out = "selection_policy_unknown:" + policy;
        return false;
    }
    long long fit_s = 0;
    if (!parse_utc_timestamp_seconds(fit_created_utc, &fit_s)) {
        if (error_out) *error_out = "fit_created_utc_unparseable:" + fit_created_utc;
        return false;
    }
    long long from_s = fit_s;
    if (policy == kSelectionPolicyExplicitOperatorReuse) {
        if (!parse_utc_timestamp_seconds(selected_at_utc, &from_s)) {
            if (error_out) *error_out = "selected_at_utc_unparseable:" + selected_at_utc;
            return false;
        }
    }
    if (valid_from_utc_out) *valid_from_utc_out = format_utc_timestamp_seconds(from_s);
    if (valid_until_utc_out) {
        *valid_until_utc_out = format_utc_timestamp_seconds(
            from_s + kPhysicalRegistrationFreshnessSeconds);
    }
    return true;
}

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
    return resolve_active_physical_registration(
        calibration_base_dir, camera_serial, expected_width_px,
        expected_height_px, expected_pixel_format, current_utc_timestamp());
}

PhysicalRegistrationSelectionResolution resolve_active_physical_registration(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format,
    const std::string& evaluated_at_utc)
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
        const int pointer_version = result.pointer.value("schema_version", 0);
        if (result.pointer.value("schema_id", std::string()) !=
                kActivePhysicalRegistrationPointerSchemaId ||
            (pointer_version != kActivePhysicalRegistrationPointerSchemaVersion &&
             pointer_version != kActivePhysicalRegistrationPointerReadableV1) ||
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
        bool fresh = false;
        std::string freshness_error;
        if (!evaluate_freshness(result.pointer, result.candidate, evaluated_at_utc,
                                &result.freshness, &fresh, &freshness_error)) {
            result.status = "invalid_pointer";
            result.error = freshness_error;
            return result;
        }
        if (!fresh) {
            // Never silently used: an expired selection is invalid until the
            // operator registers again or explicitly reuses it.
            result.status = "invalid_selected";
            result.error = "selection_expired:valid_until=" +
                result.freshness.value("valid_until_utc", std::string());
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
    const PhysicalRegistrationSelectionRequest& request,
    std::string* error_out)
{
    const std::string& selected_at_utc = request.selected_at_utc;
    if (!known_selection_policy(request.policy)) {
        if (error_out) *error_out = "Unknown physical registration selection policy: " + request.policy;
        return false;
    }
    if (request.policy == kSelectionPolicyAcceptedWithGroupFit &&
        request.group_transaction_id.empty()) {
        if (error_out) *error_out = "A group-accepted selection needs its group transaction id.";
        return false;
    }
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
    std::string valid_from;
    std::string valid_until;
    std::string window_error;
    if (!physical_registration_freshness_window(
            validated.created_utc, request.policy, selected_at_utc,
            &valid_from, &valid_until, &window_error)) {
        if (error_out) *error_out = "Physical registration freshness: " + window_error;
        return false;
    }
    long long fit_s = 0;
    long long selected_s = 0;
    parse_utc_timestamp_seconds(validated.created_utc, &fit_s);
    if (!parse_utc_timestamp_seconds(selected_at_utc, &selected_s)) {
        if (error_out) *error_out = "Selection time does not parse: " + selected_at_utc;
        return false;
    }
    const long long fit_age_s = selected_s - fit_s;
    const bool fit_inside_window = fit_age_s < kPhysicalRegistrationFreshnessSeconds;
    if (request.policy == kSelectionPolicyExplicitOperatorReuse && fit_inside_window) {
        if (error_out) {
            *error_out = "This fit is still inside its one-day window; select it normally, not as a reuse.";
        }
        return false;
    }
    if (request.policy != kSelectionPolicyExplicitOperatorReuse && !fit_inside_window) {
        if (error_out) {
            *error_out = "This fit is older than one day; register again, or reuse it explicitly.";
        }
        return false;
    }
    nlohmann::json freshness = {
        {"rule", kPhysicalRegistrationFreshnessRule},
        {"fit_created_utc", validated.created_utc},
        {"fit_age_seconds_at_selection", fit_age_s},
        {"valid_from_utc", valid_from},
        {"valid_until_utc", valid_until},
    };
    nlohmann::json pointer = {
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
        {"selection_policy", request.policy},
        {"freshness", std::move(freshness)},
    };
    if (request.policy == kSelectionPolicyAcceptedWithGroupFit) {
        pointer["group_transaction_id"] = request.group_transaction_id;
    }
    return write_json_atomically(
        active_physical_registration_pointer_path(
            calibration_base_dir, validated.camera_serial),
        pointer, error_out);
}

bool clear_active_physical_registration(
    const fs::path& calibration_base_dir,
    const std::string& camera_serial,
    const std::string& cleared_at_utc,
    std::string* error_out,
    const std::string& policy,
    const std::string& group_transaction_id)
{
    if (camera_serial.empty()) {
        if (error_out) *error_out = "Camera serial is required to clear selection.";
        return false;
    }
    if (policy != kClearPolicyExplicitOperatorClear &&
        policy != kClearPolicyFitNotAcceptedInGroup) {
        if (error_out) *error_out = "Unknown physical registration clear policy: " + policy;
        return false;
    }
    if (policy == kClearPolicyFitNotAcceptedInGroup && group_transaction_id.empty()) {
        if (error_out) *error_out = "A group clear needs its group transaction id.";
        return false;
    }
    nlohmann::json pointer = {
        {"schema_id", kActivePhysicalRegistrationPointerSchemaId},
        {"schema_version", kActivePhysicalRegistrationPointerSchemaVersion},
        {"status", "cleared"},
        {"camera_serial", camera_serial},
        {"cleared_at_utc", cleared_at_utc},
        {"selection", nullptr},
        {"selection_policy", policy},
    };
    if (!group_transaction_id.empty()) pointer["group_transaction_id"] = group_transaction_id;
    return write_json_atomically(
        active_physical_registration_pointer_path(
            calibration_base_dir, camera_serial),
        pointer, error_out);
}

bool apply_physical_registration_group_acceptance(
    const fs::path& calibration_base_dir,
    const std::string& group_transaction_id,
    const std::vector<PhysicalRegistrationArtifactCandidate>& accepted,
    const std::vector<std::string>& not_accepted_camera_serials,
    const std::string& accepted_at_utc,
    std::vector<PhysicalRegistrationGroupAcceptanceResult>* results_out,
    std::string* error_out)
{
    if (group_transaction_id.empty()) {
        if (error_out) *error_out = "group_transaction_id_missing";
        return false;
    }
    std::vector<PhysicalRegistrationGroupAcceptanceResult> results;
    std::vector<std::string> failures;
    const auto pointer_sha = [&](const std::string& serial) {
        std::string sha;
        std::string ignored;
        checksum::file_sha256(
            active_physical_registration_pointer_path(calibration_base_dir, serial),
            &sha, &ignored);
        return sha;
    };
    for (const auto& candidate : accepted) {
        PhysicalRegistrationGroupAcceptanceResult result;
        result.camera_serial = candidate.camera_serial;
        result.action = "selected";
        result.artifact_id = candidate.artifact_id;
        result.observation_sha256 = candidate.observation_sha256;
        PhysicalRegistrationSelectionRequest request;
        request.selected_at_utc = accepted_at_utc;
        request.policy = kSelectionPolicyAcceptedWithGroupFit;
        request.group_transaction_id = group_transaction_id;
        if (!select_physical_registration_artifact(
                calibration_base_dir, candidate, request, &result.error)) {
            failures.push_back(candidate.camera_serial + ": " + result.error);
        } else {
            result.pointer_sha256 = pointer_sha(candidate.camera_serial);
        }
        results.push_back(std::move(result));
    }
    for (const auto& serial : not_accepted_camera_serials) {
        PhysicalRegistrationGroupAcceptanceResult result;
        result.camera_serial = serial;
        result.action = "cleared";
        if (!clear_active_physical_registration(
                calibration_base_dir, serial, accepted_at_utc, &result.error,
                kClearPolicyFitNotAcceptedInGroup, group_transaction_id)) {
            failures.push_back(serial + ": " + result.error);
        } else {
            result.pointer_sha256 = pointer_sha(serial);
        }
        results.push_back(std::move(result));
    }
    if (results_out) *results_out = std::move(results);
    if (!failures.empty()) {
        if (error_out) {
            std::string joined = "physical_registration_group_selection_failed:";
            for (const auto& failure : failures) joined += " [" + failure + "]";
            *error_out = joined;
        }
        return false;
    }
    return true;
}

}  // namespace orange::gui::spatial_layout
