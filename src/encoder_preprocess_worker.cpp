// src/encoder_preprocess_worker.cpp

#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include "nvtx_profiling.h"
#include "worker_entry_release.h"
#include <npp.h>
#include <nppi.h>
#include <nppi_color_conversion.h>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef PIPELINE_PROFILE
#if defined(YOLO_PROFILE) && YOLO_PROFILE
#define PIPELINE_PROFILE 1
#else
#define PIPELINE_PROFILE 0
#endif
#endif
#if PIPELINE_PROFILE
static constexpr int kEncProfileLogEvery = 60;
#endif
static constexpr size_t kHelperPreprocessHostHistoryLimit = 32;
static constexpr uint64_t kSourceReleaseProbeFrameMin = 90;
static constexpr uint64_t kSourceReleaseProbeFrameMax = 140;
static constexpr uint64_t kDetectPriorityWaitTimeoutNs = 50ULL * 1000ULL * 1000ULL;
static constexpr auto kDetectPriorityWaitPollInterval = std::chrono::microseconds(50);

namespace {
void check_npp_status(NppStatus status, const char* operation)
{
    if (status != NPP_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with NPP status " + std::to_string(status));
    }
}

#ifdef ENABLE_NVTX_PROFILING
std::string BuildRecordingNvtxLabel(const char* stage,
                                    const std::string& camera_serial,
                                    const WORKER_ENTRY* entry,
                                    int preprocess_gpu_id)
{
    std::ostringstream oss;
    oss << stage
        << " cam=" << (camera_serial.empty() ? "unknown" : camera_serial)
        << " preprocess_gpu=" << preprocess_gpu_id;
    if (entry) {
        oss << " rec_frame=" << entry->recording_frame_id
            << " local_frame=" << entry->frame_id
            << " source_gpu=" << entry->image_gpu_id
            << " route=" << (entry->recording_route_helper ? "helper" : "primary")
            << " helper_requested=" << (entry->recording_helper_requested ? 1 : 0)
            << " yolo_dispatched=" << (entry->yolo_dispatched ? 1 : 0)
            << " yolo_detach=" << (entry->yolo_input_detach_requested ? 1 : 0);
    }
    return oss.str();
}
#endif

int resolve_encoder_entry_pool_size(bool direct_input_enabled,
                                    int direct_input_slot_count,
                                    int configured_entry_pool_size,
                                    int default_entry_pool_size)
{
    if (direct_input_enabled) {
        return direct_input_slot_count;
    }

    const char* env = std::getenv("ORANGE_ENCODER_ENTRY_POOL_SIZE");
    if (!env || !*env) {
        if (configured_entry_pool_size <= 0) {
            return default_entry_pool_size;
        }
        if (configured_entry_pool_size > default_entry_pool_size) {
            std::cerr << "[EncoderPreprocessWorker] Configured encoder_entry_pool_size must be within [1,"
                      << default_entry_pool_size << "], using default "
                      << default_entry_pool_size << std::endl;
            return default_entry_pool_size;
        }
        return configured_entry_pool_size;
    }

    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end == env || *end != '\0') {
        std::cerr << "[EncoderPreprocessWorker] Ignoring invalid ORANGE_ENCODER_ENTRY_POOL_SIZE='"
                  << env << "', using default " << default_entry_pool_size << std::endl;
        return default_entry_pool_size;
    }
    if (parsed < 1 || parsed > default_entry_pool_size) {
        std::cerr << "[EncoderPreprocessWorker] ORANGE_ENCODER_ENTRY_POOL_SIZE must be within [1,"
                  << default_entry_pool_size << "], using default "
                  << default_entry_pool_size << std::endl;
        return default_entry_pool_size;
    }
    return static_cast<int>(parsed);
}

uint64_t helper_host_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool env_flag_enabled_by_default(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return true;
    }
    const char first = value[0];
    return !(first == '0' || first == 'n' || first == 'N' ||
             first == 'f' || first == 'F' ||
             first == 'o' || first == 'O');
}

bool env_flag_disabled_by_default(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return false;
    }
    const char first = value[0];
    return !(first == '0' || first == 'n' || first == 'N' ||
             first == 'f' || first == 'F' ||
             first == 'o' || first == 'O');
}

int64_t env_int64_or_default(const char* name, int64_t default_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }
    char* end = nullptr;
    const long long parsed = std::strtoll(value, &end, 10);
    if (end == value || parsed < default_value) {
        std::cerr << "[EncoderPreprocessWorker] Ignoring invalid " << name
                  << "=" << value << std::endl;
        return default_value;
    }
    return static_cast<int64_t>(parsed);
}
} // namespace

EncoderPreprocessWorker::EncoderPreprocessWorker(
    const char* name,
    CameraParams* cam_params,
    int preprocess_gpu_id,
    const RecordingOutputConfig& recording_output_config,
    bool direct_input_enabled,
    int encoder_pitch,
    int direct_input_slot_count,
    int configured_entry_pool_size,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    CameraControl* camera_control
)
    : CThreadWorker(name),
      camera_params_(cam_params),
      preprocess_gpu_id_(preprocess_gpu_id >= 0 ? preprocess_gpu_id : cam_params->gpu_id),
      m_recycle_queue_(recycle_queue),
      camera_control_(camera_control),
      m_stream(nullptr),
      m_hw_worker_(nullptr),
      d_input_staging_(nullptr),
      d_rgba_resize_(nullptr),
      d_uv_default_plane_(nullptr),
      direct_input_enabled_(direct_input_enabled),
      recording_output_config_(recording_output_config),
      encoder_pitch_(encoder_pitch),
      direct_input_pitch_(direct_input_enabled ? 0 : encoder_pitch),
      direct_input_slot_count_(direct_input_enabled ? direct_input_slot_count : 0),
      output_width_(recording_output_config_.resolved_width > 0 ? recording_output_config_.resolved_width : static_cast<int>(camera_params_->width)),
      output_height_(recording_output_config_.resolved_height > 0 ? recording_output_config_.resolved_height : static_cast<int>(camera_params_->height)),
      resize_source_size_{static_cast<int>(camera_params_->width), static_cast<int>(camera_params_->height)},
      resize_source_roi_{0, 0, static_cast<int>(camera_params_->width), static_cast<int>(camera_params_->height)},
      resize_output_size_{output_width_, output_height_},
      resize_output_roi_{0, 0, output_width_, output_height_},
      last_fps_update_time_(std::chrono::steady_clock::now())  // Initialize FPS timer
{
    defer_source_release_enabled_ =
        env_flag_enabled_by_default("ORANGE_PREPROCESS_DEFER_SOURCE_RELEASE");
    helper_noop_source_read_enabled_ =
        env_flag_disabled_by_default("ORANGE_PREPROCESS_HELPER_NOOP_SOURCE_READ");
    detect_priority_wait_enabled_ =
        env_flag_disabled_by_default("ORANGE_RECORDING_DETECT_PRIORITY");
    yolo_detach_input_enabled_ =
        env_flag_enabled_by_default("ORANGE_YOLO_DETACH_INPUT");
    helper_copy_limit_bytes_ =
        env_int64_or_default("ORANGE_PREPROCESS_HELPER_COPY_BYTES", -1);
    helper_copy_delay_ns_ =
        env_int64_or_default("ORANGE_PREPROCESS_HELPER_COPY_DELAY_NS", 0);
    ck(cudaSetDevice(preprocess_gpu_id_));
    ck(cudaStreamCreate(&m_stream));
    if (defer_source_release_enabled_) {
        std::cout << "[EncoderPreprocessWorker] Deferred source release enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << std::endl;
    }
    if (helper_noop_source_read_enabled_) {
        std::cout << "[EncoderPreprocessWorker] Helper source-read noop enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << std::endl;
    }
    if (detect_priority_wait_enabled_) {
        std::cout << "[EncoderPreprocessWorker] Recording detect-priority gate enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << " timeout_ns=" << kDetectPriorityWaitTimeoutNs
                  << std::endl;
    }
    if (yolo_detach_input_enabled_) {
        std::cout << "[EncoderPreprocessWorker] YOLO input detach gate enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << std::endl;
    }
    if (helper_copy_limit_bytes_ >= 0) {
        std::cout << "[EncoderPreprocessWorker] Helper peer-copy byte limit enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << " bytes=" << helper_copy_limit_bytes_
                  << std::endl;
    }
    if (helper_copy_delay_ns_ > 0) {
        std::cout << "[EncoderPreprocessWorker] Helper peer-copy delay enabled for camera "
                  << camera_params_->camera_serial
                  << " preprocess_gpu=" << preprocess_gpu_id_
                  << " delay_ns=" << helper_copy_delay_ns_
                  << std::endl;
    }

    if (direct_input_enabled_ && direct_input_slot_count_ <= 0) {
        throw std::runtime_error("Direct-input mode requires a positive slot count");
    }

    // Initialize GPU resources needed for color conversion
    frame_original_gpu_.d_orig = nullptr;
    frame_original_gpu_.size_pic = static_cast<int>(camera_params_->width * camera_params_->height * sizeof(unsigned char));
    initialize_gpu_debayer(&debayer_gpu_, camera_params_);

    if (direct_input_enabled_) {
        size_t direct_pitch = 0;
        direct_input_surfaces_.reserve(static_cast<size_t>(direct_input_slot_count_));
        for (int i = 0; i < direct_input_slot_count_; ++i) {
            void* surface = nullptr;
            size_t slot_pitch = 0;
            ck(cudaMallocPitch(&surface, &slot_pitch,
                               static_cast<size_t>(output_width_),
                               static_cast<size_t>(output_height_) * 3 / 2));
            if (i == 0) {
                direct_pitch = slot_pitch;
                direct_input_pitch_ = static_cast<int>(slot_pitch);
            } else if (slot_pitch != direct_pitch) {
                cudaFree(surface);
                throw std::runtime_error("Direct-input NV12 surface pitch mismatch across slot allocations");
            }
            direct_input_surfaces_.push_back(surface);
            free_direct_input_slots_.push(i);
        }
    }

    if (camera_params_->color) {
        if (recording_output_config_.resize_enabled) {
            ck(cudaMalloc(&d_rgba_resize_, static_cast<size_t>(output_width_) * output_height_ * 4));
        }
    } else {
        const int uv_pitch = direct_input_enabled_ ? direct_input_pitch_ : encoder_pitch_;
        size_t uv_plane_size = static_cast<size_t>(uv_pitch) * output_height_ / 2;
        ck(cudaMalloc(&d_uv_default_plane_, uv_plane_size));
        ck(cudaMemset(d_uv_default_plane_, 128, uv_plane_size));
    }

    const int entry_pool_size = resolve_encoder_entry_pool_size(
        direct_input_enabled_,
        direct_input_slot_count_,
        configured_entry_pool_size,
        DEFAULT_ENCODER_ENTRY_POOL_SIZE);
    if (!direct_input_enabled_ && entry_pool_size != DEFAULT_ENCODER_ENTRY_POOL_SIZE) {
        std::cout << "[EncoderPreprocessWorker] Using " << entry_pool_size
                  << " encoder entry slots instead of "
                  << DEFAULT_ENCODER_ENTRY_POOL_SIZE << std::endl;
    }
    encoder_entry_pool_.resize(static_cast<size_t>(entry_pool_size));

    // Create a pool of metadata entries that will be passed to the hardware encoder
    size_t prepared_frame_size = static_cast<size_t>(encoder_pitch_) * output_height_ * 3 / 2;
    for (int i = 0; i < entry_pool_size; ++i) {
        if (!direct_input_enabled_) {
            ck(cudaMalloc(&encoder_entry_pool_[i].d_prepared_frame, prepared_frame_size));
            encoder_entry_pool_[i].surface_pitch = static_cast<size_t>(encoder_pitch_);
        }
        encoder_entry_pool_[i].preprocess_complete_event = nullptr;
        free_encoder_entries_.push(&encoder_entry_pool_[i]);
    }
    available_buffers_.store(direct_input_enabled_ ? direct_input_slot_count_ : entry_pool_size, std::memory_order_relaxed);
    
    // --- Initialize the Event Pool ---
    event_pool_.resize(static_cast<size_t>(entry_pool_size));
    for (int i = 0; i < entry_pool_size; ++i) {
        ck(cudaEventCreateWithFlags(&event_pool_[i], cudaEventDisableTiming));
        free_events_.push(&event_pool_[i]);
    }
    available_events_.store(entry_pool_size, std::memory_order_relaxed);

    copy_start_event_pool_.resize(static_cast<size_t>(entry_pool_size));
    copy_end_event_pool_.resize(static_cast<size_t>(entry_pool_size));
    for (int i = 0; i < entry_pool_size; ++i) {
        ck(cudaEventCreate(&copy_start_event_pool_[i]));
        ck(cudaEventCreate(&copy_end_event_pool_[i]));
        encoder_entry_pool_[i].copy_start_event = copy_start_event_pool_[i];
        encoder_entry_pool_[i].copy_end_event = copy_end_event_pool_[i];
    }

    const int source_release_event_pool_size =
        std::max(entry_pool_size, DEFAULT_ENCODER_ENTRY_POOL_SIZE);
    source_release_event_pool_.resize(static_cast<size_t>(source_release_event_pool_size));
    for (int i = 0; i < source_release_event_pool_size; ++i) {
        ck(cudaEventCreateWithFlags(&source_release_event_pool_[i], cudaEventDisableTiming));
        free_source_release_events_.push(&source_release_event_pool_[i]);
    }
}


EncoderPreprocessWorker::~EncoderPreprocessWorker()
{
    ck(cudaSetDevice(preprocess_gpu_id_));
    drain_pending_source_releases(true);
    dump_helper_preprocess_host_history();
    dump_source_release_history();
    if (m_stream) cudaStreamDestroy(m_stream);

    // Free all buffers in the pool
    for (auto& encoder_entry : encoder_entry_pool_) {
        if (encoder_entry.d_prepared_frame && !direct_input_enabled_) {
            cudaFree(encoder_entry.d_prepared_frame);
        }
    }
    encoder_entry_pool_.clear();

    for (void* surface : direct_input_surfaces_) {
        if (surface) {
            cudaFree(surface);
        }
    }
    direct_input_surfaces_.clear();

    if (d_input_staging_) cudaFree(d_input_staging_);
    if (d_rgba_resize_) cudaFree(d_rgba_resize_);
    if (d_uv_default_plane_) cudaFree(d_uv_default_plane_);
    if (debayer_gpu_.d_debayer) cudaFree(debayer_gpu_.d_debayer);

    // --- Clean up the Event Pool ---
    for (auto& event : event_pool_) {
        if (event) cudaEventDestroy(event);
    }
    event_pool_.clear();

    for (auto& event : copy_start_event_pool_) {
        if (event) cudaEventDestroy(event);
    }
    copy_start_event_pool_.clear();

    for (auto& event : copy_end_event_pool_) {
        if (event) cudaEventDestroy(event);
    }
    copy_end_event_pool_.clear();

    for (auto& event : source_release_event_pool_) {
        if (event) cudaEventDestroy(event);
    }
    source_release_event_pool_.clear();
}

std::vector<PeerAccessRouteState> EncoderPreprocessWorker::peer_access_states() const
{
    std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
    std::vector<PeerAccessRouteState> states;
    states.reserve(peer_access_states_.size());
    for (const auto& [source_gpu_id, state] : peer_access_states_) {
        (void)source_gpu_id;
        states.push_back(state);
    }
    return states;
}

void EncoderPreprocessWorker::SetHwWorker(EncoderHwWorker* hw_worker)
{
    m_hw_worker_ = hw_worker;
}

double EncoderPreprocessWorker::get_hw_fps() const
{
    return m_hw_worker_ ? m_hw_worker_->get_fps() : 0.0;
}

uint64_t EncoderPreprocessWorker::get_hw_encode_failures() const
{
    return m_hw_worker_ ? m_hw_worker_->get_encode_failures() : 0;
}

uint64_t EncoderPreprocessWorker::get_hw_slow_frames() const
{
    return m_hw_worker_ ? m_hw_worker_->get_slow_frames() : 0;
}

int EncoderPreprocessWorker::get_hw_queue_depth() const
{
    return m_hw_worker_ ? m_hw_worker_->get_queue_depth() : -1;
}

void EncoderPreprocessWorker::append_helper_preprocess_host_sample(
    const HelperPreprocessHostSample& sample)
{
    std::lock_guard<std::mutex> lock(helper_preprocess_host_history_mutex_);
    if (helper_preprocess_host_history_.size() >= kHelperPreprocessHostHistoryLimit) {
        return;
    }
    helper_preprocess_host_history_.push_back(sample);
}

void EncoderPreprocessWorker::dump_helper_preprocess_host_history() const
{
    std::deque<HelperPreprocessHostSample> history_copy;
    {
        std::lock_guard<std::mutex> lock(helper_preprocess_host_history_mutex_);
        history_copy = helper_preprocess_host_history_;
    }

    if (history_copy.empty()) {
        return;
    }

    std::cerr << "[HELPER_PRE_HOST_DUMP] cam=" << camera_params_->camera_serial
              << " target_gpu=" << preprocess_gpu_id_
              << " sampled_helper_frames=" << history_copy.size()
              << " helper_frames_seen=" << helper_preprocess_host_seen_.load(std::memory_order_relaxed)
              << std::endl;

    for (size_t index = 0; index < history_copy.size(); ++index) {
        const auto& sample = history_copy[index];
        const double queue_wait_ms = sample.start_host_ns > sample.enqueue_host_ns
            ? static_cast<double>(sample.start_host_ns - sample.enqueue_host_ns) / 1e6
            : 0.0;
        const double worker_service_ms = sample.done_host_ns > sample.start_host_ns
            ? static_cast<double>(sample.done_host_ns - sample.start_host_ns) / 1e6
            : 0.0;
        const double total_ms = sample.done_host_ns > sample.enqueue_host_ns
            ? static_cast<double>(sample.done_host_ns - sample.enqueue_host_ns) / 1e6
            : 0.0;
        std::cerr << "  [HELPER_PRE_DUMP][" << index << "]"
                  << " recording_frame=" << sample.recording_frame_id
                  << " local_frame=" << sample.local_frame_id
                  << " source_gpu=" << sample.source_gpu_id
                  << " target_gpu=" << sample.target_gpu_id
                  << " gpu_direct=" << (sample.gpu_direct_mode ? 1 : 0)
                  << " direct_input=" << (sample.direct_input_enabled ? 1 : 0)
                  << " enqueue_q=" << sample.queue_depth_on_enqueue
                  << " start_q=" << sample.queue_depth_on_start
                  << " enqueue_free_buf=" << sample.available_buffers_on_enqueue
                  << " enqueue_free_evt=" << sample.available_events_on_enqueue
                  << " start_free_buf=" << sample.available_buffers_on_start
                  << " start_free_evt=" << sample.available_events_on_start
                  << " copy_bytes=" << sample.helper_copy_bytes
                  << " copy_delay_ns=" << sample.helper_copy_delay_ns
                  << " waits=" << sample.resource_waits_on_start
                  << " drops=" << sample.frames_dropped_on_start
                  << " queue_wait_ms=" << queue_wait_ms
                  << " worker_service_ms=" << worker_service_ms
                  << " total_ms=" << total_ms
                  << std::endl;
    }
}

void EncoderPreprocessWorker::append_source_release_sample(
    const SourceReleaseSample& sample)
{
    if (sample.recording_frame_id < kSourceReleaseProbeFrameMin ||
        sample.recording_frame_id > kSourceReleaseProbeFrameMax) {
        return;
    }

    std::lock_guard<std::mutex> lock(source_release_history_mutex_);
    source_release_history_.push_back(sample);
}

void EncoderPreprocessWorker::dump_source_release_history() const
{
    std::deque<SourceReleaseSample> history_copy;
    {
        std::lock_guard<std::mutex> lock(source_release_history_mutex_);
        history_copy = source_release_history_;
    }

    const uint64_t event_misses =
        source_release_event_misses_.load(std::memory_order_relaxed);
    const uint64_t fallback_stream_syncs =
        source_release_fallback_stream_syncs_.load(std::memory_order_relaxed);
    if (history_copy.empty() && event_misses == 0 && fallback_stream_syncs == 0) {
        return;
    }

    std::cerr << "[SOURCE_RELEASE_DUMP] cam=" << camera_params_->camera_serial
              << " target_gpu=" << preprocess_gpu_id_
              << " sampled_frames=" << history_copy.size()
              << " frame_window=" << kSourceReleaseProbeFrameMin
              << "-" << kSourceReleaseProbeFrameMax
              << " event_misses=" << event_misses
              << " fallback_stream_syncs=" << fallback_stream_syncs
              << std::endl;

    for (size_t index = 0; index < history_copy.size(); ++index) {
        const auto& sample = history_copy[index];
        const double receive_to_submit_ms =
            (sample.submit_host_ns > sample.receive_host_ns && sample.receive_host_ns > 0)
                ? static_cast<double>(sample.submit_host_ns - sample.receive_host_ns) / 1e6
                : 0.0;
        const double submit_to_worker_ms =
            (sample.worker_start_host_ns > sample.submit_host_ns && sample.submit_host_ns > 0)
                ? static_cast<double>(sample.worker_start_host_ns - sample.submit_host_ns) / 1e6
                : 0.0;
        const double worker_to_event_record_ms =
            (sample.source_safe_event_record_host_ns > sample.worker_start_host_ns &&
             sample.worker_start_host_ns > 0)
                ? static_cast<double>(
                      sample.source_safe_event_record_host_ns - sample.worker_start_host_ns) / 1e6
                : 0.0;
        const double event_record_to_release_ms =
            (sample.source_release_host_ns > sample.source_safe_event_record_host_ns &&
             sample.source_safe_event_record_host_ns > 0)
                ? static_cast<double>(
                      sample.source_release_host_ns - sample.source_safe_event_record_host_ns) / 1e6
                : 0.0;
        const double submit_to_release_ms =
            (sample.source_release_host_ns > sample.submit_host_ns && sample.submit_host_ns > 0)
                ? static_cast<double>(sample.source_release_host_ns - sample.submit_host_ns) / 1e6
                : 0.0;
        std::cerr << "  [SOURCE_RELEASE][" << index << "]"
                  << " recording_frame=" << sample.recording_frame_id
                  << " local_frame=" << sample.local_frame_id
                  << " source_gpu=" << sample.source_gpu_id
                  << " target_gpu=" << sample.target_gpu_id
                  << " gpu_direct=" << (sample.gpu_direct_mode ? 1 : 0)
                  << " cross_gpu=" << (sample.cross_gpu_copy ? 1 : 0)
                  << " direct_input=" << (sample.direct_input_enabled ? 1 : 0)
                  << " pending_on_record=" << sample.pending_releases_on_record
                  << " receive_to_submit_ms=" << receive_to_submit_ms
                  << " submit_to_worker_ms=" << submit_to_worker_ms
                  << " worker_to_event_record_ms=" << worker_to_event_record_ms
                  << " event_record_to_release_ms=" << event_record_to_release_ms
                  << " submit_to_release_ms=" << submit_to_release_ms
                  << std::endl;
    }
}

cudaEvent_t* EncoderPreprocessWorker::acquire_source_release_event()
{
    cudaEvent_t* event = nullptr;
    if (!defer_source_release_enabled_) {
        return nullptr;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        drain_pending_source_releases(false);
        if (free_source_release_events_.pop(event)) {
            return event;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    source_release_event_misses_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[SOURCE_RELEASE] No source-release CUDA event available for camera "
              << camera_params_->camera_serial
              << " target_gpu=" << preprocess_gpu_id_
              << "; falling back to stream-synchronized source release"
              << std::endl;
    return nullptr;
}

void EncoderPreprocessWorker::defer_source_release(
    WORKER_ENTRY* entry,
    cudaEvent_t* event,
    const SourceReleaseSample& sample)
{
    if (!entry || !event) {
        if (event) {
            free_source_release_events_.push(event);
        }
        release_source_entry(entry);
        return;
    }
    PendingSourceRelease pending;
    pending.entry = entry;
    pending.event = event;
    pending.sample = sample;
    pending.sample.pending_releases_on_record =
        static_cast<int>(pending_source_releases_.size());
    pending_source_releases_.push_back(pending);
    pending_source_release_count_.store(
        static_cast<int>(pending_source_releases_.size()),
        std::memory_order_relaxed);
}

void EncoderPreprocessWorker::drain_pending_source_releases(bool synchronize_all)
{
    for (auto it = pending_source_releases_.begin();
         it != pending_source_releases_.end(); ) {
        cudaError_t status = cudaSuccess;
        if (synchronize_all) {
            status = cudaEventSynchronize(*it->event);
        } else {
            status = cudaEventQuery(*it->event);
        }

        if (status == cudaErrorNotReady) {
            ++it;
            continue;
        }

        if (status != cudaSuccess) {
            std::cerr << "[SOURCE_RELEASE] CUDA event wait/query failed for camera "
                      << camera_params_->camera_serial
                      << " recording_frame=" << it->sample.recording_frame_id
                      << " error=" << cudaGetErrorString(status)
                      << std::endl;
            cudaGetLastError();
        }

        it->sample.source_release_host_ns = helper_host_now_ns();
        append_source_release_sample(it->sample);
        release_source_entry(it->entry);
        free_source_release_events_.push(it->event);
        it = pending_source_releases_.erase(it);
        pending_source_release_count_.store(
            static_cast<int>(pending_source_releases_.size()),
            std::memory_order_relaxed);
    }
}

void EncoderPreprocessWorker::release_source_after_stream_work(
    WORKER_ENTRY* entry,
    cudaEvent_t* event,
    bool event_recorded,
    const SourceReleaseSample& sample)
{
    if (event_recorded && event) {
        defer_source_release(entry, event, sample);
        return;
    }

    if (event) {
        free_source_release_events_.push(event);
    }

    if (m_stream) {
        source_release_fallback_stream_syncs_.fetch_add(1, std::memory_order_relaxed);
        const cudaError_t sync_status = cudaStreamSynchronize(m_stream);
        if (sync_status != cudaSuccess) {
            std::cerr << "[SOURCE_RELEASE] Fallback stream sync failed before source release"
                      << " camera=" << (camera_params_ ? camera_params_->camera_serial : "unknown")
                      << " recording_frame=" << sample.recording_frame_id
                      << " local_frame=" << sample.local_frame_id
                      << " preprocess_gpu=" << preprocess_gpu_id_
                      << " error=" << cudaGetErrorString(sync_status)
                      << std::endl;
            cudaGetLastError();
        }
        // The stream has surfaced completion or failure for queued work. Release
        // deterministically so a source frame cannot wedge the recycle pool.
    }

    SourceReleaseSample fallback_sample = sample;
    fallback_sample.source_release_host_ns = helper_host_now_ns();
    append_source_release_sample(fallback_sample);
    release_source_entry(entry);
}

void EncoderPreprocessWorker::release_source_entry(WORKER_ENTRY* entry)
{
    release_worker_entry_to_recycle(
        m_recycle_queue_,
        entry,
        WorkerEntryReleaseContext{
            camera_params_ ? camera_params_->camera_serial.c_str() : nullptr,
            threadName});
}

bool EncoderPreprocessWorker::IsDrained()
{
    return (in_flight_.load(std::memory_order_relaxed) == 0) &&
           (GetCountQueueInSize() == 0) &&
           (pending_source_release_count_.load(std::memory_order_relaxed) == 0);
}

void EncoderPreprocessWorker::PrepareCrossGpuInput(int source_gpu_id)
{
    if (source_gpu_id < 0 || source_gpu_id == preprocess_gpu_id_) {
        return;
    }

    ck(cudaSetDevice(preprocess_gpu_id_));
    if (!ensure_peer_access_enabled(source_gpu_id)) {
        throw std::runtime_error(
            "Cross-GPU preprocess prewarm requested from source GPU " +
            std::to_string(source_gpu_id) + " to preprocess GPU " +
            std::to_string(preprocess_gpu_id_) + ", but peer access is unavailable");
    }
    if (!d_input_staging_) {
        ck(cudaMalloc(&d_input_staging_, static_cast<size_t>(frame_original_gpu_.size_pic)));
    }

    cudaEvent_t prewarm_event = nullptr;
    ck(cudaEventCreateWithFlags(&prewarm_event, cudaEventDisableTiming));
    ck(cudaEventRecord(prewarm_event, m_stream));
    ck(cudaEventSynchronize(prewarm_event));
    ck(cudaEventDestroy(prewarm_event));

    std::cout << "[EncoderPreprocessWorker] Prewarmed cross-GPU input for camera "
              << camera_params_->camera_serial
              << " source_gpu=" << source_gpu_id
              << " preprocess_gpu=" << preprocess_gpu_id_
              << " staging_bytes=" << frame_original_gpu_.size_pic
              << std::endl;
}

bool EncoderPreprocessWorker::ensure_peer_access_enabled(int source_gpu_id)
{
    if (source_gpu_id < 0 || source_gpu_id == preprocess_gpu_id_) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
        PeerAccessRouteState& state = peer_access_states_[source_gpu_id];
        state.source_gpu_id = source_gpu_id;
        state.target_gpu_id = preprocess_gpu_id_;
        if (peer_access_enabled_gpus_.count(source_gpu_id) > 0) {
            state.can_access_peer = true;
            state.peer_access_enable_attempted = true;
            state.peer_access_enabled = true;
            state.enable_error.clear();
            return true;
        }
    }

    int can_access_peer = 0;
    ck(cudaDeviceCanAccessPeer(&can_access_peer, preprocess_gpu_id_, source_gpu_id));
    {
        std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
        PeerAccessRouteState& state = peer_access_states_[source_gpu_id];
        state.source_gpu_id = source_gpu_id;
        state.target_gpu_id = preprocess_gpu_id_;
        state.can_access_peer = (can_access_peer != 0);
        state.peer_access_enable_attempted = false;
        state.peer_access_enabled = false;
        state.enable_error.clear();
    }
    if (!can_access_peer) {
        return false;
    }

    ck(cudaSetDevice(preprocess_gpu_id_));
    {
        std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
        peer_access_states_[source_gpu_id].peer_access_enable_attempted = true;
    }
    cudaError_t enable_status = cudaDeviceEnablePeerAccess(source_gpu_id, 0);
    if (enable_status == cudaErrorPeerAccessAlreadyEnabled) {
        cudaGetLastError();
        std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
        PeerAccessRouteState& state = peer_access_states_[source_gpu_id];
        state.peer_access_enabled = true;
        state.enable_error.clear();
    } else if (enable_status != cudaSuccess) {
        {
            std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
            PeerAccessRouteState& state = peer_access_states_[source_gpu_id];
            state.peer_access_enabled = false;
            state.enable_error = cudaGetErrorString(enable_status);
        }
        throw std::runtime_error(
            "Failed to enable CUDA peer access from GPU " + std::to_string(preprocess_gpu_id_) +
            " to GPU " + std::to_string(source_gpu_id) + ": " +
            cudaGetErrorString(enable_status));
    }

    peer_access_enabled_gpus_.insert(source_gpu_id);
    {
        std::lock_guard<std::mutex> lock(peer_access_states_mutex_);
        PeerAccessRouteState& state = peer_access_states_[source_gpu_id];
        state.peer_access_enabled = true;
        state.enable_error.clear();
    }
    return true;
}

void EncoderPreprocessWorker::record_detect_priority_wait(uint64_t wait_ns, bool timeout)
{
    detect_priority_waited_frames_.fetch_add(1, std::memory_order_relaxed);
    detect_priority_wait_total_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
    uint64_t observed_max = detect_priority_wait_max_ns_.load(std::memory_order_relaxed);
    while (wait_ns > observed_max &&
           !detect_priority_wait_max_ns_.compare_exchange_weak(
               observed_max,
               wait_ns,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    if (timeout) {
        detect_priority_wait_timeouts_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool EncoderPreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    auto start_time = std::chrono::steady_clock::now();

    if (!pending_source_releases_.empty()) {
        ck(cudaSetDevice(preprocess_gpu_id_));
        drain_pending_source_releases(false);
    }

    if (!entry) {
        if (camera_control_ &&
            camera_control_->recording_draining &&
            pending_source_release_count_.load(std::memory_order_relaxed) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            (void)PutObjectToQueueIn(nullptr);
        }
        return false;
    }

    const uint64_t worker_start_host_ns = helper_host_now_ns();

    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;

    // If we're not recording and not draining, just recycle and move on.
    if (!recording_enabled && !draining) {
        release_source_entry(entry);
        return false;
    }

    in_flight_.fetch_add(1, std::memory_order_relaxed);
    struct InFlightGuard {
        std::atomic<int>* in_flight = nullptr;

        ~InFlightGuard()
        {
            if (in_flight) {
                in_flight->fetch_sub(1, std::memory_order_relaxed);
            }
        }
    } in_flight_guard{&in_flight_};

    // Own the WORKER_ENTRY source ref until it has either been released
    // immediately or protected by the event/sync-backed stream-release path.
    struct SourceEntryGuard {
        EncoderPreprocessWorker* owner = nullptr;
        WORKER_ENTRY* entry = nullptr;
        cudaEvent_t** event = nullptr;
        bool* event_recorded = nullptr;
        SourceReleaseSample* sample = nullptr;
        bool use_stream_release = false;

        ~SourceEntryGuard()
        {
            if (!owner || !entry) {
                return;
            }
            if (use_stream_release) {
                SourceReleaseSample empty_sample;
                owner->release_source_after_stream_work(
                    entry,
                    event ? *event : nullptr,
                    event_recorded ? *event_recorded : false,
                    sample ? *sample : empty_sample);
                if (event) {
                    *event = nullptr;
                }
            } else {
                owner->release_source_entry(entry);
            }
        }

        void UseStreamRelease(cudaEvent_t** event_in,
                              bool* event_recorded_in,
                              SourceReleaseSample* sample_in)
        {
            event = event_in;
            event_recorded = event_recorded_in;
            sample = sample_in;
            use_stream_release = true;
        }

        void ReleaseAfterStreamWork()
        {
            if (!owner || !entry) {
                return;
            }
            SourceReleaseSample empty_sample;
            owner->release_source_after_stream_work(
                entry,
                event ? *event : nullptr,
                event_recorded ? *event_recorded : false,
                sample ? *sample : empty_sample);
            if (event) {
                *event = nullptr;
            }
            entry = nullptr;
        }
    } source_entry_guard{this, entry};

    NVTX_ENCODE_DYNAMIC(BuildRecordingNvtxLabel(
        "Recording preprocess worker",
        camera_params_->camera_serial,
        entry,
        preprocess_gpu_id_));

    // Track successful frame processing
    frame_counter_.fetch_add(1, std::memory_order_relaxed);
    
    // FPS calculation and logging every second
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        const int frames = frame_counter_.exchange(0, std::memory_order_relaxed);
        current_fps_.store(frames / elapsed.count(), std::memory_order_relaxed);
        last_fps_update_time_ = now;
    }

    ck(cudaSetDevice(preprocess_gpu_id_));
    EnsureNppStream(m_stream);

    if (helper_noop_source_read_enabled_ &&
        entry->image_gpu_id >= 0 &&
        entry->image_gpu_id != preprocess_gpu_id_) {
        const uint64_t helper_index =
            helper_preprocess_host_seen_.fetch_add(1, std::memory_order_relaxed);
        if (helper_index < kHelperPreprocessHostHistoryLimit) {
            HelperPreprocessHostSample helper_host_sample;
            helper_host_sample.recording_frame_id = entry->recording_frame_id;
            helper_host_sample.local_frame_id = entry->frame_id;
            helper_host_sample.source_gpu_id = entry->image_gpu_id;
            helper_host_sample.target_gpu_id = preprocess_gpu_id_;
            helper_host_sample.gpu_direct_mode = entry->gpu_direct_mode;
            helper_host_sample.direct_input_enabled = direct_input_enabled_;
            helper_host_sample.enqueue_host_ns = entry->helper_enqueue_host_ns;
            helper_host_sample.start_host_ns = worker_start_host_ns;
            helper_host_sample.done_host_ns = helper_host_now_ns();
            helper_host_sample.queue_depth_on_enqueue = entry->helper_enqueue_queue_depth;
            helper_host_sample.queue_depth_on_start = GetCountQueueInSize();
            helper_host_sample.available_buffers_on_enqueue =
                entry->helper_enqueue_available_buffers;
            helper_host_sample.available_events_on_enqueue =
                entry->helper_enqueue_available_events;
            helper_host_sample.available_buffers_on_start =
                available_buffers_.load(std::memory_order_relaxed);
            helper_host_sample.available_events_on_start =
                available_events_.load(std::memory_order_relaxed);
            helper_host_sample.helper_copy_bytes = 0;
            helper_host_sample.helper_copy_delay_ns = 0;
            helper_host_sample.resource_waits_on_start =
                resource_waits_.load(std::memory_order_relaxed);
            helper_host_sample.frames_dropped_on_start =
                frames_dropped_.load(std::memory_order_relaxed);
            append_helper_preprocess_host_sample(helper_host_sample);
        }
        if (!entry->gpu_direct_mode) {
            cudaEvent_t* source_ready_event = entry->delayed_consumer_event();
            cudaError_t sync_status =
                source_ready_event ? cudaEventSynchronize(*source_ready_event) : cudaSuccess;
            if (sync_status != cudaSuccess) {
                std::cerr << "[EncoderPreprocessWorker] Helper source-read noop event sync "
                          << "failed for camera " << camera_params_->camera_serial
                          << " target_gpu=" << preprocess_gpu_id_
                          << " recording_frame=" << entry->recording_frame_id
                          << " error=" << cudaGetErrorString(sync_status)
                          << std::endl;
                cudaGetLastError();
            }
        }
        return false;
    }

    if (detect_priority_wait_enabled_ &&
        entry->yolo_dispatched &&
        !entry->recording_route_helper &&
        entry->image_gpu_id == preprocess_gpu_id_) {
        NVTX_SYNC_DYNAMIC(BuildRecordingNvtxLabel(
            "Recording detect_priority wait",
            camera_params_->camera_serial,
            entry,
            preprocess_gpu_id_));
        detect_priority_gated_frames_.fetch_add(1, std::memory_order_relaxed);
        const bool wait_for_yolo_input =
            yolo_detach_input_enabled_ &&
            entry->yolo_input_detach_requested &&
            entry->yolo_input_ready_event;
        const auto wait_start = std::chrono::steady_clock::now();
        bool waited = false;
        bool timed_out = false;
        auto wait_target_ready = [&]() {
            if (!wait_for_yolo_input) {
                return entry->detections_ready.load(std::memory_order_acquire);
            }
            if (!entry->yolo_input_ready_event_recorded.load(std::memory_order_acquire)) {
                return false;
            }
            cudaError_t status = cudaEventQuery(entry->yolo_input_ready_event);
            if (status == cudaSuccess) {
                return true;
            }
            if (status == cudaErrorNotReady) {
                return false;
            }
            std::cerr << "[EncoderPreprocessWorker] YOLO input-ready event query failed for camera "
                      << camera_params_->camera_serial
                      << " preprocess_gpu=" << preprocess_gpu_id_
                      << " recording_frame=" << entry->recording_frame_id
                      << " error=" << cudaGetErrorString(status)
                      << std::endl;
            cudaGetLastError();
            return true;
        };
        while (!wait_target_ready()) {
            waited = true;
            const uint64_t elapsed_ns =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
            if (elapsed_ns >= kDetectPriorityWaitTimeoutNs) {
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(kDetectPriorityWaitPollInterval);
        }
        if (waited) {
            const uint64_t wait_ns =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
            record_detect_priority_wait(wait_ns, timed_out);
            if (timed_out &&
                !detect_priority_timeout_logged_.exchange(true, std::memory_order_relaxed)) {
                std::cerr << "[EncoderPreprocessWorker] Recording detect-priority wait timed out for camera "
                          << camera_params_->camera_serial
                          << " preprocess_gpu=" << preprocess_gpu_id_
                          << " recording_frame=" << entry->recording_frame_id
                          << " target=" << (wait_for_yolo_input ? "yolo_input_ready" : "detections_ready")
                          << " wait_ns=" << wait_ns
                          << std::endl;
            }
        }
    }

#if PIPELINE_PROFILE
    struct EncProfileEvents {
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

        ~EncProfileEvents() {
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

    static thread_local EncProfileEvents enc_prof_events;
    static thread_local bool enc_prof_inflight = false;
    static thread_local int enc_prof_count = 0;
    enc_prof_events.Init(preprocess_gpu_id_);
    if (enc_prof_inflight) {
        cudaError_t enc_status = cudaEventQuery(enc_prof_events.end);
        if (enc_status == cudaSuccess) {
            float enc_ms = 0.0f;
            ck(cudaEventElapsedTime(&enc_ms, enc_prof_events.start, enc_prof_events.end));
            std::cout << "[ENC_PRE_TIME] Cam " << camera_params_->camera_serial
                      << " GPU " << preprocess_gpu_id_
                      << " ms=" << enc_ms
                      << " q=" << GetCountQueueInSize()
                      << std::endl;
            enc_prof_inflight = false;
        } else if (enc_status != cudaErrorNotReady) {
            std::cerr << "[ENC_PRE_TIME] Cam " << camera_params_->camera_serial
                      << " event query failed: " << cudaGetErrorString(enc_status)
                      << std::endl;
            enc_prof_inflight = false;
        }
    }
#endif

    // Acquire resources for the next stage with retry logic
    ENCODER_WORKER_ENTRY* encoder_entry = nullptr;
    cudaEvent_t* event = nullptr;
    int direct_input_slot_id = -1;
    struct EncoderResourceGuard {
        EncoderPreprocessWorker* owner = nullptr;
        ENCODER_WORKER_ENTRY** entry = nullptr;
        cudaEvent_t** event = nullptr;
        int* direct_input_slot_id = nullptr;
        uint64_t frame_id = 0;
        uint64_t recording_frame_id = 0;
        bool buffer_counter_borrowed = false;
        bool event_counter_borrowed = false;
        bool stream_work_queued = false;
        bool active = true;

        ~EncoderResourceGuard()
        {
            ReturnToPools();
        }

        void MarkBufferCounterBorrowed()
        {
            buffer_counter_borrowed = true;
        }

        void MarkEventCounterBorrowed()
        {
            event_counter_borrowed = true;
        }

        void MarkStreamWorkQueued()
        {
            stream_work_queued = true;
        }

        void TransferToConsumer()
        {
            active = false;
            if (entry) {
                *entry = nullptr;
            }
            if (event) {
                *event = nullptr;
            }
            if (direct_input_slot_id) {
                *direct_input_slot_id = -1;
            }
        }

        void ReturnToPools()
        {
            if (!active || !owner) {
                return;
            }

            if (stream_work_queued && owner->m_stream) {
                // If queued preprocess work may still touch the encoder buffer,
                // synchronize before returning it to the free pool.
                const cudaError_t sync_status = cudaStreamSynchronize(owner->m_stream);
                if (sync_status != cudaSuccess) {
                    std::cerr << "[EncoderPreprocessWorker] Preprocess stream sync failed before "
                              << "returning encoder resources"
                              << " camera=" << (owner->camera_params_
                                                   ? owner->camera_params_->camera_serial
                                                   : "unknown")
                              << " recording_frame=" << recording_frame_id
                              << " local_frame=" << frame_id
                              << " preprocess_gpu=" << owner->preprocess_gpu_id_
                              << " error=" << cudaGetErrorString(sync_status)
                              << std::endl;
                    cudaGetLastError();
                }
            }

            if (entry && *entry) {
                if ((*entry)->direct_input) {
                    (*entry)->d_prepared_frame = nullptr;
                }
                (*entry)->slot_id = -1;
                (*entry)->direct_input = false;
                (*entry)->preprocess_complete_event = nullptr;
                (*entry)->cross_gpu_copy_performed = false;
                owner->free_encoder_entries_.push(*entry);
                *entry = nullptr;
            }
            if (event && *event) {
                owner->free_events_.push(*event);
                *event = nullptr;
            }
            if (direct_input_slot_id && *direct_input_slot_id >= 0) {
                owner->free_direct_input_slots_.push(*direct_input_slot_id);
                *direct_input_slot_id = -1;
            }
            if (buffer_counter_borrowed) {
                owner->available_buffers_.fetch_add(1, std::memory_order_relaxed);
                buffer_counter_borrowed = false;
            }
            if (event_counter_borrowed) {
                owner->available_events_.fetch_add(1, std::memory_order_relaxed);
                event_counter_borrowed = false;
            }
            active = false;
        }
    } encoder_resource_guard{
        this,
        &encoder_entry,
        &event,
        &direct_input_slot_id,
        entry->frame_id,
        entry->recording_frame_id};
    
    int retry_count = 0;
    while (((!free_encoder_entries_.pop(encoder_entry)) ||
            (!free_events_.pop(event)) ||
            (direct_input_enabled_ && !free_direct_input_slots_.pop(direct_input_slot_id))) &&
           retry_count < 3) {
        if (encoder_entry) {
            free_encoder_entries_.push(encoder_entry);
            encoder_entry = nullptr;
        }
        if (event) {
            free_events_.push(event);
            event = nullptr;
        }
        if (direct_input_enabled_ && direct_input_slot_id >= 0) {
            free_direct_input_slots_.push(direct_input_slot_id);
            direct_input_slot_id = -1;
        }
        resource_waits_++;
        retry_count++;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    if (!encoder_entry || !event || (direct_input_enabled_ && direct_input_slot_id < 0)) {
        frames_dropped_++;
        return false;
    }
    
    // Successfully acquired resources - update counters
    if (direct_input_enabled_ || encoder_entry->d_prepared_frame) {
        available_buffers_--;
        encoder_resource_guard.MarkBufferCounterBorrowed();
    }
    available_events_--;
    encoder_resource_guard.MarkEventCounterBorrowed();
    encoder_entry->preprocess_complete_event = event;
    encoder_entry->slot_id = direct_input_slot_id;
    encoder_entry->direct_input = direct_input_enabled_;
    encoder_entry->surface_pitch = static_cast<size_t>(direct_input_enabled_ ? direct_input_pitch_ : encoder_pitch_);
    encoder_entry->surface_gpu_id = preprocess_gpu_id_;
    encoder_entry->cross_gpu_copy_performed = false;
    if (direct_input_enabled_) {
        encoder_entry->d_prepared_frame = static_cast<unsigned char*>(direct_input_surfaces_[static_cast<size_t>(direct_input_slot_id)]);
    }

    cudaEvent_t* source_release_event = acquire_source_release_event();
    bool source_release_event_recorded = false;
    uint64_t source_release_event_record_host_ns = 0;
    bool cross_gpu_copy_performed = false;
    SourceReleaseSample source_release_sample;
    source_release_sample.recording_frame_id = entry->recording_frame_id;
    source_release_sample.local_frame_id = entry->frame_id;
    source_release_sample.source_gpu_id = entry->image_gpu_id;
    source_release_sample.target_gpu_id = preprocess_gpu_id_;
    source_release_sample.gpu_direct_mode = entry->gpu_direct_mode;
    source_release_sample.direct_input_enabled = direct_input_enabled_;
    source_release_sample.receive_host_ns = entry->acquisition_receive_host_ns;
    source_release_sample.submit_host_ns = entry->recording_submit_host_ns;
    source_release_sample.worker_start_host_ns = worker_start_host_ns;
    source_entry_guard.UseStreamRelease(
        &source_release_event,
        &source_release_event_recorded,
        &source_release_sample);

#if PIPELINE_PROFILE
    bool sample_preprocess = false;
    if (!enc_prof_inflight) {
        enc_prof_count++;
        if (enc_prof_count % kEncProfileLogEvery == 0) {
            sample_preprocess = true;
        ck(cudaEventRecord(enc_prof_events.start, m_stream));
        }
    }
#endif

    // Wait for the raw frame data to be ready from the acquisition thread.
    encoder_resource_guard.MarkStreamWorkQueued();
    cudaEvent_t* source_ready_event = entry->delayed_consumer_event();
    if (source_ready_event) {
        NVTX_SYNC_DYNAMIC(BuildRecordingNvtxLabel(
            "Recording source_ready stream_wait",
            camera_params_->camera_serial,
            entry,
            preprocess_gpu_id_));
        ck(cudaStreamWaitEvent(m_stream, *source_ready_event, 0));
    }

    unsigned char* input_source = entry->delayed_consumer_image();
    bool sample_helper_cross_gpu = false;
    HelperPreprocessHostSample helper_host_sample;
    if (entry->image_gpu_id >= 0 && entry->image_gpu_id != preprocess_gpu_id_) {
        NVTX_GPU_COPY_DYNAMIC(BuildRecordingNvtxLabel(
            "Recording helper peer_copy",
            camera_params_->camera_serial,
            entry,
            preprocess_gpu_id_));
        if (!ensure_peer_access_enabled(entry->image_gpu_id)) {
            throw std::runtime_error(
                "Cross-GPU preprocess requested from source GPU " +
                std::to_string(entry->image_gpu_id) + " to preprocess GPU " +
                std::to_string(preprocess_gpu_id_) + ", but peer access is unavailable");
        }
        if (!d_input_staging_) {
            ck(cudaMalloc(&d_input_staging_, static_cast<size_t>(frame_original_gpu_.size_pic)));
        }
        const size_t full_copy_bytes = static_cast<size_t>(frame_original_gpu_.size_pic);
        const size_t helper_copy_bytes =
            helper_copy_limit_bytes_ >= 0
                ? std::min(full_copy_bytes, static_cast<size_t>(helper_copy_limit_bytes_))
                : full_copy_bytes;
        ck(cudaEventRecord(encoder_entry->copy_start_event, m_stream));
        const uint64_t helper_index =
            helper_preprocess_host_seen_.fetch_add(1, std::memory_order_relaxed);
        if (helper_index < kHelperPreprocessHostHistoryLimit) {
            sample_helper_cross_gpu = true;
            helper_host_sample.recording_frame_id = entry->recording_frame_id;
            helper_host_sample.local_frame_id = entry->frame_id;
            helper_host_sample.source_gpu_id = entry->image_gpu_id;
            helper_host_sample.target_gpu_id = preprocess_gpu_id_;
            helper_host_sample.gpu_direct_mode = entry->gpu_direct_mode;
            helper_host_sample.direct_input_enabled = direct_input_enabled_;
            helper_host_sample.enqueue_host_ns = entry->helper_enqueue_host_ns;
            helper_host_sample.start_host_ns = helper_host_now_ns();
            helper_host_sample.queue_depth_on_enqueue = entry->helper_enqueue_queue_depth;
            helper_host_sample.queue_depth_on_start = GetCountQueueInSize();
            helper_host_sample.available_buffers_on_enqueue =
                entry->helper_enqueue_available_buffers;
            helper_host_sample.available_events_on_enqueue =
                entry->helper_enqueue_available_events;
            helper_host_sample.available_buffers_on_start =
                available_buffers_.load(std::memory_order_relaxed);
            helper_host_sample.available_events_on_start =
                available_events_.load(std::memory_order_relaxed);
            helper_host_sample.helper_copy_bytes = helper_copy_bytes;
            helper_host_sample.helper_copy_delay_ns = helper_copy_delay_ns_;
            helper_host_sample.resource_waits_on_start =
                resource_waits_.load(std::memory_order_relaxed);
            helper_host_sample.frames_dropped_on_start =
                frames_dropped_.load(std::memory_order_relaxed);
        }
        if (helper_copy_delay_ns_ > 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(helper_copy_delay_ns_));
        }
        if (helper_copy_bytes > 0) {
            ck(cudaMemcpyPeerAsync(
                d_input_staging_,
                preprocess_gpu_id_,
                entry->d_image,
                entry->image_gpu_id,
                helper_copy_bytes,
                m_stream));
        }
        ck(cudaEventRecord(encoder_entry->copy_end_event, m_stream));
        if (source_release_event && !source_release_event_recorded) {
            ck(cudaEventRecord(*source_release_event, m_stream));
            source_release_event_recorded = true;
            source_release_event_record_host_ns = helper_host_now_ns();
            source_release_sample.source_safe_event_record_host_ns =
                source_release_event_record_host_ns;
        }
        encoder_entry->cross_gpu_copy_performed = helper_copy_bytes > 0;
        cross_gpu_copy_performed = helper_copy_bytes > 0;
        input_source = d_input_staging_;
    }

    // --- Perform the copy and color conversion ---
    {
    NVTX_ENCODE_DYNAMIC(BuildRecordingNvtxLabel(
        "Recording source_gpu preprocess",
        camera_params_->camera_serial,
        entry,
        preprocess_gpu_id_));
    if (camera_params_->color) {
        // Full Color Pipeline: RAW -> RGBA -> NV12
        frame_original_gpu_.d_orig = input_source;
        
        // 1. Debayer RAW Bayer to RGBA
        debayer_frame_gpu(camera_params_, &frame_original_gpu_, &debayer_gpu_);

        unsigned char* rgba_source = debayer_gpu_.d_debayer;
        if (recording_output_config_.resize_enabled) {
            check_npp_status(
                nppiResize_8u_C4R(
                    debayer_gpu_.d_debayer,
                    static_cast<int>(camera_params_->width) * 4,
                    resize_source_size_,
                    resize_source_roi_,
                    d_rgba_resize_,
                    output_width_ * 4,
                    resize_output_size_,
                    resize_output_roi_,
                    NPPI_INTER_SUPER),
                "nppiResize_8u_C4R");
            rgba_source = d_rgba_resize_;
        }

        // 2. Convert RGBA directly to NV12 for the encoder.
        launch_rgba_to_nv12_kernel(
            rgba_source,
            encoder_entry->d_prepared_frame,
            output_width_,
            output_height_,
            static_cast<int>(encoder_entry->surface_pitch),
            m_stream);

    } else {
        // Monochrome path: Copy Y plane and fill UV planes to create an NV12-compatible frame
        unsigned char* d_y_plane_dst = encoder_entry->d_prepared_frame;
        unsigned char* d_uv_plane_dst = d_y_plane_dst + (encoder_entry->surface_pitch * output_height_);

        if (recording_output_config_.resize_enabled) {
            check_npp_status(
                nppiResize_8u_C1R(
                    input_source,
                    static_cast<int>(camera_params_->width),
                    resize_source_size_,
                    resize_source_roi_,
                    d_y_plane_dst,
                    static_cast<int>(encoder_entry->surface_pitch),
                    resize_output_size_,
                    resize_output_roi_,
                    NPPI_INTER_SUPER),
                "nppiResize_8u_C1R");
        } else {
            ck(cudaMemcpy2DAsync(
                d_y_plane_dst,
                encoder_entry->surface_pitch,
                input_source,
                camera_params_->width,
                output_width_,
                output_height_,
                cudaMemcpyDeviceToDevice,
                m_stream));
        }

        size_t uv_plane_size = encoder_entry->surface_pitch * output_height_ / 2;
        ck(cudaMemcpyAsync(d_uv_plane_dst, d_uv_default_plane_, uv_plane_size, cudaMemcpyDeviceToDevice, m_stream));
    }
    }
    
    // Record an event in the stream once all the above GPU work is queued.
    {
    NVTX_SYNC_DYNAMIC(BuildRecordingNvtxLabel(
        "Recording preprocess_complete event record",
        camera_params_->camera_serial,
        entry,
        preprocess_gpu_id_));
    ck(cudaEventRecord(*encoder_entry->preprocess_complete_event, m_stream));
    if (source_release_event && !source_release_event_recorded) {
        ck(cudaEventRecord(*source_release_event, m_stream));
        source_release_event_recorded = true;
        source_release_event_record_host_ns = helper_host_now_ns();
        source_release_sample.source_safe_event_record_host_ns =
            source_release_event_record_host_ns;
    }
    }
    if (sample_helper_cross_gpu) {
        helper_host_sample.done_host_ns = helper_host_now_ns();
        append_helper_preprocess_host_sample(helper_host_sample);
    }
#if PIPELINE_PROFILE
    if (sample_preprocess) {
        ck(cudaEventRecord(enc_prof_events.end, m_stream));
        enc_prof_inflight = true;
    }
#endif

    if (m_hw_worker_) {
        encoder_entry->recording_frame_id = entry->recording_frame_id;
        encoder_entry->timestamp = entry->timestamp;
        encoder_entry->timestamp_sys = entry->timestamp_sys;
    }

    source_release_sample.cross_gpu_copy = cross_gpu_copy_performed;
    source_release_sample.source_safe_event_record_host_ns =
        source_release_event_record_host_ns;
    const uint64_t source_frame_id = entry->frame_id;
    const uint64_t source_recording_frame_id = entry->recording_frame_id;
    source_entry_guard.ReleaseAfterStreamWork();

    // Pass the prepared frame and its completion event to the hardware encoder.
    if (m_hw_worker_ && m_hw_worker_->PutObjectToQueueIn(encoder_entry)) {
        encoder_resource_guard.TransferToConsumer();
    } else {
        if (m_hw_worker_) {
            std::cerr << "[EncoderPreprocessWorker] Hardware worker enqueue rejected"
                      << " cam=" << camera_params_->camera_serial
                      << " frame=" << source_frame_id
                      << " recording_frame=" << source_recording_frame_id
                      << std::endl;
        }
    }

    // Measure total preprocessing time
    auto preprocess_end = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(preprocess_end - start_time).count();
    
    // Log slow frames (> 12.5ms for 80fps target)
    // if (duration_us > 12500) {
    //     std::cout << "[PERF WARNING] " << threadName 
    //               << " Camera " << camera_params_->camera_serial
    //               << " slow frame: " << duration_us << "μs"
    //               << " (target: <12500μs for 80fps)" << std::endl;
    // }

    return false; // This worker never passes items to its own output queue.
}
