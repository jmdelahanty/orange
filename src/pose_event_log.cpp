#include "pose_event_log.h"

#include "fsuid_guard.h"
#include "json.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <utility>

namespace pose_event_log {
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

nlohmann::json keypoint_to_json(const PoseKeypointRecord& keypoint)
{
    nlohmann::json out = {
        {"x_px", keypoint.x_px},
        {"y_px", keypoint.y_px},
        {"confidence", keypoint.confidence},
        {"visible", keypoint.visible}
    };
    if (!keypoint.label.empty()) {
        out["label"] = keypoint.label;
    }
    return out;
}

nlohmann::json pose_instance_to_json(const PoseInstanceRecord& pose)
{
    nlohmann::json keypoints = nlohmann::json::array();
    for (const PoseKeypointRecord& keypoint : pose.keypoints) {
        keypoints.push_back(keypoint_to_json(keypoint));
    }

    nlohmann::json out = {
        {"index", pose.index},
        {"confidence", pose.confidence},
        {"keypoints", std::move(keypoints)}
    };
    if (!pose.label.empty()) {
        out["label"] = pose.label;
    }
    return out;
}

} // namespace

PoseEventLogger::PoseEventLogger(const std::string& camera_serial,
                                 int camera_id,
                                 const std::string& worker_name)
    : camera_serial_(camera_serial),
      camera_id_(camera_id),
      worker_name_(worker_name),
      running_(true)
{
    thread_ = std::thread(&PoseEventLogger::ThreadMain, this);
}

PoseEventLogger::~PoseEventLogger()
{
    Stop();
}

void PoseEventLogger::Stop()
{
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

void PoseEventLogger::Close()
{
    Event event;
    event.type = EventType::kClose;
    EnqueueEvent(std::move(event));
}

void PoseEventLogger::Enqueue(PoseResultRecord record)
{
    if (record.recording_folder.empty()) {
        return;
    }
    Event event;
    event.type = EventType::kPoseResult;
    event.result = std::move(record);
    EnqueueEvent(std::move(event));
}

void PoseEventLogger::EnqueueEvent(Event&& event)
{
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

std::string PoseEventLogger::RecordingIdFromFolder(const std::string& folder)
{
    try {
        return std::filesystem::path(folder).filename().string();
    } catch (...) {
        const size_t pos = folder.find_last_of('/');
        return pos == std::string::npos ? folder : folder.substr(pos + 1);
    }
}

void PoseEventLogger::OpenFile(const std::string& folder)
{
    if (folder.empty()) {
        return;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        std::cerr << "[POSE_EVENT_LOG] " << worker_name_
                  << " failed to create " << folder << ": " << ec.message() << std::endl;
        return;
    }

    current_folder_ = folder;
    recording_id_ = RecordingIdFromFolder(folder);
    file_path_ = current_folder_ + "/Cam" + camera_serial_ + "_pose_events.jsonl";

    const bool first_open = opened_folders_.insert(current_folder_).second;
    const auto mode = std::ios::out | (first_open ? std::ios::trunc : std::ios::app);
    file_.open(file_path_, mode);
    if (!file_) {
        std::cerr << "[POSE_EVENT_LOG] " << worker_name_
                  << " failed to open " << file_path_ << std::endl;
        current_folder_.clear();
        recording_id_.clear();
        file_path_.clear();
        return;
    }
    std::cout << "[POSE_EVENT_LOG] " << worker_name_
              << " logging to " << file_path_ << std::endl;
}

void PoseEventLogger::CloseFile()
{
    if (file_.is_open()) {
        file_.close();
    }
    current_folder_.clear();
    recording_id_.clear();
    file_path_.clear();
}

void PoseEventLogger::RotateIfNeeded(const std::string& folder)
{
    if (folder == current_folder_ && file_.is_open()) {
        return;
    }
    CloseFile();
    OpenFile(folder);
}

void PoseEventLogger::WriteResult(PoseResultRecord record)
{
    if (record.recording_folder.empty()) {
        return;
    }
    RotateIfNeeded(record.recording_folder);
    if (!file_.is_open()) {
        return;
    }

    if (record.event_epoch_us == 0) {
        record.event_epoch_us = epoch_time_us();
    }
    if (record.event_monotonic_us == 0) {
        record.event_monotonic_us = steady_time_us();
    }

    uint64_t& next_sequence = next_sequence_by_folder_[record.recording_folder];
    if (next_sequence == 0) {
        next_sequence = 1;
    }

    nlohmann::json poses = nlohmann::json::array();
    for (const PoseInstanceRecord& pose : record.poses) {
        poses.push_back(pose_instance_to_json(pose));
    }

    nlohmann::json pose = {
        {"status", record.status},
        {"backend", record.backend},
        {"mode", record.mode},
        {"model_id", record.model_id},
        {"engine_path", record.engine_path},
        {"skeleton_id", record.skeleton_id},
        {"skeleton_path", record.skeleton_path},
        {"gpu_id", record.gpu_id},
        {"coordinate_space", "crop_pixels"},
        {"instance_count", static_cast<int>(record.poses.size())}
    };
    if (!record.error.empty()) {
        pose["error"] = record.error;
    }

    const nlohmann::json root = {
        {"schema_id", "orange.pose_event"},
        {"schema_version", 1},
        {"event_sequence", next_sequence++},
        {"event_kind", "pose_result"},
        {"recording_id", recording_id_},
        {"camera_serial", camera_serial_},
        {"camera_id", camera_id_},
        {"frame", {
            {"local_frame_id", record.local_frame_id},
            {"camera_frame_id", record.camera_frame_id},
            {"recording_frame_id", record.recording_frame_id},
            {"record_active", record.record_active}
        }},
        {"timestamps", {
            {"camera_timestamp", record.camera_timestamp},
            {"timestamp_sys_ns", record.timestamp_sys_ns},
            {"event_epoch_us", record.event_epoch_us},
            {"event_monotonic_us", record.event_monotonic_us}
        }},
        {"source_frame", {
            {"width_px", record.source_width},
            {"height_px", record.source_height}
        }},
        {"crop", {
            {"coordinate_space", "source_frame_pixels"},
            {"x_px", record.crop_x_px},
            {"y_px", record.crop_y_px},
            {"width_px", record.crop_width_px},
            {"height_px", record.crop_height_px},
            {"blank_frame", record.blank_frame}
        }},
        {"detection", {
            {"has_detection", record.has_detection},
            {"confidence", record.detection_confidence},
            {"x_px", record.detection_x_px},
            {"y_px", record.detection_y_px},
            {"width_px", record.detection_width_px},
            {"height_px", record.detection_height_px},
            {"coordinate_space", "source_frame_pixels"}
        }},
        {"pose", std::move(pose)},
        {"poses", std::move(poses)},
        {"latency_ms", {
            {"capture_to_detect_done", record.timing.capture_to_detect_done_ms},
            {"detect_to_crop_worker_start", record.timing.detect_to_crop_worker_start_ms},
            {"crop_worker_start_to_crop_ready", record.timing.crop_worker_start_to_crop_ready_ms},
            {"detect_to_crop_ready", record.timing.detect_to_crop_ready_ms},
            {"crop_ready_to_pose_start", record.timing.crop_ready_to_pose_start_ms},
            {"pose_start_to_pose_done", record.timing.pose_start_to_pose_done_ms},
            {"capture_to_pose_done", record.timing.capture_to_pose_done_ms}
        }}
    };

    file_ << root.dump() << '\n';
}

void PoseEventLogger::ThreadMain()
{
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
            case EventType::kPoseResult:
                WriteResult(std::move(event.result));
                break;
        }

        lock.lock();
    }
    CloseFile();
    if (dropped_ > 0) {
        std::cerr << "[POSE_EVENT_LOG] " << worker_name_
                  << " dropped " << dropped_ << " events" << std::endl;
    }
}

} // namespace pose_event_log
