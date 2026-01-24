// src/crop_and_encode_worker.cpp

#include "crop_and_encode_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "project.h" // Add this include
#include <nppi.h>
#include <npp.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>
#include <algorithm> // For std::max_element

CropAndEncodeWorker::CropAndEncodeWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& folder_name,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    unsigned char* display_buffer_pbo,
    CameraControl* camera_control
):CThreadWorker(name),
camera_params_(camera_params),
base_folder_name_(folder_name),
m_recycle_queue(recycle_queue),
d_display_buffer_pbo_(display_buffer_pbo),
camera_control_(camera_control),
d_cropped_rgba_(nullptr)
{

    std::cout << "[CropAndEncodeWorker] Initializing " << name << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    ck(cudaStreamCreate(&m_display_stream));

    // Allocate buffer for RGBA cropped frame for display
    ck(cudaMalloc(&d_cropped_rgba_, 256 * 256 * 4));

    try {
        CUcontext cuContext;
        ck(cuCtxGetCurrent(&cuContext));

        encoder_ = new NvEncoderCuda(cuContext, 256, 256, NV_ENC_BUFFER_FORMAT_NV12);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;

        GUID codecGuid = NV_ENC_CODEC_HEVC_GUID;
        GUID presetGuid = NV_ENC_PRESET_P7_GUID;
        NV_ENC_TUNING_INFO tuningInfo = NV_ENC_TUNING_INFO_LOSSLESS;

        std::cout << "[CropAndEncodeWorker] CONFIGURING FOR LOSSLESS (HEVC), HIGH-QUALITY RECORDING." << std::endl;

        encoder_->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);

        encodeConfig.gopLength = 1; // Keyframe every frame for lossless
        encodeConfig.frameIntervalP = 1;
        initializeParams.frameRateNum = camera_params_->frame_rate;
        initializeParams.frameRateDen = 1;
        initializeParams.enablePTD = 1;
        encodeConfig.rcParams.constQP = { 0, 0, 0 };

        encoder_->CreateEncoder(&initializeParams);
        encoder_->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);
        
        const NvEncInputFrame *tempFrame = encoder_->GetNextInputFrame();
        encoder_pitch_ = tempFrame->pitch;

        // Allocate and initialize the blank frame buffer
        const size_t encoder_buffer_size = static_cast<size_t>(encoder_pitch_) * 256 * 3 / 2;
        ck(cudaMalloc(&d_blank_frame_, encoder_buffer_size));

        // --- Correct YUV Initialization for a Black Frame ---
        // 1. Set the Y (luma) plane to 0 for black.
        size_t luma_size = static_cast<size_t>(encoder_pitch_) * 256;
        ck(cudaMemsetAsync(d_blank_frame_, 0, luma_size, m_stream));

        // 2. Set the UV (chroma) plane to 128 for neutral color.
        size_t chroma_size = static_cast<size_t>(encoder_pitch_) * 256 / 2;
        unsigned char* d_uv_plane = d_blank_frame_ + luma_size;
        ck(cudaMemsetAsync(d_uv_plane, 128, chroma_size, m_stream));

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Failed to initialize encoder: " << e.what() << std::endl;
        if (encoder_) {
            delete encoder_;
            encoder_ = nullptr;
        }
        throw;
    }
}

CropAndEncodeWorker::~CropAndEncodeWorker() {
    std::cout << "[CropAndEncodeWorker] Destructor for " << threadName << std::endl;

    if (camera_params_) {
        ck(cudaSetDevice(camera_params_->gpu_id));
    }

    // Always flush and close the writer and encoder.
    flush_and_close();

    // Explicitly delete the encoder to release its resources.
    if (encoder_) {
        delete encoder_;
        encoder_ = nullptr;
    }

    if (d_blank_frame_) cudaFree(d_blank_frame_);
    if (m_stream) cudaStreamDestroy(m_stream);
    if (m_display_stream) cudaStreamDestroy(m_display_stream);
}

void CropAndEncodeWorker::flush_and_close() {
    std::cout << "[CropAndEncodeWorker] Flushing and closing for " << threadName << std::endl;

    if (encoder_) {
        std::vector<std::vector<uint8_t>> packets;
        encoder_->EndEncode(packets);
        for (auto& p : packets) {
            // Use the next available frame ID for flushed frames
            writer_.video->push_packet(p.data(), static_cast<int>(p.size()), ++last_frame_id_used_); // Corrected line
        }
        std::cout << "[CropAndEncodeWorker] Encoder flushed." << std::endl;
    }

    if (writer_.video) {
        writer_.video->quit_thread();
        writer_.video->join_thread();
        delete writer_.video;
        writer_.video = nullptr;
        std::cout << "[CropAndEncodeWorker] Video writer closed." << std::endl;
    }
    
    if (writer_.metadata && writer_.metadata->is_open()) {
        writer_.metadata->close();
        delete writer_.metadata;
        writer_.metadata = nullptr;
    }
}


bool CropAndEncodeWorker::WorkerFunction(WORKER_ENTRY* entry) {
    if (!entry) {
        // Add this check to finalize video if recording stops
        if (!camera_control_->record_video && is_recording_) {
            flush_and_close();
            is_recording_ = false;
            {
                std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
                camera_control_->recording_folder.clear();
            }
        }
        return false;
    }

    // Set the correct CUDA device for this worker's operations
    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);

    if (camera_control_->record_video && !is_recording_) {
        std::string current_recording_folder;
        {
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            if (camera_control_->recording_folder.empty()) {
                camera_control_->recording_folder = base_folder_name_ + "/" + get_current_date_time();
            }
            current_recording_folder = camera_control_->recording_folder;
        }
        make_folder(current_recording_folder);

        writer_.video_file = current_recording_folder + "/Cam" + camera_params_->camera_serial + "_crop.mp4";
        writer_.keyframe_file = current_recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_keyframe.csv";
        writer_.metadata_file = current_recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_meta.csv";

        writer_.video = new FFmpegWriter(AV_CODEC_ID_HEVC, 256, 256, camera_params_->frame_rate,
                                       writer_.video_file.c_str(), writer_.keyframe_file.c_str());
        writer_.video->create_thread();

        writer_.metadata = new std::ofstream();
        writer_.metadata->open(writer_.metadata_file.c_str());
        if (!(*writer_.metadata)) {
            std::cout << "[CropAndEncodeWorker] Warning: Could not open metadata file!" << std::endl;
        } else {
            *writer_.metadata << "frame_id,timestamp,timestamp_sys,detection_confidence,crop_x,crop_y,crop_w,crop_h\n";
        }
        is_recording_ = true;
    } else if (!camera_control_->record_video && is_recording_) {
        flush_and_close();
        is_recording_ = false;
        {
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            camera_control_->recording_folder.clear();
        }
    }

    try {
        // Wait for the incoming frame data to be ready
        if (entry->event_ptr) {
            ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
            ck(cudaStreamWaitEvent(m_display_stream, *entry->event_ptr, 0));
        }
        
        bool has_detection = !entry->detections.empty();

        if (has_detection) {
            // --- DETECTION IS FOUND ---
            pose::Object best_detection = *std::max_element(
                entry->detections.begin(), entry->detections.end(),
                [](const pose::Object& a, const pose::Object& b) { return a.prob < b.prob; });
            
            const int CROP_W = 256;
            const int CROP_H = 256;
            float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
            float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
            int ix = std::clamp(static_cast<int>(cx) - CROP_W / 2, 0, entry->width - CROP_W);
            int iy = std::clamp(static_cast<int>(cy) - CROP_H / 2, 0, entry->height - CROP_H);
            
            // --- LIVE PREVIEW LOGIC (ALWAYS RUNS) ---
            if (d_display_buffer_pbo_) {
                pose::Rect crop_rect = {(float)ix, (float)iy, (float)CROP_W, (float)CROP_H};
                gpu_crop_and_resize_rgba(entry->d_image, d_cropped_rgba_, entry->width, entry->height,
                                         crop_rect, CROP_W, CROP_H, m_display_stream);
                
                const size_t width_in_bytes = CROP_W * 4;
                ck(cudaMemcpy2DAsync(d_display_buffer_pbo_, width_in_bytes, d_cropped_rgba_, width_in_bytes,
                                     width_in_bytes, CROP_H, cudaMemcpyDeviceToDevice, m_display_stream));
            }
            
            // --- RECORDING LOGIC (ONLY RUNS IF RECORDING IS ON) ---
            if (camera_control_->record_video) {
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                unsigned char* d_nv12_dst = static_cast<unsigned char*>(encIn->inputPtr);

                ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch, entry->d_image + (iy * entry->width + ix),
                                     entry->width, CROP_W, CROP_H, cudaMemcpyDeviceToDevice, m_stream));
                
                unsigned char* d_uv_plane_dst = d_nv12_dst + encIn->pitch * CROP_H;
                ck(cudaMemset2DAsync(d_uv_plane_dst, encIn->pitch, 128, CROP_W, CROP_H / 2, m_stream));

                // Encode and write the frame to file
                std::vector<std::vector<uint8_t>> packets;
                encoder_->EncodeFrame(packets);
                for (auto& p : packets) {
                    writer_.video->push_packet(p.data(), static_cast<int>(p.size()), entry->recording_frame_id);
                    if (entry->recording_frame_id > last_frame_id_used_) {
                        last_frame_id_used_ = entry->recording_frame_id;
                    }
                }

                // Write metadata to file
                if (writer_.metadata && writer_.metadata->is_open()) {
                    *writer_.metadata << entry->recording_frame_id << ',' << entry->timestamp << ','
                                      << entry->timestamp_sys << ',' << best_detection.prob << ','
                                      << best_detection.rect.x << ',' << best_detection.rect.y << ','
                                      << best_detection.rect.width << ',' << best_detection.rect.height << '\n';
                }
            }
        } else {
            // --- NO DETECTION ---
            // Always show a blank screen for the preview if no detection
            if (d_display_buffer_pbo_) {
                 ck(cudaMemsetAsync(d_display_buffer_pbo_, 0, 256 * 256 * 4, m_display_stream));
            }
            
            // Only encode a blank frame if recording is active
            if (camera_control_->record_video) {
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                ck(cudaMemcpy2DAsync(encIn->inputPtr, encIn->pitch, d_blank_frame_,
                                     encoder_pitch_, encoder_pitch_, 256 * 3 / 2,
                                     cudaMemcpyDeviceToDevice, m_stream));

                std::vector<std::vector<uint8_t>> packets;
                encoder_->EncodeFrame(packets);
                 for (auto& p : packets) {
                    writer_.video->push_packet(p.data(), static_cast<int>(p.size()), entry->recording_frame_id);
                    if (entry->recording_frame_id > last_frame_id_used_) {
                        last_frame_id_used_ = entry->recording_frame_id;
                    }
                }
            }
        }

        // Sync stream to make sure all CUDA calls have finished before recycling the entry
        if (d_display_buffer_pbo_) {
            ck(cudaStreamSynchronize(m_display_stream));
        }

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Exception processing frame " << entry->frame_id
                  << ": " << e.what() << std::endl;
    }

    // Cleanup and recycle the entry
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue.push(entry);
    }

    return false; // This worker does not pass items to its own output queue
}
