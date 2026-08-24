#include "yolo_spatial_mask.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

nlohmann::json valid_contract()
{
    return {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"cameras", {
            {"2010095", {
                {"daily_registration_geometry", {
                    {"schema_id", "orange.recording.daily_registration_camera_geometry"},
                    {"schema_version", 1},
                    {"status", "resolved"},
                    {"mode", "selected_daily_registration"},
                    {"registration_id", "dailyreg_fixture"},
                    {"recording_snapshot_entry", {
                        {"artifact_id", "dishrim_fixture_2010095"},
                        {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
                        {"artifact_schema_version", 2},
                        {"camera_serial", "2010095"},
                        {"arena_id", "arena_3"},
                        {"coordinate_space", "camera_native_pixels"},
                        {"available_for_downstream_detection_gating", true},
                        {"gating_semantics",
                         "bounding_box_centroid_inside_valid_detection_region"},
                        {"operator_review", {{"accepted", true}}},
                        {"camera", {
                            {"width", 4512},
                            {"height", 4512},
                        }},
                        {"calibration_ref", {
                            {"fingerprint", "fnv1a64:0123456789abcdef"},
                        }},
                        {"valid_detection_region", {
                            {"coordinate_space", "camera_native_pixels"},
                            {"purpose", "bounding_box_centroid_detection_gating"},
                            {"offset_direction", "outward"},
                            {"geometry", {
                                {"type", "circle"},
                                {"center_px", {{"x", 2200.5}, {"y", 2300.25}}},
                                {"radius_px", 2100.0},
                            }},
                        }},
                        {"source", {
                            {"path", "/fixture/observation.json"},
                            {"intended_recording_relative_path",
                             "recording_geometry_assets/cameras/Cam2010095/daily_registration/rim_observation/observation.json"},
                            {"sha256", "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                        }},
                    }},
                }},
            }},
        }},
    };
}

void test_modes()
{
    using namespace orange::analytics_mask;
    Mode mode = Mode::kOff;
    require(parse_mode("off", &mode) && mode == Mode::kOff, "off mode parse");
    require(parse_mode("audit", &mode) && mode == Mode::kAudit, "audit mode parse");
    require(parse_mode("gate_only", &mode) && mode == Mode::kGateOnly,
            "gate-only mode parse");
    require(parse_mode("gate_and_input_mask", &mode) &&
                mode == Mode::kGateAndInputMask,
            "masked mode parse");
    require(!parse_mode("masked", &mode), "unknown mode must fail");
}

void test_environment_config()
{
    using namespace orange::analytics_mask;
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_MODE");
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX");
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS");
    RuntimeConfigResolveResult defaults =
        resolve_runtime_config_from_environment();
    require(defaults.ok && defaults.config.mode == Mode::kOff,
            "environment default must be off");
    require(!defaults.mode_was_explicit, "default mode must not be explicit");

    ::setenv("ORANGE_YOLO_SPATIAL_MASK_MODE", "gate-and-input-mask", 1);
    ::setenv("ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX", "32.5", 1);
    ::setenv("ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS", "900", 1);
    RuntimeConfigResolveResult configured =
        resolve_runtime_config_from_environment();
    require(configured.ok && configured.mode_was_explicit,
            "explicit environment config");
    require(configured.config.mode == Mode::kGateAndInputMask,
            "hyphenated environment mode normalization");
    require(std::abs(configured.config.input_context_outset_px - 32.5f) < 1e-5f,
            "environment context outset");
    require(configured.config.apply_timeout_ms == 900,
            "environment apply timeout");

    ::setenv("ORANGE_YOLO_SPATIAL_MASK_MODE", "unknown", 1);
    require(!resolve_runtime_config_from_environment().ok,
            "invalid explicit mode must fail");
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_MODE");
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_INPUT_CONTEXT_OUTSET_PX");
    ::unsetenv("ORANGE_YOLO_SPATIAL_MASK_APPLY_TIMEOUT_MS");
}

void test_resolution_and_outset()
{
    using namespace orange::analytics_mask;
    ResolveResult resolved = resolve_policy_from_recording_geometry_contract(
        valid_contract(), "2010095", 4512, 4512,
        Mode::kGateAndInputMask, 24.5f);
    require(resolved.ok, resolved.error);
    require(resolved.policy.registration_id == "dailyreg_fixture",
            "registration identity");
    require(resolved.policy.artifact_id == "dishrim_fixture_2010095",
            "artifact identity");
    require(!resolved.policy.recording_relative_path.empty(),
            "recording-local source path");
    require(std::abs(resolved.policy.centroid_gate_circle.radius - 2100.0f) < 1e-4f,
            "gate radius");
    require(std::abs(resolved.policy.input_circle.radius - 2124.5f) < 1e-4f,
            "input outset");
    const nlohmann::json serialized = policy_to_json(resolved.policy);
    require(serialized["mode"] == "gate_and_input_mask", "serialized mode");
    require(serialized["input_mask"]["enabled"].get<bool>(),
            "serialized input mask enabled");
}

void test_standalone_physical_registration_precedence()
{
    using namespace orange::analytics_mask;
    nlohmann::json contract = valid_contract();
    nlohmann::json physical = contract["cameras"]["2010095"]
        ["daily_registration_geometry"];
    physical["status"] = "selected_resolved";
    physical["mode"] = "selected_physical_registration";
    physical["artifact_id"] = "dishrim_fixture_2010095";
    physical.erase("registration_id");
    contract["cameras"]["2010095"]["physical_registration"] = physical;
    contract["cameras"]["2010095"].erase("daily_registration_geometry");

    ResolveResult resolved = resolve_policy_from_recording_geometry_contract(
        contract, "2010095", 4512, 4512,
        Mode::kGateAndInputMask, 0.0f);
    require(resolved.ok, resolved.error);
    require(resolved.policy.registration_id == "dishrim_fixture_2010095",
            "standalone physical artifact must provide mask identity");

    contract["cameras"]["2010095"]["daily_registration_geometry"] =
        valid_contract()["cameras"]["2010095"]["daily_registration_geometry"];
    contract["cameras"]["2010095"]["physical_registration"]["status"] =
        "invalid_selected";
    ResolveResult invalid = resolve_policy_from_recording_geometry_contract(
        contract, "2010095", 4512, 4512,
        Mode::kGateOnly, 0.0f);
    require(!invalid.ok,
            "invalid explicit physical selection must not fall back to Citrus");
    require(invalid.error_code == "invalid_selected",
            "invalid explicit selection needs a machine-readable classification");

    contract["cameras"]["2010095"].erase("physical_registration");
    contract["cameras"]["2010095"].erase("daily_registration_geometry");
    ResolveResult missing = resolve_policy_from_recording_geometry_contract(
        contract, "2010095", 4512, 4512, Mode::kGateOnly, 0.0f);
    require(!missing.ok && missing.error_code == "missing_required",
            "an enabled mask without a selection must be classified missing_required");
}

void test_strict_resolution_failures()
{
    using namespace orange::analytics_mask;
    ResolveResult wrong_shape = resolve_policy_from_recording_geometry_contract(
        valid_contract(), "2010095", 4096, 4512, Mode::kGateOnly, 0.0f);
    require(!wrong_shape.ok, "raster mismatch must fail");

    nlohmann::json stale = valid_contract();
    stale["cameras"]["2010095"]["daily_registration_geometry"]["status"] =
        "stale";
    ResolveResult stale_result = resolve_policy_from_recording_geometry_contract(
        stale, "2010095", 4512, 4512, Mode::kGateOnly, 0.0f);
    require(!stale_result.ok, "stale selection must fail");

    nlohmann::json bad_checksum = valid_contract();
    bad_checksum["cameras"]["2010095"]["daily_registration_geometry"]
        ["recording_snapshot_entry"]["source"]["sha256"] = "fnv1a64:bad";
    ResolveResult checksum_result = resolve_policy_from_recording_geometry_contract(
        bad_checksum, "2010095", 4512, 4512, Mode::kGateOnly, 0.0f);
    require(!checksum_result.ok, "non-SHA256 artifact identity must fail");

    ResolveResult off = resolve_policy_from_recording_geometry_contract(
        nlohmann::json(), "2010095", 4512, 4512, Mode::kOff, 0.0f);
    require(off.ok, "off mode must not require geometry");
}

void test_centroid_decisions()
{
    using namespace orange::analytics_mask;
    const Circle gate{100.0f, 100.0f, 50.0f};
    GateDecision center = evaluate_box_centroid(90.0f, 90.0f, 20.0f, 20.0f, gate);
    require(center.inside && std::abs(center.signed_boundary_distance_px - 50.0f) < 1e-5f,
            "center box must be inside");
    GateDecision boundary = evaluate_box_centroid(140.0f, 90.0f, 20.0f, 20.0f, gate);
    require(boundary.inside && std::abs(boundary.signed_boundary_distance_px) < 1e-5f,
            "boundary is inclusive");
    GateDecision outside = evaluate_box_centroid(141.0f, 90.0f, 20.0f, 20.0f, gate);
    require(!outside.inside && outside.signed_boundary_distance_px < 0.0f,
            "outside centroid must be rejected");
}

}  // namespace

int main()
{
    try {
        test_modes();
        test_environment_config();
        test_resolution_and_outset();
        test_standalone_physical_registration_precedence();
        test_strict_resolution_failures();
        test_centroid_decisions();
        std::cout << "yolo_spatial_mask_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "yolo_spatial_mask_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
