// synthetic_recording_bundle: a reproducible, clearly labelled SYNTHETIC
// recording folder produced by Orange's production writers, for cross-repository
// fixtures (Citrus unified-H5 generation, transfer-v2 / Palette intake review).
//
// It never opens a camera, GPU stream, recorder or Citrus socket. It runs the
// same functions a real recording start runs, in the same order:
//   1. write_recording_snapshot            -> recording_snapshot.json
//   2. update_gui_recording_geometry_contract
//        -> recording_geometry_contract.json + recording_geometry_assets/
//           (manifest + exact-byte copies of every referenced observation)
//   3. update_recording_snapshot_session_artifacts (frozen recording_contexts)
//   4. seal_immutable_recording_start_snapshot -> recording_snapshot_start.json
//   5. optional: materialize_recording_observation_binding_requests
//        -> recording_observation_bindings/{request_collection.json,requests/}
//   6. optional: build/write the single-clip recording_session.json with
//      conspicuously labelled placeholder media files.
//
// Synthetic geometry inputs come either from --fixture-rig (a self-contained
// synthetic Citrus rig/canvas/commissioning/daily-registration tree written by
// this tool) or from --canvas (any existing Citrus canvas config). The bundle is
// labelled at every level: synthetic_bundle.json at the root, session.synthetic_bundle
// inside both snapshots, data_origin = synthetic in every recording context, and
// placeholder media that are plain text, never decodable video.
//
// See docs/synthetic_recording_bundle.md.
#include "camera.h"
#include "gui/recording_snapshots.h"
#include "gui/spatial_layout/sha256.h"
#include "json.hpp"
#include "project.h"
#include "recording_context.h"
#include "recording_output_descriptor.h"
#include "session/recording_observation_request_artifacts.h"
#include "session/recording_session.h"
#include "video_capture.h"
#include "NvEncoder/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The linked Orange client sources expect the process-wide logger.
simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kBundleSchemaId = "orange.synthetic_recording_bundle";
constexpr int kBundleSchemaVersion = 1;
constexpr const char* kBundleLabel =
    "SYNTHETIC RECORDING BUNDLE: produced by Orange's production writers from synthetic "
    "inputs, with no camera, recorder or Citrus process. Not an acquired recording. "
    "Never submit to production intake.";

struct Options {
    fs::path out;
    std::string recording_id;
    std::vector<std::string> cameras;
    std::map<std::string, std::string> camera_configs;  // serial -> camera JSON path
    fs::path canvas;                                    // existing Citrus canvas config
    fs::path fixture_rig;                               // write a synthetic rig tree here
    fs::path calibration_base;                          // ORANGE_CALIBRATION_BASE_DIR
    std::string recording_type = "behavior";
    std::string recording_subtype;                      // empty = omitted (context v2)
    std::string behavior_mode = "embedded";
    std::string recording_intent = "stimulus_experiment";
    std::string binding_mode;                           // empty = derived from intent
    int request_version = 0;                            // 0 = leave env alone
    std::string captured_at_utc;                        // empty = now
    bool manifest = false;
    int placeholder_frames = 100;
    bool copy_images = false;
    unsigned width = 4512;
    unsigned height = 4512;
    unsigned frame_rate = 100;
};

[[noreturn]] void usage(const std::string& error = "")
{
    if (!error.empty()) std::cerr << "error: " << error << "\n\n";
    std::cerr <<
        "usage: synthetic_recording_bundle --out <folder> --camera <serial> [--camera ...]\n"
        "         (--fixture-rig <dir> | --canvas <citrus_canvas.json>)\n"
        "         [--recording-id <id>] [--camera-config <serial>=<camera.json>]...\n"
        "         [--calibration-base <dir>] [--captured-at <YYYY-MM-DDTHH:MM:SSZ>]\n"
        "         [--recording-type <label>] [--recording-subtype <label>|omit]\n"
        "         [--behavior-mode free|embedded|none]\n"
        "         [--intent stimulus_experiment|recording_only]\n"
        "         [--binding-mode required|optional|not_applicable]\n"
        "         [--request-version 1|2] [--manifest] [--placeholder-frames N]\n"
        "         [--copy-images] [--raster WxH] [--frame-rate N]\n\n"
        "Writes a clearly labelled synthetic recording folder with Orange's production\n"
        "snapshot, geometry-contract, asset, seal, binding-request and manifest writers.\n"
        "data_origin is always 'synthetic'. --recording-subtype omit (or absent) emits\n"
        "citrus.parent_recording_context version 2 without a subtype.\n";
    std::exit(2);
}

Options parse_args(int argc, char** argv)
{
    Options o;
    auto need = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) usage(std::string(flag) + " needs a value");
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out") o.out = need(i, "--out");
        else if (a == "--recording-id") o.recording_id = need(i, "--recording-id");
        else if (a == "--camera") o.cameras.push_back(need(i, "--camera"));
        else if (a == "--camera-config") {
            const std::string v = need(i, "--camera-config");
            const auto eq = v.find('=');
            if (eq == std::string::npos) usage("--camera-config expects <serial>=<path>");
            o.camera_configs[v.substr(0, eq)] = v.substr(eq + 1);
        }
        else if (a == "--canvas") o.canvas = need(i, "--canvas");
        else if (a == "--fixture-rig") o.fixture_rig = need(i, "--fixture-rig");
        else if (a == "--calibration-base") o.calibration_base = need(i, "--calibration-base");
        else if (a == "--captured-at") o.captured_at_utc = need(i, "--captured-at");
        else if (a == "--recording-type") o.recording_type = need(i, "--recording-type");
        else if (a == "--recording-subtype") { o.recording_subtype = need(i, "--recording-subtype"); if (o.recording_subtype == "omit") o.recording_subtype.clear(); }
        else if (a == "--behavior-mode") o.behavior_mode = need(i, "--behavior-mode");
        else if (a == "--intent") o.recording_intent = need(i, "--intent");
        else if (a == "--binding-mode") o.binding_mode = need(i, "--binding-mode");
        else if (a == "--request-version") o.request_version = std::stoi(need(i, "--request-version"));
        else if (a == "--manifest") o.manifest = true;
        else if (a == "--placeholder-frames") o.placeholder_frames = std::stoi(need(i, "--placeholder-frames"));
        else if (a == "--copy-images") o.copy_images = true;
        else if (a == "--raster") {
            const std::string v = need(i, "--raster");
            const auto x = v.find('x');
            if (x == std::string::npos) usage("--raster expects WxH");
            o.width = static_cast<unsigned>(std::stoul(v.substr(0, x)));
            o.height = static_cast<unsigned>(std::stoul(v.substr(x + 1)));
        }
        else if (a == "--frame-rate") o.frame_rate = static_cast<unsigned>(std::stoul(need(i, "--frame-rate")));
        else if (a == "-h" || a == "--help") usage();
        else usage("unknown argument " + a);
    }
    if (o.out.empty()) usage("--out is required");
    if (o.cameras.empty()) usage("at least one --camera is required");
    if (o.canvas.empty() == o.fixture_rig.empty()) usage("exactly one of --fixture-rig or --canvas is required");
    if (o.request_version != 0 && o.request_version != 1 && o.request_version != 2) usage("--request-version must be 1 or 2");
    if (o.recording_id.empty()) o.recording_id = o.out.filename().string();
    return o;
}

void write_text(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << text;
}

void write_json(const fs::path& path, const json& value)
{
    write_text(path, value.dump(2) + "\n");
}

std::string file_sha256(const fs::path& path)
{
    std::string value;
    std::string error;
    if (!orange::gui::spatial_layout::checksum::file_sha256(path, &value, &error)) {
        throw std::runtime_error("sha256 failed for " + path.string() + ": " + error);
    }
    return value;
}

// A self-contained synthetic Citrus rig tree: one arena per camera, a local
// commissioning release with an accepted homography and projected-surface
// scale per camera, one tank design, and an accepted daily registration with a
// schema-v2 dish top-rim observation per camera. All numbers are synthetic and
// every file carries a synthetic marker. Returns the canvas config path.
fs::path write_synthetic_rig(const fs::path& root, const std::vector<std::string>& cameras,
                             unsigned width, unsigned height)
{
    const std::string rig_id = "synthetic-rig";
    const std::string canvas_name = "synthetic-canvas";
    const fs::path targets = root / "targets";
    const fs::path rig = targets / "rigs" / rig_id;
    const fs::path canvas_dir = rig / canvas_name;
    const fs::path artifacts = canvas_dir / "calibration_artifacts";
    const fs::path canvas_path = canvas_dir / (canvas_name + ".json");
    const std::string tank_design_id = "synthetic-dish";
    const json synthetic_marker = {{"synthetic", true}, {"generator", "orange synthetic_recording_bundle"}};

    write_json(rig / (rig_id + "_config.json"), {
        {"schema_version", 1}, {"rig_id", rig_id}, {"rig_geometry_revision", 1},
        {"synthetic_input", synthetic_marker}});
    write_json(targets / "tank_designs" / (tank_design_id + ".json"), {
        {"schema_id", "citrus.tank_design_spec"}, {"schema_version", 1},
        {"tank_design_id", tank_design_id}, {"shape", "circular"},
        {"dimensions", {{"inner_diameter_mm", 80.0}, {"usable_area_diameter_mm", 80.0},
                        {"outer_diameter_mm", 90.0}, {"wall_thickness_mm", 5.0}}},
        {"synthetic_input", synthetic_marker}});

    json arenas = json::object();
    json members = json::array();
    json registration_targets = json::array();
    const std::string release_id = "synthetic_release";
    const fs::path release_path = artifacts / "commissioning" / release_id / "commissioning.json";
    const fs::path daily_dir = artifacts / "daily_registration" / "dailyregtxn_synthetic";
    const std::string registration_id = "dailyreg_synthetic";
    const std::string transaction_id = "dailyregtxn_synthetic";
    const double cx = width / 2.0, cy = height / 2.0;
    const double inner_r = std::min(width, height) * 0.45, valid_r = inner_r + 17.0;

    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const std::string& serial = cameras[i];
        const std::string arena_id = "arena_" + std::to_string(i + 1);
        const std::string suffix = arena_id + "_" + serial;
        arenas[arena_id] = {
            {"config_name", arena_id},
            {"active_camera_id", serial},
            {"selected_dish_type_name", tank_design_id},
            {"tank_design_id", tank_design_id},
            {"experimental_area_shape", "CIRCLE"},
            {"experimental_area_center_x_px", 480.0 + 320.0 * static_cast<double>(i)},
            {"experimental_area_center_y_px", 540.0},
            {"experimental_area_radius_mm", 40.0},
            {"camera_calibrations", json::array({{
                {"camera_id", serial},
                {"native_width_px", width}, {"native_height_px", height},
                {"arena_center_x_px", cx}, {"arena_center_y_px", cy},
                {"arena_width_px", 2.0 * inner_r}, {"arena_height_px", 2.0 * inner_r}}})}};

        // Homography: candidate + yaml + accepted active pointer.
        const std::string homography_candidate_id = "homography_" + suffix + "_synthetic";
        const fs::path h_dir = artifacts / "homography_candidates" / "synthetic" / suffix;
        const fs::path h_candidate = h_dir / "candidate.json";
        const fs::path h_yaml = h_dir / "homography.yml";
        const json matrix = {{0.08, 0.0, 480.0 + 320.0 * static_cast<double>(i) - 0.08 * cx},
                             {0.0, -0.08, 540.0 + 0.08 * cy}, {0.0, 0.0, 1.0}};
        write_json(h_candidate, {
            {"schema_id", "citrus.calibration.homography_candidate"}, {"schema_version", 1},
            {"candidate_id", homography_candidate_id}, {"arena_id", arena_id}, {"camera_id", serial},
            {"target_plane", "projected_surface"}, {"homography_matrix", matrix},
            {"synthetic_input", synthetic_marker}});
        write_text(h_yaml, "%YAML:1.0\n---\n# synthetic homography (orange synthetic_recording_bundle)\n"
                            "homography: !!opencv-matrix\n   rows: 3\n   cols: 3\n   dt: d\n"
                            "   data: [ 0.08, 0., " + std::to_string(480.0 + 320.0 * static_cast<double>(i) - 0.08 * cx) +
                            ", 0., -0.08, " + std::to_string(540.0 + 0.08 * cy) + ", 0., 0., 1. ]\n");
        const fs::path h_pointer = artifacts / ("homography_active_" + suffix + ".json");
        write_json(h_pointer, {
            {"schema_id", "citrus.calibration.active_homography"}, {"schema_version", 1},
            {"status", "accepted"}, {"rig_id", rig_id}, {"canvas_name", canvas_name},
            {"arena_id", arena_id}, {"camera_id", serial},
            {"candidate_id", homography_candidate_id},
            {"candidate_json_path", h_candidate.string()},
            {"candidate_json_checksum", file_sha256(h_candidate)},
            {"homography_yaml_path", h_yaml.string()},
            {"homography_yaml_checksum", file_sha256(h_yaml)},
            {"target_plane", "projected_surface"},
            {"homography_direction", "camera_native_px_to_final_display_canvas_px"},
            {"homography_matrix", matrix},
            {"synthetic_input", synthetic_marker}});

        // Projected-surface scale: observation + candidate + accepted active pointer.
        const fs::path s_dir = artifacts / "scale_candidates" / "synthetic" / suffix;
        const fs::path s_observation = s_dir / "observation.json";
        const fs::path s_candidate = s_dir / "candidate.json";
        const std::string scale_candidate_id = "projected_surface_scale_" + suffix + "_synthetic";
        write_json(s_observation, {
            {"schema_id", "orange.calibration.projected_surface_scale_observation"}, {"schema_version", 1},
            {"artifact_id", scale_candidate_id + "_observation"}, {"camera", {{"serial", serial}}},
            {"target_plane", "projected_surface"},
            {"clicked_points_camera_px", json::array({{{"x", 100.0}, {"y", 100.0}}, {{"x", 627.5}, {"y", 100.0}}})},
            {"real_world_ref_mm", 10.0}, {"synthetic_input", synthetic_marker}});
        write_json(s_candidate, {
            {"schema_id", "citrus.calibration.projected_surface_scale_candidate"}, {"schema_version", 1},
            {"candidate_id", scale_candidate_id}, {"arena_id", arena_id}, {"camera_id", serial},
            {"target_plane", "projected_surface"},
            {"scale", {{"camera_pixels_per_mm", 52.75}, {"canvas_pixels_per_mm", 4.22}}},
            {"synthetic_input", synthetic_marker}});
        const fs::path s_pointer = artifacts / ("scale_active_" + suffix + "_projected_surface.json");
        write_json(s_pointer, {
            {"schema_id", "citrus.calibration.active_projected_surface_scale"}, {"schema_version", 1},
            {"status", "accepted"}, {"rig_id", rig_id}, {"canvas_name", canvas_name},
            {"arena_id", arena_id}, {"camera_id", serial},
            {"candidate_id", scale_candidate_id},
            {"candidate_json_path", s_candidate.string()},
            {"candidate_json_checksum", file_sha256(s_candidate)},
            {"target_plane", "projected_surface"},
            {"direction", "physical_target_mm_to_final_display_canvas_px"},
            {"active_homography", {{"candidate_id", homography_candidate_id},
                                   {"active_pointer_path", h_pointer.string()}}},
            {"scale", {{"camera_pixels_per_mm", 52.75}, {"canvas_pixels_per_mm", 4.22},
                       {"canvas_pixels_per_mm_x", 4.21}, {"canvas_pixels_per_mm_y", 4.23}}},
            {"source_observation", {{"path", s_observation.string()}, {"sha256", file_sha256(s_observation)}}},
            {"synthetic_input", synthetic_marker}});

        members.push_back({
            {"arena_id", arena_id}, {"camera_id", serial}, {"target_plane", "projected_surface"},
            {"homography", {{"active_pointer_path", h_pointer.string()}, {"active_pointer_sha256", file_sha256(h_pointer)}}},
            {"projected_surface_scale", {{"active_pointer_path", s_pointer.string()}, {"active_pointer_sha256", file_sha256(s_pointer)}}},
            {"requirements", {{"acceptance_receipts_valid", true}, {"active_homography_compatible", true},
                              {"active_scale_compatible", true}, {"scale_bound_to_active_homography", true}}}});

        // Daily dish top-rim observation (Orange schema v2) plus its compact exports.
        const std::string observation_id = "dishrim_synthetic_" + serial;
        const fs::path obs_dir = root / "orange_data" / "calibrations" / "sessions" / "synthetic" / "artifacts" /
            ("Cam" + serial + "_" + arena_id) / "top_rim_observations" / observation_id;
        const json center = {{"x", cx}, {"y", cy}};
        const json inner_geometry = {{"type", "circle"}, {"center_px", center}, {"radius_px", inner_r}};
        const json valid_geometry = {{"type", "circle"}, {"center_px", center}, {"radius_px", valid_r}};
        const fs::path observation_path = obs_dir / "observation.json";
        write_json(observation_path, {
            {"schema_id", "orange.calibration.dish_top_rim_observation"}, {"schema_version", 2},
            {"artifact_id", observation_id}, {"physical_target", "dish_top_rim"},
            {"camera", {{"serial", serial}, {"name", "Cam" + serial}, {"width", width}, {"height", height}, {"pixel_format", "Mono8"}}},
            {"arena_context", {{"rig_id", rig_id}, {"canvas_id", canvas_name}, {"arena_id", arena_id}, {"camera_serial", serial}}},
            {"accepted_inner_rim_boundary", {{"coordinate_space", "camera_native_pixels"}, {"target_plane", "dish_top_rim"}, {"geometry", inner_geometry}}},
            {"accepted_experimental_area_boundary", {{"coordinate_space", "camera_native_pixels"}, {"target_plane", "dish_top_rim"}, {"geometry", inner_geometry}}},
            {"accepted_mask", {{"shape", "circle"}, {"coordinate_space", "camera_native_pixels"}, {"center_px", center}, {"radius_px", valid_r}}},
            {"valid_detection_region", {{"coordinate_space", "camera_native_pixels"}, {"purpose", "bounding_box_centroid_detection_gating"},
                                        {"offset_direction", "outward"}, {"geometry", valid_geometry}}},
            {"operator_review", {{"accepted", true}}},
            {"boundary_interpretation", {{"target_plane", "dish_top_rim"},
                                         {"valid_detection_region_policy", "derived_by_outward_offset_for_bounding_box_centroid_forgiveness"}}},
            {"synthetic_input", synthetic_marker}});
        write_json(obs_dir / "image_set.json", {
            {"schema_id", "orange.calibration.image_set"}, {"schema_version", 1},
            {"artifact_id", observation_id}, {"purpose", "dish_top_rim"},
            {"coordinate_space", "camera_native_pixels"}, {"target_plane", "dish_top_rim"},
            {"camera", {{"serial", serial}, {"image_shape", {{"width", width}, {"height", height}}}}},
            {"synthetic_input", synthetic_marker}});
        write_json(obs_dir / "exports" / "spatial_dish_mask_runtime_v1.json", {
            {"schema_version", 1}, {"enabled", true},
            {"geometry", {{"coordinate_space", "camera_native_pixels"},
                          {"outer_geometry", {{"type", "circle"}, {"cx", cx}, {"cy", cy}, {"r", inner_r}}},
                          {"valid_geometry", {{"type", "circle"}, {"cx", cx}, {"cy", cy}, {"r", valid_r}}}}},
            {"source_observation", {{"artifact_id", observation_id},
                                    {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
                                    {"artifact_schema_version", 2}}}});
        write_json(obs_dir / "exports" / "palette_dish_mask_v2.json", {
            {"version", "2.0"}, {"shape", "circle"}, {"orange_artifact_id", observation_id},
            {"orange_artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
            {"orange_artifact_schema_version", 2},
            {"detected_circle", {{"center", {cx, cy}}, {"radius", valid_r}}}});
        write_json(obs_dir / "manifest.json", {
            {"schema_id", "orange.calibration.manifest"}, {"schema_version", 1},
            {"artifact_id", observation_id},
            {"files", {{"image_set_json", "image_set.json"},
                       {"spatial_dish_mask_runtime_v1", "exports/spatial_dish_mask_runtime_v1.json"},
                       {"palette_dish_mask_v2", "exports/palette_dish_mask_v2.json"}}},
            {"checksums", {{"algorithm", "fnv1a64"}}}});

        registration_targets.push_back({
            {"arena_id", arena_id}, {"camera_id", serial}, {"target_plane", "projected_surface"},
            {"homography", {{"authority_canvas_name", canvas_name}, {"authority_mode", "local_canvas"},
                            {"selected_canvas_name", canvas_name}, {"candidate_id", homography_candidate_id},
                            {"candidate_path", h_candidate.string()}, {"candidate_sha256", file_sha256(h_candidate)}}},
            {"invariants", {{"arena_size_unchanged", true}, {"canvas_geometry_unchanged", true},
                            {"experimental_area_local_geometry_unchanged", true},
                            {"homography_unchanged", true}, {"scale_unchanged", true}}},
            {"rim_center_camera_px", center}, {"observed_rim_radius_camera_px", inner_r},
            {"rim_observation", {{"artifact_id", observation_id}, {"path", observation_path.string()},
                                 {"sha256", file_sha256(observation_path)}}}});
    }

    write_json(canvas_path, {
        {"canvas_name", canvas_name}, {"canvas_width_px", 1920}, {"canvas_height_px", 1080},
        {"arenas", arenas}, {"synthetic_input", synthetic_marker}});
    write_json(release_path, {
        {"schema_id", "citrus.calibration.rig_canvas_commissioning_release"}, {"schema_version", 1},
        {"status", "accepted"}, {"release_id", release_id}, {"rig_id", rig_id},
        {"canvas_name", canvas_name}, {"rig_geometry_revision", 1}, {"members", members},
        {"synthetic_input", synthetic_marker}});
    write_json(artifacts / "commissioning_active.json", {
        {"schema_id", "citrus.calibration.active_rig_canvas_commissioning"}, {"schema_version", 1},
        {"status", "accepted"}, {"release_id", release_id}, {"rig_id", rig_id}, {"canvas_name", canvas_name},
        {"manifest_path", release_path.string()}, {"manifest_sha256", file_sha256(release_path)},
        {"synthetic_input", synthetic_marker}});

    const json commissioning_base = {{"release_id", release_id}, {"manifest_path", release_path.string()},
                                     {"manifest_sha256", file_sha256(release_path)}};
    const fs::path candidate_path = daily_dir / "candidate.json";
    write_json(candidate_path, {
        {"schema_id", "citrus.calibration.daily_registration_candidate"}, {"schema_version", 1},
        {"status", "candidate"}, {"candidate_id", registration_id}, {"transaction_id", transaction_id},
        {"rig_id", rig_id}, {"canvas_name", canvas_name}, {"synthetic_input", synthetic_marker}});
    const fs::path registration_path = daily_dir / "registration.json";
    write_json(registration_path, {
        {"schema_id", "citrus.calibration.daily_registration"}, {"schema_version", 1},
        {"status", "accepted"}, {"rig_id", rig_id}, {"canvas_name", canvas_name},
        {"transaction_id", transaction_id}, {"registration_id", registration_id},
        {"valid_until_utc", "2099-12-31T23:59:59Z"},
        {"candidate_path", candidate_path.string()}, {"candidate_sha256", file_sha256(candidate_path)},
        {"commissioning_base", commissioning_base}, {"targets", registration_targets},
        {"synthetic_input", synthetic_marker}});
    write_json(artifacts / "daily_registration_runtime_selection.json", {
        {"schema_id", "citrus.calibration.daily_registration_runtime_selection"}, {"schema_version", 1},
        {"status", "accepted"}, {"rig_id", rig_id}, {"canvas_name", canvas_name},
        {"mode", "selected_daily_registration"}, {"commissioning_base", commissioning_base},
        {"registration", {{"registration_id", registration_id}, {"valid_until_utc", "2099-12-31T23:59:59Z"},
                          {"path", registration_path.string()}, {"sha256", file_sha256(registration_path)}}},
        {"synthetic_input", synthetic_marker}});
    return canvas_path;
}

// A minimal, clearly synthetic camera configuration for the snapshot's
// cameras[serial] block when no real camera JSON is supplied.
fs::path write_synthetic_camera_config(const fs::path& dir, const std::string& serial,
                                       unsigned width, unsigned height, unsigned frame_rate)
{
    const fs::path path = dir / (serial + ".json");
    write_json(path, {
        {"synthetic_input", {{"synthetic", true}, {"generator", "orange synthetic_recording_bundle"},
                             {"note", "no camera was opened; values describe the synthetic source raster"}}},
        {"name", "Cam" + serial}, {"device_serial_number", serial},
        {"device_model_name", "SYNTHETIC"}, {"camera_scan_type", "Areascan"},
        {"width", width}, {"height", height}, {"offset_x", 0}, {"offset_y", 0},
        {"frame_rate", frame_rate}, {"pixel_format", "Mono8"}, {"color", false},
        {"exposure", 100}, {"gain", 0}, {"sync_mode", "ptp_gate"}});
    return path;
}

json sha_reference(const fs::path& root, const fs::path& relative)
{
    const fs::path path = root / relative;
    return {{"relative_path", relative.generic_string()}, {"sha256", file_sha256(path)},
            {"byte_size", static_cast<std::uint64_t>(fs::file_size(path))}};
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options o = parse_args(argc, argv);
        if (fs::exists(o.out) && !fs::is_empty(o.out)) {
            throw std::runtime_error("output folder exists and is not empty: " + o.out.string());
        }
        fs::create_directories(o.out);
        const fs::path inputs_dir = o.out / "synthetic_inputs";
        fs::create_directories(inputs_dir);

        // Isolate every host-state read the writers would otherwise do.
        const fs::path calibration_base = o.calibration_base.empty()
            ? inputs_dir / "calibrations" : o.calibration_base;
        fs::create_directories(calibration_base);
        setenv("ORANGE_CALIBRATION_BASE_DIR", calibration_base.c_str(), 1);
        for (const auto& serial : o.cameras) {
            unsetenv(("ORANGE_SPATIAL_CALIBRATION_ARTIFACT_" + serial).c_str());
        }
        unsetenv("ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH");
        unsetenv("ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH");
        if (o.copy_images) setenv("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES", "1", 1);
        else unsetenv("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES");
        if (o.request_version != 0) {
            setenv("ORANGE_CITRUS_BINDING_REQUEST_VERSION", std::to_string(o.request_version).c_str(), 1);
        }

        // Geometry inputs.
        fs::path canvas = o.canvas;
        if (!o.fixture_rig.empty()) {
            canvas = write_synthetic_rig(fs::absolute(o.fixture_rig), o.cameras, o.width, o.height);
        }
        canvas = fs::absolute(canvas);
        if (!fs::is_regular_file(canvas)) throw std::runtime_error("canvas config not found: " + canvas.string());
        setenv("ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH", canvas.c_str(), 1);

        // Camera parameters and selection (record-selected so each camera is a
        // canonical source stream and an observation edge).
        std::vector<CameraParams> params(o.cameras.size());
        std::vector<CameraEachSelect> select(o.cameras.size());
        json camera_inputs = json::object();
        for (std::size_t i = 0; i < o.cameras.size(); ++i) {
            const std::string& serial = o.cameras[i];
            CameraParams& p = params[i];
            p.camera_id = static_cast<int>(i);
            p.camera_serial = serial;
            p.width = o.width;
            p.height = o.height;
            p.frame_rate = o.frame_rate;
            p.pixel_format = "Mono8";
            p.gpu_id = 0;
            const auto it = o.camera_configs.find(serial);
            const fs::path config = it != o.camera_configs.end()
                ? fs::absolute(it->second)
                : write_synthetic_camera_config(inputs_dir / "camera_configs", serial, o.width, o.height, o.frame_rate);
            p.config_path = config.string();
            select[i].stream_on = true;
            select[i].record = true;
            camera_inputs[serial] = {{"config_path", config.string()}, {"config_sha256", file_sha256(config)},
                                     {"synthetic_config", it == o.camera_configs.end()}};
        }

        // 1. Mutable snapshot (production writer).
        if (!write_recording_snapshot(o.out.string(), o.recording_id, params.data(),
                                      static_cast<int>(params.size()), o.out.parent_path().string(),
                                      /*update_latest_pointer=*/false, /*sync_camera_enabled=*/false,
                                      nullptr, "external_ipc", nullptr, 0, select.data())) {
            throw std::runtime_error("write_recording_snapshot failed");
        }

        // 2. Geometry contract + recording-local asset bundle (production writer).
        update_gui_recording_geometry_contract(o.out.string(), canvas.string(), params.data(),
                                               select.data(), static_cast<int>(params.size()));
        if (!fs::is_regular_file(o.out / "recording_geometry_contract.json")) {
            throw std::runtime_error("recording_geometry_contract.json was not written");
        }

        // 3. Frozen recording contexts (always synthetic) and the bundle label
        //    inside the snapshot, before sealing.
        orange::recording::RecordingContext context;
        context.recording_type = o.recording_type;
        if (!o.recording_subtype.empty()) context.recording_subtype = o.recording_subtype;
        context.behavior_mode = o.behavior_mode;
        context.recording_intent = o.recording_intent;
        context.data_origin = "synthetic";
        (void)orange::recording::RecordingContext::Parse(context.ToJson());  // same rules as the app config
        std::map<std::string, orange::recording::RecordingContext> contexts;
        for (const auto& serial : o.cameras) contexts.emplace(serial, context);
        const json emitted_contexts = orange::recording::EmittedRecordingContextsJson(contexts);
        const json label = {
            {"schema_id", kBundleSchemaId}, {"schema_version", kBundleSchemaVersion},
            {"synthetic", true}, {"data_origin", "synthetic"}, {"label", kBundleLabel},
            {"generator", "synthetic_recording_bundle"},
            {"geometry_source", o.fixture_rig.empty() ? "existing_canvas" : "synthetic_fixture_rig"},
            {"canvas_config_path", canvas.string()}};
        if (!update_recording_snapshot_session_artifacts(
                o.out.string(), {{"recording_contexts", emitted_contexts}, {"synthetic_bundle", label}})) {
            throw std::runtime_error("update_recording_snapshot_session_artifacts failed");
        }

        // 4. Seal the immutable start snapshot (create-once, read-only).
        json start_reference;
        std::string error;
        if (!seal_immutable_recording_start_snapshot(o.out.string(), &start_reference, &error)) {
            throw std::runtime_error("seal_immutable_recording_start_snapshot failed: " + error);
        }

        // 5. Observation binding requests (Orange's sealed binding inputs for Citrus).
        std::string binding_mode = o.binding_mode.empty() ? "optional" : o.binding_mode;
        binding_mode = orange::recording::ApplyRecordingIntentToBindingMode(o.out.string(), binding_mode, &error);
        if (binding_mode.empty()) throw std::runtime_error("binding mode: " + error);
        json requests_summary = {{"binding_mode", binding_mode}, {"status", "not_applicable"}};
        if (binding_mode != "not_applicable") {
            orange::session::RecordingObservationBindingRequestMaterialization requests;
            const std::string requested_at = o.captured_at_utc.empty() ? get_current_utc_timestamp() : o.captured_at_utc;
            if (!orange::session::materialize_recording_observation_binding_requests(
                    o.out.string(), binding_mode, requested_at, &requests, &error)) {
                throw std::runtime_error("materialize_recording_observation_binding_requests failed: " + error);
            }
            requests_summary = {{"binding_mode", binding_mode}, {"status", requests.status}, {"reason", requests.reason},
                                {"collection_relative_path", requests.collection_relative_path},
                                {"collection_sha256", requests.collection_sha256},
                                {"request_count", requests.artifacts.size()}};
            json request_rows = json::array();
            for (const auto& artifact : requests.artifacts) {
                request_rows.push_back({{"observation_context_id", artifact.observation_context_id},
                                        {"relative_path", artifact.relative_path},
                                        {"schema_version", artifact.request.value("schema_version", 0)}});
            }
            requests_summary["requests"] = request_rows;
            if (requests.status == "materialized") {
                const json collection = json::parse(std::ifstream(o.out / requests.collection_relative_path));
                if (!update_recording_snapshot_observation_binding_requests(o.out.string(), collection, &error)) {
                    throw std::runtime_error("update_recording_snapshot_observation_binding_requests failed: " + error);
                }
            }
        }

        // 6. Optional single-clip manifest with labelled placeholder media.
        json manifest_summary = {{"written", false}};
        if (o.manifest) {
            orange::session::SingleClipRecordingSessionManifestOptions m;
            m.producer = "orange_synthetic_recording_bundle";
            m.session_id = o.recording_id;
            m.recording_folder = o.out.string();
            m.status = "completed";
            m.created_at_utc = m.updated_at_utc = get_current_utc_timestamp();
            m.recording_started = true;
            m.recording_stop_requested = true;
            m.recording_stop_reason = "synthetic_bundle_placeholder";
            m.recording_drain_completed = true;
            m.actual_recording_duration_s = static_cast<double>(o.placeholder_frames) / o.frame_rate;
            m.recording_backend = {{"mode", "synthetic_placeholder"}, {"synthetic", true}};
            for (const auto& serial : o.cameras) {
                orange::session::RecordingSessionCameraArtifact artifact;
                artifact.camera_serial = serial;
                artifact.video_path = "Cam" + serial + ".mp4";
                artifact.metadata_path = "Cam" + serial + "_meta.csv";
                artifact.keyframe_path = "Cam" + serial + "_keyframe.json";
                artifact.frame_count = static_cast<std::uint64_t>(o.placeholder_frames);
                artifact.first_recording_frame_id = 1;
                artifact.last_recording_frame_id = static_cast<std::uint64_t>(o.placeholder_frames);
                artifact.packet_count = static_cast<std::uint64_t>(o.placeholder_frames);
                artifact.packet_count_source = "synthetic_placeholder";
                m.cameras.push_back(artifact);
                const std::string note = "SYNTHETIC PLACEHOLDER (orange synthetic_recording_bundle): not media, not decodable, "
                                         "not an acquired frame stream. Camera " + serial + ".\n";
                write_text(o.out / artifact.video_path, note);
                std::ostringstream csv;
                csv << "recording_frame_id,camera_frame_id,synthetic_placeholder\n";
                for (int f = 1; f <= o.placeholder_frames; ++f) csv << f << "," << f << ",1\n";
                write_text(o.out / artifact.metadata_path, csv.str());
                write_json(o.out / artifact.keyframe_path, {{"synthetic_placeholder", true}, {"camera_serial", serial},
                                                            {"keyframes", json::array()}});
            }
            const json manifest = orange::session::build_single_clip_recording_session_manifest(m);
            json written;
            if (!orange::session::write_recording_session_manifest(
                    (o.out / "recording_session.json").string(), manifest, &error, &written)) {
                throw std::runtime_error("write_recording_session_manifest failed: " + error);
            }
            manifest_summary = {{"written", true}, {"placeholder_media", true},
                                {"recording_contexts_present", written.contains("recording_contexts")},
                                {"geometry_reference_present", written.value("metadata", json::object()).contains("recording_geometry_contract")}};
        }

        // Root label: what this is, what produced it, and exact checksums of the
        // artifacts Orange's writers produced.
        const json contract = json::parse(std::ifstream(o.out / "recording_geometry_contract.json"));
        const json start = json::parse(std::ifstream(o.out / "recording_snapshot_start.json"));
        json bundle = {
            {"schema_id", kBundleSchemaId}, {"schema_version", kBundleSchemaVersion},
            {"synthetic", true}, {"data_origin", "synthetic"}, {"label", kBundleLabel},
            {"generator", {{"tool", "synthetic_recording_bundle"},
                           {"orange_source_version", start.value("source_version", json::object())},
                           {"producer_version", start.value("producer_version", "")}}},
            {"created_at_utc", get_current_utc_timestamp()},
            {"recording_id", o.recording_id},
            {"cameras", o.cameras},
            {"raster", {{"width", o.width}, {"height", o.height}, {"frame_rate", o.frame_rate}}},
            {"inputs", {{"geometry_source", o.fixture_rig.empty() ? "existing_canvas" : "synthetic_fixture_rig"},
                        {"canvas_config_path", canvas.string()}, {"canvas_config_sha256", file_sha256(canvas)},
                        {"fixture_rig_root", o.fixture_rig.empty() ? "" : fs::absolute(o.fixture_rig).string()},
                        {"calibration_base_dir", calibration_base.string()},
                        {"camera_configs", camera_inputs}}},
            {"recording_contexts", emitted_contexts},
            {"artifacts", {
                {"recording_snapshot", sha_reference(o.out, "recording_snapshot.json")},
                {"recording_snapshot_start", start_reference},
                {"recording_geometry_contract", sha_reference(o.out, "recording_geometry_contract.json")},
                {"recording_geometry_assets_manifest", sha_reference(o.out, "recording_geometry_assets/manifest.json")}}},
            {"geometry_contract_status", contract.value("status", "")},
            {"geometry_assets_status", contract.value("materialized_assets", json::object()).value("status", "")},
            {"geometry_assets_file_count", contract.value("materialized_assets", json::object()).value("file_count", 0)},
            {"observation_binding_requests", requests_summary},
            {"recording_session_manifest", manifest_summary},
            {"snapshot_distinction", "recording_snapshot_start.json is the create-once read-only seal of "
                                     "recording_snapshot.json taken after the geometry contract and the frozen "
                                     "recording contexts were written; recording_snapshot.json keeps being enriched "
                                     "(binding requests, later session artifacts) and references the sealed file."}};
        write_json(o.out / "synthetic_bundle.json", bundle);
        write_text(o.out / "SYNTHETIC_BUNDLE_README.txt",
                   std::string(kBundleLabel) + "\n\nSee synthetic_bundle.json for the generator, inputs and checksums.\n");
        std::cout << bundle.dump(2) << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "synthetic_recording_bundle: " << ex.what() << std::endl;
        return 1;
    }
}
