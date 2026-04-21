// frame_ipc_manager.h
#pragma once

#include "shaman.h"
#include "camera.h"
#include <atomic>
#include <condition_variable>
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
    explicit FrameIPCManager(CameraParams* camera_params)
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

    bool isEnabled() const { return enabled_; }
    const std::string& getQueueName() const { return queue_name_; }
    const std::string& getInitError() const { return init_error_; }
    uint64_t getFramesSent() const { return frames_sent_; }
    uint64_t getUpdatesSent() const { return updates_sent_; }
    uint64_t getBaseQueueDrops() const { return base_queue_drops_; }
    uint64_t getUpdateQueueDrops() const { return update_queue_drops_; }
    uint64_t getUpdateStaleDrops() const { return update_stale_drops_; }
    uint64_t getIpcPushFailures() const { return ipc_push_failures_; }

private:
    struct FrameEvent {
        uint64_t frame_id = 0;
        uint64_t timestamp = 0;
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
                return !running_ || !frame_queue_.Empty() || !update_queue_.Empty();
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
            if (!sent) {
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
                    update_stale_drops_++;
                }
            } else {
                update_stale_drops_++;
            }
        }
    }

    bool EmitBase(const FrameEvent& frame) {
        static const std::vector<shaman::Object> empty_detections;
        bool success = ipc_queue_->push(
            empty_detections,
            frame.frame_id,
            camera_params_->camera_id,
            frame.yolo_processing
        );
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
    }

    static constexpr size_t kQueueDepth = shaman::QUEUE_SIZE;

    CameraParams* camera_params_;
    bool enabled_ = false;
    std::unique_ptr<shaman::SharedBoxQueue> ipc_queue_;
    std::string queue_name_;
    std::string init_error_;

    BoundedQueue<FrameEvent> frame_queue_;
    BoundedQueue<UpdateEvent> update_queue_;
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
    std::atomic<uint64_t> update_stale_drops_{0};
    std::atomic<uint64_t> ipc_push_failures_{0};
};
