#include "gui/spatial_layout/projector_intensity_authority.h"

#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace orange::gui::spatial_layout {
namespace {

std::string safe_path_component(std::string value)
{
    for (char& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' ||
              ch == '_' || ch == '.')) {
            ch = '_';
        }
    }
    return value.empty() ? "unknown" : value;
}

std::string normalize_sha256(std::string value)
{
    constexpr const char* prefix = "sha256:";
    if (value.rfind(prefix, 0) == 0) {
        value.erase(0, std::char_traits<char>::length(prefix));
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool read_json(const std::filesystem::path& path,
               nlohmann::json* value_out,
               std::string* error_out)
{
    std::string bytes;
    if (!checksum::read_file(path, &bytes, error_out)) {
        return false;
    }
    nlohmann::json value = nlohmann::json::parse(bytes, nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        if (error_out != nullptr) {
            *error_out = "Invalid JSON object: " + path.string();
        }
        return false;
    }
    if (value_out != nullptr) {
        *value_out = std::move(value);
    }
    return true;
}

bool load_reference_pointer_provenance(
    const ProjectorIntensityCameraAuthorityRef& ref,
    std::string* report_path_out,
    std::string* report_sha256_out,
    int* recommended_gray_out,
    nlohmann::json* pointer_evidence_out,
    std::string* error_out)
{
    if (ref.citrus_canvas_config_path.empty() || ref.arena_id.empty() ||
        ref.camera_serial.empty()) {
        if (error_out != nullptr) {
            *error_out = "Citrus canvas/arena/camera identity is incomplete for camera " +
                ref.camera_serial + ".";
        }
        return false;
    }
    const std::filesystem::path pointer_path =
        std::filesystem::path(ref.citrus_canvas_config_path).parent_path() /
        "calibration_artifacts" /
        ("homography_reference_" + safe_path_component(ref.arena_id) + "_" +
         safe_path_component(ref.camera_serial) + "_projected_surface.json");
    nlohmann::json pointer;
    std::string error;
    if (!read_json(pointer_path, &pointer, &error)) {
        if (error_out != nullptr) {
            *error_out = "Projector-intensity provenance is absent from the active "
                "homography and the commissioning-reference pointer could not be read: " +
                error;
        }
        return false;
    }
    const nlohmann::json photometry = pointer.value(
        "source_photometry", nlohmann::json::object());
    const nlohmann::json commissioning = photometry.value(
        "commissioning_provenance", nlohmann::json::object());
    const bool identity_ok =
        pointer.value("schema_id", std::string()) ==
            "citrus.calibration.active_homography" &&
        pointer.value("schema_version", 0) == 1 &&
        pointer.value("status", std::string()) == "accepted" &&
        pointer.value("rig_id", std::string()) == ref.rig_id &&
        pointer.value("canvas_name", std::string()) == ref.canvas_id &&
        pointer.value("arena_id", std::string()) == ref.arena_id &&
        pointer.value("camera_id", std::string()) == ref.camera_serial &&
        pointer.value("target_plane", std::string()) == "projected_surface" &&
        pointer.value("homography_role", std::string()) ==
            "commissioning_reference" &&
        photometry.value("status", std::string()) == "passed";
    const std::string report_path = commissioning.value("report_path", std::string());
    const std::string report_sha256 = commissioning.value(
        "report_sha256", std::string());
    const int recommended_gray = commissioning.value(
        "recommended_foreground_gray_u8", -1);
    if (!identity_ok || report_path.empty() || report_sha256.empty() ||
        recommended_gray < 0 || recommended_gray > 255) {
        if (error_out != nullptr) {
            *error_out = "Commissioning-reference homography has invalid identity, "
                "photometry status, or projector-intensity provenance: " +
                pointer_path.string();
        }
        return false;
    }
    std::string pointer_sha256;
    if (!checksum::file_sha256(pointer_path, &pointer_sha256, &error)) {
        if (error_out != nullptr) {
            *error_out = error;
        }
        return false;
    }
    *report_path_out = report_path;
    *report_sha256_out = normalize_sha256(report_sha256);
    *recommended_gray_out = recommended_gray;
    if (pointer_evidence_out != nullptr) {
        *pointer_evidence_out = {
            {"camera_serial", ref.camera_serial},
            {"arena_id", ref.arena_id},
            {"pointer_path", pointer_path.string()},
            {"pointer_sha256", pointer_sha256},
            {"authority_role", "commissioning_reference"},
        };
    }
    return true;
}

}  // namespace

ProjectorIntensityAuthorityResult resolve_projector_intensity_authority(
    const std::vector<ProjectorIntensityCameraAuthorityRef>& camera_refs)
{
    ProjectorIntensityAuthorityResult result;
    if (camera_refs.empty()) {
        result.error = "No cameras were selected for projector-intensity resolution.";
        return result;
    }

    std::string common_report_path;
    std::string common_report_sha256;
    int reference_recommended_gray = -1;
    nlohmann::json source_evidence = nlohmann::json::array();
    std::set<std::string> requested_cameras;
    for (const ProjectorIntensityCameraAuthorityRef& ref : camera_refs) {
        if (ref.camera_serial.empty() || !requested_cameras.insert(ref.camera_serial).second) {
            result.error = "Projector-intensity camera identities are empty or duplicated.";
            return result;
        }
        std::string report_path = ref.report_path;
        std::string report_sha256 = normalize_sha256(ref.report_sha256);
        int recommended_gray = -1;
        nlohmann::json evidence = {
            {"camera_serial", ref.camera_serial},
            {"arena_id", ref.arena_id},
            {"authority_role", "active_homography_provenance"},
        };
        if (report_path.empty() || report_sha256.empty()) {
            if (!load_reference_pointer_provenance(
                    ref,
                    &report_path,
                    &report_sha256,
                    &recommended_gray,
                    &evidence,
                    &result.error)) {
                return result;
            }
        }
        if (common_report_path.empty()) {
            common_report_path = report_path;
            common_report_sha256 = report_sha256;
        } else if (report_path != common_report_path ||
                   report_sha256 != common_report_sha256) {
            result.error = "Selected cameras do not reference one identical immutable "
                "projector-intensity commissioning report.";
            return result;
        }
        if (recommended_gray >= 0) {
            if (reference_recommended_gray < 0) {
                reference_recommended_gray = recommended_gray;
            } else if (recommended_gray != reference_recommended_gray) {
                result.error = "Commissioning-reference homographies disagree on the "
                    "recommended projector intensity.";
                return result;
            }
        }
        source_evidence.push_back(std::move(evidence));
    }

    std::string actual_sha256;
    std::string error;
    if (!checksum::file_sha256(common_report_path, &actual_sha256, &error)) {
        result.error = "Could not checksum the projector-intensity commissioning report: " +
            error;
        return result;
    }
    if (normalize_sha256(actual_sha256) != common_report_sha256) {
        result.error = "Projector-intensity commissioning report checksum mismatch.";
        return result;
    }

    nlohmann::json report;
    if (!read_json(common_report_path, &report, &error)) {
        result.error = error;
        return result;
    }
    const int gray = report.value("recommended_foreground_gray_u8", -1);
    const nlohmann::json all_camera_levels = report.value(
        "level_passes_all_cameras", nlohmann::json::object());
    const std::string level_key = std::to_string(gray);
    if (report.value("schema_id", std::string()) !=
            "orange.projector_intensity_commissioning.report" ||
        report.value("schema_version", 0) != 1 ||
        report.value("status", std::string()) != "pass" ||
        gray < 0 || gray > 255 ||
        !all_camera_levels.value(level_key, false) ||
        (reference_recommended_gray >= 0 && gray != reference_recommended_gray)) {
        result.error = "Projector-intensity commissioning report is not a passing, "
            "internally consistent authority for its recommended level.";
        return result;
    }

    std::map<std::string, bool> camera_passes;
    const nlohmann::json summaries = report.value(
        "camera_level_summaries", nlohmann::json::array());
    if (!summaries.is_array()) {
        result.error = "Projector-intensity report has no camera-level summary array.";
        return result;
    }
    for (const nlohmann::json& summary : summaries) {
        if (!summary.is_object() ||
            summary.value("foreground_gray_u8", -1) != gray) {
            continue;
        }
        camera_passes[summary.value("camera_serial", std::string())] =
            summary.value("passes_quality_gate", false);
    }
    for (const std::string& camera : requested_cameras) {
        const auto it = camera_passes.find(camera);
        if (it == camera_passes.end() || !it->second) {
            result.error = "Recommended projector intensity is not qualified for selected "
                "camera " + camera + ".";
            return result;
        }
    }

    result.ok = true;
    result.foreground_gray_u8 = gray;
    result.report_path = common_report_path;
    result.report_sha256 = common_report_sha256;
    result.provenance = {
        {"schema_id", "orange.projector_intensity_commissioning.reference"},
        {"schema_version", 1},
        {"status", "validated"},
        {"report_path", result.report_path},
        {"report_sha256", result.report_sha256},
        {"recommended_foreground_gray_u8", gray},
        {"level_passes_all_cameras", true},
        {"validated_camera_serials", requested_cameras},
        {"source_evidence", source_evidence},
    };
    return result;
}

}  // namespace orange::gui::spatial_layout
