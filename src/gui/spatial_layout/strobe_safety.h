#pragma once

// IR safety (Jeremy): the NIR strobe is never continuous and never a constant
// GPO, and its duty cycle stays at or below 8 %. A calibration preflight that
// would drive the strobe high (force_manual_active), or pulse it with every
// exposure at a calibration timing above that duty, is refused before any
// camera is touched. The preflight applies the light handling before the
// calibration timing, so the check uses the requested timing up front.

#include <cstdint>
#include <sstream>
#include <string>

namespace orange::gui::spatial_layout {

inline constexpr double kMaximumNirStrobeDutyFraction = 0.08;

// Empty when allowed; otherwise the refusal text.
inline std::string calibration_strobe_safety_error(const std::string& requested_handling,
                                                   const std::uint32_t exposure_us,
                                                   const std::uint32_t frame_rate_hz)
{
    if (requested_handling == "force_manual_active") {
        return "Calibration prepare refused: force_manual_active would hold the NIR "
               "strobe on continuously.";
    }
    if (requested_handling == "keep_or_restore_mapped_pulse") {
        const double duty = static_cast<double>(exposure_us) *
                            static_cast<double>(frame_rate_hz) / 1.0e6;
        if (duty > kMaximumNirStrobeDutyFraction) {
            std::ostringstream oss;
            oss << "Calibration prepare refused: the mapped NIR strobe would pulse with every "
                << (static_cast<double>(exposure_us) / 1000.0) << " ms exposure at "
                << frame_rate_hz << " FPS, " << (duty * 100.0) << " % duty (limit "
                << (kMaximumNirStrobeDutyFraction * 100.0) << " %).";
            return oss.str();
        }
    }
    return {};
}

}  // namespace orange::gui::spatial_layout
