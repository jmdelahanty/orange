#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

struct ProjectorIntensityCameraAuthorityRef {
    std::string camera_serial;
    std::string arena_id;
    std::string rig_id;
    std::string canvas_id;
    std::string citrus_canvas_config_path;
    std::string report_path;
    std::string report_sha256;
};

struct ProjectorIntensityAuthorityResult {
    bool ok = false;
    int foreground_gray_u8 = -1;
    std::string report_path;
    std::string report_sha256;
    nlohmann::json provenance = nlohmann::json::object();
    std::string error;
};

// Resolve one immutable projector-intensity commissioning authority shared by
// every requested camera. Direct homography provenance is preferred. If an
// operational homography omitted that provenance, the role-specific
// commissioning-reference pointer is consulted and validated instead.
ProjectorIntensityAuthorityResult resolve_projector_intensity_authority(
    const std::vector<ProjectorIntensityCameraAuthorityRef>& camera_refs);

}  // namespace orange::gui::spatial_layout
