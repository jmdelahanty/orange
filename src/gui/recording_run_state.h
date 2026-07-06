#pragma once

#include "json.hpp"

#include <chrono>
#include <string>

namespace orange::gui {

// Moved verbatim from src/orange.cpp: gui_autorun_update() reads the
// active/finalizing flags in its kWaitFinalize stage, so the run-state
// struct must be visible to both translation units.
struct GuiRecordingRunState {
    bool active = false;
    bool finalizing = false;
    bool finalized = false;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
    std::string recording_started_at_utc;
    std::string recording_stop_requested_at_utc;
    std::string recording_drained_at_utc;
    std::string stop_reason = "manual_stop";
    nlohmann::json stop_control = nlohmann::json::object();
    std::chrono::steady_clock::time_point recording_started_at{};
    std::chrono::steady_clock::time_point recording_stop_requested_at{};
    std::chrono::steady_clock::time_point recording_drained_at{};
    bool diagnostic_finalize_stall_reported = false;
    // Shadow-mode incremental clip split cross-check result for the last
    // finalized run (stage 4, ORANGE_GUI_INCREMENTAL_CLIP_SHADOW); empty
    // unless shadow mode ran. Kept here so the GUI status can render it.
    std::string shadow_cross_check_summary;
};

}  // namespace orange::gui

// This symbol was defined in orange.cpp's anonymous namespace and is
// referenced there unqualified; keep that spelling compiling unchanged.
using orange::gui::GuiRecordingRunState;      // NOLINT(misc-unused-using-decls)
