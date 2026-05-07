#include "pose_worker.h"

#include "fsuid_guard.h"
#include "project.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

uint64_t steady_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

double elapsed_ms(uint64_t start_ns, uint64_t end_ns)
{
    if (start_ns == 0 || end_ns < start_ns) {
        return 0.0;
    }
    return static_cast<double>(end_ns - start_ns) / 1000000.0;
}

struct LatencySummary {
    size_t count = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

double percentile_from_sorted(const std::vector<double>& sorted_samples, double percentile)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    if (sorted_samples.size() == 1) {
        return sorted_samples.front();
    }

    const double clamped = std::min(1.0, std::max(0.0, percentile));
    const double idx = clamped * static_cast<double>(sorted_samples.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(sorted_samples.size() - 1, lo + 1);
    const double frac = idx - static_cast<double>(lo);
    return sorted_samples[lo] + (sorted_samples[hi] - sorted_samples[lo]) * frac;
}

LatencySummary summarize_latency(const std::vector<double>& samples)
{
    LatencySummary summary;
    if (samples.empty()) {
        return summary;
    }

    std::vector<double> sorted_samples = samples;
    std::sort(sorted_samples.begin(), sorted_samples.end());

    double sum = 0.0;
    for (double sample : sorted_samples) {
        sum += sample;
    }

    summary.count = sorted_samples.size();
    summary.mean_ms = sum / static_cast<double>(summary.count);
    summary.p50_ms = percentile_from_sorted(sorted_samples, 0.50);
    summary.p95_ms = percentile_from_sorted(sorted_samples, 0.95);
    summary.p99_ms = percentile_from_sorted(sorted_samples, 0.99);
    summary.max_ms = sorted_samples.back();
    return summary;
}

}  // namespace

PoseWorker::PoseWorker(const char* name, CameraParams* camera_params, CropProducer* crop_producer)
    : CThreadWorker<CropFrame>(name),
      camera_params_(camera_params),
      crop_producer_(crop_producer),
      pose_event_logger_(
          camera_params ? camera_params->camera_serial : std::string(),
          camera_params ? camera_params->camera_id : 0,
          name ? name : "PoseWorker")
{
    if (const char* engine_path = std::getenv("ORANGE_POSE_ENGINE_PATH")) {
        pose_engine_path_ = engine_path;
        if (!pose_engine_path_.empty()) {
            pose_model_id_ = build_model_id_from_path(pose_engine_path_);
        }
    }
    if (const char* skeleton_path = std::getenv("ORANGE_POSE_SKELETON_PATH")) {
        pose_skeleton_path_ = skeleton_path;
    }
    if (const char* skeleton_id = std::getenv("ORANGE_POSE_SKELETON_ID")) {
        pose_skeleton_id_ = skeleton_id;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
}

PoseWorker::~PoseWorker()
{
    CloseRecording();

    if (camera_params_) {
        ck(cudaSetDevice(camera_params_->gpu_id));
    }
    if (stream_) {
        cudaStreamSynchronize(stream_);
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }

    std::cout << "[PoseWorker] Summary for " << threadName
              << " enqueued=" << frames_enqueued_.load(std::memory_order_relaxed)
              << " processed=" << frames_processed_.load(std::memory_order_relaxed)
              << " queue_full_drops=" << queue_full_drops_.load(std::memory_order_relaxed)
              << " queue_high_water=" << queue_high_water_.load(std::memory_order_relaxed)
              << std::endl;
}

void PoseWorker::SetMaxQueueSize(int size)
{
    max_queue_size_ = std::max(1, size);
    CThreadWorker<CropFrame>::SetMaxQueueSize(max_queue_size_);
}

void PoseWorker::RotateRecordingFolder(const std::string& recording_folder)
{
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (current_recording_folder_ == recording_folder) {
        return;
    }

    if (!current_recording_folder_.empty()) {
        write_recording_summary_locked();
        pose_event_logger_.Close();
        reset_run_counters();
    }

    current_recording_folder_.clear();
    pose_perf_file_.clear();

    if (recording_folder.empty()) {
        return;
    }

    current_recording_folder_ = recording_folder;
    pose_perf_file_ =
        current_recording_folder_ + "/Cam" + camera_params_->camera_serial + "_pose_perf.csv";

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    make_folder(current_recording_folder_);

    std::ofstream pose_perf(pose_perf_file_.c_str(), std::ios::out | std::ios::trunc);
    if (!pose_perf) {
        std::cerr << "[PoseWorker] Warning: Could not open pose perf file for "
                  << threadName << ": " << pose_perf_file_ << std::endl;
        return;
    }

    pose_perf << "camera_serial,gpu_id,worker,backend,mode,queue_size,"
                 "frames_enqueued,frames_processed,queue_full_drops,queue_high_water,"
                 "capture_to_detect_done_count,capture_to_detect_done_mean_ms,capture_to_detect_done_p50_ms,"
                 "capture_to_detect_done_p95_ms,capture_to_detect_done_p99_ms,capture_to_detect_done_max_ms,"
                 "detect_to_crop_worker_start_count,detect_to_crop_worker_start_mean_ms,detect_to_crop_worker_start_p50_ms,"
                 "detect_to_crop_worker_start_p95_ms,detect_to_crop_worker_start_p99_ms,detect_to_crop_worker_start_max_ms,"
                 "crop_worker_start_to_crop_ready_count,crop_worker_start_to_crop_ready_mean_ms,crop_worker_start_to_crop_ready_p50_ms,"
                 "crop_worker_start_to_crop_ready_p95_ms,crop_worker_start_to_crop_ready_p99_ms,crop_worker_start_to_crop_ready_max_ms,"
                 "detect_to_crop_ready_count,detect_to_crop_ready_mean_ms,detect_to_crop_ready_p50_ms,"
                 "detect_to_crop_ready_p95_ms,detect_to_crop_ready_p99_ms,detect_to_crop_ready_max_ms,"
                 "crop_ready_to_pose_start_count,crop_ready_to_pose_start_mean_ms,crop_ready_to_pose_start_p50_ms,"
                 "crop_ready_to_pose_start_p95_ms,crop_ready_to_pose_start_p99_ms,crop_ready_to_pose_start_max_ms,"
                 "pose_start_to_pose_done_count,pose_start_to_pose_done_mean_ms,pose_start_to_pose_done_p50_ms,"
                 "pose_start_to_pose_done_p95_ms,pose_start_to_pose_done_p99_ms,pose_start_to_pose_done_max_ms,"
                 "capture_to_pose_done_count,capture_to_pose_done_mean_ms,capture_to_pose_done_p50_ms,"
                 "capture_to_pose_done_p95_ms,capture_to_pose_done_p99_ms,capture_to_pose_done_max_ms\n";
}

void PoseWorker::CloseRecording()
{
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (current_recording_folder_.empty()) {
        return;
    }

    write_recording_summary_locked();
    pose_event_logger_.Close();
    current_recording_folder_.clear();
    pose_perf_file_.clear();
    reset_run_counters();
}

bool PoseWorker::TryEnqueueCrop(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return false;
    }

    const bool record_active =
        crop_frame->frame.recording_frame_id > 0 && !crop_frame->frame.recording_folder.empty();
    const int queue_depth = GetCountQueueInSize();
    queue_high_water_.store(
        std::max(queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
        std::memory_order_relaxed);
    if (record_active) {
        run_queue_high_water_.store(
            std::max(run_queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
            std::memory_order_relaxed);
    }
    if (queue_depth >= max_queue_size_) {
        queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
        if (record_active) {
            run_queue_full_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (record_active) {
        run_frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    PutObjectToQueueIn(crop_frame);
    return true;
}

bool PoseWorker::WorkerFunction(CropFrame* crop_frame)
{
    if (!crop_frame) {
        CloseRecording();
        return false;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    if (crop_frame->crop_ready_event) {
        ck(cudaStreamWaitEvent(stream_, crop_frame->crop_ready_event, 0));
    }
    const uint64_t pose_start_host_ns = steady_now_ns();

    frames_processed_.fetch_add(1, std::memory_order_relaxed);
    const bool record_active =
        crop_frame->frame.recording_frame_id > 0 && !crop_frame->frame.recording_folder.empty();
    if (record_active) {
        run_frames_processed_.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t pose_done_host_ns = steady_now_ns();

    if (record_active) {
        std::lock_guard<std::mutex> lock(recording_mutex_);
        if (crop_frame->frame.acquisition_receive_host_ns > 0 &&
            crop_frame->frame.yolo_detect_done_host_ns > 0) {
            capture_to_detect_done_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.acquisition_receive_host_ns,
                crop_frame->frame.yolo_detect_done_host_ns));
        }
        if (crop_frame->frame.yolo_detect_done_host_ns > 0 &&
            crop_frame->frame.crop_producer_worker_start_host_ns > 0) {
            detect_to_crop_worker_start_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.yolo_detect_done_host_ns,
                crop_frame->frame.crop_producer_worker_start_host_ns));
        }
        if (crop_frame->frame.crop_producer_worker_start_host_ns > 0 &&
            crop_frame->frame.crop_ready_host_ns > 0) {
            crop_worker_start_to_crop_ready_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.crop_producer_worker_start_host_ns,
                crop_frame->frame.crop_ready_host_ns));
        }
        if (crop_frame->frame.yolo_detect_done_host_ns > 0 &&
            crop_frame->frame.crop_ready_host_ns > 0) {
            detect_to_crop_ready_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.yolo_detect_done_host_ns,
                crop_frame->frame.crop_ready_host_ns));
        }
        if (crop_frame->frame.crop_ready_host_ns > 0) {
            crop_ready_to_pose_start_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.crop_ready_host_ns,
                pose_start_host_ns));
        }
        pose_start_to_pose_done_samples_ms_.push_back(elapsed_ms(
            pose_start_host_ns,
            pose_done_host_ns));
        if (crop_frame->frame.acquisition_receive_host_ns > 0) {
            capture_to_pose_done_samples_ms_.push_back(elapsed_ms(
                crop_frame->frame.acquisition_receive_host_ns,
                pose_done_host_ns));
        }
    }

    if (record_active) {
        pose_event_logger_.Enqueue(build_pose_event_record(
            crop_frame->frame,
            pose_start_host_ns,
            pose_done_host_ns));
    }

    if (crop_producer_) {
        crop_producer_->RecycleAfterConsumerStream(crop_frame, stream_);
    }
    return false;
}

pose_event_log::PoseResultRecord PoseWorker::build_pose_event_record(
    const CropFrameSnapshot& frame,
    uint64_t pose_start_host_ns,
    uint64_t pose_done_host_ns) const
{
    pose_event_log::PoseResultRecord record;
    record.recording_folder = frame.recording_folder;
    record.status = "no_result";
    record.backend = pose_backend_;
    record.mode = pose_mode_;
    record.model_id = pose_model_id_;
    record.engine_path = pose_engine_path_;
    record.skeleton_id = pose_skeleton_id_;
    record.skeleton_path = pose_skeleton_path_;
    record.gpu_id = camera_params_ ? camera_params_->gpu_id : -1;

    record.local_frame_id = frame.local_frame_id;
    record.camera_frame_id = frame.camera_frame_id;
    record.recording_frame_id = frame.recording_frame_id;
    record.record_active = frame.recording_frame_id > 0 && !frame.recording_folder.empty();
    record.camera_timestamp = frame.timestamp;
    record.timestamp_sys_ns = frame.timestamp_sys;
    record.source_width = frame.source_width;
    record.source_height = frame.source_height;
    record.blank_frame = frame.blank_frame;
    record.has_detection = frame.has_detection;
    record.detection_confidence = frame.detection_confidence;
    record.detection_x_px = frame.detection_x;
    record.detection_y_px = frame.detection_y;
    record.detection_width_px = frame.detection_w;
    record.detection_height_px = frame.detection_h;
    record.crop_x_px = frame.crop_x;
    record.crop_y_px = frame.crop_y;
    record.crop_width_px = frame.crop_w;
    record.crop_height_px = frame.crop_h;

    record.timing.capture_to_detect_done_ms = elapsed_ms(
        frame.acquisition_receive_host_ns,
        frame.yolo_detect_done_host_ns);
    record.timing.detect_to_crop_worker_start_ms = elapsed_ms(
        frame.yolo_detect_done_host_ns,
        frame.crop_producer_worker_start_host_ns);
    record.timing.crop_worker_start_to_crop_ready_ms = elapsed_ms(
        frame.crop_producer_worker_start_host_ns,
        frame.crop_ready_host_ns);
    record.timing.detect_to_crop_ready_ms = elapsed_ms(
        frame.yolo_detect_done_host_ns,
        frame.crop_ready_host_ns);
    record.timing.crop_ready_to_pose_start_ms = elapsed_ms(
        frame.crop_ready_host_ns,
        pose_start_host_ns);
    record.timing.pose_start_to_pose_done_ms = elapsed_ms(
        pose_start_host_ns,
        pose_done_host_ns);
    record.timing.capture_to_pose_done_ms = elapsed_ms(
        frame.acquisition_receive_host_ns,
        pose_done_host_ns);
    return record;
}

void PoseWorker::reset_run_counters()
{
    run_frames_enqueued_.store(0, std::memory_order_relaxed);
    run_frames_processed_.store(0, std::memory_order_relaxed);
    run_queue_full_drops_.store(0, std::memory_order_relaxed);
    run_queue_high_water_.store(0, std::memory_order_relaxed);
    capture_to_detect_done_samples_ms_.clear();
    detect_to_crop_worker_start_samples_ms_.clear();
    crop_worker_start_to_crop_ready_samples_ms_.clear();
    detect_to_crop_ready_samples_ms_.clear();
    crop_ready_to_pose_start_samples_ms_.clear();
    pose_start_to_pose_done_samples_ms_.clear();
    capture_to_pose_done_samples_ms_.clear();
}

void PoseWorker::write_recording_summary_locked()
{
    if (pose_perf_file_.empty()) {
        return;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::ofstream pose_perf(pose_perf_file_.c_str(), std::ios::out | std::ios::app);
    if (!pose_perf) {
        std::cerr << "[PoseWorker] Warning: Could not append pose perf file for "
                  << threadName << ": " << pose_perf_file_ << std::endl;
        return;
    }

    const LatencySummary capture_to_detect_done =
        summarize_latency(capture_to_detect_done_samples_ms_);
    const LatencySummary detect_to_crop_worker_start =
        summarize_latency(detect_to_crop_worker_start_samples_ms_);
    const LatencySummary crop_worker_start_to_crop_ready =
        summarize_latency(crop_worker_start_to_crop_ready_samples_ms_);
    const LatencySummary detect_to_crop_ready =
        summarize_latency(detect_to_crop_ready_samples_ms_);
    const LatencySummary crop_ready_to_pose_start =
        summarize_latency(crop_ready_to_pose_start_samples_ms_);
    const LatencySummary pose_start_to_pose_done =
        summarize_latency(pose_start_to_pose_done_samples_ms_);
    const LatencySummary capture_to_pose_done =
        summarize_latency(capture_to_pose_done_samples_ms_);

    pose_perf << camera_params_->camera_serial << ','
              << camera_params_->gpu_id << ','
              << "PoseWorker,noop,noop,"
              << max_queue_size_ << ','
              << run_frames_enqueued_.load(std::memory_order_relaxed) << ','
              << run_frames_processed_.load(std::memory_order_relaxed) << ','
              << run_queue_full_drops_.load(std::memory_order_relaxed) << ','
              << run_queue_high_water_.load(std::memory_order_relaxed) << ','
              << capture_to_detect_done.count << ','
              << capture_to_detect_done.mean_ms << ','
              << capture_to_detect_done.p50_ms << ','
              << capture_to_detect_done.p95_ms << ','
              << capture_to_detect_done.p99_ms << ','
              << capture_to_detect_done.max_ms << ','
              << detect_to_crop_worker_start.count << ','
              << detect_to_crop_worker_start.mean_ms << ','
              << detect_to_crop_worker_start.p50_ms << ','
              << detect_to_crop_worker_start.p95_ms << ','
              << detect_to_crop_worker_start.p99_ms << ','
              << detect_to_crop_worker_start.max_ms << ','
              << crop_worker_start_to_crop_ready.count << ','
              << crop_worker_start_to_crop_ready.mean_ms << ','
              << crop_worker_start_to_crop_ready.p50_ms << ','
              << crop_worker_start_to_crop_ready.p95_ms << ','
              << crop_worker_start_to_crop_ready.p99_ms << ','
              << crop_worker_start_to_crop_ready.max_ms << ','
              << detect_to_crop_ready.count << ','
              << detect_to_crop_ready.mean_ms << ','
              << detect_to_crop_ready.p50_ms << ','
              << detect_to_crop_ready.p95_ms << ','
              << detect_to_crop_ready.p99_ms << ','
              << detect_to_crop_ready.max_ms << ','
              << crop_ready_to_pose_start.count << ','
              << crop_ready_to_pose_start.mean_ms << ','
              << crop_ready_to_pose_start.p50_ms << ','
              << crop_ready_to_pose_start.p95_ms << ','
              << crop_ready_to_pose_start.p99_ms << ','
              << crop_ready_to_pose_start.max_ms << ','
              << pose_start_to_pose_done.count << ','
              << pose_start_to_pose_done.mean_ms << ','
              << pose_start_to_pose_done.p50_ms << ','
              << pose_start_to_pose_done.p95_ms << ','
              << pose_start_to_pose_done.p99_ms << ','
              << pose_start_to_pose_done.max_ms << ','
              << capture_to_pose_done.count << ','
              << capture_to_pose_done.mean_ms << ','
              << capture_to_pose_done.p50_ms << ','
              << capture_to_pose_done.p95_ms << ','
              << capture_to_pose_done.p99_ms << ','
              << capture_to_pose_done.max_ms << '\n';
}
