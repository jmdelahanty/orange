// src/yolo_worker.cpp
#include "yolo_worker.h"
#include "kernel.cuh"
#include <cuda_runtime_api.h>
#include <nppi.h>
#include <npp.h>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include "yolo_payload_generated.h"
#include "message_wrapper_generated.h"
#include "opengldisplay.h"
#include "pose_shaman.h"
#include "thread.h"
#include "global.h"
#include "cuda_context_debug.h"
#include "opencv2/opencv.hpp"
#include "crop_and_encode_worker.h"
#include "frame_ipc_manager.h"

YOLOv8Worker::YOLOv8Worker(const char* name,
                           CameraParams* cam_params,
                           CameraEachSelect* cam_select,
                           CameraControl* camera_control,
                           SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker(name),
      yolov8_instance_(nullptr),
      associated_camera_params_(cam_params),
      associated_camera_select_(cam_select),
      camera_control_(camera_control),
      enet_host_context_(nullptr),
      enet_target_peer_(nullptr),
      fb_builder_(nullptr),
      last_fps_update_time_(std::chrono::steady_clock::now()),
      frame_counter_(0),
      current_fps_(0.0),
      m_recycle_queue(recycle_queue),
      m_dump_next_frame(false)
{
    ck(cudaSetDevice(associated_camera_params_->gpu_id));
    std::cout << "YOLOv8Worker constructor set to CUDA device: " << associated_camera_params_->gpu_id << std::endl;

    try {
        if (!associated_camera_params_ || !associated_camera_select_) {
            throw std::runtime_error("CameraParams or CameraEachSelect is null.");
        }

        fb_builder_ = new flatbuffers::FlatBufferBuilder(1024 * 4);

        if (associated_camera_select_->yolo_model == nullptr || strlen(associated_camera_select_->yolo_model) == 0) {
            throw std::runtime_error("YOLO model path is null or empty. Cannot initialize YOLOv8.");
        }

        yolov8_instance_ = new YOLOv8(associated_camera_select_->yolo_model,
                                     associated_camera_params_->width,
                                     associated_camera_params_->height);
        yolov8_instance_->make_pipe(true);

        std::cout << "[YOLOv8 MODEL INFO] " << name << " expects input size: "
                  << yolov8_instance_->inp_w_int << "x" << yolov8_instance_->inp_h_int << std::endl;

        initalize_gpu_frame(&frame_original_gpu_, associated_camera_params_);
        initialize_gpu_debayer(&debayer_gpu_, associated_camera_params_);

        std::cout << "YOLOv8Worker for " << name << " initialized successfully." << std::endl;
        std::cout << "[NOTE] Frame IPC handled by acquire_frames. YOLO updates frames with detection data." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "YOLOv8Worker Error for " << name << ": " << e.what() << std::endl;

        if (fb_builder_) { delete fb_builder_; fb_builder_ = nullptr; }
        if (yolov8_instance_) { delete yolov8_instance_; yolov8_instance_ = nullptr; }
        if (frame_original_gpu_.d_orig) { cudaFree(frame_original_gpu_.d_orig); frame_original_gpu_.d_orig = nullptr; }
        if (debayer_gpu_.d_debayer) { cudaFree(debayer_gpu_.d_debayer); debayer_gpu_.d_debayer = nullptr; }

        throw;
    }
}

YOLOv8Worker::~YOLOv8Worker() {
    std::cout << "YOLOv8Worker destructor for " << threadName << std::endl;

    if (associated_camera_params_) {
        ck(cudaSetDevice(associated_camera_params_->gpu_id));
        if (debayer_gpu_.d_debayer) { cudaFree(debayer_gpu_.d_debayer); }
        if (frame_original_gpu_.d_orig) { cudaFree(frame_original_gpu_.d_orig); }
        if (yolov8_instance_) { delete yolov8_instance_; }
    }

    if (fb_builder_) delete fb_builder_;

    std::cout << "YOLOv8Worker destructor complete for " << threadName << std::endl;
}

void YOLOv8Worker::SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker) {
    m_crop_worker = crop_worker;
}

void YOLOv8Worker::SetDisplayWorker(COpenGLDisplay* display_worker) {
    m_display_worker = display_worker;
}

void YOLOv8Worker::SetENetTarget(EnetContext* host_ctx, ENetPeer* target_peer)
{
    std::cout << "YOLOv8Worker (" << this->threadName
              << "): SetENetTarget called. Host_ctx: " << static_cast<void*>(host_ctx)
              << ". Target_peer: " << static_cast<void*>(target_peer) << std::endl;
    enet_host_context_ = host_ctx;
    enet_target_peer_ = target_peer;
}

bool YOLOv8Worker::WorkerFunction(WORKER_ENTRY* entry) {
    if (!yolov8_instance_ || !entry || !entry->d_image) {
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_recycle_queue.push(entry);
        }
        return false;
    }

    // Set the CUDA device for this thread.
    ck(cudaSetDevice(associated_camera_params_->gpu_id));

    try {
        const int camera_width = associated_camera_params_->width;
        const int camera_height = associated_camera_params_->height;

        // Set the NPP stream to the one used by the YOLO instance.
        nppSetStream(yolov8_instance_->stream);

        // Wait for the previous stage (acquire_frames) to finish copying data.
        if (entry->event_ptr) {
            ck(cudaStreamWaitEvent(yolov8_instance_->stream, *entry->event_ptr, 0));
        }

        frame_original_gpu_.d_orig = entry->d_image;
        debayer_gpu_.size.width = camera_width;
        debayer_gpu_.size.height = camera_height;

        // Debayer or duplicate mono channel to prepare for color conversion.
        if (associated_camera_params_->color) {
            // If color, debayer to RGBA first, then our kernel will handle the rest.
            debayer_frame_gpu(associated_camera_params_, &frame_original_gpu_, &debayer_gpu_);
            yolov8_instance_->preprocess_gpu(debayer_gpu_.d_debayer, camera_width, camera_height, true);
        } else {
            // If mono, pass the raw mono buffer directly to the kernel.
            yolov8_instance_->preprocess_gpu(frame_original_gpu_.d_orig, camera_width, camera_height, false);
        }

        // Logic for dumping a debug frame if requested.
        bool dump_this_frame = m_dump_next_frame.exchange(false);
        if (dump_this_frame)
        {
            size_t image_size_bytes = (size_t)camera_width * camera_height * 4;
            unsigned char* h_rgba_buffer = new unsigned char[image_size_bytes];
            ck(cudaMemcpy(h_rgba_buffer, debayer_gpu_.d_debayer, image_size_bytes, cudaMemcpyDeviceToHost));
            try {
                cv::Mat rgba_image(camera_height, camera_width, CV_8UC4, h_rgba_buffer);
                cv::Mat bgr_image;
                cv::cvtColor(rgba_image, bgr_image, cv::COLOR_RGBA2BGR);
                std::string filename = "debug_pre_yolo_" + std::string(associated_camera_params_->camera_serial) + "_" + std::to_string(entry->frame_id) + ".png";
                cv::imwrite(filename, bgr_image);
                std::cout << "[" << threadName << "] Saved debug image to " << filename << std::endl;
            } catch (const cv::Exception& ex) {
                std::cerr << "OpenCV exception while saving debug image: " << ex.what() << std::endl;
            }
            delete[] h_rgba_buffer;
        }

        // Preprocess and run inference. These are non-blocking CUDA calls.
        auto inference_start_time = std::chrono::steady_clock::now();
        yolov8_instance_->infer();

        // record per-frame event for synchronization
        if (entry->yolo_completion_event) {
            ck(cudaEventRecord(*entry->yolo_completion_event, yolov8_instance_->stream));
        }

        // Wait for the GPU to finish, with a timeout.
        const int timeout_us = 100000; // 100ms timeout
        bool finished_in_time = false;

        while (true) {
            cudaError_t result = cudaStreamQuery(yolov8_instance_->stream);
            if (result == cudaSuccess) {
                finished_in_time = true;
                break;
            }
            if (result != cudaErrorNotReady) {
                // An actual error occurred
                ck(result);
                break;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - inference_start_time).count();

            if (elapsed_us > timeout_us) {
                std::cerr << "[YOLOv8Worker] WARNING: Inference timed out after " << elapsed_us << "us. Dropping frame." << std::endl;
                break; // Timed out
            }

            // Wait for a very short time before polling again
            usleep(100);
        }

        if (finished_in_time) {
            // Now that the GPU is finished, process the results.
            // This completely REPLACES entry->detections, preventing stale data
            yolov8_instance_->postprocess(entry->detections);
        } else {
            // Timed out, clear any potential partial results
            entry->detections.clear();
        }

        uint64_t current_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();

        // Update velocity tracking with new detections
        velocity_tracker_.updateTracking(entry->detections, current_timestamp_us);

        // After detections are found, dispatch to the crop worker if it exists AND recording is on
        if (m_crop_worker && camera_control_->record_video) {
            // Increment the reference count because another worker will now use this entry
            entry->ref_count.fetch_add(1, std::memory_order_acq_rel);
            m_crop_worker->PutObjectToQueueIn(entry);
        }

        // Mark if we have detections
        entry->has_detections = !entry->detections.empty();
        entry->detections_ready.store(true);

        // NEW: Update Frame IPC with YOLO detection results
        if (entry->frame_ipc_manager && entry->has_detections) {
            FrameIPCManager* frame_ipc = static_cast<FrameIPCManager*>(entry->frame_ipc_manager);
            if (frame_ipc && frame_ipc->isEnabled()) {
                // Convert detections to shaman format for IPC
                std::vector<shaman::Object> shaman_objects = conv_shaman(entry->detections);

                uint64_t frame_id = (camera_control_->record_video && entry->recording_frame_id > 0)
                                   ? entry->recording_frame_id
                                   : entry->frame_id;

                // Update the frame with detection data
                frame_ipc->updateFrameWithDetections(frame_id, shaman_objects);
            }
        }

        // Handle ENet sending if configured
        if (enet_target_peer_ && associated_camera_select_->send_yolo_via_enet && !entry->detections.empty()) {
            // ENet code remains unchanged
        }

    } catch (const std::exception& e) {
        std::cerr << "[" << threadName << "] Exception in WorkerFunction: " << e.what() << std::endl;
        if (yolov8_instance_ && yolov8_instance_->stream) {
            cudaStreamSynchronize(yolov8_instance_->stream);
        }
    }

    // Reference counting for recycling the WORKER_ENTRY.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue.push(entry);
    }

    // This worker doesn't pass an item to its own output queue so we return false
    return false;
}

void YOLOv8Worker::WorkerReset() {
    last_fps_update_time_ = std::chrono::steady_clock::now();
    frame_counter_ = 0;
    current_fps_ = 0.0;
}