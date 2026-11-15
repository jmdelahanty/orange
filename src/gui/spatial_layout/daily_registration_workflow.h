#pragma once

#include "gui/spatial_layout/state.h"
#include "gui/daily_registration_autorun.h"
#include "json.hpp"

#include <string>

class SpatialSnapshotWorker;

namespace orange::gui::spatial_layout {

// Progresses only asynchronous capture/save/control acknowledgements. It does
// not cross an operator-review boundary on its own.
void advance_daily_registration_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir);

// Renders the opt-in daily workflow and issues semantic actions only from
// explicit operator buttons/confirmations.
void render_daily_registration_workflow_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked);

// Unattended daily registration (gui/daily_registration_autorun.h). Called each
// frame after the spatial-layout background work; drives the same workflow the
// panel drives, and stops for the single recorded operator confirmation.
struct DailyRegistrationAutorunState {
    std::string stage = "idle";
    std::string workflow_stage;
    bool finished = false;
    bool passed = false;
    bool exit_requested = false;
    bool result_written = false;
    std::string error;
    std::string transaction_id;
    std::string pending_confirmation_path;
    double started_s = 0.0;
    double stage_started_s = 0.0;
    double last_attempt_s = -1e9;
    nlohmann::json known_radius = nlohmann::json::object();
    nlohmann::json fit_summaries = nlohmann::json::object();
    nlohmann::json physical_state_declaration = nullptr;
    nlohmann::json confirmation = nullptr;
};

// Returns true when the GUI should close (the run finished and the config asks to exit).
bool advance_daily_registration_autorun(
    DailyRegistrationAutorunState* state,
    const orange::gui::DailyRegistrationAutorunConfig& config,
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir,
    bool streaming,
    bool recording_mutation_locked);

}  // namespace orange::gui::spatial_layout
