// frame_id_monitor.h
#ifndef FRAME_ID_MONITOR_H
#define FRAME_ID_MONITOR_H

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>

class FrameIDMonitor {
private:
    std::ofstream metadata_file_;
    std::mutex write_mutex_;
    std::unordered_map<std::string, uint64_t> last_frame_ids_;
    std::string log_path_;
    bool logging_enabled_;
    
public:
    FrameIDMonitor(const std::string& camera_serial, const std::string& base_path = "") 
        : logging_enabled_(false) {
        // Generate timestamp for unique file naming
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        
        // Construct file path - default to /home/jeremy when running as sudo
        if (base_path.empty()) {
            // Explicitly use /home/jeremy instead of HOME env variable
            // This ensures files go to the right place even when running as sudo
            log_path_ = "/home/jeremy/frame_id_monitor_" + camera_serial + "_" + ss.str() + ".csv";
        } else {
            log_path_ = base_path + "/frame_id_monitor_" + camera_serial + "_" + ss.str() + ".csv";
        }
        
        // Open file and write header
        metadata_file_.open(log_path_);
        if (metadata_file_.is_open()) {
            metadata_file_ << "timestamp_ns,camera_serial,frame_id,recording_frame_id,is_monotonic,gap_size\n";
            metadata_file_.flush();
            logging_enabled_ = true;
            std::cout << "[FrameIDMonitor] Logging to: " << log_path_ << std::endl;
        } else {
            std::cerr << "[FrameIDMonitor] Failed to open file: " << log_path_ << std::endl;
        }
    }
    
    ~FrameIDMonitor() {
        if (metadata_file_.is_open()) {
            metadata_file_.close();
            std::cout << "[FrameIDMonitor] Closed log file: " << log_path_ << std::endl;
        }
    }
    
    // Log a received frame with monotonicity check
    void logFrame(const std::string& camera_serial, 
                  uint64_t frame_id, 
                  uint64_t recording_frame_id = 0) {
        if (!logging_enabled_) return;
        
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        // Get current timestamp
        auto now = std::chrono::high_resolution_clock::now();
        auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        
        // Check monotonicity
        bool is_monotonic = true;
        int64_t gap_size = 0;
        
        auto it = last_frame_ids_.find(camera_serial);
        if (it != last_frame_ids_.end()) {
            uint64_t last_frame_id = it->second;
            if (frame_id <= last_frame_id) {
                is_monotonic = false;
                gap_size = static_cast<int64_t>(frame_id) - static_cast<int64_t>(last_frame_id);
                std::cerr << "[FrameIDMonitor] WARNING: Non-monotonic frame ID for camera " 
                         << camera_serial << ": " << last_frame_id << " -> " << frame_id 
                         << " (gap: " << gap_size << ")" << std::endl;
            } else {
                gap_size = static_cast<int64_t>(frame_id) - static_cast<int64_t>(last_frame_id);
                if (gap_size > 1) {
                    std::cout << "[FrameIDMonitor] Frame gap detected for camera " 
                             << camera_serial << ": " << last_frame_id << " -> " << frame_id 
                             << " (gap: " << gap_size << ")" << std::endl;
                }
            }
        }
        
        // Update last frame ID for this camera
        last_frame_ids_[camera_serial] = frame_id;
        
        // Write to file
        metadata_file_ << timestamp_ns << ","
                      << camera_serial << ","
                      << frame_id << ","
                      << recording_frame_id << ","
                      << (is_monotonic ? "true" : "false") << ","
                      << gap_size << "\n";
        metadata_file_.flush();
    }
    
    // Get statistics for a camera
    void printStats(const std::string& camera_serial = "") {
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        if (camera_serial.empty()) {
            std::cout << "\n[FrameIDMonitor] Statistics for all cameras:" << std::endl;
            for (const auto& pair : last_frame_ids_) {
                std::cout << "  Camera " << pair.first << ": last frame_id = " << pair.second << std::endl;
            }
        } else {
            auto it = last_frame_ids_.find(camera_serial);
            if (it != last_frame_ids_.end()) {
                std::cout << "[FrameIDMonitor] Camera " << camera_serial 
                         << ": last frame_id = " << it->second << std::endl;
            } else {
                std::cout << "[FrameIDMonitor] No frames logged for camera " << camera_serial << std::endl;
            }
        }
    }
    
    // Check if logging is active
    bool isLogging() const {
        return logging_enabled_;
    }
    
    // Get the log file path
    std::string getLogPath() const {
        return log_path_;
    }
};

// Example integration into your existing code:
// 
// In acquire_frames.cpp or main initialization:
//   std::shared_ptr<FrameIDMonitor> frame_monitor = std::make_shared<FrameIDMonitor>();
//
// When a frame is received (in your frame processing loop):
//   frame_monitor->logFrame(camera_params->camera_serial, current_entry->frame_id, current_entry->recording_frame_id);
//
// To check statistics:
//   frame_monitor->printStats();

#endif // FRAME_ID_MONITOR_H