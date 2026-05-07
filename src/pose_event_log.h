#ifndef ORANGE_POSE_EVENT_LOG_H
#define ORANGE_POSE_EVENT_LOG_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose_event_log {

struct PoseKeypointRecord {
    std::string label;
    double x_px = 0.0;
    double y_px = 0.0;
    double confidence = 0.0;
    bool visible = true;
};

struct PoseInstanceRecord {
    int index = 0;
    std::string label;
    double confidence = 0.0;
    std::vector<PoseKeypointRecord> keypoints;
};

struct PoseTimingRecord {
    double capture_to_detect_done_ms = 0.0;
    double detect_to_crop_worker_start_ms = 0.0;
    double crop_worker_start_to_crop_ready_ms = 0.0;
    double detect_to_crop_ready_ms = 0.0;
    double crop_ready_to_pose_start_ms = 0.0;
    double pose_start_to_pose_done_ms = 0.0;
    double capture_to_pose_done_ms = 0.0;
};

struct PoseResultRecord {
    std::string recording_folder;
    std::string status = "no_result";
    std::string error;
    std::string backend = "noop";
    std::string mode = "noop";
    std::string model_id = "none";
    std::string engine_path;
    std::string skeleton_id = "unknown";
    std::string skeleton_path;
    int gpu_id = -1;

    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    bool record_active = false;
    uint64_t camera_timestamp = 0;
    uint64_t timestamp_sys_ns = 0;
    uint64_t event_epoch_us = 0;
    uint64_t event_monotonic_us = 0;

    int source_width = 0;
    int source_height = 0;
    bool blank_frame = false;
    bool has_detection = false;
    double detection_confidence = 0.0;
    double detection_x_px = 0.0;
    double detection_y_px = 0.0;
    double detection_width_px = 0.0;
    double detection_height_px = 0.0;
    int crop_x_px = 0;
    int crop_y_px = 0;
    int crop_width_px = 0;
    int crop_height_px = 0;

    PoseTimingRecord timing;
    std::vector<PoseInstanceRecord> poses;
};

class PoseEventLogger {
public:
    PoseEventLogger(const std::string& camera_serial,
                    int camera_id,
                    const std::string& worker_name);
    ~PoseEventLogger();

    void Stop();
    void Close();
    void Enqueue(PoseResultRecord record);

private:
    enum class EventType {
        kPoseResult,
        kClose,
    };

    struct Event {
        EventType type = EventType::kPoseResult;
        PoseResultRecord result;
    };

    static constexpr size_t kMaxQueue = 8192;

    void EnqueueEvent(Event&& event);
    static std::string RecordingIdFromFolder(const std::string& folder);
    void OpenFile(const std::string& folder);
    void CloseFile();
    void RotateIfNeeded(const std::string& folder);
    void WriteResult(PoseResultRecord record);
    void ThreadMain();

    std::string camera_serial_;
    int camera_id_ = 0;
    std::string worker_name_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    bool running_ = false;
    std::string current_folder_;
    std::string recording_id_;
    std::string file_path_;
    std::ofstream file_;
    std::unordered_set<std::string> opened_folders_;
    std::unordered_map<std::string, uint64_t> next_sequence_by_folder_;
    size_t dropped_ = 0;
};

} // namespace pose_event_log

#endif // ORANGE_POSE_EVENT_LOG_H
