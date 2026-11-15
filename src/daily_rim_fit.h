// Daily dish-rim fit with a known radius (docs/daily_registration_autorun_plan.md).
//
// The dish's inner diameter is known (design 80.0 mm, calipered 80.15 +/- 0.3 mm,
// Jeremy 2026-11-15), so the radius in camera px at the rim-top plane is known
// from the fish-plane scale model. The primary fit therefore fixes the radius and
// fits only the centre, which stays well determined on a partial arc (Cam2010094's
// rim reaches the bottom of its frame and the camera cannot be moved without a
// recommission). A free-radius fit is kept as a cross-check for a wrong edge or a
// wrong dish.
//
// Edge selection: on each of `rays` radial rays from the starting centre, the
// bright-to-dark steps are found with the registration accuracy check's rule, and
// the one NEAREST the known radius is taken: the wall top (Jeremy 2026-11-15, the
// wall top only; the floor edge, 16-32 px inside, is an illumination edge). Each
// window is clipped at the frame, never discarded; an edge needs the operator's
// half-width of real samples on both sides.
//
// Pure C++ on an 8-bit grey buffer: no OpenCV, no GUI, no files.
#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::daily_rim_fit {

inline constexpr const char* kFitMethodId = "orange_daily_rim_fit_known_radius_v1";

struct Config {
    int rays = 720;
    // The edge rule of scripts/registration_accuracy_check.py (Jeremy approved its
    // wall-top fit, 2026-11-15): derivative of Gaussian, sigma 2 px (half-width 6 px,
    // so an edge point is at least 6 px inside the frame), a step of at least
    // max(0.4 DN/px, 1.2 % of the ray's floor-to-dark contrast), contrast >= 8 DN.
    double search_inward_px = 80.0;   // window: expected radius - this ...
    double search_outward_px = 110.0; // ... to expected radius + this, clipped at the frame
    double border_margin_px = 1.0;    // bilinear sampling needs one pixel
    double edge_sigma_px = 2.0;
    double min_step_dn = 0.4;
    double step_fraction_of_contrast = 0.012;
    double min_contrast_dn = 8.0;
    int tangential_samples = 5;            // each radial sample averages these points ...
    double tangential_half_width_px = 4.0; // ... spread over +/- this along the rim
    double inlier_px = 3.0;           // smallest inlier tolerance on |distance - model radius|
    double max_inlier_px = 12.0;      // largest (follows 3 x 1.4826 x MAD in between): the IR rim edge wanders +/- 8-12 px; the floor edge is 16-32 px inside
    bool model_ellipticity = true;    // fit a cos/sin(2t) term: the rim is a slight ellipse in the image
    int iterations = 3;               // resample rays from the refined centre this many times
    // Refusal limits, proposed from evidence (docs/daily_registration_autorun_plan.md):
    // on the 2026-11-04 IR rim captures (the production optical path) all four cameras
    // give 674-704 inliers of 720, coverage 0.97-1.00, largest gap <= 5 deg, centre
    // sigma <= 0.71 px, free - known radius +2.9..+12.6 px, fixed - free centre <= 0.98 px.
    // Every dish's inner rim is inside its camera's view by design (Jeremy 2026-11-15;
    // Cam2010094's within ~1 px of the bottom), so coverage well below 360 deg is a fault.
    int min_inliers = 180;
    double min_coverage_fraction = 0.90;
    double max_gap_deg = 20.0;
    double report_gap_min_deg = 3.0;  // gaps at least this wide are listed with a reason
    double max_center_sigma_px = 1.0;
    double max_free_radius_delta_px = 20.0;   // free fit vs the known radius
    double max_fixed_free_center_px = 3.0;    // fixed- vs free-radius centre
};

// Why a ray has no inlier edge point.
enum class RayStatus { inlier, off_rim_edge, no_edge, frame_border };

struct RayRecord {
    double angle_deg = 0.0;
    RayStatus status = RayStatus::no_edge;
};

struct ArcGap {
    double start_deg = 0.0;   // image angle: 0 = +x (right), 90 = +y (down)
    double width_deg = 0.0;
    std::string side;
    std::string reason;       // frame_border / no_edge / off_rim_edge (the majority of its rays)
    int rays_frame_border = 0;
    int rays_no_edge = 0;
    int rays_off_rim_edge = 0;
};

struct EdgePoint {
    double angle_deg = 0.0;
    double x = 0.0;
    double y = 0.0;
    double step_dn = 0.0;
    bool inlier = false;
};

struct CircleFit {
    bool ok = false;
    double cx = 0.0;
    double cy = 0.0;
    double radius_px = 0.0;
    int inliers = 0;
    double residual_median_px = 0.0;
    double residual_max_px = 0.0;
    double sigma_cx_px = 0.0;
    double sigma_cy_px = 0.0;
    double sigma_radius_px = 0.0;     // free fit only
    double ellipticity_px = 0.0;      // amplitude of the cos/sin(2t) radius term
    double ellipticity_axis_deg = 0.0;
    double inlier_tolerance_px = 0.0;
};

struct Result {
    bool ok = false;                  // the fit is usable (no refusal)
    std::vector<std::string> refusals;
    std::vector<std::string> warnings;
    double expected_radius_px = 0.0;
    int rays = 0;
    int rays_excluded_border = 0;
    int rays_without_edge = 0;
    CircleFit fixed;                  // primary: radius fixed at expected_radius_px
    CircleFit free;                   // cross-check
    double coverage_fraction = 0.0;   // fraction of 360 one-degree bins holding an inlier
    double largest_gap_deg = 0.0;
    double largest_gap_center_deg = 0.0;   // image angle: 0 = +x (right), 90 = +y (down)
    std::string largest_gap_side;          // right / bottom / left / top
    double nearest_border_px = 0.0;        // fitted circle to the nearest frame edge (negative = outside)
    double fixed_minus_free_center_px = 0.0;
    std::vector<EdgePoint> points;
    std::vector<RayRecord> ray_records;
    std::vector<ArcGap> gaps;         // every uncovered arc of at least report_gap_min_deg
};

// expected_radius_px: the known radius at the rim-top plane, in this camera's px.
// start_cx/start_cy: a starting centre (e.g. the Hough proposal); the fit iterates from it.
Result fit_known_radius(const std::uint8_t* gray, int width, int height,
                        double start_cx, double start_cy, double expected_radius_px,
                        const Config& config = {});

nlohmann::json to_json(const Result& result, const Config& config, double mm_per_px = 0.0,
                       bool include_points = false);

// The known radius in camera px at the rim-top plane, from the fish-plane scale file
// (orange.calibration.fish_plane_scale v1): s(z) = 1 / (1/s_floor - (z - P_floor - e)/k)
// at z = P_floor + wall height (the same model as orange_dish_rim_auto_accept_v1).
struct KnownRadius {
    bool ok = false;
    std::string error;
    std::string scale_file_sha256;
    double inner_diameter_mm = 0.0;
    double wall_height_mm = 0.0;
    double z_rim_top_mm = 0.0;
    double px_per_mm = 0.0;
    double radius_px = 0.0;
};

// Calipered 2026-11-15 (Jeremy): 80.4, 79.84, 80.2 mm, consistent at 0/60/120 degrees.
inline constexpr double kCaliperedInnerDiameterMm = 80.15;
inline constexpr double kCaliperedInnerDiameterUncertaintyMm = 0.3;
inline constexpr double kDishWallHeightMm = 2.8;

KnownRadius known_radius_from_scale_file(const std::string& scale_file_path, const std::string& camera_serial,
                                         double inner_diameter_mm = kCaliperedInnerDiameterMm,
                                         double wall_height_mm = kDishWallHeightMm);

}  // namespace orange::daily_rim_fit
