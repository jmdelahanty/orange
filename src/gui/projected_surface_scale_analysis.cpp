#include "gui/projected_surface_scale_analysis.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace orange::gui::projected_surface_scale {
namespace {

struct Blob {
    cv::Point2d center;
    double radius_px = 0.0;
    double circularity = 0.0;
};

bool fail(std::string* error_out, const std::string& error)
{
    if (error_out != nullptr) {
        *error_out = error;
    }
    return false;
}

std::vector<std::string> split_csv(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    for (const char ch : line) {
        if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else if (ch != '\r') {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

cv::Mat as_gray_u8(const cv::Mat& source)
{
    cv::Mat gray;
    if (source.empty()) {
        return gray;
    }
    if (source.channels() == 1) {
        if (source.depth() == CV_8U) {
            gray = source.clone();
        } else {
            cv::normalize(source, gray, 0, 255, cv::NORM_MINMAX, CV_8U);
        }
    } else if (source.channels() == 3) {
        cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
    } else if (source.channels() == 4) {
        cv::cvtColor(source, gray, cv::COLOR_RGBA2GRAY);
    }
    return gray;
}

std::vector<Blob> blobs_from_binary(const cv::Mat& binary)
{
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<Blob> blobs;
    const double image_area = static_cast<double>(binary.rows) * binary.cols;
    const double minimum_area = std::max(8.0, image_area * 1.0e-7);
    const double maximum_area = image_area * 0.01;
    for (const auto& contour : contours) {
        const double area = std::abs(cv::contourArea(contour));
        const double perimeter = cv::arcLength(contour, true);
        if (area < minimum_area || area > maximum_area || perimeter <= 0.0) {
            continue;
        }
        const cv::Rect bounds = cv::boundingRect(contour);
        const double aspect = bounds.height > 0
                                  ? static_cast<double>(bounds.width) / bounds.height
                                  : 0.0;
        const double circularity = 4.0 * CV_PI * area / (perimeter * perimeter);
        if (aspect < 0.55 || aspect > 1.8 || circularity < 0.45) {
            continue;
        }
        const cv::Moments moments = cv::moments(contour);
        if (std::abs(moments.m00) < 1.0e-9) {
            continue;
        }
        blobs.push_back({
            {moments.m10 / moments.m00, moments.m01 / moments.m00},
            std::sqrt(area / CV_PI),
            circularity,
        });
    }
    return blobs;
}

std::vector<Blob> detect_blobs(const cv::Mat& gray)
{
    cv::Mat smooth;
    cv::GaussianBlur(gray, smooth, cv::Size(5, 5), 0.0);

    int block = std::max(31, (std::min(gray.cols, gray.rows) / 30) | 1);
    block = std::min(block, 301);
    if ((block & 1) == 0) {
        ++block;
    }
    cv::Mat adaptive;
    cv::adaptiveThreshold(
        smooth,
        adaptive,
        255,
        cv::ADAPTIVE_THRESH_GAUSSIAN_C,
        cv::THRESH_BINARY,
        block,
        -3.0);
    cv::morphologyEx(
        adaptive,
        adaptive,
        cv::MORPH_OPEN,
        cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    cv::Mat global;
    cv::threshold(smooth, global, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    std::vector<Blob> blobs = blobs_from_binary(adaptive);
    const std::vector<Blob> global_blobs = blobs_from_binary(global);
    blobs.insert(blobs.end(), global_blobs.begin(), global_blobs.end());

    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) {
        return a.radius_px > b.radius_px;
    });
    std::vector<Blob> unique;
    for (const Blob& candidate : blobs) {
        const auto duplicate = std::find_if(
            unique.begin(), unique.end(), [&](const Blob& existing) {
                const double tolerance = std::max(2.0, 0.35 * std::min(
                    existing.radius_px, candidate.radius_px));
                return cv::norm(existing.center - candidate.center) <= tolerance;
            });
        if (duplicate == unique.end()) {
            unique.push_back(candidate);
        }
    }
    return unique;
}

const TargetPoint* find_target(const std::vector<TargetPoint>& points,
                               const std::string& point_id)
{
    const auto it = std::find_if(points.begin(), points.end(), [&](const auto& point) {
        return point.point_id == point_id;
    });
    return it == points.end() ? nullptr : &*it;
}

cv::Point2d transform_point(const cv::Mat& transform, const cv::Point2d& point)
{
    std::vector<cv::Point2d> input{point};
    std::vector<cv::Point2d> output;
    cv::perspectiveTransform(input, output, transform);
    return output.front();
}

std::map<std::string, int> match_points(
    const std::vector<TargetPoint>& targets,
    const std::vector<Blob>& blobs,
    const cv::Mat& target_to_camera,
    double maximum_distance_px)
{
    struct Edge {
        double distance = 0.0;
        std::string point_id;
        int blob_index = -1;
    };
    std::vector<Edge> edges;
    for (const auto& target : targets) {
        const cv::Point2d predicted = transform_point(target_to_camera, target.target_mm);
        for (int index = 0; index < static_cast<int>(blobs.size()); ++index) {
            const double distance = cv::norm(predicted - blobs[index].center);
            if (distance <= maximum_distance_px) {
                edges.push_back({distance, target.point_id, index});
            }
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        return a.distance < b.distance;
    });
    std::set<std::string> used_points;
    std::set<int> used_blobs;
    std::map<std::string, int> matches;
    for (const Edge& edge : edges) {
        if (used_points.insert(edge.point_id).second &&
            used_blobs.insert(edge.blob_index).second) {
            matches[edge.point_id] = edge.blob_index;
        }
    }
    return matches;
}

cv::Mat fit_transform(const std::vector<TargetPoint>& targets,
                      const std::vector<Blob>& blobs,
                      const std::map<std::string, int>& matches,
                      const bool inner_only,
                      std::vector<unsigned char>* inlier_mask = nullptr)
{
    std::vector<cv::Point2d> target_mm;
    std::vector<cv::Point2d> camera_px;
    for (const auto& target : targets) {
        const auto matched = matches.find(target.point_id);
        if (matched == matches.end() || !target.include_in_fit ||
            target.marker_role != "regular") {
            continue;
        }
        if (inner_only && cv::norm(target.target_mm) >= 30.0) {
            continue;
        }
        target_mm.push_back(target.target_mm);
        camera_px.push_back(blobs[matched->second].center);
    }
    if (target_mm.size() < 4) {
        return {};
    }
    cv::Mat mask;
    cv::Mat result = cv::findHomography(
        target_mm, camera_px, cv::RANSAC, 2.0, mask, 5000, 0.999);
    if (inlier_mask != nullptr && !mask.empty()) {
        inlier_mask->assign(mask.begin<unsigned char>(), mask.end<unsigned char>());
    }
    return result;
}

double rms(const std::vector<double>& values)
{
    if (values.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    const double sum = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);
    return std::sqrt(sum / values.size());
}

double maximum(const std::vector<double>& values)
{
    return values.empty()
               ? std::numeric_limits<double>::infinity()
               : *std::max_element(values.begin(), values.end());
}

struct OuterDiameterEstimate {
    double diameter_mm = 0.0;
    int valid_ray_count = 0;
    int total_ray_count = 0;
    double support_fraction = 0.0;
    double radial_mad_mm = 0.0;
};

double median(std::vector<double> values)
{
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if ((values.size() & 1u) != 0u) {
        return upper;
    }
    std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
    return 0.5 * (values[middle - 1] + upper);
}

double bilinear_sample(const cv::Mat& image, double x, double y)
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, image.cols - 1);
    const int y1 = std::min(y0 + 1, image.rows - 1);
    if (x0 < 0 || y0 < 0 || x0 >= image.cols || y0 >= image.rows) {
        return 255.0;
    }
    const double dx = x - x0;
    const double dy = y - y0;
    const double top = (1.0 - dx) * image.at<unsigned char>(y0, x0) +
                       dx * image.at<unsigned char>(y0, x1);
    const double bottom = (1.0 - dx) * image.at<unsigned char>(y1, x0) +
                          dx * image.at<unsigned char>(y1, x1);
    return (1.0 - dy) * top + dy * bottom;
}

std::optional<OuterDiameterEstimate> estimate_outer_diameter_mm(
    const cv::Mat& gray,
    const cv::Mat& target_mm_to_camera_px,
    double expected_diameter_mm)
{
    if (gray.empty() || target_mm_to_camera_px.empty()) {
        return std::nullopt;
    }
    constexpr double pixels_per_mm = 10.0;
    constexpr int size = 900;
    cv::Mat mm_to_rect = (cv::Mat_<double>(3, 3) <<
        pixels_per_mm, 0.0, size * 0.5,
        0.0, pixels_per_mm, size * 0.5,
        0.0, 0.0, 1.0);
    cv::Mat camera_to_rect = mm_to_rect * target_mm_to_camera_px.inv();
    cv::Mat rectified;
    cv::warpPerspective(
        gray, rectified, camera_to_rect, cv::Size(size, size),
        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(255));
    // The disk has only about 3 mm of projected margin inside the 83 mm dry
    // arena. Binary closing can bridge that physical gap and contour hierarchy
    // can then hide or merge the disk boundary. Measure the positive dark-to-
    // illuminated transition along independent radial rays instead. Rays where
    // a fixture or projection boundary provides no bright exterior fail their
    // contrast gate and do not influence the median.
    constexpr int total_ray_count = 360;
    constexpr double radial_search_half_width_mm = 2.5;
    constexpr double minimum_transition_contrast_u8 = 20.0;
    constexpr double minimum_edge_gradient_u8 = 12.0;
    const double expected_radius_mm = expected_diameter_mm * 0.5;
    const int minimum_radius_px = static_cast<int>(std::lround(
        (expected_radius_mm - radial_search_half_width_mm) * pixels_per_mm));
    const int maximum_radius_px = static_cast<int>(std::lround(
        (expected_radius_mm + radial_search_half_width_mm) * pixels_per_mm));
    const int sample_count = maximum_radius_px - minimum_radius_px + 1;
    std::vector<double> radii_mm;
    radii_mm.reserve(total_ray_count);
    for (int ray = 0; ray < total_ray_count; ++ray) {
        const double theta = 2.0 * CV_PI * ray / total_ray_count;
        const double cos_theta = std::cos(theta);
        const double sin_theta = std::sin(theta);
        std::vector<double> samples(sample_count);
        for (int index = 0; index < sample_count; ++index) {
            const double radius_px = minimum_radius_px + index;
            samples[index] = bilinear_sample(
                rectified,
                size * 0.5 + radius_px * cos_theta,
                size * 0.5 + radius_px * sin_theta);
        }
        const int contrast_sample_count = std::min(8, sample_count / 3);
        const double inner_mean = std::accumulate(
            samples.begin(), samples.begin() + contrast_sample_count, 0.0) /
            contrast_sample_count;
        const double outer_mean = std::accumulate(
            samples.end() - contrast_sample_count, samples.end(), 0.0) /
            contrast_sample_count;
        if (outer_mean - inner_mean < minimum_transition_contrast_u8) {
            continue;
        }
        std::vector<double> smooth(sample_count);
        for (int index = 0; index < sample_count; ++index) {
            const int begin = std::max(0, index - 2);
            const int end = std::min(sample_count - 1, index + 2);
            smooth[index] = std::accumulate(
                samples.begin() + begin, samples.begin() + end + 1, 0.0) /
                (end - begin + 1);
        }
        double best_gradient = -std::numeric_limits<double>::infinity();
        int best_index = -1;
        for (int index = 4; index + 4 < sample_count; ++index) {
            const double gradient = smooth[index + 4] - smooth[index - 4];
            if (gradient > best_gradient) {
                best_gradient = gradient;
                best_index = index;
            }
        }
        if (best_index < 0 || best_gradient < minimum_edge_gradient_u8) {
            continue;
        }
        radii_mm.push_back(
            static_cast<double>(minimum_radius_px + best_index) / pixels_per_mm);
    }
    constexpr int minimum_valid_ray_count = 60;
    if (static_cast<int>(radii_mm.size()) < minimum_valid_ray_count) {
        return std::nullopt;
    }
    const double radius_median_mm = median(radii_mm);
    std::vector<double> absolute_deviations;
    absolute_deviations.reserve(radii_mm.size());
    for (const double radius_mm : radii_mm) {
        absolute_deviations.push_back(std::abs(radius_mm - radius_median_mm));
    }
    const double radial_mad_mm = median(std::move(absolute_deviations));
    constexpr double maximum_radial_mad_mm = 0.75;
    if (!std::isfinite(radial_mad_mm) || radial_mad_mm > maximum_radial_mad_mm) {
        return std::nullopt;
    }
    return OuterDiameterEstimate{
        2.0 * radius_median_mm,
        static_cast<int>(radii_mm.size()),
        total_ray_count,
        static_cast<double>(radii_mm.size()) / total_ray_count,
        radial_mad_mm,
    };
}

cv::Mat draw_overlay(const cv::Mat& source,
                     const std::vector<Blob>& blobs,
                     const std::vector<ObservedPoint>& correspondences,
                     bool quality_pass,
                     const nlohmann::json& metrics,
                     const cv::Mat& target_mm_to_camera_px,
                     const std::optional<OuterDiameterEstimate>& outer_diameter)
{
    cv::Mat bgr;
    if (source.channels() == 1) {
        cv::cvtColor(source, bgr, cv::COLOR_GRAY2BGR);
    } else if (source.channels() == 3) {
        bgr = source.clone();
    } else {
        cv::cvtColor(source, bgr, cv::COLOR_RGBA2BGR);
    }
    for (const Blob& blob : blobs) {
        cv::circle(bgr, blob.center, std::max(2, static_cast<int>(std::lround(blob.radius_px))),
                   cv::Scalar(80, 80, 80), 1, cv::LINE_AA);
    }
    for (const auto& point : correspondences) {
        cv::Scalar color = point.holdout ? cv::Scalar(0, 170, 255)
                                         : cv::Scalar(0, 220, 0);
        if (point.marker_role != "regular") {
            color = cv::Scalar(255, 0, 255);
        }
        cv::drawMarker(bgr, point.camera_px, color, cv::MARKER_CROSS, 18, 2, cv::LINE_AA);
        if (point.marker_role != "regular" || point.holdout) {
            cv::putText(bgr, point.point_id,
                        point.camera_px + cv::Point2d(8.0, -8.0),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
        }
    }
    if (outer_diameter.has_value() && !target_mm_to_camera_px.empty()) {
        std::vector<cv::Point> perimeter;
        perimeter.reserve(181);
        const double radius_mm = outer_diameter->diameter_mm * 0.5;
        for (int sample = 0; sample <= 180; ++sample) {
            const double theta = 2.0 * CV_PI * sample / 180.0;
            const cv::Point2d camera_point = transform_point(
                target_mm_to_camera_px,
                {radius_mm * std::cos(theta), radius_mm * std::sin(theta)});
            perimeter.emplace_back(
                static_cast<int>(std::lround(camera_point.x)),
                static_cast<int>(std::lround(camera_point.y)));
        }
        cv::polylines(bgr, perimeter, true, cv::Scalar(255, 255, 0), 3, cv::LINE_AA);
    }
    std::ostringstream line;
    line << (quality_pass ? "PASS" : "FAIL")
         << " matched=" << correspondences.size()
         << " fit_rms=" << std::fixed << std::setprecision(3)
         << metrics.value("fit_rms_mm", -1.0) << "mm"
         << " holdout_rms=" << metrics.value("holdout_rms_mm", -1.0) << "mm"
         << " diameter=";
    if (outer_diameter.has_value()) {
        line << std::setprecision(2) << outer_diameter->diameter_mm << "mm";
    } else {
        line << "unresolved";
    }
    const cv::Scalar color = quality_pass ? cv::Scalar(0, 220, 0)
                                          : cv::Scalar(0, 0, 255);
    cv::putText(bgr, line.str(), cv::Point(24, 40),
                cv::FONT_HERSHEY_SIMPLEX, 0.85, color, 2, cv::LINE_AA);
    return bgr;
}

}  // namespace

nlohmann::json matrix_json(const cv::Mat& matrix)
{
    if (matrix.empty()) {
        return nullptr;
    }
    cv::Mat converted;
    matrix.convertTo(converted, CV_64F);
    nlohmann::json rows = nlohmann::json::array();
    for (int row = 0; row < converted.rows; ++row) {
        nlohmann::json values = nlohmann::json::array();
        for (int column = 0; column < converted.cols; ++column) {
            values.push_back(converted.at<double>(row, column));
        }
        rows.push_back(std::move(values));
    }
    return rows;
}

bool load_target_definition(
    const std::filesystem::path& target_json_path,
    std::vector<TargetPoint>* points_out,
    nlohmann::json* target_definition_out,
    std::string* error_out)
{
    if (points_out == nullptr || target_definition_out == nullptr) {
        return fail(error_out, "target definition output is null");
    }
    std::ifstream json_input(target_json_path);
    if (!json_input) {
        return fail(error_out, "could not open target JSON: " + target_json_path.string());
    }
    nlohmann::json target;
    try {
        json_input >> target;
    } catch (const std::exception& error) {
        return fail(error_out, "could not parse target JSON: " + std::string(error.what()));
    }
    const std::string csv_name = target.value("source_files", nlohmann::json::object())
                                     .value("coordinate_csv_path", "");
    const std::filesystem::path csv_path = target_json_path.parent_path() / csv_name;
    std::ifstream csv_input(csv_path);
    if (!csv_input) {
        return fail(error_out, "could not open target coordinate CSV: " + csv_path.string());
    }
    std::string line;
    if (!std::getline(csv_input, line)) {
        return fail(error_out, "target coordinate CSV is empty");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (
        line != "point_id,x_mm,y_mm,marker_role,nominal_diameter_mm,include_in_fit") {
        return fail(error_out, "target coordinate CSV header is invalid");
    }
    std::vector<TargetPoint> points;
    while (std::getline(csv_input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_csv(line);
        if (fields.size() != 6) {
            return fail(error_out, "target coordinate CSV row is invalid: " + line);
        }
        try {
            points.push_back({
                fields[0],
                {std::stod(fields[1]), std::stod(fields[2])},
                fields[3],
                std::stod(fields[4]),
                fields[5] == "true" || fields[5] == "1",
            });
        } catch (const std::exception&) {
            return fail(error_out, "target coordinate CSV contains invalid numbers: " + line);
        }
    }
    if (points.size() < 20 || find_target(points, "C") == nullptr ||
        find_target(points, "XPLUS") == nullptr || find_target(points, "ASYM") == nullptr) {
        return fail(error_out, "target coordinate table lacks required points");
    }
    *points_out = std::move(points);
    *target_definition_out = std::move(target);
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

AnalysisResult analyze(
    const cv::Mat& source_image,
    const std::vector<TargetPoint>& target_points,
    const nlohmann::json& target_definition,
    const cv::Mat& camera_px_to_canvas_px,
    const QualityThresholds& thresholds)
{
    AnalysisResult result;
    result.target_points = target_points;
    const cv::Mat gray = as_gray_u8(source_image);
    if (gray.empty()) {
        result.error = "source image is empty or unsupported";
        return result;
    }
    const TargetPoint* c = find_target(target_points, "C");
    const TargetPoint* xplus = find_target(target_points, "XPLUS");
    const TargetPoint* asym = find_target(target_points, "ASYM");
    if (c == nullptr || xplus == nullptr || asym == nullptr) {
        result.error = "target is missing C, XPLUS, or ASYM";
        return result;
    }
    const std::vector<Blob> blobs = detect_blobs(gray);
    if (blobs.size() < 20) {
        result.error = "too few circular transmitted-light features detected";
        result.report = {{"detected_blob_count", blobs.size()}};
        result.overlay_bgr = draw_overlay(
            source_image, blobs, {}, false, result.report, {}, std::nullopt);
        return result;
    }

    const int special_limit = std::min<int>(10, blobs.size());
    cv::Mat best_initial;
    std::map<std::string, int> best_matches;
    double best_score = -1.0;
    for (int ci = 0; ci < special_limit; ++ci) {
        for (int xi = 0; xi < special_limit; ++xi) {
            if (xi == ci) continue;
            for (int ai = 0; ai < special_limit; ++ai) {
                if (ai == ci || ai == xi) continue;
                const double cx_distance = cv::norm(blobs[xi].center - blobs[ci].center);
                if (cx_distance < 10.0) continue;
                std::array<cv::Point2f, 3> physical{{
                    cv::Point2f(static_cast<float>(c->target_mm.x), static_cast<float>(c->target_mm.y)),
                    cv::Point2f(static_cast<float>(xplus->target_mm.x), static_cast<float>(xplus->target_mm.y)),
                    cv::Point2f(static_cast<float>(asym->target_mm.x), static_cast<float>(asym->target_mm.y)),
                }};
                std::array<cv::Point2f, 3> observed{{
                    cv::Point2f(blobs[ci].center),
                    cv::Point2f(blobs[xi].center),
                    cv::Point2f(blobs[ai].center),
                }};
                const cv::Mat affine = cv::getAffineTransform(physical.data(), observed.data());
                cv::Mat initial = cv::Mat::eye(3, 3, CV_64F);
                affine.copyTo(initial(cv::Rect(0, 0, 3, 2)));
                const double px_per_mm = cx_distance / 25.0;
                auto matches = match_points(
                    target_points, blobs, initial, std::max(3.0, px_per_mm * 1.6));
                double size_score = 0.0;
                if (blobs[xi].radius_px >= blobs[ci].radius_px) size_score += 0.5;
                if (blobs[ci].radius_px >= blobs[ai].radius_px) size_score += 0.5;
                const double score = static_cast<double>(matches.size()) + size_score;
                if (score > best_score) {
                    best_score = score;
                    best_initial = initial;
                    best_matches = std::move(matches);
                }
            }
        }
    }
    if (best_initial.empty() || best_matches.size() < 12) {
        result.error = "orientation markers could not seed a physical-target fit";
        result.report = {
            {"detected_blob_count", blobs.size()},
            {"initial_match_count", best_matches.size()},
        };
        result.overlay_bgr = draw_overlay(
            source_image, blobs, {}, false, result.report, {}, std::nullopt);
        return result;
    }

    cv::Mat transform = best_initial;
    for (int iteration = 0; iteration < 4; ++iteration) {
        const double local_scale = 0.5 * (
            cv::norm(transform_point(transform, {1.0, 0.0}) -
                     transform_point(transform, {0.0, 0.0})) +
            cv::norm(transform_point(transform, {0.0, 1.0}) -
                     transform_point(transform, {0.0, 0.0})));
        best_matches = match_points(
            target_points, blobs, transform, std::max(3.0, local_scale * 1.25));
        cv::Mat refined = fit_transform(target_points, blobs, best_matches, false);
        if (refined.empty()) {
            break;
        }
        transform = refined;
    }

    cv::Mat inner_transform = fit_transform(target_points, blobs, best_matches, true);
    cv::Mat full_transform = fit_transform(target_points, blobs, best_matches, false);
    if (inner_transform.empty() || full_transform.empty()) {
        result.error = "insufficient regular points for scale fit and perimeter holdout";
        result.report = {{"matched_point_count", best_matches.size()}};
        result.overlay_bgr = draw_overlay(
            source_image, blobs, {}, false, result.report, {}, std::nullopt);
        return result;
    }
    result.target_mm_to_camera_px = full_transform;
    result.camera_px_to_target_mm = full_transform.inv();
    if (!camera_px_to_canvas_px.empty()) {
        cv::Mat camera_to_canvas;
        camera_px_to_canvas_px.convertTo(camera_to_canvas, CV_64F);
        result.target_mm_to_canvas_px = camera_to_canvas * full_transform;
    }

    std::vector<double> fit_errors;
    std::vector<double> holdout_errors;
    std::map<std::string, cv::Point2d> observed_special;
    for (const auto& target : target_points) {
        const auto matched = best_matches.find(target.point_id);
        if (matched == best_matches.end()) continue;
        const bool regular = target.marker_role == "regular";
        const bool holdout = regular && cv::norm(target.target_mm) >= 30.0;
        const bool used_for_fit = regular && !holdout && target.include_in_fit;
        const cv::Mat& evaluation_transform = holdout ? inner_transform : full_transform;
        const cv::Point2d measured_mm = transform_point(
            evaluation_transform.inv(), blobs[matched->second].center);
        const double error_mm = cv::norm(measured_mm - target.target_mm);
        if (used_for_fit) fit_errors.push_back(error_mm);
        if (holdout) holdout_errors.push_back(error_mm);
        result.correspondences.push_back({
            target.point_id,
            target.target_mm,
            blobs[matched->second].center,
            target.marker_role,
            used_for_fit,
            holdout,
            error_mm,
        });
        if (!regular) observed_special[target.point_id] = blobs[matched->second].center;
    }

    const cv::Point2d camera_origin = transform_point(full_transform, {0.0, 0.0});
    const double camera_ppm_x = cv::norm(
        transform_point(full_transform, {1.0, 0.0}) - camera_origin);
    const double camera_ppm_y = cv::norm(
        transform_point(full_transform, {0.0, 1.0}) - camera_origin);
    const double camera_ppm = std::sqrt(camera_ppm_x * camera_ppm_y);
    const double camera_anisotropy =
        std::abs(camera_ppm_x - camera_ppm_y) / std::max(camera_ppm, 1.0e-9);

    double canvas_ppm_x = 0.0;
    double canvas_ppm_y = 0.0;
    double canvas_ppm = 0.0;
    if (!result.target_mm_to_canvas_px.empty()) {
        const cv::Point2d canvas_origin = transform_point(
            result.target_mm_to_canvas_px, {0.0, 0.0});
        canvas_ppm_x = cv::norm(
            transform_point(result.target_mm_to_canvas_px, {1.0, 0.0}) - canvas_origin);
        canvas_ppm_y = cv::norm(
            transform_point(result.target_mm_to_canvas_px, {0.0, 1.0}) - canvas_origin);
        canvas_ppm = std::sqrt(canvas_ppm_x * canvas_ppm_y);
    }

    double c_to_xplus_mm = std::numeric_limits<double>::quiet_NaN();
    if (observed_special.count("C") && observed_special.count("XPLUS")) {
        c_to_xplus_mm = cv::norm(
            transform_point(result.camera_px_to_target_mm, observed_special["C"]) -
            transform_point(result.camera_px_to_target_mm, observed_special["XPLUS"]));
    }
    const double expected_outer_diameter =
        target_definition.value("fabrication", nlohmann::json::object())
            .value("measured_outer_diameter_mm", 77.0);
    const auto outer_diameter = estimate_outer_diameter_mm(
        gray, full_transform, expected_outer_diameter);
    const double matched_fraction = target_points.empty()
                                        ? 0.0
                                        : static_cast<double>(result.correspondences.size()) /
                                              target_points.size();

    nlohmann::json metrics = {
        {"detected_blob_count", blobs.size()},
        {"target_point_count", target_points.size()},
        {"matched_point_count", result.correspondences.size()},
        {"matched_fraction", matched_fraction},
        {"fit_point_count", fit_errors.size()},
        {"fit_rms_mm", rms(fit_errors)},
        {"fit_max_mm", maximum(fit_errors)},
        {"holdout_point_count", holdout_errors.size()},
        {"holdout_rms_mm", rms(holdout_errors)},
        {"holdout_max_mm", maximum(holdout_errors)},
        {"camera_pixels_per_mm", camera_ppm},
        {"camera_pixels_per_mm_x", camera_ppm_x},
        {"camera_pixels_per_mm_y", camera_ppm_y},
        {"camera_scale_anisotropy_fraction", camera_anisotropy},
        {"canvas_pixels_per_mm", canvas_ppm > 0.0 ? nlohmann::json(canvas_ppm) : nlohmann::json(nullptr)},
        {"canvas_pixels_per_mm_x", canvas_ppm_x > 0.0 ? nlohmann::json(canvas_ppm_x) : nlohmann::json(nullptr)},
        {"canvas_pixels_per_mm_y", canvas_ppm_y > 0.0 ? nlohmann::json(canvas_ppm_y) : nlohmann::json(nullptr)},
        {"c_to_xplus_measured_mm", std::isfinite(c_to_xplus_mm) ? nlohmann::json(c_to_xplus_mm) : nlohmann::json(nullptr)},
        {"c_to_xplus_expected_mm", 25.0},
        {"outer_diameter_measured_mm", outer_diameter.has_value() ? nlohmann::json(outer_diameter->diameter_mm) : nlohmann::json(nullptr)},
        {"outer_diameter_expected_mm", expected_outer_diameter},
        {"outer_diameter_valid_ray_count", outer_diameter.has_value() ? nlohmann::json(outer_diameter->valid_ray_count) : nlohmann::json(0)},
        {"outer_diameter_total_ray_count", outer_diameter.has_value() ? nlohmann::json(outer_diameter->total_ray_count) : nlohmann::json(360)},
        {"outer_diameter_support_fraction", outer_diameter.has_value() ? nlohmann::json(outer_diameter->support_fraction) : nlohmann::json(0.0)},
        {"outer_diameter_radial_mad_mm", outer_diameter.has_value() ? nlohmann::json(outer_diameter->radial_mad_mm) : nlohmann::json(nullptr)},
    };

    const auto has_marker = [&](const std::string& role) {
        return std::any_of(
            result.correspondences.begin(), result.correspondences.end(),
            [&](const ObservedPoint& point) { return point.marker_role == role; });
    };
    const bool orientation_markers_identified =
        has_marker("origin") && has_marker("x_orientation") &&
        has_marker("asymmetry");
    nlohmann::json gates = {
        {"orientation_markers_identified", orientation_markers_identified},
        {"coordinate_handedness_resolved", orientation_markers_identified},
        {"minimum_matched_points", static_cast<int>(result.correspondences.size()) >= thresholds.minimum_matched_points},
        {"minimum_matched_fraction", matched_fraction >= thresholds.minimum_matched_fraction},
        {"maximum_fit_rms_mm", rms(fit_errors) <= thresholds.maximum_fit_rms_mm},
        {"maximum_fit_error_mm", maximum(fit_errors) <= thresholds.maximum_fit_error_mm},
        {"maximum_holdout_rms_mm", rms(holdout_errors) <= thresholds.maximum_holdout_rms_mm},
        {"maximum_holdout_error_mm", maximum(holdout_errors) <= thresholds.maximum_holdout_error_mm},
        {"maximum_c_to_xplus_error_mm", std::isfinite(c_to_xplus_mm) &&
             std::abs(c_to_xplus_mm - 25.0) <= thresholds.maximum_c_to_xplus_error_mm},
        {"maximum_camera_scale_anisotropy_fraction",
             camera_anisotropy <= thresholds.maximum_camera_scale_anisotropy_fraction},
        {"outer_diameter_validation",
             outer_diameter.has_value() &&
             std::abs(outer_diameter->diameter_mm - expected_outer_diameter) <=
                 thresholds.maximum_outer_diameter_error_mm},
    };
    result.quality_pass = std::all_of(gates.begin(), gates.end(), [](const auto& item) {
        return item.is_boolean() && item.template get<bool>();
    });

    nlohmann::json correspondences = nlohmann::json::array();
    for (const auto& point : result.correspondences) {
        correspondences.push_back({
            {"point_id", point.point_id},
            {"target_mm", {point.target_mm.x, point.target_mm.y}},
            {"camera_native_px", {point.camera_px.x, point.camera_px.y}},
            {"marker_role", point.marker_role},
            {"used_for_fit", point.used_for_fit},
            {"holdout", point.holdout},
            {"residual_mm", point.residual_mm},
        });
    }
    result.report = {
        {"schema_id", "orange.calibration.projected_surface_scale_observation"},
        {"schema_version", 1},
        {"status", result.quality_pass ? "passed" : "failed_quality_gate"},
        {"coordinate_contract", {
            {"source", "camera_native_px"},
            {"measurement_destination", "physical_target_mm"},
            {"runtime_destination", "final_display_canvas_px"},
            {"target_origin", "center of marker C"},
            {"target_positive_x", "C toward XPLUS"},
            {"target_positive_y", "image-down target convention"},
        }},
        {"plane_contract", {
            {"target_plane", "projected_surface"},
            {"illuminated_hole_center_plane_z_mm", 0.0},
            {"camera_facing_target_surface_z_mm", 3.0},
            {"occluding_target_thickness_mm", 3.0},
        }},
        {"metrics", metrics},
        {"quality_thresholds", {
            {"minimum_matched_points", thresholds.minimum_matched_points},
            {"minimum_matched_fraction", thresholds.minimum_matched_fraction},
            {"maximum_fit_rms_mm", thresholds.maximum_fit_rms_mm},
            {"maximum_fit_error_mm", thresholds.maximum_fit_error_mm},
            {"maximum_holdout_rms_mm", thresholds.maximum_holdout_rms_mm},
            {"maximum_holdout_error_mm", thresholds.maximum_holdout_error_mm},
            {"maximum_c_to_xplus_error_mm", thresholds.maximum_c_to_xplus_error_mm},
            {"maximum_camera_scale_anisotropy_fraction", thresholds.maximum_camera_scale_anisotropy_fraction},
            {"maximum_outer_diameter_error_mm", thresholds.maximum_outer_diameter_error_mm},
        }},
        {"quality_gates", gates},
        {"transforms", {
            {"target_mm_to_camera_native_px", matrix_json(result.target_mm_to_camera_px)},
            {"camera_native_px_to_target_mm", matrix_json(result.camera_px_to_target_mm)},
            {"target_mm_to_final_display_canvas_px", matrix_json(result.target_mm_to_canvas_px)},
        }},
        {"correspondences", std::move(correspondences)},
        {"authority", {
            {"image_detection_and_correspondence_owner", "orange"},
            {"runtime_scale_candidate_and_promotion_owner", "citrus"},
            {"outer_diameter_used_for_rescaling", false},
            {"special_markers_used_for_final_fit", false},
        }},
    };
    result.overlay_bgr = draw_overlay(
        source_image, blobs, result.correspondences, result.quality_pass, metrics,
        full_transform, outer_diameter);
    result.ok = true;
    return result;
}

}  // namespace orange::gui::projected_surface_scale
