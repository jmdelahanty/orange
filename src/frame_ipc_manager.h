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
#include <cstdint>
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
    explicit FrameIPCManager(CameraParams* camera_params)
        : camera_params_(camera_params),
          frame_queue_(kQueueDepth),
          update_queue_(kQueueDepth) {
        // One queue per camera: the SHAMAN v2 live-state queue. The v1
        // SharedBoxQueue (/shm_cam_<serial>) was retired on 2026-10-08; Citrus
        // reads only v2, so frame IPC enabled means v2 enabled.
        try {
            queue_name_ =
                shaman_v2::queue_name_for_camera_serial(camera_params_->camera_serial);
            v2_queue_ = std::make_unique<shaman_v2::SharedLiveStateQueue>(
                queue_name_,
                true /* writer */);
            v2_publisher_ = std::make_unique<shaman_v2::LiveStatePublisher>(*v2_queue_);
            enabled_ = true;
            std::cout << "[FrameIPC] Shaman v2 live-state queue enabled: "
                      << queue_name_ << std::endl;
        } catch (const std::exception& e) {
            init_error_ = e.what();
            enabled_ = false;
            v2_queue_.reset();
            v2_publisher_.reset();
            std::cerr << "[FrameIPC] Failed to initialize Shaman v2 queue "
                      << queue_name_ << ": " << init_error_ << std::endl;
        } catch (...) {
            init_error_ = "unknown exception";
            enabled_ = false;
            v2_queue_.reset();
            v2_publisher_.reset();
            std::cerr << "[FrameIPC] Failed to initialize Shaman v2 queue "
                      << queue_name_ << ": " << init_error_ << std::endl;
        }

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

    // Test hooks: hold the writer thread so a test can enqueue base, YOLO and
    // pose events in a chosen order before any of them is drained.
    void PauseWriterForTest() {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_paused_for_test_ = true;
    }
    void ResumeWriterForTest() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writer_paused_for_test_ = false;
        }
        cv_.notify_one();
    }

    // `identity` carries the camera/acquisition identity and timestamps that
    // the v2 base-frame slot publishes.
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
            frame_queue_.PushDropOldest(std::move(event), ++arrival_sequence_, &dropped);
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
            synthetic_objects);
    }

    // Synthetic runtime detections are a test seam: they terminate the
    // pending base state like a real result and mark their objects synthetic.
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
            shaman_v2::DetectionResultReason::kNone);
    }

    // A scheduled YOLO frame whose worker enqueue was rejected still receives
    // a terminal state.
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
            shaman_v2::DetectionResultReason::kYoloWorkerEnqueueRejected);
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
        event.synthetic_objects = synthetic_objects;

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            update_queue_.PushDropOldest(std::move(event), ++arrival_sequence_, &dropped);
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
            pose_update_queue_.PushDropOldest(std::move(pose_slot), ++arrival_sequence_, &dropped);
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
    // Kept for callers written while v1 and v2 coexisted: v2 is the only queue.
    bool isV2Enabled() const { return enabled_; }
    const std::string& getV2QueueName() const { return queue_name_; }
    const std::string& getV2InitError() const { return init_error_; }
    uint64_t getBaseQueueDrops() const { return base_queue_drops_; }
    uint64_t getUpdateQueueDrops() const { return update_queue_drops_; }
    uint64_t getPoseUpdateQueueDrops() const { return pose_update_queue_drops_; }
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
        bool synthetic_objects = false;
    };

    // Each entry carries the manager-wide arrival sequence so DrainQueues can
    // process base, YOLO and pose events in the order they were enqueued.
    template <typename T>
    class BoundedQueue {
    public:
        explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

        void PushDropOldest(T item, uint64_t arrival_sequence, bool* dropped) {
            if (dropped) {
                *dropped = false;
            }
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                if (dropped) {
                    *dropped = true;
                }
            }
            queue_.push_back(Entry{arrival_sequence, std::move(item)});
        }

        bool Pop(T& out) {
            if (queue_.empty()) {
                return false;
            }
            out = std::move(queue_.front().item);
            queue_.pop_front();
            return true;
        }

        bool PeekSequence(uint64_t* arrival_sequence) const {
            if (queue_.empty()) {
                return false;
            }
            if (arrival_sequence) {
                *arrival_sequence = queue_.front().arrival_sequence;
            }
            return true;
        }

        bool Empty() const {
            return queue_.empty();
        }

    private:
        struct Entry {
            uint64_t arrival_sequence = 0;
            T item;
        };
        size_t capacity_;
        std::deque<Entry> queue_;
    };

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
                return !running_ ||
                       (!writer_paused_for_test_ &&
                        (!frame_queue_.Empty() || !update_queue_.Empty() ||
                         !pose_update_queue_.Empty()));
            });
            lock.unlock();
            DrainQueues();
            lock.lock();
        }
        lock.unlock();
        DrainQueues();
    }

    enum class DrainKind { kNone, kBase, kUpdate, kPose };

    // Events are drained in arrival order across the three queues
    // (2026-09-23). Draining every base frame first, then the YOLO updates,
    // then the pose updates let a writer thread that woke one frame period
    // late apply base N+1 before YOLO N and pose N, which the live-state
    // publisher then dropped as stale (about 0.9 % of frames at 100 fps
    // even though inference finishes in about 3 ms).
    void DrainQueues() {
        if (!enabled_ || !v2_publisher_) {
            return;
        }
        while (true) {
            FrameEvent frame;
            UpdateEvent update;
            shaman_v2::Slot pose_update;
            DrainKind kind = DrainKind::kNone;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uint64_t best = UINT64_MAX;
                uint64_t seq = 0;
                if (frame_queue_.PeekSequence(&seq) && seq < best) {
                    best = seq;
                    kind = DrainKind::kBase;
                }
                if (update_queue_.PeekSequence(&seq) && seq < best) {
                    best = seq;
                    kind = DrainKind::kUpdate;
                }
                if (pose_update_queue_.PeekSequence(&seq) && seq < best) {
                    best = seq;
                    kind = DrainKind::kPose;
                }
                switch (kind) {
                    case DrainKind::kBase: frame_queue_.Pop(frame); break;
                    case DrainKind::kUpdate: update_queue_.Pop(update); break;
                    case DrainKind::kPose: pose_update_queue_.Pop(pose_update); break;
                    case DrainKind::kNone: return;
                }
            }
            switch (kind) {
                case DrainKind::kBase: ProcessBaseEvent(frame); break;
                case DrainKind::kUpdate: ProcessUpdateEvent(std::move(update)); break;
                case DrainKind::kPose: EmitV2Pose(pose_update); break;
                case DrainKind::kNone: return;
            }
        }
    }

    void ProcessBaseEvent(const FrameEvent& frame) {
        EmitV2Base(frame);
    }

    void ProcessUpdateEvent(const UpdateEvent& update) {
        // The v2 publisher owns the pending/stale decision for detection
        // results against its own monotonic state identity.
        EmitV2Yolo(update);
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

    static constexpr size_t kQueueDepth = 8;

    CameraParams* camera_params_;
    bool enabled_ = false;
    std::string queue_name_;
    std::string init_error_;
    std::unique_ptr<shaman_v2::SharedLiveStateQueue> v2_queue_;
    std::unique_ptr<shaman_v2::LiveStatePublisher> v2_publisher_;

    BoundedQueue<FrameEvent> frame_queue_;
    BoundedQueue<UpdateEvent> update_queue_;
    BoundedQueue<shaman_v2::Slot> pose_update_queue_{kQueueDepth};
    uint64_t arrival_sequence_ = 0;        // under mutex_
    bool writer_paused_for_test_ = false;  // under mutex_
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    bool running_ = false;

    std::atomic<uint64_t> base_queue_drops_{0};
    std::atomic<uint64_t> update_queue_drops_{0};
    std::atomic<uint64_t> pose_update_queue_drops_{0};
};
