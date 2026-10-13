// Golden bytes for the YOLO spatial-mask runtime policy (orange::analytics_mask::policy_to_json),
// embedded in the detector event logs and the recording geometry contract.
//
// Writes the policy in every mode (off from the default Policy, and audit / gate_only /
// gate_and_input_mask resolved from a geometry-contract fixture, with and without an input
// outset) and compares the bytes with
// tools/fixtures/generated_outputs/golden/spatial_mask_policies.jsonl, produced by the
// hand-written writer before it switched to the generated orange.analytics.spatial_mask_runtime
// v1 code (2026-10-12).
//
//   spatial_mask_policy_golden_tests <golden file> [--write]
#include "citrus_recording_geometry.h"
#include "yolo_spatial_mask.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
nlohmann::json contract()
{
    return {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", orange::recording_geometry::kRecordingGeometryContractSchemaVersion},
        {"cameras", {{"2010095", {{"daily_registration_geometry", {
            {"schema_id", "orange.recording.daily_registration_camera_geometry"},
            {"schema_version", 2},
            {"status", "resolved"},
            {"mode", "selected_daily_registration"},
            {"registration_id", "dailyreg_golden"},
            {"recording_snapshot_entry", {
                {"artifact_id", "dishrim_golden_2010095"},
                {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
                {"artifact_schema_version", 2},
                {"camera_serial", "2010095"},
                {"arena_id", "arena_3"},
                {"coordinate_space", "camera_native_pixels"},
                {"available_for_downstream_detection_gating", true},
                {"gating_semantics", "bounding_box_centroid_inside_valid_detection_region"},
                {"operator_review", {{"accepted", true}}},
                {"camera", {{"width", 4512}, {"height", 4512}}},
                {"calibration_ref", {{"fingerprint", "fnv1a64:0123456789abcdef"}}},
                {"valid_detection_region", {
                    {"coordinate_space", "camera_native_pixels"},
                    {"purpose", "bounding_box_centroid_detection_gating"},
                    {"offset_direction", "outward"},
                    {"geometry", {{"type", "circle"}, {"center_px", {{"x", 2200.123}, {"y", 2300.25}}},
                                  {"radius_px", 2099.875}}},
                }},
                {"source", {
                    {"path", "/fixture/observation.json"},
                    {"intended_recording_relative_path",
                     "recording_geometry_assets/cameras/Cam2010095/daily_registration/rim_observation/observation.json"},
                    {"sha256", "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                }},
            }},
        }}}}}},
    };
}
}  // namespace

int main(int argc, char** argv)
{
    using namespace orange::analytics_mask;
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: spatial_mask_policy_golden_tests <golden file> [--write]\n";
        return 2;
    }
    std::ostringstream produced;
    produced << "off\t" << policy_to_json(Policy{}).dump() << '\n';
    for (const Mode mode : {Mode::kAudit, Mode::kGateOnly, Mode::kGateAndInputMask}) {
        for (const float outset : {0.0f, 24.5f}) {
            const ResolveResult resolved = resolve_policy_from_recording_geometry_contract(
                contract(), "2010095", 4512, 4512, mode, outset);
            if (!resolved.ok) {
                std::cerr << "fixture did not resolve: " << resolved.error << "\n";
                return 1;
            }
            produced << mode_to_string(mode) << "_outset_" << outset << '\t'
                     << policy_to_json(resolved.policy).dump() << '\n';
        }
    }
    if (argc == 3 && std::string(argv[2]) == "--write") {
        std::ofstream(argv[1], std::ios::binary) << produced.str();
        std::cout << "wrote " << argv[1] << "\n";
        return 0;
    }
    std::ifstream golden_in(argv[1]);
    std::stringstream golden;
    golden << golden_in.rdbuf();
    if (golden.str() != produced.str()) {
        std::cout << "[FAIL] policies differ from the golden file\n--- golden\n" << golden.str()
                  << "--- produced\n" << produced.str();
        return 1;
    }
    std::cout << "[PASS] policies equal the golden file (" << produced.str().size() << " bytes)\n";
    return 0;
}
