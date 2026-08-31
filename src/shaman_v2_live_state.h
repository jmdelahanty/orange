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
        if (!slot_payload_valid(base)) {
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
        // A base frame is not a YOLO terminal result, even if a caller
        // happened to leave detection-looking fields populated in it.  Pose
        // evidence received before the same-frame YOLO terminal is retained
        // privately and must not make this interim state authoritative.
        has_authoritative_yolo_ = false;
        has_pending_pose_evidence_ = false;
        pending_pose_evidence_ = Slot{};

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
        if (!slot_payload_valid(update)) {
            counters_.queue_drops++;
            return false;
        }
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
        if (!slot_payload_valid(update)) {
            counters_.queue_drops++;
            return false;
        }
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
        // A pose worker can finish before a delayed YOLO update for the same
        // frame. Keep any already-enriched pose attached to its exact
        // detection when the authoritative YOLO vector arrives.
        const Slot previous = current_;
        const Slot early_pose = pending_pose_evidence_;
        const bool had_early_pose = has_pending_pose_evidence_;
        current_.source_frame_id = current_.state_frame_id;
        current_.detection_status = update.detection_status;
        if (update.detection_model_id_hash != 0) {
            current_.detection_model_id_hash = update.detection_model_id_hash;
        }
        current_.source_detection_count = update.source_detection_count;
        current_.retained_detection_count = update.retained_detection_count;
        current_.transmitted_object_count = update.transmitted_object_count;
        current_.objects_truncated = update.objects_truncated;
        current_.object_order = update.object_order;
        current_.detection_reason = update.detection_reason;
        current_.object_count = update.object_count;
        for (uint32_t i = 0; i < update.object_count && i < kMaxObjects; ++i) {
            current_.objects[i] = update.objects[i];
        }
        has_authoritative_yolo_ = true;
        for (uint32_t previous_index = 0;
             previous_index < previous.object_count && previous_index < kMaxObjects;
             ++previous_index) {
            const Object& previous_object = previous.objects[previous_index];
            if ((previous_object.flags & kObjectHasPose) == 0) {
                continue;
            }
            const int match = find_unique_bbox_match(
                previous_object, current_.objects, current_.object_count);
            if (match >= 0) {
                merge_pose_object(&current_.objects[match], previous_object);
            }
        }
        if (had_early_pose) {
            for (uint32_t pose_index = 0;
                 pose_index < early_pose.object_count && pose_index < kMaxObjects;
                 ++pose_index) {
                const int match = find_unique_bbox_match(
                    early_pose.objects[pose_index],
                    current_.objects,
                    current_.object_count);
                if (match >= 0) {
                    merge_pose_object(
                        &current_.objects[match], early_pose.objects[pose_index]);
                }
            }
        }
        has_pending_pose_evidence_ = false;
        pending_pose_evidence_ = Slot{};
    }

    void apply_pose_update(const Slot& update)
    {
        current_.source_frame_id = current_.state_frame_id;
        current_.pose_status = update.pose_status;
        current_.pose_model_id_hash = update.pose_model_id_hash;
        current_.pose_skeleton_id_hash = update.pose_skeleton_id_hash;
        if (!has_authoritative_yolo_) {
            // A pose worker can finish before YOLO. Keep this evidence out of
            // the published detection vector until the same-frame YOLO
            // terminal establishes authoritative counts and ordering.
            pending_pose_evidence_ = update;
            has_pending_pose_evidence_ = true;
            return;
        }
        for (uint32_t pose_index = 0;
             pose_index < update.object_count && pose_index < kMaxObjects;
             ++pose_index) {
            const int match = find_unique_bbox_match(
                update.objects[pose_index], current_.objects, current_.object_count);
            if (match >= 0) {
                merge_pose_object(&current_.objects[match], update.objects[pose_index]);
            }
        }
    }

    static bool same_bbox(const Object& lhs, const Object& rhs)
    {
        return lhs.x_px == rhs.x_px && lhs.y_px == rhs.y_px &&
               lhs.width_px == rhs.width_px && lhs.height_px == rhs.height_px;
    }

    static int find_unique_bbox_match(const Object& needle,
                                      const Object* haystack,
                                      uint32_t haystack_count)
    {
        int match = -1;
        for (uint32_t index = 0; index < haystack_count && index < kMaxObjects;
             ++index) {
            if (!same_bbox(needle, haystack[index])) {
                continue;
            }
            if (match >= 0) {
                // Identical boxes are not enough to identify one pose target.
                return -1;
            }
            match = static_cast<int>(index);
        }
        return match;
    }

    static void merge_pose_object(Object* detection, const Object& pose)
    {
        if (!detection) {
            return;
        }
        detection->flags |= pose.flags &
            (kObjectHasPose | kObjectSynthetic);
        detection->keypoint_count = pose.keypoint_count;
        for (uint32_t index = 0;
             index < pose.keypoint_count && index < kMaxKeypointsPerObject;
             ++index) {
            detection->keypoints[index] = pose.keypoints[index];
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
    bool has_authoritative_yolo_ = false;
    bool has_pending_pose_evidence_ = false;
    uint64_t latest_base_state_frame_id_ = 0;
    Slot current_;
    Slot pending_pose_evidence_;
    std::map<uint64_t, Slot> pending_yolo_;
    std::map<uint64_t, Slot> pending_pose_;
    LiveStateCounters counters_;
    mutable std::mutex mutex_;
};

} // namespace shaman_v2
