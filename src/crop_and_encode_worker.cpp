// src/crop_and_encode_worker.cpp

#include "crop_and_encode_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "project.h" // Add this include
#include "fsuid_guard.h"
#include <nppi.h>
#include <npp.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>
#include <algorithm> // For std::max_element

namespace {
constexpr int kDisplayGpuId = 0;
}

int CropAndEncodeWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

CropAndEncodeWorker::CropAndEncodeWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& folder_name,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    unsigned char* display_buffer_pbo,
    CameraControl* camera_control,
    int crop_size_px
):CThreadWorker(name),
camera_params_(camera_params),
base_folder_name_(folder_name),
crop_width_(SanitizeCropSize(crop_size_px)),
crop_height_(SanitizeCropSize(crop_size_px)),
m_recycle_queue(recycle_queue),
d_display_buffer_pbo_(display_buffer_pbo),
camera_control_(camera_control),
d_cropped_rgba_(nullptr)
{

    std::cout << "[CropAndEncodeWorker] Initializing " << name << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));

    // Transitional crop buffer: this will become the shared crop payload for pose.
    ck(cudaMalloc(&d_cropped_rgba_, crop_preview_bytes()));

    if (d_display_buffer_pbo_ && camera_params_->gpu_id != kDisplayGpuId) {
        // OpenGL PBOs are mapped on the display GPU. For cross-GPU cameras,
        // stage only the small preview crop through host memory.
        ck(cudaHostAlloc(&h_display_crop_, crop_preview_bytes(), cudaHostAllocDefault));
        ck(cudaSetDevice(kDisplayGpuId));
        ck(cudaStreamCreate(&m_display_stream));
        ck(cudaSetDevice(camera_params_->gpu_id));
    }

    try {
        CUcontext cuContext;
        ck(cuCtxGetCurrent(&cuContext));

        encoder_ = new NvEncoderCuda(cuContext, crop_width_, crop_height_, NV_ENC_BUFFER_FORMAT_NV12);

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
        const size_t encoder_buffer_size = static_cast<size_t>(encoder_pitch_) * crop_height_ * 3 / 2;
        ck(cudaMalloc(&d_blank_frame_, encoder_buffer_size));

        // --- Correct YUV Initialization for a Black Frame ---
        // 1. Set the Y (luma) plane to 0 for black.
        size_t luma_size = static_cast<size_t>(encoder_pitch_) * crop_height_;
        ck(cudaMemsetAsync(d_blank_frame_, 0, luma_size, m_stream));

        // 2. Set the UV (chroma) plane to 128 for neutral color.
        size_t chroma_size = static_cast<size_t>(encoder_pitch_) * crop_height_ / 2;
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

    if (is_recording_) {
        finalize_recording();
    } else {
        // Always flush and close the writer and encoder.
        flush_and_close();
    }

    // Explicitly delete the encoder to release its resources.
    if (encoder_) {
        delete encoder_;
        encoder_ = nullptr;
    }

    if (d_cropped_rgba_) {
        cudaFree(d_cropped_rgba_);
        d_cropped_rgba_ = nullptr;
    }
    if (d_blank_frame_) {
        cudaFree(d_blank_frame_);
        d_blank_frame_ = nullptr;
    }
    if (h_display_crop_) {
        cudaFreeHost(h_display_crop_);
        h_display_crop_ = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    if (m_display_stream) {
        cudaSetDevice(kDisplayGpuId);
        cudaStreamDestroy(m_display_stream);
        m_display_stream = nullptr;
    }
}

bool CropAndEncodeWorker::ensure_recording_started(const std::string& recording_folder) {
    if (is_recording_) {
        return true;
    }

    if (recording_folder.empty()) {
        std::cerr << "[CropAndEncodeWorker] Refusing to start crop recording for "
                  << threadName
                  << ": frame has no recording folder." << std::endl;
        return false;
    }

    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(recording_folder);

        writer_.video_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop.mp4";
        writer_.keyframe_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_keyframe.csv";
        writer_.metadata_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_meta.csv";

        writer_.video = new FFmpegWriter(
            AV_CODEC_ID_HEVC,
            crop_width_,
            crop_height_,
            camera_params_->frame_rate,
            writer_.video_file.c_str(),
            writer_.keyframe_file.c_str());
        writer_.video->create_thread();

        writer_.metadata = new std::ofstream();
        writer_.metadata->open(writer_.metadata_file.c_str());
        if (!(*writer_.metadata)) {
            std::cout << "[CropAndEncodeWorker] Warning: Could not open metadata file!" << std::endl;
        } else {
            *writer_.metadata
                << "recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,"
                << "has_detection,blank_frame,detection_confidence,"
                << "crop_x,crop_y,crop_w,crop_h,"
                << "detection_x,detection_y,detection_w,detection_h\n";
        }
    }

    last_frame_id_used_ = 0;
    encoder_flushed_ = false;
    camera_control_->active_recorders.fetch_add(1, std::memory_order_relaxed);
    is_recording_ = true;
    return true;
}

void CropAndEncodeWorker::push_encoded_packets(
    std::vector<std::vector<uint8_t>>& packets,
    const std::vector<uint64_t>& output_timestamps,
    uint64_t fallback_zero_based_frame)
{
    if (!writer_.video) {
        if (!packets.empty()) {
            std::cerr << "[CropAndEncodeWorker] Warning: dropping "
                      << packets.size()
                      << " encoded packets because crop writer is not open." << std::endl;
        }
        return;
    }

    for (size_t i = 0; i < packets.size(); ++i) {
        const int64_t sample_index = i < output_timestamps.size()
            ? static_cast<int64_t>(output_timestamps[i])
            : static_cast<int64_t>(fallback_zero_based_frame);
        writer_.video->push_packet(
            packets[i].data(),
            static_cast<int>(packets[i].size()),
            sample_index);
        last_frame_id_used_ = std::max<uint64_t>(
            last_frame_id_used_,
            static_cast<uint64_t>(sample_index + 1));
    }
}

void CropAndEncodeWorker::write_metadata_row(
    const WORKER_ENTRY* entry,
    bool has_detection,
    bool blank_frame,
    float detection_confidence,
    int crop_x,
    int crop_y,
    int crop_w,
    int crop_h,
    float detection_x,
    float detection_y,
    float detection_w,
    float detection_h)
{
    if (!writer_.metadata || !writer_.metadata->is_open() || !entry) {
        return;
    }

    *writer_.metadata << entry->recording_frame_id << ','
                      << entry->frame_id << ','
                      << entry->camera_frame_id << ','
                      << entry->timestamp << ','
                      << entry->timestamp_sys << ','
                      << (has_detection ? 1 : 0) << ','
                      << (blank_frame ? 1 : 0) << ','
                      << detection_confidence << ','
                      << crop_x << ','
                      << crop_y << ','
                      << crop_w << ','
                      << crop_h << ','
                      << detection_x << ','
                      << detection_y << ','
                      << detection_w << ','
                      << detection_h << '\n';
}

void CropAndEncodeWorker::release_entry(WORKER_ENTRY* entry) {
    if (!entry) {
        return;
    }

    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue.push(entry);
    }
}

size_t CropAndEncodeWorker::crop_preview_bytes() const
{
    return static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_) * 4;
}

bool CropAndEncodeWorker::display_cuda_ok(cudaError_t status, const char* operation)
{
    if (status == cudaSuccess) {
        return true;
    }

    if (!display_preview_disabled_) {
        std::cerr << "[CropAndEncodeWorker] Disabling crop preview for "
                  << threadName << ": " << operation << " failed: "
                  << cudaGetErrorString(status) << std::endl;
    }

    display_preview_disabled_ = true;
    d_display_buffer_pbo_ = nullptr;
    if (camera_params_) {
        cudaSetDevice(camera_params_->gpu_id);
    }
    return false;
}

void CropAndEncodeWorker::copy_crop_to_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemcpyAsync(
                d_display_buffer_pbo_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToDevice,
                m_stream),
            "cudaMemcpyAsync(crop preview same GPU)");
        return;
    }

    if (!h_display_crop_ || !m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview cross-GPU staging");
        return;
    }

    if (!display_cuda_ok(
            cudaMemcpyAsync(
                h_display_crop_,
                d_cropped_rgba_,
                crop_preview_bytes(),
                cudaMemcpyDeviceToHost,
                m_stream),
            "cudaMemcpyAsync(crop preview device-to-host)")) {
        return;
    }
    if (!display_cuda_ok(cudaStreamSynchronize(m_stream), "cudaStreamSynchronize(crop preview camera stream)")) {
        return;
    }
    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview display GPU)")) {
        return;
    }
    display_cuda_ok(
        cudaMemcpyAsync(
            d_display_buffer_pbo_,
            h_display_crop_,
            crop_preview_bytes(),
            cudaMemcpyHostToDevice,
            m_display_stream),
        "cudaMemcpyAsync(crop preview host-to-display)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::clear_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        display_cuda_ok(
            cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), m_stream),
            "cudaMemsetAsync(crop preview same GPU)");
        return;
    }

    if (!m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview clear display GPU)")) {
        return;
    }
    display_cuda_ok(
        cudaMemsetAsync(d_display_buffer_pbo_, 0, crop_preview_bytes(), m_display_stream),
        "cudaMemsetAsync(crop preview display GPU)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::synchronize_display_preview()
{
    if (!d_display_buffer_pbo_ || display_preview_disabled_) {
        return;
    }

    if (camera_params_->gpu_id == kDisplayGpuId) {
        return;
    }

    if (!m_display_stream) {
        display_cuda_ok(cudaErrorInvalidResourceHandle, "crop preview display stream synchronize");
        return;
    }

    if (!display_cuda_ok(cudaSetDevice(kDisplayGpuId), "cudaSetDevice(crop preview synchronize display GPU)")) {
        return;
    }
    display_cuda_ok(cudaStreamSynchronize(m_display_stream), "cudaStreamSynchronize(crop preview display stream)");
    cudaSetDevice(camera_params_->gpu_id);
}

void CropAndEncodeWorker::flush_and_close() {
    std::cout << "[CropAndEncodeWorker] Flushing and closing for " << threadName << std::endl;

    if (encoder_ && writer_.video && !encoder_flushed_) {
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint64_t> output_timestamps;
        encoder_->EndEncode(packets, nullptr, &output_timestamps);
        push_encoded_packets(packets, output_timestamps, last_frame_id_used_);
        encoder_flushed_ = true;
        std::cout << "[CropAndEncodeWorker] Encoder flushed." << std::endl;
    }

    if (writer_.video) {
        std::cout << "[CropAndEncodeWorker] Closing video writer for " << threadName
                  << " queued_packets=" << writer_.video->queued_packets()
                  << " queued_bytes=" << writer_.video->queued_bytes()
                  << std::endl;
        writer_.video->quit_thread();
        std::cout << "[CropAndEncodeWorker] Video writer quit signal sent." << std::endl;
        writer_.video->join_thread();
        std::cout << "[CropAndEncodeWorker] Video writer thread joined." << std::endl;
        delete writer_.video;
        writer_.video = nullptr;
        std::cout << "[CropAndEncodeWorker] Video writer closed." << std::endl;
    }
    
    if (writer_.metadata) {
        if (writer_.metadata->is_open()) {
            writer_.metadata->close();
        }
        delete writer_.metadata;
        writer_.metadata = nullptr;
    }
}

void CropAndEncodeWorker::finalize_recording()
{
    if (!is_recording_) {
        return;
    }

    flush_and_close();
    is_recording_ = false;

    if (camera_control_) {
        int remaining = camera_control_->active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining == 0) {
            if (camera_control_->recording_draining) {
                camera_control_->recording_draining = false;
            }
            camera_control_->stop_record = false;
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            camera_control_->recording_folder.clear();
        }
    }
}

bool CropAndEncodeWorker::drain_ready()
{
    return (GetCountQueueInSize() == 0);
}


bool CropAndEncodeWorker::WorkerFunction(WORKER_ENTRY* entry) {
    // Set the correct CUDA device for this worker's operations.
    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);

    if (!entry) {
        return false;
    }

    // YOLO reaches this worker after acquisition, so global record_video can
    // change before a frame is processed. Use the per-frame recording identity
    // to avoid encoding stale pre-record frames or dropping valid late frames.
    const bool frame_should_encode =
        entry->recording_frame_id > 0 && !entry->recording_folder.empty();

    if (frame_should_encode && !is_recording_) {
        ensure_recording_started(entry->recording_folder);
    } else if (!frame_should_encode && is_recording_ && !camera_control_->record_video) {
        // The queue has reached post-recording preview frames; close the run.
        finalize_recording();
    }

    const bool encode_this_frame = frame_should_encode && is_recording_;

    try {
        if (entry->width < crop_width_ || entry->height < crop_height_) {
            std::cerr << "[CropAndEncodeWorker] Dropping crop frame for camera "
                      << camera_params_->camera_serial
                      << ": source frame "
                      << entry->width << "x" << entry->height
                      << " is smaller than crop "
                      << crop_width_ << "x" << crop_height_ << std::endl;
            release_entry(entry);
            return false;
        }

        // Wait for the incoming frame data to be ready
        if (entry->event_ptr) {
            ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
        }
        
        bool has_detection = !entry->detections.empty();

        if (has_detection) {
            // --- DETECTION IS FOUND ---
            pose::Object best_detection = *std::max_element(
                entry->detections.begin(), entry->detections.end(),
                [](const pose::Object& a, const pose::Object& b) { return a.prob < b.prob; });
            
            const int CROP_W = crop_width_;
            const int CROP_H = crop_height_;
            float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
            float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
            int ix = std::clamp(static_cast<int>(cx) - CROP_W / 2, 0, entry->width - CROP_W);
            int iy = std::clamp(static_cast<int>(cy) - CROP_H / 2, 0, entry->height - CROP_H);
            
            // --- LIVE PREVIEW LOGIC (ALWAYS RUNS) ---
            if (d_display_buffer_pbo_) {
                pose::Rect crop_rect = {(float)ix, (float)iy, (float)CROP_W, (float)CROP_H};
                gpu_crop_and_resize_rgba(entry->d_image, d_cropped_rgba_, entry->width, entry->height,
                                         crop_rect, CROP_W, CROP_H, m_stream);
                
                copy_crop_to_display_preview();
            }
            
            // --- RECORDING LOGIC (ONLY RUNS IF RECORDING IS ON) ---
            if (encode_this_frame) {
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                unsigned char* d_nv12_dst = static_cast<unsigned char*>(encIn->inputPtr);
                const uint64_t zero_based_recording_frame =
                    entry->recording_frame_id > 0 ? entry->recording_frame_id - 1 : 0;
                NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
                pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
                pic_params.inputTimeStamp = zero_based_recording_frame;
                pic_params.inputDuration = 1;

                ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch, entry->d_image + (iy * entry->width + ix),
                                     entry->width, CROP_W, CROP_H, cudaMemcpyDeviceToDevice, m_stream));
                
                unsigned char* d_uv_plane_dst = d_nv12_dst + encIn->pitch * CROP_H;
                ck(cudaMemset2DAsync(d_uv_plane_dst, encIn->pitch, 128, CROP_W, CROP_H / 2, m_stream));

                // Encode and write the frame to file
                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                encoder_->EncodeFrame(packets, &pic_params, nullptr, &output_timestamps);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);

                // Write metadata to file
                write_metadata_row(
                    entry,
                    true,
                    false,
                    best_detection.prob,
                    ix,
                    iy,
                    CROP_W,
                    CROP_H,
                    best_detection.rect.x,
                    best_detection.rect.y,
                    best_detection.rect.width,
                    best_detection.rect.height);
            }
        } else {
            // --- NO DETECTION ---
            // Always show a blank screen for the preview if no detection
            if (d_display_buffer_pbo_) {
                clear_display_preview();
            }
            
            // Only encode a blank frame if recording is active
            if (encode_this_frame) {
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                const uint64_t zero_based_recording_frame =
                    entry->recording_frame_id > 0 ? entry->recording_frame_id - 1 : 0;
                NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
                pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
                pic_params.inputTimeStamp = zero_based_recording_frame;
                pic_params.inputDuration = 1;
                ck(cudaMemcpy2DAsync(encIn->inputPtr, encIn->pitch, d_blank_frame_,
                                     encoder_pitch_, encoder_pitch_, crop_height_ * 3 / 2,
                                     cudaMemcpyDeviceToDevice, m_stream));

                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                encoder_->EncodeFrame(packets, &pic_params, nullptr, &output_timestamps);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);
                write_metadata_row(
                    entry,
                    false,
                    true,
                    0.0f,
                    0,
                    0,
                    0,
                    0,
                    0.0f,
                    0.0f,
                    0.0f,
                    0.0f);
            }
        }

        // Sync streams before releasing the source entry. This is transitional;
        // the future crop producer should replace this with readiness events.
        ck(cudaStreamSynchronize(m_stream));
        if (d_display_buffer_pbo_) {
            synchronize_display_preview();
        }

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Exception processing frame " << entry->frame_id
                  << ": " << e.what() << std::endl;
    }

    // Cleanup and recycle the entry
    release_entry(entry);

    return false; // This worker does not pass items to its own output queue
}
