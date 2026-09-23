#include "pose_worker.h"

#include "common.hpp"
#include "frame_ipc_manager.h"
#include "fsuid_guard.h"
#include "NvInferPlugin.h"
#include "optimized_yolo_preprocess.h"
#include "pose_crop_from_roi.h"
#include "project.h"
#include "yolo_runtime_flags.h"

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

LatencySummary summarize_latency(const orange::BoundedSampleStatistics& samples)
{
    LatencySummary summary;
    if (samples.empty()) {
        return summary;
    }

    const std::vector<double> sorted_samples = samples.sorted_retained_samples();
    summary.count = static_cast<size_t>(samples.sample_count());
    summary.mean_ms = samples.mean();
    summary.p50_ms = orange::percentile_from_sorted_samples(sorted_samples, 0.50);
    summary.p95_ms = orange::percentile_from_sorted_samples(sorted_samples, 0.95);
    summary.p99_ms = orange::percentile_from_sorted_samples(sorted_samples, 0.99);
    summary.max_ms = samples.max();
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
        if (!bound_to_defaults_) {
            bind_tensors();
        }
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

        decode_best_pose_from(h_output_, crop_frame.frame.crop_w, crop_frame.frame.crop_h,
                              status_out, poses_out);
    }

    int input_width() const { return input_width_; }
    int input_height() const { return input_height_; }
    int keypoint_count() const { return static_cast<int>(keypoint_count_); }
    size_t input_bytes() const { return input_bytes_; }
    size_t output_bytes() const { return output_bytes_; }

    // Device crop path: the same preprocess the crop-producer path runs,
    // into a caller-owned input buffer on the caller's stream.
    void preprocess_into(const unsigned char* d_mono, int crop_w, int crop_h,
                         void* d_input, cudaStream_t stream) const
    {
        launch_optimized_yolo_preprocess(
            d_mono, static_cast<float*>(d_input), crop_w, crop_h,
            input_width_, input_height_, false, stream);
    }

    // Device crop path: enqueue with per-slot buffers. Must be called from
    // the one thread that drives the context in that mode (the YOLO worker).
    bool enqueue_with_buffers(void* d_input, void* d_output, float* h_output,
                              cudaStream_t stream, std::string* error_out)
    {
        if (!context_->setTensorAddress(input_name_.c_str(), d_input) ||
            !context_->setTensorAddress(output_name_.c_str(), d_output)) {
            *error_out = "bind_failed";
            return false;
        }
        bound_to_defaults_ = false;
        if (!context_->enqueueV3(stream)) {
            *error_out = "enqueue_failed";
            return false;
        }
        ck(cudaMemcpyAsync(h_output, d_output, output_bytes_, cudaMemcpyDeviceToHost, stream));
        return true;
    }

    // Step 2: capture the pose stage (enqueueV3 + output copy) into a CUDA
    // graph bound to one slot's buffers. A captured graph bakes in the
    // addresses its kernels were launched with, which is why there is one
    // graph per slot and not one for the stage: the slots exist so frame
    // N+1's input can be written while frame N's output is still being
    // decoded, and each needs its own baked addresses. TensorRT wants one
    // ordinary enqueue with the same addresses before capture (lazy
    // allocation happens there, and would break the capture). Capture is
    // thread-local so other threads' CUDA calls do not join the graph. The
    // execution context's scratch workspace is shared by every graph made
    // from it, so the graphs must run serialised on one stream, which the
    // pose stream guarantees.
    bool capture_slot_graph(void* d_input, void* d_output, float* h_output,
                            cudaStream_t stream, cudaGraphExec_t* exec_out,
                            std::string* error_out)
    {
        *exec_out = nullptr;
        if (!context_->setTensorAddress(input_name_.c_str(), d_input) ||
            !context_->setTensorAddress(output_name_.c_str(), d_output)) {
            *error_out = "bind_failed";
            return false;
        }
        bound_to_defaults_ = false;
        ck(cudaMemsetAsync(d_input, 0, input_bytes_, stream));
        if (!context_->enqueueV3(stream)) {
            *error_out = "warm_enqueue_failed";
            return false;
        }
        ck(cudaMemcpyAsync(h_output, d_output, output_bytes_, cudaMemcpyDeviceToHost, stream));
        ck(cudaStreamSynchronize(stream));

        cudaGraph_t graph = nullptr;
        ck(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
        if (!context_->enqueueV3(stream)) {
            cudaStreamEndCapture(stream, &graph);
            if (graph) {
                cudaGraphDestroy(graph);
            }
            *error_out = "captured_enqueue_failed";
            return false;
        }
        ck(cudaMemcpyAsync(h_output, d_output, output_bytes_, cudaMemcpyDeviceToHost, stream));
        ck(cudaStreamEndCapture(stream, &graph));
        cudaGraphExec_t exec = nullptr;
        const cudaError_t inst = cudaGraphInstantiate(&exec, graph, nullptr, nullptr, 0);
        cudaGraphDestroy(graph);
        if (inst != cudaSuccess) {
            cudaGetLastError();
            *error_out = std::string("graph_instantiate_failed: ") + cudaGetErrorString(inst);
            return false;
        }
        // One un-timed launch so the first real frame does not pay the
        // graph upload.
        ck(cudaGraphLaunch(exec, stream));
        ck(cudaStreamSynchronize(stream));
        *exec_out = exec;
        return true;
    }

    void decode_from(const float* h_output, int crop_w, int crop_h,
                     std::string* status_out,
                     std::vector<pose_event_log::PoseInstanceRecord>* poses_out) const
    {
        decode_best_pose_from(h_output, crop_w, crop_h, status_out, poses_out);
    }

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
        bound_to_defaults_ = true;
    }

    float output_at(const float* h_output, int channel, int candidate) const
    {
        return h_output[static_cast<size_t>(channel) *
                        static_cast<size_t>(output_candidates_) +
                        static_cast<size_t>(candidate)];
    }

    void decode_best_pose_from(const float* h_output,
                               int crop_w,
                               int crop_h,
                               std::string* status_out,
                               std::vector<pose_event_log::PoseInstanceRecord>* poses_out) const
    {
        int best_candidate = -1;
        float best_score = confidence_threshold_;
        for (int candidate = 0; candidate < output_candidates_; ++candidate) {
            const float score = output_at(h_output, 4, candidate);
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
            static_cast<float>(input_width_) / static_cast<float>(std::max(1, crop_w)),
            static_cast<float>(input_height_) / static_cast<float>(std::max(1, crop_h)));
        const float dw = (static_cast<float>(input_width_) -
                          static_cast<float>(crop_w) * ratio) * 0.5f;
        const float dh = (static_cast<float>(input_height_) -
                          static_cast<float>(crop_h) * ratio) * 0.5f;

        pose_event_log::PoseInstanceRecord pose;
        pose.index = 0;
        pose.label = "fish";
        pose.confidence = clamp_unit(best_score);
        pose.keypoints.reserve(keypoint_count_);
        for (size_t k = 0; k < keypoint_count_; ++k) {
            const int base = 5 + static_cast<int>(k) * 3;
            const float model_x = output_at(h_output, base, best_candidate);
            const float model_y = output_at(h_output, base + 1, best_candidate);
            const float keypoint_score = output_at(h_output, base + 2, best_candidate);
            pose_event_log::PoseKeypointRecord keypoint;
            keypoint.label = keypoint_labels_[k];
            keypoint.x_px = clamp(
                (model_x - dw) / std::max(1.0f, ratio),
                0.0f,
                static_cast<float>(std::max(1, crop_w)));
            keypoint.y_px = clamp(
                (model_y - dh) / std::max(1.0f, ratio),
                0.0f,
                static_cast<float>(std::max(1, crop_h)));
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
    bool bound_to_defaults_ = false;
    std::vector<std::string> keypoint_labels_;
    float confidence_threshold_ = 0.25f;
    float keypoint_confidence_threshold_ = 0.25f;
};

// One in-flight frame of the device crop path. `view` is what rides the
// worker queue (the pose thread maps it back to the slot); view.frame is the
// snapshot the event log and IPC need, filled by the YOLO thread at enqueue
// and completed from h_roi once the GPU is done.
struct PoseWorker::DeviceStageSlot {
    enum State { kFree = 0, kFilling = 1, kQueued = 2 };
    CropFrame view;
    unsigned char* d_crop_mono = nullptr;
    void* d_input = nullptr;
    void* d_output = nullptr;
    float* h_output = nullptr;
    DetectRoi* h_roi = nullptr;
    cudaEvent_t input_ready_event = nullptr;  // YOLO stream, after crop + preprocess (timing on)
    cudaEvent_t done_event = nullptr;         // pose stream, after the output copy (timing on)
    cudaGraphExec_t graph_exec = nullptr;     // step 2: enqueueV3 + output copy, bound to this slot
    // Step 3 (fused frame graph) pieces.
    DetectRoi* d_roi = nullptr;
    FusedFrameArgs* h_args = nullptr;
    FusedFrameArgs* d_args = nullptr;
    cudaEvent_t detect_done_event = nullptr;
    cudaEvent_t graph_end_event = nullptr;    // fused graph: last node; the slot is reused only after it
    cudaGraphExec_t fused_exec = nullptr;
    std::atomic<int> state{kFree};
    uint64_t enqueue_host_ns = 0;
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

    // Destructor must not throw (docs/error_handling_convention.md):
    // best-effort device selection during teardown.
    if (camera_params_ && cudaSetDevice(camera_params_->gpu_id) != cudaSuccess) {
        std::cerr << "[PoseWorker] ~PoseWorker: cudaSetDevice failed; continuing teardown" << std::endl;
    }
    if (stream_) {
        cudaStreamSynchronize(stream_);
    }
    free_device_slots();
    tensorrt_backend_.reset();
    if (stream_) {
        cudaStreamSynchronize(stream_);
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }

    if (device_stage_enabled_) {
        std::cout << "[PoseWorker] device stage summary for " << threadName
                  << " enqueued=" << device_stage_enqueued_.load(std::memory_order_relaxed)
                  << " slot_busy_drops=" << device_stage_slot_busy_.load(std::memory_order_relaxed)
                  << " failed=" << device_stage_failed_.load(std::memory_order_relaxed)
                  << " slots=" << device_slot_count_
                  << " graphs=" << device_stage_graphs_
                  << " crop_px=" << device_stage_crop_px_
                  << std::endl;
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
                 "capture_to_pose_done_p95_ms,capture_to_pose_done_p99_ms,capture_to_pose_done_max_ms,"
                 "latency_percentile_method,latency_max_retained_samples,"
                 "device_stage_gpu_count,device_stage_gpu_mean_ms,device_stage_gpu_p50_ms,"
                 "device_stage_gpu_p95_ms,device_stage_gpu_p99_ms,device_stage_gpu_max_ms\n";
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

bool PoseWorker::EnableDeviceStage(int pose_crop_px, std::string* error_out)
{
    auto fail = [&](const std::string& message) {
        if (error_out) {
            *error_out = message;
        }
        return false;
    };
    if (device_stage_enabled_) {
        return true;
    }
    if (pose_mode_ != "real" || !tensorrt_backend_) {
        return fail("device crop requires pose_worker.mode=real with a TensorRT engine");
    }
    if (pose_crop_px <= 0) {
        return fail("device crop requires a positive pose crop size");
    }
    if (pose_crop_px != tensorrt_backend_->input_width() ||
        pose_crop_px != tensorrt_backend_->input_height()) {
        // The pose crop is meant to be the model's native input so the
        // preprocess is an identity; a mismatch is letterbox-resized, which
        // is not the training contract of the 192 head model.
        std::cerr << "[PoseWorker] WARNING: pose crop " << pose_crop_px << " px does not match the engine input "
                  << tensorrt_backend_->input_width() << "x" << tensorrt_backend_->input_height()
                  << " for " << threadName << "; the crop will be letterbox-resized (set pose_worker.crop_size_px to the engine input)"
                  << std::endl;
    }
    const int slots = env_int_or_default("ORANGE_POSE_DEVICE_SLOTS", 8, 2, 64);
    try {
        ck(cudaSetDevice(camera_params_->gpu_id));
        device_slots_.reserve(static_cast<size_t>(slots));
        for (int i = 0; i < slots; ++i) {
            auto slot = std::make_unique<DeviceStageSlot>();
            ck(cudaMalloc(reinterpret_cast<void**>(&slot->d_crop_mono),
                          static_cast<size_t>(pose_crop_px) * static_cast<size_t>(pose_crop_px)));
            ck(cudaMalloc(&slot->d_input, tensorrt_backend_->input_bytes()));
            ck(cudaMalloc(&slot->d_output, tensorrt_backend_->output_bytes()));
            ck(cudaHostAlloc(reinterpret_cast<void**>(&slot->h_output), tensorrt_backend_->output_bytes(), 0));
            ck(cudaHostAlloc(reinterpret_cast<void**>(&slot->h_roi), sizeof(DetectRoi), 0));
            *slot->h_roi = DetectRoi{};
            ck(cudaEventCreate(&slot->input_ready_event));
            ck(cudaEventCreate(&slot->done_event));
            ck(cudaMalloc(reinterpret_cast<void**>(&slot->d_roi), sizeof(DetectRoi)));
            ck(cudaMemset(slot->d_roi, 0, sizeof(DetectRoi)));
            ck(cudaHostAlloc(reinterpret_cast<void**>(&slot->h_args), sizeof(FusedFrameArgs), cudaHostAllocMapped));
            *slot->h_args = FusedFrameArgs{};
            ck(cudaHostGetDevicePointer(reinterpret_cast<void**>(&slot->d_args), slot->h_args, 0));
            ck(cudaEventCreate(&slot->detect_done_event));
            ck(cudaEventCreateWithFlags(&slot->graph_end_event, cudaEventDisableTiming));
            slot->view.d_crop_mono = slot->d_crop_mono;
            device_slots_.push_back(std::move(slot));
        }
    } catch (const std::exception& ex) {
        free_device_slots();
        return fail(std::string("device crop slot allocation failed: ") + ex.what());
    }
    // ORANGE_POSE_DEVICE_GRAPH (default on): one CUDA graph per slot for the
    // pose enqueue + output copy. Off = plain enqueueV3 per frame (the step 1
    // path, 0.40 ms of CPU on the YOLO thread).
    const bool want_graphs = orange::yolo_flags::EnvFlag("ORANGE_POSE_DEVICE_GRAPH", true);
    int captured = 0;
    if (want_graphs) {
        for (auto& slot : device_slots_) {
            std::string error;
            try {
                if (tensorrt_backend_->capture_slot_graph(
                        slot->d_input, slot->d_output, slot->h_output, stream_,
                        &slot->graph_exec, &error)) {
                    ++captured;
                } else {
                    std::cerr << "[PoseWorker] pose graph capture failed for " << threadName
                              << ": " << error << "; that slot uses plain enqueue" << std::endl;
                }
            } catch (const std::exception& ex) {
                std::cerr << "[PoseWorker] pose graph capture threw for " << threadName
                          << ": " << ex.what() << "; that slot uses plain enqueue" << std::endl;
                slot->graph_exec = nullptr;
            }
        }
    }
    device_slot_count_ = slots;
    device_stage_crop_px_ = pose_crop_px;
    device_stage_enabled_ = true;
    device_stage_graphs_ = captured;
    std::cout << "[PoseWorker] Device crop path (ORANGE_ANALYTICS_DEVICE_CROP) enabled for "
              << threadName
              << " crop_px=" << pose_crop_px
              << " engine_input=" << tensorrt_backend_->input_width() << "x"
              << tensorrt_backend_->input_height()
              << " slots=" << slots
              << " graphs=" << captured << "/" << slots
              << std::endl;
    return true;
}

void PoseWorker::free_device_slots()
{
    for (auto& slot : device_slots_) {
        if (!slot) {
            continue;
        }
        if (slot->d_crop_mono) cudaFree(slot->d_crop_mono);
        if (slot->d_input) cudaFree(slot->d_input);
        if (slot->d_output) cudaFree(slot->d_output);
        if (slot->h_output) cudaFreeHost(slot->h_output);
        if (slot->h_roi) cudaFreeHost(slot->h_roi);
        if (slot->input_ready_event) cudaEventDestroy(slot->input_ready_event);
        if (slot->done_event) cudaEventDestroy(slot->done_event);
        if (slot->graph_exec) cudaGraphExecDestroy(slot->graph_exec);
        if (slot->fused_exec) cudaGraphExecDestroy(slot->fused_exec);
        if (slot->d_roi) cudaFree(slot->d_roi);
        if (slot->h_args) cudaFreeHost(slot->h_args);
        if (slot->detect_done_event) cudaEventDestroy(slot->detect_done_event);
        if (slot->graph_end_event) cudaEventDestroy(slot->graph_end_event);
    }
    device_slots_.clear();
    device_stage_enabled_ = false;
}

PoseWorker::DeviceStageSlot* PoseWorker::find_device_slot(CropFrame* crop_frame)
{
    for (auto& slot : device_slots_) {
        if (slot && &slot->view == crop_frame) {
            return slot.get();
        }
    }
    return nullptr;
}

void PoseWorker::fill_slot_snapshot(DeviceStageSlot& slot, WORKER_ENTRY* entry, uint64_t enqueue_host_ns)
{
    CropFrameSnapshot& frame = slot.view.frame;
    frame.ResetForReuse();
    frame.recording_frame_id = entry->recording_frame_id;
    frame.local_frame_id = entry->frame_id;
    frame.camera_frame_id = entry->camera_frame_id;
    frame.timestamp = entry->timestamp;
    frame.timestamp_sys = entry->timestamp_sys;
    frame.recording_folder = entry->recording_folder;
    frame.source_width = entry->width;
    frame.source_height = entry->height;
    frame.acquisition_receive_host_ns = entry->acquisition_receive_host_ns;
    frame.crop_w = device_stage_crop_px_;
    frame.crop_h = device_stage_crop_px_;
    slot.enqueue_host_ns = enqueue_host_ns;
}

// Hand a slot whose GPU work is queued to the pose thread. On failure the
// slot is freed after its work completes and false is returned.
bool PoseWorker::publish_slot(DeviceStageSlot& slot)
{
    const CropFrameSnapshot& frame = slot.view.frame;
    slot.state.store(DeviceStageSlot::kQueued, std::memory_order_release);
    const bool record_active =
        frame.recording_frame_id > 0 && !frame.recording_folder.empty();
    const int queue_depth = GetCountQueueInSize();
    queue_high_water_.store(
        std::max(queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
        std::memory_order_relaxed);
    if (record_active) {
        run_queue_high_water_.store(
            std::max(run_queue_high_water_.load(std::memory_order_relaxed), queue_depth + 1),
            std::memory_order_relaxed);
    }
    if (!PutObjectToQueueIn(&slot.view)) {
        cudaEventSynchronize(slot.done_event);
        slot.state.store(DeviceStageSlot::kFree, std::memory_order_release);
        device_stage_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    device_stage_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (record_active) {
        run_frames_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

int PoseWorker::AcquireFusedSlot()
{
    if (!device_stage_enabled_ || device_slot_count_ <= 0) {
        return -1;
    }
    DeviceStageSlot& slot = *device_slots_[device_slot_next_ % static_cast<uint64_t>(device_slot_count_)];
    int expected = DeviceStageSlot::kFree;
    if (!slot.state.compare_exchange_strong(expected, DeviceStageSlot::kFilling)) {
        device_stage_slot_busy_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    const int index = static_cast<int>(device_slot_next_ % static_cast<uint64_t>(device_slot_count_));
    ++device_slot_next_;
    return index;
}

void PoseWorker::ReleaseFusedSlotUnused(int index)
{
    if (index < 0 || index >= device_slot_count_) {
        return;
    }
    device_slots_[static_cast<size_t>(index)]->state.store(DeviceStageSlot::kFree, std::memory_order_release);
}

bool PoseWorker::GetFusedSlot(int index, FusedSlotView* out) const
{
    if (!out || index < 0 || index >= device_slot_count_) {
        return false;
    }
    const DeviceStageSlot& slot = *device_slots_[static_cast<size_t>(index)];
    out->index = index;
    out->h_args = slot.h_args;
    out->d_args = slot.d_args;
    out->d_roi = slot.d_roi;
    out->h_roi = slot.h_roi;
    out->detect_done_event = slot.detect_done_event;
    out->done_event = slot.done_event;
    out->graph_end_event = slot.graph_end_event;
    out->fused_exec = slot.fused_exec;
    return true;
}

bool PoseWorker::WarmPoseSlot(int index, cudaStream_t stream, std::string* error_out)
{
    if (index < 0 || index >= device_slot_count_ || !tensorrt_backend_) {
        if (error_out) *error_out = "no such slot";
        return false;
    }
    DeviceStageSlot& slot = *device_slots_[static_cast<size_t>(index)];
    std::string error;
    ck(cudaMemsetAsync(slot.d_input, 0, tensorrt_backend_->input_bytes(), stream));
    if (!tensorrt_backend_->enqueue_with_buffers(slot.d_input, slot.d_output, slot.h_output, stream, &error)) {
        if (error_out) *error_out = error;
        return false;
    }
    ck(cudaStreamSynchronize(stream));
    return true;
}

bool PoseWorker::EnqueuePoseStageForCapture(int index, cudaStream_t stream, std::string* error_out)
{
    if (index < 0 || index >= device_slot_count_ || !tensorrt_backend_) {
        if (error_out) *error_out = "no such slot";
        return false;
    }
    DeviceStageSlot& slot = *device_slots_[static_cast<size_t>(index)];
    launch_pose_crop_from_roi_indirect(
        slot.d_args, slot.d_roi, slot.d_crop_mono,
        device_stage_crop_px_, device_stage_crop_px_, stream);
    tensorrt_backend_->preprocess_into(
        slot.d_crop_mono, device_stage_crop_px_, device_stage_crop_px_, slot.d_input, stream);
    // Inside a capture: external record, so the pose thread can time it.
    ck(cudaEventRecordWithFlags(slot.input_ready_event, stream, cudaEventRecordExternal));
    std::string error;
    if (!tensorrt_backend_->enqueue_with_buffers(slot.d_input, slot.d_output, slot.h_output, stream, &error)) {
        if (error_out) *error_out = error;
        return false;
    }
    return true;
}

void PoseWorker::SetFusedGraph(int index, cudaGraphExec_t exec)
{
    if (index < 0 || index >= device_slot_count_) {
        return;
    }
    DeviceStageSlot& slot = *device_slots_[static_cast<size_t>(index)];
    if (slot.fused_exec && slot.fused_exec != exec) {
        cudaGraphExecDestroy(slot.fused_exec);
    }
    slot.fused_exec = exec;
}

bool PoseWorker::PublishFusedSlot(int index, WORKER_ENTRY* entry, uint64_t enqueue_host_ns)
{
    if (index < 0 || index >= device_slot_count_ || !entry) {
        return false;
    }
    DeviceStageSlot& slot = *device_slots_[static_cast<size_t>(index)];
    fill_slot_snapshot(slot, entry, enqueue_host_ns);
    return publish_slot(slot);
}

int PoseWorker::EnqueueDeviceStage(
    WORKER_ENTRY* entry,
    const unsigned char* d_source,
    int source_pitch,
    cudaStream_t yolo_stream,
    double* cpu_ms_out,
    cudaEvent_t* done_event_out)
{
    const uint64_t start_ns = steady_now_ns();
    if (done_event_out) {
        *done_event_out = nullptr;
    }
    if (!device_stage_enabled_ || !tensorrt_backend_ || !entry || !d_source ||
        !entry->d_detect_roi || !yolo_stream) {
        return -1;
    }
    DeviceStageSlot& slot = *device_slots_[device_slot_next_ % static_cast<uint64_t>(device_slot_count_)];
    int expected = DeviceStageSlot::kFree;
    if (!slot.state.compare_exchange_strong(expected, DeviceStageSlot::kFilling)) {
        device_stage_slot_busy_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    ++device_slot_next_;

    fill_slot_snapshot(slot, entry, start_ns);
    const CropFrameSnapshot& frame = slot.view.frame;

    try {
        // YOLO stream: crop from the device ROI, preprocess into the slot's
        // input, mirror the ROI to the host, and mark the input ready. All of
        // this reads d_source, which the frame's completion event (recorded
        // by the caller after this returns) covers.
        launch_pose_crop_from_roi(
            d_source, source_pitch, entry->width, entry->height,
            entry->d_detect_roi, slot.d_crop_mono,
            device_stage_crop_px_, device_stage_crop_px_, yolo_stream);
        tensorrt_backend_->preprocess_into(
            slot.d_crop_mono, device_stage_crop_px_, device_stage_crop_px_,
            slot.d_input, yolo_stream);
        ck(cudaMemcpyAsync(slot.h_roi, entry->d_detect_roi, sizeof(DetectRoi),
                           cudaMemcpyDeviceToHost, yolo_stream));
        ck(cudaEventRecord(slot.input_ready_event, yolo_stream));

        // Pose stream: run behind the input-ready event, copy the output back,
        // and mark done. The pose thread waits on done_event and decodes.
        ck(cudaStreamWaitEvent(stream_, slot.input_ready_event, 0));
        std::string error;
        bool queued = false;
        if (slot.graph_exec) {
            const cudaError_t launch = cudaGraphLaunch(slot.graph_exec, stream_);
            if (launch == cudaSuccess) {
                queued = true;
            } else {
                cudaGetLastError();
                error = std::string("graph_launch_failed: ") + cudaGetErrorString(launch);
            }
        } else {
            queued = tensorrt_backend_->enqueue_with_buffers(
                slot.d_input, slot.d_output, slot.h_output, stream_, &error);
        }
        if (!queued) {
            std::cerr << "[PoseWorker] device stage enqueue failed for " << threadName
                      << ": " << error << std::endl;
            ck(cudaStreamSynchronize(yolo_stream));
            slot.state.store(DeviceStageSlot::kFree, std::memory_order_release);
            device_stage_failed_.fetch_add(1, std::memory_order_relaxed);
            return -2;
        }
        ck(cudaEventRecord(slot.done_event, stream_));
    } catch (const std::exception& ex) {
        std::cerr << "[PoseWorker] device stage launch failed for " << threadName
                  << ": " << ex.what() << std::endl;
        slot.state.store(DeviceStageSlot::kFree, std::memory_order_release);
        device_stage_failed_.fetch_add(1, std::memory_order_relaxed);
        return -2;
    }

    (void)frame;
    if (!publish_slot(slot)) {
        return -2;
    }
    if (cpu_ms_out) {
        *cpu_ms_out = elapsed_ms(start_ns, steady_now_ns());
    }
    if (done_event_out) {
        *done_event_out = slot.done_event;
    }
    return 1;
}

void PoseWorker::process_device_slot(DeviceStageSlot& slot)
{
    std::string pose_status = "no_result";
    std::string pose_error;
    std::vector<pose_event_log::PoseInstanceRecord> poses;
    CropFrameSnapshot& frame = slot.view.frame;
    const uint64_t pose_start_host_ns = slot.enqueue_host_ns;
    float gpu_ms = -1.0f;
    try {
        ck(cudaSetDevice(camera_params_->gpu_id));
        ck(cudaEventSynchronize(slot.done_event));
        if (cudaEventElapsedTime(&gpu_ms, slot.input_ready_event, slot.done_event) != cudaSuccess) {
            cudaGetLastError();
            gpu_ms = -1.0f;
        }
        const DetectRoi& roi = *slot.h_roi;
        frame.has_detection = roi.valid == 1;
        frame.detection_confidence = roi.score;
        frame.detection_x = roi.box_x;
        frame.detection_y = roi.box_y;
        frame.detection_w = roi.box_w;
        frame.detection_h = roi.box_h;
        frame.crop_x = roi.pose_crop_x;
        frame.crop_y = roi.pose_crop_y;
        frame.crop_w = roi.pose_crop_w > 0 ? roi.pose_crop_w : device_stage_crop_px_;
        frame.crop_h = roi.pose_crop_h > 0 ? roi.pose_crop_h : device_stage_crop_px_;
        if (frame.has_detection && tensorrt_backend_) {
            tensorrt_backend_->decode_from(slot.h_output, frame.crop_w, frame.crop_h,
                                           &pose_status, &poses);
        }
    } catch (const std::exception& ex) {
        pose_status = "failed";
        pose_error = ex.what();
        std::cerr << "[PoseWorker] device stage result failed for " << threadName
                  << ": " << ex.what() << std::endl;
    }
    const uint64_t pose_done_host_ns = steady_now_ns();

    frames_processed_.fetch_add(1, std::memory_order_relaxed);
    const bool record_active =
        frame.recording_frame_id > 0 && !frame.recording_folder.empty();
    if (record_active) {
        run_frames_processed_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(recording_mutex_);
        // On this path pose_start is the YOLO thread's enqueue, which happens
        // right after the detect graph is queued, so pose_start_to_pose_done
        // spans detect + pose on the GPU; the crop-thread columns have no
        // samples. capture_to_pose_done is the gate.
        pose_start_to_pose_done_samples_ms_.Add(elapsed_ms(pose_start_host_ns, pose_done_host_ns));
        if (frame.acquisition_receive_host_ns > 0) {
            capture_to_pose_done_samples_ms_.Add(elapsed_ms(
                frame.acquisition_receive_host_ns, pose_done_host_ns));
        }
        if (gpu_ms >= 0.0f) {
            device_stage_gpu_samples_ms_.Add(static_cast<double>(gpu_ms));
        }
    }
    if (record_active) {
        pose_event_logger_.Enqueue(build_pose_event_record(
            frame, pose_start_host_ns, pose_done_host_ns, pose_status, pose_error, poses));
    }
    publish_pose_result_v2(frame, pose_status, poses);
    // Fused graph: the pool copy trails the pose in the same graph and reads
    // the slot's argument block, so the slot goes back only once the whole
    // graph has run. Never recorded on the device crop path: returns at once.
    if (slot.graph_end_event) {
        cudaEventSynchronize(slot.graph_end_event);
    }
    slot.state.store(DeviceStageSlot::kFree, std::memory_order_release);
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
    if (DeviceStageSlot* slot = find_device_slot(crop_frame)) {
        process_device_slot(*slot);
        return false;
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
                capture_to_detect_done_samples_ms_.Add(elapsed_ms(
                    crop_frame->frame.acquisition_receive_host_ns,
                    crop_frame->frame.yolo_detect_done_host_ns));
            }
            if (crop_frame->frame.yolo_detect_done_host_ns > 0 &&
                crop_frame->frame.crop_producer_worker_start_host_ns > 0) {
                detect_to_crop_worker_start_samples_ms_.Add(elapsed_ms(
                    crop_frame->frame.yolo_detect_done_host_ns,
                    crop_frame->frame.crop_producer_worker_start_host_ns));
            }
            if (crop_frame->frame.crop_producer_worker_start_host_ns > 0 &&
                crop_frame->frame.crop_ready_host_ns > 0) {
                crop_worker_start_to_crop_ready_samples_ms_.Add(elapsed_ms(
                    crop_frame->frame.crop_producer_worker_start_host_ns,
                    crop_frame->frame.crop_ready_host_ns));
            }
            if (crop_frame->frame.yolo_detect_done_host_ns > 0 &&
                crop_frame->frame.crop_ready_host_ns > 0) {
                detect_to_crop_ready_samples_ms_.Add(elapsed_ms(
                    crop_frame->frame.yolo_detect_done_host_ns,
                    crop_frame->frame.crop_ready_host_ns));
            }
            if (crop_frame->frame.crop_ready_host_ns > 0) {
                crop_ready_to_pose_start_samples_ms_.Add(elapsed_ms(
                    crop_frame->frame.crop_ready_host_ns,
                    pose_start_host_ns));
            }
            pose_start_to_pose_done_samples_ms_.Add(elapsed_ms(
                pose_start_host_ns,
                pose_done_host_ns));
            if (crop_frame->frame.acquisition_receive_host_ns > 0) {
                capture_to_pose_done_samples_ms_.Add(elapsed_ms(
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

    // The live-state publisher merges base, YOLO and pose updates by
    // state_frame_id, and the base frame (acquire_frames) and the YOLO update
    // (yolo_worker) use the absolute per-camera frame id. Using the
    // recording id here (it restarts at 1 for every recording) made every
    // pose update stale-suppressed or mis-merged while recording, so readers
    // saw pose only outside recordings (2026-09-22). recording_frame_id stays
    // a mirror field on the slot.
    const uint64_t frame_id = frame.local_frame_id;
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
    capture_to_detect_done_samples_ms_.Reset();
    detect_to_crop_worker_start_samples_ms_.Reset();
    crop_worker_start_to_crop_ready_samples_ms_.Reset();
    detect_to_crop_ready_samples_ms_.Reset();
    crop_ready_to_pose_start_samples_ms_.Reset();
    pose_start_to_pose_done_samples_ms_.Reset();
    capture_to_pose_done_samples_ms_.Reset();
    device_stage_gpu_samples_ms_.Reset();
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
    const LatencySummary device_stage_gpu =
        summarize_latency(device_stage_gpu_samples_ms_);

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
              << capture_to_pose_done.max_ms << ','
              << "deterministic_reservoir_v1,"
              << orange::BoundedSampleStatistics::kDefaultMaxRetainedSamples << ','
              << device_stage_gpu.count << ','
              << device_stage_gpu.mean_ms << ','
              << device_stage_gpu.p50_ms << ','
              << device_stage_gpu.p95_ms << ','
              << device_stage_gpu.p99_ms << ','
              << device_stage_gpu.max_ms << '\n';
}
