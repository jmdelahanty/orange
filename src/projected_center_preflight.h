#ifndef ORANGE_PROJECTED_CENTER_PREFLIGHT_H
#define ORANGE_PROJECTED_CENTER_PREFLIGHT_H

#include "json.hpp"
#include "orange_local_control.h"

#include <string>
#include <vector>

namespace orange::control {

inline constexpr const char* kProjectedCenterPreflightMethod =
    "preflight_projected_center_verification";
inline constexpr double kProjectedCenterPreflightMinExposureUsDefault = 100000.0;
inline constexpr double kProjectedCenterPreflightMaxFrameRateHzDefault = 5.0;

struct ProjectedCenterPreflightResult {
    bool passed = false;
    std::vector<std::string> blocking_reasons;
    nlohmann::json effect = nlohmann::json::object();
};

bool IsProjectedCenterPreflightMethod(const std::string& method);

ProjectedCenterPreflightResult BuildProjectedCenterVerificationPreflight(
    const LocalControlStatusSnapshot& status,
    const nlohmann::json& params,
    const std::string& checked_at_utc);

} // namespace orange::control

#endif // ORANGE_PROJECTED_CENTER_PREFLIGHT_H
