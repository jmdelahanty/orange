#pragma once

#include "calibration_transaction.h"

#include <string>
#include <vector>

struct SpatialLayoutUiState;

namespace orange::gui::spatial_layout {

inline constexpr const char* kManualCameraPreflightTransactionOwner =
    "manual_camera_preflight";
inline constexpr const char* kSpatialDirectCaptureTransactionOwner =
    "spatial_direct_capture";
inline constexpr const char* kManualGroupTransactionOwner =
    "manual_spatial_group_capture";
inline constexpr const char* kDailyRegistrationTransactionOwner =
    "daily_registration";
inline constexpr const char* kGuidedCommissioningTransactionOwner =
    "guided_commissioning";
inline constexpr const char* kArenaCenteringTransactionOwner =
    "arena_centering_commissioning";

bool acquire_spatial_calibration_transaction(
    SpatialLayoutUiState* ui_state,
    const std::string& owner_kind,
    const std::string& owner_id,
    orange::calibration::WorkflowKind workflow,
    const std::vector<std::string>& camera_serials,
    orange::calibration::MutationSet allowed_owner_mutations,
    const std::string& reason,
    std::string* error_out);

bool require_spatial_calibration_transaction(
    const SpatialLayoutUiState& ui_state,
    const std::vector<std::string>& camera_serials,
    orange::calibration::Mutation required_owner_mutation,
    std::string* error_out);

bool spatial_calibration_transaction_owned_by(
    const SpatialLayoutUiState& ui_state,
    const std::string& owner_kind);

void release_spatial_calibration_transaction(
    SpatialLayoutUiState* ui_state,
    const std::string& terminal_status,
    const std::string& terminal_reason);

}  // namespace orange::gui::spatial_layout
