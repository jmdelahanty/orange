#include "gui/spatial_layout/calibration_transaction_bridge.h"

#include "gui/spatial_layout/state.h"

#include <utility>

namespace orange::gui::spatial_layout {

bool acquire_spatial_calibration_transaction(
    SpatialLayoutUiState* ui_state,
    const std::string& owner_kind,
    const std::string& owner_id,
    const orange::calibration::WorkflowKind workflow,
    const std::vector<std::string>& camera_serials,
    const orange::calibration::MutationSet allowed_owner_mutations,
    const std::string& reason,
    std::string* error_out)
{
    if (!ui_state) {
        if (error_out) {
            *error_out = "Spatial calibration transaction state is unavailable.";
        }
        return false;
    }
    if (ui_state->calibration_transaction_lease) {
        if (error_out) {
            *error_out =
                "Spatial calibration already owns transaction kind " +
                ui_state->calibration_transaction_owner_kind + ".";
        }
        return false;
    }

    orange::calibration::TransactionRequest request;
    request.owner_id = owner_id;
    request.workflow = workflow;
    request.camera_serials = camera_serials;
    request.allowed_owner_mutations = allowed_owner_mutations;
    request.reason = reason;
    auto acquired =
        orange::calibration::global_transaction_coordinator().TryAcquire(
            std::move(request));
    if (!acquired.ok()) {
        if (error_out) {
            *error_out = acquired.error;
        }
        return false;
    }
    ui_state->calibration_transaction_lease = std::move(acquired.lease);
    ui_state->calibration_transaction_owner_kind = owner_kind;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool require_spatial_calibration_transaction(
    const SpatialLayoutUiState& ui_state,
    const std::vector<std::string>& camera_serials,
    const orange::calibration::Mutation required_owner_mutation,
    std::string* error_out)
{
    const auto* lease = ui_state.calibration_transaction_lease.get();
    if (!lease || !lease->active()) {
        if (error_out) {
            *error_out = "The parent calibration transaction lease is missing.";
        }
        return false;
    }
    if (!lease->covers_cameras(camera_serials)) {
        if (error_out) {
            *error_out =
                "The child capture camera scope is outside its parent calibration transaction.";
        }
        return false;
    }
    if (!lease->permits(required_owner_mutation)) {
        if (error_out) {
            *error_out =
                "The parent calibration transaction does not permit " +
                std::string(orange::calibration::mutation_name(
                    required_owner_mutation)) +
                ".";
        }
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool spatial_calibration_transaction_owned_by(
    const SpatialLayoutUiState& ui_state,
    const std::string& owner_kind)
{
    return ui_state.calibration_transaction_lease &&
        ui_state.calibration_transaction_lease->active() &&
        ui_state.calibration_transaction_owner_kind == owner_kind;
}

void release_spatial_calibration_transaction(
    SpatialLayoutUiState* ui_state,
    const std::string& terminal_status,
    const std::string& terminal_reason)
{
    if (!ui_state) {
        return;
    }
    if (ui_state->calibration_transaction_lease) {
        ui_state->calibration_transaction_lease->Release(
            terminal_status, terminal_reason);
        ui_state->calibration_transaction_lease.reset();
    }
    ui_state->calibration_transaction_owner_kind.clear();
    ui_state->group_capture_owns_calibration_transaction = false;
}

}  // namespace orange::gui::spatial_layout
