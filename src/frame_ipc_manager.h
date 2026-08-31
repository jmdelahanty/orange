// frame_ipc_manager.h
#pragma once

#include "shaman.h"
#include "shaman_v2_live_state.h"
#include "camera.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct FrameIPCFrameIdentity {
    // The legacy v1 identifier retains its historical recording/local
    // switching behavior during migration. Shaman v2 uses the independent,
    // monotonic stream-local state identifier and carries every other identity
    // explicitly.
    uint64_t legacy_frame_id = 0;
    uint64_t state_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
    uint64_t detection_model_id_hash = 0;
    std::string recording_identity_token;
};

class FrameIPCManager {
public:
    explicit FrameIPCManager(CameraParams* camera_params,
                             bool force_v2_live_state = false)
        : camera_params_(camera_params),
          frame_queue_(kQueueDepth),
          update_queue_(kQueueDepth) {
        queue_name_ = "/shm_cam_" + camera_params_->camera_serial;

        try {
            ipc_queue_ = std::make_unique<shaman::SharedBoxQueue>(
                queue_name_.c_str(), true /* is_writer */);
            enabled_ = true;
        } catch (const std::exception& e) {
            init_error_ = e.what();
            enabled_ = false;
            std::cerr << "[FrameIPC] Failed to initialize " << queue_name_
                      << ": " << init_error_ << std::endl;
        } catch (...) {
            init_error_ = "unknown exception";
            enabled_ = false;
            std::cerr << "[FrameIPC] Failed to initialize " << queue_name_
                      << ": " << init_error_ << std::endl;
        }

        init_v2_if_requested(force_v2_live_state);

        if (enabled_) {
            running_ = true;
            writer_thread_ = std::thread(&FrameIPCManager::ThreadMain, this);
        }
    }

    ~FrameIPCManager() {
        StopThread();
    }

    void stop() {
        StopThread();
    }

    // `timestamp` is the original camera/acquisition timestamp from Orange
    // (`camera_timestamp_ns` terminology). The current `/shm_cam_<serial>`
    // queue does not expose that value; SharedBoxQueue stamps publish-time SHM
    // timestamps when the writer thread pushes the slot.
    bool sendFrame(const FrameIPCFrameIdentity& identity,
                   bool yolo_processing) {
        return sendFrame(identity, yolo_processing, yolo_processing);
    }

    bool sendFrame(const FrameIPCFrameIdentity& identity,
                   bool yolo_processing,
                   bool v2_detection_terminal_expected) {
        if (!enabled_) {
            return false;
        }
        FrameEvent event;
        event.identity = identity;
        event.yolo_processing = yolo_processing;
        // This is deliberately separate from yolo_processing: a worker can
        // run for recording/analytics while live v2 publication is disabled.
        event.v2_detection_terminal_expected = v2_detection_terminal_expected;

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_queue_.PushDropOldest(std::move(event), &dropped);
        }
        if (dropped) {
            base_queue_drops_++;
        }
        cv_.notify_one();
        return true;
    }

    // Transitional test/caller compatibility. New acquisition code must use
    // the closed identity overload above.
    bool sendFrame(uint64_t frame_id,
                   uint64_t timestamp,
                   bool yolo_processing) {
        FrameIPCFrameIdentity identity;
        identity.legacy_frame_id = frame_id;
        identity.state_frame_id = frame_id;
        identity.camera_frame_id = frame_id;
        identity.camera_timestamp_ns = timestamp;
        return sendFrame(identity, yolo_processing, yolo_processing);
    }

    bool updateFrameWithDetections(uint64_t legacy_frame_id,
                                   uint64_t state_frame_id,
                                   std::vector<shaman::Object> detections) {
        const auto status = detections.empty()
            ? shaman_v2::DetectionStatus::kZeroDetections
            : shaman_v2::DetectionStatus::kDetections;
        return updateFrameWithDetectionResult(
            legacy_frame_id, state_frame_id, status, std::move(detections));
    }

    // Extended grouped-live metadata. Counts are supplied by YOLO after
    // postprocessing and spatial-mask evaluation; callers that do not have
    // those stages can use the compatibility overload above.
    bool updateFrameWithDetectionResult(
        uint64_t legacy_frame_id,
        uint64_t state_frame_id,
        shaman_v2::DetectionStatus detection_status,
        std::vector<shaman::Object> detections,
        uint32_t source_detection_count,
        uint32_t retained_detection_count,
        uint64_t detection_model_id_hash,
        shaman_v2::DetectionResultReason detection_reason =
            shaman_v2::DetectionResultReason::kNone,
        bool synthetic_objects = false) {
        return enqueueDetectionUpdate(
            legacy_frame_id,
            state_frame_id,
            detection_status,
            std::move(detections),
            source_detection_count,
            retained_detection_count,
            detection_model_id_hash,
            detection_reason,
            true,
            synthetic_objects);
    }

    // Synthetic runtime detections are a v2 test seam. They must never alter
    // the legacy v1 stream, but when v2 is enabled they still terminate the
    // pending base state. The final flag is kept private to this explicit API
    // so normal detection updates cannot accidentally suppress v1 output.
    bool publishSyntheticYoloResult(
        uint64_t legacy_frame_id,
        uint64_t state_frame_id,
        std::vector<shaman::Object> detections,
        uint32_t source_detection_count,
        uint32_t retained_detection_count,
        uint64_t detection_model_id_hash,
        shaman_v2::DetectionStatus detection_status,
        shaman_v2::DetectionResultReason detection_reason) {
        if (!enabled_ || !v2_publisher_) {
            return false;
        }
        return enqueueDetectionUpdate(
            legacy_frame_id,
            state_frame_id,
            detection_status,
            std::move(detections),
            source_detection_count,
            retained_detection_count,
            detection_model_id_hash,
            detection_reason,
            false,
            true);
    }

    bool updateFrameWithDetectionResult(
        uint64_t legacy_frame_id,
        uint64_t state_frame_id,
        shaman_v2::DetectionStatus detection_status,
        std::vector<shaman::Object> detections) {
        const uint32_t count = static_cast<uint32_t>(
            std::min<std::size_t>(detections.size(), UINT32_MAX));
        return enqueueDetectionUpdate(
            legacy_frame_id,
            state_frame_id,
            detection_status,
            std::move(detections),
            count,
            count,
            0,
            shaman_v2::DetectionResultReason::kNone,
            true);
    }

    // A scheduled YOLO frame whose worker enqueue was rejected still receives
    // a terminal v2 state. It is deliberately v2-only so the legacy v1 queue
    // retains its historical absence-of-update behavior on this failure path.
    bool publishYoloWorkerEnqueueRejected(
        uint64_t legacy_frame_id,
        uint64_t state_frame_id,
        uint64_t detection_model_id_hash = 0) {
        return enqueueDetectionUpdate(
            legacy_frame_id,
            state_frame_id,
            shaman_v2::DetectionStatus::kFailed,
            {},
            0,
            0,
            detection_model_id_hash,
            shaman_v2::DetectionResultReason::kYoloWorkerEnqueueRejected,
            false);
    }

private:
    bool enqueueDetectionUpdate(
        uint64_t legacy_frame_id,
        uint64_t state_frame_id,
        shaman_v2::DetectionStatus detection_status,
        std::vector<shaman::Object> detections,
        uint32_t source_detection_count,
        uint32_t retained_detection_count,
        uint64_t detection_model_id_hash,
        shaman_v2::DetectionResultReason detection_reason,
        bool emit_legacy,
        bool synthetic_objects = false) {
        if (!enabled_) {
            return false;
        }
        if (detection_reason == shaman_v2::DetectionResultReason::kNone &&
            detection_status == shaman_v2::DetectionStatus::kZeroDetections) {
            detection_reason = source_detection_count == 0
                ? shaman_v2::DetectionResultReason::kNoSourceDetections
                : shaman_v2::DetectionResultReason::kAllDetectionsRejectedByMask;
        } else if (detection_reason == shaman_v2::DetectionResultReason::kNone &&
                   detection_status == shaman_v2::DetectionStatus::kFailed) {
            detection_reason =
                shaman_v2::DetectionResultReason::kProcessingFailed;
        }
        if (legacy_frame_id == 0 || state_frame_id == 0 ||
            detections.size() > std::numeric_limits<uint32_t>::max() ||
            retained_detection_count != detections.size() ||
            source_detection_count < retained_detection_count ||
            !valid_detection_update(
                detection_status,
                source_detection_count,
                retained_detection_count,
                detection_reason)) {
            return false;
        }
        UpdateEvent event;
        event.legacy_frame_id = legacy_frame_id;
        event.state_frame_id = state_frame_id;
        event.detection_status = detection_status;
        event.detections = std::move(detections);
        event.source_detection_count = source_detection_count;
        event.retained_detection_count = retained_detection_count;
        event.detection_model_id_hash = detection_model_id_hash;
        event.detection_reason = detection_reason;
        event.emit_legacy = emit_legacy;
        event.synthetic_objects = synthetic_objects;

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            update_queue_.PushDropOldest(std::move(event), &dropped);
        }
        if (dropped) {
            update_queue_drops_++;
        }
        cv_.notify_one();
        return true;
    }

    static bool valid_detection_update(
        shaman_v2::DetectionStatus status,
        uint32_t source_detection_count,
        uint32_t retained_detection_count,
        shaman_v2::DetectionResultReason reason) {
        using shaman_v2::DetectionResultReason;
        using shaman_v2::DetectionStatus;
        if (status != DetectionStatus::kDetections &&
            status != DetectionStatus::kZeroDetections &&
            status != DetectionStatus::kFailed) {
            return false;
        }
        if (status == DetectionStatus::kDetections) {
            return retained_detection_count > 0 &&
                   retained_detection_count <= shaman_v2::kMaxObjects &&
                   reason == DetectionResultReason::kNone;
        }
        if (status == DetectionStatus::kZeroDetections) {
            return retained_detection_count == 0 &&
                   ((source_detection_count == 0 &&
                     reason == DetectionResultReason::kNoSourceDetections) ||
                    (source_detection_count > 0 &&
                     reason == DetectionResultReason::kAllDetectionsRejectedByMask));
        }
        if (reason == DetectionResultReason::kNone) {
            return false;
        }
        if (reason == DetectionResultReason::kNoSourceDetections) {
            return source_detection_count == 0 && retained_detection_count == 0;
        }
        if (reason == DetectionResultReason::kAllDetectionsRejectedByMask) {
            return source_detection_count > 0 && retained_detection_count == 0;
        }
        if (reason == DetectionResultReason::kObjectsTruncated) {
            return retained_detection_count > shaman_v2::kMaxObjects;
        }
        return retained_detection_count == 0;
    }

public:
    bool updateFrameWithDetections(uint64_t frame_id,
                                   std::vector<shaman::Object> detections) {
        return updateFrameWithDetections(frame_id, frame_id, std::move(detections));
    }

    bool updateFrameWithPoseResult(shaman_v2::Slot pose_slot) {
        if (!enabled_ || !v2_publisher_) {
            return false;
        }
        if (pose_slot.state_frame_id == 0) {
            pose_slot.state_frame_id = pose_slot.source_frame_id;
        }
        if (pose_slot.source_frame_id == 0) {
            pose_slot.source_frame_id = pose_slot.state_frame_id;
        }
        if (pose_slot.state_frame_id == 0) {
            return false;
        }
        if (pose_slot.object_count > shaman_v2::kMaxObjects) {
            return false;
        }
        for (uint32_t index = 0; index < pose_slot.object_count; ++index) {
            if (pose_slot.objects[index].keypoint_count >
                shaman_v2::kMaxKeypointsPerObject) {
                return false;
            }
        }
        // The update API is the last host-side boundary before the v2 queue.
        // Counts/order/status/reason must be supplied coherently by the
        // producer; do not silently repair malformed metadata here.
        if (!shaman_v2::slot_payload_valid(pose_slot)) {
            return false;
        }

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pose_update_queue_.PushDropOldest(std::move(pose_slot), &dropped);
        }
        if (dropped) {
            pose_update_queue_drops_++;
        }
        cv_.notify_one();
        return true;
    }

    bool isEnabled() const { return enabled_; }
    const std::string& getQueueName() const { return queue_name_; }
    const std::string& getInitError() const { return init_error_; }
    bool isV2Enabled() const { return v2_enabled_; }
    const std::string& getV2QueueName() const { return v2_queue_name_; }
    const std::string& getV2InitError() const { return v2_init_error_; }
    uint64_t getFramesSent() const { return frames_sent_; }
    uint64_t getUpdatesSent() const { return updates_sent_; }
    uint64_t getBaseQueueDrops() const { return base_queue_drops_; }
    uint64_t getUpdateQueueDrops() const { return update_queue_drops_; }
    uint64_t getPoseUpdateQueueDrops() const { return pose_update_queue_drops_; }
    uint64_t getUpdateStaleDrops() const { return update_stale_drops_; }
    uint64_t getIpcPushFailures() const { return ipc_push_failures_; }
    shaman_v2::LiveStateCounters getV2Counters() const
    {
        return v2_publisher_ ? v2_publisher_->counters_snapshot() : shaman_v2::LiveStateCounters{};
    }

private:
    struct FrameEvent {
        FrameIPCFrameIdentity identity;
        bool yolo_processing = false;
        bool v2_detection_terminal_expected = false;
    };

    struct UpdateEvent {
        uint64_t legacy_frame_id = 0;
        uint64_t state_frame_id = 0;
        shaman_v2::DetectionStatus detection_status =
            shaman_v2::DetectionStatus::kFailed;
        std::vector<shaman::Object> detections;
        uint32_t source_detection_count = 0;
        uint32_t retained_detection_count = 0;
        uint64_t detection_model_id_hash = 0;
        shaman_v2::DetectionResultReason detection_reason =
            shaman_v2::DetectionResultReason::kNone;
        bool emit_legacy = true;
        bool synthetic_objects = false;
    };

    template <typename T>
    class BoundedQueue {
    public:
        explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

        void PushDropOldest(T item, bool* dropped) {
            if (dropped) {
                *dropped = false;
            }
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                if (dropped) {
                    *dropped = true;
                }
            }
            queue_.push_back(std::move(item));
        }

        bool Pop(T& out) {
            if (queue_.empty()) {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            return true;
        }

        bool Empty() const {
            return queue_.empty();
        }

    private:
        size_t capacity_;
        std::deque<T> queue_;
    };

    static bool env_enabled(const char* name) {
        const char* raw = std::getenv(name);
        return raw && *raw && std::string(raw) != "0";
    }

    void init_v2_if_requested(bool force_v2_live_state) {
        if (!enabled_) {
            return;
        }
        if (!force_v2_live_state && !env_enabled("ORANGE_SHAMAN_V2_LIVE_STATE")) {
            return;
        }
        try {
            v2_queue_name_ =
                shaman_v2::queue_name_for_camera_serial(camera_params_->camera_serial);
            v2_queue_ = std::make_unique<shaman_v2::SharedLiveStateQueue>(
                v2_queue_name_,
                true /* writer */);
            v2_publisher_ = std::make_unique<shaman_v2::LiveStatePublisher>(*v2_queue_);
            v2_enabled_ = true;
            std::cout << "[FrameIPC] Shaman v2 live-state queue enabled: "
                      << v2_queue_name_ << std::endl;
        } catch (const std::exception& e) {
            v2_init_error_ = e.what();
            v2_enabled_ = false;
            v2_queue_.reset();
            v2_publisher_.reset();
            std::cerr << "[FrameIPC] Failed to initialize Shaman v2 queue for "
                      << queue_name_ << ": " << v2_init_error_ << std::endl;
        }
    }

    void StopThread() {
        if (!running_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_one();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }

    void ThreadMain() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            cv_.wait(lock, [this]() {
                return !running_ || !frame_queue_.Empty() || !update_queue_.Empty() ||
                       !pose_update_queue_.Empty();
            });
            lock.unlock();
            DrainQueues();
            lock.lock();
        }
        lock.unlock();
        DrainQueues();
    }

    void DrainQueues() {
        if (!enabled_ || !ipc_queue_) {
            return;
        }

        FrameEvent frame;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!frame_queue_.Pop(frame)) {
                    break;
                }
            }
            bool sent = EmitBase(frame);
            if (!sent && !v2_publisher_) {
                continue;
            }
            last_base_frame_id_ = frame.identity.legacy_frame_id;

            if (pending_update_valid_ &&
                pending_update_.legacy_frame_id == last_base_frame_id_) {
                EmitLegacyUpdate(pending_update_);
                pending_update_valid_ = false;
            }
        }

        UpdateEvent update;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!update_queue_.Pop(update)) {
                    break;
                }
            }
            // V2 owns a separate monotonic identity and performs its own
            // bounded pending/stale decision. Do not couple it to the legacy
            // recording/local identifier switch below.
            EmitV2Yolo(update);
            if (!update.emit_legacy) {
                continue;
            }
            if (update.legacy_frame_id == last_base_frame_id_) {
                EmitLegacyUpdate(update);
                continue;
            }
            if (update.legacy_frame_id > last_base_frame_id_) {
                if (!pending_update_valid_ ||
                    update.legacy_frame_id >= pending_update_.legacy_frame_id) {
                    pending_update_ = std::move(update);
                    pending_update_valid_ = true;
                } else {
                    // Preserve Citrus latest-state semantics by suppressing
                    // older delayed detections from the live queue.
                    update_stale_drops_++;
                }
            } else {
                // Preserve Citrus latest-state semantics by suppressing
                // older delayed detections from the live queue.
                update_stale_drops_++;
            }
        }

        shaman_v2::Slot pose_update;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pose_update_queue_.Pop(pose_update)) {
                    break;
                }
            }
            EmitV2Pose(pose_update);
        }
    }

    bool EmitBase(const FrameEvent& frame) {
        static const std::vector<shaman::Object> empty_detections;
        // Current SHM ABI only carries publish-time timestamps stamped inside
        // SharedBoxQueue::push(...). `frame.timestamp` is intentionally not
        // written into `/shm_cam_<serial>`.
        bool success = ipc_queue_->push(
            empty_detections,
            frame.identity.legacy_frame_id,
            camera_params_->camera_id,
            frame.yolo_processing
        );
        EmitV2Base(frame);
        if (success) {
            frames_sent_++;
            return true;
        }
        ipc_push_failures_++;
        return false;
    }

    void EmitLegacyUpdate(const UpdateEvent& update) {
        bool success = ipc_queue_->push(
            update.detections,
            update.legacy_frame_id,
            camera_params_->camera_id,
            true
        );
        if (success) {
            updates_sent_++;
        } else {
            ipc_push_failures_++;
        }
    }

    static shaman_v2::Object ConvertObjectV2(const shaman::Object& object) {
        shaman_v2::Object out;
        out.x_px = object.rect.x;
        out.y_px = object.rect.y;
        out.width_px = object.rect.width;
        out.height_px = object.rect.height;
        out.confidence = object.prob;
        out.label_id = object.label;
        out.track_id = -1;
        out.flags = shaman_v2::kObjectHasBbox;
        const size_t packed_keypoints = object.num_kps / 3;
        const uint32_t keypoint_count = static_cast<uint32_t>(
            std::min<size_t>(packed_keypoints, shaman_v2::kMaxKeypointsPerObject));
        out.keypoint_count = keypoint_count;
        if (keypoint_count > 0) {
            out.flags |= shaman_v2::kObjectHasPose;
        }
        for (uint32_t i = 0; i < keypoint_count; ++i) {
            out.keypoints[i].x_px = object.kps[i * 3 + 0];
            out.keypoints[i].y_px = object.kps[i * 3 + 1];
            out.keypoints[i].confidence = object.kps[i * 3 + 2];
            out.keypoints[i].label_id = static_cast<uint16_t>(i);
            out.keypoints[i].flags =
                out.keypoints[i].confidence > 0.0f ? shaman_v2::kKeypointVisible : 0;
        }
        return out;
    }

    void EmitV2Base(const FrameEvent& frame) {
        if (!v2_publisher_) {
            return;
        }
        shaman_v2::Slot slot;
        slot.state_frame_id = frame.identity.state_frame_id;
        slot.source_frame_id = frame.identity.state_frame_id;
        slot.camera_frame_id = frame.identity.camera_frame_id;
        slot.recording_frame_id = frame.identity.recording_frame_id;
        slot.camera_timestamp_ns = frame.identity.camera_timestamp_ns;
        slot.timestamp_sys_ns = frame.identity.timestamp_sys_ns;
        slot.detection_model_id_hash = frame.identity.detection_model_id_hash != 0
            ? frame.identity.detection_model_id_hash
            : (frame.yolo_processing ? shaman_v2::fnv1a64("unknown") : 0);
        shaman_v2::copy_recording_identity_token(
            slot.recording_identity_token,
            frame.identity.recording_identity_token);
        slot.camera_id = static_cast<uint32_t>(camera_params_->camera_id);
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
        slot.source_width_px = static_cast<uint32_t>(camera_params_->width);
        slot.source_height_px = static_cast<uint32_t>(camera_params_->height);
        slot.detection_status = static_cast<uint32_t>(
            frame.v2_detection_terminal_expected
                ? shaman_v2::DetectionStatus::kPending
                : shaman_v2::DetectionStatus::kNotScheduled);
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kDisabled);
        v2_publisher_->publish_base_frame(slot);
    }

    void EmitV2Yolo(const UpdateEvent& update) {
        if (!v2_publisher_) {
            return;
        }
        shaman_v2::Slot slot;
        slot.state_frame_id = update.state_frame_id;
        slot.source_frame_id = update.state_frame_id;
        slot.camera_id = static_cast<uint32_t>(camera_params_->camera_id);
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
        slot.source_width_px = static_cast<uint32_t>(camera_params_->width);
        slot.source_height_px = static_cast<uint32_t>(camera_params_->height);
        slot.detection_status = static_cast<uint32_t>(update.detection_status);
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kDisabled);
        slot.retained_detection_count = update.retained_detection_count;
        slot.source_detection_count = update.source_detection_count;
        slot.detection_model_id_hash = update.detection_model_id_hash != 0
            ? update.detection_model_id_hash
            : shaman_v2::fnv1a64("unknown");
        slot.detection_reason = static_cast<uint32_t>(update.detection_reason);
        slot.object_order = shaman_v2::kObjectOrderUnorderedPayloadLocal;
        slot.transmitted_object_count = static_cast<uint32_t>(std::min<size_t>(
            update.detections.size(), shaman_v2::kMaxObjects));
        slot.objects_truncated = slot.retained_detection_count >
                slot.transmitted_object_count
            ? slot.retained_detection_count - slot.transmitted_object_count
            : 0;
        if (slot.objects_truncated > 0) {
            slot.detection_status = static_cast<uint32_t>(
                shaman_v2::DetectionStatus::kFailed);
            slot.detection_reason = static_cast<uint32_t>(
                shaman_v2::DetectionResultReason::kObjectsTruncated);
        }
        slot.object_count = slot.transmitted_object_count;
        for (uint32_t i = 0; i < slot.object_count; ++i) {
            slot.objects[i] = ConvertObjectV2(update.detections[i]);
            if (update.synthetic_objects) {
                slot.objects[i].flags |= shaman_v2::kObjectSynthetic;
            }
        }
        v2_publisher_->publish_yolo_result(slot);
    }

    void EmitV2Pose(shaman_v2::Slot slot) {
        if (!v2_publisher_) {
            return;
        }
        if (slot.state_frame_id == 0) {
            slot.state_frame_id = slot.source_frame_id;
        }
        if (slot.source_frame_id == 0) {
            slot.source_frame_id = slot.state_frame_id;
        }
        slot.camera_id = static_cast<uint32_t>(camera_params_->camera_id);
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
        slot.source_width_px = static_cast<uint32_t>(camera_params_->width);
        slot.source_height_px = static_cast<uint32_t>(camera_params_->height);
        slot.payload_kind = static_cast<uint32_t>(shaman_v2::PayloadKind::kLatestTrackingState);
        v2_publisher_->publish_pose_result(slot);
    }

    static constexpr size_t kQueueDepth = shaman::QUEUE_SIZE;

    CameraParams* camera_params_;
    bool enabled_ = false;
    std::unique_ptr<shaman::SharedBoxQueue> ipc_queue_;
    std::string queue_name_;
    std::string init_error_;
    bool v2_enabled_ = false;
    std::string v2_queue_name_;
    std::string v2_init_error_;
    std::unique_ptr<shaman_v2::SharedLiveStateQueue> v2_queue_;
    std::unique_ptr<shaman_v2::LiveStatePublisher> v2_publisher_;

    BoundedQueue<FrameEvent> frame_queue_;
    BoundedQueue<UpdateEvent> update_queue_;
    BoundedQueue<shaman_v2::Slot> pose_update_queue_{kQueueDepth};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    bool running_ = false;

    uint64_t last_base_frame_id_ = 0;
    bool pending_update_valid_ = false;
    UpdateEvent pending_update_;

    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> updates_sent_{0};
    std::atomic<uint64_t> base_queue_drops_{0};
    std::atomic<uint64_t> update_queue_drops_{0};
    std::atomic<uint64_t> pose_update_queue_drops_{0};
    std::atomic<uint64_t> update_stale_drops_{0};  // Delayed older-frame detections suppressed from Citrus live IPC.
    std::atomic<uint64_t> ipc_push_failures_{0};
};
