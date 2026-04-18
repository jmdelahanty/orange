// src/encoder_preprocess_worker.cpp

#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include <npp.h>
#include <nppi.h>
#include <nppi_color_conversion.h>
#include <cstdlib>
#include <stdexcept>

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
static constexpr size_t kHelperPreprocessHistoryLimit = 128;
static constexpr float kHelperPreprocessSlowCopyMs = 2.0f;
static constexpr float kHelperPreprocessSlowTotalMs = 8.0f;

namespace {
void check_npp_status(NppStatus status, const char* operation)
{
    if (status != NPP_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with NPP status " + std::to_string(status));
    }
}

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

struct HelperPreprocessProfileEvents {
    cudaEvent_t copy_start{};
    cudaEvent_t copy_end{};
    cudaEvent_t total_end{};
    int device = -1;
    bool initialized = false;
    bool inflight = false;
    HelperPreprocessSample sample;

    void Init(int gpu_id) {
        if (initialized) {
            return;
        }
        device = gpu_id;
        ck(cudaSetDevice(device));
        ck(cudaEventCreate(&copy_start));
        ck(cudaEventCreate(&copy_end));
        ck(cudaEventCreate(&total_end));
        initialized = true;
    }

    ~HelperPreprocessProfileEvents() {
        if (!initialized) {
            return;
        }
        if (device >= 0) {
            cudaSetDevice(device);
        }
        cudaEventDestroy(copy_start);
        cudaEventDestroy(copy_end);
        cudaEventDestroy(total_end);
    }
};
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
    ck(cudaSetDevice(preprocess_gpu_id_));
    ck(cudaStreamCreate(&m_stream));

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
}


EncoderPreprocessWorker::~EncoderPreprocessWorker()
{
    ck(cudaSetDevice(preprocess_gpu_id_));
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

void EncoderPreprocessWorker::append_helper_preprocess_sample(
    const HelperPreprocessSample& sample)
{
    bool should_dump = false;
    {
        std::lock_guard<std::mutex> lock(helper_preprocess_history_mutex_);
        helper_preprocess_history_.push_back(sample);
        if (helper_preprocess_history_.size() > kHelperPreprocessHistoryLimit) {
            helper_preprocess_history_.pop_front();
        }
        if (!helper_preprocess_history_dumped_.load(std::memory_order_relaxed) &&
            (sample.copy_ms >= kHelperPreprocessSlowCopyMs ||
             sample.total_preprocess_ms >= kHelperPreprocessSlowTotalMs)) {
            helper_preprocess_history_dumped_.store(true, std::memory_order_relaxed);
            should_dump = true;
        }
    }

    if (should_dump) {
        dump_helper_preprocess_history(sample);
    }
}

void EncoderPreprocessWorker::dump_helper_preprocess_history(
    const HelperPreprocessSample& trigger_sample) const
{
    std::deque<HelperPreprocessSample> history_copy;
    {
        std::lock_guard<std::mutex> lock(helper_preprocess_history_mutex_);
        history_copy = helper_preprocess_history_;
    }

    std::cerr << "[HELPER_PRE_DUMP] cam=" << camera_params_->camera_serial
              << " source_gpu=" << trigger_sample.source_gpu_id
              << " target_gpu=" << trigger_sample.target_gpu_id
              << " trigger_recording_frame=" << trigger_sample.recording_frame_id
              << " trigger_local_frame=" << trigger_sample.local_frame_id
              << " trigger_copy_ms=" << trigger_sample.copy_ms
              << " trigger_total_preprocess_ms=" << trigger_sample.total_preprocess_ms
              << " history_entries=" << history_copy.size()
              << std::endl;

    for (size_t index = 0; index < history_copy.size(); ++index) {
        const auto& sample = history_copy[index];
        std::cerr << "  [HELPER_PRE_DUMP][" << index << "]"
                  << " recording_frame=" << sample.recording_frame_id
                  << " local_frame=" << sample.local_frame_id
                  << " source_gpu=" << sample.source_gpu_id
                  << " target_gpu=" << sample.target_gpu_id
                  << " gpu_direct=" << (sample.gpu_direct_mode ? 1 : 0)
                  << " direct_input=" << (sample.direct_input_enabled ? 1 : 0)
                  << " q=" << sample.queue_depth
                  << " free_buf=" << sample.available_buffers
                  << " free_evt=" << sample.available_events
                  << " waits=" << sample.resource_waits
                  << " drops=" << sample.frames_dropped
                  << " copy_ms=" << sample.copy_ms
                  << " total_pre_ms=" << sample.total_preprocess_ms
                  << std::endl;
    }
}

bool EncoderPreprocessWorker::IsDrained()
{
    return (in_flight_.load(std::memory_order_relaxed) == 0) &&
           (GetCountQueueInSize() == 0);
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

bool EncoderPreprocessWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    auto start_time = std::chrono::steady_clock::now();

    if (!entry) {
        return false;
    }

    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;

    // If we're not recording and not draining, just recycle and move on.
    if (!recording_enabled && !draining) {
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
             if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        return false;
    }

    in_flight_.fetch_add(1, std::memory_order_relaxed);

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

        if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            m_recycle_queue_.push(entry);
        }
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    
    // Successfully acquired resources - update counters
    if (direct_input_enabled_ || encoder_entry->d_prepared_frame) {
        available_buffers_--;
    }
    available_events_--;
    encoder_entry->preprocess_complete_event = event;
    encoder_entry->slot_id = direct_input_slot_id;
    encoder_entry->direct_input = direct_input_enabled_;
    encoder_entry->surface_pitch = static_cast<size_t>(direct_input_enabled_ ? direct_input_pitch_ : encoder_pitch_);
    encoder_entry->surface_gpu_id = preprocess_gpu_id_;
    encoder_entry->cross_gpu_copy_performed = false;
    if (direct_input_enabled_) {
        encoder_entry->d_prepared_frame = static_cast<unsigned char*>(direct_input_surfaces_[static_cast<size_t>(direct_input_slot_id)]);
    }

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
    if (entry->event_ptr) {
        ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
    }

    static thread_local HelperPreprocessProfileEvents helper_prof_events;
    helper_prof_events.Init(preprocess_gpu_id_);
    if (helper_prof_events.inflight) {
        cudaError_t helper_status = cudaEventQuery(helper_prof_events.total_end);
        if (helper_status == cudaSuccess) {
            float copy_ms = 0.0f;
            float total_ms = 0.0f;
            ck(cudaEventElapsedTime(&copy_ms,
                                    helper_prof_events.copy_start,
                                    helper_prof_events.copy_end));
            ck(cudaEventElapsedTime(&total_ms,
                                    helper_prof_events.copy_start,
                                    helper_prof_events.total_end));
            helper_prof_events.sample.copy_ms = copy_ms;
            helper_prof_events.sample.total_preprocess_ms = total_ms;
            append_helper_preprocess_sample(helper_prof_events.sample);
            helper_prof_events.inflight = false;
        } else if (helper_status != cudaErrorNotReady) {
            std::cerr << "[HELPER_PRE_DUMP] cam=" << camera_params_->camera_serial
                      << " helper profile event query failed: "
                      << cudaGetErrorString(helper_status) << std::endl;
            helper_prof_events.inflight = false;
        }
    }

    unsigned char* input_source = entry->d_image;
    bool sampled_helper_cross_gpu = false;
    if (entry->image_gpu_id >= 0 && entry->image_gpu_id != preprocess_gpu_id_) {
        if (!ensure_peer_access_enabled(entry->image_gpu_id)) {
            throw std::runtime_error(
                "Cross-GPU preprocess requested from source GPU " +
                std::to_string(entry->image_gpu_id) + " to preprocess GPU " +
                std::to_string(preprocess_gpu_id_) + ", but peer access is unavailable");
        }
        if (!d_input_staging_) {
            ck(cudaMalloc(&d_input_staging_, static_cast<size_t>(frame_original_gpu_.size_pic)));
        }
        ck(cudaEventRecord(encoder_entry->copy_start_event, m_stream));
        if (!helper_prof_events.inflight) {
            helper_prof_events.sample.recording_frame_id = entry->recording_frame_id;
            helper_prof_events.sample.local_frame_id = entry->frame_id;
            helper_prof_events.sample.source_gpu_id = entry->image_gpu_id;
            helper_prof_events.sample.target_gpu_id = preprocess_gpu_id_;
            helper_prof_events.sample.gpu_direct_mode = entry->gpu_direct_mode;
            helper_prof_events.sample.direct_input_enabled = direct_input_enabled_;
            helper_prof_events.sample.queue_depth = GetCountQueueInSize();
            helper_prof_events.sample.available_buffers =
                available_buffers_.load(std::memory_order_relaxed);
            helper_prof_events.sample.available_events =
                available_events_.load(std::memory_order_relaxed);
            helper_prof_events.sample.resource_waits =
                resource_waits_.load(std::memory_order_relaxed);
            helper_prof_events.sample.frames_dropped =
                frames_dropped_.load(std::memory_order_relaxed);
            ck(cudaEventRecord(helper_prof_events.copy_start, m_stream));
            sampled_helper_cross_gpu = true;
        }
        ck(cudaMemcpyPeerAsync(
            d_input_staging_,
            preprocess_gpu_id_,
            entry->d_image,
            entry->image_gpu_id,
            static_cast<size_t>(frame_original_gpu_.size_pic),
            m_stream));
        ck(cudaEventRecord(encoder_entry->copy_end_event, m_stream));
        if (sampled_helper_cross_gpu) {
            ck(cudaEventRecord(helper_prof_events.copy_end, m_stream));
        }
        encoder_entry->cross_gpu_copy_performed = true;
        input_source = d_input_staging_;
    }

    // --- Perform the copy and color conversion ---
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
    
    // Record an event in the stream once all the above GPU work is queued.
    ck(cudaEventRecord(*encoder_entry->preprocess_complete_event, m_stream));
    if (sampled_helper_cross_gpu) {
        ck(cudaEventRecord(helper_prof_events.total_end, m_stream));
        helper_prof_events.inflight = true;
    }
#if PIPELINE_PROFILE
    if (sample_preprocess) {
        ck(cudaEventRecord(enc_prof_events.end, m_stream));
        enc_prof_inflight = true;
    }
#endif

    // Release the main WORKER_ENTRY immediately.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue_.push(entry);
    }

    // Pass the prepared frame and its completion event to the hardware encoder.
    if (m_hw_worker_) {
        encoder_entry->recording_frame_id = entry->recording_frame_id;
        encoder_entry->timestamp = entry->timestamp;
        encoder_entry->timestamp_sys = entry->timestamp_sys;
        
        m_hw_worker_->PutObjectToQueueIn(encoder_entry);
    } else {
        // If there's no hardware worker, recycle the resources.
        free_encoder_entries_.push(encoder_entry);
        free_events_.push(event);
        if (direct_input_enabled_ && direct_input_slot_id >= 0) {
            free_direct_input_slots_.push(direct_input_slot_id);
            available_buffers_++;
        } else {
            available_buffers_++;
        }
        available_events_++;
    }
    in_flight_.fetch_sub(1, std::memory_order_relaxed);

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
