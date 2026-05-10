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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    bool sendFrame(uint64_t frame_id,
                   uint64_t timestamp,
                   bool yolo_processing) {
        if (!enabled_) {
            return false;
        }
        FrameEvent event;
        event.frame_id = frame_id;
        event.timestamp = timestamp;
        event.yolo_processing = yolo_processing;

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

    bool updateFrameWithDetections(uint64_t frame_id,
                                   std::vector<shaman::Object> detections) {
        if (!enabled_) {
            return false;
        }
        UpdateEvent event;
        event.frame_id = frame_id;
        event.detections = std::move(detections);

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
        uint64_t frame_id = 0;
        uint64_t timestamp = 0;  // Original camera/acquisition timestamp, not current SHM slot timestamp.
        bool yolo_processing = false;
    };

    struct UpdateEvent {
        uint64_t frame_id = 0;
        std::vector<shaman::Object> detections;
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
            last_base_frame_id_ = frame.frame_id;

            if (pending_update_valid_ && pending_update_.frame_id == last_base_frame_id_) {
                EmitUpdate(pending_update_);
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
            if (update.frame_id == last_base_frame_id_) {
                EmitUpdate(update);
                continue;
            }
            if (update.frame_id > last_base_frame_id_) {
                if (!pending_update_valid_ || update.frame_id >= pending_update_.frame_id) {
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
                EmitV2Yolo(update);
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
            frame.frame_id,
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

    void EmitUpdate(const UpdateEvent& update) {
        bool success = ipc_queue_->push(
            update.detections,
            update.frame_id,
            camera_params_->camera_id,
            true
        );
        if (success) {
            updates_sent_++;
        } else {
            ipc_push_failures_++;
        }
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
        slot.state_frame_id = frame.frame_id;
        slot.source_frame_id = frame.frame_id;
        slot.camera_timestamp_ns = frame.timestamp;
        slot.camera_id = static_cast<uint32_t>(camera_params_->camera_id);
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
        slot.source_width_px = static_cast<uint32_t>(camera_params_->width);
        slot.source_height_px = static_cast<uint32_t>(camera_params_->height);
        slot.detection_status = static_cast<uint32_t>(
            frame.yolo_processing
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
        slot.state_frame_id = update.frame_id;
        slot.source_frame_id = update.frame_id;
        slot.camera_id = static_cast<uint32_t>(camera_params_->camera_id);
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
        slot.source_width_px = static_cast<uint32_t>(camera_params_->width);
        slot.source_height_px = static_cast<uint32_t>(camera_params_->height);
        slot.detection_status = static_cast<uint32_t>(
            update.detections.empty()
                ? shaman_v2::DetectionStatus::kZeroDetections
                : shaman_v2::DetectionStatus::kDetections);
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kDisabled);
        slot.object_count = static_cast<uint32_t>(
            std::min<size_t>(update.detections.size(), shaman_v2::kMaxObjects));
        for (uint32_t i = 0; i < slot.object_count; ++i) {
            slot.objects[i] = ConvertObjectV2(update.detections[i]);
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
