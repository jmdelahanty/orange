#include "daily_rim_fit_images.h"

#include "fsuid_guard.h"
#include "outputs/common/atomic_file.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>

namespace orange::daily_rim_fit {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kOverlayFactor = 4;
constexpr int kZoomHalf = 160;      // native px either side of the rim in a zoom tile
constexpr int kZoomScale = 2;       // shown x2 (nearest)

const cv::Scalar kFixedColour(255, 255, 255);   // BGR: white
const cv::Scalar kFreeColour(255, 0, 255);      // magenta
const cv::Scalar kInlierColour(0, 220, 0);      // green
const cv::Scalar kRejectedColour(0, 0, 255);    // red
const cv::Scalar kGapColour(0, 165, 255);       // orange
const cv::Scalar kTextColour(255, 255, 255);
const cv::Scalar kWarnColour(80, 80, 255);

double model_radius(const CircleFit& fit, double t)
{
    const double phi = fit.ellipticity_axis_deg * kPi / 180.0;
    const double ec = fit.ellipticity_px * std::cos(2.0 * phi);
    const double es = fit.ellipticity_px * std::sin(2.0 * phi);
    return fit.radius_px + ec * std::cos(2.0 * t) + es * std::sin(2.0 * t);
}

cv::Point2d model_point(const CircleFit& fit, double t)
{
    const double r = model_radius(fit, t);
    return {fit.cx + r * std::cos(t), fit.cy + r * std::sin(t)};
}

// Polyline of the fit over [t0, t1], mapped by (p - origin) * scale.
std::vector<cv::Point> curve(const CircleFit& fit, double t0, double t1, int samples, cv::Point2d origin,
                             double scale)
{
    std::vector<cv::Point> out;
    out.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        const double t = t0 + (t1 - t0) * i / samples;
        const cv::Point2d p = model_point(fit, t);
        out.emplace_back(static_cast<int>(std::lround((p.x - origin.x) * scale)),
                         static_cast<int>(std::lround((p.y - origin.y) * scale)));
    }
    return out;
}

std::string fmt(const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, a, b, c, d);
    return buffer;
}

std::string join(const std::vector<std::string>& items, const std::string& separator)
{
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += separator;
        out += item;
    }
    return out;
}

void text(cv::Mat* image, const std::string& value, cv::Point at, const cv::Scalar& colour, double scale = 0.6)
{
    cv::putText(*image, value, at, cv::FONT_HERSHEY_SIMPLEX, scale, colour, 1, cv::LINE_AA);
}

bool write_png(const std::filesystem::path& path, const cv::Mat& image, std::string* error)
{
    orange::ScopedFsuid fsuid_guard;  // files belong to the sudo invoker
    (void)fsuid_guard;
    std::vector<unsigned char> bytes;
    try {
        // Level 3: lossless like every PNG, faster than the default for 20 MP frames.
        if (!cv::imencode(".png", image, bytes, {cv::IMWRITE_PNG_COMPRESSION, 3})) {
            *error = "png_encode_failed:" + path.filename().string();
            return false;
        }
    } catch (const std::exception& ex) {
        *error = "png_encode_failed:" + path.filename().string() + ":" + ex.what();
        return false;
    }
    std::string write_error;
    if (!orange::outputs::write_file_atomic(path, std::string(bytes.begin(), bytes.end()), {},
                                                     &write_error)) {
        *error = "png_write_failed:" + path.filename().string() + ":" + write_error;
        return false;
    }
    return true;
}

std::vector<std::string> summary_lines(const Result& result, const std::string& label)
{
    std::vector<std::string> lines;
    lines.push_back(label + ": " + (result.ok ? std::string("fit OK") : "REFUSED: " + join(result.refusals, ", ")));
    lines.push_back(fmt("fixed (known r %.1f px) centre (%.2f, %.2f) sigma %.2f px", result.expected_radius_px,
                        result.fixed.cx, result.fixed.cy, std::max(result.fixed.sigma_cx_px, result.fixed.sigma_cy_px)) +
                    fmt("; free - known r %+.1f px; fixed - free centre %.2f px",
                        result.free.radius_px - result.expected_radius_px, result.fixed_minus_free_center_px));
    lines.push_back(fmt("coverage %.3f; largest gap %.0f deg", result.coverage_fraction, result.largest_gap_deg) +
                    (result.largest_gap_side.empty() ? std::string() : " (" + result.largest_gap_side + ")") +
                    fmt("; inliers %.0f/%.0f; tolerance %.2f px (provisional median %.2f px)",
                        static_cast<double>(result.fixed.inliers), static_cast<double>(result.points.size()),
                        result.free.inlier_tolerance_px, result.free.provisional_residual_median_px));
    std::string sides = "edge " + result.edge_model + " (" + result.edge_model_source +
        fmt(", ring rays %.2f); per-side median residual:", result.ring_ray_fraction);
    for (const auto& side : result.sides) {
        sides += " " + side.side + fmt(" %+.1f", side.fixed_residual_median_px);
    }
    lines.push_back(sides);
    if (!result.warnings.empty()) lines.push_back("warnings: " + join(result.warnings, ", "));
    return lines;
}

cv::Mat overlay_image(const std::vector<std::uint8_t>& gray, int width, int height, const Result& result,
                      const std::string& label)
{
    const cv::Mat full(height, width, CV_8UC1, const_cast<std::uint8_t*>(gray.data()));
    cv::Mat small;
    cv::resize(full, small, cv::Size(width / kOverlayFactor, height / kOverlayFactor), 0, 0, cv::INTER_AREA);
    cv::Mat colour;
    cv::cvtColor(small, colour, cv::COLOR_GRAY2BGR);
    const double s = 1.0 / kOverlayFactor;
    const cv::Point2d origin(0.0, 0.0);
    if (result.free.ok) {
        cv::polylines(colour, curve(result.free, 0.0, 2.0 * kPi, 720, origin, s), true, kFreeColour, 1, cv::LINE_AA);
    }
    if (result.fixed.ok) {
        cv::polylines(colour, curve(result.fixed, 0.0, 2.0 * kPi, 720, origin, s), true, kFixedColour, 1,
                      cv::LINE_AA);
        const cv::Point c(static_cast<int>(std::lround(result.fixed.cx * s)),
                          static_cast<int>(std::lround(result.fixed.cy * s)));
        cv::drawMarker(colour, c, kFixedColour, cv::MARKER_CROSS, 30, 2);
    }
    for (const auto& p : result.points) {
        cv::circle(colour, cv::Point(static_cast<int>(std::lround(p.x * s)), static_cast<int>(std::lround(p.y * s))),
                   p.inlier ? 1 : 2, p.inlier ? kInlierColour : kRejectedColour, cv::FILLED);
    }
    if (result.fixed.ok) {
        for (const auto& gap : result.gaps) {
            const double t0 = gap.start_deg * kPi / 180.0;
            const double t1 = (gap.start_deg + gap.width_deg) * kPi / 180.0;
            CircleFit outside = result.fixed;
            outside.radius_px += 40.0;
            cv::polylines(colour, curve(outside, t0, t1, 60, origin, s), false, kGapColour, 4, cv::LINE_AA);
            CircleFit inside = result.fixed;
            inside.radius_px -= 260.0;
            const double tm = 0.5 * (t0 + t1);
            const cv::Point2d at = model_point(inside, tm);
            const std::string name = gap.side + " " + fmt("%.0f", gap.width_deg) + " deg " + gap.reason;
            const int x = std::clamp(static_cast<int>(at.x * s) - 90, 4, std::max(4, colour.cols - 240));
            const int y = std::clamp(static_cast<int>(at.y * s), 20, std::max(20, colour.rows - 8));
            text(&colour, name, cv::Point(x, y), kGapColour, 0.55);
        }
    }
    const auto lines = summary_lines(result, label);
    const std::vector<std::pair<cv::Scalar, std::string>> key = {
        {kFixedColour, "fixed fit, radius = known (THE centre)"},
        {kFreeColour, "free-radius fit (cross-check)"},
        {kInlierColour, "inlier edge points"},
        {kRejectedColour, "rejected edge points"},
        {kGapColour, "uncovered arcs >= report threshold"},
    };
    const int strip = 14 + 26 * static_cast<int>(lines.size() + 1 + (key.size() + 1) / 2);
    cv::Mat figure(colour.rows + strip, colour.cols, CV_8UC3, cv::Scalar(0, 0, 0));
    colour.copyTo(figure(cv::Rect(0, 0, colour.cols, colour.rows)));
    int y = colour.rows + 22;
    text(&figure, fmt("frame %.0f x %.0f px shown at 1/%.0f", width, height, kOverlayFactor), cv::Point(10, y),
         kTextColour);
    y += 26;
    for (const auto& line : lines) {
        text(&figure, line, cv::Point(10, y), result.ok ? kTextColour : kWarnColour, 0.5);
        y += 26;
    }
    for (std::size_t k = 0; k < key.size(); ++k) {
        const int x = 10 + static_cast<int>(k % 2) * (figure.cols / 2);
        cv::line(figure, cv::Point(x, y - 6), cv::Point(x + 28, y - 6), key[k].first, 4);
        text(&figure, key[k].second, cv::Point(x + 36, y), key[k].first, 0.5);
        if (k % 2 == 1) y += 26;
    }
    return figure;
}

cv::Mat zoom_image(const std::vector<std::uint8_t>& gray, int width, int height, const Result& result,
                   const std::string& label)
{
    const int tile_size = 2 * kZoomHalf * kZoomScale;
    cv::Mat sheet(2 * tile_size + 64, 2 * tile_size, CV_8UC3, cv::Scalar(0, 0, 0));
    const cv::Mat full(height, width, CV_8UC1, const_cast<std::uint8_t*>(gray.data()));
    const std::array<const char*, 4> side_names = {"top", "right", "bottom", "left"};
    const std::array<double, 4> side_angles_deg = {-90.0, 0.0, 90.0, 180.0};
    for (std::size_t k = 0; k < side_names.size(); ++k) {
        const double a = side_angles_deg[k] * kPi / 180.0;
        const cv::Point2d edge = model_point(result.fixed, a);
        const int x0 = static_cast<int>(std::lround(edge.x)) - kZoomHalf;
        const int y0 = static_cast<int>(std::lround(edge.y)) - kZoomHalf;
        cv::Mat crop(2 * kZoomHalf, 2 * kZoomHalf, CV_8UC1, cv::Scalar(0));
        const cv::Rect wanted(x0, y0, 2 * kZoomHalf, 2 * kZoomHalf);
        const cv::Rect inside = wanted & cv::Rect(0, 0, width, height);
        std::vector<std::uint8_t> values;
        if (inside.area() > 0) {
            full(inside).copyTo(crop(cv::Rect(inside.x - x0, inside.y - y0, inside.width, inside.height)));
            const cv::Mat valid = full(inside);
            for (int r = 0; r < valid.rows; ++r) {
                const auto* row = valid.ptr<std::uint8_t>(r);
                values.insert(values.end(), row, row + valid.cols);
            }
        }
        // Stretch 1st..99.5th percentile of the in-frame pixels.
        double lo = 0.0;
        double hi = 255.0;
        if (!values.empty()) {
            std::sort(values.begin(), values.end());
            lo = values[static_cast<std::size_t>(0.01 * (values.size() - 1))];
            hi = values[static_cast<std::size_t>(0.995 * (values.size() - 1))];
        }
        cv::Mat stretched;
        crop.convertTo(stretched, CV_8UC1, 255.0 / std::max(1.0, hi - lo), -lo * 255.0 / std::max(1.0, hi - lo));
        if (inside != wanted) {
            // Outside the frame stays black, not stretched.
            cv::Mat mask(crop.size(), CV_8UC1, cv::Scalar(0));
            if (inside.area() > 0) mask(cv::Rect(inside.x - x0, inside.y - y0, inside.width, inside.height)) = 255;
            stretched.setTo(0, mask == 0);
        }
        cv::Mat tile;
        cv::resize(stretched, tile, cv::Size(tile_size, tile_size), 0, 0, cv::INTER_NEAREST);
        cv::cvtColor(tile, tile, cv::COLOR_GRAY2BGR);
        const cv::Point2d origin(x0, y0);
        if (result.free.ok) {
            cv::polylines(tile, curve(result.free, a - 0.12, a + 0.12, 400, origin, kZoomScale), false, kFreeColour, 1,
                          cv::LINE_AA);
        }
        cv::polylines(tile, curve(result.fixed, a - 0.12, a + 0.12, 400, origin, kZoomScale), false, kFixedColour, 2,
                      cv::LINE_AA);
        int shown = 0;
        for (const auto& p : result.points) {
            const double qx = (p.x - x0) * kZoomScale;
            const double qy = (p.y - y0) * kZoomScale;
            if (qx < 0 || qy < 0 || qx >= tile_size || qy >= tile_size) continue;
            cv::circle(tile, cv::Point(static_cast<int>(qx), static_cast<int>(qy)), 4,
                       p.inlier ? kInlierColour : kRejectedColour, 2, cv::LINE_AA);
            if (p.inlier) ++shown;
        }
        std::vector<std::string> notes;
        if (shown == 0) notes.push_back("NO INLIER EDGE POINTS HERE");
        if (inside != wanted) notes.push_back("black = outside the frame");
        cv::rectangle(tile, cv::Rect(0, 0, tile_size, 28 + 24 * static_cast<int>(notes.size())), cv::Scalar(0, 0, 0),
                      cv::FILLED);
        text(&tile, std::string(side_names[k]) + fmt(": %.0f x %.0f native px (shown x%.0f)", 2 * kZoomHalf,
                                                      2 * kZoomHalf, kZoomScale),
             cv::Point(6, 20), kTextColour);
        for (std::size_t n = 0; n < notes.size(); ++n) {
            text(&tile, notes[n], cv::Point(6, 44 + 24 * static_cast<int>(n)), kWarnColour);
        }
        tile.copyTo(sheet(cv::Rect(static_cast<int>(k % 2) * tile_size, static_cast<int>(k / 2) * tile_size,
                                   tile_size, tile_size)));
    }
    int y = 2 * tile_size + 22;
    text(&sheet, label + (result.ok ? ": fit OK" : ": REFUSED"), cv::Point(10, y), result.ok ? kTextColour : kWarnColour);
    y += 28;
    int x = 10;
    for (const auto& [colour, name] : std::vector<std::pair<cv::Scalar, std::string>>{
             {kFixedColour, "fixed (known radius)"}, {kFreeColour, "free radius"},
             {kInlierColour, "inlier"}, {kRejectedColour, "rejected"}}) {
        cv::line(sheet, cv::Point(x, y - 6), cv::Point(x + 28, y - 6), colour, 4);
        text(&sheet, name, cv::Point(x + 36, y), colour, 0.5);
        x += 60 + 10 * static_cast<int>(name.size());
    }
    return sheet;
}

}  // namespace

FitImages write_fit_images(const std::vector<std::uint8_t>& gray, int width, int height, const Result& result,
                           const std::filesystem::path& directory, const std::string& label)
{
    FitImages images;
    if (width <= 0 || height <= 0 || gray.size() != static_cast<std::size_t>(width) * height) {
        images.error = "rim_frame_size_mismatch";
        return images;
    }
    const cv::Mat frame(height, width, CV_8UC1, const_cast<std::uint8_t*>(gray.data()));
    const auto frame_path = directory / kRimFrameFilename;
    if (!write_png(frame_path, frame, &images.error)) return images;
    images.frame_png = frame_path.string();
    try {
        const auto overlay_path = directory / kRimOverlayFilename;
        if (!write_png(overlay_path, overlay_image(gray, width, height, result, label), &images.error)) return images;
        images.overlay_png = overlay_path.string();
        if (result.fixed.ok) {
            const auto zoom_path = directory / kRimZoomFilename;
            if (!write_png(zoom_path, zoom_image(gray, width, height, result, label), &images.error)) return images;
            images.zoom_png = zoom_path.string();
        }
    } catch (const std::exception& ex) {
        images.error = std::string("rim_figure_failed:") + ex.what();
        return images;
    }
    images.ok = true;
    return images;
}

nlohmann::json to_json(const FitImages& images)
{
    const auto path_or_null = [](const std::string& path) {
        return path.empty() ? nlohmann::json(nullptr) : nlohmann::json(path);
    };
    return {
        {"ok", images.ok},
        {"error", images.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(images.error)},
        {"rim_frame_png", path_or_null(images.frame_png)},
        {"overlay_png", path_or_null(images.overlay_png)},
        {"zoom_png", path_or_null(images.zoom_png)},
    };
}

}  // namespace orange::daily_rim_fit
