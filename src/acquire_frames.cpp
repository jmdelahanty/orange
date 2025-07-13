// src/acquire_frames.cpp

#include "acquire_frames.h"
#include "nvtx_profiling.h"
#include "NvEncoder/NvCodecUtils.h"
#include "image_processing.h"
#include "frame_preprocessor.h"
#include "kernel.cuh"
#include <chrono>
#include <cuda_runtime.h>
#include "global.h"
#include "thread.h"
#include "opengldisplay.h"
#include "gpu_video_encoder.h"
#include "yolo_worker.h"
#include "image_writer_worker.h"
#include "crop_and_encode_worker.h"
#include "cuda_context_debug.h"

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
    CameraControl* camera_control,
    CameraResources* resources,
    FramePreprocessor* preprocessor)
{
    // --- Initial Setup ---
    ck(cudaSetDevice(camera_params->gpu_id));
    NVTX_CAMERA("AcquireFrames_Main");
    std::cout << "Starting acquisition loop for camera " << camera_params->camera_serial << std::endl;

    CUDA_CTX_LOG("=== ACQUIRE FRAMES START ===");
    dumpCudaState("Acquire frames startup");

    cudaStream_t stream;
    ck(cudaStreamCreate(&stream));
    CUDA_STREAM_LOG("Created acquisition stream", stream);

    CameraState camera_state{};

    NVTX_CAMERA("Camera_Acquisition_Start");
    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"), camera_params->camera_serial.c_str());

    // --- Main Loop ---
    while (camera_control->subscribe)
    {
        NVTX_RANGE("Frame_Acquisition_Loop");

        WORKER_ENTRY* current_entry = nullptr;
        cudaEvent_t* current_event = nullptr;

        if (!resources->free_entries_queue->pop(current_entry) || !resources->free_events_queue->pop(current_event)) {
            if (current_entry) resources->free_entries_queue->push(current_entry);
            if (current_event) resources->free_events_queue->push(current_event);
            usleep(100);
            continue;
        }


        camera_state.camera_return = EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000);

        if (camera_state.camera_return == EVT_SUCCESS) {
            camera_state.frame_count++;
            
            // --- Logging recommendation is here ---
            CUDA_MEM_LOG("ACQUIRED Frame", current_entry, ecam->frame_recv.bufferSize, camera_state.frame_count);

            // Populate the WORKER_ENTRY with the new frame's data
            current_entry->event_ptr = current_event;
            ck(cudaEventRecord(*current_entry->event_ptr, stream));

            current_entry->d_image = static_cast<unsigned char*>(ecam->frame_recv.imagePtr);
            current_entry->width = ecam->frame_recv.size_x;
            current_entry->height = ecam->frame_recv.size_y;
            current_entry->timestamp = ecam->frame_recv.timestamp;
            current_entry->frame_id = camera_state.frame_count;

            current_entry->gpu_direct_mode = true;
            current_entry->owns_memory = false;
            current_entry->camera_buffer_ptr = ecam->frame_recv.imagePtr;
            current_entry->camera_instance = &ecam->camera;
            current_entry->camera_frame_struct = &ecam->frame_recv;

            // Set reference count to 1 (only the preprocessor will receive this)
            current_entry->ref_count.store(1);

            // Dispatch the raw frame ONLY to the preprocessor
            if (preprocessor) {
                preprocessor->PutObjectToQueueIn(current_entry);
            } else {
                // If there's no preprocessor, we must recycle the entry immediately
                // This case shouldn't happen in a normal run
                if (current_entry->gpu_direct_mode) {
                   EVT_CameraQueueFrame(current_entry->camera_instance, current_entry->camera_frame_struct);
                }
                resources->free_entries_queue->push(current_entry);
                resources->free_events_queue->push(current_event);
            }
        }
    }

    // --- Cleanup ---
    std::cout << "Stopping acquisition loop for camera " << camera_params->camera_serial << std::endl;
    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"), camera_params->camera_serial.c_str());
    CUDA_STREAM_LOG("Destroying acquisition stream", stream);
    ck(cudaStreamDestroy(stream));
    CUDA_CTX_LOG("=== ACQUIRE FRAMES END ===");
}