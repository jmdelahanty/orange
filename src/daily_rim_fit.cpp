#include "daily_rim_fit.h"

#include "outputs/common/digest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>

namespace orange::daily_rim_fit {
namespace {

constexpr double kPi = 3.14159265358979323846;

double bilinear(const std::uint8_t* gray, int width, double x, double y)
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    const auto at = [&](int xx, int yy) {
        return static_cast<double>(gray[static_cast<std::size_t>(yy) * width + xx]);
    };
    return (1.0 - fx) * (1.0 - fy) * at(x0, y0) + fx * (1.0 - fy) * at(x0 + 1, y0) +
           (1.0 - fx) * fy * at(x0, y0 + 1) + fx * fy * at(x0 + 1, y0 + 1);
}

struct RayResult {
    bool truncated = false;
    bool found = false;
    bool ring_step_without_valley = false;   // ir_dark_ring: no valley, the strongest step taken
    EdgePoint point;
};

// One ray, the registration accuracy check's wall-top edge rule
// (scripts/registration_accuracy_check.py, approved by Jeremy 2026-11-15): samples at
// 1 px from (expected - search_inward) to (expected + search_outward), clipped where
// the frame begins (never discarded for crossing it); the dark level is the darker of
// the window's end and its darkest 5 px stretch (the IR rim ring). Bright-to-dark steps from a
// derivative of Gaussian (sigma edge_sigma_px, half-width 3 sigma; only positions
// with a full half-width of real samples on both sides, so an edge point is at least
// the half-width inside the frame), at least max(min_step_dn, step_fraction x the
// ray's floor-to-dark contrast) deep. Rays with less than min_contrast_dn contrast
// have no edge. The step nearest the known radius wins (the wall top).
//
// ir_dark_ring (EdgeModel): the strongest valley of the matched dark-line response in
// the window (min_valley_dn deep at least), placed at its centre line or at its inner
// edge (the steepest bright-to-dark step between the inner flank and the valley centre).
RayResult sample_ray(const std::uint8_t* gray, int width, int height, double cx, double cy,
                     double angle_rad, double expected, const Config& config, EdgeModel model,
                     bool probe_only = false)
{
    RayResult out;
    const double ux = std::cos(angle_rad);
    const double uy = std::sin(angle_rad);
    const double lo = std::max(1.0, expected - config.search_inward_px);
    const double hi = expected + config.search_outward_px;
    const double m = std::max(1.0, config.border_margin_px);  // bilinear sampling needs one pixel
    std::vector<double> radius;
    std::vector<double> value;
    // Each radial sample averages `tangential_samples` points spread over
    // +/- tangential_half_width_px along the rim (single-pixel rays on the IR rim ring
    // scatter by +/- 8-12 px).
    const int taps = std::max(1, config.tangential_samples);
    const double tx = -uy;
    const double ty = ux;
    for (double r = lo; r <= hi; r += 1.0) {
        double sum = 0.0;
        bool inside = true;
        for (int t = 0; t < taps && inside; ++t) {
            const double offset = taps == 1 ? 0.0
                : -config.tangential_half_width_px + 2.0 * config.tangential_half_width_px * t / (taps - 1);
            const double x = cx + r * ux + offset * tx;
            const double y = cy + r * uy + offset * ty;
            if (x < m || y < m || x > width - 1.0 - m || y > height - 1.0 - m) {
                inside = false;
                break;
            }
            sum += bilinear(gray, width, x, y);
        }
        if (!inside) {
            out.truncated = true;
            break;
        }
        radius.push_back(r);
        value.push_back(sum / taps);
    }
    const int n = static_cast<int>(value.size());
    const int half = static_cast<int>(3.0 * config.edge_sigma_px);
    if (n < 30 || n < 2 * half + 3) return out;
    // Contrast: the floor level (first 20 samples) minus the dark level (last 15, or
    // the minimum on a short, clipped window).
    const auto median_of = [](std::vector<double> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const double level_in = median_of(std::vector<double>(value.begin(), value.begin() + 20));
    double level_out = n >= 60 ? median_of(std::vector<double>(value.end() - 15, value.end()))
                               : *std::min_element(value.begin(), value.end());
    // On the production IR rim capture the rim is a ~10 px dark ring with a bright
    // field again outside it, so the window's end is not dark: take the darkest 5 px
    // stretch when it is darker (unchanged on filters-off frames, dark outside).
    for (int i = 0; i + 5 <= n; ++i) {
        double sum = 0.0;
        for (int k = 0; k < 5; ++k) sum += value[i + k];
        level_out = std::min(level_out, sum / 5.0);
    }
    const double contrast = level_in - level_out;
    // Derivative-of-Gaussian kernel with a unit response to a unit ramp.
    std::vector<double> kernel(2 * half + 1);
    double norm = 0.0;
    for (int k = -half; k <= half; ++k) {
        // d/dr of a Gaussian-smoothed profile: positive where intensity rises outward.
        const double g = k * std::exp(-0.5 * (k / config.edge_sigma_px) * (k / config.edge_sigma_px));
        kernel[k + half] = g;
        norm += std::fabs(g * k);
    }
    for (double& v : kernel) v /= norm;
    // step[i] > 0: bright inside, darker outside (the negative of the gradient).
    std::vector<double> step(n, std::numeric_limits<double>::quiet_NaN());
    for (int i = half; i + half < n; ++i) {
        double gradient = 0.0;
        for (int k = -half; k <= half; ++k) gradient += kernel[k + half] * value[i + k];
        step[i] = -gradient;
    }
    const auto parabola = [](double a, double b, double c) {
        const double denominator = a - 2.0 * b + c;
        return std::clamp(std::fabs(denominator) > 1e-12 ? 0.5 * (a - c) / denominator : 0.0, -0.5, 0.5);
    };
    if (model == EdgeModel::ir_dark_ring) {
        const int core = static_cast<int>(std::lround(config.valley_core_half_px));
        const int gap = static_cast<int>(std::lround(config.valley_gap_px));
        const int flank = std::max(1, static_cast<int>(std::lround(config.valley_flank_px)));
        const int reach = core + gap + flank;
        if (n < 2 * reach + 3) return out;
        std::vector<double> prefix(n + 1, 0.0);
        for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + value[i];
        const auto mean = [&](int a, int b) { return (prefix[b + 1] - prefix[a]) / (b - a + 1); };  // [a, b]
        std::vector<double> valley(n, -std::numeric_limits<double>::infinity());
        for (int i = reach; i + reach < n; ++i) {
            const double inner = mean(i - reach, i - core - gap - 1);
            const double outer = mean(i + core + gap + 1, i + reach);
            valley[i] = std::min(inner, outer) - mean(i - core, i + core);
        }
        int best = -1;
        for (int i = reach + 1; i + reach + 1 < n; ++i) {
            if (valley[i] < valley[i - 1] || valley[i] <= valley[i + 1]) continue;  // local maximum
            if (best < 0 || valley[i] > valley[best]) best = i;
        }
        if (best < 0 || valley[best] < config.min_valley_dn) {
            // No ring on this ray (where the field outside the rim is dark the ring is a
            // plain step into the dark): the strongest bright-to-dark step of the window,
            // the same inner edge of the dark region. Probing for the model never does this.
            if (probe_only || contrast < config.min_contrast_dn) return out;
            int edge = -1;
            for (int k = half + 1; k + half + 1 < n; ++k) {
                if (!(step[k] > 0.0)) continue;
                if (step[k] < step[k - 1] || step[k] <= step[k + 1]) continue;
                if (edge < 0 || step[k] > step[edge]) edge = k;
            }
            if (edge < 0 || step[edge] < std::max(config.min_step_dn, config.step_fraction_of_contrast * contrast)) {
                return out;
            }
            const double r = radius[edge] + parabola(step[edge - 1], step[edge], step[edge + 1]);
            out.found = true;
            out.ring_step_without_valley = true;
            out.point.angle_deg = angle_rad * 180.0 / kPi;
            out.point.x = cx + r * ux;
            out.point.y = cy + r * uy;
            out.point.step_dn = step[edge];
            return out;
        }
        double r = radius[best] + parabola(valley[best - 1], valley[best], valley[best + 1]);
        if (config.ring_edge == RingEdge::inner_edge) {
            // The inner boundary of the whole dark band: the strongest bright-to-dark step
            // between max_ring_band_px inside the valley centre and the centre. The band
            // can be ~25 px wide, striped, with a grey fringe; its deepest sub-valley need
            // not be at its inner edge, and the black core's edge is the strongest step.
            int edge = -1;
            const int lo_index = std::max(half + 1, best - static_cast<int>(config.max_ring_band_px));
            for (int k = lo_index; k <= best && k + half + 1 < n; ++k) {
                if (!(step[k] > 0.0)) continue;
                if (step[k] < step[k - 1] || step[k] <= step[k + 1]) continue;  // local maximum
                if (edge < 0 || step[k] > step[edge]) edge = k;
            }
            if (edge < 0) return out;
            r = radius[edge] + parabola(step[edge - 1], step[edge], step[edge + 1]);
        }
        out.found = true;
        out.point.angle_deg = angle_rad * 180.0 / kPi;
        out.point.x = cx + r * ux;
        out.point.y = cy + r * uy;
        out.point.step_dn = valley[best];
        return out;
    }
    if (contrast < config.min_contrast_dn) return out;
    const double threshold = std::max(config.min_step_dn, config.step_fraction_of_contrast * contrast);
    double best_distance = std::numeric_limits<double>::infinity();
    int best = -1;
    for (int i = half + 1; i + half + 1 < n; ++i) {
        if (!(step[i] >= threshold)) continue;
        if (step[i] < step[i - 1] || step[i] <= step[i + 1]) continue;  // local maximum
        const double distance = std::fabs(radius[i] - expected);
        if (distance < best_distance) { best_distance = distance; best = i; }
    }
    if (best < 0) return out;
    // Sub-pixel: parabola through the step peak.
    const double a = step[best - 1];
    const double b = step[best];
    const double c = step[best + 1];
    const double denominator = a - 2.0 * b + c;
    const double offset = std::fabs(denominator) > 1e-12 ? 0.5 * (a - c) / denominator : 0.0;
    const double r = radius[best] + std::clamp(offset, -0.5, 0.5);
    out.found = true;
    out.point.angle_deg = angle_rad * 180.0 / kPi;
    out.point.x = cx + r * ux;
    out.point.y = cy + r * uy;
    out.point.step_dn = b;
    return out;
}

double median(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

// Model: distance from the centre = R + ec*cos(2t) + es*sin(2t), t = the point's
// angle. The second harmonic absorbs the rim's slight ellipticity in the image
// (about 0.4 % on the 2026-11-04 IR captures: lens distortion or a small tilt), so
// the centre stays unbiased on a partial arc. Parameters: cx, cy, [R if free], ec, es.
struct Model {
    bool free_radius = false;
    bool harmonic = true;
    double cx = 0.0, cy = 0.0, r = 0.0, ec = 0.0, es = 0.0;
    int dof() const { return 2 + (free_radius ? 1 : 0) + (harmonic ? 2 : 0); }
    double radius_at(double t) const { return r + ec * std::cos(2.0 * t) + es * std::sin(2.0 * t); }
};

// Solve a (n x n) * x = b by Gaussian elimination with partial pivoting; also returns
// the inverse of a (for the covariance) when inverse_out is given.
bool solve(std::vector<double> a, std::vector<double> b, int n, std::vector<double>* x,
           std::vector<double>* inverse_out)
{
    std::vector<double> inv(n * n, 0.0);
    for (int i = 0; i < n; ++i) inv[i * n + i] = 1.0;
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::fabs(a[r * n + col]) > std::fabs(a[pivot * n + col])) pivot = r;
        }
        if (std::fabs(a[pivot * n + col]) < 1e-12) return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(a[col * n + c], a[pivot * n + c]);
                std::swap(inv[col * n + c], inv[pivot * n + c]);
            }
            std::swap(b[col], b[pivot]);
        }
        const double d = a[col * n + col];
        for (int c = 0; c < n; ++c) { a[col * n + c] /= d; inv[col * n + c] /= d; }
        b[col] /= d;
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a[r * n + col];
            if (f == 0.0) continue;
            for (int c = 0; c < n; ++c) { a[r * n + c] -= f * a[col * n + c]; inv[r * n + c] -= f * inv[col * n + c]; }
            b[r] -= f * b[col];
        }
    }
    *x = b;
    if (inverse_out) *inverse_out = inv;
    return true;
}

double point_residual(const EdgePoint& p, const Model& m)
{
    const double t = std::atan2(p.y - m.cy, p.x - m.cx);
    return std::hypot(p.x - m.cx, p.y - m.cy) - m.radius_at(t);
}

// Gauss-Newton over the points marked inlier. inverse_out: (J^T J)^-1 at the solution.
bool gauss_newton(const std::vector<EdgePoint>& points, Model* m, std::vector<double>* inverse_out)
{
    const int n = m->dof();
    for (int iteration = 0; iteration < 40; ++iteration) {
        std::vector<double> jtj(n * n, 0.0);
        std::vector<double> jtr(n, 0.0);
        int used = 0;
        for (const auto& p : points) {
            if (!p.inlier) continue;
            const double dx = p.x - m->cx;
            const double dy = p.y - m->cy;
            const double d2 = dx * dx + dy * dy;
            const double d = std::sqrt(d2);
            if (d < 1e-9) continue;
            const double t = std::atan2(dy, dx);
            const double c2 = std::cos(2.0 * t);
            const double s2 = std::sin(2.0 * t);
            const double residual = d - m->radius_at(t);
            const double dmodel_dt = m->harmonic ? (-2.0 * m->ec * s2 + 2.0 * m->es * c2) : 0.0;
            std::vector<double> j;
            j.push_back(-dx / d - dmodel_dt * (dy / d2));
            j.push_back(-dy / d - dmodel_dt * (-dx / d2));
            if (m->free_radius) j.push_back(-1.0);
            if (m->harmonic) { j.push_back(-c2); j.push_back(-s2); }
            for (int r = 0; r < n; ++r) {
                jtr[r] += j[r] * residual;
                for (int c = 0; c < n; ++c) jtj[r * n + c] += j[r] * j[c];
            }
            ++used;
        }
        if (used < n + 1) return false;
        std::vector<double> rhs(n);
        for (int i = 0; i < n; ++i) rhs[i] = -jtr[i];
        std::vector<double> delta;
        if (!solve(jtj, rhs, n, &delta, inverse_out)) return false;
        int k = 0;
        m->cx += delta[k++];
        m->cy += delta[k++];
        if (m->free_radius) m->r += delta[k++];
        if (m->harmonic) { m->ec += delta[k++]; m->es += delta[k++]; }
        double step = 0.0;
        for (double v : delta) step = std::max(step, std::fabs(v));
        if (step < 1e-5) break;
    }
    return true;
}

// Robust fit: the tolerance comes once from the provisional fit over every point
// (Config::inlier_scatter_factor x median |residual|, between inlier_floor_px and
// max_inlier_px); a few rounds then refit on the points within it.
CircleFit robust_fit(std::vector<EdgePoint>* points, bool free_radius, double cx, double cy,
                     double radius, const Config& config)
{
    CircleFit fit;
    Model m;
    m.free_radius = free_radius;
    m.harmonic = config.model_ellipticity;
    m.cx = cx;
    m.cy = cy;
    m.r = radius;
    for (auto& p : *points) p.inlier = true;
    if (!gauss_newton(*points, &m, nullptr)) return fit;
    std::vector<double> provisional;
    for (const auto& p : *points) provisional.push_back(std::fabs(point_residual(p, m)));
    const double provisional_median = median(provisional);
    const double tolerance = std::clamp(std::max(config.inlier_scatter_factor * provisional_median,
                                                 config.inlier_floor_px),
                                        config.inlier_floor_px, std::max(config.inlier_floor_px, config.max_inlier_px));
    for (int round = 0; round < 6; ++round) {
        for (auto& p : *points) p.inlier = std::fabs(point_residual(p, m)) <= tolerance;
        if (!gauss_newton(*points, &m, nullptr)) return fit;
    }
    fit.provisional_residual_median_px = provisional_median;
    std::vector<double> inverse;
    if (!gauss_newton(*points, &m, &inverse)) return fit;
    std::vector<double> residuals;
    double sum_squares = 0.0;
    for (auto& p : *points) {
        const double r = std::fabs(point_residual(p, m));
        p.inlier = r <= tolerance;
        if (!p.inlier) continue;
        residuals.push_back(r);
        sum_squares += r * r;
    }
    const int dof = m.dof();
    const int n = static_cast<int>(residuals.size());
    if (n <= dof) return fit;
    const double sigma2 = sum_squares / (n - dof);
    fit.sigma_cx_px = std::sqrt(sigma2 * inverse[0 * dof + 0]);
    fit.sigma_cy_px = std::sqrt(sigma2 * inverse[1 * dof + 1]);
    if (free_radius) fit.sigma_radius_px = std::sqrt(sigma2 * inverse[2 * dof + 2]);
    fit.ok = true;
    fit.cx = m.cx;
    fit.cy = m.cy;
    fit.radius_px = m.r;
    fit.ellipticity_px = std::hypot(m.ec, m.es);
    fit.ellipticity_axis_deg = 0.5 * std::atan2(m.es, m.ec) * 180.0 / kPi;
    fit.inlier_tolerance_px = tolerance;
    fit.inliers = n;
    fit.residual_median_px = median(residuals);
    fit.residual_max_px = *std::max_element(residuals.begin(), residuals.end());
    return fit;
}

// Fit `m` by Gauss-Newton on the points already marked inlier, without re-trimming,
// and fill the statistics over those points.
CircleFit fit_on_inliers(const std::vector<EdgePoint>& points, Model m)
{
    CircleFit fit;
    std::vector<double> inverse;
    if (!gauss_newton(points, &m, &inverse)) return fit;
    std::vector<double> residuals;
    double sum_squares = 0.0;
    for (const auto& p : points) {
        if (!p.inlier) continue;
        const double r = point_residual(p, m);
        residuals.push_back(std::fabs(r));
        sum_squares += r * r;
    }
    const int dof = m.dof();
    const int n = static_cast<int>(residuals.size());
    if (n <= dof) return fit;
    const double sigma2 = sum_squares / (n - dof);
    fit.sigma_cx_px = std::sqrt(sigma2 * inverse[0]);
    fit.sigma_cy_px = std::sqrt(sigma2 * inverse[dof + 1]);
    if (m.free_radius) fit.sigma_radius_px = std::sqrt(sigma2 * inverse[2 * dof + 2]);
    fit.ok = true;
    fit.cx = m.cx;
    fit.cy = m.cy;
    fit.radius_px = m.r;
    fit.ellipticity_px = std::hypot(m.ec, m.es);
    fit.ellipticity_axis_deg = 0.5 * std::atan2(m.es, m.ec) * 180.0 / kPi;
    fit.inliers = n;
    fit.residual_median_px = median(residuals);
    fit.residual_max_px = *std::max_element(residuals.begin(), residuals.end());
    return fit;
}

std::string side_of(double angle_deg)
{
    const double a = std::fmod(std::fmod(angle_deg, 360.0) + 360.0, 360.0);
    if (a < 45.0 || a >= 315.0) return "right";
    if (a < 135.0) return "bottom";
    if (a < 225.0) return "left";
    return "top";
}

}  // namespace

const char* edge_model_name(EdgeModel model)
{
    switch (model) {
    case EdgeModel::wall_top_step: return "wall_top_step";
    case EdgeModel::ir_dark_ring: return "ir_dark_ring";
    case EdgeModel::automatic: break;
    }
    return "automatic";
}

const char* ring_edge_name(RingEdge edge)
{
    return edge == RingEdge::centre_line ? "centre_line" : "inner_edge";
}

Result fit_known_radius(const std::uint8_t* gray, int width, int height, double start_cx,
                        double start_cy, double expected_radius_px, const Config& config)
{
    Result result;
    result.expected_radius_px = expected_radius_px;
    result.rays = config.rays;
    if (gray == nullptr || width < 16 || height < 16 || !(expected_radius_px > 0.0) || config.rays < 36) {
        result.refusals.push_back("invalid_input");
        return result;
    }
    double cx = start_cx;
    double cy = start_cy;
    // Edge model: from the config, or from the profiles around the starting centre.
    EdgeModel model = config.edge_model;
    {
        int with_samples = 0;
        int with_valley = 0;
        for (int i = 0; i < config.rays; ++i) {
            const double angle = 2.0 * kPi * i / config.rays;
            const RayResult ray = sample_ray(gray, width, height, cx, cy, angle, expected_radius_px, config,
                                             EdgeModel::ir_dark_ring, true);
            if (ray.truncated && !ray.found) continue;
            ++with_samples;
            if (ray.found) ++with_valley;
        }
        result.ring_ray_fraction = with_samples > 0 ? static_cast<double>(with_valley) / with_samples : 0.0;
    }
    if (model == EdgeModel::automatic) {
        model = result.ring_ray_fraction >= config.ring_ray_fraction ? EdgeModel::ir_dark_ring
                                                                     : EdgeModel::wall_top_step;
        result.edge_model_source = "profile";
    } else {
        result.edge_model_source = "config";
    }
    result.edge_model = edge_model_name(model);
    if ((config.declared_nir_filters == "in" && model != EdgeModel::ir_dark_ring) ||
        (config.declared_nir_filters == "out" && model != EdgeModel::wall_top_step)) {
        result.warnings.push_back("edge_model_disagrees_with_declared_nir_filters");
    }
    std::vector<EdgePoint> points;
    for (int pass = 0; pass < std::max(1, config.iterations); ++pass) {
        points.clear();
        result.ray_records.clear();
        result.rays_excluded_border = 0;
        result.rays_without_edge = 0;
        result.rays_ring_step_without_valley = 0;
        for (int i = 0; i < config.rays; ++i) {
            const double angle = 2.0 * kPi * i / config.rays;
            const RayResult ray = sample_ray(gray, width, height, cx, cy, angle, expected_radius_px, config, model);
            RayRecord record;
            record.angle_deg = angle * 180.0 / kPi;
            if (ray.found) {
                if (ray.ring_step_without_valley) ++result.rays_ring_step_without_valley;
                points.push_back(ray.point);
                record.status = RayStatus::off_rim_edge;  // until the fit marks it an inlier
            } else if (ray.truncated) {
                ++result.rays_excluded_border;
                record.status = RayStatus::frame_border;
            } else {
                ++result.rays_without_edge;
                record.status = RayStatus::no_edge;
            }
            result.ray_records.push_back(record);
        }
        if (static_cast<int>(points.size()) < 8) break;
        // The free-radius robust fit decides which edge points belong to one rim: a
        // dish a few px off the known radius (dish tolerance, scale model) would
        // otherwise turn every point into an outlier of the fixed-radius model.
        const CircleFit free_fit = robust_fit(&points, true, cx, cy, expected_radius_px, config);
        if (!free_fit.ok) break;
        result.free = free_fit;
        // Primary: radius fixed at the known value, centre (and ellipticity) fitted on
        // the same inliers. A uniform radius offset does not bias the centre on a full
        // arc; on a partial arc the fixed and free centres separate, which is reported.
        Model fixed_model;
        fixed_model.free_radius = false;
        fixed_model.harmonic = config.model_ellipticity;
        fixed_model.cx = free_fit.cx;
        fixed_model.cy = free_fit.cy;
        fixed_model.r = expected_radius_px;
        CircleFit fixed = fit_on_inliers(points, fixed_model);
        if (!fixed.ok) break;
        fixed.inlier_tolerance_px = free_fit.inlier_tolerance_px;
        fixed.provisional_residual_median_px = free_fit.provisional_residual_median_px;
        result.fixed = fixed;
        cx = fixed.cx;
        cy = fixed.cy;
    }
    if (!result.fixed.ok || !result.free.ok) {
        result.refusals.push_back("rim_fit_failed");
        result.points = std::move(points);
        return result;
    }
    std::array<bool, 360> covered{};
    for (const auto& p : points) {
        if (!p.inlier) continue;
        double a = std::atan2(p.y - result.fixed.cy, p.x - result.fixed.cx) * 180.0 / kPi;
        if (a < 0.0) a += 360.0;
        covered[static_cast<int>(a) % 360] = true;
    }
    const int covered_bins = static_cast<int>(std::count(covered.begin(), covered.end(), true));
    result.coverage_fraction = covered_bins / 360.0;
    int best_run = 0;
    int best_start = 0;
    if (covered_bins == 0) {
        best_run = 360;
    } else {
        for (int start = 0; start < 360; ++start) {
            if (covered[start] || covered[(start + 359) % 360] == false) continue;  // run starts after a covered bin
            int run = 0;
            while (run < 360 && !covered[(start + run) % 360]) ++run;
            if (run > best_run) { best_run = run; best_start = start; }
        }
    }
    // Per-ray inlier status, then every uncovered arc with the reason of most of its rays.
    {
        std::size_t point_index = 0;
        for (auto& record : result.ray_records) {
            if (record.status != RayStatus::off_rim_edge) continue;
            if (point_index < points.size() && points[point_index].inlier) record.status = RayStatus::inlier;
            ++point_index;
        }
        std::array<int, 360> bin_border{}, bin_no_edge{}, bin_off{};
        for (const auto& record : result.ray_records) {
            int bin = static_cast<int>(std::fmod(record.angle_deg + 360.0, 360.0)) % 360;
            if (record.status == RayStatus::frame_border) ++bin_border[bin];
            if (record.status == RayStatus::no_edge) ++bin_no_edge[bin];
            if (record.status == RayStatus::off_rim_edge) ++bin_off[bin];
        }
        if (covered_bins > 0) {
            for (int start = 0; start < 360; ++start) {
                if (covered[start] || !covered[(start + 359) % 360]) continue;
                int run = 0;
                while (run < 360 && !covered[(start + run) % 360]) ++run;
                if (run < config.report_gap_min_deg) continue;
                ArcGap gap;
                gap.start_deg = start;
                gap.width_deg = run;
                gap.side = side_of(start + run / 2.0);
                for (int k = 0; k < run; ++k) {
                    const int b = (start + k) % 360;
                    gap.rays_frame_border += bin_border[b];
                    gap.rays_no_edge += bin_no_edge[b];
                    gap.rays_off_rim_edge += bin_off[b];
                }
                if (gap.rays_frame_border >= gap.rays_no_edge && gap.rays_frame_border >= gap.rays_off_rim_edge) {
                    gap.reason = "frame_border";
                } else if (gap.rays_no_edge >= gap.rays_off_rim_edge) {
                    gap.reason = "no_edge";
                } else {
                    gap.reason = "off_rim_edge";
                }
                result.gaps.push_back(gap);
            }
        }
    }
    result.largest_gap_deg = best_run;
    result.largest_gap_center_deg = std::fmod(best_start + best_run / 2.0, 360.0);
    result.largest_gap_side = best_run > 0 ? side_of(result.largest_gap_center_deg) : "";
    const double r = result.fixed.radius_px;
    result.nearest_border_px = std::min({result.fixed.cx - r, result.fixed.cy - r,
                                         (width - 1.0) - (result.fixed.cx + r),
                                         (height - 1.0) - (result.fixed.cy + r)});
    result.points = std::move(points);

    {
        // Per quadrant: does each side follow the same edge as the others?
        const std::array<const char*, 4> names = {"right", "bottom", "left", "top"};
        std::array<std::vector<double>, 4> inlier_residuals;
        std::array<std::vector<double>, 4> all_residuals;
        std::array<int, 4> rays_per_side{};
        Model fixed_model;
        fixed_model.free_radius = false;
        fixed_model.harmonic = config.model_ellipticity;
        fixed_model.cx = result.fixed.cx;
        fixed_model.cy = result.fixed.cy;
        fixed_model.r = result.fixed.radius_px;
        const double phi = result.fixed.ellipticity_axis_deg * kPi / 180.0;
        fixed_model.ec = result.fixed.ellipticity_px * std::cos(2.0 * phi);
        fixed_model.es = result.fixed.ellipticity_px * std::sin(2.0 * phi);
        const auto quadrant = [&](double angle_deg) {
            const std::string side = side_of(angle_deg);
            for (int q = 0; q < 4; ++q) if (side == names[q]) return q;
            return 0;
        };
        for (const auto& record : result.ray_records) ++rays_per_side[quadrant(record.angle_deg)];
        for (const auto& p : result.points) {
            const int q = quadrant(p.angle_deg);
            const double r = point_residual(p, fixed_model);
            all_residuals[q].push_back(r);
            if (p.inlier) inlier_residuals[q].push_back(r);
        }
        for (int q = 0; q < 4; ++q) {
            SideStats side;
            side.side = names[q];
            side.rays = rays_per_side[q];
            side.points = static_cast<int>(all_residuals[q].size());
            side.inliers = static_cast<int>(inlier_residuals[q].size());
            if (!inlier_residuals[q].empty()) {
                side.fixed_residual_median_px = median(inlier_residuals[q]);
                std::vector<double> magnitudes;
                for (double r : inlier_residuals[q]) magnitudes.push_back(std::fabs(r));
                side.fixed_residual_median_abs_px = median(magnitudes);
            }
            if (!all_residuals[q].empty()) side.all_points_fixed_residual_median_px = median(all_residuals[q]);
            result.sides.push_back(side);
        }
    }
    if (result.fixed.inliers < config.min_inliers) result.refusals.push_back("too_few_rim_edge_points");
    if (result.coverage_fraction < config.min_coverage_fraction) result.refusals.push_back("rim_arc_coverage_too_small");
    for (const auto& gap : result.gaps) {
        // Each arc too wide is its own fault, with the side and the reason of its rays.
        if (gap.width_deg > config.max_gap_deg) {
            result.refusals.push_back("rim_arc_gap_too_large:" + gap.side + ":" + gap.reason + ":" +
                                      std::to_string(static_cast<int>(gap.width_deg)) + "deg");
        }
    }
    if (std::max(result.fixed.sigma_cx_px, result.fixed.sigma_cy_px) > config.max_center_sigma_px) {
        result.refusals.push_back("rim_center_uncertainty_too_large");
    }
    if (std::fabs(result.free.radius_px - expected_radius_px) > config.max_free_radius_delta_px) {
        // A free radius far from the known one means the points follow another edge
        // (the floor's illumination edge, the outer wall) or another dish.
        result.refusals.push_back("free_radius_disagrees_with_known_radius");
    }
    result.fixed_minus_free_center_px = std::hypot(result.fixed.cx - result.free.cx, result.fixed.cy - result.free.cy);
    if (result.fixed_minus_free_center_px > config.max_fixed_free_center_px) {
        // On a partial arc a radius error moves the fixed-radius centre: the known
        // radius and the visible arc disagree.
        result.refusals.push_back("fixed_and_free_centres_disagree");
    }
    if (result.nearest_border_px < 0.0) result.warnings.push_back("rim_extends_past_frame_edge");
    result.ok = result.refusals.empty();
    return result;
}

nlohmann::json to_json(const Result& result, const Config& config, double mm_per_px, bool include_points)
{
    const auto fit_json = [&](const CircleFit& f, bool free_radius) {
        nlohmann::json j = {
            {"ok", f.ok}, {"center_camera_px", {f.cx, f.cy}}, {"radius_camera_px", f.radius_px},
            {"inliers", f.inliers}, {"residual_median_px", f.residual_median_px},
            {"residual_max_px", f.residual_max_px},
            {"center_sigma_camera_px", {f.sigma_cx_px, f.sigma_cy_px}},
            {"radius_fixed", !free_radius},
            {"ellipticity_px", f.ellipticity_px},
            {"ellipticity_axis_deg", f.ellipticity_axis_deg},
            {"inlier_tolerance_px", f.inlier_tolerance_px},
            {"provisional_residual_median_px", f.provisional_residual_median_px},
        };
        if (free_radius) j["radius_sigma_camera_px"] = f.sigma_radius_px;
        if (mm_per_px > 0.0) {
            j["center_sigma_mm"] = {f.sigma_cx_px * mm_per_px, f.sigma_cy_px * mm_per_px};
            j["radius_mm"] = f.radius_px * mm_per_px;
        }
        return j;
    };
    nlohmann::json out = {
        {"method", kFitMethodId},
        {"ok", result.ok},
        {"refusals", result.refusals},
        {"warnings", result.warnings},
        {"expected_radius_camera_px", result.expected_radius_px},
        {"rays", result.rays},
        {"rays_excluded_frame_border", result.rays_excluded_border},
        {"rays_without_edge", result.rays_without_edge},
        {"fixed_radius_fit", fit_json(result.fixed, false)},
        {"free_radius_fit", fit_json(result.free, true)},
        {"free_minus_known_radius_px", result.free.ok ? result.free.radius_px - result.expected_radius_px : 0.0},
        {"fixed_minus_free_center_px", result.fixed_minus_free_center_px},
        {"arc_coverage_fraction", result.coverage_fraction},
        {"largest_gap_deg", result.largest_gap_deg},
        {"largest_gap_center_deg", result.largest_gap_center_deg},
        {"largest_gap_side", result.largest_gap_side},
        {"gaps", [&] {
            nlohmann::json gaps = nlohmann::json::array();
            for (const auto& g : result.gaps) {
                gaps.push_back({{"start_deg", g.start_deg}, {"width_deg", g.width_deg}, {"side", g.side},
                                {"reason", g.reason}, {"rays_frame_border", g.rays_frame_border},
                                {"rays_no_edge", g.rays_no_edge}, {"rays_off_rim_edge", g.rays_off_rim_edge}});
            }
            return gaps;
        }()},
        {"nearest_frame_border_px", result.nearest_border_px},
        {"edge_model", result.edge_model},
        {"edge_model_source", result.edge_model_source},
        {"ring_ray_fraction", result.ring_ray_fraction},
        {"rays_ring_step_without_valley", result.rays_ring_step_without_valley},
        {"declared_nir_filters", config.declared_nir_filters.empty() ? nlohmann::json(nullptr)
                                                                     : nlohmann::json(config.declared_nir_filters)},
        {"sides", [&] {
            nlohmann::json sides = nlohmann::json::array();
            for (const auto& side : result.sides) {
                sides.push_back({{"side", side.side}, {"rays", side.rays}, {"points", side.points},
                                 {"inliers", side.inliers},
                                 {"fixed_residual_median_px", side.fixed_residual_median_px},
                                 {"fixed_residual_median_abs_px", side.fixed_residual_median_abs_px},
                                 {"all_points_fixed_residual_median_px", side.all_points_fixed_residual_median_px}});
            }
            return sides;
        }()},
        {"limits", {
            {"min_inliers", config.min_inliers},
            {"min_coverage_fraction", config.min_coverage_fraction},
            {"max_gap_deg", config.max_gap_deg},
            {"report_gap_min_deg", config.report_gap_min_deg},
            {"max_center_sigma_px", config.max_center_sigma_px},
            {"max_free_radius_delta_px", config.max_free_radius_delta_px},
            {"max_fixed_free_center_px", config.max_fixed_free_center_px},
            {"inlier_scatter_factor", config.inlier_scatter_factor},
            {"inlier_floor_px", config.inlier_floor_px},
            {"max_inlier_px", config.max_inlier_px},
            {"model_ellipticity", config.model_ellipticity},
            {"border_margin_px", config.border_margin_px},
            {"search_inward_px", config.search_inward_px},
            {"search_outward_px", config.search_outward_px},
            {"edge_operator", "derivative_of_gaussian"},
            {"edge_sigma_px", config.edge_sigma_px},
            {"border_exclusion_px", static_cast<int>(3.0 * config.edge_sigma_px)},
            {"min_step_dn", config.min_step_dn},
            {"step_fraction_of_contrast", config.step_fraction_of_contrast},
            {"min_contrast_dn", config.min_contrast_dn},
            {"tangential_samples", config.tangential_samples},
            {"tangential_half_width_px", config.tangential_half_width_px},
            {"edge_model_config", edge_model_name(config.edge_model)},
            {"ring_ray_fraction", config.ring_ray_fraction},
            {"valley_operator", "matched_dark_line_min_flank_minus_core"},
            {"valley_core_half_px", config.valley_core_half_px},
            {"valley_gap_px", config.valley_gap_px},
            {"valley_flank_px", config.valley_flank_px},
            {"min_valley_dn", config.min_valley_dn},
            {"max_ring_band_px", config.max_ring_band_px},
            {"ring_edge", ring_edge_name(config.ring_edge)},
        }},
    };
    if (mm_per_px > 0.0) out["mm_per_px_at_rim_plane"] = mm_per_px;
    if (include_points) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : result.points) pts.push_back({p.x, p.y, p.inlier});
        out["edge_points_xy_inlier"] = std::move(pts);
    }
    return out;
}

KnownRadius known_radius_from_scale_file(const std::string& scale_file_path, const std::string& camera_serial,
                                         double inner_diameter_mm, double wall_height_mm)
{
    KnownRadius out;
    out.inner_diameter_mm = inner_diameter_mm;
    out.wall_height_mm = wall_height_mm;
    std::ifstream in(scale_file_path, std::ios::binary);
    if (!in) { out.error = "fish_plane_scale_unreadable"; return out; }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.scale_file_sha256 = orange::outputs::sha256_token(bytes);
    const nlohmann::json doc = nlohmann::json::parse(bytes, nullptr, false);
    if (doc.is_discarded() || doc.value("schema_id", "") != "orange.calibration.fish_plane_scale" ||
        !doc.contains("parameters") || !doc.contains("cameras") || !doc.at("cameras").contains(camera_serial)) {
        out.error = "fish_plane_scale_invalid_or_camera_missing";
        return out;
    }
    const auto& parameters = doc.at("parameters");
    const auto& camera = doc.at("cameras").at(camera_serial);
    const auto number = [](const nlohmann::json& j, const char* key, double* value) {
        if (!j.contains(key) || !j.at(key).is_number()) return false;
        *value = j.at(key).get<double>();
        return std::isfinite(*value);
    };
    double s_floor = 0.0, k = 0.0, p_floor = 0.0, e = 0.0;
    if (!number(camera, "s_floor_px_per_mm", &s_floor) || !number(camera, "k_px", &k) ||
        !number(parameters, "p_floor_mm", &p_floor) || !number(parameters, "e_mm", &e) || s_floor <= 0.0 || k <= 0.0) {
        out.error = "fish_plane_scale_parameters_missing";
        return out;
    }
    out.z_rim_top_mm = p_floor + wall_height_mm;
    out.px_per_mm = 1.0 / (1.0 / s_floor - (out.z_rim_top_mm - p_floor - e) / k);
    out.radius_px = 0.5 * inner_diameter_mm * out.px_per_mm;
    out.ok = std::isfinite(out.radius_px) && out.radius_px > 0.0;
    if (!out.ok) out.error = "fish_plane_scale_model_invalid";
    return out;
}

}  // namespace orange::daily_rim_fit
