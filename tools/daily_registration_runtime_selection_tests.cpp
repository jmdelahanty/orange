#include "gui/spatial_layout/daily_registration_runtime_selection.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace spatial = orange::gui::spatial_layout;

namespace {

constexpr const char* kPath = "/calibration/dailyreg/registration.json";
constexpr const char* kSha = "sha256:registration";

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

nlohmann::json ValidStatus(
    const std::string& operation_id = "select-op",
    const std::string& transition = "daily_registration_selected")
{
    return {
        {"operation_id", operation_id},
        {"transition", transition},
        {"runtime", {
            {"mode", "selected_daily_registration"},
            {"state", "selected_valid"},
            {"all_selected_runtime_safe", true},
            {"blocking", false},
            {"applied", true},
            {"runtime_geometry_matches_selection", true},
            {"targets", nlohmann::json::array({{
                {"arena_id", "arena_1"},
                {"camera_id", "2010093"},
                {"state", "selected_valid"},
                {"applied", true},
                {"registration_path", kPath},
                {"registration_sha256", kSha},
            }})},
        }},
    };
}

void TestExactAcknowledgementCompletes()
{
    const auto result = spatial::assess_daily_registration_runtime_selection(
        ValidStatus(), "select-op", kPath, kSha, false);
    Require(
        result.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kSelectedValid,
        "an exact acknowledged selection should complete");
    Require(result.operation_acknowledged,
            "the matching operation should be recorded");
    Require(!result.reconciled_exact_artifact,
            "a direct acknowledgement is not reconciliation");
}

void TestExactLiveArtifactReconcilesMissedAcknowledgement()
{
    const auto result = spatial::assess_daily_registration_runtime_selection(
        ValidStatus("older-op", "status_refresh"),
        "select-op", kPath, kSha, true);
    Require(
        result.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kSelectedValid,
        "the exact live artifact should win over a missed acknowledgement");
    Require(result.reconciled_exact_artifact,
            "the completion should be marked as reconciliation");
}

void TestAcknowledgedInvalidSelectionFailsImmediately()
{
    auto status = ValidStatus();
    auto& runtime = status["runtime"];
    runtime["state"] = "selected_invalid";
    runtime["all_selected_runtime_safe"] = false;
    runtime["blocking"] = true;
    runtime["applied"] = false;
    runtime["runtime_geometry_matches_selection"] = false;
    runtime["targets"][0]["state"] = "selected_invalid";
    runtime["targets"][0]["applied"] = false;
    runtime["targets"][0]["error"] =
        "daily_registration_commissioning_base_unavailable";

    const auto result = spatial::assess_daily_registration_runtime_selection(
        status, "select-op", kPath, kSha, false);
    Require(
        result.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kSelectedInvalid,
        "an acknowledged invalid terminal state must not remain pending");
    Require(
        result.error.find("daily_registration_commissioning_base_unavailable") !=
            std::string::npos,
        "the target failure should be retained for the operator");
}

void TestPendingAndTimeoutAreDistinct()
{
    const auto pending = spatial::assess_daily_registration_runtime_selection(
        nlohmann::json::object(), "select-op", kPath, kSha, false);
    Require(
        pending.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kPending,
        "an unacknowledged selection should remain pending before deadline");

    const auto timed_out = spatial::assess_daily_registration_runtime_selection(
        nullptr, "select-op", kPath, kSha, true);
    Require(
        timed_out.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kTimedOut,
        "a malformed response after the deadline should terminate safely");
}

void TestMismatchedArtifactDoesNotReconcile()
{
    auto status = ValidStatus("older-op", "status_refresh");
    status["runtime"]["targets"][0]["registration_sha256"] =
        "sha256:different";
    const auto result = spatial::assess_daily_registration_runtime_selection(
        status, "select-op", kPath, kSha, false);
    Require(
        result.disposition == spatial::
            DailyRegistrationRuntimeSelectionDisposition::kPending,
        "a different live artifact must not satisfy reconciliation");
}

}  // namespace

int main()
{
    try {
        TestExactAcknowledgementCompletes();
        TestExactLiveArtifactReconcilesMissedAcknowledgement();
        TestAcknowledgedInvalidSelectionFailsImmediately();
        TestPendingAndTimeoutAreDistinct();
        TestMismatchedArtifactDoesNotReconcile();
        std::cout << "daily registration runtime selection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "daily registration runtime selection tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
