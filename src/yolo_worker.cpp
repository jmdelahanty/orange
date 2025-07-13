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

// CHANGE 1: Update the constructor to accept and store the input queue
YOLOv8Worker::YOLOv8Worker(const char* name,
                           CameraParams* cam_params,
                           CameraEachSelect* cam_select,
                           SafeQueue<ProcessedFrame*>* input_queue, // <-- New parameter
                           SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
                           SafeQueue<ProcessedFrame*>& processed_recycle_queue)
    : CThreadWorker(name),
      m_input_queue(input_queue), // <-- Store the queue
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
      m_recycle_queue(raw_recycle_queue),
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

        initalize_gpu_frame(&frame_original_gpu_, associated_camera_params_);
        initialize_gpu_debayer(&debayer_gpu_, associated_camera_params_);

        if (associated_camera_select_->yolo && associated_camera_select_->send_yolo_via_ipc) {
            shaman_ipc_queue_ = new shaman::SharedBoxQueue(true /* is_writer */);
        }

        std::cout << "YOLOv8Worker for " << name << " initialized successfully." << std::endl;

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
        if (yolov8_instance_) { delete yolov8_instance_; }
    }

    if (shaman_ipc_queue_) delete shaman_ipc_queue_;
    if (fb_builder_) delete fb_builder_;

    std::cout << "YOLOv8Worker destructor complete for " << threadName << std::endl;
}

// CHANGE 2: Implement the overridden ThreadRunning function.
void YOLOv8Worker::ThreadRunning()
{
    printf("YOLOv8Worker Thread Start %d\n", GetID());
    while (IsMachineOn())
    {
        ProcessedFrame* f = nullptr;
        // The crucial change: Pop from the shared input queue, not the internal one.
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                // The base class would decide whether to push to an output queue.
                // Since this worker has complex dispatch logic inside its WorkerFunction,
                // we just call it directly. The return value is ignored.
                WorkerFunction(f);
            }
        }
        else
        {
            // Sleep if the queue is empty to avoid busy-waiting
            usleep(1000); // 1ms sleep
        }
    }

    // Process any remaining items in the queue after the stop signal is received
    while (true)
    {
        ProcessedFrame* f = nullptr;
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                WorkerFunction(f);
            }
        }
        else
        {
            break; // Exit when the queue is empty
        }
    }
    printf("YOLOv8Worker Thread DONE %d\n", GetID());
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

// The WorkerFunction remains the same, as its logic is sound for a single frame.
bool YOLOv8Worker::WorkerFunction(ProcessedFrame* frame) {
    if (!yolov8_instance_ || !frame || !frame->d_processed_image) {
        if (frame && frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_recycle_queue.push(frame->original_entry);
            m_processed_recycle_queue.push(frame);
        }
        return false;
    }

    ck(cudaSetDevice(associated_camera_params_->gpu_id));

    try {
        nppSetStream(yolov8_instance_->stream);
        yolov8_instance_->preprocess_gpu(frame->d_processed_image, frame->width, frame->height, true);
        yolov8_instance_->infer();
        ck(cudaStreamSynchronize(yolov8_instance_->stream));
        
        yolov8_instance_->postprocess(frame->detections);
        frame->has_detections = !frame->detections.empty();
        frame->detections_ready.store(true);

        if (m_crop_worker && !frame->detections.empty()) {
            frame->ref_count.fetch_add(1, std::memory_order_acq_rel);
            // m_crop_worker->PutObjectToQueueIn(frame); // This will need to be adapted
        }

        if (frame->has_detections && associated_camera_select_->send_yolo_via_ipc && shaman_ipc_queue_) {
            std::vector<shaman::Object> shaman_objects = conv_shaman(frame->detections);
            if (!shaman_ipc_queue_->push(shaman_objects, frame->frame_id, associated_camera_params_->camera_id)) {
                std::cerr << "[" << threadName << "] Failed to push to IPC queue." << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[" << threadName << "] Exception in WorkerFunction: " << e.what() << std::endl;
        if (yolov8_instance_ && yolov8_instance_->stream) {
            cudaStreamSynchronize(yolov8_instance_->stream);
        }
    }

    if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_recycle_queue.push(frame->original_entry);
        m_processed_recycle_queue.push(frame);
    }
    
    return false;
}

void YOLOv8Worker::WorkerReset() {
    last_fps_update_time_ = std::chrono::steady_clock::now();
    frame_counter_ = 0;
    current_fps_ = 0.0;
}