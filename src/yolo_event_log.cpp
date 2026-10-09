#include "yolo_event_log.h"
#include "event_log_format.h"
#include "model_identity.h"

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
    cv_.notify_all();
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

bool YoloEventLogger::FlushThrough(const std::string& folder, uint64_t last_id,
                                 std::chrono::milliseconds timeout) {
    if (folder.empty() || last_id == 0 || timeout.count() <= 0) return false;
    Event event;
    event.type = EventType::kFlushThrough;
    event.result.recording_folder = folder;
    event.result.recording_frame_id = last_id;
    event.deadline = std::chrono::steady_clock::now() + timeout;
    const auto deadline = event.deadline;
    event.completion = std::make_shared<std::promise<bool>>();
    auto result = event.completion->get_future();
    if (!EnqueueEvent(std::move(event))) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (running_ || !cv_.wait_until(lock, deadline, [&] { return writer_finished_; })) return false;
        const auto found = last_written_by_folder_.find(folder);
        return found != last_written_by_folder_.end() && found->second >= last_id && !failed_folders_.count(folder);
    }
    return result.wait_until(deadline) == std::future_status::ready && result.get();
}

bool YoloEventLogger::EnqueueEvent(Event&& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return false;
    }
    if (queue_.size() >= kMaxQueue) {
        dropped_++;
        return false;
    }
    queue_.push_back(std::move(event));
    cv_.notify_one();
    return true;
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
        failed_folders_.insert(folder);
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
        if (file_.fail()) failed_folders_.insert(current_folder_);
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

namespace {

nlohmann::json spatial_mask_policy_json(const YoloResultRecord& record)
{
    nlohmann::json spatial_mask = record.spatial_mask.policy
        ? *record.spatial_mask.policy
        : nlohmann::json{
            {"schema_id", "orange.analytics.spatial_mask_runtime"},
            {"schema_version", 1},
            {"mode", "off"},
            {"input_mask", {
                {"enabled", false},
                {"outside_tensor_value", 0.0},
                {"input_context_outset_px", 0.0},
                {"geometry", nlohmann::json::object()},
            }},
            {"centroid_gate", {
                {"evaluated", false},
                {"enforced", false},
                {"geometry", nlohmann::json::object()},
            }},
            {"source", nlohmann::json::object()},
        };
    spatial_mask["policy_generation"] = record.spatial_mask.policy_generation;
    return spatial_mask;
}

}  // namespace

void YoloEventLogger::WriteSessionHeaderIfNeeded(const YoloResultRecord& record)
{
    if (!header_written_folders_.insert(record.recording_folder).second) {
        return;
    }
    // Model identity: the engine bytes hashed once per path (cached by
    // model_identity, already resolved by the recording snapshot) plus the
    // weights -> onnx chain from the engine manifest. Writer thread only.
    const bool real_engine = !record.engine_path.empty() && record.engine_path != "synthetic";
    const orange::model_identity::ModelIdentity identity =
        orange::model_identity::resolve_model_identity(real_engine ? record.engine_path : std::string());
    const nlohmann::json root = {
        {"schema_id", event_log_format::kYoloEventSchemaId},
        {"schema_version", event_log_format::kYoloEventSchemaVersion},
        {"event_kind", "session_header"},
        {"recording_id", recording_id_},
        {"camera_serial", camera_serial_},
        {"camera_id", camera_id_},
        {"worker", worker_name_},
        {"frame_identity_key", "frame.recording_frame_id"},
        {"source_frame", {
            {"width_px", record.source_width},
            {"height_px", record.source_height}
        }},
        {"yolo", {
            {"worker", "YoloWorker"},
            {"model_id", record.model_id},
            {"engine_path", record.engine_path},
            {"engine_sha256", identity.engine_sha256},
            {"engine_bytes", identity.engine_bytes},
            {"weights_sha256", identity.weights_sha256},
            {"onnx_sha256", identity.onnx_sha256},
            {"engine_manifest_run_id", identity.run_id},
            {"gpu_id", record.gpu_id},
            {"coordinate_space", "source_frame_pixels"},
            {"detection_source", record.detection_source},
            {"synthetic_runtime_detection", record.synthetic_runtime_detection}
        }},
        {"spatial_mask", spatial_mask_policy_json(record)},
        {"citrus_live_ipc", {
            {"queue_name", record.queue_name},
            {"enabled", record.ipc_enabled}
        }},
        {"line_format", event_log_format::line_format_json()}
    };
    last_policy_generation_by_folder_[record.recording_folder] = record.spatial_mask.policy_generation;
    file_ << root.dump() << '\n';
}

void YoloEventLogger::WriteSpatialMaskPolicyIfChanged(const YoloResultRecord& record)
{
    uint64_t& last = last_policy_generation_by_folder_[record.recording_folder];
    if (last == record.spatial_mask.policy_generation) {
        return;
    }
    last = record.spatial_mask.policy_generation;
    const nlohmann::json root = {
        {"schema_id", event_log_format::kYoloEventSchemaId},
        {"schema_version", event_log_format::kYoloEventSchemaVersion},
        {"event_kind", "spatial_mask_policy"},
        {"camera_serial", camera_serial_},
        {"frame", {
            {"recording_frame_id", record.recording_frame_id}
        }},
        {"spatial_mask", spatial_mask_policy_json(record)}
    };
    file_ << root.dump() << '\n';
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
    // Header and policy lines carry no event_sequence: the counter numbers
    // frame lines only (1..N), so readers that pair frame line i with crop
    // row i keep working once they skip the non-frame kinds.
    WriteSessionHeaderIfNeeded(record);
    WriteSpatialMaskPolicyIfChanged(record);

    using event_log_format::round_confidence;
    using event_log_format::round_px;

    nlohmann::json detections = nlohmann::json::array();
    for (size_t i = 0; i < record.detections.size(); ++i) {
        const pose::Object& detection = record.detections[i];
        nlohmann::json item = {
            {"index", static_cast<int>(i)},
            {"x_px", round_px(detection.rect.x)},
            {"y_px", round_px(detection.rect.y)},
            {"width_px", round_px(detection.rect.width)},
            {"height_px", round_px(detection.rect.height)},
            {"label", detection.label},
            {"confidence", round_confidence(detection.prob)}
        };
        // Detect-only engines emit no keypoints; the array is present only
        // when a keypoint model wrote some.
        const size_t keypoint_count = std::min<size_t>(
            detection.num_kps,
            static_cast<size_t>(pose::MAX_KEYPOINTS));
        if (keypoint_count > 0) {
            nlohmann::json keypoints = nlohmann::json::array();
            for (size_t k = 0; k < keypoint_count; ++k) {
                keypoints.push_back(round_px(detection.kps[k]));
            }
            item["keypoints"] = std::move(keypoints);
        }
        detections.push_back(std::move(item));
    }

    nlohmann::json yolo = {
        {"status", record.status},
        {"detection_count", static_cast<int>(record.detections.size())},
        {"detection_source", record.detection_source},
        {"synthetic_runtime_detection", record.synthetic_runtime_detection},
        {"production_detection_valid", !record.synthetic_runtime_detection}
    };
    if (!record.error.empty()) {
        yolo["error"] = record.error;
    }

    nlohmann::json spatial_mask = {
        {"policy_generation", record.spatial_mask.policy_generation},
        {"result", {
            {"raw_detection_count", record.spatial_mask.raw_detection_count},
            {"inside_detection_count", record.spatial_mask.inside_detection_count},
            {"outside_detection_count", record.spatial_mask.outside_detection_count},
            {"downstream_detection_count", record.spatial_mask.downstream_detection_count},
        }}
    };
    nlohmann::json outside_detections = nlohmann::json::array();
    for (const SpatialMaskOutsideDetection& outside :
         record.spatial_mask.outside_detections) {
        outside_detections.push_back({
            {"raw_index", outside.raw_index},
            {"box", {
                {"x_px", round_px(outside.x_px)},
                {"y_px", round_px(outside.y_px)},
                {"width_px", round_px(outside.width_px)},
                {"height_px", round_px(outside.height_px)},
                {"label", outside.label},
                {"confidence", round_confidence(outside.confidence)},
            }},
            {"centroid_px", {
                {"x", round_px(outside.centroid_x_px)},
                {"y", round_px(outside.centroid_y_px)},
            }},
            {"signed_boundary_distance_px", round_px(outside.signed_boundary_distance_px)},
            {"decision", outside.rejected ? "rejected" : "would_reject"},
            {"rejected_reason", "outside_valid_detection_region"},
        });
    }
    spatial_mask["outside_detections"] = std::move(outside_detections);

    nlohmann::json root = {
        {"schema_id", event_log_format::kYoloEventSchemaId},
        {"schema_version", event_log_format::kYoloEventSchemaVersion},
        {"event_sequence", next_sequence++},
        {"event_kind", "yolo_result"},
        {"camera_serial", camera_serial_},
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
        {"spatial_mask", std::move(spatial_mask)},
        {"detections", std::move(detections)},
        {"citrus_live_ipc", {
            {"enabled", record.ipc_enabled},
            {"requested", record.ipc_requested},
            {"request_status", record.ipc_request_status}
        }}
    };

    file_ << root.dump() << '\n';
    if (file_) last_written_by_folder_[record.recording_folder] = record.recording_frame_id;
    else failed_folders_.insert(record.recording_folder);
}

void YoloEventLogger::ThreadMain() {
    std::vector<Event> pending;
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_ || !queue_.empty()) {
        if (queue_.empty()) {
            if (pending.empty()) cv_.wait(lock);
            else cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
        const bool have_event = !queue_.empty();
        Event event;
        if (have_event) { event = std::move(queue_.front()); queue_.pop_front(); }
        lock.unlock();

        if (have_event) switch (event.type) {
            case EventType::kClose:
                CloseFile();
                break;
            case EventType::kYoloResult:
                WriteResult(event.result);
                break;
            case EventType::kFlushThrough:
                if (pending.size() >= 8) event.completion->set_value(false);
                else pending.push_back(std::move(event));
                break;
        }
        for (auto it = pending.begin(); it != pending.end();) {
            const auto& folder = it->result.recording_folder;
            const auto found = last_written_by_folder_.find(folder);
            const bool reached = found != last_written_by_folder_.end() &&
                found->second >= it->result.recording_frame_id;
            const bool expired = std::chrono::steady_clock::now() >= it->deadline;
            if (!reached && !expired && !failed_folders_.count(folder)) { ++it; continue; }
            if (reached && current_folder_ == folder && file_.is_open()) {
                file_.flush();
                if (!file_) failed_folders_.insert(folder);
            }
            it->completion->set_value(reached && !expired && !failed_folders_.count(folder));
            it = pending.erase(it);
        }

        lock.lock();
    }
    CloseFile();
    for (auto& event : pending) event.completion->set_value(false);
    writer_finished_ = true;
    cv_.notify_all();
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
    record.source_width = frame.width;
    record.source_height = frame.height;
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
    record.spatial_mask.raw_detection_count =
        static_cast<int>(record.detections.size());
    record.spatial_mask.downstream_detection_count =
        static_cast<int>(record.detections.size());
    logger_.Enqueue(std::move(record));
}

}  // namespace yolo_event_log
