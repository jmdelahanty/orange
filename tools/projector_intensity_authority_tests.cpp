#include "gui/spatial_layout/projector_intensity_authority.h"
#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "could not open test fixture");
    output << value.dump(2) << '\n';
    require(output.good(), "could not write test fixture");
}

std::string file_sha256(const std::filesystem::path& path)
{
    std::string value;
    std::string error;
    require(
        orange::gui::spatial_layout::checksum::file_sha256(
            path, &value, &error),
        error);
    return value;
}

nlohmann::json passing_report()
{
    return {
        {"schema_id", "orange.projector_intensity_commissioning.report"},
        {"schema_version", 1},
        {"status", "pass"},
        {"recommended_foreground_gray_u8", 72},
        {"level_passes_all_cameras", {{"72", true}, {"76", false}}},
        {"camera_level_summaries", nlohmann::json::array({
            {{"foreground_gray_u8", 72},
             {"camera_serial", "2010093"},
             {"passes_quality_gate", true}},
            {{"foreground_gray_u8", 72},
             {"camera_serial", "2010094"},
             {"passes_quality_gate", true}},
        })},
    };
}

nlohmann::json reference_pointer(const std::string& arena,
                                 const std::string& camera,
                                 const std::filesystem::path& report,
                                 const std::string& report_sha256)
{
    return {
        {"schema_id", "citrus.calibration.active_homography"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"rig_id", "rig0"},
        {"canvas_name", "shadow"},
        {"arena_id", arena},
        {"camera_id", camera},
        {"target_plane", "projected_surface"},
        {"homography_role", "commissioning_reference"},
        {"source_photometry", {
            {"status", "passed"},
            {"commissioning_provenance", {
                {"recommended_foreground_gray_u8", 72},
                {"report_path", report.string()},
                {"report_sha256", report_sha256},
            }},
        }},
    };
}

}  // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "orange_projector_intensity_authority_tests";
    std::filesystem::remove_all(root);
    const std::filesystem::path report = root / "commissioning_report.json";
    write_json(report, passing_report());
    const std::string report_sha = file_sha256(report);
    const std::string report_sha_raw = report_sha.substr(
        std::string("sha256:").size());

    using orange::gui::spatial_layout::ProjectorIntensityCameraAuthorityRef;
    using orange::gui::spatial_layout::resolve_projector_intensity_authority;
    std::vector<ProjectorIntensityCameraAuthorityRef> direct = {
        {"2010093", "arena_1", "rig0", "shadow", "", report.string(),
         report_sha_raw},
        {"2010094", "arena_2", "rig0", "shadow", "", report.string(),
         report_sha},
    };
    auto result = resolve_projector_intensity_authority(direct);
    require(result.ok, result.error);
    require(result.foreground_gray_u8 == 72, "wrong commissioned foreground");
    require(
        result.provenance.value("status", std::string()) == "validated",
        "authority was not marked validated");

    const std::filesystem::path config = root / "shadow" / "shadow.json";
    write_json(config, nlohmann::json::object());
    const std::filesystem::path artifact_root =
        config.parent_path() / "calibration_artifacts";
    write_json(
        artifact_root /
            "homography_reference_arena_1_2010093_projected_surface.json",
        reference_pointer("arena_1", "2010093", report, report_sha));
    write_json(
        artifact_root /
            "homography_reference_arena_2_2010094_projected_surface.json",
        reference_pointer("arena_2", "2010094", report, report_sha));
    std::vector<ProjectorIntensityCameraAuthorityRef> fallback = {
        {"2010093", "arena_1", "rig0", "shadow", config.string(), "", ""},
        {"2010094", "arena_2", "rig0", "shadow", config.string(), "", ""},
    };
    result = resolve_projector_intensity_authority(fallback);
    require(result.ok, result.error);
    require(
        result.provenance.at("source_evidence").at(0).value(
            "authority_role", std::string()) == "commissioning_reference",
        "reference-pointer fallback was not recorded");

    nlohmann::json camera_failed = passing_report();
    camera_failed["camera_level_summaries"][1]["passes_quality_gate"] = false;
    write_json(report, camera_failed);
    const std::string camera_failed_sha = file_sha256(report);
    direct[0].report_sha256 = camera_failed_sha;
    direct[1].report_sha256 = camera_failed_sha;
    result = resolve_projector_intensity_authority(direct);
    require(!result.ok, "a selected camera's failed quality gate was ignored");

    nlohmann::json tampered = passing_report();
    tampered["recommended_foreground_gray_u8"] = 76;
    write_json(report, tampered);
    result = resolve_projector_intensity_authority(direct);
    require(!result.ok, "tampered report unexpectedly passed checksum validation");

    std::filesystem::remove_all(root);
    std::cout << "projector_intensity_authority_tests passed\n";
    return 0;
}
