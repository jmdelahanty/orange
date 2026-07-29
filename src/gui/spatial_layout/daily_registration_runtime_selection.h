#pragma once

#include "json.hpp"

#include <string>

namespace orange::gui::spatial_layout {

enum class DailyRegistrationRuntimeSelectionDisposition {
    kPending,
    kSelectedValid,
    kSelectedInvalid,
    kTimedOut,
};

struct DailyRegistrationRuntimeSelectionAssessment {
    DailyRegistrationRuntimeSelectionDisposition disposition =
        DailyRegistrationRuntimeSelectionDisposition::kPending;
    bool operation_acknowledged = false;
    bool reconciled_exact_artifact = false;
    std::string error;
    nlohmann::json runtime = nlohmann::json::object();
};

// Classifies one Citrus daily-registration status response. An exact live
// path/SHA match is authoritative even when the original operation
// acknowledgement was missed, allowing an interrupted Orange transaction to
// reconcile without reapplying geometry.
DailyRegistrationRuntimeSelectionAssessment
assess_daily_registration_runtime_selection(
    const nlohmann::json& daily_registration_status,
    const std::string& expected_operation_id,
    const std::string& expected_registration_path,
    const std::string& expected_registration_sha256,
    bool deadline_expired);

}  // namespace orange::gui::spatial_layout
