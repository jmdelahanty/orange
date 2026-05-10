#include "yolo_event_log_validation.h"
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

nlohmann::json make_event(const uint64_t sequence,
                          const uint64_t recording_frame_id,
                          const yolo_event_log::SyntheticYoloEventConfig& config)
{
    const bool has_detection =
        recording_frame_id > 0 &&
        (recording_frame_id % static_cast<uint64_t>(config.every_n_frames)) == 0;
    nlohmann::json detections = nlohmann::json::array();
    if (has_detection) {
        detections.push_back({
            {"label", config.label},
            {"confidence", config.confidence},
            {"bbox_xywh_px", {10.0, 20.0, 30.0, 40.0}}
        });
    }

    return {
        {"schema_id", "orange.yolo_event"},
        {"schema_version", 1},
        {"event_kind", "yolo_result"},
        {"event_sequence", sequence},
        {"frame", {{"recording_frame_id", recording_frame_id}}},
        {"yolo", {
            {"status", has_detection ? "detections" : "zero_detections"},
            {"detection_count", has_detection ? 1 : 0}
        }},
        {"detections", detections}
    };
}

void write_metadata(const std::filesystem::path& recording_folder,
                    const uint64_t max_frame_id)
{
    std::ofstream file(recording_folder / ("Cam" + std::string(kCameraSerial) + "_meta.csv"));
    require(static_cast<bool>(file), "failed to create metadata fixture");
    file << "recording_frame_id,local_frame_id\n";
    for (uint64_t frame_id = 1; frame_id <= max_frame_id; ++frame_id) {
        file << frame_id << "," << frame_id << "\n";
    }
}

using EventMutator = std::function<void(uint64_t line_number, nlohmann::json* event)>;

void write_event_log(const std::filesystem::path& recording_folder,
                     const yolo_event_log::SyntheticYoloEventConfig& config,
                     const uint64_t frame_count,
                     const EventMutator& mutator = {},
                     const uint64_t malformed_line = 0)
{
    std::ofstream file(recording_folder / ("Cam" + std::string(kCameraSerial) + "_yolo_events.jsonl"));
    require(static_cast<bool>(file), "failed to create yolo event log fixture");
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
            (*event)["yolo"]["status"] = "detections";
            (*event)["yolo"]["detection_count"] = 1;
            (*event)["detections"] = nlohmann::json::array({{{"label", 0}}});
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
            (*event)["yolo"]["status"] = "detections";
            (*event)["yolo"]["detection_count"] = 1;
            (*event)["detections"] = nlohmann::json::array({{{"label", 0}}});
        } else if (line == 12) {
            (*event)["yolo"]["status"] = "timeout";
            (*event)["yolo"]["detection_count"] = 0;
            (*event)["detections"] = nlohmann::json::array();
        } else if (line == 13) {
            (*event)["yolo"]["status"] = "failed";
            (*event)["yolo"]["detection_count"] = 0;
            (*event)["detections"] = nlohmann::json::array();
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

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"valid_synthetic_log_passes", &test_valid_synthetic_log_passes},
        {"missing_log_reports_missing", &test_missing_log_reports_missing},
        {"sequence_error_fails", &test_sequence_error_fails},
        {"cadence_error_fails", &test_cadence_error_fails},
        {"metadata_join_miss_fails", &test_metadata_join_miss_fails},
        {"malformed_json_fails", &test_malformed_json_fails},
        {"unknown_status_fails_schema", &test_unknown_status_fails_schema},
        {"real_worker_log_skips_synthetic_cadence", &test_real_worker_log_skips_synthetic_cadence},
        {"runtime_synthetic_marker_mismatch_fails_schema",
         &test_runtime_synthetic_marker_mismatch_fails_schema},
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
