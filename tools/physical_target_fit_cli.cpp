// Offline physical-target scale fit.
//
// Runs the GUI's projected-surface scale fit (orange::gui::projected_surface_scale::analyze,
// same target definition, same quality thresholds) on a saved image, without a canvas
// homography, then repeats Citrus's authoritative refit of the Orange correspondences
// (citrus src/runtime/projected_surface_scale_candidate_controller.cpp, Refit()). The Citrus
// refit is what Citrus accepts as `scale.camera_pixels_per_mm`, so `camera_px_per_mm` in the
// report is the refit value; the Orange analysis value is reported beside it.
//
// Used for a target that is not on the projection surface (e.g. on a dish floor), and to
// reproduce a gel fit from a saved calibration session. Writes fit_report.json and overlay.png
// into --out and never overwrites an existing fit_report.json.
//
// Exit codes: 0 fit ran and every gate passed; 1 analysis failed (report written when
// possible); 2 usage or input error, nothing written; 3 fit ran but a quality gate failed.

#include "gui/projected_surface_scale_analysis.h"
#include "outputs/common/atomic_file.h"
#include "outputs/common/digest.h"

#include "json.hpp"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#ifndef ORANGE_SOURCE_DIR
#define ORANGE_SOURCE_DIR ""
#endif

namespace {

namespace fs = std::filesystem;
namespace pss = orange::gui::projected_surface_scale;
using nlohmann::json;

constexpr int kExitOk = 0;
constexpr int kExitAnalysisFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitQualityFailed = 3;

constexpr const char* kDefaultTargetId = "acrylic_hole_target_78mm_pitch5_margin3_v002";
constexpr const char* kGuiScalePurpose = "projected_surface_scale_calibration";

struct Options {
    fs::path image;
    fs::path black;
    fs::path session_dir;
    std::string camera;
    std::string arena;
    std::string purpose;
    std::string capture_group;
    fs::path target_json;
    fs::path out_dir;
    std::string placement_label;
    std::optional<double> target_bottom_z_mm;
};

void usage(std::ostream& out)
{
    out << "Usage:\n"
           "  physical_target_fit_cli --image <png> [--black <png>] [--camera <serial>]\n"
           "                          [--target-json <json>] --out <dir> [placement options]\n"
           "  physical_target_fit_cli --session-dir <calsess dir> --camera <serial>\n"
           "                          [--arena <arena_id>] [--purpose <purpose>]\n"
           "                          [--capture-group <id>] [--black <png>]\n"
           "                          [--target-json <json>] --out <dir> [placement options]\n"
           "\n"
           "  --session-dir   finds artifacts/Cam<serial>_<arena>/image_set.json and the one\n"
           "                  image with role physical_target (narrowed by --purpose and\n"
           "                  --capture-group when the set holds several). The target definition\n"
           "                  defaults to the session copy that image names, as the GUI does.\n"
           "  --image         analyses a saved image. The target definition defaults to\n"
           "                  $ORANGE_PHYSICAL_TARGET_JSON, else the Citrus bundle the GUI uses.\n"
           "  --black         optional dark frame subtracted (saturating) before the fit. The GUI\n"
           "                  path does not subtract one; the report records it when used.\n"
           "  --placement-label <text>     where the target lay (e.g. gel, dish_floor)\n"
           "  --target-bottom-z-mm <z>     height of the target's bottom face above the gel\n"
           "  --out           output directory; fit_report.json there is never overwritten.\n"
           "\n"
           "Exit: 0 pass, 1 analysis failed, 2 usage/input error, 3 a quality gate failed.\n";
}

bool parse_args(int argc, char** argv, Options* options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto next = [&](std::string* value) {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                return false;
            }
            *value = argv[++index];
            return true;
        };
        std::string value;
        if (arg == "-h" || arg == "--help") {
            usage(std::cout);
            std::exit(kExitOk);
        } else if (arg == "--image") {
            if (!next(&value)) return false;
            options->image = value;
        } else if (arg == "--black") {
            if (!next(&value)) return false;
            options->black = value;
        } else if (arg == "--session-dir") {
            if (!next(&value)) return false;
            options->session_dir = value;
        } else if (arg == "--camera") {
            if (!next(&options->camera)) return false;
        } else if (arg == "--arena") {
            if (!next(&options->arena)) return false;
        } else if (arg == "--purpose") {
            if (!next(&options->purpose)) return false;
        } else if (arg == "--capture-group") {
            if (!next(&options->capture_group)) return false;
        } else if (arg == "--target-json") {
            if (!next(&value)) return false;
            options->target_json = value;
        } else if (arg == "--out") {
            if (!next(&value)) return false;
            options->out_dir = value;
        } else if (arg == "--placement-label") {
            if (!next(&options->placement_label)) return false;
        } else if (arg == "--target-bottom-z-mm") {
            if (!next(&value)) return false;
            try {
                options->target_bottom_z_mm = std::stod(value);
            } catch (const std::exception&) {
                std::cerr << "invalid --target-bottom-z-mm: " << value << "\n";
                return false;
            }
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    if (options->image.empty() == options->session_dir.empty()) {
        std::cerr << "give exactly one of --image or --session-dir\n";
        return false;
    }
    if (!options->session_dir.empty() && options->camera.empty()) {
        std::cerr << "--session-dir needs --camera\n";
        return false;
    }
    if (options->out_dir.empty()) {
        std::cerr << "--out is required\n";
        return false;
    }
    return true;
}

bool read_json_object(const fs::path& path, json* value, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "could not open JSON: " + path.string();
        return false;
    }
    *value = json::parse(input, nullptr, false);
    if (value->is_discarded() || !value->is_object()) {
        *error = "invalid JSON object: " + path.string();
        return false;
    }
    return true;
}

std::string sha256_token_of(const fs::path& path)
{
    std::string token;
    std::string error;
    if (!orange::outputs::file_sha256_token(path, &token, &error)) return {};
    return token;
}

// Same default as spatial_layout::default_physical_calibration_target_json_path() (not linked
// here: that translation unit pulls in the calibration request machinery).
fs::path default_target_json_path()
{
    if (const char* env = std::getenv("ORANGE_PHYSICAL_TARGET_JSON")) {
        if (env[0] != '\0') return fs::path(env);
    }
    const fs::path root("/home/jeremy/citrus/targets/physical_calibration_targets");
    const fs::path expected = root / kDefaultTargetId / (std::string(kDefaultTargetId) + ".json");
    if (fs::exists(expected)) return expected;
    return root / "acrylic_hole_target_788mm_pitch5_margin3_v002" /
           (std::string(kDefaultTargetId) + ".json");
}

std::string run_capture(const std::string& command)
{
    std::string output;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    ::pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

json tool_provenance()
{
    json provenance = {
        {"tool", "physical_target_fit_cli"},
        {"analysis_function", "orange::gui::projected_surface_scale::analyze"},
        {"refit_method", "cv::findHomography_from_orange_correspondences"},
        {"refit_reference",
         "citrus src/runtime/projected_surface_scale_candidate_controller.cpp Refit()"},
    };
    const std::string source_dir = ORANGE_SOURCE_DIR;
    if (!source_dir.empty() && fs::is_directory(source_dir)) {
        const std::string head = run_capture(
            "git -C '" + source_dir + "' rev-parse HEAD 2>/dev/null");
        const std::string dirty = run_capture(
            "git -C '" + source_dir + "' status --porcelain -- "
            "tools/physical_target_fit_cli.cpp "
            "src/gui/projected_surface_scale_analysis.cpp "
            "src/gui/projected_surface_scale_analysis.h 2>/dev/null");
        provenance["source_dir"] = source_dir;
        provenance["source_head_at_run"] = head.empty() ? json(nullptr) : json(head);
        provenance["fit_sources_modified_at_run"] = !dirty.empty();
    }
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        provenance["executable_path"] = exe.string();
        const std::string token = sha256_token_of(exe);
        provenance["executable_sha256"] = token.empty() ? json(nullptr) : json(token);
    }
    return provenance;
}

std::string utc_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

cv::Point2d transform_point(const cv::Mat& transform, const cv::Point2d& point)
{
    std::vector<cv::Point2d> input{point};
    std::vector<cv::Point2d> output;
    cv::perspectiveTransform(input, output, transform);
    return output.front();
}

double rms(const std::vector<double>& values)
{
    if (values.empty()) return std::numeric_limits<double>::infinity();
    const double sum = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);
    return std::sqrt(sum / values.size());
}

double maximum(const std::vector<double>& values)
{
    return values.empty() ? std::numeric_limits<double>::infinity()
                          : *std::max_element(values.begin(), values.end());
}

json finite_or_null(double value)
{
    return std::isfinite(value) ? json(value) : json(nullptr);
}

// Port of Citrus Refit() without the canvas half: camera->mm homographies from the Orange
// correspondences (RANSAC 0.25 mm), fit residuals of the inner points under the full fit,
// holdout residuals under the inner fit, and the local Jacobian scale at the target origin.
struct Refit {
    bool ok = false;
    std::string error;
    cv::Mat camera_to_mm;
    cv::Mat mm_to_camera;
    double fit_rms_mm = 0.0;
    double fit_max_mm = 0.0;
    double holdout_rms_mm = 0.0;
    double holdout_max_mm = 0.0;
    int fit_count = 0;
    int holdout_count = 0;
    double camera_ppm = 0.0;
    double camera_ppm_x = 0.0;
    double camera_ppm_y = 0.0;
    double camera_anisotropy = 0.0;
};

Refit citrus_refit(const std::vector<pss::ObservedPoint>& correspondences)
{
    Refit result;
    std::vector<cv::Point2d> fit_camera, fit_mm, all_camera, all_mm;
    std::vector<std::pair<cv::Point2d, cv::Point2d>> holdout;
    for (const auto& point : correspondences) {
        if (point.marker_role != "regular") continue;
        all_camera.push_back(point.camera_px);
        all_mm.push_back(point.target_mm);
        if (point.holdout) {
            holdout.push_back({point.camera_px, point.target_mm});
        } else if (point.used_for_fit) {
            fit_camera.push_back(point.camera_px);
            fit_mm.push_back(point.target_mm);
        }
    }
    if (fit_camera.size() < 40 || holdout.size() < 8 || all_camera.size() < 80) {
        result.error = "insufficient_scale_correspondences";
        return result;
    }
    const cv::Mat inner = cv::findHomography(fit_camera, fit_mm, cv::RANSAC, 0.25);
    const cv::Mat full = cv::findHomography(all_camera, all_mm, cv::RANSAC, 0.25);
    if (inner.empty() || full.empty()) {
        result.error = "scale_correspondence_refit_failed";
        return result;
    }
    std::vector<double> fit_errors;
    for (std::size_t index = 0; index < fit_camera.size(); ++index) {
        fit_errors.push_back(cv::norm(transform_point(full, fit_camera[index]) - fit_mm[index]));
    }
    std::vector<double> holdout_errors;
    for (const auto& point : holdout) {
        holdout_errors.push_back(cv::norm(transform_point(inner, point.first) - point.second));
    }
    const cv::Mat mm_to_camera = full.inv();
    const cv::Point2d origin = transform_point(mm_to_camera, {0.0, 0.0});
    const double x = cv::norm(transform_point(mm_to_camera, {1.0, 0.0}) - origin);
    const double y = cv::norm(transform_point(mm_to_camera, {0.0, 1.0}) - origin);
    result.ok = true;
    result.camera_to_mm = full;
    result.mm_to_camera = mm_to_camera;
    result.fit_rms_mm = rms(fit_errors);
    result.fit_max_mm = maximum(fit_errors);
    result.holdout_rms_mm = rms(holdout_errors);
    result.holdout_max_mm = maximum(holdout_errors);
    result.fit_count = static_cast<int>(fit_errors.size());
    result.holdout_count = static_cast<int>(holdout_errors.size());
    result.camera_ppm_x = x;
    result.camera_ppm_y = y;
    result.camera_ppm = std::sqrt(x * y);
    result.camera_anisotropy =
        std::abs(x - y) / std::max(result.camera_ppm, 1.0e-12);
    return result;
}

json gate(const std::string& name, const std::string& source, const json& value,
          const json& threshold, const std::string& comparison, bool pass)
{
    return {
        {"name", name},
        {"source", source},
        {"value", value},
        {"threshold", threshold},
        {"comparison", comparison},
        {"pass", pass},
    };
}

// The analysis report's gates are booleans; pair each with the metric and threshold it tests.
json orange_gates(const json& report)
{
    const json metrics = report.value("metrics", json::object());
    const json thresholds = report.value("quality_thresholds", json::object());
    const json gates = report.value("quality_gates", json::object());
    auto metric = [&](const char* key) {
        return metrics.contains(key) ? metrics[key] : json(nullptr);
    };
    auto threshold = [&](const char* key) {
        return thresholds.contains(key) ? thresholds[key] : json(nullptr);
    };
    auto passed = [&](const char* key) { return gates.value(key, false); };
    json c_to_xplus_error = nullptr;
    if (metrics.contains("c_to_xplus_measured_mm") && metrics["c_to_xplus_measured_mm"].is_number()) {
        c_to_xplus_error = std::abs(metrics["c_to_xplus_measured_mm"].get<double>() -
                                    metrics.value("c_to_xplus_expected_mm", 25.0));
    }
    json diameter_error = nullptr;
    if (metrics.contains("outer_diameter_measured_mm") &&
        metrics["outer_diameter_measured_mm"].is_number()) {
        diameter_error = std::abs(metrics["outer_diameter_measured_mm"].get<double>() -
                                  metrics.value("outer_diameter_expected_mm", 77.0));
    }
    const std::string source = "orange_analysis";
    return json::array({
        gate("orientation_markers_identified", source, passed("orientation_markers_identified"),
             true, "==", passed("orientation_markers_identified")),
        gate("coordinate_handedness_resolved", source, passed("coordinate_handedness_resolved"),
             true, "==", passed("coordinate_handedness_resolved")),
        gate("minimum_matched_points", source, metric("matched_point_count"),
             threshold("minimum_matched_points"), ">=", passed("minimum_matched_points")),
        gate("minimum_matched_fraction", source, metric("matched_fraction"),
             threshold("minimum_matched_fraction"), ">=", passed("minimum_matched_fraction")),
        gate("maximum_fit_rms_mm", source, metric("fit_rms_mm"),
             threshold("maximum_fit_rms_mm"), "<=", passed("maximum_fit_rms_mm")),
        gate("maximum_fit_error_mm", source, metric("fit_max_mm"),
             threshold("maximum_fit_error_mm"), "<=", passed("maximum_fit_error_mm")),
        gate("maximum_holdout_rms_mm", source, metric("holdout_rms_mm"),
             threshold("maximum_holdout_rms_mm"), "<=", passed("maximum_holdout_rms_mm")),
        gate("maximum_holdout_error_mm", source, metric("holdout_max_mm"),
             threshold("maximum_holdout_error_mm"), "<=", passed("maximum_holdout_error_mm")),
        gate("maximum_c_to_xplus_error_mm", source, c_to_xplus_error,
             threshold("maximum_c_to_xplus_error_mm"), "<=",
             passed("maximum_c_to_xplus_error_mm")),
        gate("maximum_camera_scale_anisotropy_fraction", source,
             metric("camera_scale_anisotropy_fraction"),
             threshold("maximum_camera_scale_anisotropy_fraction"), "<=",
             passed("maximum_camera_scale_anisotropy_fraction")),
        gate("outer_diameter_validation", source, diameter_error,
             threshold("maximum_outer_diameter_error_mm"), "<=",
             passed("outer_diameter_validation")),
    });
}

// Citrus's candidate gates (fit 0.15/0.50, holdout 0.25/0.75, anisotropy 0.03, Orange/Citrus
// agreement 0.5 %). Citrus tests anisotropy and agreement on the canvas scale; without a canvas
// they are applied to the camera scale.
json refit_gates(const Refit& refit, double orange_camera_ppm)
{
    const std::string source = "citrus_equivalent_refit";
    const double difference = orange_camera_ppm > 0.0
        ? std::abs(refit.camera_ppm - orange_camera_ppm) / orange_camera_ppm
        : std::numeric_limits<double>::infinity();
    return json::array({
        gate("refit_ok", source, refit.ok, true, "==", refit.ok),
        gate("refit_maximum_fit_rms_mm", source, finite_or_null(refit.fit_rms_mm), 0.15, "<=",
             refit.ok && refit.fit_rms_mm <= 0.15),
        gate("refit_maximum_fit_error_mm", source, finite_or_null(refit.fit_max_mm), 0.50, "<=",
             refit.ok && refit.fit_max_mm <= 0.50),
        gate("refit_maximum_holdout_rms_mm", source, finite_or_null(refit.holdout_rms_mm), 0.25,
             "<=", refit.ok && refit.holdout_rms_mm <= 0.25),
        gate("refit_maximum_holdout_error_mm", source, finite_or_null(refit.holdout_max_mm), 0.75,
             "<=", refit.ok && refit.holdout_max_mm <= 0.75),
        gate("refit_maximum_camera_scale_anisotropy_fraction", source,
             finite_or_null(refit.camera_anisotropy), 0.03, "<=",
             refit.ok && refit.camera_anisotropy <= 0.03),
        gate("orange_refit_camera_ppm_difference_fraction", source, finite_or_null(difference),
             0.005, "<=", refit.ok && difference <= 0.005),
    });
}

struct SessionImage {
    fs::path artifact_dir;
    fs::path image_set_path;
    fs::path image_path;
    json image;
};

bool find_session_image(const Options& options, SessionImage* out, std::string* error)
{
    const fs::path artifacts = options.session_dir / "artifacts";
    if (!fs::is_directory(artifacts)) {
        *error = "session has no artifacts directory: " + artifacts.string();
        return false;
    }
    const std::string prefix = "Cam" + options.camera + "_";
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(artifacts)) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (!options.arena.empty() && name != prefix + options.arena) continue;
        if (!fs::is_regular_file(entry.path() / "image_set.json")) continue;
        candidates.push_back(entry.path());
    }
    if (candidates.size() != 1) {
        *error = "expected one artifacts/" + prefix + "<arena>/image_set.json, found " +
                 std::to_string(candidates.size()) + " (use --arena)";
        return false;
    }
    out->artifact_dir = candidates.front();
    out->image_set_path = out->artifact_dir / "image_set.json";
    json image_set;
    if (!read_json_object(out->image_set_path, &image_set, error)) return false;
    if (!image_set.contains("images") || !image_set["images"].is_array()) {
        *error = "image_set.json has no images array";
        return false;
    }
    std::vector<const json*> matches;
    for (const auto& image : image_set["images"]) {
        if (!image.is_object() || image.value("role", "") != "physical_target") continue;
        if (!options.purpose.empty() && image.value("purpose", "") != options.purpose) continue;
        if (!options.capture_group.empty() &&
            image.value("capture", json::object()).value("capture_group_id", "") !=
                options.capture_group) {
            continue;
        }
        matches.push_back(&image);
    }
    if (matches.size() != 1) {
        *error = "expected one physical_target image in " + out->image_set_path.string() +
                 ", found " + std::to_string(matches.size()) +
                 " (narrow with --purpose / --capture-group)";
        return false;
    }
    out->image = *matches.front();
    const std::string relative = out->image.value("path", "");
    if (relative.empty()) {
        *error = "selected image has no path";
        return false;
    }
    out->image_path = out->artifact_dir / relative;
    return true;
}

bool write_once(const fs::path& path, const std::string& bytes, std::string* error)
{
    if (fs::exists(path)) {
        *error = "refusing to overwrite " + path.string();
        return false;
    }
    orange::outputs::CreateOnceOptions create_options;
    create_options.read_only = false;
    create_options.durability = orange::outputs::Durability::kFsync;
    return orange::outputs::create_file_once(path, bytes, create_options, error);
}

json input_file(const fs::path& path)
{
    const std::string token = sha256_token_of(path);
    return {
        {"path", fs::absolute(path).lexically_normal().string()},
        {"sha256", token.empty() ? json(nullptr) : json(token)},
    };
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_args(argc, argv, &options)) {
        usage(std::cerr);
        return kExitUsage;
    }
    const fs::path report_path = options.out_dir / "fit_report.json";
    const fs::path overlay_path = options.out_dir / "overlay.png";
    if (fs::exists(report_path) || fs::exists(overlay_path)) {
        std::cerr << "refusing to overwrite existing output in " << options.out_dir << "\n";
        return kExitUsage;
    }

    std::string error;
    json source = json::object();
    json placement = json::object();
    fs::path image_path = options.image;
    fs::path target_json_path = options.target_json;
    std::string target_json_origin = "command_line";
    if (!options.session_dir.empty()) {
        SessionImage session_image;
        if (!find_session_image(options, &session_image, &error)) {
            std::cerr << error << "\n";
            return kExitUsage;
        }
        image_path = session_image.image_path;
        const json& image = session_image.image;
        const json physical_target = image.value("physical_target", json::object());
        if (target_json_path.empty()) {
            // The GUI analyses with the session-local copy of the target bundle.
            const std::string session_json = physical_target.value("session_json_path", "");
            if (session_json.empty()) {
                std::cerr << "selected image names no physical_target.session_json_path\n";
                return kExitUsage;
            }
            target_json_path = session_image.artifact_dir / session_json;
            target_json_origin = "session_copy_named_by_image";
        }
        const json capture = image.value("capture", json::object());
        source = {
            {"mode", "session"},
            {"session_dir", fs::absolute(options.session_dir).lexically_normal().string()},
            {"artifact_dir", session_image.artifact_dir.string()},
            {"image_set", input_file(session_image.image_set_path)},
            {"image_purpose", image.value("purpose", "")},
            {"image_checksum_algorithm", image.value("checksum_algorithm", "")},
            {"image_checksum", image.value("checksum", "")},
            {"capture_group_id", capture.value("capture_group_id", "")},
            {"first_camera_frame_id", capture.value("first_camera_frame_id", json(nullptr))},
            {"camera_timestamp_ns", capture.value("camera_timestamp_ns", json(nullptr))},
            {"exposure_us", capture.value("exposure_us", json(nullptr))},
            {"target_provenance", physical_target},
        };
        for (const char* key : {"plane_id", "target_plane", "plane_z_mm_nominal",
                                "z_mm_relative_to_projection_surface", "wet_or_dry",
                                "dish_installed", "fill_state", "fixture_state"}) {
            if (image.contains(key)) placement["image_set_" + std::string(key)] = image[key];
        }
    } else {
        source = {{"mode", "image"}};
        if (target_json_path.empty()) {
            target_json_path = default_target_json_path();
            target_json_origin = std::getenv("ORANGE_PHYSICAL_TARGET_JSON") != nullptr
                ? "ORANGE_PHYSICAL_TARGET_JSON"
                : "gui_default_citrus_bundle";
        }
    }
    placement["label"] = options.placement_label.empty() ? json(nullptr)
                                                         : json(options.placement_label);
    placement["target_bottom_z_mm_above_gel"] = options.target_bottom_z_mm.has_value()
        ? json(*options.target_bottom_z_mm)
        : json(nullptr);

    std::vector<pss::TargetPoint> target_points;
    json target_definition;
    if (!pss::load_target_definition(target_json_path, &target_points, &target_definition, &error)) {
        std::cerr << error << "\n";
        return kExitUsage;
    }
    // Same check as the GUI's analyze_and_write_group.
    const double thickness_mm = target_definition.value("fabrication", json::object())
                                    .value("thickness_mm", 0.0);
    if (!std::isfinite(thickness_mm) || std::abs(thickness_mm - 3.0) > 0.01) {
        std::cerr << "physical target thickness is not the commissioned 3.0 mm\n";
        return kExitUsage;
    }
    const std::string target_id = target_definition.value(
        "physical_target_id", target_definition.value("target_id", ""));

    cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_UNCHANGED);
    if (image.empty()) {
        std::cerr << "could not read image: " << image_path << "\n";
        return kExitUsage;
    }
    json black_info = nullptr;
    if (!options.black.empty()) {
        cv::Mat black = cv::imread(options.black.string(), cv::IMREAD_UNCHANGED);
        if (black.empty() || black.size() != image.size() || black.type() != image.type()) {
            std::cerr << "black frame is unreadable or does not match the image size/type\n";
            return kExitUsage;
        }
        cv::Mat subtracted;
        cv::subtract(image, black, subtracted);  // saturating
        image = subtracted;
        black_info = input_file(options.black);
    }

    const pss::AnalysisResult analysis =
        pss::analyze(image, target_points, target_definition, cv::Mat(), pss::QualityThresholds{});

    json report = {
        {"schema_id", "orange.calibration.physical_target_fit_report"},
        {"schema_version", 1},
        {"created_utc", utc_now()},
        {"camera_id", options.camera.empty() ? json(nullptr) : json(options.camera)},
        {"inputs", {
            {"image", input_file(image_path)},
            {"image_shape", {{"width", image.cols}, {"height", image.rows},
                             {"channels", image.channels()}, {"depth", image.depth()}}},
            {"black_frame_subtracted", black_info},
            {"source", source},
            {"canvas_homography", nullptr},
        }},
        {"target", {
            {"target_id", target_id},
            {"definition", input_file(target_json_path)},
            {"definition_origin", target_json_origin},
            {"thickness_mm", thickness_mm},
        }},
        {"target_placement", placement},
        {"tool", tool_provenance()},
        {"notes", json::array({
            "camera_px_per_mm is Citrus's authoritative refit of the Orange correspondences "
            "(the value Citrus accepts as scale.camera_pixels_per_mm); "
            "camera_px_per_mm_orange_analysis is analyze()'s own metric.",
            "analysis_report.plane_contract is analyze()'s fixed projected-surface text; "
            "target_placement says where this target actually lay.",
            "No canvas homography: canvas metrics in analysis_report are null.",
        })},
    };

    int exit_code = kExitOk;
    if (!analysis.ok) {
        report["status"] = "analysis_failed";
        report["error"] = analysis.error;
        report["analysis_report"] = analysis.report;
        report["quality_pass"] = false;
        exit_code = kExitAnalysisFailed;
    } else {
        const Refit refit = citrus_refit(analysis.correspondences);
        const json metrics = analysis.report.value("metrics", json::object());
        const double orange_ppm = metrics.value("camera_pixels_per_mm", 0.0);
        json gates = orange_gates(analysis.report);
        for (const auto& item : refit_gates(refit, orange_ppm)) gates.push_back(item);
        const bool gates_pass = std::all_of(gates.begin(), gates.end(), [](const json& item) {
            return item.value("pass", false);
        });
        const bool quality_pass = analysis.quality_pass && gates_pass;
        report["status"] = quality_pass ? "passed" : "failed_quality_gate";
        report["quality_pass"] = quality_pass;
        report["orange_analysis_quality_pass"] = analysis.quality_pass;
        report["camera_px_per_mm"] = refit.ok ? json(refit.camera_ppm) : json(nullptr);
        report["camera_px_per_mm_source"] = "citrus_equivalent_refit";
        report["camera_px_per_mm_orange_analysis"] = orange_ppm;
        report["fit_rms_mm"] = refit.ok ? finite_or_null(refit.fit_rms_mm) : json(nullptr);
        report["holdout_rms_mm"] = refit.ok ? finite_or_null(refit.holdout_rms_mm) : json(nullptr);
        report["citrus_equivalent_refit"] = {
            {"ok", refit.ok},
            {"error", refit.error},
            {"method", "cv::findHomography_from_orange_correspondences"},
            {"ransac_reprojection_threshold_mm", 0.25},
            {"camera_pixels_per_mm", refit.ok ? json(refit.camera_ppm) : json(nullptr)},
            {"camera_pixels_per_mm_x", refit.ok ? json(refit.camera_ppm_x) : json(nullptr)},
            {"camera_pixels_per_mm_y", refit.ok ? json(refit.camera_ppm_y) : json(nullptr)},
            {"camera_scale_anisotropy_fraction",
             refit.ok ? json(refit.camera_anisotropy) : json(nullptr)},
            {"fit_point_count", refit.fit_count},
            {"fit_rms_mm", refit.ok ? finite_or_null(refit.fit_rms_mm) : json(nullptr)},
            {"fit_max_mm", refit.ok ? finite_or_null(refit.fit_max_mm) : json(nullptr)},
            {"holdout_point_count", refit.holdout_count},
            {"holdout_rms_mm", refit.ok ? finite_or_null(refit.holdout_rms_mm) : json(nullptr)},
            {"holdout_max_mm", refit.ok ? finite_or_null(refit.holdout_max_mm) : json(nullptr)},
            {"camera_native_px_to_target_mm", pss::matrix_json(refit.camera_to_mm)},
            {"target_mm_to_camera_native_px", pss::matrix_json(refit.mm_to_camera)},
        };
        report["orange_analysis_summary"] = {
            {"camera_pixels_per_mm", orange_ppm},
            {"fit_rms_mm", metrics.contains("fit_rms_mm") ? metrics["fit_rms_mm"] : json(nullptr)},
            {"holdout_rms_mm",
             metrics.contains("holdout_rms_mm") ? metrics["holdout_rms_mm"] : json(nullptr)},
            {"target_mm_to_camera_native_px", pss::matrix_json(analysis.target_mm_to_camera_px)},
        };
        report["target_mm_to_camera_px"] = refit.ok ? pss::matrix_json(refit.mm_to_camera)
                                                    : json(nullptr);
        report["quality_gates"] = gates;
        report["analysis_report"] = analysis.report;
        exit_code = quality_pass ? kExitOk : kExitQualityFailed;
    }

    if (!analysis.overlay_bgr.empty()) {
        std::vector<unsigned char> png;
        if (!cv::imencode(".png", analysis.overlay_bgr, png)) {
            std::cerr << "could not encode overlay\n";
            return kExitAnalysisFailed;
        }
        if (!write_once(overlay_path, std::string(png.begin(), png.end()), &error)) {
            std::cerr << error << "\n";
            return kExitAnalysisFailed;
        }
        report["overlay_png"] = input_file(overlay_path);
    } else {
        report["overlay_png"] = nullptr;
    }
    if (!write_once(report_path, report.dump(2) + "\n", &error)) {
        std::cerr << error << "\n";
        return kExitAnalysisFailed;
    }

    std::cout << "status=" << report.value("status", "")
              << " camera=" << (options.camera.empty() ? "-" : options.camera);
    if (report.contains("camera_px_per_mm") && report["camera_px_per_mm"].is_number()) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      " camera_px_per_mm=%.6f orange_analysis=%.6f fit_rms_mm=%.4f holdout_rms_mm=%.4f",
                      report["camera_px_per_mm"].get<double>(),
                      report.value("camera_px_per_mm_orange_analysis", 0.0),
                      report["fit_rms_mm"].is_number() ? report["fit_rms_mm"].get<double>() : -1.0,
                      report["holdout_rms_mm"].is_number() ? report["holdout_rms_mm"].get<double>()
                                                           : -1.0);
        std::cout << line;
    }
    if (!analysis.ok) std::cout << " error=\"" << analysis.error << "\"";
    std::cout << "\n" << report_path.string() << "\n";
    return exit_code;
}
