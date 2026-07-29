#include "gui/spatial_layout/daily_registration_runtime_selection.h"

#include <sstream>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

std::string StringValue(const nlohmann::json& value, const char* key)
{
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string()
        ? it->get<std::string>()
        : std::string();
}

bool BoolValue(
    const nlohmann::json& value,
    const char* key,
    const bool fallback = false)
{
    if (!value.is_object()) return fallback;
    const auto it = value.find(key);
    return it != value.end() && it->is_boolean()
        ? it->get<bool>()
        : fallback;
}

bool ExactSelectedRuntime(
    const nlohmann::json& runtime,
    const std::string& expected_path,
    const std::string& expected_sha256)
{
    if (!runtime.is_object() || expected_path.empty() ||
        expected_sha256.empty() ||
        StringValue(runtime, "mode") != "selected_daily_registration" ||
        StringValue(runtime, "state") != "selected_valid" ||
        !BoolValue(runtime, "all_selected_runtime_safe") ||
        BoolValue(runtime, "blocking", true) ||
        !BoolValue(runtime, "applied") ||
        !BoolValue(runtime, "runtime_geometry_matches_selection")) {
        return false;
    }
    const auto targets_it = runtime.find("targets");
    if (targets_it == runtime.end() || !targets_it->is_array() ||
        targets_it->empty()) {
        return false;
    }
    for (const auto& target : *targets_it) {
        if (!target.is_object() ||
            StringValue(target, "state") != "selected_valid" ||
            !BoolValue(target, "applied") ||
            StringValue(target, "registration_path") != expected_path ||
            StringValue(target, "registration_sha256") != expected_sha256) {
            return false;
        }
    }
    return true;
}

std::string InvalidSelectionError(const nlohmann::json& runtime)
{
    std::ostringstream message;
    message << "daily_registration_runtime_selection_invalid";
    const std::string state = StringValue(runtime, "state");
    if (!state.empty()) message << ":state=" << state;
    const std::string aggregate_error = StringValue(runtime, "error");
    if (!aggregate_error.empty()) message << ":error=" << aggregate_error;
    const auto targets_it = runtime.is_object()
        ? runtime.find("targets") : runtime.end();
    if (targets_it != runtime.end() && targets_it->is_array()) {
        for (const auto& target : *targets_it) {
            const std::string error = StringValue(target, "error");
            if (error.empty()) continue;
            std::string identity = StringValue(target, "arena_id");
            if (identity.empty()) identity = StringValue(target, "camera_id");
            message << ":" << (identity.empty() ? "target" : identity)
                    << "=" << error;
        }
    }
    return message.str();
}

}  // namespace

DailyRegistrationRuntimeSelectionAssessment
assess_daily_registration_runtime_selection(
    const nlohmann::json& daily_registration_status,
    const std::string& expected_operation_id,
    const std::string& expected_registration_path,
    const std::string& expected_registration_sha256,
    const bool deadline_expired)
{
    DailyRegistrationRuntimeSelectionAssessment result;
    if (!daily_registration_status.is_object()) {
        if (deadline_expired) {
            result.disposition =
                DailyRegistrationRuntimeSelectionDisposition::kTimedOut;
            result.error = "daily_registration_runtime_selection_timeout:no_valid_status";
        }
        return result;
    }
    const auto runtime_it = daily_registration_status.find("runtime");
    if (runtime_it != daily_registration_status.end() &&
        runtime_it->is_object()) {
        result.runtime = *runtime_it;
    }
    result.operation_acknowledged =
        !expected_operation_id.empty() &&
        StringValue(daily_registration_status, "operation_id") ==
            expected_operation_id &&
        StringValue(daily_registration_status, "transition") ==
            "daily_registration_selected";

    if (ExactSelectedRuntime(
            result.runtime,
            expected_registration_path,
            expected_registration_sha256)) {
        result.disposition =
            DailyRegistrationRuntimeSelectionDisposition::kSelectedValid;
        result.reconciled_exact_artifact = !result.operation_acknowledged;
        return result;
    }

    if (result.operation_acknowledged) {
        result.disposition =
            DailyRegistrationRuntimeSelectionDisposition::kSelectedInvalid;
        result.error = InvalidSelectionError(result.runtime);
        return result;
    }

    if (deadline_expired) {
        result.disposition =
            DailyRegistrationRuntimeSelectionDisposition::kTimedOut;
        result.error = "daily_registration_runtime_selection_timeout";
    }
    return result;
}

}  // namespace orange::gui::spatial_layout
