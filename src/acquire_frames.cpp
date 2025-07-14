// src/acquire_frames.cpp

#include "acquire_frames.h"
#include "frame_preprocessor.h" // Include the new preprocessor header
#include "nvtx_profiling.h"
#include "global.h"
#include <chrono>

// The signature changes slightly: it now needs a pointer to the preprocessor
void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    CameraResources* resources,
    FramePreprocessor* preprocessor // New parameter
){
    ck(cudaSetDevice(camera_params->gpu_id));
    NVTX_CAMERA("AcquireFrames_Main");

    auto last_fps_update_time = std::chrono::steady_clock::now();
    int frame_counter_for_fps = 0;
    unsigned long long frame_count = 0;

    // Camera startup logic (PTP, etc.) remains the same...

    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"), camera_params->camera_serial.c_str());

    while (camera_control->subscribe) {
        NVTX_RANGE_PUSH("Frame_Acquire_Loop");

        // Get a free WORKER_ENTRY and a free CUDA event from the resource pool
        WORKER_ENTRY* current_entry = nullptr;
        cudaEvent_t* current_event = nullptr;
        if (!resources->free_entries_queue->pop(current_entry) || !resources->free_events_queue->pop(current_event)) {
            if (current_entry) resources->free_entries_queue->push(current_entry);
            if (current_event) resources->free_events_queue->push(current_event);
            usleep(100);
            NVTX_RANGE_POP();
            continue;
        }

        // Get the frame from the camera SDK
        if (EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000) == EVT_SUCCESS) {
            frame_count++;

            // Copy raw data to the WORKER_ENTRY's GPU buffer
            ck(cudaMemcpyAsync(current_entry->d_image, ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize, cudaMemcpyDeviceToDevice, nullptr));
            EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv); // Re-queue SDK buffer immediately

            // Populate the entry's metadata
            current_entry->width = ecam->frame_recv.size_x;
            current_entry->height = ecam->frame_recv.size_y;
            current_entry->timestamp = ecam->frame_recv.timestamp;
            current_entry->frame_id = frame_count;
            current_entry->has_detections = camera_select->yolo;

            // Record an event to signal that the cudaMemcpy is complete
            current_entry->event_ptr = current_event;
            ck(cudaEventRecord(*current_entry->event_ptr, nullptr));

            // --- THE NEW LOGIC ---
            // Dispatch the raw frame to the preprocessor
            if (preprocessor) {
                preprocessor->PutObjectToQueueIn(current_entry);
            } else {
                // If there's no preprocessor, we must recycle the entry ourselves
                resources->recycle_queue->push(current_entry);
            }

            // FPS calculation
            frame_counter_for_fps++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_fps_update_time).count() >= 1.0) {
                streaming_fps.store(frame_counter_for_fps / std::chrono::duration<double>(now - last_fps_update_time).count());
                frame_counter_for_fps = 0;
                last_fps_update_time = now;
            }
        } else {
            // If getting a frame failed, recycle the entry and event
            resources->free_entries_queue->push(current_entry);
            resources->free_events_queue->push(current_event);
        }
        NVTX_RANGE_POP();
    }

    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"), camera_params->camera_serial.c_str());
}