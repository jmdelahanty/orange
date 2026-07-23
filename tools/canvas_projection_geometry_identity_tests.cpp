#include "gui/spatial_layout/canvas_projection_geometry_identity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
namespace compatibility =
    orange::gui::spatial_layout::canvas_compatibility;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void write_exact(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to write fixture " + path.string());
}

void write_json(const fs::path& path, const nlohmann::json& value)
{
    write_exact(path, value.dump(2) + "\n");
}

nlohmann::json canvas_fixture()
{
    return {
        {"canvas_name", "canvas1"},
        {"canvas_width_px", 1920},
        {"canvas_height_px", 1080},
        {"arenas", {
            {"arena_1", {
                {"config_name", "arena_1"},
                {"arena_region_width_mm", 80.0},
                {"arena_region_height_mm", 80.0},
                {"experimental_area_radius_mm", 40.0},
                {"experimental_area_radius_px", 160.0},
                {"calibration_pattern_mode", "circular_rings"},
                {"calibration_ring_count", 4},
                {"camera_calibrations", nlohmann::json::array({
                    {
                        {"camera_id", "cam1"},
                        {"native_width_px", 100},
                        {"native_height_px", 80},
                        {"arena_center_x_px", 50},
                        {"arena_center_y_px", 40},
                        {"arena_width_px", 80},
                        {"arena_height_px", 80},
                        {"pixels_per_mm_camera", 2.0},
                        {"pixels_per_mm_projector", 4.0},
                        {"real_world_ref_mm", 10.0},
                        {"scale_image_path", "scale.png"},
                        {"scale_models", nlohmann::json::array({
                            {{"target_plane", "projected_surface"},
                             {"pixels_per_mm_camera", 2.0}},
                            {{"target_plane", "fish_observation"},
                             {"id", "keep"}},
                        })},
                    },
                })},
            }},
        }},
    };
}

void test_cache_only_and_geometry_changes()
{
    const auto accepted = canvas_fixture();
    std::string accepted_fingerprint;
    std::string fingerprint_error;
    require(
        compatibility::projection_geometry_fingerprint(
            accepted.dump(2), &accepted_fingerprint, &fingerprint_error),
        fingerprint_error);
    require(
        accepted_fingerprint ==
            "sha256:01932ac4c8edd3b539c5b9a46b2a341761dc2149f83fa49129b0fa2af9da0857",
        "cross-language projection-geometry fingerprint vector changed: " +
            accepted_fingerprint);
    auto current = accepted;
    auto& arena = current["arenas"]["arena_1"];
    arena["calibration_pattern_mode"] = "rectangular_grid";
    arena["calibration_ring_count"] = 8;
    arena["experimental_area_radius_px"] = 167.1;
    auto& camera = arena["camera_calibrations"][0];
    camera["pixels_per_mm_camera"] = 53.1;
    camera["pixels_per_mm_projector"] = 4.3;
    camera["scale_models"][0]["pixels_per_mm_camera"] = 53.1;

    auto result = compatibility::compare_canvas_bytes(
        current.dump(2), accepted.dump(2));
    require(result.compatible, "cache-only changes should be compatible");
    require(!result.exact_file_checksum_match, "fixture should differ by bytes");
    require(result.projection_geometry_match, "geometry identity should match");
    require(result.basis == "projection_geometry_identity_v1", "wrong basis");
    require(
        result.warning == "canvas_non_geometry_calibration_state_only_change",
        "semantic relaxation must be explicit");

    current["arenas"]["arena_1"]["camera_calibrations"][0]
           ["arena_center_x_px"] = 260;
    result = compatibility::compare_canvas_bytes(current.dump(2), accepted.dump(2));
    require(!result.compatible, "arena-center change must fail closed");
    require(result.error == "canvas_projection_geometry_changed", "wrong failure");
}

void test_release_anchored_relaxation()
{
    const fs::path root = fs::temp_directory_path() /
        ("orange_canvas_compatibility_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    try {
        const fs::path current_canvas_path = root / "shadow.json";
        const fs::path accepted_canvas_path = root / "accepted.json";
        const fs::path artifact_pointer_path = root / "homography_active.json";
        const fs::path release_path = root / "release" / "commissioning.json";
        const fs::path commissioning_pointer_path = root / "commissioning_active.json";

        const auto accepted = canvas_fixture();
        auto current = accepted;
        current["arenas"]["arena_1"]["calibration_pattern_mode"] =
            "rectangular_grid";
        current["arenas"]["arena_1"]["camera_calibrations"][0]
               ["pixels_per_mm_camera"] = 53.0;
        write_json(accepted_canvas_path, accepted);
        write_json(current_canvas_path, current);
        write_json(artifact_pointer_path, {
            {"schema_id", "citrus.calibration.active_homography"},
            {"schema_version", 1},
            {"status", "accepted"},
        });
        std::string accepted_bytes;
        std::string artifact_bytes;
        std::string error;
        require(compatibility::read_file(accepted_canvas_path, &accepted_bytes, &error), error);
        require(compatibility::read_file(artifact_pointer_path, &artifact_bytes, &error), error);
        const std::string accepted_sha = compatibility::sha256(accepted_bytes);
        write_json(release_path, {
            {"schema_id", "citrus.calibration.rig_canvas_commissioning_release"},
            {"schema_version", 1},
            {"status", "accepted"},
            {"release_id", "release1"},
            {"rig_id", "rig1"},
            {"canvas_name", "shadow"},
            {"canvas_configuration", {
                {"snapshot_path", accepted_canvas_path.string()},
                {"snapshot_sha256", accepted_sha},
            }},
            {"members", nlohmann::json::array({
                {
                    {"arena_id", "arena_1"},
                    {"camera_id", "2010093"},
                    {"homography", {
                        {"active_pointer_path", artifact_pointer_path.string()},
                        {"active_pointer_sha256", compatibility::sha256(artifact_bytes)},
                    }},
                },
            })},
        });
        std::string release_bytes;
        require(compatibility::read_file(release_path, &release_bytes, &error), error);
        write_json(commissioning_pointer_path, {
            {"schema_id", "citrus.calibration.active_rig_canvas_commissioning"},
            {"schema_version", 1},
            {"status", "accepted"},
            {"release_id", "release1"},
            {"rig_id", "rig1"},
            {"canvas_name", "shadow"},
            {"manifest_path", release_path.string()},
            {"manifest_sha256", compatibility::sha256(release_bytes)},
        });

        auto result = compatibility::validate_active_artifact_canvas(
            "rig1", "shadow", "arena_1", "2010093", "homography",
            current_canvas_path, accepted_sha, artifact_pointer_path,
            commissioning_pointer_path);
        require(result.compatible, "release-anchored cache change should pass");
        require(result.commissioning_release_id == "release1", "release not recorded");
        require(result.basis == "projection_geometry_identity_v1", "wrong basis");

        write_exact(artifact_pointer_path, "tampered\n");
        result = compatibility::validate_active_artifact_canvas(
            "rig1", "shadow", "arena_1", "2010093", "homography",
            current_canvas_path, accepted_sha, artifact_pointer_path,
            commissioning_pointer_path);
        require(!result.compatible, "tampered active pointer was accepted");
        require(
            result.error == "commissioning_artifact_pointer_checksum_mismatch",
            "wrong tampered-pointer failure");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

}  // namespace

int main()
{
    try {
        test_cache_only_and_geometry_changes();
        test_release_anchored_relaxation();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    std::cout << "[PASS] canvas projection geometry identity tests\n";
    return 0;
}
