#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace orange::recording_geometry {

inline constexpr const char* kRecordingGeometryContractSchemaId =
    "orange.recording.geometry_contract";
// v2 (2026-10-12): embeds dish_top_rim_observation v3 and geometry_assets v2.
inline constexpr int kRecordingGeometryContractSchemaVersion = 2;
inline constexpr int kDailyRegistrationGeometrySchemaVersion = 2;
inline constexpr int kDailyRegistrationCameraGeometrySchemaVersion = 2;

struct CitrusGeometryResolveRequest {
    std::string selected_canvas_config_path;
    std::string selection_source;
    std::string captured_at_utc;
    std::vector<std::string> camera_serials;
};

struct CitrusGeometryResolveResult {
    // A contract is always returned. Missing selection and validation failures
    // are represented explicitly so metadata collection never forces Citrus
    // participation or blocks an Orange recording.
    nlohmann::json contract = nlohmann::json::object();
    bool configured = false;
    bool fully_resolved = false;
};

CitrusGeometryResolveResult resolve_citrus_recording_geometry(
    const CitrusGeometryResolveRequest& request);

}  // namespace orange::recording_geometry
