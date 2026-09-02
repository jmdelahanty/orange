#pragma once

#include "recording_output_descriptor.h"
#include "session/spatial_roi_recorder_camera_contract.h"

#include <string>
#include <vector>

namespace orange::session::spatial_roi {

// The camera-level contract is authenticated and plan-ordered before this
// seam is called. This builder only projects that closed view into the shared
// output-descriptor type; it never reads the filesystem or accepts paths from
// a caller. The first recorder slice is exactly four independent outputs.
bool build_spatial_roi_recording_outputs(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const std::string& status,
    std::vector<orange::session::RecordingOutputDescriptor>* outputs_out,
    std::string* error_out = nullptr);

}  // namespace orange::session::spatial_roi
