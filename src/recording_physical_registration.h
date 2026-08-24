#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orange::recording_geometry {

struct RecordingPhysicalCamera {
    std::string camera_serial;
    int width_px = 0;
    int height_px = 0;
    std::string pixel_format;
};

// Resolves the normal Orange calibration root. Tests and deployments may set
// ORANGE_CALIBRATION_BASE_DIR; otherwise this follows the GUI's established
// $HOME/orange_data/calibrations layout.
std::filesystem::path resolve_recording_calibration_base_dir();

// Revalidates each participating camera's exact active physical-registration
// pointer and appends a recording-bound snapshot to an existing geometry
// contract. Missing selections are non-blocking. An invalid selected pointer
// remains explicit and is never replaced with an older nearby artifact.
void append_recording_physical_registrations(
    nlohmann::json* recording_geometry_contract,
    const std::filesystem::path& calibration_base_dir,
    const std::vector<RecordingPhysicalCamera>& cameras,
    const std::string& captured_at_utc);

// Marks the recording-bound entry used by a live mask policy without assuming
// whether the source was standalone Orange physical registration or the
// backwards-compatible Citrus daily-registration envelope.
bool mark_recording_dish_mask_runtime_use(
    nlohmann::json* recording_geometry_contract,
    const std::string& camera_serial,
    const std::string& mode,
    bool centroid_gate_active,
    bool neural_input_mask_active);

}  // namespace orange::recording_geometry
