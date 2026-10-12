#include "pose_event_log.h"
#include "orange_outputs/orange_pose_event_v3.h"
#include "timing/log_writer.h"
#include "timing/clock.h"
#include "event_log_format.h"
#include "model_identity.h"

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
    return orange::timing::realtime_now_us();
}

uint64_t steady_time_us()
{
    return orange::timing::monotonic_now_us();
}

namespace ev = orange::outputs::gen::pose_event_v3;

ev::OrangePoseEventV3PoseResultPosesItem pose_instance(const PoseInstanceRecord& pose)
{
    // v2: labels live in the session header (pose.keypoint_labels), in keypoint
    // order; coordinates are crop pixels rounded to 0.001 px.
    ev::OrangePoseEventV3PoseResultPosesItem out;
    out.index = pose.index;
    out.confidence = event_log_format::round_confidence(pose.confidence);
    if (!pose.label.empty()) {
        out.label = pose.label;
    }
    for (const PoseKeypointRecord& keypoint : pose.keypoints) {
        ev::OrangePoseEventV3PoseResultPosesItemKeypointsItem kp;
        kp.x_px = event_log_format::round_px(keypoint.x_px);
        kp.y_px = event_log_format::round_px(keypoint.y_px);
        kp.confidence = event_log_format::round_confidence(keypoint.confidence);
        kp.visible = keypoint.visible;
        out.keypoints.push_back(kp);
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
    orange::timing::LogWriter::Instance().RegisterFlusher(
        this, [this](std::chrono::milliseconds timeout, const std::string& seal_folder) {
            return Flush(timeout, seal_folder);
        });
}

PoseEventLogger::~PoseEventLogger()
{
    Stop();
}

void PoseEventLogger::Stop()
{
    orange::timing::LogWriter::Instance().UnregisterFlusher(this);
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

bool PoseEventLogger::Flush(std::chrono::milliseconds timeout, const std::string& seal_folder)
{
    Event event;
    event.type = EventType::kFlush;
    event.seal_folder = seal_folder;
    event.completion = std::make_shared<std::promise<bool>>();
    auto result = event.completion->get_future();
    {
        // A barrier is never dropped for a full queue.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return true;  // the writer has stopped: its file is closed
        }
        queue_.push_back(std::move(event));
    }
    cv_.notify_one();
    return result.wait_for(timeout) == std::future_status::ready && result.get();
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
    bool sealed = false;
    for (const std::string& sealed_folder : sealed_folders_) {
        sealed = sealed || orange::timing::LogWriter::PathUnder(file_path_, sealed_folder);
    }
    if (sealed) {
        // Finalize already digested this folder: a late row must not change
        // the sealed file.
        std::cerr << "[POSE_EVENT_LOG] " << worker_name_ << " not reopening " << file_path_
                  << " (folder sealed by finalize)" << std::endl;
        current_folder_.clear();
        recording_id_.clear();
        file_path_.clear();
        return;
    }

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

void PoseEventLogger::WriteSessionHeaderIfNeeded(const PoseResultRecord& record)
{
    if (!header_written_folders_.insert(record.recording_folder).second) {
        return;
    }
    // Model identity from the engine bytes and the engine manifest (cached by
    // model_identity; the recording snapshot resolved the same path already).
    // The worker's own digest is reported beside it so a disagreement shows.
    const orange::model_identity::ModelIdentity identity =
        orange::model_identity::resolve_model_identity(record.engine_path);
    ev::OrangePoseEventV3SessionHeader header;
    header.recording_id = recording_id_;
    header.camera_serial = camera_serial_;
    header.camera_id = camera_id_;
    header.worker = worker_name_;
    header.source_frame.width_px = static_cast<std::uint64_t>(record.source_width);
    header.source_frame.height_px = static_cast<std::uint64_t>(record.source_height);
    ev::OrangePoseEventV3SessionHeaderPose& pose = header.pose;
    pose.backend = record.backend;
    pose.mode = record.mode;
    pose.model_id = record.model_id;
    pose.engine_path = record.engine_path;
    pose.engine_sha256 = identity.engine_sha256.empty() ? record.engine_sha256 : identity.engine_sha256;
    pose.engine_bytes = identity.engine_bytes;
    pose.worker_engine_sha256 = record.engine_sha256;
    pose.weights_sha256 = identity.weights_sha256;
    pose.onnx_sha256 = identity.onnx_sha256;
    pose.engine_manifest_run_id = identity.run_id;
    pose.ipc_model_id_hash = record.ipc_model_id_hash;
    pose.skeleton_id = record.skeleton_id;
    pose.skeleton_path = record.skeleton_path;
    pose.skeleton_sha256 = record.skeleton_sha256;
    pose.ipc_skeleton_id_hash = record.ipc_skeleton_id_hash;
    pose.keypoint_labels = record.keypoint_labels;
    pose.gpu_id = record.gpu_id;
    header.crop.note = "crop x_px/y_px/width_px/height_px on each frame line; keypoints are crop pixels relative to that origin";
    header.detection.note = "frame lines carry has_detection and confidence only; the box is on the same frame's yolo_result line and in the crop ledger";
    file_ << ev::to_json(header).dump() << '\n';
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

    // Producers stamp event time at result time; this fallback only covers
    // records built without one (tests, synthetic emitters).
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
    WriteSessionHeaderIfNeeded(record);

    using event_log_format::round_confidence;
    using event_log_format::round_ms;

    ev::OrangePoseEventV3PoseResult line;
    line.event_sequence = next_sequence++;
    line.camera_serial = camera_serial_;
    line.frame.stream_frame_id = record.local_frame_id;
    line.frame.camera_frame_id = record.camera_frame_id;
    line.frame.recording_frame_id = record.recording_frame_id;
    line.frame.record_active = record.record_active;
    line.timestamps.camera_timestamp = record.camera_timestamp;
    line.timestamps.timestamp_sys_ns = record.timestamp_sys_ns;
    line.timestamps.event_epoch_us = record.event_epoch_us;
    line.timestamps.event_monotonic_us = record.event_monotonic_us;
    line.crop.x_px = record.crop_x_px;
    line.crop.y_px = record.crop_y_px;
    line.crop.width_px = record.crop_width_px;
    line.crop.height_px = record.crop_height_px;
    line.crop.blank_frame = record.blank_frame;
    line.detection.has_detection = record.has_detection;
    line.detection.confidence = round_confidence(record.detection_confidence);
    line.pose.status = record.status;
    line.pose.instance_count = record.poses.size();
    if (!record.error.empty()) {
        line.pose.error = record.error;
    }
    for (const PoseInstanceRecord& pose : record.poses) {
        line.poses.push_back(pose_instance(pose));
    }
    const PoseTimingRecord& t = record.timing;
    line.latency_ms.capture_to_detect_done = round_ms(t.capture_to_detect_done_ms);
    line.latency_ms.detect_to_crop_worker_start = round_ms(t.detect_to_crop_worker_start_ms);
    line.latency_ms.crop_worker_start_to_crop_ready = round_ms(t.crop_worker_start_to_crop_ready_ms);
    line.latency_ms.detect_to_crop_ready = round_ms(t.detect_to_crop_ready_ms);
    line.latency_ms.crop_ready_to_pose_start = round_ms(t.crop_ready_to_pose_start_ms);
    line.latency_ms.pose_start_to_pose_done = round_ms(t.pose_start_to_pose_done_ms);
    line.latency_ms.capture_to_pose_done = round_ms(t.capture_to_pose_done_ms);

    file_ << ev::to_json(line).dump() << '\n';
    ++rows_written_;
    ++rows_since_flush_;
    MaybeFlush(false);
}

// Bound what a crash can lose to about a second of rows: flush after one
// second of buffered rows, or when the queue goes idle with rows pending.
void PoseEventLogger::MaybeFlush(bool force)
{
    if (rows_since_flush_ == 0 || !file_.is_open()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (force || now - last_flush_ >= std::chrono::seconds(1)) {
        file_.flush();
        last_flush_ = now;
        rows_since_flush_ = 0;
        ++flushes_;
    }
}

void PoseEventLogger::ThreadMain()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_ || !queue_.empty()) {
        if (queue_.empty()) {
            // Idle: flush on the one-second cadence (not per row: at 100 fps
            // the queue is empty between rows), and after a full second of
            // silence flush whatever is pending.
            const bool timed_out =
                cv_.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout;
            if (rows_since_flush_ > 0) {
                lock.unlock();
                MaybeFlush(timed_out);
                lock.lock();
            }
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
            case EventType::kFlush: {
                MaybeFlush(true);
                const bool ok = !file_.is_open() || static_cast<bool>(file_);
                if (file_.is_open() && !event.seal_folder.empty() &&
                    orange::timing::LogWriter::PathUnder(file_path_, event.seal_folder)) {
                    CloseFile();  // finalize is about to digest it
                }
                if (!event.seal_folder.empty()) sealed_folders_.push_back(event.seal_folder);
                event.completion->set_value(ok);
                break;
            }
        }

        lock.lock();
    }
    CloseFile();
    std::cout << "[POSE_EVENT_LOG] " << worker_name_ << " wrote " << rows_written_
              << " rows, flushes " << flushes_ << ", dropped " << dropped_ << std::endl;
    if (dropped_ > 0) {
        std::cerr << "[POSE_EVENT_LOG] " << worker_name_
                  << " dropped " << dropped_ << " events" << std::endl;
    }
}

} // namespace pose_event_log
