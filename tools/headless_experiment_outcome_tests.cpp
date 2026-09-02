#include "headless_experiment_outcome.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using orange::headless::ExperimentRunOutcome;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void aggregate_failure_precedence()
{
    using orange::headless::ResolveExperimentRunOutcome;

    require(ResolveExperimentRunOutcome(false, false, false) ==
                ExperimentRunOutcome::kPass,
            "healthy session and camera diagnostics should pass");
    require(ResolveExperimentRunOutcome(false, false, true) ==
                ExperimentRunOutcome::kMarginal,
            "camera marginal should remain marginal");
    require(ResolveExperimentRunOutcome(false, true, true) ==
                ExperimentRunOutcome::kFail,
            "camera failure should take precedence over marginal");
    require(ResolveExperimentRunOutcome(true, false, false) ==
                ExperimentRunOutcome::kFail,
            "session failure must not be overwritten by passing cameras");
    require(ResolveExperimentRunOutcome(true, false, true) ==
                ExperimentRunOutcome::kFail,
            "session failure must take precedence over camera marginal");
}

void reported_run_is_fail_closed()
{
    using orange::headless::ResolveReportedExperimentRunOutcome;

    require(ResolveReportedExperimentRunOutcome("completed", "pass") ==
                ExperimentRunOutcome::kPass,
            "closed completed/pass pair should pass");
    require(ResolveReportedExperimentRunOutcome("completed", "marginal") ==
                ExperimentRunOutcome::kMarginal,
            "closed completed/marginal pair should remain marginal");
    require(ResolveReportedExperimentRunOutcome("completed", "fail") ==
                ExperimentRunOutcome::kFail,
            "reported failure should fail");
    require(ResolveReportedExperimentRunOutcome("failed", "pass") ==
                ExperimentRunOutcome::kFail,
            "failed status must override stale passing diagnostics");
    require(ResolveReportedExperimentRunOutcome("failed", "marginal") ==
                ExperimentRunOutcome::kFail,
            "failed status must override marginal diagnostics");
    require(ResolveReportedExperimentRunOutcome("completed", "") ==
                ExperimentRunOutcome::kFail,
            "missing pass/fail classification must fail closed");
    require(ResolveReportedExperimentRunOutcome("unknown", "pass") ==
                ExperimentRunOutcome::kFail,
            "unknown run status must fail closed");
}

void outcome_names_are_closed()
{
    using orange::headless::ExperimentRunOutcomeName;

    require(std::string(ExperimentRunOutcomeName(ExperimentRunOutcome::kPass)) ==
                "pass",
            "pass name mismatch");
    require(std::string(ExperimentRunOutcomeName(
                ExperimentRunOutcome::kMarginal)) == "marginal",
            "marginal name mismatch");
    require(std::string(ExperimentRunOutcomeName(ExperimentRunOutcome::kFail)) ==
                "fail",
            "fail name mismatch");
}

}  // namespace

int main()
{
    try {
        aggregate_failure_precedence();
        std::cout << "[PASS] aggregate_failure_precedence\n";
        reported_run_is_fail_closed();
        std::cout << "[PASS] reported_run_is_fail_closed\n";
        outcome_names_are_closed();
        std::cout << "[PASS] outcome_names_are_closed\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
