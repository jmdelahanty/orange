#include "gui/spatial_layout/commissioning_finalize_request.h"

namespace orange::gui::spatial_layout {

nlohmann::json build_commissioning_finalize_params(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    bool accept_commissioning_armed)
{
    return {
        {"transaction_id", transaction_id},
        {"canvas_path", canvas_path},
        {"expected_canvas_checksum", expected_canvas_checksum},
        {"orange_session_dirs", nlohmann::json::array()},
        {"accept_commissioning_armed", accept_commissioning_armed},
    };
}

}  // namespace orange::gui::spatial_layout
