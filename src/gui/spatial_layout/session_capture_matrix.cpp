#include "gui/spatial_layout/session_capture_matrix.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

enum class MatrixCellState {
    NotExpected,
    Missing,
    Complete,
    Error
};

enum class MatrixExpectationKind {
    DryArena,
    DryScale,
    DryHomography,
    DryValidation,
    PhysicalProjectedSurface,
    PhysicalDishBase,
    PhysicalFishHeight,
    WetScale,
    WetHomography,
    WetCrosshair,
    WetValidation,
    DishTopObservation
};

struct MatrixRow {
    const char* id;
    const char* label;
    const char* description;
};

struct MatrixColumn {
    const char* id;
    const char* label;
};

struct MatrixExpectation {
    int row = 0;
    int column = 0;
    MatrixExpectationKind kind = MatrixExpectationKind::DryArena;
    const char* label;
};

struct MatrixCellEvaluation {
    MatrixCellState state = MatrixCellState::NotExpected;
    const MatrixExpectation* expectation = nullptr;
    int best_image_index = -1;
    int candidate_count = 0;
    std::vector<std::string> details;
};

constexpr int kColumnArena = 0;
constexpr int kColumnScale = 1;
constexpr int kColumnHomography = 2;
constexpr int kColumnCrosshair = 3;
constexpr int kColumnValidation = 4;
constexpr int kColumnPhysicalTarget = 5;
constexpr int kColumnDishTop = 6;

constexpr int kRowDryProjectionSurface = 0;
constexpr int kRowPhysicalProjectedSurface = 1;
constexpr int kRowPhysicalDishBase = 2;
constexpr int kRowPhysicalFishHeight = 3;
constexpr int kRowWetProjectionSurface = 4;
constexpr int kRowDishTopObservation = 5;

constexpr std::array<MatrixColumn, 7> kColumns{{
    {"arena", "Arena"},
    {"scale", "Scale"},
    {"homography", "Homography"},
    {"crosshair", "Crosshair"},
    {"validation", "Validation"},
    {"physical_target", "Physical Target"},
    {"dish_top", "Dish Top"}
}};

constexpr std::array<MatrixRow, 6> kRows{{
    {"dry_projection_surface",
     "Dry Projection Surface",
     "Dry projector/projection-surface reference and validation captures."},
    {"physical_projected_surface",
     "C0 Physical Dry",
     "Dry physical target at projection/gel surface height."},
    {"physical_dish_base",
     "Cbase Physical Dry",
     "Dry physical target at dish/base inner-surface height."},
    {"physical_fish_height",
     "Cfish Physical Dry",
     "Dry physical target at approximate fish height."},
    {"wet_projection_surface",
     "Wet Projection Surface",
     "Wet runtime-stack projection-surface captures with dish/shelf/water present."},
    {"dish_top_observation",
     "Dish Top Observation",
     "Accepted dish top-rim / valid-area observation artifacts."}
}};

constexpr std::array<MatrixExpectation, 12> kExpectations{{
    {kRowDryProjectionSurface, kColumnArena, MatrixExpectationKind::DryArena,
     "Dry full-camera arena projection"},
    {kRowDryProjectionSurface, kColumnScale, MatrixExpectationKind::DryScale,
     "Dry projection-surface scale image"},
    {kRowDryProjectionSurface, kColumnHomography, MatrixExpectationKind::DryHomography,
     "Dry projection-surface homography"},
    {kRowDryProjectionSurface, kColumnValidation, MatrixExpectationKind::DryValidation,
     "Dry projection-surface validation pattern"},
    {kRowPhysicalProjectedSurface, kColumnPhysicalTarget,
     MatrixExpectationKind::PhysicalProjectedSurface,
     "Dry C0 physical target"},
    {kRowPhysicalDishBase, kColumnPhysicalTarget, MatrixExpectationKind::PhysicalDishBase,
     "Dry Cbase physical target"},
    {kRowPhysicalFishHeight, kColumnPhysicalTarget, MatrixExpectationKind::PhysicalFishHeight,
     "Dry Cfish physical target"},
    {kRowWetProjectionSurface, kColumnScale, MatrixExpectationKind::WetScale,
     "Wet projection-surface scale image"},
    {kRowWetProjectionSurface, kColumnHomography, MatrixExpectationKind::WetHomography,
     "Wet projection-surface rings homography"},
    {kRowWetProjectionSurface, kColumnCrosshair, MatrixExpectationKind::WetCrosshair,
     "Wet projection-surface crosshair"},
    {kRowWetProjectionSurface, kColumnValidation, MatrixExpectationKind::WetValidation,
     "Wet projection-surface validation pattern"},
    {kRowDishTopObservation, kColumnDishTop, MatrixExpectationKind::DishTopObservation,
     "Dish top observation"}
}};

std::string empty_to_unknown(const std::string& value)
{
    return value.empty() ? std::string("unknown") : value;
}

void add_detail(std::vector<std::string>* details, const std::string& detail)
{
    if (details != nullptr && !detail.empty()) {
        details->push_back(detail);
    }
}

bool check_string(std::vector<std::string>* details,
                  const std::string& actual,
                  const std::string& expected,
                  const char* field)
{
    if (actual == expected) {
        return true;
    }
    std::ostringstream oss;
    oss << field << " expected " << expected << ", saw " << empty_to_unknown(actual);
    add_detail(details, oss.str());
    return false;
}

bool check_string_one_of(std::vector<std::string>* details,
                         const std::string& actual,
                         const std::string& expected_a,
                         const std::string& expected_b,
                         const char* field)
{
    if (actual == expected_a || actual == expected_b) {
        return true;
    }
    std::ostringstream oss;
    oss << field << " expected " << expected_a << " or " << expected_b
        << ", saw " << empty_to_unknown(actual);
    add_detail(details, oss.str());
    return false;
}

bool check_bool(std::vector<std::string>* details,
                const bool actual,
                const bool expected,
                const char* field)
{
    if (actual == expected) {
        return true;
    }
    std::ostringstream oss;
    oss << field << " expected " << (expected ? "true" : "false")
        << ", saw " << (actual ? "true" : "false");
    add_detail(details, oss.str());
    return false;
}

bool is_stage_purpose_candidate(const SpatialLayoutSessionReviewImage& image,
                                const char* capture_stage,
                                const char* purpose)
{
    return image.capture_stage == capture_stage &&
           image.purpose == purpose;
}

bool is_candidate_for_expectation(const SpatialLayoutSessionReviewImage& image,
                                  const MatrixExpectationKind kind)
{
    switch (kind) {
    case MatrixExpectationKind::DryArena:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_dry_reference",
            "arena_projection");
    case MatrixExpectationKind::DryScale:
        return is_stage_purpose_candidate(
                   image,
                   "projected_surface_physical_scale_commissioning",
                   "projected_surface_scale_calibration") ||
               is_stage_purpose_candidate(
                   image,
                   "projected_surface_dry_reference",
                   "scale_image");
    case MatrixExpectationKind::DryHomography:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_dry_reference",
            "homography_grid");
    case MatrixExpectationKind::DryValidation:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_dry_reference",
            "validation_pattern");
    case MatrixExpectationKind::PhysicalProjectedSurface:
        return image.capture_stage == "camera_physical_projected_surface";
    case MatrixExpectationKind::PhysicalDishBase:
        return image.capture_stage == "camera_physical_dish_base_inner_surface";
    case MatrixExpectationKind::PhysicalFishHeight:
        return image.capture_stage == "camera_physical_fish_height";
    case MatrixExpectationKind::WetScale:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_wet_runtime_stack",
            "scale_image");
    case MatrixExpectationKind::WetHomography:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_wet_runtime_stack",
            "homography_grid");
    case MatrixExpectationKind::WetCrosshair:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_wet_runtime_stack",
            "crosshair_alignment");
    case MatrixExpectationKind::WetValidation:
        return is_stage_purpose_candidate(
            image,
            "projected_surface_wet_runtime_stack",
            "validation_pattern");
    case MatrixExpectationKind::DishTopObservation:
        return image.capture_stage == "dish_top_observation" ||
               image.purpose == "dish_top_rim" ||
               image.target_plane == "dish_top_rim";
    }
    return false;
}

bool check_common_dry_projection_surface(const SpatialLayoutSessionReviewImage& image,
                                         std::vector<std::string>* details)
{
    bool ok = true;
    ok &= check_string(details, image.capture_stage, "projected_surface_dry_reference", "capture_stage");
    ok &= check_string(details, image.target_plane, "projected_surface", "target_plane");
    ok &= check_string(details, image.metadata.wet_or_dry, "dry", "wet_or_dry");
    ok &= check_string(details, image.metadata.parity_group_role, "dry_reference", "parity_group_role");
    ok &= check_bool(details, image.metadata.reference_only, true, "reference_only");
    ok &= check_string(details, image.metadata.pattern_domain, "full_projected_surface", "pattern_domain");
    return ok;
}

bool check_dry_projection_product(const SpatialLayoutSessionReviewImage& image,
                                  const MatrixExpectationKind kind,
                                  std::vector<std::string>* details)
{
    if (kind == MatrixExpectationKind::DryScale &&
        image.purpose == "projected_surface_scale_calibration") {
        bool ok = true;
        ok &= check_string(details, image.capture_stage,
                           "projected_surface_physical_scale_commissioning",
                           "capture_stage");
        ok &= check_string(details, image.target_plane, "projected_surface",
                           "target_plane");
        ok &= check_string(details, image.role, "physical_target", "image role");
        ok &= check_string(details, image.metadata.wet_or_dry, "dry", "wet_or_dry");
        ok &= check_string(details, image.metadata.parity_group_role,
                           "projected_surface_scale", "parity_group_role");
        ok &= check_bool(details, image.metadata.reference_only, false,
                         "reference_only");
        ok &= check_bool(details, image.metadata.physical_target_used, true,
                         "physical_target_used");
        ok &= check_string(details, image.metadata.target_method,
                           "physical_target_known_xy", "target_method");
        ok &= check_string(details, image.metadata.pattern_type,
                           "physical_point_set", "pattern_type");
        if (image.metadata.target_id.empty()) {
            add_detail(details, "target_id is missing");
            ok = false;
        }
        return ok;
    }
    bool ok = check_common_dry_projection_surface(image, details);
    switch (kind) {
    case MatrixExpectationKind::DryArena:
        ok &= check_string(details, image.purpose, "arena_projection", "purpose");
        ok &= check_string(details, image.role, "projected_arena", "image role");
        break;
    case MatrixExpectationKind::DryScale:
        ok &= check_string(details, image.purpose, "scale_image", "purpose");
        ok &= check_string(details, image.role, "scale_target", "image role");
        ok &= check_string(details, image.metadata.target_method, "ruler_only", "target_method");
        ok &= check_string(details, image.metadata.pattern_type, "ruler", "pattern_type");
        break;
    case MatrixExpectationKind::DryHomography:
        ok &= check_string(details, image.purpose, "homography_grid", "purpose");
        ok &= check_string(details, image.role, "grid_on", "image role");
        ok &= check_string(details, image.metadata.target_method, "projected_pattern_on_diffuser", "target_method");
        ok &= check_string(details, image.metadata.pattern_type, "rectangular_grid", "pattern_type");
        break;
    case MatrixExpectationKind::DryValidation:
        ok &= check_string(details, image.purpose, "validation_pattern", "purpose");
        ok &= check_string(details, image.role, "validation_pattern_on", "image role");
        ok &= check_string(details, image.metadata.pattern_type, "validation_pattern", "pattern_type");
        break;
    default:
        break;
    }
    return ok;
}

bool check_physical_target_product(const SpatialLayoutSessionReviewImage& image,
                                   const char* capture_stage,
                                   const char* target_plane,
                                   const char* plane_id,
                                   const char* parity_role,
                                   std::vector<std::string>* details)
{
    bool ok = true;
    ok &= check_string(details, image.capture_stage, capture_stage, "capture_stage");
    ok &= check_string(
        details,
        image.purpose,
        "dry_physical_target_height_parallax_diagnostic",
        "purpose");
    ok &= check_string(details, image.target_plane, target_plane, "target_plane");
    ok &= check_string(details, image.role, "physical_target", "image role");
    ok &= check_string(details, image.metadata.wet_or_dry, "dry", "wet_or_dry");
    ok &= check_string_one_of(
        details,
        image.metadata.fill_state,
        "dry_or_empty",
        "not_applicable",
        "fill_state");
    ok &= check_bool(details, image.metadata.open_water_surface_present, false, "open_water_surface_present");
    ok &= check_string(details, image.metadata.water_settled_status, "not_applicable", "water_settled_status");
    ok &= check_bool(details, image.metadata.reference_only, true, "reference_only");
    ok &= check_bool(details, image.metadata.physical_target_used, true, "physical_target_used");
    ok &= check_bool(
        details,
        image.metadata.projected_pattern_used_as_coordinate_target,
        false,
        "projected_pattern_used_as_coordinate_target");
    ok &= check_string(details, image.metadata.plane_id, plane_id, "plane_id");
    ok &= check_string(details, image.metadata.parity_group_role, parity_role, "parity_group_role");
    if (image.metadata.target_id.empty()) {
        add_detail(details, "target_id is missing");
        ok = false;
    }
    return ok;
}

bool check_common_wet_projection_surface(const SpatialLayoutSessionReviewImage& image,
                                         std::vector<std::string>* details)
{
    bool ok = true;
    ok &= check_string(details, image.capture_stage, "projected_surface_wet_runtime_stack", "capture_stage");
    ok &= check_string(details, image.target_plane, "projected_surface", "target_plane");
    ok &= check_string(details, image.metadata.wet_or_dry, "wet", "wet_or_dry");
    ok &= check_bool(details, image.metadata.imaging_shelf_installed, true, "imaging_shelf_installed");
    ok &= check_bool(details, image.metadata.dish_installed, true, "dish_installed");
    ok &= check_bool(details, image.metadata.open_water_surface_present, true, "open_water_surface_present");
    ok &= check_string(details, image.metadata.water_settled_status, "settled", "water_settled_status");
    ok &= check_bool(details, image.metadata.reference_only, false, "reference_only");
    ok &= check_string(details, image.metadata.pattern_domain, "circular_experimental_domain", "pattern_domain");
    ok &= check_string(details, image.metadata.parity_group_role, "wet_projected_surface", "parity_group_role");
    if (image.metadata.matched_parity_group_id.empty()) {
        add_detail(details, "matched_parity_group_id is missing");
        ok = false;
    }
    return ok;
}

bool check_wet_projection_product(const SpatialLayoutSessionReviewImage& image,
                                  const MatrixExpectationKind kind,
                                  std::vector<std::string>* details)
{
    bool ok = check_common_wet_projection_surface(image, details);
    switch (kind) {
    case MatrixExpectationKind::WetScale:
        ok &= check_string(details, image.purpose, "scale_image", "purpose");
        ok &= check_string(details, image.role, "scale_target", "image role");
        ok &= check_string(details, image.metadata.target_method, "ruler_only", "target_method");
        ok &= check_string(details, image.metadata.pattern_type, "ruler", "pattern_type");
        break;
    case MatrixExpectationKind::WetHomography:
        ok &= check_string(details, image.purpose, "homography_grid", "purpose");
        ok &= check_string(details, image.role, "grid_on", "image role");
        ok &= check_string(details, image.metadata.target_method, "projected_pattern_on_diffuser", "target_method");
        ok &= check_string(details, image.metadata.pattern_type, "circular_rings", "pattern_type");
        break;
    case MatrixExpectationKind::WetCrosshair:
        ok &= check_string(details, image.purpose, "crosshair_alignment", "purpose");
        ok &= check_string(details, image.role, "crosshair_on", "image role");
        ok &= check_string(details, image.metadata.pattern_type, "crosshair", "pattern_type");
        break;
    case MatrixExpectationKind::WetValidation:
        ok &= check_string(details, image.purpose, "validation_pattern", "purpose");
        ok &= check_string(details, image.role, "validation_pattern_on", "image role");
        ok &= check_string(details, image.metadata.pattern_type, "validation_pattern", "pattern_type");
        break;
    default:
        break;
    }
    return ok;
}

bool check_dish_top_observation(const SpatialLayoutSessionReviewImage& image,
                                std::vector<std::string>* details)
{
    bool ok = true;
    ok &= check_string(details, image.capture_stage, "dish_top_observation", "capture_stage");
    ok &= check_string(details, image.target_plane, "dish_top_rim", "target_plane");
    ok &= check_string(details, image.purpose, "dish_top_rim", "purpose");
    if (!image.has_accepted_circle && !image.has_linked_accepted_top_rim) {
        add_detail(details, "no accepted or linked top-rim circle was found");
        ok = false;
    }
    return ok;
}

bool check_expectation(const SpatialLayoutSessionReviewImage& image,
                       const MatrixExpectationKind kind,
                       std::vector<std::string>* details)
{
    switch (kind) {
    case MatrixExpectationKind::DryArena:
    case MatrixExpectationKind::DryScale:
    case MatrixExpectationKind::DryHomography:
    case MatrixExpectationKind::DryValidation:
        return check_dry_projection_product(image, kind, details);
    case MatrixExpectationKind::PhysicalProjectedSurface:
        return check_physical_target_product(
            image,
            "camera_physical_projected_surface",
            "projected_surface",
            "projected_surface_physical",
            "physical_projected_surface",
            details);
    case MatrixExpectationKind::PhysicalDishBase:
        return check_physical_target_product(
            image,
            "camera_physical_dish_base_inner_surface",
            "tank_bottom_inner_surface",
            "dish_base_inner_surface_physical",
            "physical_dish_base",
            details);
    case MatrixExpectationKind::PhysicalFishHeight:
        return check_physical_target_product(
            image,
            "camera_physical_fish_height",
            "estimated_fish_plane",
            "fish_height_physical_assumed",
            "physical_fish_height",
            details);
    case MatrixExpectationKind::WetScale:
    case MatrixExpectationKind::WetHomography:
    case MatrixExpectationKind::WetCrosshair:
    case MatrixExpectationKind::WetValidation:
        return check_wet_projection_product(image, kind, details);
    case MatrixExpectationKind::DishTopObservation:
        return check_dish_top_observation(image, details);
    }
    return false;
}

const MatrixExpectation* expectation_for_cell(const int row, const int column)
{
    for (const MatrixExpectation& expectation : kExpectations) {
        if (expectation.row == row && expectation.column == column) {
            return &expectation;
        }
    }
    return nullptr;
}

MatrixCellEvaluation evaluate_cell(const SpatialLayoutUiState& ui_state,
                                   const MatrixExpectation& expectation)
{
    MatrixCellEvaluation evaluation;
    evaluation.expectation = &expectation;
    evaluation.state = MatrixCellState::Missing;
    std::vector<std::string> first_error_details;
    int first_error_index = -1;

    for (size_t image_index = 0; image_index < ui_state.session_review_images.size(); ++image_index) {
        const SpatialLayoutSessionReviewImage& image =
            ui_state.session_review_images[image_index];
        if (!is_candidate_for_expectation(image, expectation.kind)) {
            continue;
        }
        ++evaluation.candidate_count;
        std::vector<std::string> details;
        if (check_expectation(image, expectation.kind, &details)) {
            evaluation.state = MatrixCellState::Complete;
            evaluation.best_image_index = static_cast<int>(image_index);
            evaluation.details = {
                "Matched " + image.label
            };
            return evaluation;
        }
        if (first_error_index < 0) {
            first_error_index = static_cast<int>(image_index);
            first_error_details = std::move(details);
        }
    }

    if (evaluation.candidate_count > 0) {
        evaluation.state = MatrixCellState::Error;
        evaluation.best_image_index = first_error_index;
        evaluation.details = std::move(first_error_details);
    } else {
        evaluation.details = {"Expected artifact has not been saved in this session."};
    }
    return evaluation;
}

ImVec4 color_for_cell_state(const MatrixCellState state)
{
    switch (state) {
    case MatrixCellState::Complete:
        return ImVec4(0.18f, 0.52f, 0.28f, 1.0f);
    case MatrixCellState::Error:
        return ImVec4(0.72f, 0.18f, 0.16f, 1.0f);
    case MatrixCellState::Missing:
        return ImVec4(0.26f, 0.28f, 0.31f, 1.0f);
    case MatrixCellState::NotExpected:
        return ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return ImVec4(0.26f, 0.28f, 0.31f, 1.0f);
}

const char* text_for_cell_state(const MatrixCellState state)
{
    switch (state) {
    case MatrixCellState::Complete:
        return "OK";
    case MatrixCellState::Error:
        return "!";
    case MatrixCellState::Missing:
        return "X";
    case MatrixCellState::NotExpected:
        return "";
    }
    return "";
}

std::string state_label(const MatrixCellState state)
{
    switch (state) {
    case MatrixCellState::Complete:
        return "complete";
    case MatrixCellState::Error:
        return "error";
    case MatrixCellState::Missing:
        return "missing";
    case MatrixCellState::NotExpected:
        return "not expected";
    }
    return "unknown";
}

std::string build_tooltip(const MatrixCellEvaluation& evaluation)
{
    std::ostringstream oss;
    if (evaluation.expectation != nullptr) {
        oss << evaluation.expectation->label << '\n';
    }
    oss << "State: " << state_label(evaluation.state) << '\n';
    oss << "Candidates: " << evaluation.candidate_count;
    for (const std::string& detail : evaluation.details) {
        oss << '\n' << detail;
    }
    return oss.str();
}

void render_selected_cell_details(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.selected_session_capture_matrix_row < 0 ||
        ui_state.selected_session_capture_matrix_column < 0 ||
        ui_state.selected_session_capture_matrix_row >= static_cast<int>(kRows.size()) ||
        ui_state.selected_session_capture_matrix_column >= static_cast<int>(kColumns.size())) {
        return;
    }
    const MatrixExpectation* expectation =
        expectation_for_cell(
            ui_state.selected_session_capture_matrix_row,
            ui_state.selected_session_capture_matrix_column);
    if (expectation == nullptr) {
        return;
    }

    const MatrixCellEvaluation evaluation = evaluate_cell(ui_state, *expectation);
    ImGui::SeparatorText("Selected Capture Cell");
    ImGui::Text(
        "%s / %s: %s",
        kRows[static_cast<size_t>(expectation->row)].label,
        kColumns[static_cast<size_t>(expectation->column)].label,
        state_label(evaluation.state).c_str());
    ImGui::TextDisabled("%s", expectation->label);
    if (evaluation.best_image_index >= 0 &&
        evaluation.best_image_index < static_cast<int>(ui_state.session_review_images.size())) {
        const SpatialLayoutSessionReviewImage& image =
            ui_state.session_review_images[static_cast<size_t>(evaluation.best_image_index)];
        ImGui::TextWrapped("%s", image.label.c_str());
        if (!image.image_set_path.empty()) {
            ImGui::TextDisabled("%s", image.image_set_path.c_str());
        }
    }
    for (const std::string& detail : evaluation.details) {
        ImGui::BulletText("%s", detail.c_str());
    }
}

}  // namespace

void render_session_capture_matrix_panel(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    ImGui::SeparatorText("Session Capture Matrix");
    if (ui_state->calibration_session_id.empty() &&
        ui_state->loaded_calibration_session_index_path.empty()) {
        ImGui::TextDisabled("Start or load a calibration session to track expected captures.");
    } else {
        ImGui::TextDisabled(
            "Green=complete, red=metadata error, X=expected but not saved.");
    }

    int complete_count = 0;
    int error_count = 0;
    int missing_count = 0;
    for (const MatrixExpectation& expectation : kExpectations) {
        const MatrixCellEvaluation evaluation = evaluate_cell(*ui_state, expectation);
        if (evaluation.state == MatrixCellState::Complete) {
            ++complete_count;
        } else if (evaluation.state == MatrixCellState::Error) {
            ++error_count;
        } else if (evaluation.state == MatrixCellState::Missing) {
            ++missing_count;
        }
    }
    ImGui::Text(
        "Complete %d / %zu   Missing %d   Errors %d",
        complete_count,
        kExpectations.size(),
        missing_count,
        error_count);

    if (ImGui::BeginTable(
            "SessionCaptureMatrix",
            static_cast<int>(kColumns.size()) + 1,
            ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        for (const MatrixColumn& column : kColumns) {
            ImGui::TableSetupColumn(column.label, ImGuiTableColumnFlags_WidthStretch, 1.0f);
        }
        ImGui::TableHeadersRow();

        for (size_t row_index = 0; row_index < kRows.size(); ++row_index) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kRows[row_index].label);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", kRows[row_index].description);
            }

            for (size_t column_index = 0; column_index < kColumns.size(); ++column_index) {
                ImGui::TableSetColumnIndex(static_cast<int>(column_index) + 1);
                const MatrixExpectation* expectation =
                    expectation_for_cell(
                        static_cast<int>(row_index),
                        static_cast<int>(column_index));
                if (expectation == nullptr) {
                    ImGui::TextDisabled("-");
                    continue;
                }
                const MatrixCellEvaluation evaluation = evaluate_cell(*ui_state, *expectation);
                const bool selected =
                    ui_state->selected_session_capture_matrix_row ==
                        static_cast<int>(row_index) &&
                    ui_state->selected_session_capture_matrix_column ==
                        static_cast<int>(column_index);
                const ImVec4 color = color_for_cell_state(evaluation.state);
                const ImVec4 hover_color = ImVec4(
                    std::min(color.x + 0.10f, 1.0f),
                    std::min(color.y + 0.10f, 1.0f),
                    std::min(color.z + 0.10f, 1.0f),
                    color.w);
                ImGui::PushID(static_cast<int>(row_index * kColumns.size() + column_index));
                ImGui::PushStyleColor(ImGuiCol_Button, color);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_color);
                ImGui::PushStyleColor(
                    ImGuiCol_ButtonActive,
                    selected ? ImVec4(0.20f, 0.42f, 0.82f, 1.0f) : hover_color);
                if (ImGui::Button(text_for_cell_state(evaluation.state), ImVec2(-1.0f, 0.0f))) {
                    ui_state->selected_session_capture_matrix_row =
                        static_cast<int>(row_index);
                    ui_state->selected_session_capture_matrix_column =
                        static_cast<int>(column_index);
                    if (evaluation.best_image_index >= 0) {
                        ui_state->selected_session_review_image = evaluation.best_image_index;
                    }
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) {
                    const std::string tooltip = build_tooltip(evaluation);
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    render_selected_cell_details(*ui_state);
}

}  // namespace orange::gui::spatial_layout
