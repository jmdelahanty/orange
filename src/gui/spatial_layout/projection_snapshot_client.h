#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"

#include <string>

namespace orange::calibration {
struct CalibrationImageSetRequest;
}

namespace orange::gui::spatial_layout {

struct CitrusProjectionSnapshotQueryResult {
    bool attempted = false;
    bool ok = false;
    nlohmann::json snapshot = nlohmann::json::object();
    std::string reason;
};

CitrusProjectionSnapshotQueryResult query_citrus_active_projection_snapshot(
    const std::string& phase,
    const std::string& operation_id);

void clear_captured_citrus_projection_snapshot_metadata(SpatialLayoutUiState* ui_state);

void set_captured_citrus_projection_snapshots(
    SpatialLayoutUiState* ui_state,
    const nlohmann::json& pre_capture,
    const nlohmann::json& post_capture);

bool snapshot_projection_matches_context(
    const nlohmann::json& snapshot,
    const orange::calibration::CalibrationImageSetRequest& request);

nlohmann::json make_citrus_projection_epoch_consistency(
    const orange::calibration::CalibrationImageSetRequest& request);

} // namespace orange::gui::spatial_layout
