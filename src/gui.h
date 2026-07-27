#ifndef ORANGE_GUI
#define ORANGE_GUI
#include "gx_helper.h"
#include "gui/texture_resources.h"
#include "camera.h"
#include "project.h"
#include <math.h>
#include <thread>
#include <unordered_map>
#include "acquire_frames.h"
#include "yolo_worker.h" // Include the YoloWorker header
#include "network_base.h"
#include "enet_thread.h"

// utility structure for realtime plot
struct ScrollingBuffer {
    int MaxSize;
    int Offset;
    ImVector<ImVec2> Data;
    ScrollingBuffer(int max_size = 2000) {
        MaxSize = max_size;
        Offset  = 0;
        Data.reserve(MaxSize);
    }
    void AddPoint(float x, float y) {
        if (Data.size() < MaxSize)
            Data.push_back(ImVec2(x,y));
        else {
            Data[Offset] = ImVec2(x,y);
            Offset =  (Offset + 1) % MaxSize;
        }
    }
    void Erase() {
        if (Data.size() > 0) {
            Data.shrink(0);
            Offset  = 0;
        }
    }
};

// utility structure for realtime plot
struct RollingBuffer {
    float Span;
    ImVector<ImVec2> Data;
    RollingBuffer() {
        Span = 10.0f;
        Data.reserve(2000);
    }
    void AddPoint(float x, float y) {
        float xmod = fmodf(x, Span);
        if (!Data.empty() && xmod < Data.back().x)
            Data.shrink(0);
        Data.push_back(ImVec2(xmod, y));
    }
};

// Speed tracking data structure
struct SpeedTrackingData {
    std::unordered_map<int, ScrollingBuffer> track_buffers;  // One buffer per track ID
    std::unordered_map<int, ImVec4> track_colors;           // Color per track ID
    float max_speed_seen;
    float current_time;
    
    SpeedTrackingData() : max_speed_seen(5.0f), current_time(0.0f) {}
    
    void AddSpeedData(int track_id, float speed_cm_per_sec, float time) {
        // Create buffer if it doesn't exist
        if (track_buffers.find(track_id) == track_buffers.end()) {
            track_buffers[track_id] = ScrollingBuffer(1000);
            // Generate a unique color for this track
            track_colors[track_id] = GenerateTrackColor(track_id);
        }
        
        track_buffers[track_id].AddPoint(time, speed_cm_per_sec);
        
        // Update max speed for auto-scaling
        if (speed_cm_per_sec > max_speed_seen) {
            max_speed_seen = speed_cm_per_sec * 1.1f;  // 10% padding
        }
        
        current_time = time;
    }
    
    void ClearOldTracks(float current_time, float max_age = 10.0f) {
        auto it = track_buffers.begin();
        while (it != track_buffers.end()) {
            // Check if track has recent data
            bool has_recent_data = false;
            if (!it->second.Data.empty()) {
                float last_time = it->second.Data.back().x;
                has_recent_data = (current_time - last_time) < max_age;
            }
            
            if (!has_recent_data) {
                track_colors.erase(it->first);
                it = track_buffers.erase(it);
            } else {
                ++it;
            }
        }
    }
    
private:
    ImVec4 GenerateTrackColor(int track_id) {
        // Generate different colors for different tracks
        float hue = (track_id * 137.5f) / 360.0f;  // Golden ratio for good distribution
        while (hue > 1.0f) hue -= 1.0f;
        
        // Convert HSV to RGB (simplified)
        float r, g, b;
        if (hue < 1.0f/3.0f) {
            r = 1.0f; g = hue * 3.0f; b = 0.0f;
        } else if (hue < 2.0f/3.0f) {
            r = 2.0f - hue * 3.0f; g = 1.0f; b = 0.0f;
        } else {
            r = 0.0f; g = 1.0f; b = (hue - 2.0f/3.0f) * 3.0f;
        }
        
        return ImVec4(r, g, b, 1.0f);
    }
};

#endif
