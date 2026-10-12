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

// orange.pose_event v3 lines (the shapes of tools/fixtures/generated_outputs/golden).
nlohmann::json make_noop_header()
{
    return {
        {"schema_id", "orange.pose_event"}, {"schema_version", 3}, {"event_kind", "session_header"},
        {"recording_id", "test"}, {"camera_serial", "2010095"}, {"camera_id", 2}, {"worker", "PoseWorker"},
        {"frame_identity_key", "frame.recording_frame_id"},
        {"source_frame", {{"width_px", 4512}, {"height_px", 4512}}},
        {"line_format", {{"confidence_decimals", 4}, {"coordinate_decimals", 3}, {"latency_decimals", 3}}},
        {"pose", {{"worker", "PoseWorker"}, {"backend", "noop"}, {"mode", "noop"}, {"model_id", "none"},
                  {"engine_path", ""}, {"engine_sha256", ""}, {"engine_bytes", 0}, {"worker_engine_sha256", ""},
                  {"weights_sha256", ""}, {"onnx_sha256", ""}, {"engine_manifest_run_id", ""},
                  {"ipc_model_id_hash", 0}, {"skeleton_id", "unknown"}, {"skeleton_path", ""},
                  {"skeleton_sha256", ""}, {"ipc_skeleton_id_hash", 0}, {"keypoint_labels", nlohmann::json::array()},
                  {"gpu_id", 5}, {"coordinate_space", "crop_pixels"}}},
        {"crop", {{"coordinate_space", "source_frame_pixels"}, {"note", "crop note"}}},
        {"detection", {{"coordinate_space", "source_frame_pixels"}, {"note", "detection note"}}}
    };
}

nlohmann::json make_noop_event(uint64_t sequence, uint64_t recording_frame_id)
{
    return {
        {"schema_id", "orange.pose_event"}, {"schema_version", 3}, {"event_kind", "pose_result"},
        {"event_sequence", sequence}, {"camera_serial", "2010095"},
        {"frame", {
            {"stream_frame_id", recording_frame_id + 100},
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
        {"crop", {{"x_px", 10}, {"y_px", 20}, {"width_px", 256}, {"height_px", 256}, {"blank_frame", false}}},
        {"detection", {{"has_detection", true}, {"confidence", 0.9}}},
        {"pose", {{"status", "no_result"}, {"instance_count", 0}}},
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
    meta << "recording_frame_id,timestamp,timestamp_sys\n";
    meta << "1,1235,5679\n";
    meta << "2,1236,5680\n";
}

// Writes `header` (unless null) and then the frame lines.
void write_jsonl(const std::filesystem::path& folder, const std::vector<nlohmann::json>& rows,
                 const nlohmann::json& header = make_noop_header())
{
    std::ofstream out(folder / "Cam2010095_pose_events.jsonl");
    if (!header.is_null()) {
        out << header.dump() << '\n';
    }
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
    nlohmann::json header = make_noop_header();
    header["pose"]["backend"] = "tensorrt";
    write_jsonl(dir.path, {make_noop_event(1, 1)}, header);
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

bool test_crop_metadata_source()
{
    TestDir dir("crop_metadata_source");
    write_meta_csv(dir.path); write_jsonl(dir.path, {make_noop_event(1, 1)});
    const auto crop = dir.path / "Cam2010095_crop_meta.csv";
    std::filesystem::rename(dir.path / "Cam2010095_meta.csv", crop);
    auto stats = pose_event_log::summarize_pose_event_log(dir.path.string(), "2010095", noop_config(), crop);
    bool ok = require(stats.status == "pass" && stats.metadata_rows > 0, "crop-only pose join required full-frame metadata");
    write_jsonl(dir.path, {make_noop_event(1, 9000)});
    stats = pose_event_log::summarize_pose_event_log(dir.path.string(), "2010095", noop_config(), crop);
    ok &= require(stats.status == "fail" && stats.metadata_join_misses == 1, "crop source pose join ignored missing parent frame");
    std::filesystem::remove(crop);
    stats = pose_event_log::summarize_pose_event_log(dir.path.string(), "2010095", noop_config(), crop);
    return require(stats.status == "fail", "missing explicit source silently skipped pose join") && ok;
}
}  // namespace

bool test_header_and_strictness()
{
    bool ok = true;
    TestDir no_header("no_header");
    write_meta_csv(no_header.path);
    write_jsonl(no_header.path, {make_noop_event(1, 1)}, nullptr);
    const auto bad = pose_event_log::summarize_pose_event_log(no_header.path.string(), "2010095", noop_config());
    ok &= require(bad.status == "fail" && bad.schema_errors > 0, "a frame line without a header fails");

    TestDir version("version");
    write_meta_csv(version.path);
    nlohmann::json old = make_noop_event(2, 2);
    old["schema_version"] = 2;
    write_jsonl(version.path, {make_noop_event(1, 1), old});
    const auto v2 = pose_event_log::summarize_pose_event_log(version.path.string(), "2010095", noop_config());
    ok &= require(v2.status == "fail" && v2.schema_errors == 1 &&
                  v2.error.find("schema_version") != std::string::npos, "a v2 line in a v3 log fails, named");

    TestDir count("count");
    write_meta_csv(count.path);
    nlohmann::json miscounted = make_noop_event(1, 1);
    miscounted["pose"]["instance_count"] = 1;
    write_jsonl(count.path, {miscounted});
    const auto c = pose_event_log::summarize_pose_event_log(count.path.string(), "2010095", noop_config());
    ok &= require(c.status == "fail" && c.schema_errors == 1, "instance_count must equal the poses listed");
    return ok;
}

int main()
{
    bool ok = true;
    ok &= test_crop_metadata_source();
    ok &= test_noop_passes();
    ok &= test_header_and_strictness();
    ok &= test_missing_log_fails();
    ok &= test_wrong_noop_fails();
    ok &= test_sequence_fails();
    if (!ok) {
        return 1;
    }
    std::cout << "pose_event_log_validation_tests passed" << std::endl;
    return 0;
}
