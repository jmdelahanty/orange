#include "headless_experiment_outcome.h"

namespace orange::headless {

ExperimentRunOutcome ResolveExperimentRunOutcome(
    const bool run_failed,
    const bool any_camera_failed,
    const bool any_camera_marginal) noexcept
{
    if (run_failed || any_camera_failed) {
        return ExperimentRunOutcome::kFail;
    }
    if (any_camera_marginal) {
        return ExperimentRunOutcome::kMarginal;
    }
    return ExperimentRunOutcome::kPass;
}

ExperimentRunOutcome ResolveReportedExperimentRunOutcome(
    const std::string_view status,
    const std::string_view pass_fail) noexcept
{
    if (status != "completed") {
        return ExperimentRunOutcome::kFail;
    }
    if (pass_fail == "pass") {
        return ExperimentRunOutcome::kPass;
    }
    if (pass_fail == "marginal") {
        return ExperimentRunOutcome::kMarginal;
    }
    return ExperimentRunOutcome::kFail;
}

const char* ExperimentRunOutcomeName(
    const ExperimentRunOutcome outcome) noexcept
{
    switch (outcome) {
    case ExperimentRunOutcome::kPass:
        return "pass";
    case ExperimentRunOutcome::kMarginal:
        return "marginal";
    case ExperimentRunOutcome::kFail:
        return "fail";
    }
    return "fail";
}

}  // namespace orange::headless
