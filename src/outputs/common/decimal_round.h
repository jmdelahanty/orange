// Rounding a number to a stated count of decimals for a JSON output, so that it
// serializes as its shortest decimal text (nlohmann dumps the shortest text that
// reads back as the same double).
//
// Why not std::round(v * 10^d) / 10^d: the host build uses -Ofast -ffast-math
// (ORANGE_USE_FAST_MATH), whose -freciprocal-math turns the division by a known
// constant into a multiplication by its reciprocal, and 10^-d is not exact in
// binary. The result is then 1 ulp from the nearest double to the decimal and
// prints as, for example, 180.96200000000002 (= 180962 * 0.001) instead of
// 180.962. Found by Palette's consumer checks of the 2026-11-15 rig recordings;
// it affected every rounded Orange output built with fast math (image levels,
// empty-background statistics, YOLO and pose event logs).
//
// Here the decimal is formed as text ("<units>e-<d>") and read with strtod,
// which returns the nearest double by contract whatever the compiler flags.
// The text has no decimal point, so the locale does not matter. The schema
// generator (tools/schema_codegen/codegen.py) emits the same function into
// every generated header, which may use only the standard library.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace orange::outputs {

// Half away from zero (std::round), then the nearest double to that decimal.
inline double round_decimals(double value, int decimals)
{
    if (!std::isfinite(value) || decimals < 0 || decimals > 17) return value;
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i) scale *= 10.0;
    const double units = std::round(value * scale);
    if (!std::isfinite(units) || std::fabs(units) >= 9007199254740992.0) return value;  // beyond exact integers
    char text[48];
    std::snprintf(text, sizeof text, "%.0fe-%d", units, decimals);
    return std::strtod(text, nullptr);
}

}  // namespace orange::outputs
