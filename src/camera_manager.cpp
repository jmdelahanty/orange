#pragma once

#include <vector>
#include <memory>
#include <string>
#include "emergent_camera.h"
#include "frame_streaming.h"
#include "gpu_streaming.h"
#include "camera_params.h"

namespace evt {

class CameraManager {
public:
    // Structure to hold camera instance and its associated objects
    struct CameraInstance {
        std::unique_ptr<EmergentCamera> camera;
        std::unique_ptr<GPUStreaming> gpu_stream;
        GPUStreaming::StreamingConfig config;
        CameraParams params;
        bool is_streaming{false};
        bool is_recording{false};
    };

    CameraManager() = default;
    ~CameraManager() = default;

    void initializeCameras(const std::vector<bool>& selected_cameras,
                          const std::vector<GigEVisionDeviceInfo>& device_info,
                          const std::vector<std::string>& config_files,
                          const std::unordered_map<std::string, CameraParams>& known_configs) {
        cameras.clear();
        cameras.reserve(device_info.size());
        
        LOG(INFO) << "Starting camera initialization with " << device_info.size() << " devices";
        
        for (size_t i = 0; i < device_info.size(); ++i) {
            if (!selected_cameras[i]) continue;

            LOG(INFO) << "Initializing camera " << i << " (serial: " << device_info[i].serialNumber << ")";
            
            CameraInstance instance;
            
            // Look up configuration
            std::string serial(device_info[i].serialNumber);
            auto config_it = known_configs.find(serial);
            if (config_it != known_configs.end()) {
                LOG(INFO) << "Found configuration for camera " << serial;
                instance.params = config_it->second;
                
                // Create and open camera first to get valid ranges
                instance.camera = std::make_unique<EmergentCamera>(instance.params);
                instance.camera->open(&device_info[i]);
                
                // Get ranges after opening camera
                instance.camera->updateCameraRanges();
                const auto& ranges = instance.camera->getResolutionRange();
                
                // Now validate and clamp the loaded config values
                instance.params.width = std::clamp(instance.params.width, 
                    static_cast<int>(ranges.width_min), 
                    static_cast<int>(ranges.width_max));
                instance.params.height = std::clamp(instance.params.height,
                    static_cast<int>(ranges.height_min), 
                    static_cast<int>(ranges.height_max));
                    
                LOG(INFO) << "Verified camera settings:"
                        << "\n  Width: " << instance.params.width
                        << "\n  Height: " << instance.params.height
                        << "\n  Exposure: " << instance.params.exposure
                        << "\n  Frame Rate: " << instance.params.frame_rate
                        << "\n  Gain: " << instance.params.gain;
            } else {
                // Fall back to defaults
                instance.params.camera_serial = serial;
                instance.params.camera_name = device_info[i].userDefinedName;
                LOG(INFO) << "No config found for camera " << serial << ", using defaults";
            }
            
            // Always log the current camera state
            instance.camera->logCurrentState("Initial camera state");
            cameras.push_back(std::move(instance));
            LOG(INFO) << "Successfully initialized camera " << i;
        }
    }

    // Start/stop streaming for specific camera
    void startStreaming(size_t camera_idx, bool enable_gpu = false) {
        if (camera_idx >= cameras.size()) return;
        
        auto& instance = cameras[camera_idx];
        if (instance.is_streaming) return;

        try {
            // First ensure camera is open and configured
            if (!instance.camera->isOpen()) {
                // Configure GPU direct before opening camera
                if (instance.params.gpu_direct) {
                    // The GPU configuration is now handled inside EmergentCamera
                    instance.params.gpu_id = enable_gpu ? 0 : -1;  // Set GPU ID based on enable_gpu flag
                }

                instance.camera->open(&instance.params.device_info);
                
                // Update all camera parameters in correct order
                instance.camera->updateResolution(instance.params.width, instance.params.height);
                instance.camera->updateOffset(0, 0);  // Reset offset first
                instance.camera->updatePixelFormat(instance.params.pixel_format);
                
                if (instance.params.color) {
                    instance.camera->setParameter("ColorTemp", instance.params.color_temp);
                }
                
                // Update core parameters
                instance.camera->updateGain(instance.params.gain);
                instance.camera->updateExposure(instance.params.exposure);
                instance.camera->updateFrameRate(instance.params.frame_rate);
                instance.camera->updateFocus(instance.params.focus);
                instance.camera->updateIris(instance.params.iris);
            }

            LOG(INFO) << "Attempting to allocate frame buffers for camera " << camera_idx;
            
            // Get current camera state before allocation
            instance.camera->logCurrentState("Before frame buffer allocation");
            
            // Allocate frame buffers before starting stream
            constexpr int default_buffer_count = 32;
            try {
                instance.camera->allocateFrameBuffers(default_buffer_count);
            } catch (const CameraException& e) {
                LOG(ERROR) << "Frame buffer allocation failed with error code: " 
                          << e.getErrorCode() << "\n" << e.what();
                throw;
            }

            // Start the actual stream
            try {
                instance.camera->startStream();
            } catch (const CameraException& e) {
                LOG(ERROR) << "Stream start failed after successful buffer allocation: " 
                          << e.what();
                throw;
            }

            // Configure GPU streaming if requested
            if (enable_gpu) {
                // Configure GPU streaming
                instance.config.enable_gpu_direct = true;
                instance.config.gpu_device_id = instance.params.gpu_id;
                instance.gpu_stream = std::make_unique<GPUStreaming>(
                    *instance.camera,
                    instance.config
                );
                
                // Start GPU streaming
                instance.gpu_stream->startStreaming([](const void* data, 
                                                   size_t size,
                                                   int width, 
                                                   int height,
                                                   uint64_t timestamp) {
                    // Frame callback - integrate with GUI display
                });
            }
            
            instance.is_streaming = true;
            LOG(INFO) << "Camera streaming started successfully";
        }
        catch (const CameraException& e) {
            // Make sure to clean up on failure
            if (instance.gpu_stream) {
                instance.gpu_stream->stopStreaming();
                instance.gpu_stream.reset();
            }
            instance.is_streaming = false;
            LOG(ERROR) << "Failed to start streaming: " << e.what();
            throw;
        }
    }

    void stopStreaming(size_t camera_idx) {
        if (camera_idx >= cameras.size()) return;
        
        auto& instance = cameras[camera_idx];
        if (!instance.is_streaming) return;

        try {
            if (instance.gpu_stream) {
                instance.gpu_stream->stopStreaming();
            }
            
            instance.is_streaming = false;
        }
        catch (const CameraException& e) {
            std::cerr << "Error stopping stream: " << e.what() << std::endl;
            throw;
        }
    }

    // Start/stop recording
    void startRecording(const std::string& output_folder,
                       const std::string& encoder_setup) {
        if (recording_active) return;
        
        try {
            for (auto& instance : cameras) {
                if (instance.is_streaming) {
                    // Configure recording
                    instance.config.enable_encoding = true;
                    instance.config.output_folder = output_folder;
                    instance.config.encoder_setup = encoder_setup;
                    
                    // Restart streaming with recording enabled
                    if (instance.gpu_stream) {
                        instance.gpu_stream->stopStreaming();
                        setupGPUStreaming(instance, output_folder, encoder_setup);
                        instance.gpu_stream->startStreaming([](const void* data, 
                                                           size_t size,
                                                           int width, 
                                                           int height,
                                                           uint64_t timestamp) {
                            // Frame callback for recording
                        });
                    }
                }
            }
            
            recording_active = true;
        }
        catch (const CameraException& e) {
            std::cerr << "Failed to start recording: " << e.what() << std::endl;
            throw;
        }
    }

    void stopRecording() {
        if (!recording_active) return;
        
        try {
            for (auto& instance : cameras) {
                if (instance.is_streaming && instance.gpu_stream) {
                    // Restart streaming without recording
                    instance.gpu_stream->stopStreaming();
                    instance.config.enable_encoding = false;
                    setupGPUStreaming(instance, "", "");
                    instance.gpu_stream->startStreaming([](const void* data, 
                                                       size_t size,
                                                       int width, 
                                                       int height,
                                                       uint64_t timestamp) {
                        // Frame callback for display only
                    });
                }
            }
            
            recording_active = false;
        }
        catch (const CameraException& e) {
            std::cerr << "Error stopping recording: " << e.what() << std::endl;
            throw;
        }
    }

    // Camera control methods
    void updateExposure(size_t camera_idx, int exposure) {
        if (camera_idx >= cameras.size()) return;
        cameras[camera_idx].camera->updateExposure(exposure);
    }

    void updateGain(size_t camera_idx, int gain) {
        if (camera_idx >= cameras.size()) return;
        cameras[camera_idx].camera->updateGain(gain);
    }

    void updateFrameRate(size_t camera_idx, int frame_rate) {
        if (camera_idx >= cameras.size()) return;
        cameras[camera_idx].camera->updateFrameRate(frame_rate);
    }

    // Status queries
    size_t getCameraCount() const { return cameras.size(); }
    bool isStreaming(size_t camera_idx) const { 
        return camera_idx < cameras.size() && cameras[camera_idx].is_streaming;
    }
    bool isRecording() const { return recording_active; }
    
    // Access to camera instances for GUI
    const CameraInstance& getCamera(size_t idx) const { return cameras[idx]; }
    CameraInstance& getCamera(size_t idx) { return cameras[idx]; }

    // Add a safe method to get camera parameters
    const CameraParams& getCameraParams(size_t idx) const {
        if (idx >= cameras.size() || !cameras[idx].camera) {
            static const CameraParams default_params;
            return default_params;
        }
        return cameras[idx].params;
    }

private:
    std::vector<CameraInstance> cameras;
    bool recording_active{false};
    
    void loadCameraConfig(CameraInstance& instance, 
                         const std::string& config_file,
                         const GigEVisionDeviceInfo& device_info) {
        // TODO: Implement configuration loading
    }

    void setupGPUStreaming(CameraInstance& instance,
                          const std::string& output_folder,
                          const std::string& encoder_setup) {
        // TODO: Implement GPU streaming setup
    }
};

} // namespace evt