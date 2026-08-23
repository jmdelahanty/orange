#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

enum class SpatialLayoutPersistencePanelEvent {
    None,
    StartNewCalibrationSession,
    SaveTopRimObservation,
    SaveCalibrationImageSet,
    SaveGroupCalibrationImageSets,
    SaveArenaLayoutArtifact,
    SaveLinkedArenaLayoutArtifacts,
    LoadArenaLayoutArtifact,
    LoadCalibrationSession,
    LoadSelectedSessionImage
};

struct SpatialLayoutPersistencePanelState {
    bool citrus_linked_layout_matches_selected_camera = false;
    bool captured_in_full_resolution = false;
    bool top_rim_save_busy = false;
    bool generic_image_set_save_busy = false;
    bool can_save_top_rim_observation = false;
    std::string top_rim_ineligible_reason;
    bool can_save_generic_image_set = false;
    bool can_save_group_image_sets = false;
    bool can_save_linked_arena_layouts = false;
};

SpatialLayoutPersistencePanelEvent render_spatial_layout_persistence_panel(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutPersistencePanelState& panel_state);

} // namespace orange::gui::spatial_layout
