#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

inline constexpr const char* kActivePhysicalRegistrationPointerSchemaId =
    "orange.calibration.active_physical_registration_pointer";
inline constexpr int kActivePhysicalRegistrationPointerSchemaVersion = 1;

struct PhysicalRegistrationArtifactCandidate {
    std::string artifact_id;
    std::string camera_serial;
    std::string created_utc;
    std::filesystem::path observation_path;
    std::string observation_sha256;
    std::filesystem::path manifest_path;
    std::string manifest_sha256;
    std::string dish_fill_state;
    std::string physical_state_summary;
    std::string pixel_format;
    int width_px = 0;
    int height_px = 0;
    double accepted_center_x_px = 0.0;
    double accepted_center_y_px = 0.0;
    double accepted_radius_px = 0.0;
    double centroid_gate_outset_px = 0.0;
    bool operator_confirmed = false;
    bool compatible = false;
    std::string compatibility_reason;
    nlohmann::json observation = nlohmann::json::object();
};

struct PhysicalRegistrationSelectionResolution {
    bool pointer_exists = false;
    bool selected = false;
    bool valid = false;
    std::string status = "not_selected";
    std::string error;
    std::filesystem::path pointer_path;
    std::string pointer_sha256;
    nlohmann::json pointer = nlohmann::json::object();
    PhysicalRegistrationArtifactCandidate candidate;
};

std::filesystem::path active_physical_registration_pointer_path(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial);

std::vector<PhysicalRegistrationArtifactCandidate>
discover_physical_registration_artifacts(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format,
    std::vector<std::string>* warnings_out = nullptr);

PhysicalRegistrationArtifactCandidate validate_physical_registration_artifact(
    const std::filesystem::path& calibration_base_dir,
    const std::filesystem::path& observation_path,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format);

PhysicalRegistrationSelectionResolution resolve_active_physical_registration(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    int expected_width_px,
    int expected_height_px,
    const std::string& expected_pixel_format);

bool select_physical_registration_artifact(
    const std::filesystem::path& calibration_base_dir,
    const PhysicalRegistrationArtifactCandidate& candidate,
    const std::string& selected_at_utc,
    std::string* error_out);

bool clear_active_physical_registration(
    const std::filesystem::path& calibration_base_dir,
    const std::string& camera_serial,
    const std::string& cleared_at_utc,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
