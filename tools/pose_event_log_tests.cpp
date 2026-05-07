#include "pose_event_log.h"
#include "json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

std::filesystem::path unique_temp_dir()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("orange_pose_event_log_test_" + std::to_string(now));
}

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path)
{
    std::vector<nlohmann::json> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line));
    }
    return rows;
}

pose_event_log::PoseResultRecord make_record(const std::string& folder, uint64_t recording_frame_id)
{
    pose_event_log::PoseResultRecord record;
    record.recording_folder = folder;
    record.status = "no_result";
    record.backend = "noop";
    record.mode = "noop";
    record.model_id = "none";
    record.gpu_id = 5;
    record.local_frame_id = recording_frame_id + 100;
    record.camera_frame_id = recording_frame_id + 1000;
    record.recording_frame_id = recording_frame_id;
    record.record_active = true;
    record.camera_timestamp = 123456789 + recording_frame_id;
    record.timestamp_sys_ns = 987654321 + recording_frame_id;
    record.source_width = 4512;
    record.source_height = 4512;
    record.has_detection = true;
    record.detection_confidence = 0.92;
    record.detection_x_px = 100.0;
    record.detection_y_px = 200.0;
    record.detection_width_px = 300.0;
    record.detection_height_px = 320.0;
    record.crop_x_px = 90;
    record.crop_y_px = 190;
    record.crop_width_px = 328;
    record.crop_height_px = 328;
    record.timing.capture_to_detect_done_ms = 4.5;
    record.timing.detect_to_crop_ready_ms = 0.8;
    record.timing.crop_ready_to_pose_start_ms = 0.01;
    record.timing.pose_start_to_pose_done_ms = 0.0;
    record.timing.capture_to_pose_done_ms = 5.4;
    return record;
}

} // namespace

int main()
{
    const std::filesystem::path temp_dir = unique_temp_dir();
    std::filesystem::create_directories(temp_dir);

    {
        pose_event_log::PoseEventLogger logger("2010095", 2010095, "PoseEventLogTest");
        logger.Enqueue(make_record(temp_dir.string(), 1));
        logger.Enqueue(make_record(temp_dir.string(), 2));
        logger.Close();
        logger.Stop();
    }

    const std::filesystem::path jsonl_path = temp_dir / "Cam2010095_pose_events.jsonl";
    const std::vector<nlohmann::json> rows = read_jsonl(jsonl_path);

    bool ok = true;
    ok &= expect(rows.size() == 2, "pose event logger writes two rows");
    if (rows.size() >= 2) {
        ok &= expect(rows[0]["schema_id"] == "orange.pose_event", "schema id is orange.pose_event");
        ok &= expect(rows[0]["event_kind"] == "pose_result", "event kind is pose_result");
        ok &= expect(rows[0]["event_sequence"] == 1, "first event sequence is 1");
        ok &= expect(rows[1]["event_sequence"] == 2, "second event sequence is 2");
        ok &= expect(rows[0]["frame"]["recording_frame_id"] == 1, "recording frame id is preserved");
        ok &= expect(rows[0]["pose"]["backend"] == "noop", "backend is recorded");
        ok &= expect(rows[0]["pose"]["status"] == "no_result", "status is recorded");
        ok &= expect(rows[0]["pose"]["instance_count"] == 0, "empty pose list is explicit");
        ok &= expect(rows[0]["crop"]["width_px"] == 328, "crop width is recorded");
        ok &= expect(rows[0]["detection"]["has_detection"] == true, "detection state is recorded");
        ok &= expect(rows[0]["latency_ms"]["capture_to_pose_done"] == 5.4, "pose latency is recorded");
        ok &= expect(rows[0]["timestamps"]["event_epoch_us"].get<uint64_t>() > 0, "event epoch is filled");
    }

    std::filesystem::remove_all(temp_dir);
    if (!ok) {
        return 1;
    }
    std::cout << "pose_event_log_tests passed" << std::endl;
    return 0;
}
