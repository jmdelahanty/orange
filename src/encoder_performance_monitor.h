// encoder_performance_monitor.h
// Enhanced performance monitoring and bottleneck detection for encoders

#pragma once
#include <chrono>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <condition_variable>

// Performance monitoring helper class
class EncoderPerformanceMonitor {
public:
    struct Metrics {
        std::atomic<double> fps{0.0};
        std::atomic<uint64_t> frames_processed{0};
        std::atomic<uint64_t> frames_dropped{0};
        std::atomic<uint64_t> queue_depth{0};
        std::atomic<uint64_t> slow_frames{0};
        std::atomic<uint64_t> encode_failures{0};
        std::atomic<uint64_t> resource_waits{0};
        std::atomic<double> avg_latency_ms{0.0};
        std::atomic<double> max_latency_ms{0.0};
    };

    void updateMetrics(const std::string& worker_name, const Metrics& metrics) {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_metrics_[worker_name] = metrics;
    }

    void printSummary() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::cout << "\n╔══════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║               ENCODER PIPELINE PERFORMANCE MONITOR               ║" << std::endl;
        std::cout << "╠══════════════════════════════════════════════════════════════════╣" << std::endl;
        
        for (const auto& [name, metrics] : worker_metrics_) {
            std::cout << "║ " << std::left << std::setw(30) << name 
                     << std::right << std::setw(36) << " ║" << std::endl;
            
            std::cout << "║   FPS: " << std::fixed << std::setprecision(1) << std::setw(5) 
                     << metrics.fps.load() 
                     << " | Queue: " << std::setw(4) << metrics.queue_depth.load()
                     << " | Processed: " << std::setw(8) << metrics.frames_processed.load()
                     << " | Dropped: " << std::setw(4) << metrics.frames_dropped.load() 
                     << "    ║" << std::endl;
            
            std::cout << "║   Latency (avg/max): " 
                     << std::setw(6) << metrics.avg_latency_ms.load() << "/" 
                     << std::setw(6) << metrics.max_latency_ms.load() << " ms"
                     << " | Slow: " << std::setw(4) << metrics.slow_frames.load()
                     << " | Fails: " << std::setw(4) << metrics.encode_failures.load()
                     << "  ║" << std::endl;
            
            // Alert on issues
            if (metrics.queue_depth.load() > 50) {
                std::cout << "║   ⚠️  WARNING: High queue depth - possible bottleneck!          ║" << std::endl;
            }
            if (metrics.fps.load() < 30 && metrics.frames_processed.load() > 100) {
                std::cout << "║   ⚠️  WARNING: Low FPS - encoder may be stuck!                  ║" << std::endl;
            }
        }
        
        std::cout << "╚══════════════════════════════════════════════════════════════════╝" << std::endl;
    }

private:
    std::mutex mutex_;
    std::map<std::string, Metrics> worker_metrics_;
};

// Global instance for easy access
inline EncoderPerformanceMonitor g_encoder_monitor;