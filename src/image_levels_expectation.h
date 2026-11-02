// The camera config's image-levels expectation (camera JSON `image_levels`,
// orange.camera.image_levels v1); parsed and checked in image_levels.h. Kept
// free of json.hpp because camera.h includes it.
#pragma once

#include <optional>
#include <string>

namespace orange::image_levels {

// `configured` is false when the camera JSON has no block.
struct CameraExpectation {
    bool configured = false;
    std::optional<double> expected_mean;
    double mean_tolerance_fraction = 0.15;
    std::optional<double> max_saturated_fraction;
    // Where the numbers came from (free-form object: source, measured_at_utc,
    // settings, statistics, note), kept as JSON text so camera.h stays free of json.hpp.
    std::string basis_json;
};

}  // namespace orange::image_levels
