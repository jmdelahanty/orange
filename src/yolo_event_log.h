#pragma once

#include "common.hpp"

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

namespace yolo_event_log {

struct YoloResultRecord {
    std::string recording_folder;
    std::string status;
    std::string error;
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t ipc_frame_id = 0;
    bool record_active = false;
    uint64_t camera_timestamp = 0;
    uint64_t timestamp_sys_ns = 0;
    uint64_t event_epoch_us = 0;
    uint64_t event_monotonic_us = 0;
    int gpu_id = -1;
    std::string model_id = "unknown";
    std::string engine_path;
    std::vector<pose::Object> detections;
    std::string queue_name;
    bool ipc_enabled = false;
    bool ipc_requested = false;
    std::string ipc_request_status = "not_enabled";
};

class YoloEventLogger {
public:
    YoloEventLogger(const std::string& camera_serial,
                    int camera_id,
                    const std::string& worker_name);
    ~YoloEventLogger();

    void Stop();
    void Close();
    void Enqueue(YoloResultRecord record);

private:
    enum class EventType {
        kYoloResult,
        kClose,
    };

    struct Event {
        EventType type = EventType::kYoloResult;
        YoloResultRecord result;
    };

    static constexpr size_t kMaxQueue = 8192;

    void EnqueueEvent(Event&& event);
    static std::string RecordingIdFromFolder(const std::string& folder);
    void OpenFile(const std::string& folder);
    void CloseFile();
    void RotateIfNeeded(const std::string& folder);
    void WriteResult(const YoloResultRecord& record);
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

}  // namespace yolo_event_log
