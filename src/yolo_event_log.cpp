#include "yolo_event_log.h"

#include "fsuid_guard.h"
#include "json.hpp"
#include "project.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <utility>

namespace yolo_event_log {

namespace {

uint64_t epoch_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

uint64_t steady_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

YoloEventLogger::YoloEventLogger(const std::string& camera_serial,
                                 int camera_id,
                                 const std::string& worker_name)
    : camera_serial_(camera_serial),
      camera_id_(camera_id),
      worker_name_(worker_name),
      running_(true) {
    thread_ = std::thread(&YoloEventLogger::ThreadMain, this);
}

YoloEventLogger::~YoloEventLogger() {
    Stop();
}

void YoloEventLogger::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void YoloEventLogger::Close() {
    Event event;
    event.type = EventType::kClose;
    EnqueueEvent(std::move(event));
}

void YoloEventLogger::Enqueue(YoloResultRecord record) {
    if (record.recording_folder.empty()) {
        return;
    }
    Event event;
    event.type = EventType::kYoloResult;
    event.result = std::move(record);
    EnqueueEvent(std::move(event));
}

void YoloEventLogger::EnqueueEvent(Event&& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    if (queue_.size() >= kMaxQueue) {
        dropped_++;
        return;
    }
    queue_.push_back(std::move(event));
    cv_.notify_one();
}

std::string YoloEventLogger::RecordingIdFromFolder(const std::string& folder) {
    try {
        return std::filesystem::path(folder).filename().string();
    } catch (...) {
        const size_t pos = folder.find_last_of('/');
        return pos == std::string::npos ? folder : folder.substr(pos + 1);
    }
}

void YoloEventLogger::OpenFile(const std::string& folder) {
    if (folder.empty()) {
        return;
    }
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    make_folder(folder);
    current_folder_ = folder;
    recording_id_ = RecordingIdFromFolder(folder);
    file_path_ = current_folder_ + "/Cam" + camera_serial_ + "_yolo_events.jsonl";

    const bool first_open = opened_folders_.insert(current_folder_).second;
    const auto mode = std::ios::out | (first_open ? std::ios::trunc : std::ios::app);
    file_.open(file_path_, mode);
    if (!file_) {
        std::cerr << "[YOLO_EVENT_LOG] " << worker_name_
                  << " failed to open " << file_path_ << std::endl;
        current_folder_.clear();
        recording_id_.clear();
        file_path_.clear();
        return;
    }
    std::cout << "[YOLO_EVENT_LOG] " << worker_name_
              << " logging to " << file_path_ << std::endl;
}

void YoloEventLogger::CloseFile() {
    if (file_.is_open()) {
        file_.close();
    }
    current_folder_.clear();
    recording_id_.clear();
    file_path_.clear();
}

void YoloEventLogger::RotateIfNeeded(const std::string& folder) {
    if (folder == current_folder_ && file_.is_open()) {
        return;
    }
    CloseFile();
    OpenFile(folder);
}

void YoloEventLogger::WriteResult(const YoloResultRecord& record) {
    if (record.recording_folder.empty()) {
        return;
    }
    RotateIfNeeded(record.recording_folder);
    if (!file_.is_open()) {
        return;
    }

    uint64_t& next_sequence = next_sequence_by_folder_[record.recording_folder];
    if (next_sequence == 0) {
        next_sequence = 1;
    }

    nlohmann::json detections = nlohmann::json::array();
    for (size_t i = 0; i < record.detections.size(); ++i) {
        const pose::Object& detection = record.detections[i];
        nlohmann::json keypoints = nlohmann::json::array();
        const size_t keypoint_count = std::min<size_t>(
            detection.num_kps,
            static_cast<size_t>(pose::MAX_KEYPOINTS));
        for (size_t k = 0; k < keypoint_count; ++k) {
            keypoints.push_back(detection.kps[k]);
        }
        detections.push_back({
            {"index", static_cast<int>(i)},
            {"x_px", detection.rect.x},
            {"y_px", detection.rect.y},
            {"width_px", detection.rect.width},
            {"height_px", detection.rect.height},
            {"label", detection.label},
            {"confidence", detection.prob},
            {"keypoints", std::move(keypoints)}
        });
    }

    nlohmann::json yolo = {
        {"status", record.status},
        {"detection_count", static_cast<int>(record.detections.size())},
        {"coordinate_space", "source_frame_pixels"},
        {"model_id", record.model_id},
        {"engine_path", record.engine_path},
        {"gpu_id", record.gpu_id},
        {"detection_source", record.detection_source},
        {"synthetic_runtime_detection", record.synthetic_runtime_detection},
        {"production_detection_valid", !record.synthetic_runtime_detection}
    };
    if (!record.error.empty()) {
        yolo["error"] = record.error;
    }

    nlohmann::json root = {
        {"schema_id", "orange.yolo_event"},
        {"schema_version", 1},
        {"event_sequence", next_sequence++},
        {"event_kind", "yolo_result"},
        {"recording_id", recording_id_},
        {"camera_serial", camera_serial_},
        {"camera_id", camera_id_},
        {"frame", {
            {"local_frame_id", record.local_frame_id},
            {"camera_frame_id", record.camera_frame_id},
            {"recording_frame_id", record.recording_frame_id},
            {"ipc_frame_id", record.ipc_frame_id},
            {"record_active", record.record_active}
        }},
        {"timestamps", {
            {"camera_timestamp", record.camera_timestamp},
            {"timestamp_sys_ns", record.timestamp_sys_ns},
            {"event_epoch_us", record.event_epoch_us},
            {"event_monotonic_us", record.event_monotonic_us}
        }},
        {"yolo", std::move(yolo)},
        {"detections", std::move(detections)},
        {"citrus_live_ipc", {
            {"queue_name", record.queue_name},
            {"enabled", record.ipc_enabled},
            {"requested", record.ipc_requested},
            {"request_status", record.ipc_request_status}
        }}
    };

    file_ << root.dump() << '\n';
}

void YoloEventLogger::ThreadMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_ || !queue_.empty()) {
        if (queue_.empty()) {
            cv_.wait(lock);
            continue;
        }
        Event event = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();

        switch (event.type) {
            case EventType::kClose:
                CloseFile();
                break;
            case EventType::kYoloResult:
                WriteResult(event.result);
                break;
        }

        lock.lock();
    }
    CloseFile();
    if (dropped_ > 0) {
        std::cerr << "[YOLO_EVENT_LOG] " << worker_name_
                  << " dropped " << dropped_ << " events" << std::endl;
    }
}

SyntheticYoloEventEmitter::SyntheticYoloEventEmitter(
    const std::string& camera_serial,
    int camera_id,
    int gpu_id,
    const std::string& queue_name,
    bool ipc_enabled,
    SyntheticYoloEventConfig config)
    : config_(std::move(config)),
      camera_serial_(camera_serial),
      camera_id_(camera_id),
      gpu_id_(gpu_id),
      queue_name_(queue_name),
      ipc_enabled_(ipc_enabled),
      logger_(camera_serial, camera_id, "SyntheticYOLO_Cam_" + camera_serial)
{
}

pose::Object SyntheticYoloEventEmitter::BuildDetection(
    uint64_t recording_frame_id,
    int width,
    int height) const
{
    const float box_width = 40.0f;
    const float box_height = 30.0f;
    const float max_x = std::max(0.0f, static_cast<float>(width) - box_width);
    const float max_y = std::max(0.0f, static_cast<float>(height) - box_height);
    pose::Object detection{};
    detection.rect.x = std::min(100.0f + static_cast<float>(recording_frame_id % 50), max_x);
    detection.rect.y = std::min(200.0f, max_y);
    detection.rect.width = std::min(box_width, std::max(0.0f, static_cast<float>(width)));
    detection.rect.height = std::min(box_height, std::max(0.0f, static_cast<float>(height)));
    detection.label = config_.label;
    detection.prob = static_cast<float>(config_.confidence);
    detection.num_kps = 0;
    return detection;
}

void SyntheticYoloEventEmitter::EmitFrame(const SyntheticYoloFrameInput& frame)
{
    if (!config_.enabled() ||
        frame.recording_folder.empty() ||
        frame.recording_frame_id == 0) {
        return;
    }

    const int cadence = std::max(1, config_.every_n_frames);
    const bool has_detection = (frame.recording_frame_id % static_cast<uint64_t>(cadence)) == 0;
    if (!has_detection && !config_.emit_zero_detections) {
        return;
    }

    YoloResultRecord record;
    record.recording_folder = frame.recording_folder;
    record.status = has_detection ? "detections" : "zero_detections";
    record.local_frame_id = frame.local_frame_id;
    record.camera_frame_id = frame.camera_frame_id;
    record.recording_frame_id = frame.recording_frame_id;
    record.ipc_frame_id = frame.ipc_frame_id;
    record.record_active = frame.record_active;
    record.camera_timestamp = frame.camera_timestamp;
    record.timestamp_sys_ns = frame.timestamp_sys_ns;
    record.event_epoch_us = epoch_time_us();
    record.event_monotonic_us = steady_time_us();
    record.gpu_id = gpu_id_;
    record.model_id = "synthetic_headless_v1";
    record.engine_path = "synthetic";
    record.queue_name = queue_name_;
    record.ipc_enabled = ipc_enabled_;
    record.ipc_requested = false;
    if (!ipc_enabled_) {
        record.ipc_request_status = "not_enabled";
    } else if (has_detection) {
        record.ipc_request_status = "not_requested_synthetic";
    } else {
        record.ipc_request_status = "not_requested_zero_detections";
    }
    if (has_detection) {
        record.detections.push_back(BuildDetection(
            frame.recording_frame_id,
            frame.width,
            frame.height));
    }
    logger_.Enqueue(std::move(record));
}

}  // namespace yolo_event_log
