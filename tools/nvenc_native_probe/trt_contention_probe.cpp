// Standalone paced TensorRT/CUDA-graph probe for same-die NVENC contention.
//
// Input is initialized to zero once and remains device-resident. This measures
// inference plumbing and GPU contention only; it says nothing about detection
// quality or preprocessing cost. By default the graph includes output D2H
// copies, matching Orange's captured detect graph.

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void CheckCuda(cudaError_t status, const char* expression, int line)
{
    if (status == cudaSuccess) {
        return;
    }
    std::ostringstream message;
    message << "CUDA failure at line " << line << " for " << expression
            << ": " << cudaGetErrorString(status);
    throw std::runtime_error(message.str());
}

#define CUDA_CHECK(expression) CheckCuda((expression), #expression, __LINE__)

uint64_t MonotonicNs()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL
        + static_cast<uint64_t>(now.tv_nsec);
}

void SleepUntilNs(uint64_t deadline_ns)
{
    timespec deadline{};
    deadline.tv_sec = static_cast<time_t>(deadline_ns / 1000000000ULL);
    deadline.tv_nsec = static_cast<long>(deadline_ns % 1000000000ULL);
    int status = 0;
    do {
        status = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (status == EINTR);
    if (status != 0) {
        throw std::runtime_error(
            "clock_nanosleep failed: " + std::string(std::strerror(status)));
    }
}

std::vector<char> ReadFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Cannot open engine: " + path);
    }
    const std::streamsize size = input.tellg();
    if (size <= 0) {
        throw std::runtime_error("Engine is empty: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (!input.read(bytes.data(), size)) {
        throw std::runtime_error("Cannot read engine: " + path);
    }
    return bytes;
}

bool HasDynamicDimension(const nvinfer1::Dims& dims)
{
    for (int32_t index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] < 0) {
            return true;
        }
    }
    return false;
}

size_t TensorVolume(const nvinfer1::Dims& dims)
{
    if (dims.nbDims < 0) {
        throw std::runtime_error("Tensor has invalid dimensions");
    }
    size_t volume = 1;
    for (int32_t index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] <= 0) {
            throw std::runtime_error("Tensor has unresolved or empty dimensions");
        }
        const size_t dimension = static_cast<size_t>(dims.d[index]);
        if (volume > std::numeric_limits<size_t>::max() / dimension) {
            throw std::runtime_error("Tensor element count overflow");
        }
        volume *= dimension;
    }
    return volume;
}

size_t DataTypeBits(nvinfer1::DataType type)
{
    switch (type) {
    case nvinfer1::DataType::kFLOAT: return 32;
    case nvinfer1::DataType::kHALF: return 16;
    case nvinfer1::DataType::kINT8: return 8;
    case nvinfer1::DataType::kINT32: return 32;
    case nvinfer1::DataType::kBOOL: return 8;
    case nvinfer1::DataType::kUINT8: return 8;
    case nvinfer1::DataType::kFP8: return 8;
    case nvinfer1::DataType::kBF16: return 16;
    case nvinfer1::DataType::kINT64: return 64;
    case nvinfer1::DataType::kINT4: return 4;
    }
    throw std::runtime_error("Unsupported TensorRT data type");
}

std::string DimsString(const nvinfer1::Dims& dims)
{
    std::ostringstream text;
    for (int32_t index = 0; index < dims.nbDims; ++index) {
        if (index != 0) {
            text << 'x';
        }
        text << dims.d[index];
    }
    return text.str();
}

std::string CsvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (char character : value) {
        if (character == '\"') {
            escaped += "\"\"";
        } else {
            escaped += character;
        }
    }
    escaped += '\"';
    return escaped;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream escaped;
    for (unsigned char character : value) {
        switch (character) {
        case '\\': escaped << "\\\\"; break;
        case '\"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) {
                escaped << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(character)
                        << std::dec << std::setfill(' ');
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

class Logger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override
    {
        if (severity <= Severity::kWARNING) {
            std::cerr << "[TensorRT] " << message << '\n';
        }
    }
};

struct Options {
    std::string engine;
    std::string csv;
    std::string summary_json;
    std::string label{"inference-alone"};
    int gpu_id{1};
    double fps{100.0};
    uint64_t warmup{100};
    uint64_t iterations{2000};
    uint64_t start_at_monotonic_ns{0};
    bool copy_outputs{true};
};

void PrintUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " --engine FILE --csv FILE [options]\n"
        << "  --gpu-id N                    CUDA device ordinal (default 1)\n"
        << "  --fps N                       Absolute pacing rate (default 100)\n"
        << "  --warmup N                    Paced graph warmup launches (default 100)\n"
        << "  --iterations N                Measured launches (default 2000)\n"
        << "  --start-at-monotonic-ns N     Absolute first measured deadline\n"
        << "  --label TEXT                  Condition label for CSV/JSON\n"
        << "  --summary-json FILE           Optional summary output\n"
        << "  --no-output-copy              Capture enqueueV3 without output D2H\n";
}

std::string RequireValue(int& index, int argc, char** argv)
{
    if (++index >= argc) {
        throw std::runtime_error("Missing value for " + std::string(argv[index - 1]));
    }
    return argv[index];
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--engine") {
            options.engine = RequireValue(index, argc, argv);
        } else if (argument == "--csv") {
            options.csv = RequireValue(index, argc, argv);
        } else if (argument == "--summary-json") {
            options.summary_json = RequireValue(index, argc, argv);
        } else if (argument == "--label") {
            options.label = RequireValue(index, argc, argv);
        } else if (argument == "--gpu-id") {
            options.gpu_id = std::stoi(RequireValue(index, argc, argv));
        } else if (argument == "--fps") {
            options.fps = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--warmup") {
            options.warmup = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--iterations") {
            options.iterations = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--start-at-monotonic-ns") {
            options.start_at_monotonic_ns
                = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--no-output-copy") {
            options.copy_outputs = false;
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + argument);
        }
    }
    if (options.engine.empty() || options.csv.empty()) {
        throw std::runtime_error("--engine and --csv are required");
    }
    if (options.gpu_id < 0 || !std::isfinite(options.fps) || options.fps <= 0.0
        || options.iterations == 0) {
        throw std::runtime_error("Invalid gpu/fps/iteration option");
    }
    return options;
}

struct TensorBuffer {
    std::string name;
    nvinfer1::TensorIOMode mode{nvinfer1::TensorIOMode::kNONE};
    nvinfer1::DataType type{nvinfer1::DataType::kFLOAT};
    nvinfer1::Dims dims{};
    size_t bytes{0};
    void* device{nullptr};
    void* host{nullptr};
};

class TrtGraphHarness {
public:
    TrtGraphHarness(const Options& options, Logger& logger)
        : copy_outputs_(options.copy_outputs)
    {
        CUDA_CHECK(cudaSetDevice(options.gpu_id));
        cudaDeviceProp properties{};
        CUDA_CHECK(cudaGetDeviceProperties(&properties, options.gpu_id));
        device_name_ = properties.name;

        int least_priority = 0;
        int greatest_priority = 0;
        CUDA_CHECK(cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority));
        stream_priority_ = greatest_priority;
        CUDA_CHECK(cudaStreamCreateWithPriority(
            &stream_, cudaStreamNonBlocking, stream_priority_));

        if (!initLibNvInferPlugins(&logger, "")) {
            throw std::runtime_error("initLibNvInferPlugins failed");
        }
        const std::vector<char> serialized = ReadFile(options.engine);
        runtime_ = nvinfer1::createInferRuntime(logger);
        if (runtime_ == nullptr) {
            throw std::runtime_error("createInferRuntime failed");
        }
        engine_ = runtime_->deserializeCudaEngine(serialized.data(), serialized.size());
        if (engine_ == nullptr) {
            throw std::runtime_error("deserializeCudaEngine failed");
        }
        context_ = engine_->createExecutionContext();
        if (context_ == nullptr) {
            throw std::runtime_error("createExecutionContext failed");
        }

        SelectProfileShapes();
        AllocateAndBind();
        CUDA_CHECK(cudaStreamSynchronize(stream_));

        // TensorRT may initialize resources on the first enqueue. Keep that
        // work outside graph capture and every measured interval.
        for (int iteration = 0; iteration < 3; ++iteration) {
            EnqueueWork();
            CUDA_CHECK(cudaStreamSynchronize(stream_));
        }
        CaptureGraph();
        CUDA_CHECK(cudaGraphUpload(graph_exec_, stream_));
        CUDA_CHECK(cudaStreamSynchronize(stream_));
        CUDA_CHECK(cudaEventCreate(&gpu_start_));
        CUDA_CHECK(cudaEventCreate(&gpu_stop_));
    }

    ~TrtGraphHarness()
    {
        if (stream_ != nullptr) {
            cudaStreamSynchronize(stream_);
        }
        if (gpu_stop_ != nullptr) cudaEventDestroy(gpu_stop_);
        if (gpu_start_ != nullptr) cudaEventDestroy(gpu_start_);
        if (graph_exec_ != nullptr) cudaGraphExecDestroy(graph_exec_);
        if (graph_ != nullptr) cudaGraphDestroy(graph_);
        for (auto& tensor : tensors_) {
            if (tensor.host != nullptr) cudaFreeHost(tensor.host);
            if (tensor.device != nullptr) cudaFree(tensor.device);
        }
        if (context_ != nullptr) delete context_;
        if (engine_ != nullptr) delete engine_;
        if (runtime_ != nullptr) delete runtime_;
        if (stream_ != nullptr) cudaStreamDestroy(stream_);
    }

    TrtGraphHarness(const TrtGraphHarness&) = delete;
    TrtGraphHarness& operator=(const TrtGraphHarness&) = delete;

    void RunGraphAndSynchronize()
    {
        CUDA_CHECK(cudaGraphLaunch(graph_exec_, stream_));
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

    cudaStream_t stream() const { return stream_; }
    cudaGraphExec_t graph_exec() const { return graph_exec_; }
    cudaEvent_t gpu_start() const { return gpu_start_; }
    cudaEvent_t gpu_stop() const { return gpu_stop_; }
    int stream_priority() const { return stream_priority_; }
    const std::string& device_name() const { return device_name_; }
    const std::vector<TensorBuffer>& tensors() const { return tensors_; }

private:
    void SelectProfileShapes()
    {
        const int32_t count = engine_->getNbIOTensors();
        for (int32_t index = 0; index < count; ++index) {
            const char* name = engine_->getIOTensorName(index);
            if (name == nullptr) {
                throw std::runtime_error("Engine returned a null tensor name");
            }
            if (engine_->getTensorIOMode(name) != nvinfer1::TensorIOMode::kINPUT) {
                continue;
            }
            if (engine_->isShapeInferenceIO(name)) {
                throw std::runtime_error(
                    "Shape-tensor inputs are unsupported by this focused probe: "
                    + std::string(name));
            }
            nvinfer1::Dims dims = engine_->getTensorShape(name);
            if (HasDynamicDimension(dims)) {
                dims = engine_->getProfileShape(
                    name, 0, nvinfer1::OptProfileSelector::kMAX);
                if (HasDynamicDimension(dims) || !context_->setInputShape(name, dims)) {
                    throw std::runtime_error(
                        "Cannot select profile-max shape for input: " + std::string(name));
                }
            }
        }
        const int32_t unresolved = context_->inferShapes(0, nullptr);
        if (unresolved != 0) {
            throw std::runtime_error(
                "TensorRT input/output shapes remain unresolved: "
                + std::to_string(unresolved));
        }
    }

    void AllocateAndBind()
    {
        const int32_t count = engine_->getNbIOTensors();
        tensors_.reserve(static_cast<size_t>(count));
        for (int32_t index = 0; index < count; ++index) {
            TensorBuffer tensor;
            const char* name = engine_->getIOTensorName(index);
            tensor.name = name;
            tensor.mode = engine_->getTensorIOMode(name);
            tensor.type = engine_->getTensorDataType(name);
            tensor.dims = context_->getTensorShape(name);
            if (engine_->getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE) {
                throw std::runtime_error(
                    "Host-location tensors are unsupported: " + tensor.name);
            }
            nvinfer1::Dims storage_dims = tensor.dims;
            const int32_t vectorized_dimension
                = engine_->getTensorVectorizedDim(name, 0);
            const size_t logical_volume = TensorVolume(storage_dims);
            if (vectorized_dimension >= 0) {
                const int32_t bytes_per_component
                    = engine_->getTensorBytesPerComponent(name, 0);
                const int32_t components
                    = engine_->getTensorComponentsPerElement(name, 0);
                if (bytes_per_component <= 0 || components <= 0) {
                    throw std::runtime_error(
                        "Invalid vectorized tensor storage metadata: " + tensor.name);
                }
                const int64_t scalar_dimension = storage_dims.d[vectorized_dimension];
                storage_dims.d[vectorized_dimension]
                    = (scalar_dimension + components - 1) / components;
                const size_t vector_volume = TensorVolume(storage_dims);
                const size_t vector_bytes
                    = static_cast<size_t>(bytes_per_component)
                    * static_cast<size_t>(components);
                if (vector_volume > std::numeric_limits<size_t>::max() / vector_bytes) {
                    throw std::runtime_error("Tensor byte count overflow: " + tensor.name);
                }
                tensor.bytes = vector_volume * vector_bytes;
            } else {
                const size_t bits = DataTypeBits(tensor.type);
                if (logical_volume > (std::numeric_limits<size_t>::max() - 7) / bits) {
                    throw std::runtime_error("Tensor bit count overflow: " + tensor.name);
                }
                tensor.bytes = (logical_volume * bits + 7) / 8;
            }
            CUDA_CHECK(cudaMalloc(&tensor.device, tensor.bytes));
            if (tensor.mode == nvinfer1::TensorIOMode::kINPUT) {
                CUDA_CHECK(cudaMemsetAsync(tensor.device, 0, tensor.bytes, stream_));
            } else if (tensor.mode == nvinfer1::TensorIOMode::kOUTPUT && copy_outputs_) {
                CUDA_CHECK(cudaHostAlloc(&tensor.host, tensor.bytes, cudaHostAllocDefault));
            }
            if (!context_->setTensorAddress(tensor.name.c_str(), tensor.device)) {
                throw std::runtime_error("setTensorAddress failed: " + tensor.name);
            }
            tensors_.push_back(std::move(tensor));
        }
    }

    void EnqueueWork()
    {
        if (!context_->enqueueV3(stream_)) {
            throw std::runtime_error("TensorRT enqueueV3 failed");
        }
        if (!copy_outputs_) {
            return;
        }
        for (const auto& tensor : tensors_) {
            if (tensor.mode == nvinfer1::TensorIOMode::kOUTPUT) {
                CUDA_CHECK(cudaMemcpyAsync(
                    tensor.host, tensor.device, tensor.bytes,
                    cudaMemcpyDeviceToHost, stream_));
            }
        }
    }

    void CaptureGraph()
    {
        CUDA_CHECK(cudaStreamSynchronize(stream_));
        CUDA_CHECK(cudaStreamBeginCapture(stream_, cudaStreamCaptureModeThreadLocal));
        EnqueueWork();
        const cudaError_t end_status = cudaStreamEndCapture(stream_, &graph_);
        if (end_status != cudaSuccess || graph_ == nullptr) {
            throw std::runtime_error(
                "Required CUDA graph capture failed: "
                + std::string(cudaGetErrorString(end_status)));
        }
        CUDA_CHECK(cudaGraphInstantiate(&graph_exec_, graph_, 0));
        if (graph_exec_ == nullptr) {
            throw std::runtime_error("Required CUDA graph instantiation returned null");
        }
    }

    bool copy_outputs_{true};
    nvinfer1::IRuntime* runtime_{nullptr};
    nvinfer1::ICudaEngine* engine_{nullptr};
    nvinfer1::IExecutionContext* context_{nullptr};
    cudaStream_t stream_{nullptr};
    cudaGraph_t graph_{nullptr};
    cudaGraphExec_t graph_exec_{nullptr};
    cudaEvent_t gpu_start_{nullptr};
    cudaEvent_t gpu_stop_{nullptr};
    std::vector<TensorBuffer> tensors_;
    int stream_priority_{0};
    std::string device_name_;
};

struct Row {
    uint64_t iteration{0};
    uint64_t scheduled_ns{0};
    uint64_t wake_ns{0};
    uint64_t submit_start_ns{0};
    uint64_t submit_end_ns{0};
    uint64_t sync_start_ns{0};
    uint64_t completion_ns{0};
    double start_deadline_lateness_us{0.0};
    double start_interval_us{0.0};
    double cpu_submit_us{0.0};
    double cpu_sync_us{0.0};
    double gpu_graph_ms{0.0};
    double service_us{0.0};
    double completion_vs_next_deadline_us{0.0};
    uint64_t missed_start_periods{0};
    bool deadline_missed{false};
};

double NsToUs(uint64_t later, uint64_t earlier)
{
    return static_cast<double>(later - earlier) / 1000.0;
}

double SignedNsToUs(int64_t value)
{
    return static_cast<double>(value) / 1000.0;
}

struct Summary {
    size_t count{0};
    double mean{0.0};
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double max{0.0};
};

Summary Summarize(std::vector<double> values)
{
    if (values.empty()) {
        return {};
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const size_t index = std::min(
            values.size() - 1,
            static_cast<size_t>(std::ceil(fraction * values.size())) - 1);
        return values[index];
    };
    return {
        values.size(), sum / static_cast<double>(values.size()),
        percentile(0.50), percentile(0.95), percentile(0.99), values.back()};
}

void WriteSummaryMetric(
    std::ostream& output, const std::string& name, const Summary& summary,
    bool trailing_comma)
{
    output << "    \"" << name << "\": {"
           << "\"count\": " << summary.count
           << ", \"mean\": " << summary.mean
           << ", \"p50\": " << summary.p50
           << ", \"p95\": " << summary.p95
           << ", \"p99\": " << summary.p99
           << ", \"max\": " << summary.max << "}"
           << (trailing_comma ? "," : "") << '\n';
}

void WriteSummaryJson(
    const Options& options, const TrtGraphHarness& harness,
    uint64_t period_ns, uint64_t measurement_start_ns,
    const std::vector<Row>& rows)
{
    if (options.summary_json.empty()) {
        return;
    }
    std::vector<double> start_lateness;
    std::vector<double> start_interval;
    std::vector<double> cpu_submit;
    std::vector<double> cpu_sync;
    std::vector<double> gpu_graph;
    std::vector<double> service;
    std::vector<double> completion_lateness;
    size_t deadline_misses = 0;
    for (const Row& row : rows) {
        start_lateness.push_back(row.start_deadline_lateness_us);
        if (row.iteration != 0) start_interval.push_back(row.start_interval_us);
        cpu_submit.push_back(row.cpu_submit_us);
        cpu_sync.push_back(row.cpu_sync_us);
        gpu_graph.push_back(row.gpu_graph_ms);
        service.push_back(row.service_us);
        completion_lateness.push_back(row.completion_vs_next_deadline_us);
        deadline_misses += row.deadline_missed ? 1U : 0U;
    }

    std::ofstream output(options.summary_json);
    if (!output) {
        throw std::runtime_error("Cannot open summary JSON: " + options.summary_json);
    }
    output << std::fixed << std::setprecision(6);
    output << "{\n"
           << "  \"label\": \"" << JsonEscape(options.label) << "\",\n"
           << "  \"engine\": \"" << JsonEscape(options.engine) << "\",\n"
           << "  \"device\": " << options.gpu_id << ",\n"
           << "  \"device_name\": \"" << JsonEscape(harness.device_name()) << "\",\n"
           << "  \"stream_priority\": " << harness.stream_priority() << ",\n"
           << "  \"cuda_graph_required\": true,\n"
           << "  \"copy_outputs\": " << (options.copy_outputs ? "true" : "false") << ",\n"
           << "  \"fps\": " << options.fps << ",\n"
           << "  \"period_ns\": " << period_ns << ",\n"
           << "  \"warmup_iterations\": " << options.warmup << ",\n"
           << "  \"measured_iterations\": " << options.iterations << ",\n"
           << "  \"measurement_start_monotonic_ns\": " << measurement_start_ns << ",\n"
           << "  \"deadline_misses\": " << deadline_misses << ",\n"
           << "  \"metrics\": {\n";
    WriteSummaryMetric(output, "start_deadline_lateness_us", Summarize(start_lateness), true);
    WriteSummaryMetric(output, "start_interval_us", Summarize(start_interval), true);
    WriteSummaryMetric(output, "cpu_submit_us", Summarize(cpu_submit), true);
    WriteSummaryMetric(output, "cpu_sync_us", Summarize(cpu_sync), true);
    WriteSummaryMetric(output, "gpu_graph_ms", Summarize(gpu_graph), true);
    WriteSummaryMetric(output, "service_us", Summarize(service), true);
    WriteSummaryMetric(
        output, "completion_vs_next_deadline_us",
        Summarize(completion_lateness), false);
    output << "  }\n}\n";
}

int Main(int argc, char** argv)
{
    const Options options = ParseOptions(argc, argv);
    const uint64_t period_ns = static_cast<uint64_t>(
        std::llround(1000000000.0 / options.fps));
    if (period_ns == 0) {
        throw std::runtime_error("Requested FPS produces a zero period");
    }

    Logger logger;
    TrtGraphHarness harness(options, logger);
    std::cout << "engine=" << options.engine << '\n'
              << "device=" << options.gpu_id << " name=" << harness.device_name() << '\n'
              << "stream_priority=" << harness.stream_priority()
              << " (must remain identical across comparison conditions)\n"
              << "cuda_graph=required output_copy="
              << (options.copy_outputs ? "on" : "off") << '\n'
              << "fps=" << options.fps << " period_ns=" << period_ns
              << " warmup=" << options.warmup
              << " iterations=" << options.iterations << '\n';
    for (const auto& tensor : harness.tensors()) {
        std::cout << "tensor name=" << tensor.name
                  << " mode=" << static_cast<int>(tensor.mode)
                  << " dtype=" << static_cast<int>(tensor.type)
                  << " dims=" << DimsString(tensor.dims)
                  << " bytes=" << tensor.bytes << '\n';
    }

    // Warm up at the same cadence used for measurement so fixed-rate driver
    // and scheduler behavior is established before the first retained row.
    // An explicit measurement deadline also pins the warmup immediately before
    // that deadline instead of warming early and idling until the coordinated
    // encoder/inference phase point.
    uint64_t measurement_start_ns = options.start_at_monotonic_ns;
    uint64_t warmup_start = 0;
    if (measurement_start_ns == 0) {
        warmup_start = MonotonicNs() + period_ns;
    } else {
        if (options.warmup > std::numeric_limits<uint64_t>::max() / period_ns) {
            throw std::runtime_error("Warmup timeline overflows uint64_t");
        }
        const uint64_t warmup_span_ns = options.warmup * period_ns;
        if (measurement_start_ns <= warmup_span_ns) {
            throw std::runtime_error(
                "--start-at-monotonic-ns is smaller than the warmup timeline");
        }
        warmup_start = measurement_start_ns - warmup_span_ns;
        if (warmup_start <= MonotonicNs()) {
            throw std::runtime_error(
                "--start-at-monotonic-ns does not leave the paced warmup in the future");
        }
    }
    for (uint64_t iteration = 0; iteration < options.warmup; ++iteration) {
        SleepUntilNs(warmup_start + iteration * period_ns);
        harness.RunGraphAndSynchronize();
    }

    if (measurement_start_ns == 0) {
        measurement_start_ns = MonotonicNs() + period_ns;
    } else if (measurement_start_ns <= MonotonicNs()) {
        throw std::runtime_error(
            "--start-at-monotonic-ns is not in the future after initialization/warmup");
    }

    std::ofstream csv(options.csv);
    if (!csv) {
        throw std::runtime_error("Cannot open CSV: " + options.csv);
    }
    csv << "iteration,label,scheduled_monotonic_ns,wake_monotonic_ns,"
           "submit_start_monotonic_ns,submit_end_monotonic_ns,"
           "sync_start_monotonic_ns,completion_monotonic_ns,"
           "start_deadline_lateness_us,start_interval_us,cpu_submit_us,"
           "cpu_sync_us,gpu_graph_ms,service_us,"
           "completion_vs_next_deadline_us,missed_start_periods,deadline_missed\n";
    csv << std::fixed << std::setprecision(6);

    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(options.iterations));
    uint64_t previous_submit_start_ns = 0;
    for (uint64_t iteration = 0; iteration < options.iterations; ++iteration) {
        Row row;
        row.iteration = iteration;
        row.scheduled_ns = measurement_start_ns + iteration * period_ns;
        SleepUntilNs(row.scheduled_ns);
        row.wake_ns = MonotonicNs();

        CUDA_CHECK(cudaEventRecord(harness.gpu_start(), harness.stream()));
        row.submit_start_ns = MonotonicNs();
        CUDA_CHECK(cudaGraphLaunch(harness.graph_exec(), harness.stream()));
        CUDA_CHECK(cudaEventRecord(harness.gpu_stop(), harness.stream()));
        row.submit_end_ns = MonotonicNs();
        row.sync_start_ns = MonotonicNs();
        CUDA_CHECK(cudaEventSynchronize(harness.gpu_stop()));
        row.completion_ns = MonotonicNs();

        float gpu_ms = 0.0F;
        CUDA_CHECK(cudaEventElapsedTime(
            &gpu_ms, harness.gpu_start(), harness.gpu_stop()));
        const int64_t start_lateness_ns
            = static_cast<int64_t>(row.submit_start_ns)
            - static_cast<int64_t>(row.scheduled_ns);
        const uint64_t next_deadline_ns = row.scheduled_ns + period_ns;
        const int64_t completion_lateness_ns
            = static_cast<int64_t>(row.completion_ns)
            - static_cast<int64_t>(next_deadline_ns);

        row.start_deadline_lateness_us = SignedNsToUs(start_lateness_ns);
        row.start_interval_us = previous_submit_start_ns == 0
            ? 0.0 : NsToUs(row.submit_start_ns, previous_submit_start_ns);
        row.cpu_submit_us = NsToUs(row.submit_end_ns, row.submit_start_ns);
        row.cpu_sync_us = NsToUs(row.completion_ns, row.sync_start_ns);
        row.gpu_graph_ms = static_cast<double>(gpu_ms);
        row.service_us = NsToUs(row.completion_ns, row.submit_start_ns);
        row.completion_vs_next_deadline_us
            = SignedNsToUs(completion_lateness_ns);
        row.missed_start_periods = start_lateness_ns > 0
            ? static_cast<uint64_t>(start_lateness_ns) / period_ns : 0;
        row.deadline_missed = completion_lateness_ns > 0;
        previous_submit_start_ns = row.submit_start_ns;

        csv << row.iteration << ',' << CsvEscape(options.label) << ','
            << row.scheduled_ns << ',' << row.wake_ns << ','
            << row.submit_start_ns << ',' << row.submit_end_ns << ','
            << row.sync_start_ns << ',' << row.completion_ns << ','
            << row.start_deadline_lateness_us << ',' << row.start_interval_us << ','
            << row.cpu_submit_us << ',' << row.cpu_sync_us << ','
            << row.gpu_graph_ms << ',' << row.service_us << ','
            << row.completion_vs_next_deadline_us << ','
            << row.missed_start_periods << ','
            << (row.deadline_missed ? 1 : 0) << '\n';
        rows.push_back(row);
    }
    csv.close();
    if (!csv) {
        throw std::runtime_error("Failed while writing CSV: " + options.csv);
    }

    WriteSummaryJson(options, harness, period_ns, measurement_start_ns, rows);
    const Summary gpu = Summarize([&] {
        std::vector<double> values;
        values.reserve(rows.size());
        for (const auto& row : rows) values.push_back(row.gpu_graph_ms);
        return values;
    }());
    const size_t misses = static_cast<size_t>(std::count_if(
        rows.begin(), rows.end(), [](const Row& row) { return row.deadline_missed; }));
    std::cout << std::fixed << std::setprecision(6)
              << "measurement_start_monotonic_ns=" << measurement_start_ns << '\n'
              << "gpu_graph_ms mean=" << gpu.mean << " p95=" << gpu.p95
              << " p99=" << gpu.p99 << " max=" << gpu.max << '\n'
              << "deadline_misses=" << misses << '/' << rows.size() << '\n'
              << "csv=" << options.csv << '\n';
    if (!options.summary_json.empty()) {
        std::cout << "summary_json=" << options.summary_json << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        return Main(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "trt_contention_probe: " << error.what() << '\n';
        return 1;
    }
}
