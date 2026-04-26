// src/video_capture.h
#ifndef ORANGE_VIDEO_CAPTURE
#define ORANGE_VIDEO_CAPTURE
#include "thread.h"
#include "camera.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <limits>
#include <mutex>
#include "network_base.h"
#include "common.hpp" // For pose::Object
#include <cuda_runtime.h>
#include "NvEncoder/NvCodecUtils.h"

class COpenGLDisplay;
class GPUVideoEncoder;
class YoloWorker;
class ImageWriterWorker;
class CropAndEncodeWorker;
class FrameIPCManager;

typedef struct {
    unsigned char* d_image;
    unsigned char* d_image_pool;
    unsigned char* d_analytics_image = nullptr;
    int width;
    int height;
    int pixelFormat;
    unsigned long long timestamp;
    unsigned long long frame_id;
    uint64_t camera_frame_id;
    uint64_t recording_frame_id;
    uint64_t ipc_frame_id;
    uint64_t source_buffer_bytes = 0;
    std::string recording_folder;
    uint64_t timestamp_sys;
    int image_gpu_id = -1;
    uint64_t acquisition_receive_host_ns = 0;
    uint64_t ingress_event_record_host_ns = 0;
    uint64_t yolo_ptp_done_host_ns = 0;
    uint64_t yolo_resource_ready_host_ns = 0;
    uint64_t yolo_pointer_attrs_done_host_ns = 0;
    uint64_t yolo_dispatch_ready_host_ns = 0;
    uint64_t yolo_before_recording_submit_host_ns = 0;
    uint64_t yolo_after_recording_submit_host_ns = 0;
    uint64_t yolo_enqueue_host_ns = 0;
    uint64_t yolo_enqueued_host_ns = 0;
    uint64_t yolo_dequeue_host_ns = 0;
    int yolo_queue_depth_at_enqueue = -1;
    int yolo_queue_depth_after_enqueue = -1;
    int yolo_queue_depth_after_dequeue = -1;
    bool yolo_dispatched = false;
    bool yolo_input_detach_requested = false;
    uint64_t yolo_input_ready_host_ns = 0;
    std::atomic<bool> yolo_input_ready_event_recorded;
    std::atomic<bool> yolo_completion_event_recorded;
    uint64_t yolo_detect_done_host_ns = 0;
    uint64_t recording_submit_host_ns = 0;
    int recording_target_gpu_id = -1;
    bool recording_helper_requested = false;
    bool recording_route_helper = false;
    uint64_t helper_enqueue_host_ns = 0;
    int helper_enqueue_queue_depth = -1;
    int helper_enqueue_available_buffers = -1;
    int helper_enqueue_available_events = -1;
    
    // YOLO detection fields
    std::vector<pose::Object> detections;
    bool has_detections;
    std::atomic<bool> detections_ready;

    // Velocity tracking fields
    bool tracking_ready;
    
    // Reference counting for memory management
    std::atomic<int> ref_count;
    
    // GPU Direct optimization fields
    bool gpu_direct_mode = false;
    bool owns_memory = true;
    
    // Camera buffer management (only used when gpu_direct_mode = true)
    void* camera_buffer_ptr = nullptr;
    Emergent::CEmergentCamera* camera_instance = nullptr;
    Emergent::CEmergentFrame* camera_frame_struct = nullptr;
    Emergent::CEmergentFrame camera_frame_recv;
    
    // Event for synchronization between workers
    cudaEvent_t* event_ptr; 

    // Optional event for an acquisition-time owned analytics copy
    cudaEvent_t analytics_ready_event = nullptr;
    bool analytics_owned_frame_valid = false;

    bool has_analytics_owned_source() const
    {
        return analytics_owned_frame_valid &&
               d_analytics_image != nullptr &&
               analytics_ready_event != nullptr;
    }

    unsigned char* delayed_consumer_image() const
    {
        return has_analytics_owned_source() ? d_analytics_image : d_image;
    }

    cudaEvent_t* delayed_consumer_event()
    {
        return has_analytics_owned_source() ? &analytics_ready_event : event_ptr;
    }

    const cudaEvent_t* delayed_consumer_event() const
    {
        return has_analytics_owned_source() ? &analytics_ready_event : event_ptr;
    }

    // New event specifically for YOLO completion
    cudaEvent_t* yolo_completion_event; 

    // Per-entry event recorded after YOLO has copied/preprocessed into its
    // owned TensorRT input buffer.
    cudaEvent_t yolo_input_ready_event = nullptr;

    // Frame IPC manager pointer (NEW)
    // This allows workers (especially YOLO) to update frame data with detections
    FrameIPCManager* frame_ipc_manager = nullptr;

} WORKER_ENTRY;

enum PictureSaveState {
    State_Frame_Idle = 0,
    State_Write_New_Frame = 1
};

struct CameraControl
{
    bool open = false;
    bool subscribe = false;
    bool stop_record = false;
    bool record_video = false;
    bool recording_draining = false;
    std::atomic<int> active_recorders{0};
    bool sync_camera = false;
    std::mutex recording_folder_mutex;
    std::string recording_folder;
};

struct CameraResources {
    static constexpr int DEFAULT_ACQUIRE_WORK_ENTRIES_MAX = 240;
    static constexpr int EVENT_POOL_SIZE = 256;

    WORKER_ENTRY* worker_entry_pool = nullptr;
    SafeQueue<WORKER_ENTRY*>* free_entries_queue = nullptr;
    SafeQueue<WORKER_ENTRY*>* recycle_queue = nullptr;

    std::vector<cudaEvent_t> event_pool;
    std::vector<cudaEvent_t> yolo_event_pool;
    SafeQueue<cudaEvent_t*>* free_events_queue = nullptr;
    SafeQueue<cudaEvent_t*>* yolo_events_queue = nullptr;
    int acquire_work_entries_max = DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;

    CameraResources() = default;
    CameraResources(const CameraResources&) = delete;
    CameraResources& operator=(const CameraResources&) = delete;

    CameraResources(CameraResources&& other) noexcept {
        worker_entry_pool = other.worker_entry_pool;
        free_entries_queue = other.free_entries_queue;
        recycle_queue = other.recycle_queue;
        event_pool = std::move(other.event_pool);
        yolo_event_pool = std::move(other.yolo_event_pool);
        free_events_queue = other.free_events_queue;
        yolo_events_queue = other.yolo_events_queue;
        acquire_work_entries_max = other.acquire_work_entries_max;
        other.worker_entry_pool = nullptr;
        other.free_entries_queue = nullptr;
        other.recycle_queue = nullptr;
        other.free_events_queue = nullptr;
        other.yolo_events_queue = nullptr;
        other.acquire_work_entries_max = DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
    }

    CameraResources& operator=(CameraResources&& other) noexcept {
        if (this != &other) {
            cleanup();
            worker_entry_pool = other.worker_entry_pool;
            free_entries_queue = other.free_entries_queue;
            recycle_queue = other.recycle_queue;
            event_pool = std::move(other.event_pool);
            yolo_event_pool = std::move(other.yolo_event_pool);
            free_events_queue = other.free_events_queue;
            yolo_events_queue = other.yolo_events_queue;
            acquire_work_entries_max = other.acquire_work_entries_max;
            other.worker_entry_pool = nullptr;
            other.free_entries_queue = nullptr;
            other.recycle_queue = nullptr;
            other.free_events_queue = nullptr;
            other.yolo_events_queue = nullptr;
            other.acquire_work_entries_max = DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
        }
        return *this;
    }

    static int resolve_acquire_work_entries_max(int configured_value) {
        const char* env = std::getenv("ORANGE_ACQUIRE_WORK_ENTRIES_MAX");
        if (!env || !*env) {
            if (configured_value <= 0) {
                return DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
            }
            if (configured_value > DEFAULT_ACQUIRE_WORK_ENTRIES_MAX) {
                std::cerr << "[CameraResources] Configured acquire_work_entries must be within [1,"
                          << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << "], using default "
                          << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << std::endl;
                return DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
            }
            return configured_value;
        }
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end == env || *end != '\0') {
            std::cerr << "[CameraResources] Ignoring invalid ORANGE_ACQUIRE_WORK_ENTRIES_MAX='"
                      << env << "', using default " << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << std::endl;
            return DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
        }
        if (parsed < 1 || parsed > DEFAULT_ACQUIRE_WORK_ENTRIES_MAX) {
            std::cerr << "[CameraResources] ORANGE_ACQUIRE_WORK_ENTRIES_MAX must be within [1,"
                      << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << "], using default "
                      << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << std::endl;
            return DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;
        }
        return static_cast<int>(parsed);
    }

    void initialize(int gpu_id,
                    size_t frame_size,
                    bool enable_yolo_events = true,
                    int configured_acquire_work_entries_max = 0) {
        ck(cudaSetDevice(gpu_id));

        acquire_work_entries_max = resolve_acquire_work_entries_max(configured_acquire_work_entries_max);
        if (acquire_work_entries_max != DEFAULT_ACQUIRE_WORK_ENTRIES_MAX) {
            std::cout << "[CameraResources] Using " << acquire_work_entries_max
                      << " acquisition work entries instead of "
                      << DEFAULT_ACQUIRE_WORK_ENTRIES_MAX << std::endl;
        }

        worker_entry_pool = new WORKER_ENTRY[acquire_work_entries_max];
        for (int i = 0; i < acquire_work_entries_max; ++i) {
            ck(cudaMalloc(&worker_entry_pool[i].d_image, frame_size));
            worker_entry_pool[i].d_image_pool = worker_entry_pool[i].d_image;
            ck(cudaEventCreateWithFlags(&worker_entry_pool[i].analytics_ready_event, cudaEventDisableTiming));
            ck(cudaEventCreateWithFlags(&worker_entry_pool[i].yolo_input_ready_event, cudaEventDisableTiming));
            worker_entry_pool[i].image_gpu_id = gpu_id;
            worker_entry_pool[i].yolo_input_ready_event_recorded.store(false);
            worker_entry_pool[i].yolo_completion_event_recorded.store(false);
            // Initialize the new frame_ipc_manager pointer to nullptr
            worker_entry_pool[i].frame_ipc_manager = nullptr;
            worker_entry_pool[i].camera_frame_id = 0;
            worker_entry_pool[i].recording_frame_id = 0;
            worker_entry_pool[i].ipc_frame_id = 0;
            worker_entry_pool[i].source_buffer_bytes = 0;
            worker_entry_pool[i].recording_folder.clear();
        }
        
        free_entries_queue = new SafeQueue<WORKER_ENTRY*>();
        for (int i = 0; i < acquire_work_entries_max; ++i) {
            free_entries_queue->push(&worker_entry_pool[i]);
        }
        
        recycle_queue = new SafeQueue<WORKER_ENTRY*>();
        
        event_pool.resize(EVENT_POOL_SIZE);
        free_events_queue = new SafeQueue<cudaEvent_t*>();
        for (int i = 0; i < EVENT_POOL_SIZE; ++i) {
            ck(cudaEventCreateWithFlags(&event_pool[i], cudaEventDisableTiming));
            free_events_queue->push(&event_pool[i]);
        }

        if (enable_yolo_events) {
            yolo_event_pool.resize(EVENT_POOL_SIZE);
            yolo_events_queue = new SafeQueue<cudaEvent_t*>();
            for (int i = 0; i < EVENT_POOL_SIZE; ++i) {
                ck(cudaEventCreateWithFlags(&yolo_event_pool[i], cudaEventDisableTiming));
                yolo_events_queue->push(&yolo_event_pool[i]);
            }
        }
    }

    void cleanup() {
        if (worker_entry_pool) {
            for (int i = 0; i < acquire_work_entries_max; ++i) {
                if (worker_entry_pool[i].analytics_ready_event) {
                    cudaEventDestroy(worker_entry_pool[i].analytics_ready_event);
                }
                if (worker_entry_pool[i].yolo_input_ready_event) {
                    cudaEventDestroy(worker_entry_pool[i].yolo_input_ready_event);
                }
                if (worker_entry_pool[i].d_image_pool) {
                    cudaFree(worker_entry_pool[i].d_image_pool);
                }
                worker_entry_pool[i].d_image = nullptr;
                worker_entry_pool[i].d_image_pool = nullptr;
                worker_entry_pool[i].d_analytics_image = nullptr;
                worker_entry_pool[i].analytics_ready_event = nullptr;
                worker_entry_pool[i].yolo_input_ready_event = nullptr;
            }
            delete[] worker_entry_pool;
            worker_entry_pool = nullptr;
        }
        acquire_work_entries_max = DEFAULT_ACQUIRE_WORK_ENTRIES_MAX;

        if (free_entries_queue) { delete free_entries_queue; free_entries_queue = nullptr; }
        if (recycle_queue) { delete recycle_queue; recycle_queue = nullptr; }
        
        if (free_events_queue) { delete free_events_queue; free_events_queue = nullptr; }
        for (auto& event : event_pool) {
            if (event) cudaEventDestroy(event);
        }
        event_pool.clear();

        if (yolo_events_queue) { delete yolo_events_queue; yolo_events_queue = nullptr; }
        for (auto& event : yolo_event_pool) {
            if (event) cudaEventDestroy(event);
        }
        yolo_event_pool.clear();
    }
};


struct CameraEachSelect
{
    bool stream_on = true;
    bool record = false;
    bool yolo = false;
    bool crop_and_encode = false;
    bool pose = false;
    int downsample = 1;
    int display_preview_max_fps = 60;
    bool record_output_override = false;
    std::string record_output_mode = "factor";
    int record_downsample_factor = 1;
    int record_output_width = 1024;
    int record_output_height = 1024;
    PictureSaveState frame_save_state = State_Frame_Idle;
    std::string frame_save_format;
    std::string frame_save_name;
    int pictures_counter = 0;
    bool selected_to_save = false;
    std::string picture_save_folder;
    const char* yolo_model = nullptr;

    // Frame IPC settings (NEW)
    bool send_frame_ipc = true;      // Enable frame synchronization via IPC
    bool send_yolo_via_frame_ipc = true;
    
    // Legacy/deprecated settings (kept for backward compatibility)
    bool send_yolo_via_enet = false;
};

struct CameraState
{
    int camera_return = 0;
    unsigned short id_prev = 0;
    // Counts true camera frame-id gaps. SDK GetFrame errors are tracked separately.
    uint64_t dropped_frames = 0;
    uint64_t get_frame_errors = 0;
    int last_get_frame_error_code = 0;
    unsigned int frames_recd = 0;
    unsigned long long frame_count = 0;
};

static inline uint64_t count_camera_frame_id_gaps(unsigned short prev_id, unsigned short current_id)
{
    if (prev_id == 0 || current_id == 0) {
        return 0;
    }

    constexpr uint32_t kFrameIdMax = std::numeric_limits<unsigned short>::max();
    const uint32_t expected_next = (prev_id == kFrameIdMax) ? 1u : (static_cast<uint32_t>(prev_id) + 1u);
    const uint32_t current = static_cast<uint32_t>(current_id);

    if (current == expected_next) {
        return 0;
    }

    if (current > prev_id) {
        return current - expected_next;
    }

    return (kFrameIdMax - static_cast<uint32_t>(prev_id)) + current;
}

static inline unsigned short next_camera_frame_id_prev(unsigned short current_id)
{
    constexpr unsigned short kFrameIdMax = std::numeric_limits<unsigned short>::max();
    return (current_id == kFrameIdMax) ? 0 : current_id;
}

struct PTPState
{
    int ptp_offset;
    int ptp_offset_sum=0;
    int ptp_offset_prev=0;
    unsigned int ptp_time_low;
    unsigned int ptp_time_high;
    unsigned int ptp_time_plus_delta_to_start_low;
    unsigned int ptp_time_plus_delta_to_start_high;
    unsigned long long ptp_time_delta_sum = 0;
    unsigned long long ptp_time_delta_samples = 0;
    unsigned long long ptp_time_delta;
    unsigned long long ptp_time;
    unsigned long long ptp_time_prev;
    unsigned long long ptp_register_read_count = 0;
    unsigned long long last_ptp_register_read_frame = 0;
    bool ptp_register_read_this_frame = false;
    unsigned long long ptp_time_countdown;
    unsigned long long frame_ts;
    unsigned long long frame_ts_prev;
    unsigned long long frame_ts_delta;
    unsigned long long frame_ts_delta_sum = 0;
    unsigned long long ptp_time_plus_delta_to_start;
    char ptp_status[100];
    unsigned long ptp_status_sz_ret;
    unsigned int ptp_time_plus_delta_to_start_uint;
};

void report_statistics(CameraParams *camera_params,
                       CameraState *camera_state,
                       double time_diff,
                       uint64_t preprocess_resource_waits = 0,
                       uint64_t preprocess_frames_dropped = 0,
                       uint64_t encode_failures = 0,
                       uint64_t encode_slow_frames = 0);
void show_ptp_offset(PTPState *ptp_state, CameraEmergent *ecam);
void start_ptp_sync(PTPState *ptp_state, PTPParams *ptp_params, CameraParams *camera_params, CameraEmergent *ecam, unsigned int delay_in_second);
void grab_frames_after_countdown(PTPState *ptp_state, CameraEmergent *ecam);
bool try_start_timer();
bool try_stop_timer();
#endif
