#pragma once
#include <iostream>
#include <map>
#include <mutex>
#include <atomic>

class PerformanceMonitor {
public:
    struct CameraMetrics {
        double acquisition_fps = 0;
        double preprocess_fps = 0;
        double encode_fps = 0;
        size_t preprocess_queue_depth = 0;
        size_t encode_queue_depth = 0;
        uint64_t frames_dropped = 0;
    };
    
    void update_metrics(int camera_id, const CameraMetrics& metrics) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_metrics_[camera_id] = metrics;
    }
    
    void print_summary() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::cout << "\n=== PERFORMANCE SUMMARY ===" << std::endl;
        double total_fps = 0;
        for (const auto& [cam_id, metrics] : camera_metrics_) {
            std::cout << "Camera " << cam_id << ": "
                      << "Acq=" << metrics.acquisition_fps << " fps, "
                      << "Pre=" << metrics.preprocess_fps << " fps, "
                      << "Enc=" << metrics.encode_fps << " fps, "
                      << "PreQ=" << metrics.preprocess_queue_depth << ", "
                      << "EncQ=" << metrics.encode_queue_depth << ", "
                      << "Drop=" << metrics.frames_dropped << std::endl;
            total_fps += metrics.encode_fps;
        }
        std::cout << "TOTAL THROUGHPUT: " << total_fps << " fps" << std::endl;
        std::cout << "========================\n" << std::endl;
    }
    
private:
    std::map<int, CameraMetrics> camera_metrics_;
    std::mutex mutex_;
};