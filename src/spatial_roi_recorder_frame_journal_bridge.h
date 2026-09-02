#pragma once

#include "spatial_roi_recorder_frame_journal.h"
#include "spatial_roi_recorder_ipc_session.h"

#include <string>

namespace orange::spatial_roi::recording {

// Convert the session's observer outcome into the journal's canonical
// transport record. The session has already validated the complete FRAME;
// this seam deliberately does not copy CUDA IPC capability strings beyond
// detach.
bool make_spatial_roi_recorder_frame_transport_outcome(
    const ipc::SpatialRoiRecorderIpcFrameOutcome& session_outcome,
    SpatialRoiRecorderFrameTransportOutcome* outcome_out,
    std::string* error_out = nullptr) noexcept;

}  // namespace orange::spatial_roi::recording
