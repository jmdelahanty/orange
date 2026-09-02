#pragma once

#include <string_view>

namespace orange::headless {

enum class ExperimentRunOutcome {
    kPass,
    kMarginal,
    kFail,
};

// Aggregate live/session and per-camera diagnostics. A failure already latched
// by the recording session always has precedence over passing camera metrics.
ExperimentRunOutcome ResolveExperimentRunOutcome(
    bool run_failed,
    bool any_camera_failed,
    bool any_camera_marginal) noexcept;

// Re-read a serialized run entry defensively. Only the closed, consistent
// completed/pass and completed/marginal pairs are non-fail outcomes. In
// particular, status=failed can never be overridden by pass_fail=pass.
ExperimentRunOutcome ResolveReportedExperimentRunOutcome(
    std::string_view status,
    std::string_view pass_fail) noexcept;

const char* ExperimentRunOutcomeName(ExperimentRunOutcome outcome) noexcept;

}  // namespace orange::headless
