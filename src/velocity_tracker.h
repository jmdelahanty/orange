// velocity_tracker.h

#ifndef VELOCITY_TRACKER_H
#define VELOCITY_TRACKER_H

#include "common.hpp"
#include <deque>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <limits>

struct PositionHistory {
    float x, y;  // Center position in pixels
    uint64_t timestamp_us;  // Timestamp in microseconds
};

struct TrackedObject {
    int track_id;
    pose::Object latest_detection;
    std::deque<PositionHistory> position_history;
    float current_speed_pixels_per_sec;
    float current_speed_physical_units;  // For future calibration
    bool is_valid;
    uint64_t last_seen_timestamp;
    
    // Default constructor
    TrackedObject() : track_id(-1), current_speed_pixels_per_sec(0.0f), 
                     current_speed_physical_units(0.0f), is_valid(false), 
                     last_seen_timestamp(0) {}
    
    // Constructor with parameters
    TrackedObject(int id, const pose::Object& detection, uint64_t timestamp) 
        : track_id(id), latest_detection(detection), current_speed_pixels_per_sec(0.0f), 
          current_speed_physical_units(0.0f), is_valid(true), last_seen_timestamp(timestamp) {
        
        // Add initial position
        float center_x = detection.rect.x + detection.rect.width * 0.5f;
        float center_y = detection.rect.y + detection.rect.height * 0.5f;
        position_history.push_back({center_x, center_y, timestamp});
    }
};

class VelocityTracker {
private:
    TrackedObject single_tracked_object_;
    bool has_active_track_;
    static constexpr size_t MAX_HISTORY_SIZE = 10;  // Keep last 10 positions
    static constexpr uint64_t MAX_TRACKING_GAP_US = 1000000;  // 1 second max gap
    static constexpr float MAX_DISTANCE_THRESHOLD = 150.0f;  // Max pixels between frames for same object
    static constexpr int CONSISTENT_TRACK_ID = 1;  // Always use track ID 1 for now...
    static constexpr int PIXELS_PER_CM = 500.0f; // Within Pancake0 current configuration, should be a config value in a file...

    
public:
    VelocityTracker() : has_active_track_(false) {}
    
    // Main tracking function - call this after YOLO postprocess
    void updateTracking(const std::vector<pose::Object>& detections, uint64_t timestamp_us);
    
    // Get current tracked objects with speed data
    std::vector<TrackedObject> getTrackedObjects() const;
    
    // Get speed for specific track ID
    float getSpeed(int track_id) const;

    float getSpeedCmPerSec(int track_id) const;
    
    // Clear old/stale tracks
    void cleanupStale(uint64_t current_timestamp);
    
private:
    float calculateDistance(const pose::Object& detection, const TrackedObject& tracked);
    void updateObjectSpeed(TrackedObject& obj, uint64_t timestamp);
    pose::Object findBestDetection(const std::vector<pose::Object>& detections);
};

// Main tracking update function - MODIFIED for single object
inline void VelocityTracker::updateTracking(const std::vector<pose::Object>& detections, uint64_t timestamp_us) {
    // Clean up stale tracks first
    cleanupStale(timestamp_us);
    
    if (detections.empty()) {
        return;  // No detections, nothing to track
    }
    
    // Find the best detection (highest confidence)
    pose::Object best_detection = findBestDetection(detections);
    
    if (!has_active_track_) {
        // Create new track with consistent ID
        single_tracked_object_ = TrackedObject(CONSISTENT_TRACK_ID, best_detection, timestamp_us);
        has_active_track_ = true;
    } else {
        // Check if we should continue tracking this object
        float distance = calculateDistance(best_detection, single_tracked_object_);
        
        if (distance < MAX_DISTANCE_THRESHOLD) {
            // Continue tracking - update existing track
            single_tracked_object_.latest_detection = best_detection;
            single_tracked_object_.last_seen_timestamp = timestamp_us;
            
            // Add new position to history
            float center_x = best_detection.rect.x + best_detection.rect.width * 0.5f;
            float center_y = best_detection.rect.y + best_detection.rect.height * 0.5f;
            single_tracked_object_.position_history.push_back({center_x, center_y, timestamp_us});
            
            // Limit history size
            if (single_tracked_object_.position_history.size() > MAX_HISTORY_SIZE) {
                single_tracked_object_.position_history.pop_front();
            }
            
            // Update speed calculation
            updateObjectSpeed(single_tracked_object_, timestamp_us);
        } else {
            // Object moved too far - start tracking the new detection
            single_tracked_object_ = TrackedObject(CONSISTENT_TRACK_ID, best_detection, timestamp_us);
        }
    }
}

// Find the best detection (highest probability)
inline pose::Object VelocityTracker::findBestDetection(const std::vector<pose::Object>& detections) {
    auto best_it = std::max_element(detections.begin(), detections.end(),
        [](const pose::Object& a, const pose::Object& b) {
            return a.prob < b.prob;  // Find highest probability
        });
    return *best_it;
}

// Clean up stale tracks - MODIFIED for single object
inline void VelocityTracker::cleanupStale(uint64_t current_timestamp) {
    if (has_active_track_) {
        if (!single_tracked_object_.is_valid || 
            (current_timestamp - single_tracked_object_.last_seen_timestamp) > MAX_TRACKING_GAP_US) {
            has_active_track_ = false;
        }
    }
}

// Get current tracked objects - MODIFIED for single object
inline std::vector<TrackedObject> VelocityTracker::getTrackedObjects() const {
    std::vector<TrackedObject> result;
    if (has_active_track_ && single_tracked_object_.is_valid) {
        result.push_back(single_tracked_object_);
    }
    return result;
}

// Get speed for specific track - MODIFIED for single object
inline float VelocityTracker::getSpeed(int track_id) const {
    if (has_active_track_ && single_tracked_object_.is_valid && 
        single_tracked_object_.track_id == track_id) {
        return single_tracked_object_.current_speed_pixels_per_sec;
    }
    return 0.0f;
}


// Calculate distance between detection and tracked object center
inline float VelocityTracker::calculateDistance(const pose::Object& detection, const TrackedObject& tracked) {
    float det_center_x = detection.rect.x + detection.rect.width * 0.5f;
    float det_center_y = detection.rect.y + detection.rect.height * 0.5f;
    
    if (tracked.position_history.empty()) return std::numeric_limits<float>::max();
    
    const auto& last_pos = tracked.position_history.back();
    float dx = det_center_x - last_pos.x;
    float dy = det_center_y - last_pos.y;
    
    return std::sqrt(dx * dx + dy * dy);
}

// Update speed calculation for a tracked object
inline void VelocityTracker::updateObjectSpeed(TrackedObject& obj, [[maybe_unused]] uint64_t timestamp) {
    if (obj.position_history.size() < 2) {
        obj.current_speed_pixels_per_sec = 0.0f;
        obj.current_speed_physical_units = 0.0f;
        return;
    }
    
    // Use last two positions for speed calculation
    const auto& current_pos = obj.position_history.back();
    const auto& prev_pos = obj.position_history[obj.position_history.size() - 2];
    
    // Calculate distance moved in pixels
    float dx = current_pos.x - prev_pos.x;
    float dy = current_pos.y - prev_pos.y;
    float distance_pixels = std::sqrt(dx * dx + dy * dy);
    
    // Calculate time difference in seconds
    uint64_t time_diff_us = current_pos.timestamp_us - prev_pos.timestamp_us;
    float time_diff_sec = time_diff_us / 1000000.0f;
    
    // Calculate speed in pixels per second
    if (time_diff_sec > 0.0f) {
        obj.current_speed_pixels_per_sec = distance_pixels / time_diff_sec;
        // CONVERT TO CM/S using calibration
        obj.current_speed_physical_units = obj.current_speed_pixels_per_sec / PIXELS_PER_CM;
    } else {
        obj.current_speed_pixels_per_sec = 0.0f;
        obj.current_speed_physical_units = 0.0f;
    }
}

inline float VelocityTracker::getSpeedCmPerSec(int track_id) const {
    if (has_active_track_ && single_tracked_object_.is_valid && 
        single_tracked_object_.track_id == track_id) {
        return single_tracked_object_.current_speed_physical_units;
    }
    return 0.0f;
}


#endif // VELOCITY_TRACKER_H