// frame_ipc_manager.h
#pragma once

#include "shaman.h"
#include "camera.h"
#include <memory>
#include <string>
#include <atomic>
#include <iostream>

class FrameIPCManager {
public:
    FrameIPCManager(CameraParams* camera_params)
        : camera_params_(camera_params),
          frames_sent_(0),
          last_frame_id_(0),
          frames_with_detections_(0),
          frames_without_detections_(0) {
        
        // Create per-camera queue name
        queue_name_ = "/shm_cam_" + camera_params_->camera_serial;
        
        try {
            ipc_queue_ = std::make_unique<shaman::SharedBoxQueue>(
                queue_name_.c_str(), true /* is_writer */);
            std::cout << "[FrameIPCManager] Created IPC queue: " << queue_name_ 
                      << " for camera " << camera_params_->camera_serial 
                      << " (ID: " << camera_params_->camera_id << ")" << std::endl;
            enabled_ = true;
        } catch (const std::exception& e) {
            std::cerr << "[FrameIPCManager] Failed to create IPC queue: " 
                      << e.what() << std::endl;
            enabled_ = false;
        }
    }
    
    ~FrameIPCManager() {
        if (enabled_ && frames_sent_ > 0) {
            // std::cout << "[FrameIPCManager] Camera " << camera_params_->camera_serial 
            //           << " shutdown stats:" << std::endl
            //           << "  - Total frames sent: " << frames_sent_ << std::endl
            //           << "  - With detections: " << frames_with_detections_ << std::endl
            //           << "  - Without detections: " << frames_without_detections_ << std::endl;
        }
    }
    
    // Send frame data - called for EVERY frame
    bool sendFrame(uint64_t frame_id, 
                   uint64_t timestamp,
                   const std::vector<shaman::Object>& detections = {},
                   bool yolo_processing = false) {
        if (!enabled_ || !ipc_queue_) return false;
        
        // Verify frame ID is monotonically increasing
        if (last_frame_id_ != 0 && frame_id != last_frame_id_ + 1) {
            std::cerr << "[FrameIPCManager] WARNING: Frame gap detected! Camera " 
                      << camera_params_->camera_id
                      << " jumped from " << last_frame_id_ 
                      << " to " << frame_id << std::endl;
        }
        
        // Send the frame data
        bool success = ipc_queue_->push(detections, frame_id, 
                                        camera_params_->camera_id, 
                                        yolo_processing);
        
        if (success) {
            frames_sent_++;
            last_frame_id_ = frame_id;
            
            if (detections.empty()) {
                frames_without_detections_++;
            } else {
                frames_with_detections_++;
            }
            
            // Log periodically to avoid spam
            // if (frames_sent_ % 100 == 0) {
            //     std::cout << "[FrameIPCManager] Camera " << camera_params_->camera_serial 
            //               << " sent " << frames_sent_ << " frames "
            //               << "(" << frames_with_detections_ << " with detections)" << std::endl;
            // }
        } else {
            // std::cerr << "[FrameIPCManager] Queue full! Camera " << camera_params_->camera_serial
            //           << " dropped frame " << frame_id << std::endl;
        }
        
        return success;
    }
    
    // Update existing frame with detection data (called from YOLO worker)
    bool updateFrameWithDetections(uint64_t frame_id,
                                   const std::vector<shaman::Object>& detections) {
        if (!enabled_ || !ipc_queue_) return false;
        
        // Send update with detections
        return ipc_queue_->push(detections, frame_id, 
                               camera_params_->camera_id, 
                               true /* yolo_processing = true */);
    }
    
    bool isEnabled() const { return enabled_; }
    const std::string& getQueueName() const { return queue_name_; }
    uint64_t getFramesSent() const { return frames_sent_; }
    
private:
    CameraParams* camera_params_;
    bool enabled_ = false;
    std::unique_ptr<shaman::SharedBoxQueue> ipc_queue_;
    std::string queue_name_;
    std::atomic<uint64_t> frames_sent_;
    std::atomic<uint64_t> last_frame_id_;
    std::atomic<uint64_t> frames_with_detections_;
    std::atomic<uint64_t> frames_without_detections_;
};