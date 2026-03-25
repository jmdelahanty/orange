// src/acquire_frames.cpp

#include "acquire_frames.h"
#include "nvtx_profiling.h"
#include "NvEncoder/NvCodecUtils.h"
#include "image_processing.h"
#include "kernel.cuh"
#include <chrono>
#include <deque>
#include <cuda_runtime.h>
#include "global.h"
#include "thread.h"
#include "opengldisplay.h"
#include "gpu_video_encoder.h"
#include "yolo_worker.h"
#include "image_writer_worker.h"
#include "crop_and_encode_worker.h"
#include "cuda_context_debug.h"
#include "encoder_preprocess_worker.h"
#include "frame_ipc_manager.h"
#include "project.h"
#include <cstdlib>
#include <limits>

#ifndef PIPELINE_PROFILE
#if defined(YOLO_PROFILE) && YOLO_PROFILE
#define PIPELINE_PROFILE 1
#else
#define PIPELINE_PROFILE 0
#endif
#endif
#if PIPELINE_PROFILE
static constexpr int kCopyProfileLogEvery = 60;
#endif

namespace {
struct RunningInt64Stats {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    long double sum = 0.0;
    uint64_t samples = 0;
    int64_t last = 0;

    void add(int64_t value) {
        if (value < min) min = value;
        if (value > max) max = value;
        sum += static_cast<long double>(value);
        last = value;
        ++samples;
    }

    void reset() {
        min = std::numeric_limits<int64_t>::max();
        max = std::numeric_limits<int64_t>::min();
        sum = 0.0;
        samples = 0;
        last = 0;
    }

    nlohmann::json to_json() const {
        nlohmann::json out = nlohmann::json::object();
        out["samples"] = samples;
        if (samples == 0) {
            return out;
        }
        out["min"] = min;
        out["max"] = max;
        out["last"] = last;
        out["mean"] = static_cast<double>(sum / static_cast<long double>(samples));
        return out;
    }
};

std::string current_recording_folder(CameraControl* camera_control) {
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    return camera_control->recording_folder;
}
}

static inline void PTP_timestamp_checking(PTPState *ptp_state, CameraEmergent *ecam, CameraState *camera_state){
    NVTX_RANGE("PTP_Timestamp_Check");
    EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh", &ptp_state->ptp_time_high);
    EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow", &ptp_state->ptp_time_low);
    ptp_state->ptp_time = (((unsigned long long)(ptp_state->ptp_time_high)) << 32) | ((unsigned long long)(ptp_state->ptp_time_low));
    ptp_state->frame_ts = ecam->frame_recv.timestamp;
    if (camera_state->frame_count != 0) {
        ptp_state->ptp_time_delta = ptp_state->ptp_time - ptp_state->ptp_time_prev;
        ptp_state->ptp_time_delta_sum += ptp_state->ptp_time_delta;
        ptp_state->frame_ts_delta = ptp_state->frame_ts - ptp_state->frame_ts_prev;
        ptp_state->frame_ts_delta_sum += ptp_state->frame_ts_delta;
    }
    ptp_state->ptp_time_prev = ptp_state->ptp_time;
    ptp_state->frame_ts_prev = ptp_state->frame_ts;
}

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    INDIGOSignalBuilder* indigo_signal_builder,
    COpenGLDisplay* openGLDisplay,
    EncoderPreprocessWorker* encoder_preprocess_worker,
    YOLOv8Worker* yolo_worker,
    ImageWriterWorker* image_writer,
    CameraResources* resources,
    FrameIPCManager* frame_ipc_manager
){
    ck(cudaSetDevice(camera_params->gpu_id));
    NVTX_CAMERA("AcquireFrames_Main");
    std::cout << "Starting acquisition loop for camera " << camera_params->camera_serial << std::endl;

    {
        NVTX_RANGE("CUDA_Context_Setup");
        CUDA_CTX_LOG("=== ACQUIRE FRAMES START ===");
        dumpCudaState("Acquire frames startup");
        ck(cudaSetDevice(camera_params->gpu_id));
        CUDA_RT_LOG("Set device to " + std::to_string(camera_params->gpu_id));
    }

    cudaStream_t stream;
    FrameProcess frame_process_save;

    {
        NVTX_RANGE("Stream_and_Buffer_Init");
        ck(cudaStreamCreate(&stream));
        CUDA_STREAM_LOG("Created acquisition stream", stream);

        initalize_gpu_frame(&frame_process_save.frame_original, camera_params);
        initialize_gpu_debayer(&frame_process_save.debayer, camera_params);
        initialize_cpu_frame(&frame_process_save.frame_cpu, camera_params);
        ck(cudaMalloc((void **)&frame_process_save.d_convert, (size_t)camera_params->width * camera_params->height * 3));
    }

    FrameIPCManager* ipc_manager = frame_ipc_manager;
    if (ipc_manager && !ipc_manager->isEnabled()) {
        ipc_manager = nullptr;
    }

    CameraState camera_state{};
    // std::cout << "[DEBUG] Camera " << camera_params->camera_serial 
    //       << " - camera_state address: " << &camera_state 
    //       << ", frame_count address: " << &camera_state.frame_count << std::endl;
    PTPState ptp_state{};
    RunningInt64Stats ptp_offset_stats{};
    RunningInt64Stats latch_minus_frame_stats{};
    RunningInt64Stats frame_delta_stats{};
    RunningInt64Stats latch_delta_stats{};
    std::string ptp_summary_recording_folder;
    StopWatch w;
    auto last_fps_update_time = std::chrono::steady_clock::now();
    int frame_counter_for_fps = 0;
    uint64_t local_recording_frame_count = 0;
    uint64_t gpu_direct_frames = 0;
    uint64_t gpu_ring_copy_frames = 0;
    uint64_t gpu_copy_frames = 0;
    uint64_t gpu_direct_attr_errors = 0;
    uint64_t gpu_direct_non_device = 0;
    uint64_t gpu_direct_wrong_device = 0;
    auto last_gpu_direct_log_time = std::chrono::steady_clock::now();
    int free_entries_available = CameraResources::ACQUIRE_WORK_ENTRIES_MAX;
    int free_events_available = CameraResources::EVENT_POOL_SIZE;
    int yolo_events_available = CameraResources::EVENT_POOL_SIZE;
    int free_entries_low = free_entries_available;
    int free_events_low = free_events_available;
    int yolo_events_low = yolo_events_available;
    struct PendingRequeue {
        Emergent::CEmergentCamera* camera;
        Emergent::CEmergentFrame* frame;
        cudaEvent_t* event;
    };
    std::deque<PendingRequeue> pending_requeues;
    int yolo_decimate = 1;
    const char* yolo_decimate_env = std::getenv("ORANGE_YOLO_DECIMATE");
    if (yolo_decimate_env && *yolo_decimate_env) {
        char* end = nullptr;
        long parsed = std::strtol(yolo_decimate_env, &end, 10);
        if (end != yolo_decimate_env && parsed > 1 && parsed < 10000) {
            yolo_decimate = static_cast<int>(parsed);
        }
    }
    uint64_t yolo_decimate_counter = 0;
    bool last_yolo_enabled = false;
    if (yolo_decimate > 1) {
        std::cout << "[YOLO] Cam " << camera_params->camera_serial
                  << " decimate=1/" << yolo_decimate << std::endl;
    }

    auto reset_ptp_summary_stats = [&]() {
        ptp_offset_stats.reset();
        latch_minus_frame_stats.reset();
        frame_delta_stats.reset();
        latch_delta_stats.reset();
    };

    auto build_ptp_camera_summary_json = [&](bool finalized) {
        nlohmann::json summary = nlohmann::json::object();
        summary["camera_serial"] = camera_params->camera_serial;
        summary["camera_id"] = camera_params->camera_id;
        summary["gpu_id"] = camera_params->gpu_id;
        summary["sync_camera_enabled"] = camera_control->sync_camera;
        summary["finalized"] = finalized;
        summary["updated_at_utc"] = get_current_utc_timestamp();
        summary["frame_count"] = camera_state.frame_count;
        summary["frames_received"] = camera_state.frames_recd;
        summary["dropped_frames"] = camera_state.dropped_frames;
        summary["last_frame_timestamp_ns"] = ptp_state.frame_ts;
        summary["last_latched_ptp_time_ns"] = ptp_state.ptp_time;
        summary["ptp_offset_ns"] = ptp_offset_stats.to_json();
        summary["latch_minus_frame_ns"] = latch_minus_frame_stats.to_json();
        summary["frame_delta_ns"] = frame_delta_stats.to_json();
        summary["latch_delta_ns"] = latch_delta_stats.to_json();
        const uint64_t delta_samples = (camera_state.frame_count > 1) ? (camera_state.frame_count - 1) : 0;
        summary["delta_samples"] = delta_samples;
        if (delta_samples > 0) {
            summary["avg_frame_delta_ns_running"] = ptp_state.frame_ts_delta_sum / delta_samples;
            summary["avg_latch_delta_ns_running"] = ptp_state.ptp_time_delta_sum / delta_samples;
        }
        return summary;
    };

    {
        NVTX_RANGE("Camera_Initialization");
        if (camera_control->sync_camera) {
            NVTX_RANGE_PUSH("PTP_Sync_Setup");
            show_ptp_offset(&ptp_state, ecam);
            start_ptp_sync(&ptp_state, ptp_params, camera_params, ecam, 3);
            NVTX_RANGE_POP();
        }

        NVTX_CAMERA("Camera_Acquisition_Start");
        check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"), camera_params->camera_serial.c_str());

        if (camera_control->sync_camera) {
            NVTX_RANGE_PUSH("PTP_Countdown");
            grab_frames_after_countdown(&ptp_state, ecam);
            NVTX_RANGE_POP();
        } else {
            try_start_timer();
        }
    }

    w.Start();

#if PIPELINE_PROFILE
    struct CopyProfileEvents {
        cudaEvent_t start{};
        cudaEvent_t end{};
        int device = -1;
        bool initialized = false;

        void Init(int gpu_id) {
            if (initialized) {
                return;
            }
            device = gpu_id;
            ck(cudaSetDevice(device));
            ck(cudaEventCreate(&start));
            ck(cudaEventCreate(&end));
            initialized = true;
        }

        ~CopyProfileEvents() {
            if (!initialized) {
                return;
            }
            if (device >= 0) {
                cudaSetDevice(device);
            }
            cudaEventDestroy(start);
            cudaEventDestroy(end);
        }
    };

    static thread_local CopyProfileEvents copy_prof_events;
    static thread_local bool copy_prof_inflight = false;
    static thread_local int copy_prof_count = 0;
#endif

    while (camera_control->subscribe) {
        NVTX_RANGE_PUSH("Frame_Processing_Loop");

#if PIPELINE_PROFILE
        copy_prof_events.Init(camera_params->gpu_id);
        if (copy_prof_inflight) {
            cudaError_t copy_status = cudaEventQuery(copy_prof_events.end);
            if (copy_status == cudaSuccess) {
                float copy_ms = 0.0f;
                ck(cudaEventElapsedTime(&copy_ms, copy_prof_events.start, copy_prof_events.end));
                std::cout << "[COPY_TIME] Cam " << camera_params->camera_serial
                          << " GPU " << camera_params->gpu_id
                          << " ring_ms=" << copy_ms << std::endl;
                copy_prof_inflight = false;
            } else if (copy_status != cudaErrorNotReady) {
                std::cerr << "[COPY_TIME] Cam " << camera_params->camera_serial
                          << " event query failed: " << cudaGetErrorString(copy_status)
                          << std::endl;
                copy_prof_inflight = false;
            }
        }
#endif

        for (auto it = pending_requeues.begin(); it != pending_requeues.end(); ) {
            cudaError_t status = cudaEventQuery(*it->event);
            if (status == cudaSuccess) {
                EVT_CameraQueueFrame(it->camera, it->frame);
                it = pending_requeues.erase(it);
                continue;
            }
            if (status == cudaErrorNotReady) {
                ++it;
                continue;
            }
            std::cerr << "[GPU_DIRECT] Requeue event query failed: "
                      << cudaGetErrorString(status) << std::endl;
            EVT_CameraQueueFrame(it->camera, it->frame);
            it = pending_requeues.erase(it);
        }

        WORKER_ENTRY* recycled_entry = nullptr;
        while(resources->recycle_queue->pop(recycled_entry)) {
            if (recycled_entry) {
                if (recycled_entry->event_ptr) {
                    resources->free_events_queue->push(recycled_entry->event_ptr);
                    free_events_available++;
                }
                if (recycled_entry->yolo_completion_event) {
                    resources->yolo_events_queue->push(recycled_entry->yolo_completion_event);
                    yolo_events_available++;
                }
                resources->free_entries_queue->push(recycled_entry);
                free_entries_available++;
            }
        }

        WORKER_ENTRY* current_entry = nullptr;
        cudaEvent_t* current_event = nullptr;
        cudaEvent_t* yolo_event = nullptr;

        bool got_entry = resources->free_entries_queue->pop(current_entry);
        if (got_entry) {
            free_entries_available--;
            if (free_entries_available < free_entries_low) free_entries_low = free_entries_available;
        }
        bool got_event = resources->free_events_queue->pop(current_event);
        if (got_event) {
            free_events_available--;
            if (free_events_available < free_events_low) free_events_low = free_events_available;
        }
        bool got_yolo_event = resources->yolo_events_queue->pop(yolo_event);
        if (got_yolo_event) {
            yolo_events_available--;
            if (yolo_events_available < yolo_events_low) yolo_events_low = yolo_events_available;
        }

        if (!got_entry || !got_event || !got_yolo_event) {
            if (got_entry) {
                resources->free_entries_queue->push(current_entry);
                free_entries_available++;
            }
            if (got_event) {
                resources->free_events_queue->push(current_event);
                free_events_available++;
            }
            if (got_yolo_event) {
                resources->yolo_events_queue->push(yolo_event);
                yolo_events_available++;
            }
            NVTX_RANGE_POP();
            usleep(100);
            continue;
        }

        camera_state.camera_return = EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000);

        if (camera_state.camera_return == EVT_SUCCESS) {
            if (camera_control->sync_camera) {
                PTP_timestamp_checking(&ptp_state, ecam, &camera_state);
            }

            struct timespec ts_rt1;
            clock_gettime(CLOCK_REALTIME, &ts_rt1);
            uint64_t real_time = (ts_rt1.tv_sec * 1000000000LL) + ts_rt1.tv_nsec;
            camera_state.frames_recd++;
            camera_state.frame_count++;
            current_entry->frame_id = camera_state.frame_count; // Assign absolute frame ID

            // If recording is active, increment and assign the recording-specific frame ID
            if (camera_control->record_video) {
                current_entry->recording_frame_id = ++local_recording_frame_count;
            } else {
                current_entry->recording_frame_id = 0; // Or another sentinel value if preferred
                local_recording_frame_count = 0;
            }

            const std::string live_recording_folder = current_recording_folder(camera_control);
            if (live_recording_folder != ptp_summary_recording_folder) {
                if (!ptp_summary_recording_folder.empty() && camera_control->sync_camera) {
                    update_ptp_sync_summary_camera(
                        ptp_summary_recording_folder,
                        camera_params->camera_serial,
                        build_ptp_camera_summary_json(true));
                }
                ptp_summary_recording_folder = live_recording_folder;
                reset_ptp_summary_stats();
            }

        //     std::cout << "[DEBUG] Camera " << camera_params->camera_serial 
        //   << " incremented to frame_count=" << camera_state.frame_count 
        //   << " (entry->frame_id=" << current_entry->frame_id << ")" << std::endl;

            if (current_entry->d_image_pool) {
                current_entry->d_image = current_entry->d_image_pool;
            }
            current_entry->gpu_direct_mode = false;
            current_entry->owns_memory = true;
            current_entry->camera_buffer_ptr = nullptr;
            current_entry->camera_instance = nullptr;
            current_entry->camera_frame_struct = nullptr;

            bool will_display = (camera_select->stream_on && openGLDisplay);
            bool will_record = (camera_control->record_video && encoder_preprocess_worker);
            bool yolo_enabled = (camera_select->yolo && yolo_worker);
            if (yolo_enabled && !last_yolo_enabled) {
                yolo_decimate_counter = 0;
            }
            last_yolo_enabled = yolo_enabled;
            bool will_yolo = yolo_enabled;
            if (yolo_enabled && yolo_decimate > 1) {
                if ((yolo_decimate_counter++ % static_cast<uint64_t>(yolo_decimate)) != 0) {
                    will_yolo = false;
                }
            }
            int dispatch_count = 0;
            if (will_display) dispatch_count++;
            if (will_record) dispatch_count++;
            if (will_yolo) dispatch_count++;

            cudaPointerAttributes attrs{};
            cudaError_t attr_status = cudaPointerGetAttributes(&attrs, ecam->frame_recv.imagePtr);
            if (attr_status != cudaSuccess) {
                gpu_direct_attr_errors++;
                cudaGetLastError(); // Clear error so we can continue.
            }
            bool use_direct_pointer = (attr_status == cudaSuccess &&
                                       attrs.type == cudaMemoryTypeDevice &&
                                       attrs.device == camera_params->gpu_id);
            bool use_ring_copy = (use_direct_pointer && dispatch_count > 1);

            if (attr_status == cudaSuccess) {
                if (attrs.type != cudaMemoryTypeDevice) {
                    gpu_direct_non_device++;
                } else if (attrs.device != camera_params->gpu_id) {
                    gpu_direct_wrong_device++;
                }
            }

            if (use_direct_pointer && !use_ring_copy) {
                gpu_direct_frames++;
                current_entry->d_image = static_cast<unsigned char*>(ecam->frame_recv.imagePtr);
                current_entry->gpu_direct_mode = true;
                current_entry->owns_memory = false;
            } else {
#if PIPELINE_PROFILE
                bool sample_copy = false;
                if (use_ring_copy && !copy_prof_inflight) {
                    copy_prof_count++;
                    if (copy_prof_count % kCopyProfileLogEvery == 0) {
                        sample_copy = true;
                        ck(cudaEventRecord(copy_prof_events.start, stream));
                    }
                }
#endif
                if (use_ring_copy) {
                    gpu_ring_copy_frames++;
                } else {
                    gpu_copy_frames++;
                }
                ck(cudaMemcpyAsync(current_entry->d_image, ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize, cudaMemcpyDeviceToDevice, stream));
#if PIPELINE_PROFILE
                if (sample_copy) {
                    ck(cudaEventRecord(copy_prof_events.end, stream));
                    copy_prof_inflight = true;
                }
#endif
                current_entry->gpu_direct_mode = false;
                current_entry->owns_memory = true;
                current_entry->camera_buffer_ptr = nullptr;
                current_entry->camera_instance = nullptr;
                current_entry->camera_frame_struct = nullptr;
                if (!use_direct_pointer) {
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                }
            }

            current_entry->event_ptr = current_event;
            if (!will_yolo && yolo_event) {
                resources->yolo_events_queue->push(yolo_event);
                yolo_events_available++;
                yolo_event = nullptr;
            }
            current_entry->yolo_completion_event = yolo_event;
            ck(cudaEventRecord(*current_entry->event_ptr, stream));
            if (use_ring_copy) {
                pending_requeues.push_back({&ecam->camera, &ecam->frame_recv, current_entry->event_ptr});
            }

            current_entry->width = ecam->frame_recv.size_x;
            current_entry->height = ecam->frame_recv.size_y;
            current_entry->pixelFormat = ecam->frame_recv.pixel_type;
            current_entry->timestamp = ecam->frame_recv.timestamp;
            current_entry->timestamp_sys = real_time;
            current_entry->frame_id = camera_state.frame_count;
            current_entry->has_detections = will_yolo;
            current_entry->detections_ready.store(false);
            current_entry->ipc_frame_id = 0;

            // FRAME_IPC: Send frame data immediately after capture
            // This ensures EVERY frame is sent for synchronization
            if (ipc_manager) {
                // ALWAYS use recording_frame_id when it's valid (non-zero or recording active)
                // This ensures consistency - if a frame gets recorded, we use its recording ID
                // Otherwise, fall back to absolute frame_id
                uint64_t frame_id_for_ipc;
                
                if (camera_control->record_video && current_entry->recording_frame_id > 0) {
                    // Recording is active and we have a valid recording frame ID
                    frame_id_for_ipc = current_entry->recording_frame_id;
                } else {
                    // Not recording or recording just stopped - use absolute frame ID
                    // This ensures continuity even when recording toggles
                    frame_id_for_ipc = current_entry->frame_id;
                }
                
                current_entry->ipc_frame_id = frame_id_for_ipc;
                bool yolo_will_process = will_yolo;
                
                // Log the frame ID being sent (for debugging)
                static uint64_t last_logged_frame = 0;
                if (frame_id_for_ipc != last_logged_frame + 1 && last_logged_frame != 0) {
                    // IPC logging disabled: non-sequential frame ID warning.
                }
                last_logged_frame = frame_id_for_ipc;
                
                bool ipc_success = ipc_manager->sendFrame(
                    frame_id_for_ipc,
                    current_entry->timestamp,
                    yolo_will_process
                );
                
                if (!ipc_success) {
                    // IPC logging disabled: queue full warning.
                }
            }
            
            // FRAME_IPC: Store IPC manager pointer in entry for YOLO to use later
            // This allows YOLO to update the frame with detection data
            current_entry->frame_ipc_manager = ipc_manager;

            if (camera_select->frame_save_state == State_Write_New_Frame && image_writer) {
                ImageWriter_Entry* save_job = new ImageWriter_Entry();
                save_job->event_ptr = current_event;
                image_writer->PutObjectToQueueIn(save_job);
            }
            
            // Create a dispatch counter to track how many workers will process this frame
            // If the camera is set to stream, record video, or run YOLO detection,
            // we increment the dispatch count for each active worker.
            // If no workers are active, we return the frame to the free queue.
            // This allows us to efficiently manage resources and avoid unnecessary processing.
            if (dispatch_count > 0) {
                current_entry->ref_count.store(dispatch_count);

                if (will_display) openGLDisplay->PutObjectToQueueIn(current_entry);
                if (will_record) encoder_preprocess_worker->PutObjectToQueueIn(current_entry);
                if (will_yolo) yolo_worker->PutObjectToQueueIn(current_entry);

                if (use_direct_pointer && !use_ring_copy) {
                    current_entry->camera_buffer_ptr = ecam->frame_recv.imagePtr;
                    current_entry->camera_instance = &ecam->camera;
                    current_entry->camera_frame_struct = &ecam->frame_recv;
                }

            } else {
                // FRAME_IPC: Important - even if no workers are active, we still sent the frame IPC above
                // This ensures frame synchronization works even when just recording without display/YOLO
                if (use_direct_pointer && !use_ring_copy) {
                    EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv);
                }
                resources->free_events_queue->push(current_event);
                if (yolo_event) {
                    resources->yolo_events_queue->push(yolo_event);
                    yolo_events_available++;
                }
                resources->free_entries_queue->push(current_entry);
                free_events_available++;
                free_entries_available++;
            }

            frame_counter_for_fps++;
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - last_fps_update_time;
            if (elapsed.count() >= 1.0) {
                streaming_fps.store(frame_counter_for_fps / elapsed.count());
                frame_counter_for_fps = 0;
                last_fps_update_time = now;
            }

            std::chrono::duration<double> direct_elapsed = now - last_gpu_direct_log_time;
            if (direct_elapsed.count() >= 1.0) {
                if (camera_control->sync_camera && camera_state.frame_count > 0) {
                    const int64_t latch_minus_frame_ns =
                        static_cast<int64_t>(ptp_state.ptp_time) - static_cast<int64_t>(ptp_state.frame_ts);
                    const uint64_t delta_samples =
                        (camera_state.frame_count > 1) ? (camera_state.frame_count - 1) : 0;
                    const uint64_t avg_frame_delta_ns =
                        (delta_samples > 0) ? (ptp_state.frame_ts_delta_sum / delta_samples) : 0;
                    const uint64_t avg_latch_delta_ns =
                        (delta_samples > 0) ? (ptp_state.ptp_time_delta_sum / delta_samples) : 0;
                    int32_t current_ptp_offset = 0;
                    EVT_ERROR ptp_offset_ret = EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset", &current_ptp_offset);
                    if (ptp_offset_ret == EVT_SUCCESS) {
                        ptp_offset_stats.add(current_ptp_offset);
                    }
                    latch_minus_frame_stats.add(latch_minus_frame_ns);
                    frame_delta_stats.add(static_cast<int64_t>(ptp_state.frame_ts_delta));
                    latch_delta_stats.add(static_cast<int64_t>(ptp_state.ptp_time_delta));
                    if (!ptp_summary_recording_folder.empty()) {
                        update_ptp_sync_summary_camera(
                            ptp_summary_recording_folder,
                            camera_params->camera_serial,
                            build_ptp_camera_summary_json(false));
                    }
                    if (ptp_offset_ret == EVT_SUCCESS) {
                        std::cout << "[PTP_LIVE] Cam " << camera_params->camera_serial
                                  << " frame=" << camera_state.frame_count
                                  << " ptp_offset_ns=" << current_ptp_offset
                                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                                  << " frame_delta_ns=" << ptp_state.frame_ts_delta
                                  << " latch_delta_ns=" << ptp_state.ptp_time_delta
                                  << " avg_frame_delta_ns=" << avg_frame_delta_ns
                                  << " avg_latch_delta_ns=" << avg_latch_delta_ns
                                  << std::endl;
                    } else {
                        std::cout << "[PTP_LIVE] Cam " << camera_params->camera_serial
                                  << " frame=" << camera_state.frame_count
                                  << " ptp_offset_ns=NA"
                                  << " latch_minus_frame_ns=" << latch_minus_frame_ns
                                  << " frame_delta_ns=" << ptp_state.frame_ts_delta
                                  << " latch_delta_ns=" << ptp_state.ptp_time_delta
                                  << " avg_frame_delta_ns=" << avg_frame_delta_ns
                                  << " avg_latch_delta_ns=" << avg_latch_delta_ns
                                  << std::endl;
                    }
                }
                std::cout << "[GPU_DIRECT] Cam " << camera_params->camera_serial
                          << " GPU " << camera_params->gpu_id
                          << " direct=" << gpu_direct_frames
                          << " ring=" << gpu_ring_copy_frames
                          << " copy=" << gpu_copy_frames
                          << " attr_err=" << gpu_direct_attr_errors
                          << " non_dev=" << gpu_direct_non_device
                          << " wrong_dev=" << gpu_direct_wrong_device
                          << " stream=" << (camera_select->stream_on ? "on" : "off")
                          << " yolo=" << (camera_select->yolo ? "on" : "off")
                          << " record=" << (camera_control->record_video ? "on" : "off")
                          << std::endl;
                int display_q = openGLDisplay ? openGLDisplay->GetCountQueueInSize() : -1;
                int yolo_q = yolo_worker ? yolo_worker->GetCountQueueInSize() : -1;
                int preprocess_q = encoder_preprocess_worker ? encoder_preprocess_worker->GetCountQueueInSize() : -1;
                int enc_buffers = encoder_preprocess_worker ? encoder_preprocess_worker->available_buffers_.load() : -1;
                int enc_events = encoder_preprocess_worker ? encoder_preprocess_worker->available_events_.load() : -1;
                uint64_t enc_waits = encoder_preprocess_worker ? encoder_preprocess_worker->get_resource_waits() : 0;
                std::cout << "[PIPELINE] Cam " << camera_params->camera_serial
                          << " free=" << free_entries_available << "/" << free_entries_low
                          << " ev=" << free_events_available << "/" << free_events_low
                          << " yev=" << yolo_events_available << "/" << yolo_events_low
                          << " pend=" << pending_requeues.size()
                          << " q(d/y/p)=" << display_q << "/" << yolo_q << "/" << preprocess_q
                          << " enc_buf=" << enc_buffers
                          << " enc_evt=" << enc_events
                          << " enc_waits=" << enc_waits
                          << std::endl;
                gpu_direct_frames = 0;
                gpu_ring_copy_frames = 0;
                gpu_copy_frames = 0;
                gpu_direct_attr_errors = 0;
                gpu_direct_non_device = 0;
                gpu_direct_wrong_device = 0;
                last_gpu_direct_log_time = now;
                free_entries_low = free_entries_available;
                free_events_low = free_events_available;
                yolo_events_low = yolo_events_available;
            }
        }
        NVTX_RANGE_POP();
    }

    for (const auto& pending : pending_requeues) {
        cudaEventSynchronize(*pending.event);
        EVT_CameraQueueFrame(pending.camera, pending.frame);
    }
    pending_requeues.clear();

    if (camera_control->sync_camera && !ptp_summary_recording_folder.empty()) {
        update_ptp_sync_summary_camera(
            ptp_summary_recording_folder,
            camera_params->camera_serial,
            build_ptp_camera_summary_json(true));
    }

    // Cleanup
    {
        NVTX_RANGE("Cleanup_and_Shutdown");
        CUDA_CTX_LOG("=== ACQUIRE FRAMES CLEANUP ===");

        // FRAME_IPC: Log final statistics if IPC was active
        if (ipc_manager) {
            // IPC logging disabled: final stats.
        }

        {
            NVTX_CAMERA("Camera_Acquisition_Stop");
            check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"), camera_params->camera_serial.c_str());
        }

        if (!ptp_params->network_sync) {
            try_stop_timer();
        }
        double time_diff = w.Stop();
        report_statistics(camera_params, &camera_state, time_diff);

        {
            NVTX_RANGE("Memory_Cleanup");
            cudaFree(frame_process_save.frame_original.d_orig);
            cudaFree(frame_process_save.debayer.d_debayer);
            cudaFree(frame_process_save.d_convert);
            free(frame_process_save.frame_cpu.frame);
        }

        CUDA_STREAM_LOG("Destroying acquisition stream", stream);
        cudaStreamDestroy(stream);

        CUDA_CTX_LOG("=== ACQUIRE FRAMES END ===");
        std::cout << "Acquire frames thread finished for camera: " << camera_params->camera_serial << std::endl;

        CUcontext popped_context;
    }
}
