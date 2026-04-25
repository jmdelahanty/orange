#include "NvEncoder/NvEncoderCuda.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested{false};

struct Options {
    int gpu_id = 0;
    uint32_t width = 4512;
    uint32_t height = 4512;
    uint32_t fps = 60;
    uint32_t duration_seconds = 0;
    std::string codec = "hevc";
    std::string preset = "p1";
    std::string tuning = "ll";
    uint32_t gop = 25;
    uint32_t bitrate_bps = 150000000;
    uint32_t max_bitrate_bps = 150000000;
    uint32_t vbv_buffer_size = 150000000;
    uint32_t extra_output_delay = 3;
    bool monochrome = true;
    bool pace = true;
    std::string pattern = "solid";
    std::string raw_file_path;
    uint32_t raw_pitch = 0;
    uint64_t raw_frame_bytes = 0;
    uint32_t raw_cache_frames = 16;
    std::string bitstream_out_path;
    std::string csv_path;
};

struct FrameSample {
    uint64_t frame_index = 0;
    double elapsed_ms = 0.0;
    double fill_enqueue_ms = 0.0;
    double encode_total_ms = 0.0;
    double encode_picture_ms = 0.0;
    double completion_wait_ms = 0.0;
    double lock_bitstream_ms = 0.0;
    double bitstream_copy_ms = 0.0;
    double unlock_bitstream_ms = 0.0;
    double unmap_input_resource_ms = 0.0;
    double bitstream_fetch_ms = 0.0;
    uint32_t output_packets = 0;
    uint64_t output_bytes = 0;
    uint32_t returned_packets = 0;
    uint64_t returned_bytes = 0;
    double sleep_ms = 0.0;
    double late_ms = 0.0;
};

void signal_handler(int)
{
    g_stop_requested.store(true, std::memory_order_release);
}

double ns_to_ms(uint64_t ns)
{
    return static_cast<double>(ns) / 1000000.0;
}

uint64_t elapsed_ns(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[noreturn]] void usage(const char* argv0, int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out
        << "Usage:\n"
        << "  " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --gpu-id <int>              CUDA device id. Default 0.\n"
        << "  --width <int>               Encode width. Default 4512.\n"
        << "  --height <int>              Encode height. Default 4512.\n"
        << "  --fps <int>                 Target pacing FPS. Default 60.\n"
        << "  --duration <sec>            Duration. 0 means run until signal. Default 0.\n"
        << "  --codec <hevc|h264>         Default hevc.\n"
        << "  --preset <p1|p3|p5|p7>      Default p1.\n"
        << "  --tuning <ll|ull|hq|lossless> Default ll.\n"
        << "  --gop <int>                 GOP length. Default 25.\n"
        << "  --bitrate-bps <int>         Average bitrate. Default 150000000.\n"
        << "  --max-bitrate-bps <int>     Max bitrate. Default 150000000.\n"
        << "  --vbv-buffer-size <int>     VBV buffer size. Default 150000000.\n"
        << "  --extra-output-delay <int>  NvEncoder extra output delay. Default 3.\n"
        << "  --pattern <solid|host-noise|raw-file>\n"
        << "                              Input pattern. solid uses device memset; host-noise copies one of several pinned random NV12 frames; raw-file loops cached NV12 frames. Default solid.\n"
        << "  --raw-file <path>           Required for --pattern raw-file. Raw NV12 frame dump.\n"
        << "  --raw-pitch <int>           Source pitch for raw-file frames. Default width.\n"
        << "  --raw-frame-bytes <int>     Source bytes per raw-file frame. Default pitch * height * 3 / 2.\n"
        << "  --raw-cache-frames <int>    Max raw frames to cache in pinned host memory. Default 16.\n"
        << "  --bitstream-out <path>      Optional raw elementary stream output.\n"
        << "  --csv <path>                Optional per-frame timing CSV.\n"
        << "  --monochrome                Enable NVENC monochrome encoding. Default.\n"
        << "  --no-monochrome             Disable monochrome encoding.\n"
        << "  --no-pace                   Run as fast as possible instead of FPS pacing.\n"
        << "  --help\n";
    std::exit(exit_code);
}

uint32_t parse_u32(const std::string& value, const char* name)
{
    if (value.empty()) {
        throw std::runtime_error(std::string("Missing value for ") + name);
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("Invalid ") + name + " value: " + value);
    }
    return static_cast<uint32_t>(parsed);
}

int parse_i32(const std::string& value, const char* name)
{
    if (value.empty()) {
        throw std::runtime_error(std::string("Missing value for ") + name);
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Invalid ") + name + " value: " + value);
    }
    return static_cast<int>(parsed);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string inline_value;
        const std::size_t equals = arg.find('=');
        if (arg.rfind("--", 0) == 0 && equals != std::string::npos) {
            inline_value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }
        auto consume = [&](const char* name) -> std::string {
            if (!inline_value.empty()) {
                return inline_value;
            }
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            usage(argv[0], 0);
        } else if (arg == "--gpu-id" || arg == "--gpu") {
            options.gpu_id = parse_i32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--width") {
            options.width = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--height") {
            options.height = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--fps") {
            options.fps = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--duration") {
            options.duration_seconds = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--codec") {
            options.codec = lower_ascii(consume(arg.c_str()));
        } else if (arg == "--preset") {
            options.preset = lower_ascii(consume(arg.c_str()));
        } else if (arg == "--tuning") {
            options.tuning = lower_ascii(consume(arg.c_str()));
        } else if (arg == "--gop") {
            options.gop = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--bitrate-bps") {
            options.bitrate_bps = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--max-bitrate-bps") {
            options.max_bitrate_bps = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--vbv-buffer-size") {
            options.vbv_buffer_size = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--extra-output-delay") {
            options.extra_output_delay = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--pattern") {
            options.pattern = lower_ascii(consume(arg.c_str()));
        } else if (arg == "--raw-file") {
            options.raw_file_path = consume(arg.c_str());
        } else if (arg == "--raw-pitch") {
            options.raw_pitch = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--raw-frame-bytes") {
            options.raw_frame_bytes = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--raw-cache-frames") {
            options.raw_cache_frames = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--bitstream-out") {
            options.bitstream_out_path = consume(arg.c_str());
        } else if (arg == "--csv") {
            options.csv_path = consume(arg.c_str());
        } else if (arg == "--monochrome") {
            options.monochrome = true;
        } else if (arg == "--no-monochrome") {
            options.monochrome = false;
        } else if (arg == "--no-pace") {
            options.pace = false;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.width == 0 || options.height == 0) {
        throw std::runtime_error("--width and --height must be positive");
    }
    if (options.fps == 0 && options.pace) {
        throw std::runtime_error("--fps must be positive unless --no-pace is used");
    }
    if (options.codec != "hevc" && options.codec != "h264") {
        throw std::runtime_error("--codec must be hevc or h264");
    }
    if (options.preset != "p1" && options.preset != "p3" &&
        options.preset != "p5" && options.preset != "p7") {
        throw std::runtime_error("--preset must be p1, p3, p5, or p7");
    }
    if (options.tuning != "ll" && options.tuning != "ull" &&
        options.tuning != "hq" && options.tuning != "lossless") {
        throw std::runtime_error("--tuning must be ll, ull, hq, or lossless");
    }
    if (options.extra_output_delay > 64) {
        throw std::runtime_error("--extra-output-delay must be <= 64");
    }
    if (options.pattern != "solid" &&
        options.pattern != "host-noise" &&
        options.pattern != "raw-file") {
        throw std::runtime_error("--pattern must be solid, host-noise, or raw-file");
    }
    if (options.raw_pitch == 0) {
        options.raw_pitch = options.width;
    }
    if (options.raw_frame_bytes == 0) {
        options.raw_frame_bytes =
            static_cast<uint64_t>(options.raw_pitch) *
            static_cast<uint64_t>(options.height) * 3ULL / 2ULL;
    }
    if (options.pattern == "raw-file") {
        if (options.raw_file_path.empty()) {
            throw std::runtime_error("--pattern raw-file requires --raw-file");
        }
        if (options.raw_pitch < options.width) {
            throw std::runtime_error("--raw-pitch must be >= --width");
        }
        const uint64_t minimum_frame_bytes =
            static_cast<uint64_t>(options.raw_pitch) *
            static_cast<uint64_t>(options.height) * 3ULL / 2ULL;
        if (options.raw_frame_bytes < minimum_frame_bytes) {
            throw std::runtime_error("--raw-frame-bytes is smaller than pitch * height * 3 / 2");
        }
        if (options.raw_cache_frames == 0) {
            throw std::runtime_error("--raw-cache-frames must be positive");
        }
    }
    return options;
}

void check_cuda(cudaError_t status, const char* call)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(call) + " failed: " + cudaGetErrorString(status));
    }
}

void check_cu(CUresult status, const char* call)
{
    if (status != CUDA_SUCCESS) {
        const char* name = nullptr;
        cuGetErrorName(status, &name);
        throw std::runtime_error(
            std::string(call) + " failed: " + (name ? name : "unknown"));
    }
}

GUID resolve_codec_guid(const std::string& codec)
{
    return codec == "hevc" ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
}

GUID resolve_preset_guid(const std::string& preset)
{
    if (preset == "p1") {
        return NV_ENC_PRESET_P1_GUID;
    }
    if (preset == "p5") {
        return NV_ENC_PRESET_P5_GUID;
    }
    if (preset == "p7") {
        return NV_ENC_PRESET_P7_GUID;
    }
    return NV_ENC_PRESET_P3_GUID;
}

NV_ENC_TUNING_INFO resolve_tuning_info(const std::string& tuning)
{
    if (tuning == "ll") {
        return NV_ENC_TUNING_INFO_LOW_LATENCY;
    }
    if (tuning == "ull") {
        return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    }
    if (tuning == "lossless") {
        return NV_ENC_TUNING_INFO_LOSSLESS;
    }
    return NV_ENC_TUNING_INFO_HIGH_QUALITY;
}

void configure_encoder_params(const Options& options,
                              NV_ENC_INITIALIZE_PARAMS* initialize_params,
                              NV_ENC_CONFIG* encode_config,
                              NvEncoderCuda* encoder)
{
    *initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    *encode_config = {NV_ENC_CONFIG_VER};
    initialize_params->encodeConfig = encode_config;

    encoder->CreateDefaultEncoderParams(
        initialize_params,
        resolve_codec_guid(options.codec),
        resolve_preset_guid(options.preset),
        resolve_tuning_info(options.tuning));

    initialize_params->encodeWidth = options.width;
    initialize_params->encodeHeight = options.height;
    initialize_params->darWidth = options.width;
    initialize_params->darHeight = options.height;
    initialize_params->frameRateNum = std::max<uint32_t>(1, options.fps);
    initialize_params->frameRateDen = 1;
    initialize_params->enablePTD = 1;

    encode_config->gopLength = options.gop;
    encode_config->frameIntervalP = 1;

    if (options.tuning == "lossless") {
        encode_config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encode_config->rcParams.constQP = {0, 0, 0};
        encode_config->rcParams.averageBitRate = 0;
        encode_config->rcParams.maxBitRate = 0;
        encode_config->rcParams.vbvBufferSize = 0;
        encode_config->rcParams.enableAQ = 0;
        encode_config->rcParams.enableTemporalAQ = 0;
        encode_config->rcParams.enableLookahead = 0;
        encode_config->rcParams.lowDelayKeyFrameScale = 0;
        encode_config->gopLength = 1;
    } else {
        const bool low_latency = options.tuning == "ll" || options.tuning == "ull";
        encode_config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encode_config->rcParams.averageBitRate = options.bitrate_bps;
        encode_config->rcParams.maxBitRate = std::max(options.max_bitrate_bps, options.bitrate_bps);
        encode_config->rcParams.vbvBufferSize =
            options.vbv_buffer_size > 0 ? options.vbv_buffer_size : encode_config->rcParams.maxBitRate;
        encode_config->rcParams.enableAQ = 1;
        encode_config->rcParams.enableTemporalAQ = 1;
        encode_config->rcParams.enableLookahead = low_latency ? 0 : 1;
        encode_config->rcParams.lowDelayKeyFrameScale = low_latency ? 1 : 0;
    }

    if (options.monochrome) {
        encode_config->monoChromeEncoding = 1;
    }
}

void fill_input_frame(const NvEncInputFrame& frame,
                      uint32_t width,
                      uint32_t height,
                      uint8_t y_value,
                      cudaStream_t stream)
{
    auto* base = static_cast<uint8_t*>(frame.inputPtr);
    check_cuda(
        cudaMemset2DAsync(base, frame.pitch, y_value, width, height, stream),
        "cudaMemset2DAsync(luma)");

    const uint32_t chroma_width =
        NvEncoder::GetChromaWidthInBytes(frame.bufferFormat, width);
    const uint32_t chroma_height =
        NvEncoder::GetChromaHeight(frame.bufferFormat, height);
    for (uint32_t plane = 0; plane < frame.numChromaPlanes; ++plane) {
        check_cuda(
            cudaMemset2DAsync(
                base + frame.chromaOffsets[plane],
                frame.chromaPitch,
                128,
                chroma_width,
                chroma_height,
                stream),
            "cudaMemset2DAsync(chroma)");
    }
}

struct PinnedHostFrame {
    uint8_t* ptr = nullptr;
    size_t bytes = 0;

    PinnedHostFrame() = default;
    PinnedHostFrame(const PinnedHostFrame&) = delete;
    PinnedHostFrame& operator=(const PinnedHostFrame&) = delete;

    PinnedHostFrame(PinnedHostFrame&& other) noexcept
        : ptr(other.ptr), bytes(other.bytes)
    {
        other.ptr = nullptr;
        other.bytes = 0;
    }

    PinnedHostFrame& operator=(PinnedHostFrame&& other) noexcept
    {
        if (this != &other) {
            if (ptr) {
                cudaFreeHost(ptr);
            }
            ptr = other.ptr;
            bytes = other.bytes;
            other.ptr = nullptr;
            other.bytes = 0;
        }
        return *this;
    }

    ~PinnedHostFrame()
    {
        if (ptr) {
            cudaFreeHost(ptr);
        }
    }
};

std::vector<PinnedHostFrame> make_host_noise_frames(uint32_t width,
                                                    uint32_t height,
                                                    size_t frame_count)
{
    const size_t luma_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t chroma_size = luma_size / 2;
    const size_t bytes = luma_size + chroma_size;
    std::vector<PinnedHostFrame> frames;
    frames.reserve(frame_count);

    std::mt19937 rng(0x0a16f00dU);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
        PinnedHostFrame frame;
        check_cuda(cudaHostAlloc(reinterpret_cast<void**>(&frame.ptr), bytes, cudaHostAllocDefault),
                   "cudaHostAlloc(host-noise frame)");
        frame.bytes = bytes;

        for (size_t i = 0; i < luma_size; ++i) {
            frame.ptr[i] = static_cast<uint8_t>(dist(rng));
        }
        std::memset(frame.ptr + luma_size, 128, chroma_size);
        frames.push_back(std::move(frame));
    }
    return frames;
}

void copy_host_frame_to_input(CUcontext cu_context,
                              const uint8_t* host_frame,
                              uint32_t host_pitch,
                              const NvEncInputFrame& frame,
                              uint32_t width,
                              uint32_t height,
                              cudaStream_t stream)
{
    NvEncoderCuda::CopyToDeviceFrame(
        cu_context,
        const_cast<uint8_t*>(host_frame),
        host_pitch,
        reinterpret_cast<CUdeviceptr>(frame.inputPtr),
        frame.pitch,
        static_cast<int>(width),
        static_cast<int>(height),
        CU_MEMORYTYPE_HOST,
        frame.bufferFormat,
        frame.chromaOffsets,
        frame.numChromaPlanes,
        false,
        reinterpret_cast<CUstream>(stream));
}

std::vector<PinnedHostFrame> load_raw_file_frames(const std::string& path,
                                                  uint64_t frame_bytes,
                                                  uint32_t max_frames)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open raw input file: " + path);
    }
    if (frame_bytes == 0 ||
        frame_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("Invalid raw frame byte count");
    }

    std::vector<PinnedHostFrame> frames;
    frames.reserve(max_frames);
    const size_t bytes = static_cast<size_t>(frame_bytes);
    for (uint32_t frame_idx = 0; frame_idx < max_frames; ++frame_idx) {
        PinnedHostFrame frame;
        check_cuda(cudaHostAlloc(reinterpret_cast<void**>(&frame.ptr), bytes, cudaHostAllocDefault),
                   "cudaHostAlloc(raw-file frame)");
        frame.bytes = bytes;

        input.read(reinterpret_cast<char*>(frame.ptr), static_cast<std::streamsize>(bytes));
        const std::streamsize read_count = input.gcount();
        if (read_count == 0 && input.eof()) {
            break;
        }
        if (read_count != static_cast<std::streamsize>(bytes)) {
            throw std::runtime_error("Raw input ended with a partial frame: " + path);
        }
        frames.push_back(std::move(frame));
    }

    if (frames.empty()) {
        throw std::runtime_error("Raw input contained no complete frames: " + path);
    }
    return frames;
}

void write_csv_header(std::ofstream& csv)
{
    csv << "frame_index,elapsed_ms,fill_enqueue_ms,encode_total_ms,"
           "encode_picture_ms,completion_wait_ms,lock_bitstream_ms,"
           "bitstream_copy_ms,unlock_bitstream_ms,unmap_input_resource_ms,"
           "bitstream_fetch_ms,output_packets,output_bytes,returned_packets,"
           "returned_bytes,sleep_ms,late_ms\n";
}

void write_csv_row(std::ofstream& csv, const FrameSample& sample)
{
    csv << sample.frame_index << ","
        << std::fixed << std::setprecision(6)
        << sample.elapsed_ms << ","
        << sample.fill_enqueue_ms << ","
        << sample.encode_total_ms << ","
        << sample.encode_picture_ms << ","
        << sample.completion_wait_ms << ","
        << sample.lock_bitstream_ms << ","
        << sample.bitstream_copy_ms << ","
        << sample.unlock_bitstream_ms << ","
        << sample.unmap_input_resource_ms << ","
        << sample.bitstream_fetch_ms << ","
        << sample.output_packets << ","
        << sample.output_bytes << ","
        << sample.returned_packets << ","
        << sample.returned_bytes << ","
        << sample.sleep_ms << ","
        << sample.late_ms << "\n";
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(std::ceil(values.size() * p / 100.0)) - 1);
    return values[index];
}

double mean(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

void print_summary(const Options& options,
                   const std::vector<FrameSample>& samples,
                   uint64_t total_packets,
                   uint64_t total_bytes,
                   double total_elapsed_s)
{
    std::vector<double> encode_total;
    std::vector<double> encode_picture;
    std::vector<double> lock_bitstream;
    std::vector<double> bitstream_fetch;
    std::vector<double> late;
    encode_total.reserve(samples.size());
    encode_picture.reserve(samples.size());
    lock_bitstream.reserve(samples.size());
    bitstream_fetch.reserve(samples.size());
    late.reserve(samples.size());
    for (const FrameSample& sample : samples) {
        encode_total.push_back(sample.encode_total_ms);
        encode_picture.push_back(sample.encode_picture_ms);
        lock_bitstream.push_back(sample.lock_bitstream_ms);
        bitstream_fetch.push_back(sample.bitstream_fetch_ms);
        late.push_back(sample.late_ms);
    }

    const double fps = total_elapsed_s > 0.0
        ? static_cast<double>(samples.size()) / total_elapsed_s
        : 0.0;
    std::cout << "NVENC stress summary\n"
              << "  gpu_id=" << options.gpu_id << "\n"
              << "  resolution=" << options.width << "x" << options.height << "\n"
              << "  target_fps=" << options.fps << "\n"
              << "  achieved_fps=" << std::fixed << std::setprecision(3) << fps << "\n"
              << "  frames=" << samples.size() << "\n"
              << "  packets=" << total_packets << "\n"
              << "  bytes=" << total_bytes << "\n"
              << "  encode_total_ms_mean=" << mean(encode_total)
              << " p95=" << percentile(encode_total, 95.0)
              << " max=" << (encode_total.empty() ? 0.0 : *std::max_element(encode_total.begin(), encode_total.end())) << "\n"
              << "  encode_picture_ms_mean=" << mean(encode_picture)
              << " p95=" << percentile(encode_picture, 95.0)
              << " max=" << (encode_picture.empty() ? 0.0 : *std::max_element(encode_picture.begin(), encode_picture.end())) << "\n"
              << "  lock_bitstream_ms_mean=" << mean(lock_bitstream)
              << " p95=" << percentile(lock_bitstream, 95.0)
              << " max=" << (lock_bitstream.empty() ? 0.0 : *std::max_element(lock_bitstream.begin(), lock_bitstream.end())) << "\n"
              << "  bitstream_fetch_ms_mean=" << mean(bitstream_fetch)
              << " p95=" << percentile(bitstream_fetch, 95.0)
              << " max=" << (bitstream_fetch.empty() ? 0.0 : *std::max_element(bitstream_fetch.begin(), bitstream_fetch.end())) << "\n"
              << "  late_ms_p95=" << percentile(late, 95.0)
              << " max=" << (late.empty() ? 0.0 : *std::max_element(late.begin(), late.end())) << "\n";
    if (!options.csv_path.empty()) {
        std::cout << "  csv=" << options.csv_path << "\n";
    }
    if (!options.bitstream_out_path.empty()) {
        std::cout << "  bitstream_out=" << options.bitstream_out_path << "\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parse_options(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        check_cu(cuInit(0), "cuInit");
        check_cuda(cudaSetDevice(options.gpu_id), "cudaSetDevice");
        check_cuda(cudaFree(nullptr), "cudaFree(0)");

        CUcontext cu_context = nullptr;
        check_cu(cuCtxGetCurrent(&cu_context), "cuCtxGetCurrent");
        if (!cu_context) {
            throw std::runtime_error("No current CUDA context after cudaSetDevice");
        }

        cudaStream_t stream = nullptr;
        check_cuda(
            cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
            "cudaStreamCreateWithFlags");

        NvEncoderCuda encoder(
            cu_context,
            options.width,
            options.height,
            NV_ENC_BUFFER_FORMAT_NV12,
            options.extra_output_delay);
        NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        configure_encoder_params(options, &initialize_params, &encode_config, &encoder);
        encoder.CreateEncoder(&initialize_params);
        encoder.SetIOCudaStreams(
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream),
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream));

        std::ofstream csv;
        if (!options.csv_path.empty()) {
            csv.open(options.csv_path);
            if (!csv) {
                throw std::runtime_error("Failed to open CSV output: " + options.csv_path);
            }
            write_csv_header(csv);
        }

        std::cout << "NVENC stress load started"
                  << " gpu_id=" << options.gpu_id
                  << " resolution=" << options.width << "x" << options.height
                  << " fps=" << options.fps
                  << " duration_s=" << options.duration_seconds
              << " codec=" << options.codec
              << " preset=" << options.preset
              << " tuning=" << options.tuning
              << " pattern=" << options.pattern
              << " monochrome=" << (options.monochrome ? "true" : "false")
              << std::endl;

        std::vector<PinnedHostFrame> host_noise_frames;
        if (options.pattern == "host-noise") {
            host_noise_frames = make_host_noise_frames(options.width, options.height, 4);
            std::cout << "NVENC host-noise frames prepared"
                      << " count=" << host_noise_frames.size()
                      << " bytes_each=" << host_noise_frames.front().bytes
                      << std::endl;
        }

        std::vector<PinnedHostFrame> raw_file_frames;
        if (options.pattern == "raw-file") {
            raw_file_frames = load_raw_file_frames(
                options.raw_file_path,
                options.raw_frame_bytes,
                options.raw_cache_frames);
            std::cout << "NVENC raw-file frames prepared"
                      << " path=" << options.raw_file_path
                      << " count=" << raw_file_frames.size()
                      << " pitch=" << options.raw_pitch
                      << " bytes_each=" << raw_file_frames.front().bytes
                      << std::endl;
        }

        std::vector<FrameSample> samples;
        samples.reserve(options.duration_seconds > 0
            ? static_cast<size_t>(options.duration_seconds) * std::max<uint32_t>(1, options.fps)
            : 4096);

        std::ofstream bitstream_out;
        if (!options.bitstream_out_path.empty()) {
            bitstream_out.open(options.bitstream_out_path, std::ios::binary | std::ios::trunc);
            if (!bitstream_out) {
                throw std::runtime_error("Failed to open bitstream output: " + options.bitstream_out_path);
            }
        }

        std::vector<std::vector<uint8_t>> packets;
        const auto run_start = std::chrono::steady_clock::now();
        const uint64_t max_frames = options.duration_seconds > 0
            ? static_cast<uint64_t>(options.duration_seconds) * std::max<uint32_t>(1, options.fps)
            : std::numeric_limits<uint64_t>::max();
        const auto frame_period = options.fps > 0
            ? std::chrono::nanoseconds(1000000000LL / static_cast<int64_t>(options.fps))
            : std::chrono::nanoseconds(0);

        uint64_t total_packets = 0;
        uint64_t total_bytes = 0;

        for (uint64_t frame_index = 0;
             frame_index < max_frames && !g_stop_requested.load(std::memory_order_acquire);
             ++frame_index) {
            FrameSample sample;
            sample.frame_index = frame_index;

            const auto fill_start = std::chrono::steady_clock::now();
            const NvEncInputFrame* input_frame = encoder.GetNextInputFrame();
            if (!input_frame || !input_frame->inputPtr) {
                throw std::runtime_error("NvEncoder returned no input frame");
            }
            if (options.pattern == "host-noise") {
                const PinnedHostFrame& host_frame =
                    host_noise_frames[frame_index % host_noise_frames.size()];
                copy_host_frame_to_input(
                    cu_context,
                    host_frame.ptr,
                    options.width,
                    *input_frame,
                    options.width,
                    options.height,
                    stream);
            } else if (options.pattern == "raw-file") {
                const PinnedHostFrame& raw_frame =
                    raw_file_frames[frame_index % raw_file_frames.size()];
                copy_host_frame_to_input(
                    cu_context,
                    raw_frame.ptr,
                    options.raw_pitch,
                    *input_frame,
                    options.width,
                    options.height,
                    stream);
            } else {
                fill_input_frame(
                    *input_frame,
                    options.width,
                    options.height,
                    static_cast<uint8_t>(16 + (frame_index % 200)),
                    stream);
            }
            sample.fill_enqueue_ms = ns_to_ms(elapsed_ns(fill_start));

            NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
            pic_params.frameIdx = static_cast<uint32_t>(frame_index & 0xffffffffu);
            pic_params.inputTimeStamp = frame_index;
            pic_params.inputDuration = 1;

            NvEncoderEncodeFrameTiming timing;
            uint64_t fetch_ns = 0;
            const auto encode_start = std::chrono::steady_clock::now();
            encoder.EncodeFrame(
                packets,
                &pic_params,
                nullptr,
                nullptr,
                &fetch_ns,
                &timing);
            sample.encode_total_ms = ns_to_ms(elapsed_ns(encode_start));
            sample.encode_picture_ms = ns_to_ms(timing.encode_picture_ns);
            sample.completion_wait_ms = ns_to_ms(timing.completion_wait_ns);
            sample.lock_bitstream_ms = ns_to_ms(timing.lock_bitstream_ns);
            sample.bitstream_copy_ms = ns_to_ms(timing.bitstream_copy_ns);
            sample.unlock_bitstream_ms = ns_to_ms(timing.unlock_bitstream_ns);
            sample.unmap_input_resource_ms = ns_to_ms(timing.unmap_input_resource_ns);
            sample.bitstream_fetch_ms = ns_to_ms(fetch_ns > 0 ? fetch_ns : timing.bitstream_fetch_ns);
            sample.output_packets = timing.output_packets;
            sample.output_bytes = timing.output_bytes;
            sample.returned_packets = static_cast<uint32_t>(packets.size());
            for (const auto& packet : packets) {
                sample.returned_bytes += packet.size();
                if (bitstream_out && !packet.empty()) {
                    bitstream_out.write(
                        reinterpret_cast<const char*>(packet.data()),
                        static_cast<std::streamsize>(packet.size()));
                }
            }
            if (bitstream_out && !bitstream_out) {
                throw std::runtime_error("Failed while writing bitstream output: " +
                                         options.bitstream_out_path);
            }
            total_packets += sample.returned_packets;
            total_bytes += sample.returned_bytes;

            if (options.pace && options.fps > 0) {
                const auto target_time = run_start + frame_period * static_cast<int64_t>(frame_index + 1);
                const auto now = std::chrono::steady_clock::now();
                if (now < target_time) {
                    const auto sleep_duration = target_time - now;
                    sample.sleep_ms = static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sleep_duration).count()) / 1000000.0;
                    std::this_thread::sleep_until(target_time);
                } else {
                    sample.late_ms = static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now - target_time).count()) / 1000000.0;
                }
            }

            sample.elapsed_ms = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - run_start).count()) / 1000000.0;
            samples.push_back(sample);
            if (csv) {
                write_csv_row(csv, sample);
                if ((frame_index % 60) == 0) {
                    csv.flush();
                }
            }
        }

        NvEncoderEncodeFrameTiming end_timing;
        uint64_t end_fetch_ns = 0;
        encoder.EndEncode(packets, nullptr, nullptr, &end_fetch_ns, &end_timing);
        total_packets += packets.size();
        for (const auto& packet : packets) {
            total_bytes += packet.size();
            if (bitstream_out && !packet.empty()) {
                bitstream_out.write(
                    reinterpret_cast<const char*>(packet.data()),
                    static_cast<std::streamsize>(packet.size()));
            }
        }
        if (bitstream_out) {
            bitstream_out.flush();
            if (!bitstream_out) {
                throw std::runtime_error("Failed while flushing bitstream output: " +
                                         options.bitstream_out_path);
            }
        }
        encoder.DestroyEncoder();
        check_cuda(cudaStreamDestroy(stream), "cudaStreamDestroy");

        const double total_elapsed_s = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - run_start).count()) / 1000000000.0;
        if (csv) {
            csv.flush();
        }
        print_summary(options, samples, total_packets, total_bytes, total_elapsed_s);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "nvenc_stress_load failed: " << ex.what() << std::endl;
        return 1;
    }
}
