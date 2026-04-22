// src/yolo_worker.cpp
#include "yolo_worker.h"
#include "kernel.cuh"
#include "npp_utils.h"
#include <cuda_runtime_api.h>
#include <nppi.h>
#include <npp.h>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include "yolo_payload_generated.h"
#include "message_wrapper_generated.h"
#include "opengldisplay.h"
#include "pose_shaman.h"
#include "thread.h"
#include "global.h"
#include "cuda_context_debug.h"
#include "opencv2/opencv.hpp"
#include "crop_and_encode_worker.h"
#include "frame_ipc_manager.h"
#include "yolo_event_log.h"
#include "project.h"
#include "fsuid_guard.h"
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <utility>
#include <cstring>
#include <pthread.h>
#include <sched.h>

#ifndef YOLO_PROFILE
#define YOLO_PROFILE 0
#endif
#if YOLO_PROFILE
static constexpr int kYoloProfileLogEvery = 60;
#endif

namespace {
bool SkipCpuResults()
{
    static const bool enabled = []() {
        const char* env = std::getenv("ORANGE_YOLO_SKIP_CPU_RESULTS");
        const bool on = env && *env && std::strcmp(env, "0") != 0;
        if (on) {
            std::cout << "[YOLO] CPU results disabled (postprocess/tracking/IPC/ENet)." << std::endl;
        }
        return on;
    }();
    return enabled;
}

bool UseEventSyncWait()
{
    static const bool enabled = []() {
        const char* env = std::getenv("ORANGE_YOLO_SYNC_EVENT");
        const bool on = env && *env && std::strcmp(env, "0") != 0;
        if (on) {
            std::cout << "[YOLO] Using cudaEventSynchronize for GPU sync." << std::endl;
        }
        return on;
    }();
    return enabled;
}

bool ParseCpuList(const char* env, cpu_set_t* out_set, std::string* out_str)
{
    if (!env || !*env) {
        return false;
    }
    CPU_ZERO(out_set);
    bool any = false;
    std::string spec(env);
    size_t pos = 0;
    while (pos < spec.size()) {
        while (pos < spec.size() && (spec[pos] == ' ' || spec[pos] == ',')) {
            pos++;
        }
        if (pos >= spec.size()) {
            break;
        }
        size_t end = pos;
        while (end < spec.size() && spec[end] != ',') {
            end++;
        }
        std::string token = spec.substr(pos, end - pos);
        size_t dash = token.find('-');
        if (dash == std::string::npos) {
            int cpu = std::atoi(token.c_str());
            if (cpu >= 0) {
                CPU_SET(cpu, out_set);
                any = true;
            }
        } else {
            int start = std::atoi(token.substr(0, dash).c_str());
            int stop = std::atoi(token.substr(dash + 1).c_str());
            if (start > stop) {
                std::swap(start, stop);
            }
            if (start >= 0) {
                for (int cpu = start; cpu <= stop; ++cpu) {
                    CPU_SET(cpu, out_set);
                    any = true;
                }
            }
        }
        pos = end + 1;
    }
    if (out_str) {
        *out_str = spec;
    }
    return any;
}

void ApplyYoloAffinity()
{
    const char* env = std::getenv("ORANGE_YOLO_AFFINITY");
    cpu_set_t cpuset;
    std::string spec;
    if (!ParseCpuList(env, &cpuset, &spec)) {
        return;
    }
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "[YOLO] Failed to set affinity (" << spec << "): " << std::strerror(rc) << std::endl;
    } else {
        std::cout << "[YOLO] Applied CPU affinity: " << spec << std::endl;
    }
}
}  // namespace

namespace yolo_perf {
struct YoloPerfRecord {
    uint64_t frame_id = 0;
    uint64_t recording_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    int queue_depth = 0;
    double fps = 0.0;
    int ok = 0;
    double wait_ms = -1.0;
    double pre_ms = -1.0;
    double gap_ms = -1.0;
    double enqueue_ms = -1.0;
    double infer_ms = -1.0;
    double sync_ms = -1.0;
    double cpu_wait_event_ms = -1.0;
    double cpu_npp_set_stream_ms = -1.0;
    double cpu_preprocess_ms = -1.0;
    double cpu_dump_ms = -1.0;
    double cpu_infer_call_ms = -1.0;
    double cpu_event_record_ms = -1.0;
    double cpu_pre_sync_ms = -1.0;
    double cpu_pre_sync_other_ms = -1.0;
    double cpu_post_sync_ms = -1.0;
    double queue_ms = -1.0;
    double post_ms = -1.0;
    double track_ms = -1.0;
    double ipc_ms = -1.0;
    double enet_ms = -1.0;
    double total_ms = -1.0;
};

enum class YoloPerfEventType {
    kSample,
    kRotate,
    kClose,
};

struct YoloPerfEvent {
    YoloPerfEventType type = YoloPerfEventType::kSample;
    YoloPerfRecord record;
    std::string folder;
};

class YoloPerfLogger {
public:
    YoloPerfLogger(const std::string& camera_serial, const std::string& worker_name)
        : camera_serial_(camera_serial),
          worker_name_(worker_name),
          running_(true) {
        thread_ = std::thread(&YoloPerfLogger::ThreadMain, this);
    }

    ~YoloPerfLogger() {
        Stop();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Rotate(const std::string& folder) {
        YoloPerfEvent event;
        event.type = YoloPerfEventType::kRotate;
        event.folder = folder;
        EnqueueEvent(std::move(event));
    }

    void Close() {
        YoloPerfEvent event;
        event.type = YoloPerfEventType::kClose;
        EnqueueEvent(std::move(event));
    }

    void Enqueue(const YoloPerfRecord& record) {
        YoloPerfEvent event;
        event.type = YoloPerfEventType::kSample;
        event.record = record;
        EnqueueEvent(std::move(event));
    }

private:
    static constexpr size_t kMaxQueue = 8192;

    void EnqueueEvent(YoloPerfEvent&& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        if (queue_.size() >= kMaxQueue) {
            dropped_++;
            return;
        }
        queue_.push_back(std::move(event));
        cv_.notify_one();
    }

    void OpenFile(const std::string& folder) {
        if (folder.empty()) {
            return;
        }
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(folder);
        current_folder_ = folder;
        file_path_ = current_folder_ + "/Cam" + camera_serial_ + "_yolo_perf.csv";
        file_.open(file_path_, std::ios::out | std::ios::trunc);
        if (!file_) {
            std::cerr << "[YOLO_PERF] " << worker_name_
                      << " failed to open " << file_path_ << std::endl;
            return;
        }
        file_ << "frame_id,recording_frame_id,timestamp,timestamp_sys,queue_depth,fps,ok,"
                 "wait_ms,pre_ms,gap_ms,enqueue_ms,infer_ms,sync_ms,"
                 "cpu_wait_event_ms,cpu_npp_set_stream_ms,cpu_preprocess_ms,cpu_dump_ms,cpu_infer_call_ms,cpu_event_record_ms,cpu_pre_sync_ms,cpu_pre_sync_other_ms,cpu_post_sync_ms,"
                 "queue_ms,post_ms,track_ms,ipc_ms,enet_ms,total_ms\n";
        file_ << std::fixed << std::setprecision(6);
        std::cout << "[YOLO_PERF] " << worker_name_ << " logging to " << file_path_ << std::endl;
    }

    void CloseFile() {
        if (file_.is_open()) {
            file_.close();
        }
        current_folder_.clear();
        file_path_.clear();
    }

    void WriteRecord(const YoloPerfRecord& record) {
        if (!file_.is_open()) {
            return;
        }
        file_ << record.frame_id << ","
              << record.recording_frame_id << ","
              << record.timestamp << ","
              << record.timestamp_sys << ","
              << record.queue_depth << ","
              << record.fps << ","
              << record.ok << ","
              << record.wait_ms << ","
              << record.pre_ms << ","
              << record.gap_ms << ","
              << record.enqueue_ms << ","
              << record.infer_ms << ","
              << record.sync_ms << ","
              << record.cpu_wait_event_ms << ","
              << record.cpu_npp_set_stream_ms << ","
              << record.cpu_preprocess_ms << ","
              << record.cpu_dump_ms << ","
              << record.cpu_infer_call_ms << ","
              << record.cpu_event_record_ms << ","
              << record.cpu_pre_sync_ms << ","
              << record.cpu_pre_sync_other_ms << ","
              << record.cpu_post_sync_ms << ","
              << record.queue_ms << ","
              << record.post_ms << ","
              << record.track_ms << ","
              << record.ipc_ms << ","
              << record.enet_ms << ","
              << record.total_ms << "\n";
    }

    void ThreadMain() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_ || !queue_.empty()) {
            if (queue_.empty()) {
                cv_.wait(lock);
                continue;
            }
            YoloPerfEvent event = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();

            switch (event.type) {
                case YoloPerfEventType::kRotate:
                    if (event.folder != current_folder_) {
                        CloseFile();
                        OpenFile(event.folder);
                    }
                    break;
                case YoloPerfEventType::kClose:
                    CloseFile();
                    break;
                case YoloPerfEventType::kSample:
                    WriteRecord(event.record);
                    break;
            }

            lock.lock();
        }
        CloseFile();
        if (dropped_ > 0) {
            std::cerr << "[YOLO_PERF] " << worker_name_
                      << " dropped " << dropped_ << " samples" << std::endl;
        }
    }

    std::string camera_serial_;
    std::string worker_name_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<YoloPerfEvent> queue_;
    bool running_;
    std::string current_folder_;
    std::string file_path_;
    std::ofstream file_;
    size_t dropped_ = 0;
};

struct YoloPerfConfig {
    bool enabled = true;
    int sample_rate = 1;
};

YoloPerfConfig GetYoloPerfConfig() {
    static YoloPerfConfig config = []() {
        YoloPerfConfig cfg;
        if (const char* env = std::getenv("ORANGE_YOLO_PERF_LOG")) {
            if (std::atoi(env) == 0) {
                cfg.enabled = false;
            }
        }
        if (const char* env = std::getenv("ORANGE_YOLO_PERF_SAMPLE")) {
            int sample = std::atoi(env);
            if (sample > 1) {
                cfg.sample_rate = sample;
            }
        }
        return cfg;
    }();
    return config;
}
} // namespace yolo_perf

YoloWorker::YoloWorker(const char* name,
                       CameraParams* cam_params,
                       CameraEachSelect* cam_select,
                       CameraControl* camera_control,
                       SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker(name),
      yolov8_instance_(nullptr),
      associated_camera_params_(cam_params),
      associated_camera_select_(cam_select),
      camera_control_(camera_control),
      enet_host_context_(nullptr),
      enet_target_peer_(nullptr),
      fb_builder_(nullptr),
      last_fps_update_time_(std::chrono::steady_clock::now()),
      frame_counter_(0),
      current_fps_(0.0),
      m_recycle_queue(recycle_queue),
      m_dump_next_frame(false)
{
    ck(cudaSetDevice(associated_camera_params_->gpu_id));
    std::cout << "YoloWorker constructor set to CUDA device: " << associated_camera_params_->gpu_id << std::endl;

    try {
        if (!associated_camera_params_ || !associated_camera_select_) {
            throw std::runtime_error("CameraParams or CameraEachSelect is null.");
        }

        fb_builder_ = new flatbuffers::FlatBufferBuilder(1024 * 4);

        if (associated_camera_select_->yolo_model == nullptr || strlen(associated_camera_select_->yolo_model) == 0) {
            throw std::runtime_error("YOLO model path is null or empty. Cannot initialize YOLOv8.");
        }

        yolov8_instance_ = new YOLOv8(associated_camera_select_->yolo_model,
                                     associated_camera_params_->width,
                                     associated_camera_params_->height);
        yolov8_instance_->make_pipe(true);

        std::cout << "[YOLOv8 MODEL INFO] " << name << " expects input size: "
                  << yolov8_instance_->inp_w_int << "x" << yolov8_instance_->inp_h_int << std::endl;

        initalize_gpu_frame(&frame_original_gpu_, associated_camera_params_);
        initialize_gpu_debayer(&debayer_gpu_, associated_camera_params_);

        std::cout << "YoloWorker for " << name << " initialized successfully." << std::endl;
        // IPC logging disabled: YOLO IPC note.

        event_logger_ = std::make_unique<yolo_event_log::YoloEventLogger>(
            associated_camera_params_->camera_serial,
            associated_camera_params_->camera_id,
            threadName
        );

        const yolo_perf::YoloPerfConfig perf_cfg = yolo_perf::GetYoloPerfConfig();
        if (perf_cfg.enabled) {
            perf_logger_ = std::make_unique<yolo_perf::YoloPerfLogger>(
                associated_camera_params_->camera_serial,
                threadName
            );
            perf_sample_rate_ = perf_cfg.sample_rate;
            if (perf_sample_rate_ < 1) {
                perf_sample_rate_ = 1;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "YoloWorker Error for " << name << ": " << e.what() << std::endl;

        if (fb_builder_) { delete fb_builder_; fb_builder_ = nullptr; }
        if (yolov8_instance_) { delete yolov8_instance_; yolov8_instance_ = nullptr; }
        if (event_logger_) { event_logger_->Stop(); event_logger_.reset(); }
        if (frame_original_gpu_.d_orig) { cudaFree(frame_original_gpu_.d_orig); frame_original_gpu_.d_orig = nullptr; }
        if (debayer_gpu_.d_debayer) { cudaFree(debayer_gpu_.d_debayer); debayer_gpu_.d_debayer = nullptr; }

        throw;
    }
}

YoloWorker::~YoloWorker() {
    std::cout << "YoloWorker destructor for " << threadName << std::endl;

    if (perf_logger_) {
        perf_logger_->Stop();
        perf_logger_.reset();
    }
    if (event_logger_) {
        event_logger_->Stop();
        event_logger_.reset();
    }

    if (associated_camera_params_) {
        ck(cudaSetDevice(associated_camera_params_->gpu_id));
        if (debayer_gpu_.d_debayer) { cudaFree(debayer_gpu_.d_debayer); }
        if (frame_original_gpu_.d_orig) { cudaFree(frame_original_gpu_.d_orig); }
        if (yolov8_instance_) { delete yolov8_instance_; }
    }

    if (fb_builder_) delete fb_builder_;

    std::cout << "YoloWorker destructor complete for " << threadName << std::endl;
}

void YoloWorker::SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker) {
    m_crop_worker = crop_worker;
}

void YoloWorker::SetDisplayWorker(COpenGLDisplay* display_worker) {
    m_display_worker = display_worker;
}

void YoloWorker::SetENetTarget(EnetContext* host_ctx, ENetPeer* target_peer)
{
    std::cout << "YoloWorker (" << this->threadName
              << "): SetENetTarget called. Host_ctx: " << static_cast<void*>(host_ctx)
              << ". Target_peer: " << static_cast<void*>(target_peer) << std::endl;
    enet_host_context_ = host_ctx;
    enet_target_peer_ = target_peer;
}

bool YoloWorker::WorkerFunction(WORKER_ENTRY* entry) {
    static thread_local bool affinity_set = false;
    if (!affinity_set) {
        ApplyYoloAffinity();
        affinity_set = true;
    }
    if (!yolov8_instance_ || !entry || !entry->d_image) {
        if (entry && entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_recycle_queue.push(entry);
        }
        return false;
    }

    // Set the CUDA device for this thread.
    ck(cudaSetDevice(associated_camera_params_->gpu_id));

    try {
        const bool skip_cpu_results = SkipCpuResults();
        if (perf_logger_) {
            std::string recording_folder;
            {
                std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
                recording_folder = camera_control_->recording_folder;
            }
            if (recording_folder != perf_log_folder_) {
                perf_log_folder_ = recording_folder;
                if (perf_log_folder_.empty()) {
                    perf_logger_->Close();
                } else {
                    perf_logger_->Rotate(perf_log_folder_);
                }
            }
        }

        const auto fps_now = std::chrono::steady_clock::now();
        frame_counter_++;
        std::chrono::duration<double> fps_elapsed = fps_now - last_fps_update_time_;
        if (fps_elapsed.count() >= 1.0) {
            const double fps = frame_counter_ / fps_elapsed.count();
            current_fps_.store(fps, std::memory_order_relaxed);
            frame_counter_ = 0;
            last_fps_update_time_ = fps_now;
#if YOLO_PROFILE
            std::cout << "[YOLO_FPS] " << threadName
                      << " fps=" << fps
                      << " q=" << GetCountQueueInSize()
                      << std::endl;
#endif
        }

        float ms_pre = -1.0f;
        float ms_gap = -1.0f;
        float ms_infer = -1.0f;
        float ms_wait = -1.0f;
        double ms_enqueue = -1.0;
        double ms_sync_wait = -1.0;
        double ms_queue = -1.0;
        double ms_post = -1.0;
        double ms_track = -1.0;
        double ms_ipc = -1.0;
        double ms_enet = -1.0;
        double ms_cpu_wait_event = 0.0;
        double ms_cpu_npp_set_stream = 0.0;
        double ms_cpu_preprocess = -1.0;
        double ms_cpu_dump = 0.0;
        double ms_cpu_infer_call = -1.0;
        double ms_cpu_event_record = 0.0;
        double ms_cpu_pre_sync = -1.0;
        double ms_cpu_pre_sync_other = -1.0;
        double ms_cpu_post_sync = -1.0;
        double ms_total = -1.0;
        const auto cpu_start = std::chrono::steady_clock::now();
#if YOLO_PROFILE
        struct YoloProfileEvents {
            cudaEvent_t pre_start{};
            cudaEvent_t pre_end{};
            cudaEvent_t infer_start{};
            cudaEvent_t infer_end{};
            cudaEvent_t wait_start{};
            cudaEvent_t wait_end{};
            int device = -1;
            bool initialized = false;

            void Init(int gpu_id) {
                if (initialized) {
                    return;
                }
                device = gpu_id;
                ck(cudaSetDevice(device));
                ck(cudaEventCreate(&pre_start));
                ck(cudaEventCreate(&pre_end));
                ck(cudaEventCreate(&infer_start));
                ck(cudaEventCreate(&infer_end));
                ck(cudaEventCreate(&wait_start));
                ck(cudaEventCreate(&wait_end));
                initialized = true;
            }

            ~YoloProfileEvents() {
                if (!initialized) {
                    return;
                }
                if (device >= 0) {
                    cudaSetDevice(device);
                }
                cudaEventDestroy(pre_start);
                cudaEventDestroy(pre_end);
                cudaEventDestroy(infer_start);
                cudaEventDestroy(infer_end);
                cudaEventDestroy(wait_start);
                cudaEventDestroy(wait_end);
            }
        };

        static thread_local YoloProfileEvents prof_events;
        static thread_local int prof_count = 0;
        prof_events.Init(associated_camera_params_->gpu_id);
        bool timed_wait = false;
#endif
        const int camera_width = associated_camera_params_->width;
        const int camera_height = associated_camera_params_->height;

        // Set the NPP stream to the one used by the YOLO instance.
        const auto cpu_npp_set_start = std::chrono::steady_clock::now();
        EnsureNppStream(yolov8_instance_->stream);
        const auto cpu_npp_set_end = std::chrono::steady_clock::now();
        ms_cpu_npp_set_stream = std::chrono::duration<double, std::milli>(cpu_npp_set_end - cpu_npp_set_start).count();

        // Wait for the previous stage (acquire_frames) to finish copying data.
        if (entry->event_ptr) {
            const auto cpu_wait_start = std::chrono::steady_clock::now();
#if YOLO_PROFILE
            ck(cudaEventRecord(prof_events.wait_start, yolov8_instance_->stream));
#endif
            ck(cudaStreamWaitEvent(yolov8_instance_->stream, *entry->event_ptr, 0));
#if YOLO_PROFILE
            ck(cudaEventRecord(prof_events.wait_end, yolov8_instance_->stream));
            timed_wait = true;
#endif
            const auto cpu_wait_end = std::chrono::steady_clock::now();
            ms_cpu_wait_event = std::chrono::duration<double, std::milli>(cpu_wait_end - cpu_wait_start).count();
        } else {
            ms_cpu_wait_event = 0.0;
        }

        frame_original_gpu_.d_orig = entry->d_image;
        debayer_gpu_.size.width = camera_width;
        debayer_gpu_.size.height = camera_height;

        // Debayer or duplicate mono channel to prepare for color conversion.
        const auto cpu_preprocess_start = std::chrono::steady_clock::now();
#if YOLO_PROFILE
        ck(cudaEventRecord(prof_events.pre_start, yolov8_instance_->stream));
#endif
        if (associated_camera_params_->color) {
            // If color, debayer to RGBA first, then our kernel will handle the rest.
            debayer_frame_gpu(associated_camera_params_, &frame_original_gpu_, &debayer_gpu_);
            yolov8_instance_->preprocess_gpu(debayer_gpu_.d_debayer, camera_width, camera_height, true);
        } else {
            // If mono, pass the raw mono buffer directly to the kernel.
            yolov8_instance_->preprocess_gpu(frame_original_gpu_.d_orig, camera_width, camera_height, false);
        }
#if YOLO_PROFILE
        ck(cudaEventRecord(prof_events.pre_end, yolov8_instance_->stream));
#endif
        const auto cpu_preprocess_end = std::chrono::steady_clock::now();
        ms_cpu_preprocess = std::chrono::duration<double, std::milli>(cpu_preprocess_end - cpu_preprocess_start).count();

        // Logic for dumping a debug frame if requested.
        bool dump_this_frame = m_dump_next_frame.exchange(false);
        if (dump_this_frame)
        {
            const auto cpu_dump_start = std::chrono::steady_clock::now();
            size_t image_size_bytes = (size_t)camera_width * camera_height * 4;
            unsigned char* h_rgba_buffer = new unsigned char[image_size_bytes];
            ck(cudaMemcpy(h_rgba_buffer, debayer_gpu_.d_debayer, image_size_bytes, cudaMemcpyDeviceToHost));
            try {
                cv::Mat rgba_image(camera_height, camera_width, CV_8UC4, h_rgba_buffer);
                cv::Mat bgr_image;
                cv::cvtColor(rgba_image, bgr_image, cv::COLOR_RGBA2BGR);
                std::string filename = "debug_pre_yolo_" + std::string(associated_camera_params_->camera_serial) + "_" + std::to_string(entry->frame_id) + ".png";
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                cv::imwrite(filename, bgr_image);
                std::cout << "[" << threadName << "] Saved debug image to " << filename << std::endl;
            } catch (const cv::Exception& ex) {
                std::cerr << "OpenCV exception while saving debug image: " << ex.what() << std::endl;
            }
            delete[] h_rgba_buffer;
            const auto cpu_dump_end = std::chrono::steady_clock::now();
            ms_cpu_dump = std::chrono::duration<double, std::milli>(cpu_dump_end - cpu_dump_start).count();
        }

        // Preprocess and run inference. These are non-blocking CUDA calls.
        const auto infer_call_start = std::chrono::steady_clock::now();
        auto inference_start_time = infer_call_start;
#if YOLO_PROFILE
        ck(cudaEventRecord(prof_events.infer_start, yolov8_instance_->stream));
#endif
        yolov8_instance_->infer();
#if YOLO_PROFILE
        const auto infer_call_end = std::chrono::steady_clock::now();
        ms_enqueue = std::chrono::duration<double, std::milli>(infer_call_end - infer_call_start).count();
        ck(cudaEventRecord(prof_events.infer_end, yolov8_instance_->stream));
#else
        const auto infer_call_end = std::chrono::steady_clock::now();
#endif
        ms_cpu_infer_call = std::chrono::duration<double, std::milli>(infer_call_end - infer_call_start).count();

        // record per-frame event for synchronization
        if (entry->yolo_completion_event) {
            const auto cpu_event_start = std::chrono::steady_clock::now();
            ck(cudaEventRecord(*entry->yolo_completion_event, yolov8_instance_->stream));
            const auto cpu_event_end = std::chrono::steady_clock::now();
            ms_cpu_event_record = std::chrono::duration<double, std::milli>(cpu_event_end - cpu_event_start).count();
        }

        // Wait for the GPU to finish, with a timeout.
        const int timeout_us = 100000; // 100ms timeout
        bool finished_in_time = false;

        const auto sync_wait_start = std::chrono::steady_clock::now();
        if (UseEventSyncWait() && entry->yolo_completion_event) {
            cudaError_t result = cudaEventSynchronize(*entry->yolo_completion_event);
            if (result == cudaSuccess) {
                finished_in_time = true;
            } else {
                ck(result);
            }
        } else {
            while (true) {
                cudaError_t result = cudaStreamQuery(yolov8_instance_->stream);
                if (result == cudaSuccess) {
                    finished_in_time = true;
                    break;
                }
                if (result != cudaErrorNotReady) {
                    // An actual error occurred
                    ck(result);
                    break;
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - inference_start_time).count();

                if (elapsed_us > timeout_us) {
                    std::cerr << "[YoloWorker] WARNING: Inference timed out after " << elapsed_us << "us. Dropping frame." << std::endl;
                    break; // Timed out
                }

                // Wait for a very short time before polling again
                usleep(100);
            }
        }
        const auto sync_wait_end = std::chrono::steady_clock::now();
#if YOLO_PROFILE
        ms_sync_wait = std::chrono::duration<double, std::milli>(sync_wait_end - sync_wait_start).count();
#endif
        ms_cpu_pre_sync = std::chrono::duration<double, std::milli>(sync_wait_start - cpu_start).count();
        ms_cpu_pre_sync_other = ms_cpu_pre_sync - (
            ms_cpu_wait_event +
            ms_cpu_npp_set_stream +
            ms_cpu_preprocess +
            ms_cpu_dump +
            ms_cpu_infer_call +
            ms_cpu_event_record
        );
        if (ms_cpu_pre_sync_other < 0.0) {
            ms_cpu_pre_sync_other = 0.0;
        }

#if YOLO_PROFILE
        if (finished_in_time) {
            ck(cudaEventSynchronize(prof_events.infer_end));
            if (timed_wait) {
                ck(cudaEventElapsedTime(&ms_wait, prof_events.wait_start, prof_events.wait_end));
            }
            ck(cudaEventElapsedTime(&ms_pre, prof_events.pre_start, prof_events.pre_end));
            ck(cudaEventElapsedTime(&ms_gap, prof_events.pre_end, prof_events.infer_start));
            ck(cudaEventElapsedTime(&ms_infer, prof_events.infer_start, prof_events.infer_end));
            ms_queue = ms_sync_wait - ms_infer;
            if (ms_queue < 0.0) {
                ms_queue = 0.0;
            }
        }
#endif
        if (finished_in_time) {
            // Now that the GPU is finished, process the results.
            // This completely REPLACES entry->detections, preventing stale data
#if YOLO_PROFILE
            const auto post_start = std::chrono::steady_clock::now();
#endif
            if (!skip_cpu_results) {
                yolov8_instance_->postprocess(entry->detections);
            } else {
                entry->detections.clear();
            }
#if YOLO_PROFILE
            const auto post_end = std::chrono::steady_clock::now();
            ms_post = std::chrono::duration<double, std::milli>(post_end - post_start).count();
#endif
        } else {
            // Timed out, clear any potential partial results
            entry->detections.clear();
        }

        // Update velocity tracking with new detections
#if YOLO_PROFILE
        const auto track_start = std::chrono::steady_clock::now();
#endif
        if (!skip_cpu_results) {
            uint64_t current_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
            velocity_tracker_.updateTracking(entry->detections, current_timestamp_us);
        }
#if YOLO_PROFILE
        const auto track_end = std::chrono::steady_clock::now();
        ms_track = std::chrono::duration<double, std::milli>(track_end - track_start).count();
#endif

        // After detections are found, dispatch to the crop worker if it exists AND recording is on
        if (!skip_cpu_results && m_crop_worker && camera_control_->record_video) {
            // Increment the reference count because another worker will now use this entry
            entry->ref_count.fetch_add(1, std::memory_order_acq_rel);
            m_crop_worker->PutObjectToQueueIn(entry);
        }

        // Mark if we have detections
        entry->has_detections = !entry->detections.empty();
        entry->detections_ready.store(true);

        // NEW: Update Frame IPC with YOLO detection results
#if YOLO_PROFILE
        const auto ipc_start = std::chrono::steady_clock::now();
#endif
        uint64_t frame_id_for_ipc = entry->ipc_frame_id;
        if (frame_id_for_ipc == 0) {
            frame_id_for_ipc = (entry->recording_frame_id > 0)
                               ? entry->recording_frame_id
                               : entry->frame_id;
        }

        FrameIPCManager* frame_ipc = entry->frame_ipc_manager;
        const bool frame_ipc_enabled = frame_ipc && frame_ipc->isEnabled();
        const std::string frame_ipc_queue_name = frame_ipc
            ? frame_ipc->getQueueName()
            : ("/shm_cam_" + associated_camera_params_->camera_serial);
        bool frame_ipc_update_requested = false;
        std::string frame_ipc_request_status = frame_ipc_enabled
            ? "not_requested_zero_detections"
            : "not_enabled";

        std::string yolo_status;
        std::string yolo_error;
        if (!finished_in_time) {
            yolo_status = "timeout";
            yolo_error = "inference_timeout";
            frame_ipc_request_status = frame_ipc_enabled
                ? "not_requested_failed"
                : "not_enabled";
        } else if (skip_cpu_results) {
            yolo_status = "failed";
            yolo_error = "cpu_results_skipped";
            frame_ipc_request_status = frame_ipc_enabled
                ? "not_requested_failed"
                : "not_enabled";
        } else if (entry->has_detections) {
            yolo_status = "detections";
            frame_ipc_request_status = frame_ipc_enabled ? "queued" : "not_enabled";
        } else {
            yolo_status = "zero_detections";
            frame_ipc_request_status = frame_ipc_enabled
                ? "not_requested_zero_detections"
                : "not_enabled";
        }

        if (!skip_cpu_results && frame_ipc && entry->has_detections) {
            if (frame_ipc && frame_ipc->isEnabled()) {
                // Convert detections to shaman format for IPC
                std::vector<shaman::Object> shaman_objects = conv_shaman(entry->detections);

                // Update the frame with detection data
                frame_ipc_update_requested =
                    frame_ipc->updateFrameWithDetections(frame_id_for_ipc, std::move(shaman_objects));
                if (!frame_ipc_update_requested) {
                    frame_ipc_request_status = "not_enabled";
                }
            }
        }
#if YOLO_PROFILE
        const auto ipc_end = std::chrono::steady_clock::now();
        ms_ipc = std::chrono::duration<double, std::milli>(ipc_end - ipc_start).count();
#endif

        // Handle ENet sending if configured
#if YOLO_PROFILE
        const auto enet_start = std::chrono::steady_clock::now();
#endif
        if (!skip_cpu_results && enet_target_peer_ && associated_camera_select_->send_yolo_via_enet && !entry->detections.empty()) {
            // ENet code remains unchanged
        }
#if YOLO_PROFILE
        const auto enet_end = std::chrono::steady_clock::now();
        ms_enet = std::chrono::duration<double, std::milli>(enet_end - enet_start).count();
#endif

        if (event_logger_ && !entry->recording_folder.empty()) {
            yolo_event_log::YoloResultRecord record;
            record.recording_folder = entry->recording_folder;
            record.status = std::move(yolo_status);
            record.error = std::move(yolo_error);
            record.local_frame_id = entry->frame_id;
            record.camera_frame_id = entry->camera_frame_id != 0
                ? entry->camera_frame_id
                : entry->frame_id;
            record.recording_frame_id = entry->recording_frame_id;
            record.ipc_frame_id = frame_id_for_ipc;
            record.record_active = entry->recording_frame_id > 0;
            record.camera_timestamp = entry->timestamp;
            record.timestamp_sys_ns = entry->timestamp_sys;
            record.event_epoch_us = get_epoch_time_us();
            record.event_monotonic_us = get_steady_time_us();
            record.gpu_id = associated_camera_params_->gpu_id;
            record.engine_path = associated_camera_select_->yolo_model
                ? associated_camera_select_->yolo_model
                : "";
            record.detections = entry->detections;
            record.queue_name = frame_ipc_queue_name;
            record.ipc_enabled = frame_ipc_enabled;
            record.ipc_requested = frame_ipc_update_requested;
            record.ipc_request_status = std::move(frame_ipc_request_status);
            event_logger_->Enqueue(std::move(record));
        }

        const auto cpu_end = std::chrono::steady_clock::now();
        ms_total = std::chrono::duration<double, std::milli>(cpu_end - cpu_start).count();
        ms_cpu_post_sync = std::chrono::duration<double, std::milli>(cpu_end - sync_wait_end).count();
#if YOLO_PROFILE
        prof_count++;
        if (prof_count % kYoloProfileLogEvery == 0) {
            std::cout << "[YOLO_TIME] " << threadName
                      << " wait=" << ms_wait << "ms"
                      << " pre=" << ms_pre << "ms"
                      << " gap=" << ms_gap << "ms"
                      << " enqueue=" << ms_enqueue << "ms"
                      << " infer=" << ms_infer << "ms"
                      << " sync=" << ms_sync_wait << "ms"
                      << " cpu_wait=" << ms_cpu_wait_event << "ms"
                      << " cpu_npp_set=" << ms_cpu_npp_set_stream << "ms"
                      << " cpu_pre=" << ms_cpu_preprocess << "ms"
                      << " cpu_dump=" << ms_cpu_dump << "ms"
                      << " cpu_infer=" << ms_cpu_infer_call << "ms"
                      << " cpu_event=" << ms_cpu_event_record << "ms"
                      << " cpu_pre_sync=" << ms_cpu_pre_sync << "ms"
                      << " cpu_pre_other=" << ms_cpu_pre_sync_other << "ms"
                      << " cpu_post_sync=" << ms_cpu_post_sync << "ms"
                      << " queue=" << ms_queue << "ms"
                      << " post=" << ms_post << "ms"
                      << " track=" << ms_track << "ms"
                      << " ipc=" << ms_ipc << "ms"
                      << " enet=" << ms_enet << "ms"
                      << " total=" << ms_total << "ms"
                      << std::endl;
        }
#endif

        if (perf_logger_ && !perf_log_folder_.empty()) {
            perf_sample_counter_++;
            if (perf_sample_counter_ % static_cast<uint64_t>(perf_sample_rate_) == 0) {
                yolo_perf::YoloPerfRecord record;
                record.frame_id = entry->frame_id;
                record.recording_frame_id = entry->recording_frame_id;
                record.timestamp = entry->timestamp;
                record.timestamp_sys = entry->timestamp_sys;
                record.queue_depth = GetCountQueueInSize();
                record.fps = current_fps_.load(std::memory_order_relaxed);
                record.ok = finished_in_time ? 1 : 0;
                record.wait_ms = ms_wait;
                record.pre_ms = ms_pre;
                record.gap_ms = ms_gap;
                record.enqueue_ms = ms_enqueue;
                record.infer_ms = ms_infer;
                record.sync_ms = ms_sync_wait;
                record.cpu_wait_event_ms = ms_cpu_wait_event;
                record.cpu_npp_set_stream_ms = ms_cpu_npp_set_stream;
                record.cpu_preprocess_ms = ms_cpu_preprocess;
                record.cpu_dump_ms = ms_cpu_dump;
                record.cpu_infer_call_ms = ms_cpu_infer_call;
                record.cpu_event_record_ms = ms_cpu_event_record;
                record.cpu_pre_sync_ms = ms_cpu_pre_sync;
                record.cpu_pre_sync_other_ms = ms_cpu_pre_sync_other;
                record.cpu_post_sync_ms = ms_cpu_post_sync;
                record.queue_ms = ms_queue;
                record.post_ms = ms_post;
                record.track_ms = ms_track;
                record.ipc_ms = ms_ipc;
                record.enet_ms = ms_enet;
                record.total_ms = ms_total;
                perf_logger_->Enqueue(record);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[" << threadName << "] Exception in WorkerFunction: " << e.what() << std::endl;
        if (yolov8_instance_ && yolov8_instance_->stream) {
            cudaStreamSynchronize(yolov8_instance_->stream);
        }
    }

    // Reference counting for recycling the WORKER_ENTRY.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
            EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
        }
        m_recycle_queue.push(entry);
    }

    // This worker doesn't pass an item to its own output queue so we return false
    return false;
}

void YoloWorker::WorkerReset() {
    last_fps_update_time_ = std::chrono::steady_clock::now();
    frame_counter_ = 0;
    current_fps_ = 0.0;
    perf_sample_counter_ = 0;
}
