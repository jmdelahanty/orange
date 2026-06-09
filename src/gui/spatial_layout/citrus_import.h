#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

std::string default_citrus_rigs_root();

std::string citrus_template_display_label(const CitrusSpatialTemplateState& template_state);

bool collect_citrus_single_circle_templates(
    const std::filesystem::path& config_path,
    const nlohmann::json& root,
    std::vector<CitrusSpatialTemplateState>* templates_out,
    std::vector<std::string>* available_camera_ids_out,
    std::string* error_out);

int find_citrus_template_index_for_camera(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial);

} // namespace orange::gui::spatial_layout
