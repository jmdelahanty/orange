#include "yolo_event_log_validation.h"
#include "yolo_spatial_mask.h"
#include "json.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kCameraSerial = "2010096";

struct TestDir {
    explicit TestDir(const std::string& name)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("orange_yolo_event_log_validation_" + name + "_" + std::to_string(now));
        std::filesystem::create_directories(path);
    }

    ~TestDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

yolo_event_log::SyntheticYoloEventConfig make_config()
{
    yolo_event_log::SyntheticYoloEventConfig config;
    config.mode = "synthetic";
    config.every_n_frames = 10;
    config.emit_zero_detections = true;
    return config;
}

// A spatial mask policy as the YOLO worker writes it (policy_to_json), with its generation.
nlohmann::json policy_json(const orange::analytics_mask::Policy& policy, uint64_t generation)
{
    nlohmann::json out = orange::analytics_mask::policy_to_json(policy);
    out["policy_generation"] = generation;
    return out;
}

// A valid gate_only policy (camera 2010096).
orange::analytics_mask::Policy gate_only_policy()
{
    orange::analytics_mask::Policy policy;
    policy.mode = orange::analytics_mask::Mode::kGateOnly;
    policy.source_width = 4512;
    policy.source_height = 4512;
    policy.centroid_gate_circle = {2256.0f, 2256.0f, 2000.0f};
    policy.input_circle = {2256.0f, 2256.0f, 2000.0f};
    policy.camera_serial = kCameraSerial;
    policy.arena_id = "arena_1";
    policy.registration_id = "dailyreg_test";
    policy.artifact_id = "dishrim_test";
    policy.artifact_schema_id = "orange.calibration.dish_top_rim_observation";
    policy.artifact_schema_version = 2;
    policy.artifact_fingerprint = "fnv1a64:0123456789abcdef";
    policy.source_path = "/test/observation.json";
    policy.recording_relative_path = "recording_geometry_assets/test/observation.json";
    policy.source_sha256 = "sha256:" + std::string(64, 'c');
    return policy;
}

// orange.yolo_event v3 lines (the shapes of tools/fixtures/generated_outputs/golden).
nlohmann::json make_header()
{
    return {
        {"schema_id", "orange.yolo_event"}, {"schema_version", 3}, {"event_kind", "session_header"},
        {"recording_id", "rec"}, {"camera_serial", kCameraSerial}, {"camera_id", 0}, {"worker", "YoloWorker"},
        {"frame_identity_key", "frame.recording_frame_id"},
        {"source_frame", {{"width_px", 4512}, {"height_px", 4512}}},
        {"line_format", {{"confidence_decimals", 4}, {"coordinate_decimals", 3}, {"latency_decimals", 3}}},
        {"yolo", {{"worker", "YoloWorker"}, {"model_id", "m"}, {"engine_path", "/e"}, {"engine_sha256", ""},
                  {"engine_bytes", 0}, {"weights_sha256", ""}, {"onnx_sha256", ""}, {"engine_manifest_run_id", ""},
                  {"gpu_id", 0}, {"coordinate_space", "source_frame_pixels"}, {"detection_source", "model"},
                  {"synthetic_runtime_detection", false}}},
        {"spatial_mask", policy_json(orange::analytics_mask::Policy{}, 1)},
        {"citrus_live_ipc", {{"queue_name", "/shm_cam_2010096_v2"}, {"enabled", true}}}
    };
}

nlohmann::json detection_json()
{
    return {{"index", 0}, {"x_px", 10.0}, {"y_px", 20.0}, {"width_px", 30.0}, {"height_px", 40.0},
            {"label", 0}, {"confidence", 0.9}};
}

nlohmann::json make_event(const uint64_t sequence,
                          const uint64_t recording_frame_id,
                          const yolo_event_log::SyntheticYoloEventConfig& config)
{
    const bool has_detection =
        recording_frame_id > 0 &&
        (recording_frame_id % static_cast<uint64_t>(config.every_n_frames)) == 0;
    const int count = has_detection ? 1 : 0;
    return {
        {"schema_id", "orange.yolo_event"}, {"schema_version", 3}, {"event_kind", "yolo_result"},
        {"event_sequence", sequence}, {"camera_serial", kCameraSerial},
        {"frame", {{"stream_frame_id", recording_frame_id + 100}, {"camera_frame_id", recording_frame_id + 200},
                   {"recording_frame_id", recording_frame_id}, {"record_active", true}}},
        {"timestamps", {{"camera_timestamp", 1000 + recording_frame_id}, {"timestamp_sys_ns", 2000 + recording_frame_id},
                        {"event_epoch_us", 3000 + recording_frame_id}, {"event_monotonic_us", 4000 + recording_frame_id}}},
        {"yolo", {{"status", has_detection ? "detections" : "zero_detections"}, {"detection_count", count},
                  {"detection_source", "model"}, {"synthetic_runtime_detection", false},
                  {"production_detection_valid", true}}},
        {"spatial_mask", {{"policy_generation", 1},
            {"result", {{"raw_detection_count", count}, {"inside_detection_count", 0},
                        {"outside_detection_count", 0}, {"downstream_detection_count", count}}},
            {"outside_detections", nlohmann::json::array()}}},
        {"detections", has_detection ? nlohmann::json::array({detection_json()}) : nlohmann::json::array()},
        {"citrus_live_ipc", {{"enabled", true}, {"requested", true}, {"request_status", "published"}}}
    };
}

// Moves a frame line to a detection result, keeping the spatial mask counts consistent.
void set_detections(nlohmann::json* event, const char* status, int count)
{
    (*event)["yolo"]["status"] = status;
    (*event)["yolo"]["detection_count"] = count;
    (*event)["spatial_mask"]["result"]["raw_detection_count"] = count;
    (*event)["spatial_mask"]["result"]["downstream_detection_count"] = count;
    (*event)["detections"] = count ? nlohmann::json::array({detection_json()}) : nlohmann::json::array();
}

void write_metadata(const std::filesystem::path& recording_folder,
                    const uint64_t max_frame_id)
{
    std::ofstream file(recording_folder / ("Cam" + std::string(kCameraSerial) + "_meta.csv"));
    require(static_cast<bool>(file), "failed to create metadata fixture");
    file << "recording_frame_id,stream_frame_id\n";
    for (uint64_t frame_id = 1; frame_id <= max_frame_id; ++frame_id) {
        file << frame_id << "," << frame_id << "\n";
    }
}

using EventMutator = std::function<void(uint64_t line_number, nlohmann::json* event)>;

void write_event_log(const std::filesystem::path& recording_folder,
                     const yolo_event_log::SyntheticYoloEventConfig& config,
                     const uint64_t frame_count,
                     const EventMutator& mutator = {},
                     const uint64_t malformed_line = 0,
                     const bool with_header = true)
{
    std::ofstream file(recording_folder / ("Cam" + std::string(kCameraSerial) + "_yolo_events.jsonl"));
    require(static_cast<bool>(file), "failed to create yolo event log fixture");
    if (with_header) file << make_header().dump() << "\n";
    for (uint64_t line = 1; line <= frame_count; ++line) {
        if (line == malformed_line) {
            file << "{not-json}\n";
            continue;
        }
        nlohmann::json event = make_event(line, line, config);
        if (mutator) {
            mutator(line, &event);
        }
        file << event.dump() << "\n";
    }
}

yolo_event_log::YoloEventLogValidationStats summarize(
    const std::filesystem::path& recording_folder,
    const yolo_event_log::SyntheticYoloEventConfig& config)
{
    return yolo_event_log::summarize_yolo_event_log(
        recording_folder.string(), kCameraSerial, config);
}

void test_valid_synthetic_log_passes()
{
    TestDir dir("valid");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20);

    const auto stats = summarize(dir.path, config);
    require(stats.status == "pass", "valid synthetic event log should pass");
    require(stats.present, "valid synthetic event log should be present");
    require(stats.rows == 20, "expected 20 event rows");
    require(stats.detection_rows == 2, "expected detections on frames 10 and 20");
    require(stats.zero_rows == 18, "expected zero-detection rows for non-cadence frames");
    require(stats.metadata_rows == 20, "expected 20 metadata rows");
    require(stats.parse_errors == 0, "expected no parse errors");
    require(stats.schema_errors == 0, "expected no schema errors");
    require(stats.sequence_errors == 0, "expected no sequence errors");
    require(stats.cadence_errors == 0, "expected no cadence errors");
    require(stats.metadata_join_misses == 0, "expected no metadata join misses");
}

void test_missing_log_reports_missing()
{
    TestDir dir("missing");
    const auto config = make_config();
    write_metadata(dir.path, 20);

    const auto stats = summarize(dir.path, config);
    require(stats.status == "missing", "missing event log should report missing");
    require(!stats.present, "missing event log should not be present");
}

void test_sequence_error_fails()
{
    TestDir dir("sequence");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 7) {
            (*event)["event_sequence"] = 99;
        }
    });

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "sequence error should fail validation");
    require(stats.sequence_errors == 1, "expected one sequence error");
}

void test_cadence_error_fails()
{
    TestDir dir("cadence");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 9) {
            set_detections(event, "detections", 1);
        }
    });

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "off-cadence detection should fail validation");
    require(stats.cadence_errors == 1, "expected one cadence error");
}

void test_metadata_join_miss_fails()
{
    TestDir dir("metadata");
    const auto config = make_config();
    write_metadata(dir.path, 19);
    write_event_log(dir.path, config, 20);

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "metadata join miss should fail validation");
    require(stats.metadata_join_misses == 1, "expected one metadata join miss");
}

void test_malformed_json_fails()
{
    TestDir dir("malformed");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, {}, 4);

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "malformed JSON should fail validation");
    require(stats.parse_errors == 1, "expected one parse error");
}

void test_unknown_status_fails_schema()
{
    TestDir dir("schema");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 1) {
            (*event)["yolo"]["status"] = "pending";
        }
    });

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "unknown yolo status should fail validation");
    require(stats.schema_errors == 1, "expected one schema error");
}

void test_real_worker_log_skips_synthetic_cadence()
{
    TestDir dir("real_worker");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 9) {
            set_detections(event, "detections", 1);
        } else if (line == 12) {
            set_detections(event, "timeout", 0);
        } else if (line == 13) {
            set_detections(event, "failed", 0);
        }
    });

    const auto stats = summarize(dir.path, config);
    require(stats.status == "pass", "real worker logs should not enforce synthetic cadence");
    require(stats.cadence_errors == 0, "expected no real-worker cadence errors");
    require(stats.detection_rows == 3, "expected three detection rows");
    require(stats.timeout_rows == 1, "expected one timeout row");
    require(stats.failed_rows == 1, "expected one failed row");
}

void test_runtime_synthetic_marker_mismatch_fails_schema()
{
    TestDir dir("synthetic_marker");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 10) {
            (*event)["yolo"]["detection_source"] = "synthetic_center_box";
            (*event)["yolo"]["synthetic_runtime_detection"] = true;
            (*event)["yolo"]["production_detection_valid"] = true;
        }
    });

    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "runtime synthetic production marker mismatch should fail");
    require(stats.schema_errors == 1, "expected one runtime synthetic marker schema error");
}

void test_valid_spatial_mask_result_passes()
{
    TestDir dir("spatial_valid");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20);
    const auto stats = summarize(dir.path, config);
    require(stats.status == "pass", "valid spatial mask result should pass");
    require(stats.schema_errors == 0, "valid spatial mask schema");
}

void test_spatial_mask_count_mismatch_fails_schema()
{
    TestDir dir("spatial_invalid");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 1) {
            (*event)["spatial_mask"]["result"]["raw_detection_count"] = 1;
        }
    });
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail", "spatial count mismatch should fail");
    require(stats.schema_errors == 1, "one spatial schema error expected");
}

void test_unknown_policy_generation_fails_schema()
{
    TestDir dir("spatial_generation");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 5) {
            (*event)["spatial_mask"]["policy_generation"] = 2;
        }
    });
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail" && stats.schema_errors == 1,
            "a frame line naming an undeclared policy generation must fail");
}

// A spatial_mask_policy line switches to an active gate_only policy (generation 2) from frame 11;
// frame lines name it and report one outside detection rejected.
void test_active_policy_line_passes()
{
    TestDir dir("policy_line");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line < 11) return;
        auto& mask = (*event)["spatial_mask"];
        const uint64_t count = (*event)["yolo"]["detection_count"].get<uint64_t>();
        mask["policy_generation"] = 2;
        mask["result"] = {{"raw_detection_count", count + 1}, {"inside_detection_count", count},
                          {"outside_detection_count", 1}, {"downstream_detection_count", count}};
        mask["outside_detections"] = nlohmann::json::array({{
            {"raw_index", 0},
            {"box", {{"x_px", 10.0}, {"y_px", 20.0}, {"width_px", 30.0}, {"height_px", 40.0},
                     {"label", 0}, {"confidence", 0.9}}},
            {"centroid_px", {{"x", 25.0}, {"y", 40.0}}},
            {"signed_boundary_distance_px", 12.5}, {"decision", "rejected"},
            {"rejected_reason", "outside_valid_detection_region"}}});
        (*event)["detections"] = count ? nlohmann::json::array({detection_json()}) : nlohmann::json::array();
    });
    // Insert the policy line before frame 11 (file line 12: header + 10 frames).
    const auto path = dir.path / ("Cam" + std::string(kCameraSerial) + "_yolo_events.jsonl");
    std::ifstream in(path);
    std::string text, line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (n == 12) {
            text += nlohmann::json{{"schema_id", "orange.yolo_event"}, {"schema_version", 3},
                                   {"event_kind", "spatial_mask_policy"}, {"camera_serial", kCameraSerial},
                                   {"frame", {{"recording_frame_id", 11}}},
                                   {"spatial_mask", policy_json(gate_only_policy(), 2)}}.dump() + "\n";
        }
        text += line + "\n";
    }
    in.close();
    std::ofstream(path) << text;
    const auto stats = summarize(dir.path, config);
    require(stats.status == "pass" && stats.header_rows == 2,
            "an active policy introduced by a policy line passes: " + stats.error);
}

// A policy that contradicts its mode (gate_only with the input mask on) is refused by the schema;
// every frame line naming its generation is an error, and the reason is reported.
void test_contradictory_policy_fails()
{
    TestDir dir("policy_contradiction");
    auto config = make_config();
    config.mode = "real";
    write_metadata(dir.path, 5);
    std::ofstream file(dir.path / ("Cam" + std::string(kCameraSerial) + "_yolo_events.jsonl"));
    nlohmann::json header = make_header();
    header["spatial_mask"] = policy_json(gate_only_policy(), 1);
    header["spatial_mask"]["input_mask"]["enabled"] = true;
    file << header.dump() << "\n";
    for (uint64_t frame = 1; frame <= 5; ++frame) file << make_event(frame, frame, config).dump() << "\n";
    file.close();
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail" && stats.schema_errors == 5, "five frame lines name an invalid policy");
    require(stats.error.find("/spatial_mask/input_mask/enabled: must be false") != std::string::npos,
            "the schema's reason is reported: " + stats.error);
}

void test_crop_metadata_source()
{
    TestDir dir("crop_metadata_source");
    auto config = make_config(); config.mode = "real";
    write_metadata(dir.path, 20); write_event_log(dir.path, config, 20);
    const auto crop = dir.path / ("Cam" + std::string(kCameraSerial) + "_crop_meta.csv");
    std::filesystem::rename(dir.path / ("Cam" + std::string(kCameraSerial) + "_meta.csv"), crop);
    auto stats = yolo_event_log::summarize_yolo_event_log(dir.path.string(), kCameraSerial, config, crop);
    require(stats.status == "pass" && stats.metadata_rows == 20, "crop-only detector join required full-frame metadata");
    std::filesystem::remove(crop);
    stats = yolo_event_log::summarize_yolo_event_log(dir.path.string(), kCameraSerial, config, crop);
    require(stats.status == "fail", "missing explicit source silently skipped detector join");
}
}  // namespace

void test_log_without_header_fails()
{
    TestDir dir("no_header");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, {}, 0, false);
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail" && stats.schema_errors > 0, "frame lines without a header must fail");
}

void test_other_schema_version_fails()
{
    TestDir dir("version");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 3) {
            (*event)["schema_version"] = 2;
        }
    });
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail" && stats.schema_errors == 1, "a v2 line in a v3 log must fail");
    require(stats.error.find("schema_version") != std::string::npos,
            "the failure names the mismatching field: " + stats.error);
}

void test_undeclared_key_fails()
{
    TestDir dir("undeclared");
    const auto config = make_config();
    write_metadata(dir.path, 20);
    write_event_log(dir.path, config, 20, [](const uint64_t line, nlohmann::json* event) {
        if (line == 4) {
            (*event)["frame"]["source_frame_id"] = 4;
        }
    });
    const auto stats = summarize(dir.path, config);
    require(stats.status == "fail" && stats.schema_errors == 1, "a key the schema does not declare must fail");
}

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"crop_metadata_source", &test_crop_metadata_source},
        {"valid_synthetic_log_passes", &test_valid_synthetic_log_passes},
        {"log_without_header_fails", &test_log_without_header_fails},
        {"other_schema_version_fails", &test_other_schema_version_fails},
        {"undeclared_key_fails", &test_undeclared_key_fails},
        {"missing_log_reports_missing", &test_missing_log_reports_missing},
        {"sequence_error_fails", &test_sequence_error_fails},
        {"cadence_error_fails", &test_cadence_error_fails},
        {"metadata_join_miss_fails", &test_metadata_join_miss_fails},
        {"malformed_json_fails", &test_malformed_json_fails},
        {"unknown_status_fails_schema", &test_unknown_status_fails_schema},
        {"real_worker_log_skips_synthetic_cadence", &test_real_worker_log_skips_synthetic_cadence},
        {"runtime_synthetic_marker_mismatch_fails_schema",
         &test_runtime_synthetic_marker_mismatch_fails_schema},
        {"valid_spatial_mask_result_passes",
         &test_valid_spatial_mask_result_passes},
        {"spatial_mask_count_mismatch_fails_schema",
         &test_spatial_mask_count_mismatch_fails_schema},
        {"unknown_policy_generation_fails_schema",
         &test_unknown_policy_generation_fails_schema},
        {"active_policy_line_passes", &test_active_policy_line_passes},
        {"contradictory_policy_fails", &test_contradictory_policy_fails},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All YOLO event log validation tests passed.\n";
    return EXIT_SUCCESS;
}
