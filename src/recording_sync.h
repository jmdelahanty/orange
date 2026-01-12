// recording_sync.h
#pragma once

#include <atomic>
#include <iostream>
#include <chrono>

struct RecordingSync {
    // Command and state
    std::atomic<uint64_t> target_start_frame{0};
    std::atomic<uint64_t> target_stop_frame{0};
    std::atomic<uint64_t> duration_frames{0};
    std::atomic<bool> start_pending{false};
    std::atomic<bool> stop_pending{false};
    
    // Camera coordination
    std::atomic<uint32_t> cameras_ready{0};
    std::atomic<uint32_t> cameras_recording{0};
    std::atomic<uint32_t> cameras_stopped{0};
    uint32_t total_cameras{0};
    
    // Statistics
    std::atomic<uint64_t> total_recordings_completed{0};
    std::chrono::steady_clock::time_point recording_start_time;
    std::chrono::steady_clock::time_point recording_stop_time;
    
    void reset() {
        target_start_frame = 0;
        target_stop_frame = 0;
        duration_frames = 0;
        start_pending = false;
        stop_pending = false;
        cameras_ready = 0;
        cameras_recording = 0;
        cameras_stopped = 0;
    }
    
    void scheduleRecording(uint64_t start_frame, uint64_t duration) {
        reset();
        target_start_frame = start_frame;
        duration_frames = duration;
        target_stop_frame = start_frame + duration;
        start_pending = true;
        recording_start_time = std::chrono::steady_clock::now();
        
        std::cout << "[RecordingSync] Scheduled recording:" << std::endl
                  << "  Start frame: " << start_frame << std::endl
                  << "  Duration: " << duration << " frames" << std::endl
                  << "  Stop frame: " << target_stop_frame.load() << std::endl;
    }
    
    void scheduleStop(uint64_t stop_frame) {
        target_stop_frame = stop_frame;
        stop_pending = true;
        std::cout << "[RecordingSync] Scheduled stop at frame " << stop_frame << std::endl;
    }
    
    bool shouldStart(uint64_t current_frame) const {
        return start_pending && current_frame >= target_start_frame;
    }
    
    bool shouldStop(uint64_t current_frame, uint64_t recording_frame_id) const {
        // Stop if manual stop issued and we've reached the stop frame
        if (stop_pending && current_frame >= target_stop_frame) {
            return true;
        }
        
        // Stop if we've recorded the requested duration
        if (duration_frames > 0 && recording_frame_id >= duration_frames) {
            return true;
        }
        
        return false;
    }
    
    void reportStart(const std::string& camera_serial) {
        uint32_t count = cameras_recording.fetch_add(1) + 1;
        std::cout << "[" << camera_serial << "] Started recording "
                  << "(" << count << "/" << total_cameras << " cameras active)" 
                  << std::endl;
    }
    
    void reportStop(const std::string& camera_serial, uint64_t frames_recorded) {
        uint32_t count = cameras_stopped.fetch_add(1) + 1;
        std::cout << "[" << camera_serial << "] Stopped recording after " 
                  << frames_recorded << " frames "
                  << "(" << count << "/" << total_cameras << " cameras stopped)" 
                  << std::endl;
        
        // If all cameras have stopped, report summary
        if (count == total_cameras) {
            recording_stop_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                recording_stop_time - recording_start_time);
            
            total_recordings_completed++;
            
            std::cout << "\n=== RECORDING COMPLETE ===" << std::endl
                      << "All " << total_cameras << " cameras recorded " 
                      << frames_recorded << " frames" << std::endl
                      << "Duration: " << duration.count() / 1000.0 << " seconds" << std::endl
                      << "Total recordings this session: " << total_recordings_completed.load() 
                      << std::endl << std::endl;
        }
    }
};