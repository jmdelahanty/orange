// src/yolov8_det.cpp
#include "yolov8_det.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <opencv2/opencv.hpp>

namespace {
std::mutex g_trt_enqueue_mutex;

bool UseTrtEnqueueMutex()
{
    static const bool enabled = []() {
        const char* env = std::getenv("ORANGE_YOLO_ENQUEUE_MUTEX");
        const bool on = env && *env && std::strcmp(env, "0") != 0;
        if (on) {
            std::cout << "[YOLOv8] Global enqueue mutex enabled." << std::endl;
        }
        return on;
    }();
    return enabled;
}
}  // namespace

void YOLOv8::initialize_plugins() {
    static bool plugins_initialized = false;
    if (!plugins_initialized) {
        Logger logger; // A temporary logger for this one-time initialization.
        initLibNvInferPlugins(&logger, "");
        plugins_initialized = true;
        std::cout << "[YOLOv8] TensorRT plugins initialized." << std::endl;
    }
}

YOLOv8::YOLOv8(const std::string& engine_file_path, int width, int height)
{
    this->stream = nullptr;
    this->d_planar = nullptr;
    this->engine = nullptr;
    this->runtime = nullptr;
    this->context = nullptr;
    this->use_cuda_graph_ = true;
    this->infer_graph_ = nullptr;
    this->infer_graph_exec_ = nullptr;

    const char* graph_env = std::getenv("ORANGE_YOLO_CUDA_GRAPH");
    if (graph_env && *graph_env && std::strcmp(graph_env, "0") == 0) {
        this->use_cuda_graph_ = false;
    }
    std::cout << "[YOLOv8] CUDA graph mode: "
              << (this->use_cuda_graph_ ? "enabled" : "disabled")
              << std::endl;

    std::ifstream file(engine_file_path, std::ios::binary);
    if (!file.good()) {
        throw std::runtime_error("YOLOv8: Cannot open engine file: " + engine_file_path);
    }
    
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    char* trtModelStream = new char[size];
    assert(trtModelStream);
    file.read(trtModelStream, size);
    file.close();
    
    this->runtime = nvinfer1::createInferRuntime(this->gLogger);
    if (!this->runtime) {
        delete[] trtModelStream;
        throw std::runtime_error("YOLOv8: Failed to create TensorRT runtime");
    }

    this->engine = this->runtime->deserializeCudaEngine(trtModelStream, size);
    if (!this->engine) {
        delete[] trtModelStream;
        throw std::runtime_error("YOLOv8: Failed to deserialize CUDA engine");
    }
    delete[] trtModelStream;
    
    this->context = this->engine->createExecutionContext();
    if (!this->context) {
        throw std::runtime_error("YOLOv8: Failed to create execution context");
    }

    this->num_bindings = this->engine->getNbIOTensors();

    for (int i = 0; i < this->num_bindings; ++i) {
        Binding binding;
        nvinfer1::Dims dims;
        const char* name = this->engine->getIOTensorName(i);
        nvinfer1::DataType dtype = this->engine->getTensorDataType(name);
        binding.name = name;
        binding.dsize = type_to_size(dtype);

        nvinfer1::TensorIOMode ioMode = this->engine->getTensorIOMode(name);

        if (ioMode == nvinfer1::TensorIOMode::kINPUT) {
            this->num_inputs += 1;
            dims = this->engine->getProfileShape(name, 0, nvinfer1::OptProfileSelector::kMAX);
            binding.size = get_size_by_dims(dims);
            binding.dims = dims;
            this->input_bindings.push_back(binding);
            
            if (!this->context->setInputShape(name, dims)) {
                throw std::runtime_error("YOLOv8: Failed to set input shape in constructor for " + std::string(name));
            }
        }
        else if (ioMode == nvinfer1::TensorIOMode::kOUTPUT) {
            dims = this->context->getTensorShape(name);
            binding.size = get_size_by_dims(dims);
            binding.dims = dims;
            this->output_bindings.push_back(binding);
            this->num_outputs += 1;
        }
    }

    auto& in_binding = this->input_bindings[0];
    inp_h_int = in_binding.dims.d[2];
    inp_w_int = in_binding.dims.d[3];
}

YOLOv8::~YOLOv8()
{
    if (this->stream) {
        cudaStreamSynchronize(this->stream);
    }
    if (this->infer_graph_exec_) { cudaGraphExecDestroy(this->infer_graph_exec_); }
    if (this->infer_graph_) { cudaGraphDestroy(this->infer_graph_); }
    if (this->context) { delete this->context; }
    if (this->engine) { delete this->engine; }
    if (this->runtime) { delete this->runtime; }
    for (size_t i = 0; i < this->device_ptrs.size(); ++i) { if (this->device_ptrs[i]) cudaFree(this->device_ptrs[i]); }
    for (size_t i = 0; i < this->host_ptrs.size(); ++i) { if (this->host_ptrs[i]) cudaFreeHost(this->host_ptrs[i]); }
    if (this->stream) { cudaStreamDestroy(this->stream); }
}

void YOLOv8::make_pipe(bool warmup, int max_width, int max_height)
{
    if (this->stream == nullptr) {
        int least_priority = 0;
        int greatest_priority = 0;
        CHECK(cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority));
        int priority = 0;
        unsigned int stream_flags = 0;
        const char* priority_env = std::getenv("ORANGE_YOLO_STREAM_PRIORITY");
        if (priority_env && *priority_env) {
            if (std::strcmp(priority_env, "high") == 0) {
                priority = greatest_priority;
            } else if (std::strcmp(priority_env, "low") == 0) {
                priority = least_priority;
            } else {
                char* end = nullptr;
                long parsed = std::strtol(priority_env, &end, 10);
                if (end != priority_env && *end == '\0') {
                    if (parsed < greatest_priority) {
                        parsed = greatest_priority;
                    } else if (parsed > least_priority) {
                        parsed = least_priority;
                    }
                    priority = static_cast<int>(parsed);
                }
            }
        }
        const char* nonblocking_env = std::getenv("ORANGE_YOLO_STREAM_NONBLOCKING");
        if (nonblocking_env && *nonblocking_env && std::strcmp(nonblocking_env, "0") != 0) {
            stream_flags = cudaStreamNonBlocking;
        }
        if (priority < greatest_priority) {
            priority = greatest_priority;
        } else if (priority > least_priority) {
            priority = least_priority;
        }
        CHECK(cudaStreamCreateWithPriority(&this->stream, stream_flags, priority));
        std::cout << "[YOLOv8] Stream priority " << priority
                  << " (range " << greatest_priority << ".." << least_priority << ")"
                  << " nonblocking=" << (stream_flags == cudaStreamNonBlocking ? "on" : "off")
                  << std::endl;
    }

    for (size_t i = 0; i < this->input_bindings.size(); ++i) {
        auto& bindings = this->input_bindings[i];
        void* d_ptr;
        CHECK(cudaMallocAsync(&d_ptr, bindings.size * bindings.dsize, this->stream));
        this->device_ptrs.push_back(d_ptr);
    }

    for (size_t i = 0; i < this->output_bindings.size(); ++i) {
        auto& bindings = this->output_bindings[i];
        void* d_ptr;
        void* h_ptr;
        CHECK(cudaMallocAsync(&d_ptr, bindings.size * bindings.dsize, this->stream));
        CHECK(cudaHostAlloc(&h_ptr, bindings.size * bindings.dsize, 0));
        this->device_ptrs.push_back(d_ptr);
        this->host_ptrs.push_back(h_ptr);
    }

    this->bind_tensors();

    if (warmup) {
        for (int i = 0; i < 10; i++) {
            for (size_t j = 0; j < this->input_bindings.size(); ++j) {
                size_t size = this->input_bindings[j].size * this->input_bindings[j].dsize;
                void* h_ptr = malloc(size);
                memset(h_ptr, 0, size);
                CHECK(cudaMemcpyAsync(this->device_ptrs[j], h_ptr, size, cudaMemcpyHostToDevice, this->stream));
                free(h_ptr);
            }
            this->infer();
        }
    }

    if (this->use_cuda_graph_) {
        capture_infer_graph();
    }
}

void YOLOv8::preprocess_gpu(unsigned char *d_src, int source_width, int source_height, bool is_color)
{
    const float inp_h  = (float)inp_h_int;
    const float inp_w  = (float)inp_w_int;
    float r = std::min(inp_h / (float)source_height, inp_w / (float)source_width);
    
    launch_optimized_yolo_preprocess(
        d_src,
        (float*)this->device_ptrs[0], //directly write to device pointer
        source_width, source_height,
        inp_w_int, inp_h_int,
        is_color,
        this->stream
    );

    this->pparam.ratio  = 1.0f / r;
    this->pparam.dw     = (inp_w - source_width * r) / 2.0f;
    this->pparam.dh     = (inp_h - source_height * r) / 2.0f;
    this->pparam.width  = (float)source_width;
    this->pparam.height = (float)source_height;
}

bool YOLOv8::capture_infer_graph()
{
    if (!this->use_cuda_graph_ || this->infer_graph_exec_) {
        return this->infer_graph_exec_ != nullptr;
    }
    if (!this->context || !this->stream) {
        std::cerr << "[YOLOv8] CUDA graph capture failed: context or stream not initialized." << std::endl;
        this->use_cuda_graph_ = false;
        return false;
    }

    cudaError_t err = cudaStreamSynchronize(this->stream);
    if (err != cudaSuccess) {
        std::cerr << "[YOLOv8] CUDA graph capture sync failed: "
                  << cudaGetErrorString(err) << std::endl;
        this->use_cuda_graph_ = false;
        return false;
    }

    err = cudaStreamBeginCapture(this->stream, cudaStreamCaptureModeThreadLocal);
    if (err != cudaSuccess) {
        std::cerr << "[YOLOv8] CUDA graph capture begin failed: "
                  << cudaGetErrorString(err) << std::endl;
        this->use_cuda_graph_ = false;
        return false;
    }

    if (!this->context->enqueueV3(this->stream)) {
        cudaStreamEndCapture(this->stream, &this->infer_graph_);
        throw std::runtime_error("YOLOv8::capture_infer_graph: enqueueV3 failed");
    }

    for (int i = 0; i < this->num_outputs; ++i) {
        size_t osize = this->output_bindings[i].size * this->output_bindings[i].dsize;
        CHECK(cudaMemcpyAsync(
            this->host_ptrs[i],
            this->device_ptrs[this->num_inputs + i],
            osize,
            cudaMemcpyDeviceToHost,
            this->stream));
    }

    err = cudaStreamEndCapture(this->stream, &this->infer_graph_);
    if (err != cudaSuccess) {
        std::cerr << "[YOLOv8] CUDA graph capture end failed: "
                  << cudaGetErrorString(err) << std::endl;
        this->use_cuda_graph_ = false;
        return false;
    }

    err = cudaGraphInstantiate(&this->infer_graph_exec_, this->infer_graph_, nullptr, nullptr, 0);
    if (err != cudaSuccess) {
        std::cerr << "[YOLOv8] CUDA graph instantiate failed: "
                  << cudaGetErrorString(err) << std::endl;
        this->use_cuda_graph_ = false;
        return false;
    }

    std::cout << "[YOLOv8] CUDA graph captured." << std::endl;
    return true;
}

void YOLOv8::bind_tensors()
{
    if (!this->context) {
        throw std::runtime_error("YOLOv8::bind_tensors: Execution context is null");
    }
    if (this->device_ptrs.size() < static_cast<size_t>(this->num_inputs + this->num_outputs)) {
        throw std::runtime_error("YOLOv8::bind_tensors: Device pointers are not initialized");
    }

    for (size_t i = 0; i < this->input_bindings.size(); ++i) {
        const auto& binding = this->input_bindings[i];
        if (!this->context->setTensorAddress(binding.name.c_str(), this->device_ptrs[i])) {
            throw std::runtime_error("YOLOv8::bind_tensors: Failed to set tensor address for " + binding.name);
        }
    }

    for (size_t i = 0; i < this->output_bindings.size(); ++i) {
        const auto& binding = this->output_bindings[i];
        if (!this->context->setTensorAddress(binding.name.c_str(),
                                             this->device_ptrs[this->num_inputs + i])) {
            throw std::runtime_error("YOLOv8::bind_tensors: Failed to set tensor address for " + binding.name);
        }
    }

    this->tensor_addresses_set_ = true;
}

void YOLOv8::infer()
{
    if (!this->context) {
        throw std::runtime_error("YOLOv8::infer: Execution context is null");
    }
    
    if (!this->stream) {
        throw std::runtime_error("YOLOv8::infer: CUDA stream is null");
    }
    
    if (!this->tensor_addresses_set_) {
        this->bind_tensors();
    }

    if (this->use_cuda_graph_ && this->infer_graph_exec_) {
        cudaError_t err = cudaGraphLaunch(this->infer_graph_exec_, this->stream);
        if (err != cudaSuccess) {
            std::cerr << "[YOLOv8] CUDA graph launch failed: "
                      << cudaGetErrorString(err) << ". Falling back to enqueue."
                      << std::endl;
            this->use_cuda_graph_ = false;
        } else {
            return;
        }
    }

    if (UseTrtEnqueueMutex()) {
        std::lock_guard<std::mutex> lock(g_trt_enqueue_mutex);
        if (!this->context->enqueueV3(this->stream)) {
            throw std::runtime_error("YOLOv8::infer: enqueueV3 failed");
        }
    } else {
        if (!this->context->enqueueV3(this->stream)) {
            throw std::runtime_error("YOLOv8::infer: enqueueV3 failed");
        }
    }

    for (int i = 0; i < this->num_outputs; ++i) {
        size_t osize = this->output_bindings[i].size * this->output_bindings[i].dsize;
        CHECK(cudaMemcpyAsync(
            this->host_ptrs[i],
            this->device_ptrs[this->num_inputs + i],
            osize,
            cudaMemcpyDeviceToHost,
            this->stream));
    }
}


void YOLOv8::postprocess(std::vector<Object>& objs)
{
    objs.clear();
    int* num_dets = static_cast<int*>(this->host_ptrs[0]);
    float* boxes    = static_cast<float*>(this->host_ptrs[1]);
    float* scores   = static_cast<float*>(this->host_ptrs[2]);
    int* labels   = static_cast<int*>(this->host_ptrs[3]);

    // FIX: Add a sanity check for the number of detections.
    // A very large number indicates a mismatch between the model's output
    // and what the code expects. This often happens when a new engine
    // with built-in NMS is used with code expecting raw output.
    int detections_to_process = num_dets[0];
    const int MAX_REASONABLE_DETS = 1000; // A safe upper limit for detections in one frame.

    if (detections_to_process < 0 || detections_to_process > MAX_REASONABLE_DETS) {
        std::cerr << "[YOLOv8 Postprocess Warning] Unreasonable number of detections reported: "
                  << detections_to_process
                  << ". Clamping to a maximum of " << MAX_REASONABLE_DETS
                  << ". This strongly suggests an engine model mismatch." << std::endl;
        detections_to_process = MAX_REASONABLE_DETS;
    }

    float& dw_letterbox    = this->pparam.dw;
    float& dh_letterbox    = this->pparam.dh;
    float& original_img_w = this->pparam.width;
    float& original_img_h = this->pparam.height;
    float& inv_ratio       = this->pparam.ratio;

    // Use the sanitized number of detections in the loop
    for (int i = 0; i < detections_to_process; i++) {
        float* ptr = boxes + i * 4;

        float x0_letterboxed = *ptr++;
        float y0_letterboxed = *ptr++;
        float x1_letterboxed = *ptr++;
        float y1_letterboxed = *ptr;

        float x0_resized = x0_letterboxed - dw_letterbox;
        float y0_resized = y0_letterboxed - dh_letterbox;
        float x1_resized = x1_letterboxed - dw_letterbox;
        float y1_resized = y1_letterboxed - dh_letterbox;

        float x0_original = clamp(x0_resized * inv_ratio, 0.f, original_img_w);
        float y0_original = clamp(y0_resized * inv_ratio, 0.f, original_img_h);
        float x1_original = clamp(x1_resized * inv_ratio, 0.f, original_img_w);
        float y1_original = clamp(y1_resized * inv_ratio, 0.f, original_img_h);

        Object obj;
        obj.rect.x      = x0_original;
        obj.rect.y      = y0_original;
        obj.rect.width  = x1_original - x0_original;
        obj.rect.height = y1_original - y0_original;
        obj.prob        = *(scores + i);
        obj.label       = *(labels + i);
        objs.push_back(obj);
    }
}

void YOLOv8::copy_keypoints_gpu(float* d_points, const std::vector<Object>& objs)
{
    float h_points[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    if (!objs.empty()) {
        const auto& obj = objs[0];
        h_points[0] = obj.rect.x;
        h_points[1] = obj.rect.y;
        h_points[2] = obj.rect.x + obj.rect.width;
        h_points[3] = obj.rect.y;
        h_points[4] = obj.rect.x + obj.rect.width;
        h_points[5] = obj.rect.y + obj.rect.height;
        h_points[6] = obj.rect.x;
        h_points[7] = obj.rect.y + obj.rect.height;
    }
    CHECK(cudaMemcpy(d_points, h_points, sizeof(float) * 8, cudaMemcpyHostToDevice));
}

void YOLOv8::draw_objects(const cv::Mat&                                image,
                          cv::Mat&                                      res,
                          const std::vector<Object>&                    objs,
                          const std::vector<std::string>&               CLASS_NAMES,
                          const std::vector<std::vector<unsigned int>>& COLORS)
{
    res = image.clone();
    for (auto& obj : objs) {
        cv::Scalar color = cv::Scalar(COLORS[obj.label][0], COLORS[obj.label][1], COLORS[obj.label][2]);
        
        // Create the cv::Rect on-the-fly from our custom pose::Rect
        cv::Rect cv_rect(static_cast<int>(obj.rect.x), 
                         static_cast<int>(obj.rect.y), 
                         static_cast<int>(obj.rect.width), 
                         static_cast<int>(obj.rect.height));
        cv::rectangle(res, cv_rect, color, 2);

        char text[256];
        sprintf(text, "%s %.1f%%", CLASS_NAMES[obj.label].c_str(), obj.prob * 100);

        int      baseLine   = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseLine);

        int x = (int)obj.rect.x;
        int y = (int)obj.rect.y + 1;

        if (y > res.rows)
            y = res.rows;

        cv::rectangle(res, cv::Rect(x, y, label_size.width, label_size.height + baseLine), {0, 0, 255}, -1);
        cv::putText(res, text, cv::Point(x, y + label_size.height), cv::FONT_HERSHEY_SIMPLEX, 0.4, {255, 255, 255}, 1);
    }
}
