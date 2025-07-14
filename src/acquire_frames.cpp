// src/acquire_frames.cpp

#include "acquire_frames.h"
#include "frame_preprocessor.h"
#include "nvtx_profiling.h"
#include "global.h"
#include <chrono>

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    CameraResources* resources,
    FramePreprocessor* preprocessor
){
    ck(cudaSetDevice(camera_params->gpu_id));
    NVTX_CAMERA("AcquireFrames_Main");

    auto last_fps_update_time = std::chrono::steady_clock::now();
    int frame_counter_for_fps = 0;
    unsigned long long frame_count = 0;

    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"), camera_params->camera_serial.c_str());

    while (camera_control->subscribe) {
        NVTX_RANGE_PUSH("Frame_Acquire_Loop");

        // --- START: MODIFIED BUFFER ACQUISITION LOGIC ---
        WORKER_ENTRY* current_entry = nullptr;
        
        // Prioritize re-using an entry from the recycle queue.
        if (!resources->recycle_queue->pop(current_entry)) {
            // If the recycle queue is empty, try to get a fresh one.
            if (!resources->free_entries_queue->pop(current_entry)) {
                // If both are empty, wait and try again.
                usleep(100); 
                NVTX_RANGE_POP();
                continue;
            }
        }
        
        cudaEvent_t* current_event = nullptr;
        if (!resources->free_events_queue->pop(current_event)) {
            // If we have an entry but no event, we must return the entry and try again.
            resources->free_entries_queue->push(current_entry);
            usleep(100);
            NVTX_RANGE_POP();
            continue;
        }

        if (EVT_CameraGetFrame(&ecam->camera, &ecam->frame_recv, 1000) == EVT_SUCCESS) {
            frame_count++;

            ck(cudaMemcpyAsync(current_entry->d_image, ecam->frame_recv.imagePtr, ecam->frame_recv.bufferSize, cudaMemcpyDeviceToDevice, nullptr));
            EVT_CameraQueueFrame(&ecam->camera, &ecam->frame_recv); 

            current_entry->width = ecam->frame_recv.size_x;
            current_entry->height = ecam->frame_recv.size_y;
            current_entry->timestamp = ecam->frame_recv.timestamp;
            current_entry->frame_id = camera_params->camera_frame_counter.fetch_add(1, std::memory_order_relaxed);
            current_entry->has_detections = camera_select->yolo;

            current_entry->event_ptr = current_event;
            ck(cudaEventRecord(*current_entry->event_ptr, nullptr));

            if (preprocessor) {
                preprocessor->PutObjectToQueueIn(current_entry);
            } else {
                // This branch is now more critical: without a preprocessor, direct recycling is needed.
                resources->recycle_queue->push(current_entry);
            }

            frame_counter_for_fps++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_fps_update_time).count() >= 1.0) {
                streaming_fps.store(frame_counter_for_fps / std::chrono::duration<double>(now - last_fps_update_time).count());
                frame_counter_for_fps = 0;
                last_fps_update_time = now;
            }
        } else {
            // If getting a frame failed, recycle both the entry and event.
            resources->recycle_queue->push(current_entry);
            resources->free_events_queue->push(current_event);
        }
        NVTX_RANGE_POP();
    }

    check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop"), camera_params->camera_serial.c_str());
}