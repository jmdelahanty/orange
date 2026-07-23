#pragma once

#include "gui/spatial_layout/state.h"

#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

struct DailyRegistrationPreviewOverlay {
    bool available = false;
    bool selected_for_runtime = false;
    bool has_raw_hough_circle = false;
    orange::spatial::RuntimeGeometry raw_hough_circle;
    orange::spatial::RuntimeGeometry accepted_rim_circle;
    Point2d registered_center_camera_px;
    std::vector<Point2d> registered_outline_camera_px;
    std::string transaction_id;
    std::string registration_id;
    std::string rim_artifact_id;
};

// Resolves the exact daily-registration geometry for the image currently shown
// in the spatial preview. An in-progress/just-completed Orange transaction is
// preferred over Citrus runtime status so candidate review remains stable. A
// runtime-derived overlay is accepted only when the selected registration and
// the imported commissioned homography identity agree.
bool resolve_daily_registration_preview_overlay(
    const SpatialLayoutUiState& ui_state,
    DailyRegistrationPreviewOverlay* overlay);

}  // namespace orange::gui::spatial_layout
