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

YOLOv8Worker::YOLOv8Worker(const char* name,
                           CameraParams* cam_params,
                           CameraEachSelect* cam_select,
                           SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
                           SafeQueue<ProcessedFrame*>& processed_recycle_queue)
    : CThreadWorker(name),
      yolov8_instance_(nullptr),
      associated_camera_params_(cam_params),
      associated_camera_select_(cam_select),
      enet_host_context_(nullptr),
      enet_target_peer_(nullptr),
      fb_builder_(nullptr),
      last_fps_update_time_(std::chrono::steady_clock::now()),
      frame_counter_(0),
      current_fps_(0.0),
      shaman_ipc_queue_(nullptr),
      m_raw_recycle_queue(raw_recycle_queue),
      m_processed_recycle_queue(processed_recycle_queue),
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

        if (associated_camera_select_->yolo && associated_camera_select_->send_yolo_via_ipc) {
            shaman_ipc_queue_ = new shaman::SharedBoxQueue(true /* is_writer */);
        }

        std::cout << "YOLOv8Worker for " << name << " initialized successfully." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "YOLOv8Worker Error for " << name << ": " << e.what() << std::endl;
        
        if (fb_builder_) { delete fb_builder_; fb_builder_ = nullptr; }
        if (yolov8_instance_) { delete yolov8_instance_; yolov8_instance_ = nullptr; }
        
        throw;
    }
}

YOLOv8Worker::~YOLOv8Worker() {
    std::cout << "YOLOv8Worker destructor for " << threadName << std::endl;
    
    if (associated_camera_params_) {
        ck(cudaSetDevice(associated_camera_params_->gpu_id));
        if (yolov8_instance_) { delete yolov8_instance_; }
    }
    
    if (shaman_ipc_queue_) delete shaman_ipc_queue_;
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


bool YOLOv8Worker::WorkerFunction(ProcessedFrame* frame) {
    if (!yolov8_instance_ || !frame || !frame->d_processed_image) {
        if (frame) {
            // If something is wrong with the frame, we must still handle recycling
            if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                m_raw_recycle_queue.push(frame->original_entry);
                m_processed_recycle_queue.push(frame);
            }
        }
        return false;
    }

    // Set the CUDA device for this thread.
    ck(cudaSetDevice(associated_camera_params_->gpu_id));

    try {
        nppSetStream(yolov8_instance_->stream);

        // Wait for the FramePreprocessor to finish its work
        if (frame->processed_event_ptr) {
            ck(cudaStreamWaitEvent(yolov8_instance_->stream, *frame->processed_event_ptr, 0));
        }

        // --- THE BIG CHANGE ---
        // The frame is already RGBA. We can use it directly.
        // The 'is_color' flag is now true because the input is RGBA.
        yolov8_instance_->preprocess_gpu(frame->d_processed_image, frame->width, frame->height, true);

        // Preprocess and run inference. These are non-blocking CUDA calls.
        yolov8_instance_->infer();

        // This event now signals that YOLO's GPU work is done.
        // It should be part of the ProcessedFrame struct if other workers need to wait on it.
        // For now, we'll synchronize here.
        ck(cudaStreamSynchronize(yolov8_instance_->stream));
        
        // Now that the GPU is finished, process the results.
        yolov8_instance_->postprocess(frame->detections);

        // After detections are found, dispatch to the crop worker if it exists
        if (m_crop_worker && !frame->detections.empty()) {
            frame->ref_count.fetch_add(1, std::memory_order_acq_rel); // Increment ref count for the new worker
            // Note: The crop worker will also need to be updated to accept a ProcessedFrame*
            // m_crop_worker->PutObjectToQueueIn(frame);
        }
        
        frame->detections_ready.store(true);

        // FPS calculation and IPC/ENet logic remains the same.
        frame_counter_++;
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_fps_update_time_;
        if (elapsed.count() >= 1.0) {
            current_fps_.store(frame_counter_ / elapsed.count());
            std::cout << "[" << this->threadName << "] Inference FPS: " << current_fps_.load()
                      << " (Queue depth: " << this->GetCountQueueInSize() << ")" << std::endl;
            frame_counter_ = 0;
            last_fps_update_time_ = now;
        }

        if (frame->has_detections) {
            if (associated_camera_select_->send_yolo_via_ipc && shaman_ipc_queue_) {
                std::vector<shaman::Object> shaman_objects = conv_shaman(frame->detections);
                if (!shaman_ipc_queue_->push(shaman_objects, frame->frame_id, associated_camera_params_->camera_id)) {
                    std::cerr << "[" << threadName << "] Failed to push to IPC queue." << std::endl;
                }
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[" << threadName << "] Exception in WorkerFunction: " << e.what() << std::endl;
        if (yolov8_instance_ && yolov8_instance_->stream) {
            cudaStreamSynchronize(yolov8_instance_->stream);
        }
    }

    // --- NEW RECYCLING LOGIC ---
    // This worker is now responsible for recycling both structs when it's the last one.
    if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Recycle the raw entry from the acquisition stage
        if (frame->original_entry->gpu_direct_mode && frame->original_entry->camera_instance && frame->original_entry->camera_frame_struct) {
            EVT_CameraQueueFrame(frame->original_entry->camera_instance, frame->original_entry->camera_frame_struct);
        }
        m_raw_recycle_queue.push(frame->original_entry);
        
        // Recycle the processed frame struct itself
        m_processed_recycle_queue.push(frame);
    }
    
    // This worker doesn't pass an item to its own output queue so we return false
    return false;
}

void YOLOv8Worker::WorkerReset() {
    last_fps_update_time_ = std::chrono::steady_clock::now();
    frame_counter_ = 0;
    current_fps_ = 0.0;
}

double YOLOv8Worker::get_fps() const {
    return current_fps_.load(std::memory_order_relaxed);
}