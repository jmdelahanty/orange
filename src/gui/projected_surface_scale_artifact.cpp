#include "gui/projected_surface_scale_artifact.h"

#include "fsuid_guard.h"
#include "gui/projected_surface_scale_analysis.h"
#include "gui/spatial_layout/physical_target_bundle.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace orange::gui::projected_surface_scale {
namespace {

namespace fs = std::filesystem;

bool read_json(const fs::path& path, nlohmann::json* value, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "could not open JSON: " + path.string();
        return false;
    }
    *value = nlohmann::json::parse(input, nullptr, false);
    if (value->is_discarded() || !value->is_object()) {
        if (error) *error = "invalid JSON object: " + path.string();
        return false;
    }
    return true;
}

bool write_bytes_exclusive(const fs::path& path,
                           const std::vector<unsigned char>& bytes,
                           std::string* error)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create scale artifact directory: " + ec.message();
        return false;
    }
    if (fs::exists(path)) {
        if (error) *error = "refusing to overwrite immutable scale artifact: " + path.string();
        return false;
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error) *error = "could not create temporary scale artifact: " + temporary.string();
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            if (error) *error = "could not write temporary scale artifact: " + temporary.string();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        if (error) *error = "could not publish scale artifact: " + ec.message();
        return false;
    }
    return true;
}

bool write_json_exclusive(const fs::path& path,
                          const nlohmann::json& value,
                          std::string* error)
{
    const std::string text = value.dump(2) + "\n";
    return write_bytes_exclusive(
        path,
        std::vector<unsigned char>(text.begin(), text.end()),
        error);
}

bool write_png_exclusive(const fs::path& path,
                         const cv::Mat& image,
                         std::string* error)
{
    std::vector<unsigned char> bytes;
    if (!cv::imencode(".png", image, bytes)) {
        if (error) *error = "could not encode projected-surface scale overlay";
        return false;
    }
    return write_bytes_exclusive(path, bytes, error);
}

const CitrusSpatialTemplateState* template_for_camera(
    const SpatialLayoutUiState& state,
    const std::string& camera_serial)
{
    const CitrusSpatialTemplateState* match = nullptr;
    for (const auto& candidate : state.citrus_canvas_templates) {
        if (candidate.source_camera_id != camera_serial) continue;
        if (match != nullptr) return nullptr;
        match = &candidate;
    }
    return match;
}

cv::Mat homography_from_template(const CitrusSpatialTemplateState& source)
{
    cv::Mat matrix(3, 3, CV_64F);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            matrix.at<double>(row, column) =
                source.camera_to_canvas_homography[static_cast<std::size_t>(row * 3 + column)];
        }
    }
    return matrix;
}

cv::Mat gray_from_capture(const SpatialLayoutGroupCaptureFrame& capture)
{
    const std::size_t expected = static_cast<std::size_t>(capture.width) *
                                 static_cast<std::size_t>(capture.height) * 4u;
    if (!capture.valid || capture.width <= 0 || capture.height <= 0 ||
        capture.rgba.size() < expected) {
        return {};
    }
    const cv::Mat rgba(capture.height, capture.width, CV_8UC4,
                       const_cast<unsigned char*>(capture.rgba.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    return gray;
}

const nlohmann::json* find_source_image(
    const nlohmann::json& image_set,
    const std::string& capture_group_id)
{
    if (!image_set.contains("images") || !image_set["images"].is_array()) return nullptr;
    const nlohmann::json* match = nullptr;
    for (const auto& image : image_set["images"]) {
        if (!image.is_object() ||
            image.value("purpose", "") != "projected_surface_scale_calibration" ||
            image.value("capture", nlohmann::json::object())
                    .value("capture_group_id", "") != capture_group_id) {
            continue;
        }
        if (match != nullptr) return nullptr;
        match = &image;
    }
    return match;
}

std::string sha256_prefixed(const fs::path& path, std::string* error)
{
    std::string value;
    if (!spatial_layout::compute_file_sha256(path, &value, error)) return {};
    return "sha256:" + value;
}

}  // namespace

GroupArtifactResult analyze_and_write_group(
    const SpatialLayoutUiState& spatial_state,
    const fs::path& calibration_session_dir,
    const fs::path& citrus_canvas_path)
{
    GroupArtifactResult result;
    std::vector<fs::path> newly_written_paths;
    auto fail = [&](const std::string& error) {
        std::error_code cleanup_error;
        for (auto path = newly_written_paths.rbegin();
             path != newly_written_paths.rend(); ++path) {
            fs::remove(*path, cleanup_error);
            cleanup_error.clear();
        }
        result.error = error;
        return result;
    };
    if (calibration_session_dir.empty() || !fs::is_directory(calibration_session_dir)) {
        return fail("projected-surface scale session directory is missing");
    }
    if (spatial_state.group_capture_id.empty() || spatial_state.group_captures.empty()) {
        return fail("projected-surface scale capture group is empty");
    }
    std::string error;
    const std::string canvas_sha256 = sha256_prefixed(citrus_canvas_path, &error);
    if (canvas_sha256.empty()) return fail(error);
    result.canvas_sha256 = canvas_sha256;

    nlohmann::json observation_refs = nlohmann::json::array();
    nlohmann::json verification_targets = nlohmann::json::array();
    std::set<std::string> identities;
    for (const auto& capture : spatial_state.group_captures) {
        const CitrusSpatialTemplateState* canvas_template =
            template_for_camera(spatial_state, capture.camera_serial);
        if (canvas_template == nullptr ||
            canvas_template->source_config_name.empty() ||
            !canvas_template->has_authoritative_camera_to_canvas_homography) {
            return fail("camera " + capture.camera_serial +
                        " lacks one authoritative Citrus arena/homography mapping");
        }
        if (canvas_template->homography_canvas_checksum != canvas_sha256) {
            return fail("camera " + capture.camera_serial +
                        " homography is not bound to the current Citrus canvas checksum");
        }
        const std::string arena_id = canvas_template->source_config_name;
        if (!identities.insert(arena_id + "\n" + capture.camera_serial).second) {
            return fail("duplicate arena/camera in projected-surface scale group");
        }
        const fs::path artifact_dir = calibration_session_dir / "artifacts" /
            ("Cam" + capture.camera_serial + "_" +
             spatial_layout::sanitize_artifact_component(arena_id));
        const fs::path image_set_path = artifact_dir / "image_set.json";
        nlohmann::json image_set;
        if (!read_json(image_set_path, &image_set, &error)) return fail(error);
        const nlohmann::json* image =
            find_source_image(image_set, spatial_state.group_capture_id);
        if (image == nullptr) {
            return fail("could not identify one saved scale source image for " +
                        arena_id + "/" + capture.camera_serial);
        }
        const auto physical_target = image->value(
            "physical_target", nlohmann::json::object());
        const fs::path target_json_path = artifact_dir /
            physical_target.value("session_json_path", "");
        std::vector<TargetPoint> target_points;
        nlohmann::json target_definition;
        if (!load_target_definition(
                target_json_path, &target_points, &target_definition, &error)) {
            return fail(error);
        }
        const double thickness_mm = target_definition
            .value("fabrication", nlohmann::json::object())
            .value("thickness_mm", 0.0);
        if (!std::isfinite(thickness_mm) || std::abs(thickness_mm - 3.0) > 0.01) {
            return fail("physical target thickness is not the commissioned 3.0 mm");
        }
        const cv::Mat gray = gray_from_capture(capture);
        if (gray.empty()) {
            return fail("captured RGBA buffer is invalid for camera " +
                        capture.camera_serial);
        }
        AnalysisResult analysis = analyze(
            gray,
            target_points,
            target_definition,
            homography_from_template(*canvas_template));
        if (!analysis.ok) {
            return fail("scale analysis failed for " + arena_id + "/" +
                        capture.camera_serial + ": " + analysis.error);
        }

        const fs::path source_path = artifact_dir / image->value("path", "");
        const fs::path output_dir = artifact_dir / "scale_observations" /
            spatial_layout::sanitize_artifact_component(spatial_state.group_capture_id);
        const fs::path overlay_path = output_dir / "overlay.png";
        const fs::path observation_path = output_dir / "observation.json";
        analysis.report["rig_id"] = canvas_template->source_rig_name;
        analysis.report["canvas_name"] = canvas_template->source_canvas_name;
        analysis.report["arena_id"] = arena_id;
        analysis.report["camera_id"] = capture.camera_serial;
        analysis.report["target_plane"] = "projected_surface";
        analysis.report["capture_group_id"] = spatial_state.group_capture_id;
        analysis.report["target_id"] = physical_target.value("target_id", "");
        analysis.report["source_capture"] = {
            {"image_path", source_path.string()},
            {"image_checksum_algorithm", image->value("checksum_algorithm", "")},
            {"image_checksum", image->value("checksum", "")},
            {"image_set_path", image_set_path.string()},
            {"first_camera_frame_id", capture.first_camera_frame_id},
            {"last_camera_frame_id", capture.last_camera_frame_id},
            {"camera_timestamp_ns", capture.camera_timestamp_ns},
            {"camera_timestamp_clock_domain", "camera_ptp"},
        };
        analysis.report["target_provenance"] = physical_target;
        analysis.report["active_homography"] = {
            {"authority_status", canvas_template->homography_authority_status},
            {"candidate_id", canvas_template->homography_candidate_id},
            {"candidate_set_id", canvas_template->homography_candidate_set_id},
            {"candidate_json_path", canvas_template->homography_candidate_json_path},
            {"active_pointer_path", canvas_template->homography_active_pointer_path},
            {"canvas_checksum", canvas_template->homography_canvas_checksum},
            {"matrix_camera_native_px_to_final_display_canvas_px",
             matrix_json(homography_from_template(*canvas_template))},
        };
        analysis.report["artifact_paths"] = {
            {"observation_json", observation_path.string()},
            {"overlay_png", overlay_path.string()},
        };
        if (!write_png_exclusive(overlay_path, analysis.overlay_bgr, &error)) {
            return fail(error);
        }
        newly_written_paths.push_back(overlay_path);
        if (!write_json_exclusive(observation_path, analysis.report, &error)) {
            return fail(error);
        }
        newly_written_paths.push_back(observation_path);
        const std::string observation_sha256 =
            sha256_prefixed(observation_path, &error);
        if (observation_sha256.empty()) return fail(error);
        observation_refs.push_back({
            {"arena_id", arena_id},
            {"camera_id", capture.camera_serial},
            {"observation_path", observation_path.string()},
            {"observation_sha256", observation_sha256},
        });
        verification_targets.push_back({
            {"arena_id", arena_id},
            {"camera_id", capture.camera_serial},
            {"status", analysis.quality_pass ? "passed" : "failed"},
            {"metrics", analysis.report.value("metrics", nlohmann::json::object())},
            {"quality_gates", analysis.report.value(
                "quality_gates", nlohmann::json::object())},
            {"overlay_path", overlay_path.string()},
        });
        if (!analysis.quality_pass) result.quality_pass = false;
    }

    result.quality_pass = std::all_of(
        verification_targets.begin(), verification_targets.end(),
        [](const auto& target) { return target.value("status", "") == "passed"; });
    result.observations = observation_refs;
    result.verification = {
        {"schema_id", "orange.calibration.projected_surface_scale_verification"},
        {"schema_version", 1},
        {"status", result.quality_pass ? "passed" : "failed"},
        {"capture_group_id", spatial_state.group_capture_id},
        {"ptp_grouped_capture", true},
        {"orientation_markers_automatically_validated", true},
        {"pitch_mm_fit_authority", 5.0},
        {"c_to_xplus_mm_independent_validation", 25.0},
        {"outer_diameter_mm_independent_validation", 77.0},
        {"target_thickness_mm", 3.0},
        {"targets", verification_targets},
    };
    const fs::path set_dir = calibration_session_dir / "artifacts" /
        ("projected_surface_scale_" +
         spatial_layout::sanitize_artifact_component(spatial_state.group_capture_id));
    const std::string artifact_id = set_dir.filename().string();
    result.manifest_path = set_dir / "manifest.json";
    const nlohmann::json manifest = {
        {"schema_id", "orange.calibration.projected_surface_scale_observation_set"},
        {"schema_version", 1},
        {"artifact_id", artifact_id},
        {"artifact_schema_id", "orange.calibration.projected_surface_scale_observation_set"},
        {"artifact_schema_version", 1},
        {"created_utc", get_current_utc_timestamp()},
        {"status", result.quality_pass ? "passed" : "failed_quality_gate"},
        {"capture_group_id", spatial_state.group_capture_id},
        {"citrus_canvas_path", citrus_canvas_path.string()},
        {"citrus_canvas_sha256", canvas_sha256},
        {"observations", result.observations},
        {"verification", result.verification},
        {"ownership", {
            {"detection_correspondence_and_qc", "orange"},
            {"refit_candidate_promotion_and_runtime_activation", "citrus"},
        }},
        {"producer", {
            {"application", "orange"},
            {"component", "projected_surface_scale_artifact"},
        }},
        {"summary", {
            {"purpose", "projected_surface_scale_calibration"},
            {"target_plane", "projected_surface"},
            {"capture_group_id", spatial_state.group_capture_id},
            {"camera_count", result.observations.size()},
            {"quality_status", result.quality_pass ? "passed" : "failed"},
        }},
        {"storage", {
            {"relative_manifest_path", artifact_id + "/manifest.json"},
        }},
    };
    if (!write_json_exclusive(result.manifest_path, manifest, &error)) {
        return fail(error);
    }
    newly_written_paths.push_back(result.manifest_path);
    if (!spatial_layout::update_spatial_calibration_session_index(
            calibration_session_dir.string(),
            (calibration_session_dir / "artifacts").string(),
            manifest,
            &error)) {
        return fail("scale artifacts were written but session index update failed: " + error);
    }
    result.ok = true;
    return result;
}

}  // namespace orange::gui::projected_surface_scale
