// Synthetic checks of the known-radius daily rim fit (src/daily_rim_fit.h).
#include "daily_rim_fit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    } else {
        std::cout << "ok: " << what << "\n";
    }
}

struct Scene {
    int width = 1400;
    int height = 1400;
    double cx = 700.0;
    double cy = 700.0;
    double radius = 600.0;          // where the rim's inner bright-to-dark edge is
    double ellipticity_px = 0.0;    // cos(2t) amplitude of the edge radius
    bool ir_dark_ring = true;       // IR look: bright, a 10 px dark ring, bright outside
    double decoy_inner_px = 0.0;    // an extra bright-to-dark step this far inside (floor edge)
    double noise = 2.0;
    double bump_px = 0.0;           // the edge pushed outward by this much over one arc
    double bump_start_deg = 0.0;
    double bump_width_deg = 0.0;
};

std::vector<std::uint8_t> render(const Scene& s, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, s.noise);
    std::vector<std::uint8_t> img(static_cast<std::size_t>(s.width) * s.height);
    for (int y = 0; y < s.height; ++y) {
        for (int x = 0; x < s.width; ++x) {
            const double dx = x - s.cx;
            const double dy = y - s.cy;
            const double d = std::hypot(dx, dy);
            const double t = std::atan2(dy, dx);
            double edge = s.radius + s.ellipticity_px * std::cos(2.0 * t);
            const double deg = std::fmod(t * 180.0 / 3.14159265358979323846 + 360.0, 360.0);
            if (s.bump_px != 0.0 && deg >= s.bump_start_deg && deg < s.bump_start_deg + s.bump_width_deg) {
                edge += s.bump_px;
            }
            double v;
            if (s.decoy_inner_px > 0.0 && d < edge - s.decoy_inner_px) {
                v = 200.0;
            } else if (d < edge) {
                v = s.decoy_inner_px > 0.0 ? 140.0 : 200.0;
            } else if (s.ir_dark_ring && d < edge + 10.0) {
                v = 70.0;
            } else {
                v = s.ir_dark_ring ? 190.0 : 15.0;
            }
            // soften edges by a 1.5 px ramp
            const double ramp = std::clamp((d - edge) / 1.5, -1.0, 1.0);
            (void)ramp;
            v += noise(rng);
            img[static_cast<std::size_t>(y) * s.width + x] = static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
        }
    }
    return img;
}

namespace fit = orange::daily_rim_fit;

}  // namespace

int main()
{
    {
        Scene s;
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 690.0, 712.0, 600.0);
        expect(r.ok, "IR-like full rim: fit ok");
        expect(std::hypot(r.fixed.cx - 700.0, r.fixed.cy - 700.0) < 1.0, "IR-like full rim: centre within 1 px");
        expect(r.coverage_fraction > 0.95 && r.largest_gap_deg < 10.0, "IR-like full rim: full coverage");
        expect(std::fabs(r.free.radius_px - 600.0) < 2.0, "IR-like full rim: free radius matches");
    }
    {
        Scene s;
        s.ellipticity_px = 5.0;
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 700.0, 600.0);
        expect(r.ok, "elliptical rim (5 px cos2t): fit ok");
        expect(std::hypot(r.fixed.cx - 700.0, r.fixed.cy - 700.0) < 1.0, "elliptical rim: centre within 1 px");
        expect(std::fabs(r.fixed.ellipticity_px - 5.0) < 1.5, "elliptical rim: ellipticity measured");
    }
    {
        // 2026-11-15 rig trial (2010093): a sharp edge with a coherent 21 deg arc 5-10 px
        // outside the ellipse model. A tolerance that followed the trimmed points shrank
        // below that arc and refused the rim on coverage; the tolerance from the
        // provisional all-point fit, floored at 8 px, keeps it.
        Scene s;
        s.noise = 1.0;
        s.ellipticity_px = 2.5;
        s.bump_px = 7.0;
        s.bump_start_deg = 308.0;
        s.bump_width_deg = 25.0;
        auto img = render(s);
        const fit::Config config;
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 700.0, 600.0, config);
        expect(r.ok, "coherent +7 px arc over 25 deg: fit ok");
        expect(r.coverage_fraction > 0.97 && r.largest_gap_deg < 5.0, "coherent +7 px arc: the arc stays covered");
        expect(std::hypot(r.fixed.cx - 700.0, r.fixed.cy - 700.0) < 1.5, "coherent +7 px arc: centre within 1.5 px");
        const double rule = std::clamp(std::max(config.inlier_scatter_factor * r.free.provisional_residual_median_px,
                                                config.inlier_floor_px), config.inlier_floor_px, config.max_inlier_px);
        expect(std::fabs(r.free.inlier_tolerance_px - rule) < 1e-9 && r.free.inlier_tolerance_px >= 8.0,
               "tolerance = clamp(max(k x provisional median, floor), floor, cap), never below 8 px");
        expect(r.free.inlier_tolerance_px >= r.free.residual_median_px && r.fixed.inlier_tolerance_px == r.free.inlier_tolerance_px,
               "tolerance not tighter than the inliers' scatter; fixed fit reports the same tolerance");
        // The same scene with a 3 px floor and the provisional factor at 1 reproduces the
        // rig refusal: the arc falls out.
        fit::Config tight = config;
        tight.inlier_floor_px = 3.0;
        tight.inlier_scatter_factor = 1.0;
        const auto t = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 700.0, 600.0, tight);
        expect(t.largest_gap_deg >= 20.0, "coherent +7 px arc with a 3 px tolerance: the arc is lost (rig refusal reproduced)");
    }
    {
        // The rim reaches past the bottom of the frame (Cam2010094's chronic case).
        Scene s;
        s.height = 1300;
        s.cy = 696.0;  // bottom of the rim at y = 1296: within the 8 px border margin
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 690.0, 600.0);
        expect(r.ok, "rim 3 px from the frame bottom: not refused");
        expect(r.coverage_fraction > 0.95, "rim 3 px from the frame bottom: coverage above 95 %");
        expect(std::hypot(r.fixed.cx - 700.0, r.fixed.cy - 696.0) < 1.0, "rim 3 px from the frame bottom: centre within 1 px");
    }
    {
        // A rim cut by the frame (a placement fault): refused, the arc named at the frame border.
        Scene cut;
        cut.height = 1250;
        cut.cy = 696.0;  // bottom of the rim at y = 1296: 50 px outside the frame
        auto cut_img = render(cut);
        const auto rc = fit::fit_known_radius(cut_img.data(), cut.width, cut.height, 700.0, 690.0, 600.0);
        bool border_arc = false;
        for (const auto& gap : rc.gaps) border_arc = border_arc || (gap.side == "bottom" && gap.reason == "frame_border");
        expect(!rc.ok && border_arc, "rim cut by the frame: refused, bottom arc reason frame_border");
    }
    {
        // Rim mostly outside the frame: only a short arc is visible.
        Scene s;
        s.width = 900;
        s.cx = 1100.0;  // only the left part of the rim is in frame
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 1100.0, 700.0, 600.0);
        expect(!r.ok, "short visible arc: refused");
    }
    {
        // A floor edge 28 px inside the rim: the edge nearest the known radius wins.
        Scene s;
        s.decoy_inner_px = 28.0;
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 700.0, 600.0);
        expect(r.ok, "floor-edge decoy: fit ok");
        expect(std::fabs(r.free.radius_px - 600.0) < 2.0, "floor-edge decoy: wall top chosen, not the floor edge");
    }
    {
        // The known radius is wrong by 30 px (another dish or a wrong model).
        Scene s;
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 700.0, 700.0, 630.0);
        expect(!r.ok, "known radius 30 px off: refused");
        bool named = false;
        for (const auto& reason : r.refusals) named = named || reason == "free_radius_disagrees_with_known_radius";
        expect(named, "known radius 30 px off: refusal names the radius disagreement");
    }
    {
        // Grey look: bright floor, dark outside.
        Scene s;
        s.ir_dark_ring = false;
        auto img = render(s);
        const auto r = fit::fit_known_radius(img.data(), s.width, s.height, 705.0, 695.0, 603.0);
        expect(r.ok, "step edge (filters off): fit ok");
        expect(std::hypot(r.fixed.cx - 700.0, r.fixed.cy - 700.0) < 1.5, "step edge: centre within 1.5 px");
    }
    {
        const auto j = fit::to_json(fit::Result{}, fit::Config{}, 0.0187);
        expect(j.at("method") == fit::kFitMethodId && j.contains("limits"), "json carries method and limits");
    }
    std::cout << (failures == 0 ? "daily_rim_fit_tests passed" : "daily_rim_fit_tests FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
