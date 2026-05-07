#include "pose_event_log_validation.h"

#include "json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

struct TestDir {
    explicit TestDir(const std::string& name)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("orange_pose_event_log_validation_" + name + "_" + std::to_string(now));
        std::filesystem::create_directories(path);
    }

    ~TestDir()
    {
        std::filesystem::remove_all(path);
    }

    std::filesystem::path path;
};

pose_event_log::PoseEventLogValidationConfig noop_config()
{
    pose_event_log::PoseEventLogValidationConfig config;
    config.mode = "noop";
    return config;
}

nlohmann::json make_noop_event(uint64_t sequence, uint64_t recording_frame_id)
{
    return {
        {"schema_id", "orange.pose_event"},
        {"schema_version", 1},
        {"event_sequence", sequence},
        {"event_kind", "pose_result"},
        {"recording_id", "test"},
        {"camera_serial", "2010095"},
        {"camera_id", 2010095},
        {"frame", {
            {"local_frame_id", recording_frame_id + 100},
            {"camera_frame_id", recording_frame_id + 1000},
            {"recording_frame_id", recording_frame_id},
            {"record_active", true}
        }},
        {"timestamps", {
            {"camera_timestamp", 1234 + recording_frame_id},
            {"timestamp_sys_ns", 5678 + recording_frame_id},
            {"event_epoch_us", 9012 + recording_frame_id},
            {"event_monotonic_us", 3456 + recording_frame_id}
        }},
        {"source_frame", {
            {"width_px", 4512},
            {"height_px", 4512}
        }},
        {"crop", {
            {"coordinate_space", "source_frame_pixels"},
            {"x_px", 10},
            {"y_px", 20},
            {"width_px", 256},
            {"height_px", 256},
            {"blank_frame", false}
        }},
        {"detection", {
            {"has_detection", true},
            {"confidence", 0.9},
            {"x_px", 10.0},
            {"y_px", 20.0},
            {"width_px", 256.0},
            {"height_px", 256.0},
            {"coordinate_space", "source_frame_pixels"}
        }},
        {"pose", {
            {"status", "no_result"},
            {"backend", "noop"},
            {"mode", "noop"},
            {"model_id", "none"},
            {"engine_path", ""},
            {"skeleton_id", "unknown"},
            {"skeleton_path", ""},
            {"gpu_id", 5},
            {"coordinate_space", "crop_pixels"},
            {"instance_count", 0}
        }},
        {"poses", nlohmann::json::array()},
        {"latency_ms", {
            {"capture_to_detect_done", 1.0},
            {"detect_to_crop_worker_start", 0.1},
            {"crop_worker_start_to_crop_ready", 0.2},
            {"detect_to_crop_ready", 0.3},
            {"crop_ready_to_pose_start", 0.01},
            {"pose_start_to_pose_done", 0.0},
            {"capture_to_pose_done", 1.31}
        }}
    };
}

void write_meta_csv(const std::filesystem::path& folder)
{
    std::ofstream meta(folder / "Cam2010095_meta.csv");
    meta << "frame_id,timestamp,timestamp_sys\n";
    meta << "1,1235,5679\n";
    meta << "2,1236,5680\n";
}

void write_jsonl(const std::filesystem::path& folder, const std::vector<nlohmann::json>& rows)
{
    std::ofstream out(folder / "Cam2010095_pose_events.jsonl");
    for (const auto& row : rows) {
        out << row.dump() << '\n';
    }
}

bool test_noop_passes()
{
    TestDir dir("pass");
    write_meta_csv(dir.path);
    write_jsonl(dir.path, {make_noop_event(1, 1), make_noop_event(2, 2)});
    const auto stats = pose_event_log::summarize_pose_event_log(
        dir.path.string(),
        "2010095",
        noop_config());

    bool ok = true;
    ok &= require(stats.status == "pass", "noop pose log should pass");
    ok &= require(stats.present, "pose log is present");
    ok &= require(stats.rows == 2, "pose log row count");
    ok &= require(stats.no_result_rows == 2, "noop row count");
    ok &= require(stats.noop_errors == 0, "no noop errors");
    ok &= require(stats.metadata_join_misses == 0, "metadata join succeeds");
    return ok;
}

bool test_missing_log_fails()
{
    TestDir dir("missing");
    const auto stats = pose_event_log::summarize_pose_event_log(
        dir.path.string(),
        "2010095",
        noop_config());
    return require(stats.status == "missing", "missing pose log should report missing") &&
           require(!stats.present, "missing pose log is not present");
}

bool test_wrong_noop_fails()
{
    TestDir dir("wrong_noop");
    write_meta_csv(dir.path);
    nlohmann::json event = make_noop_event(1, 1);
    event["pose"]["backend"] = "tensorrt";
    write_jsonl(dir.path, {event});
    const auto stats = pose_event_log::summarize_pose_event_log(
        dir.path.string(),
        "2010095",
        noop_config());
    return require(stats.status == "fail", "non-noop row should fail noop validation") &&
           require(stats.noop_errors == 1, "noop error count");
}

bool test_sequence_fails()
{
    TestDir dir("sequence");
    write_meta_csv(dir.path);
    write_jsonl(dir.path, {make_noop_event(2, 1)});
    const auto stats = pose_event_log::summarize_pose_event_log(
        dir.path.string(),
        "2010095",
        noop_config());
    return require(stats.status == "fail", "bad sequence should fail") &&
           require(stats.sequence_errors == 1, "sequence error count");
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= test_noop_passes();
    ok &= test_missing_log_fails();
    ok &= test_wrong_noop_fails();
    ok &= test_sequence_fails();
    if (!ok) {
        return 1;
    }
    std::cout << "pose_event_log_validation_tests passed" << std::endl;
    return 0;
}
