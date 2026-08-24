#include "dish_top_rim_observation.h"
#include "fnv1a64_fingerprint.h"
#include "gui/spatial_layout/physical_registration_selection.h"
#include "recording_physical_registration.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using orange::gui::spatial_layout::clear_active_physical_registration;
using orange::gui::spatial_layout::discover_physical_registration_artifacts;
using orange::gui::spatial_layout::active_physical_registration_pointer_path;
using orange::gui::spatial_layout::resolve_active_physical_registration;
using orange::gui::spatial_layout::select_physical_registration_artifact;
using orange::gui::spatial_layout::validate_physical_registration_artifact;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void write_json(const fs::path& path, const nlohmann::json& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::trunc);
    require(static_cast<bool>(stream), "could not create " + path.string());
    stream << value.dump(2) << '\n';
    require(static_cast<bool>(stream), "could not write " + path.string());
}

std::string fnv1a64(const std::string& bytes)
{
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kPrime;
    }
    return orange::calibration::format_fnv1a64_fingerprint(hash);
}

void write_complete_artifact(
    const fs::path& observation_path,
    const nlohmann::json& observation)
{
    const fs::path artifact_dir = observation_path.parent_path();
    write_json(observation_path, observation);
    const std::array<std::pair<std::string, std::string>, 4> files = {{
        {"captures/source_frame.png", "source-frame"},
        {"overlays/top_rim_fit.png", "review-overlay"},
        {"overlays/registration_hough_overlay.png", "hough-overlay"},
        {"overlays/valid_detection_region.png", "gate-overlay"},
    }};
    for (const auto& [relative, contents] : files) {
        const fs::path path = artifact_dir / relative;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << contents;
        require(static_cast<bool>(stream), "could not write " + path.string());
    }
    write_json(artifact_dir / "image_set.json", {
        {"schema_id", "orange.calibration.image_set"},
        {"schema_version", 2},
    });
    write_json(artifact_dir / "exports/spatial_dish_mask_runtime_v1.json", {
        {"schema_id", "orange.spatial.dish_mask_runtime"},
        {"schema_version", 1},
    });
    write_json(artifact_dir / "exports/palette_dish_mask_v2.json", {
        {"schema_id", "palette.dish_mask"},
        {"schema_version", 2},
    });
    const std::string artifact_id = observation.value("artifact_id", "");
    const nlohmann::json manifest = {
        {"schema_id", orange::calibration::kCalibrationManifestSchemaId},
        {"schema_version",
         orange::calibration::kCalibrationManifestSchemaVersion},
        {"artifact_id", artifact_id},
        {"artifact_schema_id",
         orange::calibration::kDishTopRimObservationSchemaId},
        {"artifact_schema_version",
         orange::calibration::kDishTopRimObservationSchemaVersion},
        {"files", {
            {"manifest", "manifest.json"},
            {"observation_json", "observation.json"},
            {"image_set_json", "image_set.json"},
            {"spatial_dish_mask_runtime_v1",
             "exports/spatial_dish_mask_runtime_v1.json"},
            {"palette_dish_mask_v2",
             "exports/palette_dish_mask_v2.json"},
            {"source_frame", files[0].first},
            {"review_overlay", files[1].first},
            {"registration_hough_overlay", files[2].first},
            {"valid_detection_overlay", files[3].first},
        }},
        {"checksums", {
            {"algorithm", orange::calibration::kCalibrationFingerprintAlgorithm},
            {"source_frame", fnv1a64(files[0].second)},
            {"review_overlay", fnv1a64(files[1].second)},
            {"registration_hough_overlay", fnv1a64(files[2].second)},
            {"valid_detection_overlay", fnv1a64(files[3].second)},
        }},
    };
    write_json(artifact_dir / "manifest.json", manifest);
}

nlohmann::json accepted_observation(
    const std::string& artifact_id,
    const std::string& camera_serial,
    int width,
    int height,
    const std::string& pixel_format)
{
    return {
        {"schema_id", orange::calibration::kDishTopRimObservationSchemaId},
        {"schema_version",
         orange::calibration::kDishTopRimObservationSchemaVersion},
        {"artifact_id", artifact_id},
        {"created_utc", "2026-08-23T12:00:00Z"},
        {"registration_products", {
            {"physical_registration", {
                {"product_id",
                 orange::calibration::kDailyPhysicalDishRegistrationProductId},
                {"authority", "orange"},
                {"status", "accepted"},
                {"runtime_selection_status", "not_selected_by_artifact_writer"},
                {"coordinate_space", "camera_native_pixels"},
                {"citrus_required", false},
            }},
            {"projection_registration", {
                {"product_id",
                 orange::calibration::kDailyProjectionRegistrationProductId},
                {"authority", "citrus"},
                {"status", "not_applicable"},
                {"reason", "no_active_projection_canvas"},
            }},
        }},
        {"camera", {
            {"serial", camera_serial},
            {"name", "Cam" + camera_serial},
            {"width", width},
            {"height", height},
            {"pixel_format", pixel_format},
        }},
        {"capture", {
            {"dish_fill_state", "recording_fill_level"},
            {"projector_state", "not_in_use"},
            {"filter_state", "production_ir_filter_installed"},
        }},
        {"accepted_inner_rim_boundary", {
            {"coordinate_space", "camera_native_pixels"},
            {"target_plane", "dish_top_rim"},
            {"operator_confirmed", true},
            {"geometry", {
                {"type", "circle"},
                {"center_px", {{"x", 2251.25}, {"y", 2248.75}}},
                {"radius_px", 2098.5},
            }},
        }},
        {"accepted_mask", {
            {"shape", "circle"},
            {"coordinate_space", "camera_native_pixels"},
            {"center_px", {{"x", 2251.25}, {"y", 2248.75}}},
            {"radius_px", 2103.5},
        }},
        {"valid_detection_region", {
            {"coordinate_space", "camera_native_pixels"},
            {"derived_from", "accepted_inner_rim_boundary"},
            {"centroid_gate_outset_px", 5.0},
            {"offset_direction", "outward"},
            {"purpose", "bounding_box_centroid_detection_gating"},
            {"geometry", {
                {"type", "circle"},
                {"center_px", {{"x", 2251.25}, {"y", 2248.75}}},
                {"radius_px", 2103.5},
            }},
        }},
        {"operator_review", {{"accepted", true}}},
    };
}

fs::path make_root()
{
    const fs::path root = fs::path("/tmp") /
        ("orange_physical_registration_selection_test_" +
         std::to_string(static_cast<long long>(::getpid())));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    return root;
}

void test_selection_lifecycle()
{
    const fs::path root = make_root();
    const fs::path observation_path = root / "sessions" / "session_a" /
        "artifacts" / "Cam2010095_arena_unknown" /
        "top_rim_observations" / "rim_a" / "observation.json";
    write_complete_artifact(
        observation_path,
        accepted_observation("rim_a", "2010095", 4512, 4512, "Mono8"));

    std::vector<std::string> warnings;
    auto candidates = discover_physical_registration_artifacts(
        root, "2010095", 4512, 4512, "Mono8", &warnings);
    require(warnings.empty(), "valid discovery should not warn");
    require(candidates.size() == 1, "one accepted candidate expected");
    require(candidates[0].compatible, "candidate must be compatible");
    require(candidates[0].accepted_center_x_px == 2251.25,
            "accepted center must be preserved");
    require(candidates[0].centroid_gate_outset_px == 5.0,
            "centroid outset must be preserved");

    std::string error;
    require(select_physical_registration_artifact(
                root, candidates[0], "2026-08-23T12:01:00Z", &error),
            "selection should succeed: " + error);
    auto resolved = resolve_active_physical_registration(
        root, "2010095", 4512, 4512, "Mono8");
    require(resolved.pointer_exists, "active pointer should exist");
    require(resolved.selected && resolved.valid,
            "selected artifact must revalidate after restart");
    require(resolved.status == "selected", "selected status expected");
    require(resolved.candidate.observation_path == observation_path,
            "pointer must resolve exact observation");

    nlohmann::json tampered = accepted_observation(
        "rim_a", "2010095", 4512, 4512, "Mono8");
    tampered["accepted_inner_rim_boundary"]["geometry"]["radius_px"] =
        1999.0;
    write_json(observation_path, tampered);
    resolved = resolve_active_physical_registration(
        root, "2010095", 4512, 4512, "Mono8");
    require(resolved.selected && !resolved.valid,
            "tampered selected artifact must fail closed");
    require(resolved.status == "invalid_selected",
            "tamper must be classified as invalid selected evidence");

    require(clear_active_physical_registration(
                root, "2010095", "2026-08-23T12:02:00Z", &error),
            "clear should succeed: " + error);
    resolved = resolve_active_physical_registration(
        root, "2010095", 4512, 4512, "Mono8");
    require(resolved.pointer_exists && !resolved.selected && !resolved.valid,
            "cleared pointer must be explicitly unselected");
    require(resolved.status == "not_selected", "clear status expected");
    require(fs::is_regular_file(observation_path),
            "clearing must not delete immutable evidence");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_compatibility_and_selection_race()
{
    const fs::path root = make_root();
    const fs::path observation_path = root / "sessions" / "session_b" /
        "artifacts" / "Cam2010096_arena_unknown" /
        "top_rim_observations" / "rim_b" / "observation.json";
    write_complete_artifact(
        observation_path,
        accepted_observation("rim_b", "2010096", 4512, 4512, "Mono8"));

    auto candidates = discover_physical_registration_artifacts(
        root, "2010096", 4512, 4512, "Mono8");
    require(candidates.size() == 1 && candidates[0].compatible,
            "baseline candidate expected");

    const auto raster_mismatch = discover_physical_registration_artifacts(
        root, "2010096", 2256, 2256, "Mono8");
    require(raster_mismatch.size() == 1 && !raster_mismatch[0].compatible,
            "native-raster mismatch must remain visible but incompatible");
    require(raster_mismatch[0].compatibility_reason ==
                "native_raster_mismatch",
            "native-raster reason expected");

    const auto format_mismatch = discover_physical_registration_artifacts(
        root, "2010096", 4512, 4512, "Mono12");
    require(format_mismatch.size() == 1 && !format_mismatch[0].compatible,
            "pixel-format mismatch must be incompatible");

    nlohmann::json changed = accepted_observation(
        "rim_b", "2010096", 4512, 4512, "Mono8");
    changed["capture"]["dish_fill_state"] = "changed_after_review";
    write_json(observation_path, changed);
    std::string error;
    require(!select_physical_registration_artifact(
                root, candidates[0], "2026-08-23T12:03:00Z", &error),
            "selection must reject evidence changed after review");
    require(!error.empty(), "race rejection should explain failure");

    const auto resolved = resolve_active_physical_registration(
        root, "2010096", 4512, 4512, "Mono8");
    require(!resolved.pointer_exists,
            "failed selection must not publish an active pointer");

    const fs::path incomplete_path = root / "sessions" / "session_incomplete" /
        "artifacts" / "Cam2010096_arena_unknown" /
        "top_rim_observations" / "rim_incomplete" / "observation.json";
    write_json(
        incomplete_path,
        accepted_observation(
            "rim_incomplete", "2010096", 4512, 4512, "Mono8"));
    const auto with_incomplete = discover_physical_registration_artifacts(
        root, "2010096", 4512, 4512, "Mono8");
    const auto incomplete = std::find_if(
        with_incomplete.begin(), with_incomplete.end(),
        [](const auto& candidate) {
            return candidate.artifact_id == "rim_incomplete";
        });
    require(incomplete != with_incomplete.end() && !incomplete->compatible,
            "partially written evidence must remain visible but unselectable");
    require(incomplete->compatibility_reason ==
                "artifact_completion_manifest_missing",
            "partial artifact should explain its missing completion manifest");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_malformed_json_fails_closed()
{
    const fs::path root = make_root();
    const fs::path observation_path = root / "sessions" / "session_bad" /
        "artifacts" / "Cam2010095_arena_unknown" /
        "top_rim_observations" / "rim_bad" / "observation.json";
    nlohmann::json malformed = accepted_observation(
        "rim_bad", "2010095", 4512, 4512, "Mono8");
    malformed["registration_products"] = nullptr;
    write_json(observation_path, malformed);

    const auto candidate = validate_physical_registration_artifact(
        root, observation_path, "2010095", 4512, 4512, "Mono8");
    require(!candidate.compatible,
            "null registration products must be rejected without throwing");

    const fs::path pointer_path = active_physical_registration_pointer_path(
        root, "2010095");
    write_json(pointer_path, {
        {"schema_id",
         orange::gui::spatial_layout::
             kActivePhysicalRegistrationPointerSchemaId},
        {"schema_version",
         orange::gui::spatial_layout::
             kActivePhysicalRegistrationPointerSchemaVersion},
        {"status", "selected"},
        {"camera_serial", "2010095"},
        {"selection", nullptr},
    });
    const auto resolved = resolve_active_physical_registration(
        root, "2010095", 4512, 4512, "Mono8");
    require(resolved.pointer_exists && !resolved.valid && !resolved.selected,
            "null selected pointer must fail closed without throwing");
    require(resolved.status == "invalid_pointer",
            "null selection must be classified as an invalid pointer");

    write_json(pointer_path, nlohmann::json::array({"not", "an", "object"}));
    const auto non_object_resolved = resolve_active_physical_registration(
        root, "2010095", 4512, 4512, "Mono8");
    require(non_object_resolved.pointer_exists &&
                !non_object_resolved.valid &&
                non_object_resolved.status == "invalid_pointer",
            "non-object active pointer must fail closed without throwing");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void test_recording_prearm_snapshot_and_runtime_marking()
{
    const fs::path root = make_root();
    const fs::path observation_path = root / "sessions" / "session_recording" /
        "artifacts" / "Cam2010095_arena_unknown" /
        "top_rim_observations" / "rim_recording" / "observation.json";
    write_complete_artifact(
        observation_path,
        accepted_observation(
            "rim_recording", "2010095", 4512, 4512, "Mono8"));
    const auto candidate = validate_physical_registration_artifact(
        root, observation_path, "2010095", 4512, 4512, "Mono8");
    require(candidate.compatible, "recording candidate must validate");
    std::string error;
    require(select_physical_registration_artifact(
                root, candidate, "2026-08-23T12:04:00Z", &error),
            "recording selection should succeed: " + error);

    nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"cameras", {{"2010095", {{"camera_serial", "2010095"}}}}},
    };
    orange::recording_geometry::append_recording_physical_registrations(
        &contract, root,
        {{"2010095", 4512, 4512, "Mono8"}},
        "2026-08-23T12:05:00Z");
    const auto& physical = contract["cameras"]["2010095"]
        ["physical_registration"];
    require(physical.value("status", "") == "selected_resolved",
            "recording pre-arm must resolve selected physical evidence");
    require(physical["active_pointer"].value("sha256", "").rfind(
                "sha256:", 0) == 0,
            "recording pre-arm must digest the active pointer");
    require(physical["recording_snapshot_entry"]
                ["valid_detection_region"]["geometry"]
                .value("radius_px", 0.0) == 2103.5,
            "recording snapshot must embed the exact centroid gate");
    require(physical["compact_artifacts"].contains(
                "spatial_dish_mask_runtime_v1"),
            "recording snapshot must bind the compact runtime mask export");
    require(orange::recording_geometry::mark_recording_dish_mask_runtime_use(
                &contract, "2010095", "gate_and_input_mask", true, true),
            "runtime mask use must mark the selected physical entry");
    require(physical["recording_snapshot_entry"].value(
                "active_in_orange_neural_input_mask", false),
            "runtime marking must preserve neural input-mask use");

    nlohmann::json no_selection_contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"cameras", {{"2010096", {{"camera_serial", "2010096"}}}}},
    };
    orange::recording_geometry::append_recording_physical_registrations(
        &no_selection_contract, root,
        {{"2010096", 4512, 4512, "Mono8"}},
        "2026-08-23T12:05:00Z");
    require(no_selection_contract["cameras"]["2010096"]
                ["physical_registration"].value("status", "") ==
                "not_performed",
            "ordinary recording must preserve an explicit optional not-selected state");

    nlohmann::json changed = accepted_observation(
        "rim_recording", "2010095", 4512, 4512, "Mono8");
    changed["accepted_inner_rim_boundary"]["geometry"]["radius_px"] = 2000.0;
    write_json(observation_path, changed);
    nlohmann::json invalid_contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"cameras", {{"2010095", {{"camera_serial", "2010095"}}}}},
    };
    orange::recording_geometry::append_recording_physical_registrations(
        &invalid_contract, root,
        {{"2010095", 4512, 4512, "Mono8"}},
        "2026-08-23T12:06:00Z");
    require(invalid_contract["cameras"]["2010095"]
                ["physical_registration"].value("status", "") ==
                "invalid_selected",
            "a selected artifact changed after selection must remain invalid");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

}  // namespace

int main()
{
    try {
        test_selection_lifecycle();
        test_compatibility_and_selection_race();
        test_malformed_json_fails_closed();
        test_recording_prearm_snapshot_and_runtime_marking();
        std::cout << "physical registration selection tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "physical registration selection tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
