// src/crop_and_encode_worker.cpp

#include "crop_and_encode_worker.h"
#include "crop_preview_worker.h"
#include "crop_producer_worker.h"
#include "external_recorder_ipc_protocol.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "project.h" // Add this include
#include "fsuid_guard.h"
#include "video_encode_profile.h"
#include <nppi.h>
#include <npp.h>
#include <nppi_color_conversion.h>
#include <nppi_geometry_transforms.h>
#include <algorithm> // For std::max_element
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>

namespace {
constexpr const char* kCropRecordingSinkModeEnv = "ORANGE_CROP_RECORDING_SINK_MODE";

std::string normalize_crop_recording_sink_mode(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty() || value == "real" || value == "in_process" || value == "inprocess") {
        return "in_process";
    }
    if (value == "external_ipc") {
        return value;
    }
    return {};
}

std::string resolve_crop_recording_sink_mode(const char* worker_name)
{
    const char* env_value = std::getenv(kCropRecordingSinkModeEnv);
    const std::string normalized =
        normalize_crop_recording_sink_mode(env_value ? env_value : "");
    if (normalized.empty()) {
        std::cerr << "[CropAndEncodeWorker] Ignoring invalid "
                  << kCropRecordingSinkModeEnv << "='"
                  << (env_value ? env_value : "")
                  << "' for " << (worker_name ? worker_name : "unknown")
                  << "; using in_process" << std::endl;
        return "in_process";
    }
    return normalized;
}

uint64_t steady_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

double elapsed_ms(uint64_t start_ns, uint64_t end_ns)
{
    if (end_ns < start_ns) {
        return 0.0;
    }
    return static_cast<double>(end_ns - start_ns) / 1000000.0;
}

size_t encoded_packet_bytes(const std::vector<std::vector<uint8_t>>& packets)
{
    size_t total = 0;
    for (const auto& packet : packets) {
        total += packet.size();
    }
    return total;
}

}

int CropAndEncodeWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

class CropAndEncodeWorker::ExternalCropIpcClient {
public:
    ExternalCropIpcClient(std::string camera_serial,
                          int source_gpu_id,
                          int crop_width,
                          int crop_height,
                          int gop_length)
        : camera_serial_(std::move(camera_serial)),
          source_gpu_id_(source_gpu_id),
          crop_width_(crop_width),
          crop_height_(crop_height),
          gop_length_(std::max(1, gop_length))
    {
    }

    ~ExternalCropIpcClient()
    {
        close_socket();
    }

    void Close()
    {
        send_client_drain_control("crop_recording_drained");
        send_client_finalize_control("crop_recording_drained");
        close_socket();
    }

    void NotifyDrain(const char* reason)
    {
        send_client_drain_control(reason ? reason : "crop_recording_draining");
    }

    bool Submit(const CropFrameSnapshot& frame,
                unsigned char* d_crop_mono,
                uint64_t bytes,
                const std::string& recording_folder)
    {
        if (!d_crop_mono || bytes == 0) {
            log_limited("missing crop buffer for frame " +
                        std::to_string(frame.recording_frame_id));
            return false;
        }
        refresh_session_from_environment(recording_folder);
        if (source_gpu_id_ >= 0) {
            cudaSetDevice(source_gpu_id_);
        }

        std::string handle_hex;
        auto cached = handle_cache_.find(d_crop_mono);
        if (cached == handle_cache_.end()) {
            cudaIpcMemHandle_t handle{};
            const cudaError_t status =
                cudaIpcGetMemHandle(&handle, static_cast<void*>(d_crop_mono));
            if (status != cudaSuccess) {
                log_limited(std::string("cudaIpcGetMemHandle failed: ") +
                            cudaGetErrorString(status));
                return false;
            }
            handle_hex = handle_to_hex(handle);
            handle_cache_.emplace(d_crop_mono, handle_hex);
        } else {
            handle_hex = cached->second;
        }

        if (!ensure_connected()) {
            return false;
        }

        const uint64_t zero_based_frame =
            frame.recording_frame_id > 0 ? frame.recording_frame_id - 1 : 0;
        const uint64_t gop_index =
            zero_based_frame / static_cast<uint64_t>(gop_length_);
        const uint32_t frame_index_within_gop = static_cast<uint32_t>(
            zero_based_frame % static_cast<uint64_t>(gop_length_));

        std::ostringstream msg;
        msg << "FRAME "
            << camera_serial_ << " "
            << frame.recording_frame_id << " "
            << frame.local_frame_id << " "
            << source_gpu_id_ << " "
            << crop_width_ << " "
            << crop_height_ << " "
            << 0 << " "
            << bytes << " "
            << frame.timestamp << " "
            << frame.timestamp_sys << " "
            << handle_hex << " "
            << session_id_ << " "
            << stream_id_ << " "
            << gop_index << " "
            << frame_index_within_gop << " "
            << source_gpu_id_ << " "
            << 0 << " "
            << "single_shard"
            << "\n";

        if (!send_all(msg.str())) {
            log_limited("send crop frame descriptor failed: " +
                        std::string(std::strerror(errno)));
            close_socket();
            return false;
        }
        if (!read_ack(frame.recording_frame_id)) {
            log_limited("crop detach ack failed for frame " +
                        std::to_string(frame.recording_frame_id));
            close_socket();
            return false;
        }
        return true;
    }

private:
    static std::string handle_to_hex(const cudaIpcMemHandle_t& handle)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&handle);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (size_t i = 0; i < sizeof(cudaIpcMemHandle_t); ++i) {
            out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        }
        return out.str();
    }

    std::string resolve_session_id(const std::string& recording_folder) const
    {
        const std::string per_camera =
            "ORANGE_EXTERNAL_CROP_RECORDER_SESSION_ID_CAM_" + camera_serial_;
        if (const char* value = std::getenv(per_camera.c_str()); value && *value) {
            return value;
        }
        if (const char* value = std::getenv("ORANGE_EXTERNAL_CROP_RECORDER_SESSION_ID");
            value && *value) {
            return value;
        }
        const std::string full_per_camera =
            "ORANGE_EXTERNAL_RECORDER_SESSION_ID_CAM_" + camera_serial_;
        if (const char* value = std::getenv(full_per_camera.c_str()); value && *value) {
            return value;
        }
        if (const char* value = std::getenv("ORANGE_EXTERNAL_RECORDER_SESSION_ID");
            value && *value) {
            return value;
        }
        if (!recording_folder.empty()) {
            return std::filesystem::path(recording_folder).filename().string();
        }
        return "external_crop_ipc_" + camera_serial_;
    }

    std::string resolve_socket_path() const
    {
        const std::string per_camera =
            "ORANGE_EXTERNAL_CROP_RECORDER_SOCKET_CAM_" + camera_serial_;
        if (const char* value = std::getenv(per_camera.c_str()); value && *value) {
            return value;
        }
        if (const char* value = std::getenv("ORANGE_EXTERNAL_CROP_RECORDER_SOCKET");
            value && *value) {
            return value;
        }
        const std::string supervised_crop =
            "ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_" + camera_serial_ + "_crop";
        if (const char* value = std::getenv(supervised_crop.c_str()); value && *value) {
            return value;
        }
        return "/tmp/orange_external_recorder_" + camera_serial_ + "_crop.sock";
    }

    void refresh_session_from_environment(const std::string& recording_folder)
    {
        const std::string next_session = resolve_session_id(recording_folder);
        const std::string next_socket = resolve_socket_path();
        const std::string next_stream = camera_serial_ + "_crop";
        if (next_session == session_id_ &&
            next_socket == socket_path_ &&
            next_stream == stream_id_) {
            return;
        }
        close_socket();
        session_id_ = next_session;
        socket_path_ = next_socket;
        stream_id_ = next_stream;
    }

    bool ensure_connected()
    {
        if (socket_fd_ >= 0) {
            return true;
        }
        socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            log_limited("socket() failed: " + std::string(std::strerror(errno)));
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(addr.sun_path)) {
            log_limited("socket path too long: " + socket_path_);
            close_socket();
            return false;
        }
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log_limited("connect(" + socket_path_ + ") failed: " +
                        std::string(std::strerror(errno)));
            close_socket();
            return false;
        }
        std::cout << "[ExternalCropIpcRecorder] Connected crop stream "
                  << camera_serial_ << " to " << socket_path_ << std::endl;
        if (!read_recorder_hello()) {
            close_socket();
            return false;
        }
        return true;
    }

    bool send_all(const std::string& data)
    {
        const char* cursor = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
            const ssize_t written = send(socket_fd_, cursor, remaining, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        return true;
    }

    bool read_protocol_line(std::string* line)
    {
        while (receive_buffer_.find('\n') == std::string::npos) {
            char ch = '\0';
            const ssize_t n = recv(socket_fd_, &ch, 1, 0);
            if (n <= 0) {
                return false;
            }
            receive_buffer_.push_back(ch);
            if (receive_buffer_.size() > 4096) {
                return false;
            }
        }
        const size_t newline = receive_buffer_.find('\n');
        if (line) {
            *line = receive_buffer_.substr(0, newline);
        }
        receive_buffer_.erase(0, newline + 1);
        return true;
    }

    bool read_ack(uint64_t recording_frame_id)
    {
        bool deferred_release = false;
        while (true) {
            std::string line;
            if (!read_protocol_line(&line)) {
                return false;
            }
            bool malformed_status = false;
            if (handle_recorder_status_line(line, &malformed_status)) {
                if (malformed_status) {
                    return false;
                }
                continue;
            }

            std::istringstream in(line);
            std::string kind;
            uint64_t frame_id = 0;
            in >> kind >> frame_id;
            if (kind != "ACK" || frame_id != recording_frame_id) {
                return false;
            }

            std::string token;
            while (in >> token) {
                if (token == "deferred_release") {
                    deferred_release = true;
                    break;
                }
            }
            break;
        }
        if (!deferred_release) {
            return true;
        }

        while (true) {
            std::string line;
            if (!read_protocol_line(&line)) {
                return false;
            }
            bool malformed_status = false;
            if (handle_recorder_status_line(line, &malformed_status)) {
                if (malformed_status) {
                    return false;
                }
                continue;
            }
            std::istringstream release_in(line);
            std::string kind;
            uint64_t frame_id = 0;
            release_in >> kind >> frame_id;
            return kind == "RELEASE" && frame_id == recording_frame_id;
        }
    }

    bool read_recorder_hello()
    {
        std::string line;
        if (!read_protocol_line(&line)) {
            log_limited("failed waiting for external crop recorder protocol hello");
            return false;
        }
        orange::external_recorder::ipc::HelloFields hello;
        if (!orange::external_recorder::ipc::parse_recorder_hello_line(line, &hello)) {
            log_limited("invalid external crop recorder protocol hello: " +
                        hello.error + " line='" + line + "'");
            return false;
        }
        if (!send_all(orange::external_recorder::ipc::build_client_hello_line(
                camera_serial_,
                session_id_,
                stream_id_,
                "orange_crop"))) {
            log_limited("send crop client protocol hello failed: " +
                        std::string(std::strerror(errno)));
            return false;
        }
        client_hello_sent_ = true;
        return true;
    }

    bool send_client_control(const char* command, const char* reason)
    {
        if (socket_fd_ < 0 || !client_hello_sent_) {
            return false;
        }
        if (!send_all(orange::external_recorder::ipc::build_client_control_line(
                camera_serial_,
                session_id_,
                stream_id_,
                "orange_crop",
                command ? command : "",
                reason ? reason : "drained"))) {
            log_limited("send crop client control failed: " +
                        std::string(std::strerror(errno)));
            return false;
        }
        return true;
    }

    void send_client_drain_control(const char* reason)
    {
        if (client_drain_sent_) {
            return;
        }
        if (send_client_control(
                orange::external_recorder::ipc::kClientControlDrain,
                reason)) {
            client_drain_sent_ = true;
        }
    }

    void send_client_finalize_control(const char* reason)
    {
        if (client_finalize_sent_) {
            return;
        }
        if (!client_drain_sent_) {
            send_client_drain_control(reason);
        }
        if (!send_client_control(
                orange::external_recorder::ipc::kClientControlFinalize,
                reason)) {
            return;
        }
        client_finalize_sent_ = true;
    }

    bool handle_recorder_status_line(const std::string& line, bool* malformed)
    {
        if (malformed) {
            *malformed = false;
        }
        if (!orange::external_recorder::ipc::starts_with_kind(
                line,
                orange::external_recorder::ipc::kRecorderStatusKind)) {
            return false;
        }
        orange::external_recorder::ipc::RecorderStatusFields status;
        if (!orange::external_recorder::ipc::parse_recorder_status_line(
                line,
                &status)) {
            log_limited("invalid external crop recorder status protocol line: " +
                        status.error + " line='" + line + "'");
            if (malformed) {
                *malformed = true;
            }
        }
        return true;
    }

    void close_socket()
    {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        receive_buffer_.clear();
        client_hello_sent_ = false;
        client_drain_sent_ = false;
        client_finalize_sent_ = false;
    }

    void log_limited(const std::string& message)
    {
        const uint64_t count = failures_logged_++;
        if (count < 10 || (count % 100) == 0) {
            std::cerr << "[ExternalCropIpcRecorder] camera=" << camera_serial_
                      << " " << message << std::endl;
        }
    }

    std::string camera_serial_;
    int source_gpu_id_ = -1;
    int crop_width_ = 0;
    int crop_height_ = 0;
    int gop_length_ = 1;
    std::string session_id_;
    std::string stream_id_;
    std::string socket_path_;
    int socket_fd_ = -1;
    bool client_hello_sent_ = false;
    bool client_drain_sent_ = false;
    bool client_finalize_sent_ = false;
    std::string receive_buffer_;
    std::unordered_map<unsigned char*, std::string> handle_cache_;
    uint64_t failures_logged_ = 0;
};

CropAndEncodeWorker::CropAndEncodeWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& folder_name,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    unsigned char* /*display_buffer_pbo*/,
    CameraControl* camera_control,
    int crop_size_px
):CThreadWorker(name),
camera_params_(camera_params),
base_folder_name_(folder_name),
crop_width_(SanitizeCropSize(crop_size_px)),
crop_height_(SanitizeCropSize(crop_size_px)),
m_recycle_queue(recycle_queue),
camera_control_(camera_control)
{

    std::cout << "[CropAndEncodeWorker] Initializing " << name << " on GPU " << camera_params_->gpu_id << std::endl;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&m_stream, cudaStreamNonBlocking));
    crop_recording_sink_mode_ = resolve_crop_recording_sink_mode(name);
    std::cout << "[CropAndEncodeWorker] Crop recording sink mode for "
              << name << ": " << crop_recording_sink_mode_ << std::endl;

    if (external_crop_recording_enabled()) {
        // The external crop recorder contract uses GOP=1 so every crop frame is
        // independently routable and future crop clip rollover can use exact
        // recording-frame boundaries.
        const int gop_length = 1;
        external_crop_ipc_ = std::make_unique<ExternalCropIpcClient>(
            camera_params_->camera_serial,
            camera_params_->gpu_id,
            crop_width_,
            crop_height_,
            gop_length);
        ck(cudaMalloc(
            &d_blank_frame_,
            static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_)));
        ck(cudaMemsetAsync(
            d_blank_frame_,
            0,
            static_cast<size_t>(crop_width_) * static_cast<size_t>(crop_height_),
            m_stream));
        return;
    }

    try {
        CUcontext cuContext;
        ck(cuCtxGetCurrent(&cuContext));

        encoder_ = new NvEncoderCuda(cuContext, crop_width_, crop_height_, NV_ENC_BUFFER_FORMAT_NV12);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;

        VideoEncodeProfile encode_profile =
            build_crop_video_encode_profile(*camera_params_, crop_width_, crop_height_);
        const VideoEncodeProfileNvencGuids nvenc_guids =
            resolve_video_encode_profile_nvenc_guids(encode_profile);

        encoder_->CreateDefaultEncoderParams(
            &initializeParams,
            nvenc_guids.codec_guid,
            nvenc_guids.preset_guid,
            nvenc_guids.tuning_info);
        apply_video_encode_profile_to_nvenc_config(
            encode_profile,
            &initializeParams,
            &encodeConfig);
        std::cout << "[CropAndEncodeWorker] Resolved video encode profile for "
                  << name << ": "
                  << video_encode_profile_summary(encode_profile)
                  << std::endl;

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

    if (d_blank_frame_) {
        cudaFree(d_blank_frame_);
        d_blank_frame_ = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }

    std::cout << "[CropAndEncodeWorker] Summary for " << threadName
              << " jobs_enqueued=" << jobs_enqueued_.load(std::memory_order_relaxed)
              << " queue_full_drops=" << queue_full_drops_.load(std::memory_order_relaxed)
              << " queue_high_water=" << queue_high_water_.load(std::memory_order_relaxed)
              << " encoded_frames=" << encoded_frames_
              << " blank_frames=" << blank_frames_encoded_
              << " dropped_frames=" << dropped_frames_
              << std::endl;
}

void CropAndEncodeWorker::reset_recording_counters()
{
    run_jobs_enqueued_ = 0;
    run_queue_full_drops_ = 0;
    run_queue_high_water_ = 0;
}

void CropAndEncodeWorker::SetMaxQueueSize(int size)
{
    max_queue_size_ = std::max(1, size);
    CThreadWorker<CropEncodeJob>::SetMaxQueueSize(max_queue_size_);
}

bool CropAndEncodeWorker::TryEnqueueJob(CropEncodeJob* job)
{
    if (!job) {
        return false;
    }

    const int queue_depth = GetCountQueueInSize();
    queue_high_water_.store(
        std::max(queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
        std::memory_order_relaxed);
    const bool record_active =
        job->frame.recording_frame_id > 0 && !job->frame.recording_folder.empty();
    if (record_active) {
        run_queue_high_water_ = std::max(run_queue_high_water_, queue_depth + 1);
    }
    if (queue_depth >= max_queue_size_) {
        queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
        if (record_active) {
            ++run_queue_full_drops_;
        }
        return false;
    }

    jobs_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (record_active) {
        ++run_jobs_enqueued_;
    }
    PutObjectToQueueIn(job);
    return true;
}

bool CropAndEncodeWorker::external_crop_recording_enabled() const
{
    return crop_recording_sink_mode_ == "external_ipc";
}

bool CropAndEncodeWorker::submit_external_crop_frame(const CropFrameSnapshot& frame,
                                                     unsigned char* d_crop_mono,
                                                     CropEncodePerfSample* perf)
{
    if (!external_crop_ipc_) {
        if (perf) {
            perf->dropped = true;
            perf->drop_reason = "external_crop_ipc_not_initialized";
        }
        return false;
    }

    const uint64_t submit_start_ns = steady_now_ns();
    const cudaError_t sync_status = cudaStreamSynchronize(m_stream);
    if (sync_status != cudaSuccess) {
        std::cerr << "[CropAndEncodeWorker] External crop IPC source sync failed for "
                  << threadName << " frame " << frame.recording_frame_id
                  << ": " << cudaGetErrorString(sync_status) << std::endl;
        if (perf) {
            perf->dropped = true;
            perf->drop_reason = "external_crop_ipc_source_sync_failed";
            perf->stream_sync_ms += elapsed_ms(submit_start_ns, steady_now_ns());
        }
        cudaGetLastError();
        return false;
    }
    if (perf) {
        perf->stream_sync_ms += elapsed_ms(submit_start_ns, steady_now_ns());
    }

    const uint64_t ipc_start_ns = steady_now_ns();
    const bool ok = external_crop_ipc_->Submit(
        frame,
        d_crop_mono,
        static_cast<uint64_t>(crop_width_) * static_cast<uint64_t>(crop_height_),
        frame.recording_folder);
    if (perf) {
        perf->encode_submit_cpu_ms = elapsed_ms(ipc_start_ns, steady_now_ns());
        if (!ok) {
            perf->dropped = true;
            perf->drop_reason = "external_crop_ipc_submit_failed";
        }
    }
    return ok;
}

void CropAndEncodeWorker::RotateRecordingFolder(const std::string& recording_folder)
{
    if (recording_folder.empty()) {
        return;
    }

    if (current_sidecar_recording_folder_ == recording_folder) {
        return;
    }

    if (!current_sidecar_recording_folder_.empty()) {
        write_sidecar_summary();
    }

    reset_recording_counters();
    current_sidecar_recording_folder_ = recording_folder;

    crop_sidecar_perf_file_ =
        recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_sidecar_perf.csv";

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream sidecar_perf(crop_sidecar_perf_file_.c_str(), std::ios::out | std::ios::trunc);
    if (!sidecar_perf) {
        std::cerr << "[CropAndEncodeWorker] Warning: Could not open crop sidecar perf file for "
                  << threadName << ": " << crop_sidecar_perf_file_ << std::endl;
        return;
    }

    sidecar_perf
        << "camera_serial,gpu_id,worker,queue_size,"
        << "producer_jobs_offered,producer_jobs_enqueued,producer_queue_full_drops,"
        << "producer_blank_jobs_offered,producer_blank_jobs_enqueued,"
        << "producer_dropped_jobs_offered,producer_dropped_jobs_enqueued,"
        << "consumer_jobs_enqueued,consumer_queue_full_drops,consumer_queue_high_water,"
        << "crop_frame_pool_size,"
        << "producer_recording_crop_frame_offered,producer_recording_crop_frame_accepted,"
        << "producer_recording_crop_frame_dropped,"
        << "producer_preview_crop_frame_offered,producer_preview_crop_frame_accepted,"
        << "producer_preview_crop_frame_dropped,"
        << "producer_pose_crop_frame_offered,producer_pose_crop_frame_accepted,"
        << "producer_pose_crop_frame_dropped,"
        << "producer_frames_produced_total,producer_frames_recycled_total,"
        << "producer_crop_frame_release_total,producer_crop_frame_pool_misses_total,"
        << "producer_source_release_event_misses_total,"
        << "producer_pending_source_releases,producer_pending_crop_frame_recycles,"
        << "preview_max_fps,preview_disabled,preview_display_enabled_final,preview_frames_offered,"
        << "preview_frames_updated,preview_frames_skipped_by_cadence,"
        << "preview_clears_updated,preview_queue_full_drops,preview_queue_high_water,"
        << "preview_serial_final\n";
}

void CropAndEncodeWorker::write_sidecar_summary()
{
    if (crop_sidecar_perf_file_.empty()) {
        return;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream sidecar_perf(crop_sidecar_perf_file_.c_str(), std::ios::out | std::ios::app);
    if (!sidecar_perf) {
        std::cerr << "[CropAndEncodeWorker] Warning: Could not append crop sidecar perf file for "
                  << threadName << ": " << crop_sidecar_perf_file_ << std::endl;
        return;
    }

    CropProducerWorker::RecordingCounters producer_counters;
    if (crop_producer_worker_) {
        producer_counters = crop_producer_worker_->GetRecordingCounters();
    }
    CropPreviewWorker::Summary preview_summary;
    if (crop_preview_worker_) {
        crop_preview_worker_->WaitUntilIdle(2000);
        preview_summary = crop_preview_worker_->GetSummary();
    }
    CropProducer::FanoutCounters fanout_counters;
    if (crop_producer_) {
        fanout_counters = crop_producer_->GetFanoutCounters();
    }

    sidecar_perf
        << camera_params_->camera_serial << ','
        << camera_params_->gpu_id << ','
        << "CropAndEncodeWorker,"
        << max_queue_size_ << ','
        << producer_counters.jobs_offered << ','
        << producer_counters.jobs_enqueued << ','
        << producer_counters.queue_full_drops << ','
        << producer_counters.blank_jobs_offered << ','
        << producer_counters.blank_jobs_enqueued << ','
        << producer_counters.dropped_jobs_offered << ','
        << producer_counters.dropped_jobs_enqueued << ','
        << run_jobs_enqueued_ << ','
        << run_queue_full_drops_ << ','
        << run_queue_high_water_ << ','
        << (crop_producer_ ? crop_producer_->crop_frame_pool_size() : 0) << ','
        << fanout_counters.recording_crop_frame_offered << ','
        << fanout_counters.recording_crop_frame_accepted << ','
        << fanout_counters.recording_crop_frame_dropped << ','
        << fanout_counters.preview_crop_frame_offered << ','
        << fanout_counters.preview_crop_frame_accepted << ','
        << fanout_counters.preview_crop_frame_dropped << ','
        << fanout_counters.pose_crop_frame_offered << ','
        << fanout_counters.pose_crop_frame_accepted << ','
        << fanout_counters.pose_crop_frame_dropped << ','
        << fanout_counters.frames_produced_total << ','
        << fanout_counters.frames_recycled_total << ','
        << fanout_counters.crop_frame_release_total << ','
        << fanout_counters.crop_frame_pool_misses_total << ','
        << fanout_counters.source_release_event_misses_total << ','
        << fanout_counters.pending_source_releases << ','
        << fanout_counters.pending_crop_frame_recycles << ','
        << preview_summary.max_fps << ','
        << (preview_summary.available ? 0 : 1) << ','
        << (preview_summary.display_enabled ? 1 : 0) << ','
        << preview_summary.frames_offered << ','
        << preview_summary.frames_updated << ','
        << preview_summary.frames_skipped_by_cadence << ','
        << preview_summary.clears_updated << ','
        << preview_summary.queue_full_drops << ','
        << preview_summary.queue_high_water << ','
        << preview_summary.serial << '\n';
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

    if (crop_sidecar_perf_file_.empty()) {
        RotateRecordingFolder(recording_folder);
    }

    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(recording_folder);

        writer_.video_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop.mp4";
        writer_.keyframe_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_keyframe.json";
        writer_.metadata_file = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_meta.csv";
        crop_perf_file_ = recording_folder + "/Cam" + camera_params_->camera_serial + "_crop_perf.csv";
        if (!external_crop_recording_enabled()) {
            const auto metadata_tags = build_video_encode_metadata_tags(
                build_crop_video_encode_profile(*camera_params_, crop_width_, crop_height_));

            writer_.video = new FFmpegWriter(
                AV_CODEC_ID_HEVC,
                crop_width_,
                crop_height_,
                camera_params_->frame_rate,
                writer_.video_file.c_str(),
                writer_.keyframe_file.c_str(),
                metadata_tags);
            writer_.video->create_thread();
        }

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

        crop_perf_.open(crop_perf_file_.c_str());
        if (!crop_perf_) {
            std::cout << "[CropAndEncodeWorker] Warning: Could not open crop perf file!" << std::endl;
        } else {
            crop_perf_
                << "recording_frame_id,local_frame_id,camera_frame_id,"
                << "worker_start_steady_ns,queue_depth_start,encode_active,"
                << "has_detection,blank_frame,dropped,drop_reason,"
                << "crop_x,crop_y,crop_w,crop_h,"
                << "packet_count,encoded_bytes,"
                << "event_wait_cpu_ms,crop_pool_wait_ms,crop_producer_cpu_ms,"
                << "crop_source_wait_enqueue_cpu_ms,analytics_owned_wait_cpu_ms,source_stage_enqueue_cpu_ms,"
                << "crop_copy_start_event_record_cpu_ms,"
                << "crop_roi_copy_enqueue_cpu_ms,"
                << "crop_ready_event_record_cpu_ms,source_release_event_record_cpu_ms,"
                << "crop_copy_gpu_ms,"
                << "crop_preview_cpu_ms,encode_submit_cpu_ms,"
                << "metadata_cpu_ms,stream_sync_ms,display_sync_ms,total_ms\n";
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

void CropAndEncodeWorker::write_metadata_row(const CropFrameSnapshot& frame)
{
    if (!writer_.metadata || !writer_.metadata->is_open()) {
        return;
    }

    *writer_.metadata << frame.recording_frame_id << ','
                      << frame.local_frame_id << ','
                      << frame.camera_frame_id << ','
                      << frame.timestamp << ','
                      << frame.timestamp_sys << ','
                      << (frame.has_detection ? 1 : 0) << ','
                      << (frame.blank_frame ? 1 : 0) << ','
                      << frame.detection_confidence << ','
                      << frame.crop_x << ','
                      << frame.crop_y << ','
                      << frame.crop_w << ','
                      << frame.crop_h << ','
                      << frame.detection_x << ','
                      << frame.detection_y << ','
                      << frame.detection_w << ','
                      << frame.detection_h << '\n';
}

void CropAndEncodeWorker::write_perf_row(const CropFrameSnapshot& frame, const CropEncodePerfSample& sample)
{
    if (!crop_perf_.is_open()) {
        return;
    }

    crop_perf_ << frame.recording_frame_id << ','
               << frame.local_frame_id << ','
               << frame.camera_frame_id << ','
               << sample.worker_start_steady_ns << ','
               << sample.queue_depth_start << ','
               << (sample.encode_active ? 1 : 0) << ','
               << (sample.has_detection ? 1 : 0) << ','
               << (sample.blank_frame ? 1 : 0) << ','
               << (sample.dropped ? 1 : 0) << ','
               << sample.drop_reason << ','
               << sample.crop_x << ','
               << sample.crop_y << ','
               << sample.crop_w << ','
               << sample.crop_h << ','
               << sample.packet_count << ','
               << sample.encoded_bytes << ','
               << sample.event_wait_cpu_ms << ','
               << sample.crop_pool_wait_ms << ','
               << sample.crop_producer_cpu_ms << ','
               << sample.crop_source_wait_enqueue_cpu_ms << ','
               << sample.analytics_owned_wait_cpu_ms << ','
               << sample.source_stage_enqueue_cpu_ms << ','
               << sample.crop_copy_start_event_record_cpu_ms << ','
               << sample.crop_roi_copy_enqueue_cpu_ms << ','
               << sample.crop_ready_event_record_cpu_ms << ','
               << sample.source_release_event_record_cpu_ms << ','
               << sample.crop_copy_gpu_ms << ','
               << sample.crop_preview_cpu_ms << ','
               << sample.encode_submit_cpu_ms << ','
               << sample.metadata_cpu_ms << ','
               << sample.stream_sync_ms << ','
               << sample.display_sync_ms << ','
               << sample.total_ms << '\n';
}

void CropAndEncodeWorker::flush_and_close() {
    std::cout << "[CropAndEncodeWorker] Flushing and closing for " << threadName << std::endl;

    if (external_crop_ipc_) {
        external_crop_ipc_->Close();
    }

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

    if (crop_perf_.is_open()) {
        crop_perf_.close();
    }
}

void CropAndEncodeWorker::finalize_recording()
{
    if (!is_recording_) {
        return;
    }

    write_sidecar_summary();
    flush_and_close();
    is_recording_ = false;
    crop_sidecar_perf_file_.clear();
    current_sidecar_recording_folder_.clear();
    reset_recording_counters();
    if (crop_producer_worker_) {
        crop_producer_worker_->CloseRecording();
    }

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
    return GetCountQueueInSize() == 0;
}


bool CropAndEncodeWorker::WorkerFunction(CropEncodeJob* raw_job) {
    // Set the correct CUDA device for this worker's operations.
    ck(cudaSetDevice(camera_params_->gpu_id));
    EnsureNppStream(m_stream);

    std::unique_ptr<CropEncodeJob> job(raw_job);
    if (camera_control_ && !camera_control_->record_video && is_recording_ &&
        external_crop_ipc_) {
        external_crop_ipc_->NotifyDrain("crop_recording_draining");
    }
    if (!job) {
        if (camera_control_ && !camera_control_->record_video && is_recording_) {
            if (!camera_control_->recording_draining || drain_ready()) {
                finalize_recording();
            }
        }
        return false;
    }

    CropFrameSnapshot& frame = job->frame;
    CropEncodePerfSample& perf = job->perf;
    CropFrame* active_crop_frame = job->crop_frame;
    CropFrame* timing_crop_frame = active_crop_frame;

    const bool frame_should_encode =
        frame.recording_frame_id > 0 && !frame.recording_folder.empty();

    if (frame_should_encode && !is_recording_) {
        ensure_recording_started(frame.recording_folder);
    } else if (!frame_should_encode && is_recording_ && !camera_control_->record_video) {
        // The queue has reached post-recording preview frames; close the run.
        finalize_recording();
    }

    const bool encode_this_frame = frame_should_encode && is_recording_;
    perf.encode_active = encode_this_frame;

    try {
        if (frame.has_detection && perf.dropped) {
            dropped_frames_++;
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            if (encode_this_frame) {
                write_perf_row(frame, perf);
            }
            return false;
        }

        if (frame.has_detection) {
            const int CROP_W = crop_width_;
            const int CROP_H = crop_height_;
            const uint64_t zero_based_recording_frame =
                frame.recording_frame_id > 0 ? frame.recording_frame_id - 1 : 0;
            const NvEncInputFrame* encIn = nullptr;
            NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
            bool encode_prepared = false;

            if (active_crop_frame) {
                ck(cudaStreamWaitEvent(m_stream, active_crop_frame->crop_ready_event, 0));
            }
            
            if (encode_this_frame && active_crop_frame && external_crop_recording_enabled()) {
                encode_prepared = submit_external_crop_frame(frame, active_crop_frame->d_crop_mono, &perf);
                if (encode_prepared) {
                    encoded_frames_++;
                    const uint64_t metadata_start_ns = steady_now_ns();
                    write_metadata_row(frame);
                    perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
                }
            } else if (encode_this_frame && active_crop_frame) {
                encIn = encoder_->GetNextInputFrame();
                unsigned char* d_nv12_dst = static_cast<unsigned char*>(encIn->inputPtr);
                pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
                pic_params.inputTimeStamp = zero_based_recording_frame;
                pic_params.inputDuration = 1;

                ck(cudaMemcpy2DAsync(d_nv12_dst, encIn->pitch,
                                     active_crop_frame->d_crop_mono, CROP_W,
                                     CROP_W, CROP_H, cudaMemcpyDeviceToDevice, m_stream));
                
                unsigned char* d_uv_plane_dst = d_nv12_dst + encIn->pitch * CROP_H;
                ck(cudaMemset2DAsync(d_uv_plane_dst, encIn->pitch, 128, CROP_W, CROP_H / 2, m_stream));
                encode_prepared = true;
            }

            if (active_crop_frame && crop_producer_) {
                crop_producer_->RecycleAfterConsumerStream(active_crop_frame, m_stream);
                active_crop_frame = nullptr;
            }

            // --- RECORDING LOGIC (ONLY RUNS IF RECORDING IS ON) ---
            if (encode_prepared && !external_crop_recording_enabled()) {
                const uint64_t encode_start_ns = steady_now_ns();

                // Encode and write the frame to file
                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                encoder_->EncodeFrame(packets, &pic_params, nullptr, &output_timestamps);
                perf.packet_count = packets.size();
                perf.encoded_bytes = encoded_packet_bytes(packets);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);
                perf.encode_submit_cpu_ms = elapsed_ms(encode_start_ns, steady_now_ns());
                encoded_frames_++;

                // Write metadata to file
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        } else {
            // --- NO DETECTION ---
            // Only encode a blank frame if recording is active
            if (encode_this_frame && external_crop_recording_enabled()) {
                perf.blank_frame = true;
                frame.blank_frame = true;
                if (submit_external_crop_frame(frame, d_blank_frame_, &perf)) {
                    encoded_frames_++;
                    blank_frames_encoded_++;
                    const uint64_t metadata_start_ns = steady_now_ns();
                    write_metadata_row(frame);
                    perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
                }
            } else if (encode_this_frame) {
                perf.blank_frame = true;
                frame.blank_frame = true;
                const uint64_t encode_start_ns = steady_now_ns();
                const NvEncInputFrame* encIn = encoder_->GetNextInputFrame();
                const uint64_t zero_based_recording_frame =
                    frame.recording_frame_id > 0 ? frame.recording_frame_id - 1 : 0;
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
                perf.packet_count = packets.size();
                perf.encoded_bytes = encoded_packet_bytes(packets);
                push_encoded_packets(packets, output_timestamps, zero_based_recording_frame);
                perf.encode_submit_cpu_ms = elapsed_ms(encode_start_ns, steady_now_ns());
                encoded_frames_++;
                blank_frames_encoded_++;
                const uint64_t metadata_start_ns = steady_now_ns();
                write_metadata_row(frame);
                perf.metadata_cpu_ms = elapsed_ms(metadata_start_ns, steady_now_ns());
            }
        }

        if (crop_producer_) {
            crop_producer_->QueryCopyTiming(timing_crop_frame, &perf);
        }

        if (encode_this_frame) {
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }

    } catch (const std::exception& e) {
        std::cerr << "[CropAndEncodeWorker] Exception processing frame " << frame.local_frame_id
                  << ": " << e.what() << std::endl;
        if (encode_this_frame) {
            dropped_frames_++;
            perf.dropped = true;
            perf.drop_reason = "exception";
            perf.total_ms = elapsed_ms(perf.worker_start_steady_ns, steady_now_ns());
            write_perf_row(frame, perf);
        }
    }

    // Cleanup and recycle the entry
    if (active_crop_frame) {
        try {
            if (crop_producer_) {
                crop_producer_->RecycleAfterConsumerStream(active_crop_frame, m_stream);
            }
            active_crop_frame = nullptr;
        } catch (const std::exception& e) {
            std::cerr << "[CropAndEncodeWorker] Failed to defer crop frame recycle for frame "
                      << frame.local_frame_id
                      << ": " << e.what()
                      << "; synchronizing consumer stream before recycle." << std::endl;
            const uint64_t stream_sync_start_ns = steady_now_ns();
            cudaError_t status = cudaStreamSynchronize(m_stream);
            perf.stream_sync_ms += elapsed_ms(stream_sync_start_ns, steady_now_ns());
            if (status != cudaSuccess) {
                std::cerr << "[CropAndEncodeWorker] CropFrame fallback sync failed for frame "
                          << frame.local_frame_id
                          << ": " << cudaGetErrorString(status) << std::endl;
                cudaGetLastError();
            }
            if (crop_producer_) {
                crop_producer_->RecycleNow(active_crop_frame);
            }
            active_crop_frame = nullptr;
        }
    }
    if (crop_producer_) {
        crop_producer_->DrainPending(false);
    }

    return false; // This worker does not pass items to its own output queue
}
