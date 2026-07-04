#include "pose_worker.h"

#include "common.hpp"
#include "frame_ipc_manager.h"
#include "fsuid_guard.h"
#include "NvInferPlugin.h"
#include "optimized_yolo_preprocess.h"
#include "project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
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

void release_pose_crop_lease_after_stream_noexcept(
    CropFrameLease& crop_frame_lease,
    cudaStream_t stream,
    const char* worker_name)
{
    if (!crop_frame_lease) {
        return;
    }

    const CropFrame* crop_frame = crop_frame_lease.get();
    try {
        crop_frame_lease.ReleaseAfterStream(stream);
    } catch (const std::exception& ex) {
        std::cerr << "[PoseWorker] Failed to defer CropFrame release"
                  << " worker=" << (worker_name ? worker_name : "unknown")
                  << " frame=" << (crop_frame ? crop_frame->frame.local_frame_id : 0)
                  << " recording_frame="
                  << (crop_frame ? crop_frame->frame.recording_frame_id : 0)
                  << ": " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "[PoseWorker] Failed to defer CropFrame release"
                  << " worker=" << (worker_name ? worker_name : "unknown")
                  << " with unknown exception." << std::endl;
    }
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

std::once_flag g_pose_trt_plugins_once;

void initialize_pose_trt_plugins()
{
    std::call_once(g_pose_trt_plugins_once, []() {
        Logger logger(nvinfer1::ILogger::Severity::kWARNING);
        initLibNvInferPlugins(&logger, "");
        std::cout << "[PoseWorker] TensorRT plugins initialized." << std::endl;
    });
}

std::vector<char> read_binary_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Pose TensorRT: cannot open engine file: " + path);
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Pose TensorRT: empty engine file: " + path);
    }
    file.seekg(0, std::ios::beg);
    std::vector<char> data(static_cast<size_t>(size));
    file.read(data.data(), size);
    if (!file) {
        throw std::runtime_error("Pose TensorRT: failed to read engine file: " + path);
    }
    return data;
}

bool dims_has_dynamic_extent(const nvinfer1::Dims& dims)
{
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] < 0) {
            return true;
        }
    }
    return false;
}

std::string dims_to_string(const nvinfer1::Dims& dims)
{
    std::ostringstream oss;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (i > 0) {
            oss << 'x';
        }
        oss << dims.d[i];
    }
    return oss.str();
}

float clamp_unit(float value)
{
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::max(0.0f, std::min(1.0f, value));
}

std::vector<std::string> default_pose_keypoint_labels(size_t keypoint_count)
{
    if (keypoint_count == 3) {
        return {"bladder", "eye_left", "eye_right"};
    }
    std::vector<std::string> labels;
    labels.reserve(keypoint_count);
    for (size_t i = 0; i < keypoint_count; ++i) {
        labels.push_back("keypoint_" + std::to_string(i));
    }
    return labels;
}

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

int env_int_or_default(const char* name, int default_value, int min_value, int max_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || (end && *end != '\0')) {
        std::cerr << "[PoseWorker] Ignoring invalid " << name << "='" << raw << "'"
                  << std::endl;
        return default_value;
    }
    if (parsed < min_value || parsed > max_value) {
        std::cerr << "[PoseWorker] Ignoring out-of-range " << name << "=" << parsed
                  << " (expected " << min_value << "-" << max_value << ")"
                  << std::endl;
        return default_value;
    }
    return static_cast<int>(parsed);
}

}  // namespace

class TensorRtPoseBackend {
public:
    TensorRtPoseBackend(const std::string& engine_path, int gpu_id, cudaStream_t stream)
        : engine_path_(engine_path),
          gpu_id_(gpu_id),
          stream_(stream)
    {
        if (engine_path_.empty()) {
            throw std::runtime_error("Pose TensorRT: engine path is empty");
        }
        if (!stream_) {
            throw std::runtime_error("Pose TensorRT: CUDA stream is null");
        }

        ck(cudaSetDevice(gpu_id_));
        initialize_pose_trt_plugins();
        const std::vector<char> engine_bytes = read_binary_file(engine_path_);
        runtime_ = nvinfer1::createInferRuntime(logger_);
        if (!runtime_) {
            throw std::runtime_error("Pose TensorRT: failed to create runtime");
        }
        engine_ = runtime_->deserializeCudaEngine(engine_bytes.data(), engine_bytes.size());
        if (!engine_) {
            throw std::runtime_error("Pose TensorRT: failed to deserialize engine");
        }
        context_ = engine_->createExecutionContext();
        if (!context_) {
            throw std::runtime_error("Pose TensorRT: failed to create execution context");
        }

        bind_metadata();
        allocate_buffers();
        bind_tensors();

        std::cout << "[PoseWorker] TensorRT pose backend loaded."
                  << " engine=" << engine_path_
                  << " gpu=" << gpu_id_
                  << " input=" << input_name_ << ":" << dims_to_string(input_dims_)
                  << " output=" << output_name_ << ":" << dims_to_string(output_dims_)
                  << " keypoints=" << keypoint_count_
                  << std::endl;
    }

    ~TensorRtPoseBackend()
    {
        if (stream_) {
            cudaStreamSynchronize(stream_);
        }
        if (d_input_) {
            cudaFree(d_input_);
        }
        if (d_output_) {
            cudaFree(d_output_);
        }
        if (h_output_) {
            cudaFreeHost(h_output_);
        }
        if (context_) {
            delete context_;
        }
        if (engine_) {
            delete engine_;
        }
        if (runtime_) {
            delete runtime_;
        }
    }

    void Warmup(int iterations)
    {
        if (iterations <= 0) {
            return;
        }
        ck(cudaSetDevice(gpu_id_));
        std::cout << "[PoseWorker] TensorRT pose prewarm starting. iterations="
                  << iterations << std::endl;
        for (int i = 0; i < iterations; ++i) {
            ck(cudaMemsetAsync(d_input_, 0, input_bytes_, stream_));
            if (!context_->enqueueV3(stream_)) {
                throw std::runtime_error("Pose TensorRT: prewarm enqueue failed");
            }
            ck(cudaMemcpyAsync(
                h_output_,
                d_output_,
                output_bytes_,
                cudaMemcpyDeviceToHost,
                stream_));
            ck(cudaStreamSynchronize(stream_));
        }
        std::cout << "[PoseWorker] TensorRT pose prewarm complete." << std::endl;
    }

    void infer(const CropFrame& crop_frame,
               std::string* status_out,
               std::string* error_out,
               std::vector<pose_event_log::PoseInstanceRecord>* poses_out)
    {
        if (!status_out || !error_out || !poses_out) {
            throw std::runtime_error("Pose TensorRT: null output pointer");
        }
        *status_out = "no_result";
        error_out->clear();
        poses_out->clear();

        if (!crop_frame.d_crop_mono) {
            *status_out = "failed";
            *error_out = "missing_crop_buffer";
            return;
        }

        ck(cudaSetDevice(gpu_id_));
        launch_optimized_yolo_preprocess(
            crop_frame.d_crop_mono,
            static_cast<float*>(d_input_),
            crop_frame.frame.crop_w,
            crop_frame.frame.crop_h,
            input_width_,
            input_height_,
            false,
            stream_);

        if (!context_->enqueueV3(stream_)) {
            *status_out = "failed";
            *error_out = "enqueue_failed";
            return;
        }

        ck(cudaMemcpyAsync(
            h_output_,
            d_output_,
            output_bytes_,
            cudaMemcpyDeviceToHost,
            stream_));
        ck(cudaStreamSynchronize(stream_));

        decode_best_pose(crop_frame, status_out, poses_out);
    }

    int input_width() const { return input_width_; }
    int input_height() const { return input_height_; }
    int keypoint_count() const { return static_cast<int>(keypoint_count_); }

private:
    void bind_metadata()
    {
        const int tensor_count = engine_->getNbIOTensors();
        for (int i = 0; i < tensor_count; ++i) {
            const char* name = engine_->getIOTensorName(i);
            if (!name) {
                continue;
            }
            const nvinfer1::TensorIOMode mode = engine_->getTensorIOMode(name);
            if (mode == nvinfer1::TensorIOMode::kINPUT) {
                input_name_ = name;
            } else if (mode == nvinfer1::TensorIOMode::kOUTPUT) {
                output_name_ = name;
            }
        }

        if (input_name_.empty() || output_name_.empty()) {
            throw std::runtime_error("Pose TensorRT: expected one input and one output tensor");
        }

        input_dtype_ = engine_->getTensorDataType(input_name_.c_str());
        output_dtype_ = engine_->getTensorDataType(output_name_.c_str());
        if (input_dtype_ != nvinfer1::DataType::kFLOAT) {
            throw std::runtime_error(
                "Pose TensorRT: only FP32 input tensors are supported by the current crop preprocess path");
        }
        if (output_dtype_ != nvinfer1::DataType::kFLOAT) {
            throw std::runtime_error(
                "Pose TensorRT: only FP32 output tensors are supported by the current decoder");
        }

        input_dims_ = engine_->getTensorShape(input_name_.c_str());
        if (dims_has_dynamic_extent(input_dims_)) {
            input_dims_ = engine_->getProfileShape(
                input_name_.c_str(),
                0,
                nvinfer1::OptProfileSelector::kOPT);
            if (!context_->setInputShape(input_name_.c_str(), input_dims_)) {
                throw std::runtime_error("Pose TensorRT: failed to set dynamic input shape");
            }
        }
        if (input_dims_.nbDims != 4 || input_dims_.d[0] != 1 || input_dims_.d[1] != 3) {
            throw std::runtime_error(
                "Pose TensorRT: expected NCHW input shape 1x3xHxW, got " +
                dims_to_string(input_dims_));
        }
        input_height_ = input_dims_.d[2];
        input_width_ = input_dims_.d[3];
        input_size_ = static_cast<size_t>(get_size_by_dims(input_dims_));
        if (input_height_ <= 0 || input_width_ <= 0 || input_size_ == 0) {
            throw std::runtime_error("Pose TensorRT: invalid input shape " +
                                     dims_to_string(input_dims_));
        }

        output_dims_ = context_->getTensorShape(output_name_.c_str());
        if (dims_has_dynamic_extent(output_dims_)) {
            output_dims_ = engine_->getTensorShape(output_name_.c_str());
        }
        if (output_dims_.nbDims != 3 || output_dims_.d[0] != 1) {
            throw std::runtime_error(
                "Pose TensorRT: expected output shape 1xCxN, got " +
                dims_to_string(output_dims_));
        }
        output_channels_ = output_dims_.d[1];
        output_candidates_ = output_dims_.d[2];
        if (output_channels_ < 8 || output_candidates_ <= 0 ||
            ((output_channels_ - 5) % 3) != 0) {
            throw std::runtime_error(
                "Pose TensorRT: expected YOLO-pose output 1x(5+3K)xN, got " +
                dims_to_string(output_dims_));
        }
        keypoint_count_ = static_cast<size_t>((output_channels_ - 5) / 3);
        keypoint_labels_ = default_pose_keypoint_labels(keypoint_count_);
        output_size_ = static_cast<size_t>(get_size_by_dims(output_dims_));
    }

    void allocate_buffers()
    {
        input_bytes_ = input_size_ * type_to_size(input_dtype_);
        output_bytes_ = output_size_ * type_to_size(output_dtype_);
        ck(cudaMalloc(&d_input_, input_bytes_));
        ck(cudaMalloc(&d_output_, output_bytes_));
        ck(cudaHostAlloc(reinterpret_cast<void**>(&h_output_), output_bytes_, 0));
    }

    void bind_tensors()
    {
        if (!context_->setTensorAddress(input_name_.c_str(), d_input_)) {
            throw std::runtime_error("Pose TensorRT: failed to bind input tensor");
        }
        if (!context_->setTensorAddress(output_name_.c_str(), d_output_)) {
            throw std::runtime_error("Pose TensorRT: failed to bind output tensor");
        }
    }

    float output_at(int channel, int candidate) const
    {
        return h_output_[static_cast<size_t>(channel) *
                         static_cast<size_t>(output_candidates_) +
                         static_cast<size_t>(candidate)];
    }

    void decode_best_pose(const CropFrame& crop_frame,
                          std::string* status_out,
                          std::vector<pose_event_log::PoseInstanceRecord>* poses_out) const
    {
        int best_candidate = -1;
        float best_score = confidence_threshold_;
        for (int candidate = 0; candidate < output_candidates_; ++candidate) {
            const float score = output_at(4, candidate);
            if (std::isfinite(score) && score > best_score) {
                best_score = score;
                best_candidate = candidate;
            }
        }

        if (best_candidate < 0) {
            *status_out = "no_result";
            return;
        }

        const float ratio = std::min(
            static_cast<float>(input_width_) / static_cast<float>(std::max(1, crop_frame.frame.crop_w)),
            static_cast<float>(input_height_) / static_cast<float>(std::max(1, crop_frame.frame.crop_h)));
        const float dw = (static_cast<float>(input_width_) -
                          static_cast<float>(crop_frame.frame.crop_w) * ratio) * 0.5f;
        const float dh = (static_cast<float>(input_height_) -
                          static_cast<float>(crop_frame.frame.crop_h) * ratio) * 0.5f;

        pose_event_log::PoseInstanceRecord pose;
        pose.index = 0;
        pose.label = "fish";
        pose.confidence = clamp_unit(best_score);
        pose.keypoints.reserve(keypoint_count_);
        for (size_t k = 0; k < keypoint_count_; ++k) {
            const int base = 5 + static_cast<int>(k) * 3;
            const float model_x = output_at(base, best_candidate);
            const float model_y = output_at(base + 1, best_candidate);
            const float keypoint_score = output_at(base + 2, best_candidate);
            pose_event_log::PoseKeypointRecord keypoint;
            keypoint.label = keypoint_labels_[k];
            keypoint.x_px = clamp(
                (model_x - dw) / std::max(1.0f, ratio),
                0.0f,
                static_cast<float>(std::max(1, crop_frame.frame.crop_w)));
            keypoint.y_px = clamp(
                (model_y - dh) / std::max(1.0f, ratio),
                0.0f,
                static_cast<float>(std::max(1, crop_frame.frame.crop_h)));
            keypoint.confidence = clamp_unit(keypoint_score);
            keypoint.visible = keypoint.confidence >= keypoint_confidence_threshold_;
            pose.keypoints.push_back(keypoint);
        }

        poses_out->push_back(std::move(pose));
        *status_out = "poses";
    }

    std::string engine_path_;
    int gpu_id_ = -1;
    cudaStream_t stream_ = nullptr;
    Logger logger_{nvinfer1::ILogger::Severity::kERROR};
    nvinfer1::IRuntime* runtime_ = nullptr;
    nvinfer1::ICudaEngine* engine_ = nullptr;
    nvinfer1::IExecutionContext* context_ = nullptr;
    std::string input_name_;
    std::string output_name_;
    nvinfer1::DataType input_dtype_ = nvinfer1::DataType::kFLOAT;
    nvinfer1::DataType output_dtype_ = nvinfer1::DataType::kFLOAT;
    nvinfer1::Dims input_dims_{};
    nvinfer1::Dims output_dims_{};
    int input_width_ = 0;
    int input_height_ = 0;
    int output_channels_ = 0;
    int output_candidates_ = 0;
    size_t keypoint_count_ = 0;
    size_t input_size_ = 0;
    size_t output_size_ = 0;
    size_t input_bytes_ = 0;
    size_t output_bytes_ = 0;
    void* d_input_ = nullptr;
    void* d_output_ = nullptr;
    float* h_output_ = nullptr;
    std::vector<std::string> keypoint_labels_;
    float confidence_threshold_ = 0.25f;
    float keypoint_confidence_threshold_ = 0.25f;
};

PoseWorker::PoseWorker(const char* name,
                       CameraParams* camera_params,
                       CropProducer* crop_producer,
                       FrameIPCManager* frame_ipc_manager)
    : CThreadWorker<CropFrame>(name),
      camera_params_(camera_params),
      crop_producer_(crop_producer),
      frame_ipc_manager_(frame_ipc_manager),
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
    if (const char* mode = std::getenv("ORANGE_POSE_MODE")) {
        if (std::strcmp(mode, "real") == 0) {
            pose_mode_ = "real";
            pose_backend_ = "tensorrt";
        } else if (std::strcmp(mode, "noop") == 0) {
            pose_mode_ = "noop";
            pose_backend_ = "noop";
        }
    } else if (!pose_engine_path_.empty()) {
        pose_mode_ = "real";
        pose_backend_ = "tensorrt";
    }
    if (const char* skeleton_path = std::getenv("ORANGE_POSE_SKELETON_PATH")) {
        pose_skeleton_path_ = skeleton_path;
    }
    if (const char* skeleton_id = std::getenv("ORANGE_POSE_SKELETON_ID")) {
        pose_skeleton_id_ = skeleton_id;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
    if (pose_mode_ == "real") {
        tensorrt_backend_ = std::make_unique<TensorRtPoseBackend>(
            pose_engine_path_,
            camera_params_->gpu_id,
            stream_);
        pose_prewarm_iterations_ =
            env_int_or_default("ORANGE_POSE_PREWARM_ITERATIONS", 0, 0, 1000);
        tensorrt_backend_->Warmup(pose_prewarm_iterations_);
    }
}

PoseWorker::~PoseWorker()
{
    CloseRecording();

    if (camera_params_) {
        ck(cudaSetDevice(camera_params_->gpu_id));
    }
    tensorrt_backend_.reset();
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

bool PoseWorker::TryEnqueueCrop(CropFrameLease crop_frame_lease)
{
    CropFrame* crop_frame = crop_frame_lease.get();
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

    if (!PutObjectToQueueIn(crop_frame)) {
        std::cerr << "[PoseWorker] enqueue rejected after stop"
                  << " worker=" << threadName
                  << " frame=" << crop_frame->frame.local_frame_id
                  << " recording_frame=" << crop_frame->frame.recording_frame_id
                  << std::endl;
        return false;
    }
    crop_frame_lease.Transfer();
    frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (record_active) {
        run_frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool PoseWorker::WorkerFunction(CropFrame* crop_frame)
{
    if (!crop_frame) {
        return false;  // defensive; flush ticks arrive via OnFlushTick()
    }

    CropFrameLease crop_frame_lease(
        crop_producer_,
        crop_frame,
        CropFrameLease::RetainMode::AdoptExisting);

    try {
        ck(cudaSetDevice(camera_params_->gpu_id));
        if (crop_frame->crop_ready_event) {
            ck(cudaStreamWaitEvent(stream_, crop_frame->crop_ready_event, 0));
        }
        const uint64_t pose_start_host_ns = steady_now_ns();
        std::string pose_status = "no_result";
        std::string pose_error;
        std::vector<pose_event_log::PoseInstanceRecord> poses;
        if (tensorrt_backend_) {
            try {
                tensorrt_backend_->infer(
                    *crop_frame,
                    &pose_status,
                    &pose_error,
                    &poses);
            } catch (const std::exception& ex) {
                pose_status = "failed";
                pose_error = ex.what();
                std::cerr << "[PoseWorker] TensorRT pose inference failed for "
                          << threadName << ": " << ex.what() << std::endl;
            }
        }

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
                pose_done_host_ns,
                pose_status,
                pose_error,
                poses));
        }
        publish_pose_result_v2(crop_frame->frame, pose_status, poses);

        crop_frame_lease.ReleaseAfterStream(stream_);
    } catch (...) {
        release_pose_crop_lease_after_stream_noexcept(
            crop_frame_lease,
            stream_,
            threadName);
        throw;
    }
    return false;
}

pose_event_log::PoseResultRecord PoseWorker::build_pose_event_record(
    const CropFrameSnapshot& frame,
    uint64_t pose_start_host_ns,
    uint64_t pose_done_host_ns,
    const std::string& status,
    const std::string& error,
    const std::vector<pose_event_log::PoseInstanceRecord>& poses) const
{
    pose_event_log::PoseResultRecord record;
    record.recording_folder = frame.recording_folder;
    record.status = status;
    record.error = error;
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
    record.poses = poses;
    return record;
}

void PoseWorker::publish_pose_result_v2(
    const CropFrameSnapshot& frame,
    const std::string& status,
    const std::vector<pose_event_log::PoseInstanceRecord>& poses)
{
    if (!frame_ipc_manager_ || !frame_ipc_manager_->isV2Enabled()) {
        return;
    }

    const uint64_t frame_id =
        frame.recording_frame_id > 0 ? frame.recording_frame_id : frame.local_frame_id;
    if (frame_id == 0) {
        return;
    }

    shaman_v2::Slot slot;
    slot.state_frame_id = frame_id;
    slot.source_frame_id = frame_id;
    slot.camera_frame_id = frame.camera_frame_id;
    slot.recording_frame_id = frame.recording_frame_id;
    slot.camera_timestamp_ns = frame.timestamp;
    slot.timestamp_sys_ns = frame.timestamp_sys;
    slot.camera_id = camera_params_ ? static_cast<uint32_t>(camera_params_->camera_id) : 0;
    if (camera_params_) {
        shaman_v2::copy_camera_serial(slot.camera_serial, camera_params_->camera_serial);
    }
    slot.source_width_px = static_cast<uint32_t>(std::max(0, frame.source_width));
    slot.source_height_px = static_cast<uint32_t>(std::max(0, frame.source_height));
    slot.detection_status = frame.has_detection
        ? static_cast<uint32_t>(shaman_v2::DetectionStatus::kDetections)
        : static_cast<uint32_t>(shaman_v2::DetectionStatus::kNotScheduled);
    if (status == "poses") {
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kPoses);
    } else if (status == "failed") {
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kFailed);
    } else {
        slot.pose_status = static_cast<uint32_t>(shaman_v2::PoseStatus::kNoResult);
    }
    slot.pose_model_id_hash = fnv1a64(pose_model_id_);
    slot.pose_skeleton_id_hash = fnv1a64(pose_skeleton_id_);

    const bool publish_detection_bbox = frame.has_detection &&
        frame.detection_w > 0.0f && frame.detection_h > 0.0f;
    const size_t object_count = !poses.empty()
        ? poses.size()
        : (publish_detection_bbox ? 1U : 0U);
    slot.object_count = static_cast<uint32_t>(
        std::min<size_t>(object_count, shaman_v2::kMaxObjects));

    for (uint32_t object_index = 0; object_index < slot.object_count; ++object_index) {
        const pose_event_log::PoseInstanceRecord* pose =
            object_index < poses.size() ? &poses[object_index] : nullptr;
        shaman_v2::Object& object = slot.objects[object_index];
        object.x_px = frame.detection_x;
        object.y_px = frame.detection_y;
        object.width_px = frame.detection_w;
        object.height_px = frame.detection_h;
        object.confidence = pose ? static_cast<float>(pose->confidence) : frame.detection_confidence;
        object.label_id = 0;
        object.track_id = -1;
        object.flags = publish_detection_bbox ? static_cast<uint32_t>(shaman_v2::kObjectHasBbox) : 0u;
        if (pose) {
            object.flags |= shaman_v2::kObjectHasPose;
            object.keypoint_count = static_cast<uint32_t>(
                std::min<size_t>(pose->keypoints.size(), shaman_v2::kMaxKeypointsPerObject));
            for (uint32_t keypoint_index = 0; keypoint_index < object.keypoint_count; ++keypoint_index) {
                const pose_event_log::PoseKeypointRecord& keypoint =
                    pose->keypoints[keypoint_index];
                object.keypoints[keypoint_index].x_px =
                    static_cast<float>(frame.crop_x + keypoint.x_px);
                object.keypoints[keypoint_index].y_px =
                    static_cast<float>(frame.crop_y + keypoint.y_px);
                object.keypoints[keypoint_index].confidence =
                    static_cast<float>(keypoint.confidence);
                object.keypoints[keypoint_index].label_id =
                    static_cast<uint16_t>(keypoint_index);
                object.keypoints[keypoint_index].flags =
                    keypoint.visible ? shaman_v2::kKeypointVisible : 0;
            }
        }
    }

    (void)frame_ipc_manager_->updateFrameWithPoseResult(std::move(slot));
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
              << "PoseWorker," << pose_backend_ << ',' << pose_mode_ << ','
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
