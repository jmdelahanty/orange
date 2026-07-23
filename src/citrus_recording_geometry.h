#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace orange::recording_geometry {

inline constexpr const char* kRecordingGeometryContractSchemaId =
    "orange.recording.geometry_contract";
inline constexpr int kRecordingGeometryContractSchemaVersion = 1;

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
