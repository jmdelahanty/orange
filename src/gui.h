#ifndef ORANGE_GUI
#define ORANGE_GUI
#include "gx_helper.h"
#include "camera.h"
#include "project.h"
#include <math.h>
#include <thread>
#include <unordered_map>
#include "acquire_frames.h"
#include "yolo_worker.h" // Include the YOLOv8Worker header
#include "network_base.h"
#include "enet_thread.h"

struct GL_Texture {
    GLuint texture;
    GLuint pbo;
    cudaGraphicsResource_t cuda_resource;
    unsigned char* cuda_buffer;
    size_t cuda_pbo_storage_buffer_size;
    cudaStream_t streams;
    int num_channels;
};

void setup_texture(GL_Texture& tex, int width, int height) {
    cudaStreamCreate(&tex.streams);
    create_pbo(&tex.pbo, width, height);
    register_pbo_to_cuda(&tex.pbo, &tex.cuda_resource);
    map_cuda_resource(&tex.cuda_resource, tex.streams);
    cuda_pointer_from_resource(&tex.cuda_buffer, &tex.cuda_pbo_storage_buffer_size, &tex.cuda_resource);
    create_texture(&tex.texture, width, height);
}

void upload_texture_from_pbo(GL_Texture& tex, int width, int height) {
    bind_pbo(&tex.pbo);
    bind_texture(&tex.texture);
    upload_image_pbo_to_texture(width, height);  // Uses currently bound PBO and texture
    unbind_pbo();
    unbind_texture();
}

void clear_upload_and_cleanup(GL_Texture& tex, int width, int height) {
    // Clear the CUDA buffer
    int size_pic = width * height * sizeof(unsigned char) * 4;
    if (tex.cuda_buffer) { // Check if buffer was actually allocated (e.g. stream_on was true)
      cudaMemset(tex.cuda_buffer, 0, size_pic);
    }

    // Upload from PBO to texture
    upload_texture_from_pbo(tex, width, height);

    // Cleanup resources
    gx_delete_buffer(&tex.pbo);
    if (tex.cuda_resource) { // Check if resource was registered
      unmap_cuda_resource(&tex.cuda_resource);
      cuda_unregister_pbo(tex.cuda_resource);
    }
    if (tex.streams) {
      cudaStreamDestroy(tex.streams);
      tex.streams = nullptr;
    }
    if (tex.texture) {
      glDeleteTextures(1, &tex.texture);
      tex.texture = 0;
    }
    tex.cuda_buffer = nullptr;
}

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
