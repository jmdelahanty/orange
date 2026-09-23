#pragma once

#include "common.hpp"
#include "json.hpp"
#include "yolo_event_log_config.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yolo_event_log {

struct SpatialMaskOutsideDetection {
    int raw_index = -1;
    float x_px = 0.0f;
    float y_px = 0.0f;
    float width_px = 0.0f;
    float height_px = 0.0f;
    int label = 0;
    float confidence = 0.0f;
    float centroid_x_px = 0.0f;
    float centroid_y_px = 0.0f;
    float signed_boundary_distance_px = 0.0f;
    bool rejected = false;
};

struct SpatialMaskResult {
    std::shared_ptr<const nlohmann::json> policy;
    uint64_t policy_generation = 0;
    int raw_detection_count = 0;
    int inside_detection_count = 0;
    int outside_detection_count = 0;
    int downstream_detection_count = 0;
    std::vector<SpatialMaskOutsideDetection> outside_detections;
};

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
    std::string detection_source = "model";
    bool synthetic_runtime_detection = false;
    std::vector<pose::Object> detections;
    SpatialMaskResult spatial_mask;
    std::string queue_name;
    bool ipc_enabled = false;
    bool ipc_requested = false;
    std::string ipc_request_status = "not_enabled";
};

struct SyntheticYoloFrameInput {
    std::string recording_folder;
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t ipc_frame_id = 0;
    bool record_active = false;
    uint64_t camera_timestamp = 0;
    uint64_t timestamp_sys_ns = 0;
    int width = 0;
    int height = 0;
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
    // Control/finalization thread only. A FIFO flush alone is insufficient:
    // the detector may enqueue the last result after finalization requests it.
    // This waits for that source identity, then flushes on the existing writer.
    // Success is a visibility barrier, not a completeness proof (the master
    // correspondence validator still checks every row and rejects gaps).
    bool FlushThrough(const std::string& folder, uint64_t last_recording_frame_id,
                      std::chrono::milliseconds timeout);

private:
    enum class EventType {
        kYoloResult,
        kClose,
        kFlushThrough,
    };

    struct Event {
        EventType type = EventType::kYoloResult;
        YoloResultRecord result;
        std::shared_ptr<std::promise<bool>> completion;
        std::chrono::steady_clock::time_point deadline;
    };

    static constexpr size_t kMaxQueue = 8192;

    bool EnqueueEvent(Event&& event);
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
    bool writer_finished_ = false;
    std::string current_folder_;
    std::string recording_id_;
    std::string file_path_;
    std::ofstream file_;
    std::unordered_set<std::string> opened_folders_;
    std::unordered_map<std::string, uint64_t> next_sequence_by_folder_;
    // Writer-thread owned; never inspected from detector/acquisition threads.
    std::unordered_map<std::string, uint64_t> last_written_by_folder_;
    std::unordered_set<std::string> failed_folders_;
    size_t dropped_ = 0;
};

class SyntheticYoloEventEmitter {
public:
    SyntheticYoloEventEmitter(const std::string& camera_serial,
                              int camera_id,
                              int gpu_id,
                              const std::string& queue_name,
                              bool ipc_enabled,
                              SyntheticYoloEventConfig config);

    void EmitFrame(const SyntheticYoloFrameInput& frame);

private:
    pose::Object BuildDetection(uint64_t recording_frame_id, int width, int height) const;

    SyntheticYoloEventConfig config_;
    std::string camera_serial_;
    int camera_id_ = 0;
    int gpu_id_ = -1;
    std::string queue_name_;
    bool ipc_enabled_ = false;
    YoloEventLogger logger_;
};

}  // namespace yolo_event_log
