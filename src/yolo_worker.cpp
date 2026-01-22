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

#ifndef YOLO_PROFILE
#define YOLO_PROFILE 0
#endif
#if YOLO_PROFILE
static constexpr int kYoloProfileLogEvery = 60;
#endif

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
        // IPC logging disabled: YOLO IPC note.

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
        const auto fps_now = std::chrono::steady_clock::now();
        frame_counter_++;
        std::chrono::duration<double> fps_elapsed = fps_now - last_fps_update_time_;
        if (fps_elapsed.count() >= 1.0) {
            const double fps = frame_counter_ / fps_elapsed.count();
            current_fps_.store(fps, std::memory_order_relaxed);
            frame_counter_ = 0;
            last_fps_update_time_ = fps_now;
#if YOLO_PROFILE
            std::cout << "[YOLO_FPS] " << threadName
                      << " fps=" << fps
                      << " q=" << GetCountQueueInSize()
                      << std::endl;
#endif
        }
#if YOLO_PROFILE
        static thread_local bool prof_init = false;
        static thread_local cudaEvent_t e_pre_start;
        static thread_local cudaEvent_t e_pre_end;
        static thread_local cudaEvent_t e_infer_start;
        static thread_local cudaEvent_t e_infer_end;
        static thread_local cudaEvent_t e_wait_start;
        static thread_local cudaEvent_t e_wait_end;
        static thread_local int prof_count = 0;
        if (!prof_init) {
            ck(cudaEventCreate(&e_pre_start));
            ck(cudaEventCreate(&e_pre_end));
            ck(cudaEventCreate(&e_infer_start));
            ck(cudaEventCreate(&e_infer_end));
            ck(cudaEventCreate(&e_wait_start));
            ck(cudaEventCreate(&e_wait_end));
            prof_init = true;
        }
        float ms_pre = 0.0f;
        float ms_gap = 0.0f;
        float ms_infer = 0.0f;
        float ms_wait = 0.0f;
        double ms_enqueue = 0.0;
        double ms_sync_wait = 0.0;
        double ms_queue = 0.0;
        double ms_post = 0.0;
        double ms_track = 0.0;
        double ms_ipc = 0.0;
        double ms_enet = 0.0;
        const auto cpu_start = std::chrono::steady_clock::now();
        bool timed_wait = false;
#endif
        const int camera_width = associated_camera_params_->width;
        const int camera_height = associated_camera_params_->height;

        // Set the NPP stream to the one used by the YOLO instance.
        nppSetStream(yolov8_instance_->stream);

        // Wait for the previous stage (acquire_frames) to finish copying data.
        if (entry->event_ptr) {
#if YOLO_PROFILE
            ck(cudaEventRecord(e_wait_start, yolov8_instance_->stream));
#endif
            ck(cudaStreamWaitEvent(yolov8_instance_->stream, *entry->event_ptr, 0));
#if YOLO_PROFILE
            ck(cudaEventRecord(e_wait_end, yolov8_instance_->stream));
            timed_wait = true;
#endif
        }

        frame_original_gpu_.d_orig = entry->d_image;
        debayer_gpu_.size.width = camera_width;
        debayer_gpu_.size.height = camera_height;

        // Debayer or duplicate mono channel to prepare for color conversion.
#if YOLO_PROFILE
        ck(cudaEventRecord(e_pre_start, yolov8_instance_->stream));
#endif
        if (associated_camera_params_->color) {
            // If color, debayer to RGBA first, then our kernel will handle the rest.
            debayer_frame_gpu(associated_camera_params_, &frame_original_gpu_, &debayer_gpu_);
            yolov8_instance_->preprocess_gpu(debayer_gpu_.d_debayer, camera_width, camera_height, true);
        } else {
            // If mono, pass the raw mono buffer directly to the kernel.
            yolov8_instance_->preprocess_gpu(frame_original_gpu_.d_orig, camera_width, camera_height, false);
        }
#if YOLO_PROFILE
        ck(cudaEventRecord(e_pre_end, yolov8_instance_->stream));
#endif

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
#if YOLO_PROFILE
        const auto infer_call_start = inference_start_time;
        ck(cudaEventRecord(e_infer_start, yolov8_instance_->stream));
#endif
        yolov8_instance_->infer();
#if YOLO_PROFILE
        const auto infer_call_end = std::chrono::steady_clock::now();
        ms_enqueue = std::chrono::duration<double, std::milli>(infer_call_end - infer_call_start).count();
        ck(cudaEventRecord(e_infer_end, yolov8_instance_->stream));
#endif

        // record per-frame event for synchronization
        if (entry->yolo_completion_event) {
            ck(cudaEventRecord(*entry->yolo_completion_event, yolov8_instance_->stream));
        }

        // Wait for the GPU to finish, with a timeout.
        const int timeout_us = 100000; // 100ms timeout
        bool finished_in_time = false;

#if YOLO_PROFILE
        const auto sync_wait_start = std::chrono::steady_clock::now();
#endif
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
#if YOLO_PROFILE
        const auto sync_wait_end = std::chrono::steady_clock::now();
        ms_sync_wait = std::chrono::duration<double, std::milli>(sync_wait_end - sync_wait_start).count();
#endif

#if YOLO_PROFILE
        if (finished_in_time) {
            ck(cudaEventSynchronize(e_infer_end));
            if (timed_wait) {
                ck(cudaEventElapsedTime(&ms_wait, e_wait_start, e_wait_end));
            }
            ck(cudaEventElapsedTime(&ms_pre, e_pre_start, e_pre_end));
            ck(cudaEventElapsedTime(&ms_gap, e_pre_end, e_infer_start));
            ck(cudaEventElapsedTime(&ms_infer, e_infer_start, e_infer_end));
            ms_queue = ms_sync_wait - ms_infer;
            if (ms_queue < 0.0) {
                ms_queue = 0.0;
            }
        }
#endif
        if (finished_in_time) {
            // Now that the GPU is finished, process the results.
            // This completely REPLACES entry->detections, preventing stale data
#if YOLO_PROFILE
            const auto post_start = std::chrono::steady_clock::now();
#endif
            yolov8_instance_->postprocess(entry->detections);
#if YOLO_PROFILE
            const auto post_end = std::chrono::steady_clock::now();
            ms_post = std::chrono::duration<double, std::milli>(post_end - post_start).count();
#endif
        } else {
            // Timed out, clear any potential partial results
            entry->detections.clear();
        }

        uint64_t current_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();

        // Update velocity tracking with new detections
#if YOLO_PROFILE
        const auto track_start = std::chrono::steady_clock::now();
#endif
        velocity_tracker_.updateTracking(entry->detections, current_timestamp_us);
#if YOLO_PROFILE
        const auto track_end = std::chrono::steady_clock::now();
        ms_track = std::chrono::duration<double, std::milli>(track_end - track_start).count();
#endif

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
#if YOLO_PROFILE
        const auto ipc_start = std::chrono::steady_clock::now();
#endif
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
#if YOLO_PROFILE
        const auto ipc_end = std::chrono::steady_clock::now();
        ms_ipc = std::chrono::duration<double, std::milli>(ipc_end - ipc_start).count();
#endif

        // Handle ENet sending if configured
#if YOLO_PROFILE
        const auto enet_start = std::chrono::steady_clock::now();
#endif
        if (enet_target_peer_ && associated_camera_select_->send_yolo_via_enet && !entry->detections.empty()) {
            // ENet code remains unchanged
        }
#if YOLO_PROFILE
        const auto enet_end = std::chrono::steady_clock::now();
        ms_enet = std::chrono::duration<double, std::milli>(enet_end - enet_start).count();
#endif

#if YOLO_PROFILE
        const auto cpu_end = std::chrono::steady_clock::now();
        const double ms_total = std::chrono::duration<double, std::milli>(cpu_end - cpu_start).count();
        prof_count++;
        if (prof_count % kYoloProfileLogEvery == 0) {
            std::cout << "[YOLO_TIME] " << threadName
                      << " wait=" << ms_wait << "ms"
                      << " pre=" << ms_pre << "ms"
                      << " gap=" << ms_gap << "ms"
                      << " enqueue=" << ms_enqueue << "ms"
                      << " infer=" << ms_infer << "ms"
                      << " sync=" << ms_sync_wait << "ms"
                      << " queue=" << ms_queue << "ms"
                      << " post=" << ms_post << "ms"
                      << " track=" << ms_track << "ms"
                      << " ipc=" << ms_ipc << "ms"
                      << " enet=" << ms_enet << "ms"
                      << " total=" << ms_total << "ms"
                      << std::endl;
        }
#endif
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
