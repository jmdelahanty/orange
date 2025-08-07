// test_frame_producer.cpp
// Compile: g++ -std=c++17 test_frame_producer.cpp -o test_frame_producer -lrt -lpthread

#include "shaman.h"  // Your updated shaman.h with per-camera support
#include <thread>
#include <chrono>
#include <map>
#include <memory>
#include <cmath>
#include <random>
#include <csignal>
#include <atomic>
#include <iomanip>  // Added for std::setprecision

std::atomic<bool> g_keep_running(true);

void signal_handler(int signum) {
    std::cout << "\nShutting down producer..." << std::endl;
    g_keep_running = false;
}

// Camera simulator that sends EVERY frame
class CameraSimulator {
public:
    struct Config {
        uint32_t camera_id;
        float fps;
        bool enable_yolo_simulation;
        float detection_probability;  // Chance of detecting objects (0.0 - 1.0)
    };
    
private:
    Config config_;
    std::unique_ptr<shaman::SharedBoxQueue> ipc_queue_;
    std::string queue_name_;
    uint64_t frame_counter_;
    std::chrono::steady_clock::time_point last_frame_time_;
    std::chrono::microseconds frame_interval_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> prob_dist_;
    
    // Simulation state for moving objects
    float object_angle_;
    float angular_velocity_;
    
public:
    CameraSimulator(const Config& config) 
        : config_(config),
          frame_counter_(0),
          rng_(std::random_device{}()),
          prob_dist_(0.0f, 1.0f),
          object_angle_(config.camera_id * 0.5f),  // Different starting angle per camera
          angular_velocity_(0.1f + (config.camera_id % 10) * 0.01f) {
        
        // Create per-camera IPC queue
        queue_name_ = shaman::getCameraQueueName(config.camera_id);
        
        try {
            ipc_queue_ = std::make_unique<shaman::SharedBoxQueue>(
                queue_name_.c_str(), true /* writer */);
            std::cout << "[Camera " << config.camera_id << "] Created IPC queue: " 
                      << queue_name_ << std::endl;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to create IPC queue: " + std::string(e.what()));
        }
        
        // Calculate frame interval based on FPS
        frame_interval_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / config.fps));
        last_frame_time_ = std::chrono::steady_clock::now();
    }
    
    // Simulate one frame - ALWAYS sends IPC data
    void captureAndSendFrame() {
        // Increment frame counter
        frame_counter_++;
        
        // Prepare frame data
        std::vector<shaman::Object> detections;
        bool yolo_processing = false;
        
        // Simulate YOLO processing if enabled
        if (config_.enable_yolo_simulation) {
            yolo_processing = true;
            
            // Randomly decide if objects are detected this frame
            if (prob_dist_(rng_) < config_.detection_probability) {
                // Generate 1-3 simulated objects
                int num_objects = 1 + (rng_() % 3);
                
                for (int i = 0; i < num_objects; ++i) {
                    shaman::Object obj;
                    
                    // Simulate object moving in a circle
                    float radius = 200.0f + i * 50.0f;
                    float angle = object_angle_ + i * 0.5f;
                    
                    obj.rect.x = 640 + radius * cos(angle);
                    obj.rect.y = 360 + radius * sin(angle);
                    obj.rect.width = 40.0f + (rng_() % 40);
                    obj.rect.height = 60.0f + (rng_() % 40);
                    obj.label = i % 5;  // Simulate different object classes
                    obj.prob = 0.7f + prob_dist_(rng_) * 0.3f;  // 0.7 - 1.0 confidence
                    obj.num_kps = 0;  // No keypoints for this simulation
                    
                    detections.push_back(obj);
                }
                
                // Update angle for next frame
                object_angle_ += angular_velocity_;
                if (object_angle_ > 2 * M_PI) {
                    object_angle_ -= 2 * M_PI;
                }
            }
        }
        
        // ALWAYS send frame data (with or without detections)
        bool success = ipc_queue_->push(detections, frame_counter_, config_.camera_id, yolo_processing);
        
        if (success) {
            // Log based on what was sent
            if (detections.empty()) {
                if (frame_counter_ % 100 == 0) {  // Log every 100 frames to avoid spam
                    std::cout << "[Camera " << config_.camera_id 
                              << "] Sent frame " << frame_counter_ 
                              << " (no detections)" << std::endl;
                }
            } else {
                std::cout << "[Camera " << config_.camera_id 
                          << "] Sent frame " << frame_counter_ 
                          << " with " << detections.size() << " objects" << std::endl;
            }
        } else {
            std::cerr << "[Camera " << config_.camera_id 
                      << "] ERROR: Failed to send frame " << frame_counter_ 
                      << " (queue full?)" << std::endl;
        }
    }
    
    // Maintain target FPS
    void waitForNextFrame() {
        auto now = std::chrono::steady_clock::now();
        auto next_frame_time = last_frame_time_ + frame_interval_;
        
        if (now < next_frame_time) {
            std::this_thread::sleep_until(next_frame_time);
        }
        
        last_frame_time_ = std::chrono::steady_clock::now();
    }
    
    uint64_t getFrameCount() const { return frame_counter_; }
    uint32_t getCameraId() const { return config_.camera_id; }
};

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    
    std::cout << "=== Test Frame Producer ===" << std::endl;
    std::cout << "This producer sends EVERY frame via IPC for synchronization" << std::endl;
    std::cout << "Usage: " << argv[0] << " [mode]" << std::endl;
    std::cout << "Modes:" << std::endl;
    std::cout << "  0 - Frame-only mode (no YOLO simulation)" << std::endl;
    std::cout << "  1 - YOLO simulation with occasional detections" << std::endl;
    std::cout << "  2 - YOLO simulation with frequent detections" << std::endl;
    std::cout << "Press Ctrl+C to stop\n" << std::endl;
    
    // Parse mode argument
    int mode = 1;  // Default mode
    if (argc > 1) {
        mode = std::atoi(argv[1]);
    }
    
    // Configure cameras based on mode
    std::vector<CameraSimulator::Config> camera_configs;
    
    switch (mode) {
        case 0:  // Frame-only mode
            std::cout << "Running in FRAME-ONLY mode (no YOLO simulation)" << std::endl;
            camera_configs = {
                {2010093, 30.0f, false, 0.0f},
                {2010094, 30.0f, false, 0.0f},
                {2010095, 60.0f, false, 0.0f},
                {2010096, 60.0f, false, 0.0f}
            };
            break;
            
        case 1:  // YOLO with occasional detections
            std::cout << "Running in YOLO mode with OCCASIONAL detections" << std::endl;
            camera_configs = {
                {2010093, 30.0f, true, 0.3f},  // 30% chance of detections
                {2010094, 30.0f, true, 0.3f},
                {2010095, 60.0f, true, 0.3f},
                {2010096, 60.0f, true, 0.3f}
            };
            break;
            
        case 2:  // YOLO with frequent detections
            std::cout << "Running in YOLO mode with FREQUENT detections" << std::endl;
            camera_configs = {
                {2010093, 30.0f, true, 0.8f},  // 80% chance of detections
                {2010094, 30.0f, true, 0.8f},
                {2010095, 60.0f, true, 0.8f},
                {2010096, 60.0f, true, 0.8f}
            };
            break;
            
        default:
            std::cerr << "Invalid mode: " << mode << std::endl;
            return 1;
    }
    
    // Create camera simulators
    std::vector<std::unique_ptr<CameraSimulator>> cameras;
    for (const auto& config : camera_configs) {
        try {
            cameras.push_back(std::make_unique<CameraSimulator>(config));
            std::cout << "Initialized camera " << config.camera_id 
                      << " at " << config.fps << " FPS" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to initialize camera " << config.camera_id 
                      << ": " << e.what() << std::endl;
        }
    }
    
    if (cameras.empty()) {
        std::cerr << "No cameras initialized!" << std::endl;
        return 1;
    }
    
    std::cout << "\nStarting frame production..." << std::endl;
    
    // Main loop - simulate all cameras
    auto start_time = std::chrono::steady_clock::now();
    
    while (g_keep_running) {
        // Each camera runs at its own rate
        for (auto& camera : cameras) {
            camera->captureAndSendFrame();
            camera->waitForNextFrame();
        }
        
        // Periodic status update
        static auto last_status = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last_status).count() > 5.0f) {
            std::cout << "\n=== Status Update ===" << std::endl;
            auto runtime = std::chrono::duration<float>(now - start_time).count();
            for (const auto& camera : cameras) {
                float actual_fps = camera->getFrameCount() / runtime;
                std::cout << "Camera " << camera->getCameraId() 
                          << ": " << camera->getFrameCount() << " frames sent"
                          << " (avg " << std::fixed << std::setprecision(1) 
                          << actual_fps << " FPS)" << std::endl;
            }
            last_status = now;
        }
    }
    
    // Shutdown statistics
    std::cout << "\n=== Final Statistics ===" << std::endl;
    auto end_time = std::chrono::steady_clock::now();
    auto total_runtime = std::chrono::duration<float>(end_time - start_time).count();
    
    for (const auto& camera : cameras) {
        float actual_fps = camera->getFrameCount() / total_runtime;
        std::cout << "Camera " << camera->getCameraId() 
                  << ": " << camera->getFrameCount() << " total frames"
                  << " (" << std::fixed << std::setprecision(1) 
                  << actual_fps << " FPS average)" << std::endl;
    }
    
    std::cout << "Total runtime: " << std::fixed << std::setprecision(1) 
              << total_runtime << " seconds" << std::endl;
    
    return 0;
}