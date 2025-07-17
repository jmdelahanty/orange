#include "crop_and_encode_worker.h"
#include "kernel.cuh"
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
    unsigned char* display_buffer_pbo
):CThreadWorker(name),
camera_params_(camera_params),
folder_name_(folder_name),
m_recycle_queue(recycle_queue),
d_display_buffer_pbo_(display_buffer_pbo),
d_cropped_rgba_(nullptr)
{

    std::cout << "[CropAndEncodeWorker] Initializing " << name << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Allocate buffer for RGBA cropped frame for display
    ck(cudaMalloc(&d_cropped_rgba_, 256 * 256 * 4));

    writer_.video_file = folder_name_ + "/Cam" + camera_params_->camera_serial + "_crop.mp4";
    writer_.keyframe_file = folder_name_ + "/Cam" + camera_params_->camera_serial + "_crop_keyframe.csv";
    writer_.metadata_file = folder_name_ + "/Cam" + camera_params_->camera_serial + "_crop_meta.csv";

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

        encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
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
        return false;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    nppSetStream(m_stream);

    try {
        // Wait for the frame to be ready from the acquisition thread
        if (entry->event_ptr) {
            ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
        }
        
        const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
        unsigned char* d_nv12_dst = static_cast<unsigned char*>(encIn->inputPtr);

        pose::Object best_detection{};
        bool has_detection = !entry->detections.empty();

        if (has_detection) {
            // --- DETECTION PATH ---
            // Find the detection with the highest confidence
            best_detection = *std::max_element(
                entry->detections.begin(), entry->detections.end(),
                [](const pose::Object& a, const pose::Object& b) { return a.prob < b.prob; });
            
            // Calculate the top-left corner of the 256x256 crop, centered on the detection
            const int CROP_W = 256;
            const int CROP_H = 256;
            float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
            float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
            int ix = std::clamp(static_cast<int>(cx) - CROP_W / 2, 0, entry->width - CROP_W);
            int iy = std::clamp(static_cast<int>(cy) - CROP_H / 2, 0, entry->height - CROP_H);
            
            // --- NEW: LOGIC FOR LIVE DISPLAY ---
            if (d_display_buffer_pbo_) {
                // Create a temporary rect for the kernel
                pose::Rect crop_rect = {(float)ix, (float)iy, (float)CROP_W, (float)CROP_H};
                
                // Use the new kernel to crop from the full mono frame and convert to RGBA
                gpu_crop_and_resize_rgba(
                    entry->d_image,      // Source: Full-size mono frame
                    d_cropped_rgba_,     // Destination: Our internal RGBA buffer
                    entry->width, entry->height,
                    crop_rect,
                    CROP_W, CROP_H,
                    m_stream
                );
                
                // Copy the resulting RGBA crop to the PBO for the GUI to render
                ck(cudaMemcpyAsync(d_display_buffer_pbo_, d_cropped_rgba_, CROP_W * CROP_H * 4, cudaMemcpyDeviceToDevice, m_stream));
            }
            
            // --- EXISTING ENCODING LOGIC ---
            // Copy the mono crop region to the Y plane of the encoder's input buffer
            ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch, entry->d_image + (iy * entry->width + ix),
                                 entry->width, CROP_W, CROP_H, cudaMemcpyDeviceToDevice, m_stream));
            
            // Fill the UV plane with a neutral gray (128)
            unsigned char* d_uv_plane_dst = d_nv12_dst + encIn->pitch * CROP_H;
            ck(cudaMemset2DAsync(d_uv_plane_dst, encIn->pitch, 128, CROP_W, CROP_H / 2, m_stream));

        } else {
            // --- NO DETECTION PATH ---
            // If the PBO pointer is valid, copy the blank frame to it for display
            if (d_display_buffer_pbo_) {
                 ck(cudaMemsetAsync(d_display_buffer_pbo_, 0, 256 * 256 * 4, m_stream)); // Black RGBA frame
            }
            
            // Copy the pre-made blank YUV frame to the encoder's input buffer
            ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch, d_blank_frame_,
                                 encoder_pitch_, encoder_pitch_, 256 * 3 / 2,
                                 cudaMemcpyDeviceToDevice, m_stream));
        }

        // --- COMMON LOGIC ---
        std::vector<std::vector<uint8_t>> packets;
        encoder_->EncodeFrame(packets);
        for (auto& p : packets) {
            writer_.video->push_packet(p.data(), static_cast<int>(p.size()), entry->recording_frame_id);
            if (entry->recording_frame_id > last_frame_id_used_) {
                last_frame_id_used_ = entry->recording_frame_id;
            }
        }

        // Write metadata
        if (writer_.metadata && writer_.metadata->is_open()) {
            *writer_.metadata << entry->recording_frame_id << ',' << entry->timestamp << ','
                              << entry->timestamp_sys << ',' << (has_detection ? best_detection.prob : 0.0f) << ','
                              << (has_detection ? best_detection.rect.x : 0.0f) << ',' << (has_detection ? best_detection.rect.y : 0.0f) << ','
                              << (has_detection ? best_detection.rect.width : 0.0f) << ',' << (has_detection ? best_detection.rect.height : 0.0f) << '\n';
        }

        // Synchronize the stream to ensure all copies (to PBO and encoder) are complete
        ck(cudaStreamSynchronize(m_stream));

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

    return false;
}