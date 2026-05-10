#pragma once

#include "shaman_v2.h"

#include <cstdint>
#include <map>
#include <mutex>

namespace shaman_v2 {

struct LiveStateCounters {
    uint64_t frames_published = 0;
    uint64_t yolo_updates_published = 0;
    uint64_t pose_updates_published = 0;
    uint64_t yolo_stale_suppressed = 0;
    uint64_t pose_stale_suppressed = 0;
    uint64_t pending_drops = 0;
    uint64_t queue_drops = 0;
    uint64_t push_failures = 0;
};

class LiveStatePublisher {
public:
    explicit LiveStatePublisher(SharedLiveStateQueue& queue, size_t max_pending_updates = 8)
        : queue_(queue),
          max_pending_updates_(max_pending_updates)
    {
    }

    bool publish_base_frame(Slot base)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (base.state_frame_id == 0) {
            base.state_frame_id = base.source_frame_id;
        }
        if (base.source_frame_id == 0) {
            base.source_frame_id = base.state_frame_id;
        }
        if (base.state_frame_id == 0) {
            counters_.queue_drops++;
            return false;
        }
        if (has_current_ && base.state_frame_id < latest_base_state_frame_id_) {
            counters_.queue_drops++;
            return false;
        }

        latest_base_state_frame_id_ = base.state_frame_id;
        current_ = base;
        current_.source_frame_id = current_.state_frame_id;
        has_current_ = true;

        drop_pending_before(latest_base_state_frame_id_);

        bool ok = publish_current(&counters_.frames_published);
        const auto yolo_it = pending_yolo_.find(latest_base_state_frame_id_);
        if (yolo_it != pending_yolo_.end()) {
            apply_yolo_update(yolo_it->second);
            ok = publish_current(&counters_.yolo_updates_published) && ok;
            pending_yolo_.erase(yolo_it);
        }
        const auto pose_it = pending_pose_.find(latest_base_state_frame_id_);
        if (pose_it != pending_pose_.end()) {
            apply_pose_update(pose_it->second);
            ok = publish_current(&counters_.pose_updates_published) && ok;
            pending_pose_.erase(pose_it);
        }
        return ok;
    }

    bool publish_yolo_result(const Slot& update)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t frame_id = update.source_frame_id;
        if (is_stale(frame_id)) {
            counters_.yolo_stale_suppressed++;
            queue_.note_stale_suppressed();
            return false;
        }
        if (!is_current(frame_id)) {
            hold_pending(pending_yolo_, frame_id, update);
            return false;
        }

        apply_yolo_update(update);
        return publish_current(&counters_.yolo_updates_published);
    }

    bool publish_pose_result(const Slot& update)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t frame_id = update.source_frame_id;
        if (is_stale(frame_id)) {
            counters_.pose_stale_suppressed++;
            queue_.note_stale_suppressed();
            return false;
        }
        if (!is_current(frame_id)) {
            hold_pending(pending_pose_, frame_id, update);
            return false;
        }

        apply_pose_update(update);
        return publish_current(&counters_.pose_updates_published);
    }

    LiveStateCounters counters_snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return counters_;
    }

private:
    bool is_current(uint64_t frame_id) const
    {
        return has_current_ && frame_id != 0 && frame_id == latest_base_state_frame_id_;
    }

    bool is_stale(uint64_t frame_id) const
    {
        return has_current_ && frame_id != 0 && frame_id < latest_base_state_frame_id_;
    }

    void hold_pending(std::map<uint64_t, Slot>& pending, uint64_t frame_id, const Slot& update)
    {
        if (frame_id == 0) {
            counters_.pending_drops++;
            return;
        }
        if (pending.find(frame_id) == pending.end() && pending.size() >= max_pending_updates_) {
            if (pending.empty()) {
                counters_.pending_drops++;
                return;
            }
            pending.erase(pending.begin());
            counters_.pending_drops++;
        }
        pending[frame_id] = update;
    }

    void drop_pending_before(uint64_t frame_id)
    {
        while (!pending_yolo_.empty() && pending_yolo_.begin()->first < frame_id) {
            pending_yolo_.erase(pending_yolo_.begin());
            counters_.yolo_stale_suppressed++;
            queue_.note_stale_suppressed();
        }
        while (!pending_pose_.empty() && pending_pose_.begin()->first < frame_id) {
            pending_pose_.erase(pending_pose_.begin());
            counters_.pose_stale_suppressed++;
            queue_.note_stale_suppressed();
        }
    }

    void apply_yolo_update(const Slot& update)
    {
        current_.source_frame_id = current_.state_frame_id;
        current_.detection_status = update.detection_status;
        current_.detection_model_id_hash = update.detection_model_id_hash;
        current_.object_count = update.object_count;
        for (uint32_t i = 0; i < update.object_count && i < kMaxObjects; ++i) {
            current_.objects[i] = update.objects[i];
        }
    }

    void apply_pose_update(const Slot& update)
    {
        current_.source_frame_id = current_.state_frame_id;
        if (update.detection_status == static_cast<uint32_t>(DetectionStatus::kDetections) ||
            update.detection_status == static_cast<uint32_t>(DetectionStatus::kZeroDetections) ||
            update.detection_status == static_cast<uint32_t>(DetectionStatus::kFailed)) {
            current_.detection_status = update.detection_status;
            current_.detection_model_id_hash = update.detection_model_id_hash;
        }
        current_.pose_status = update.pose_status;
        current_.pose_model_id_hash = update.pose_model_id_hash;
        current_.pose_skeleton_id_hash = update.pose_skeleton_id_hash;
        if (update.object_count > 0) {
            current_.object_count = update.object_count;
            for (uint32_t i = 0; i < update.object_count && i < kMaxObjects; ++i) {
                current_.objects[i] = update.objects[i];
            }
        }
    }

    bool publish_current(uint64_t* success_counter)
    {
        if (!queue_.push_latest_state(current_)) {
            counters_.queue_drops++;
            counters_.push_failures = queue_.push_failures();
            return false;
        }
        if (success_counter) {
            (*success_counter)++;
        }
        counters_.push_failures = queue_.push_failures();
        return true;
    }

    SharedLiveStateQueue& queue_;
    size_t max_pending_updates_ = 8;
    bool has_current_ = false;
    uint64_t latest_base_state_frame_id_ = 0;
    Slot current_;
    std::map<uint64_t, Slot> pending_yolo_;
    std::map<uint64_t, Slot> pending_pose_;
    LiveStateCounters counters_;
    mutable std::mutex mutex_;
};

} // namespace shaman_v2
