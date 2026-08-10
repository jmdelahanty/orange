#pragma once

#include "json.hpp"

#include <string>

namespace orange::gui::spatial_layout {

// Builds the mutation parameters for Citrus, the commissioning authority.
// Source sessions are deliberately not accepted as an Orange input: Citrus
// derives them from the exact active homography and scale pointers it seals.
nlohmann::json build_commissioning_finalize_params(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    bool accept_commissioning_armed);

}  // namespace orange::gui::spatial_layout
