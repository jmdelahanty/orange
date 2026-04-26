#include "NvEncoder/NvEncoderCuda.h"
#include "FFmpegWriter.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested{false};

struct Options {
    std::string socket_path;
    int gpu_id = 0;
    uint64_t max_frames = 0;
    std::string csv_path;
    bool encode = false;
    uint32_t encode_max_fps = 0;
    uint32_t encode_queue_depth = 8;
    uint32_t fps = 60;
    std::string codec = "hevc";
    std::string preset = "p1";
    std::string tuning = "ll";
    uint32_t gop = 25;
    uint32_t bitrate_bps = 150000000;
    uint32_t max_bitrate_bps = 150000000;
    uint32_t vbv_buffer_size = 150000000;
    uint32_t extra_output_delay = 3;
    bool monochrome = true;
    std::string bitstream_out_path;
    std::string mp4_out_path;
    std::string mp4_keyframe_path;
    std::string encode_csv_path;
    std::string summary_json_path;
    std::string gop_routing_csv_path;
    std::string session_id;
    std::string stream_id;
    int shard_id = 0;
    std::vector<int> shard_gpu_ids;
    std::string routing_policy = "single_shard";
};

struct FrameDescriptor {
    std::string camera_serial;
    std::string session_id;
    std::string stream_id;
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    int width = 0;
    int height = 0;
    int pixel_format = 0;
    uint64_t bytes = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    std::string routing_policy = "single_shard";
    std::string handle_hex;
};

struct ImportedHandle {
    void* ptr = nullptr;
};

struct Sample {
    uint64_t frame_index = 0;
    std::string camera_serial;
    std::string session_id;
    std::string stream_id;
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    std::string routing_policy = "single_shard";
    uint64_t bytes = 0;
    double total_ms = 0.0;
    double open_handle_ms = 0.0;
    double copy_ms = 0.0;
    bool opened_handle = false;
    bool detach_copied = true;
    bool encode_enqueued = false;
    bool encode_skipped = false;
    bool encode_dropped = false;
    uint64_t encode_queue_depth = 0;
};

void signal_handler(int)
{
    g_stop_requested.store(true, std::memory_order_release);
}

[[noreturn]] void usage(const char* argv0, int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out
        << "Usage:\n"
        << "  " << argv0 << " --socket <path> [options]\n\n"
        << "Options:\n"
        << "  --socket <path>       Unix socket path to listen on.\n"
        << "  --gpu-id <int>        CUDA device for recorder-owned detach copy. Default 0.\n"
        << "  --max-frames <int>    Exit after this many frames. 0 means until signal. Default 0.\n"
        << "  --csv <path>          Optional timing CSV.\n"
        << "  --encode              Encode detached frames in a worker thread after ACK.\n"
        << "  --encode-max-fps <int> Encode at most this FPS. 0 means every frame. Default 0.\n"
        << "  --encode-queue-depth <int> Recorder-owned detach slots. Default 8.\n"
        << "  --fps <int>           Encoder nominal FPS. Default 60.\n"
        << "  --codec <hevc|h264>   Default hevc.\n"
        << "  --preset <p1|p3|p5|p7> Default p1.\n"
        << "  --tuning <ll|ull|hq|lossless> Default ll.\n"
        << "  --gop <int>           GOP length. Default 25.\n"
        << "  --bitrate-bps <int>   Average bitrate. Default 150000000.\n"
        << "  --max-bitrate-bps <int> Max bitrate. Default 150000000.\n"
        << "  --vbv-buffer-size <int> VBV buffer size. Default 150000000.\n"
        << "  --extra-output-delay <int> NvEncoder extra output delay. Default 3.\n"
        << "  --bitstream-out <path> Optional raw elementary stream output.\n"
        << "  --mp4-out <path>       Optional MP4 output. Implies --encode.\n"
        << "  --mp4-keyframe <path>  Optional keyframe sidecar path for --mp4-out.\n"
        << "  --encode-csv <path>   Optional external encode timing CSV.\n"
        << "  --summary-json <path>  Optional run summary JSON.\n"
        << "  --gop-routing-csv <path> Optional per-frame route/shard CSV.\n"
        << "  --session-id <id>     Session id for artifacts. Defaults to first descriptor.\n"
        << "  --stream-id <id>      Stream id for artifacts. Defaults to camera serial.\n"
        << "  --shard-id <int>      Recorder shard id for this process/lane. Default 0.\n"
        << "  --shard-gpu-ids <csv> Diagnostic multi-shard GPU ids, e.g. 5,6.\n"
        << "  --routing-policy <name> Routing policy label. Default single_shard.\n"
        << "  --monochrome          Enable NVENC monochrome encoding. Default.\n"
        << "  --no-monochrome       Disable monochrome encoding.\n"
        << "  --help\n";
    std::exit(exit_code);
}

uint64_t parse_u64(const std::string& value, const char* name)
{
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    }
    return static_cast<uint64_t>(parsed);
}

int parse_i32(const std::string& value, const char* name)
{
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

std::vector<int> parse_i32_list(const std::string& value, const char* name)
{
    std::vector<int> out;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item.erase(
            item.begin(),
            std::find_if(item.begin(), item.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
        item.erase(
            std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(),
            item.end());
        if (item.empty()) {
            throw std::runtime_error(std::string("Invalid empty item in ") + name);
        }
        out.push_back(parse_i32(item, name));
    }
    if (out.empty()) {
        throw std::runtime_error(std::string("Invalid empty ") + name);
    }
    return out;
}

uint32_t parse_u32(const std::string& value, const char* name)
{
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    }
    return static_cast<uint32_t>(parsed);
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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
        } else if (arg == "--socket") {
            options.socket_path = consume(arg.c_str());
        } else if (arg == "--gpu-id" || arg == "--gpu") {
            options.gpu_id = parse_i32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--max-frames") {
            options.max_frames = parse_u64(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--csv") {
            options.csv_path = consume(arg.c_str());
        } else if (arg == "--encode") {
            options.encode = true;
        } else if (arg == "--encode-max-fps") {
            options.encode_max_fps = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--encode-queue-depth") {
            options.encode_queue_depth = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--fps") {
            options.fps = parse_u32(consume(arg.c_str()), arg.c_str());
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
        } else if (arg == "--bitstream-out") {
            options.bitstream_out_path = consume(arg.c_str());
            options.encode = true;
        } else if (arg == "--mp4-out") {
            options.mp4_out_path = consume(arg.c_str());
            options.encode = true;
        } else if (arg == "--mp4-keyframe" || arg == "--mp4-keyframe-path") {
            options.mp4_keyframe_path = consume(arg.c_str());
        } else if (arg == "--encode-csv") {
            options.encode_csv_path = consume(arg.c_str());
        } else if (arg == "--summary-json") {
            options.summary_json_path = consume(arg.c_str());
        } else if (arg == "--gop-routing-csv") {
            options.gop_routing_csv_path = consume(arg.c_str());
        } else if (arg == "--session-id") {
            options.session_id = consume(arg.c_str());
        } else if (arg == "--stream-id") {
            options.stream_id = consume(arg.c_str());
        } else if (arg == "--shard-id") {
            options.shard_id = parse_i32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--shard-gpu-ids") {
            options.shard_gpu_ids = parse_i32_list(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--routing-policy") {
            options.routing_policy = lower_ascii(consume(arg.c_str()));
        } else if (arg == "--monochrome") {
            options.monochrome = true;
        } else if (arg == "--no-monochrome") {
            options.monochrome = false;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.socket_path.empty()) {
        throw std::runtime_error("--socket is required");
    }
    if (options.socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        throw std::runtime_error("--socket path is too long");
    }
    if (options.encode_queue_depth == 0) {
        throw std::runtime_error("--encode-queue-depth must be positive");
    }
    if (options.codec != "hevc" && options.codec != "h264") {
        throw std::runtime_error("--codec must be hevc or h264");
    }
    if (options.routing_policy.empty()) {
        throw std::runtime_error("--routing-policy must not be empty");
    }
    if (!options.shard_gpu_ids.empty() && options.shard_gpu_ids.size() < 2) {
        throw std::runtime_error("--shard-gpu-ids requires at least two GPU ids");
    }
    if (!options.shard_gpu_ids.empty() && options.routing_policy == "single_shard") {
        options.routing_policy = "gop_modulo";
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
        const char* text = nullptr;
        cuGetErrorName(status, &name);
        cuGetErrorString(status, &text);
        throw std::runtime_error(
            std::string(call) + " failed: " + (name ? name : "CUDA_ERROR") +
            " " + (text ? text : ""));
    }
}

double elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count()) / 1000000.0;
}

bool hex_to_ipc_handle(const std::string& hex, cudaIpcMemHandle_t* handle)
{
    if (!handle || hex.size() != sizeof(cudaIpcMemHandle_t) * 2) {
        return false;
    }
    auto* bytes = reinterpret_cast<unsigned char*>(handle);
    for (size_t i = 0; i < sizeof(cudaIpcMemHandle_t); ++i) {
        const std::string byte_hex = hex.substr(i * 2, 2);
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(byte_hex.c_str(), &end, 16);
        if (end == byte_hex.c_str() || *end != '\0' || parsed > 0xffUL) {
            return false;
        }
        bytes[i] = static_cast<unsigned char>(parsed);
    }
    return true;
}

double ns_to_ms(uint64_t ns)
{
    return static_cast<double>(ns) / 1000000.0;
}

void ensure_parent_directory(const std::string& path)
{
    if (path.empty()) {
        return;
    }
    const std::filesystem::path fs_path(path);
    const std::filesystem::path parent = fs_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

std::string derive_keyframe_path(const std::string& mp4_path)
{
    std::filesystem::path path(mp4_path);
    const std::string stem = path.stem().string();
    path.replace_filename(stem + "_keyframes.csv");
    return path.string();
}

std::string add_suffix_to_path_stem(const std::string& path, const std::string& suffix)
{
    if (path.empty()) {
        return {};
    }
    std::filesystem::path fs_path(path);
    const std::string stem = fs_path.stem().string();
    fs_path.replace_filename(stem + suffix + fs_path.extension().string());
    return fs_path.string();
}

std::string shard_suffix(size_t shard_index, int gpu_id)
{
    return "_shard" + std::to_string(shard_index) + "_gpu" + std::to_string(gpu_id);
}

std::string json_escape(const std::string& value)
{
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (c < 0x20) {
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(c)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(c);
            }
            break;
        }
    }
    return out.str();
}

uintmax_t file_size_or_zero(const std::string& path)
{
    if (path.empty()) {
        return 0;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

double percentile_ms(std::vector<double> values, double percentile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped = std::max(0.0, std::min(100.0, percentile));
    const size_t index = static_cast<size_t>(
        (clamped / 100.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

uint64_t elapsed_ns(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
}

uint64_t steady_clock_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

GUID resolve_codec_guid(const std::string& codec)
{
    return codec == "h264" ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
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
                              uint32_t width,
                              uint32_t height,
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

    initialize_params->encodeWidth = width;
    initialize_params->encodeHeight = height;
    initialize_params->darWidth = width;
    initialize_params->darHeight = height;
    initialize_params->frameRateNum = std::max<uint32_t>(1, options.fps);
    initialize_params->frameRateDen = 1;
    initialize_params->enablePTD = 1;

    encode_config->gopLength = std::max<uint32_t>(1, options.gop);
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
        encode_config->rcParams.maxBitRate =
            std::max(options.max_bitrate_bps, options.bitrate_bps);
        encode_config->rcParams.vbvBufferSize =
            options.vbv_buffer_size > 0
                ? options.vbv_buffer_size
                : encode_config->rcParams.maxBitRate;
        encode_config->rcParams.enableAQ = 0;
        encode_config->rcParams.enableTemporalAQ = 0;
        encode_config->rcParams.enableLookahead = 0;
        encode_config->rcParams.lowDelayKeyFrameScale = low_latency ? 1 : 0;
    }
    encode_config->rcParams.enableMinQP = 0;
    encode_config->rcParams.enableMaxQP = 0;
    encode_config->rcParams.strictGOPTarget = 0;
    encode_config->rcParams.enableNonRefP = 0;
    initialize_params->enableWeightedPrediction = 0;

    if (options.codec == "hevc") {
        auto& hevc = encode_config->encodeCodecConfig.hevcConfig;
        hevc.pixelBitDepthMinus8 = 0;
        hevc.idrPeriod = encode_config->gopLength;
        hevc.sliceMode = 0;
        hevc.sliceModeData = 0;
        hevc.maxNumRefFramesInDPB = 1;
        hevc.repeatSPSPPS = 1;
        hevc.outputBufferingPeriodSEI = 0;
        hevc.outputPictureTimingSEI = 0;
        hevc.outputAUD = 0;
        hevc.enableLTR = 0;
    } else {
        auto& h264 = encode_config->encodeCodecConfig.h264Config;
        h264.idrPeriod = encode_config->gopLength;
        h264.sliceMode = 0;
        h264.sliceModeData = 0;
        h264.repeatSPSPPS = 1;
        h264.maxNumRefFrames = 1;
        h264.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
        h264.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
        h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    }
    if (options.monochrome) {
        encode_config->monoChromeEncoding = 1;
    }
}

int create_listen_socket(const std::string& socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    unlink(socket_path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string message = "bind(" + socket_path + ") failed: " +
            std::string(std::strerror(errno));
        close(fd);
        throw std::runtime_error(message);
    }
    chmod(socket_path.c_str(), 0666);
    if (listen(fd, 1) != 0) {
        const std::string message = "listen() failed: " + std::string(std::strerror(errno));
        close(fd);
        throw std::runtime_error(message);
    }
    return fd;
}

bool read_line(int fd, std::string* line)
{
    if (line) {
        line->clear();
    }
    char ch = '\0';
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        const ssize_t n = recv(fd, &ch, 1, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        if (line) {
            line->push_back(ch);
        }
    }
    return false;
}

bool write_all(int fd, const std::string& data)
{
    const char* cursor = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

bool parse_frame_descriptor(const std::string& line, FrameDescriptor* desc)
{
    if (!desc) {
        return false;
    }
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind != "FRAME") {
        return false;
    }
    in >> desc->camera_serial
       >> desc->recording_frame_id
       >> desc->local_frame_id
       >> desc->source_gpu_id
       >> desc->width
       >> desc->height
       >> desc->pixel_format
       >> desc->bytes
       >> desc->timestamp
       >> desc->timestamp_sys
       >> desc->handle_hex;
    if (!in || desc->handle_hex.empty() || desc->bytes == 0) {
        return false;
    }

    std::string session_id;
    std::string stream_id;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    std::string routing_policy;
    if (in >> session_id >> stream_id >> gop_index >> frame_index_within_gop >>
        assigned_gpu_id >> assigned_shard_id >> routing_policy) {
        desc->session_id = std::move(session_id);
        desc->stream_id = std::move(stream_id);
        desc->gop_index = gop_index;
        desc->frame_index_within_gop = frame_index_within_gop;
        desc->assigned_gpu_id = assigned_gpu_id;
        desc->assigned_shard_id = assigned_shard_id;
        desc->routing_policy = std::move(routing_policy);
    }
    if (desc->stream_id.empty()) {
        desc->stream_id = desc->camera_serial;
    }
    const uint64_t zero_based_frame =
        desc->recording_frame_id > 0 ? desc->recording_frame_id - 1 : 0;
    if (desc->gop_index == 0 && desc->frame_index_within_gop == 0 &&
        desc->recording_frame_id > 1) {
        // Older senders did not provide GOP metadata; preserve a usable best
        // effort frame index so route artifacts remain inspectable.
        desc->frame_index_within_gop = static_cast<uint32_t>(zero_based_frame);
    }
    return true;
}

void apply_descriptor_defaults(const Options& options, FrameDescriptor* desc)
{
    if (!desc) {
        return;
    }
    if (!options.session_id.empty()) {
        desc->session_id = options.session_id;
    } else if (desc->session_id.empty()) {
        desc->session_id = "external_recorder_session";
    }
    if (!options.stream_id.empty()) {
        desc->stream_id = options.stream_id;
    } else if (desc->stream_id.empty()) {
        desc->stream_id = desc->camera_serial;
    }
    desc->routing_policy = options.routing_policy;
}

void write_csv_header(std::ofstream& csv)
{
    csv << "frame_index,camera_serial,session_id,stream_id,recording_frame_id,"
           "local_frame_id,gop_index,frame_index_within_gop,source_gpu_id,"
           "assigned_gpu_id,assigned_shard_id,routing_policy,bytes,"
           "total_ms,open_handle_ms,copy_ms,opened_handle,detach_copied,"
           "encode_enqueued,encode_skipped,encode_dropped,encode_queue_depth\n";
}

void write_csv_row(std::ofstream& csv, const Sample& sample)
{
    csv << sample.frame_index << ","
        << sample.camera_serial << ","
        << sample.session_id << ","
        << sample.stream_id << ","
        << sample.recording_frame_id << ","
        << sample.local_frame_id << ","
        << sample.gop_index << ","
        << sample.frame_index_within_gop << ","
        << sample.source_gpu_id << ","
        << sample.assigned_gpu_id << ","
        << sample.assigned_shard_id << ","
        << sample.routing_policy << ","
        << sample.bytes << ","
        << std::fixed << std::setprecision(6)
        << sample.total_ms << ","
        << sample.open_handle_ms << ","
        << sample.copy_ms << ","
        << (sample.opened_handle ? "true" : "false") << ","
        << (sample.detach_copied ? "true" : "false") << ","
        << (sample.encode_enqueued ? "true" : "false") << ","
        << (sample.encode_skipped ? "true" : "false") << ","
        << (sample.encode_dropped ? "true" : "false") << ","
        << sample.encode_queue_depth << "\n";
}

void write_gop_routing_csv_header(std::ofstream& csv)
{
    csv << "frame_index,camera_serial,session_id,stream_id,recording_frame_id,"
           "local_frame_id,gop_index,frame_index_within_gop,source_gpu_id,"
           "assigned_gpu_id,assigned_shard_id,routing_policy,detach_copied,"
           "encode_enqueued,encode_skipped,encode_dropped,encode_queue_depth\n";
}

void write_gop_routing_csv_row(std::ofstream& csv, const Sample& sample)
{
    csv << sample.frame_index << ","
        << sample.camera_serial << ","
        << sample.session_id << ","
        << sample.stream_id << ","
        << sample.recording_frame_id << ","
        << sample.local_frame_id << ","
        << sample.gop_index << ","
        << sample.frame_index_within_gop << ","
        << sample.source_gpu_id << ","
        << sample.assigned_gpu_id << ","
        << sample.assigned_shard_id << ","
        << sample.routing_policy << ","
        << (sample.detach_copied ? "true" : "false") << ","
        << (sample.encode_enqueued ? "true" : "false") << ","
        << (sample.encode_skipped ? "true" : "false") << ","
        << (sample.encode_dropped ? "true" : "false") << ","
        << sample.encode_queue_depth << "\n";
}

struct EncodeSample {
    uint64_t encode_index = 0;
    uint64_t source_frame_index = 0;
    std::string camera_serial;
    std::string session_id;
    std::string stream_id;
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    std::string routing_policy = "single_shard";
    uint64_t bytes = 0;
    double enqueue_age_ms = 0.0;
    double prepare_ms = 0.0;
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
};

struct EncodeSummary {
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    std::string routing_policy = "single_shard";
    uint64_t frames_encoded = 0;
    uint64_t frames_dropped = 0;
    uint64_t returned_packets = 0;
    uint64_t returned_bytes = 0;
    uint64_t raw_packets = 0;
    uint64_t raw_bytes = 0;
    uint64_t mp4_packets = 0;
    uint64_t mp4_bytes = 0;
    uint64_t flush_packets = 0;
    uint64_t flush_bytes = 0;
    bool failed = false;
    bool mp4_queue_overflowed = false;
    uint64_t mp4_queue_overflow_events = 0;
    size_t mp4_peak_queued_packets = 0;
    size_t mp4_peak_queued_bytes = 0;
    double enqueue_age_p95_ms = 0.0;
    double prepare_p95_ms = 0.0;
    double encode_total_p95_ms = 0.0;
    double encode_picture_p95_ms = 0.0;
    double lock_bitstream_p95_ms = 0.0;
    double bitstream_fetch_p95_ms = 0.0;
    double mp4_push_mean_ms = 0.0;
    double mp4_push_max_ms = 0.0;
    double mp4_write_mean_ms = 0.0;
    double mp4_write_max_ms = 0.0;
    std::string encode_csv_path;
    std::string bitstream_out_path;
    std::string mp4_path;
    std::string mp4_keyframe_path;
};

struct MergedOutputSummary {
    bool enabled = false;
    bool failed = false;
    uint64_t packets_written = 0;
    uint64_t bytes_written = 0;
    uint64_t gops_released = 0;
    uint64_t pending_gops = 0;
    uint64_t pending_bytes = 0;
    bool mp4_queue_overflowed = false;
    uint64_t mp4_queue_overflow_events = 0;
    size_t mp4_peak_queued_packets = 0;
    size_t mp4_peak_queued_bytes = 0;
    double mp4_push_mean_ms = 0.0;
    double mp4_push_max_ms = 0.0;
    double mp4_write_mean_ms = 0.0;
    double mp4_write_max_ms = 0.0;
    std::string mp4_path;
    std::string mp4_keyframe_path;
    std::string error_message;
};

EncodeSummary aggregate_encode_summaries(const std::vector<EncodeSummary>& summaries)
{
    EncodeSummary out;
    if (summaries.empty()) {
        return out;
    }
    out.assigned_gpu_id = summaries.size() == 1 ? summaries.front().assigned_gpu_id : -1;
    out.assigned_shard_id = summaries.size() == 1 ? summaries.front().assigned_shard_id : -1;
    out.routing_policy = summaries.front().routing_policy;
    for (const EncodeSummary& summary : summaries) {
        out.frames_encoded += summary.frames_encoded;
        out.frames_dropped += summary.frames_dropped;
        out.returned_packets += summary.returned_packets;
        out.returned_bytes += summary.returned_bytes;
        out.raw_packets += summary.raw_packets;
        out.raw_bytes += summary.raw_bytes;
        out.mp4_packets += summary.mp4_packets;
        out.mp4_bytes += summary.mp4_bytes;
        out.flush_packets += summary.flush_packets;
        out.flush_bytes += summary.flush_bytes;
        out.failed = out.failed || summary.failed;
        out.mp4_queue_overflowed = out.mp4_queue_overflowed || summary.mp4_queue_overflowed;
        out.mp4_queue_overflow_events += summary.mp4_queue_overflow_events;
        out.mp4_peak_queued_packets =
            std::max(out.mp4_peak_queued_packets, summary.mp4_peak_queued_packets);
        out.mp4_peak_queued_bytes =
            std::max(out.mp4_peak_queued_bytes, summary.mp4_peak_queued_bytes);
        out.enqueue_age_p95_ms =
            std::max(out.enqueue_age_p95_ms, summary.enqueue_age_p95_ms);
        out.prepare_p95_ms =
            std::max(out.prepare_p95_ms, summary.prepare_p95_ms);
        out.encode_total_p95_ms =
            std::max(out.encode_total_p95_ms, summary.encode_total_p95_ms);
        out.encode_picture_p95_ms =
            std::max(out.encode_picture_p95_ms, summary.encode_picture_p95_ms);
        out.lock_bitstream_p95_ms =
            std::max(out.lock_bitstream_p95_ms, summary.lock_bitstream_p95_ms);
        out.bitstream_fetch_p95_ms =
            std::max(out.bitstream_fetch_p95_ms, summary.bitstream_fetch_p95_ms);
        out.mp4_push_mean_ms =
            std::max(out.mp4_push_mean_ms, summary.mp4_push_mean_ms);
        out.mp4_push_max_ms =
            std::max(out.mp4_push_max_ms, summary.mp4_push_max_ms);
        out.mp4_write_mean_ms =
            std::max(out.mp4_write_mean_ms, summary.mp4_write_mean_ms);
        out.mp4_write_max_ms =
            std::max(out.mp4_write_max_ms, summary.mp4_write_max_ms);
    }
    if (summaries.size() == 1) {
        out.encode_csv_path = summaries.front().encode_csv_path;
        out.bitstream_out_path = summaries.front().bitstream_out_path;
        out.mp4_path = summaries.front().mp4_path;
        out.mp4_keyframe_path = summaries.front().mp4_keyframe_path;
    }
    return out;
}

void write_encode_csv_header(std::ofstream& csv)
{
    csv << "encode_index,source_frame_index,camera_serial,session_id,stream_id,"
           "recording_frame_id,local_frame_id,gop_index,frame_index_within_gop,"
           "source_gpu_id,assigned_gpu_id,assigned_shard_id,routing_policy,bytes,"
           "enqueue_age_ms,prepare_ms,encode_total_ms,"
           "encode_picture_ms,completion_wait_ms,lock_bitstream_ms,"
           "bitstream_copy_ms,unlock_bitstream_ms,unmap_input_resource_ms,"
           "bitstream_fetch_ms,output_packets,output_bytes,returned_packets,"
           "returned_bytes\n";
}

void write_encode_csv_row(std::ofstream& csv, const EncodeSample& sample)
{
    csv << sample.encode_index << ","
        << sample.source_frame_index << ","
        << sample.camera_serial << ","
        << sample.session_id << ","
        << sample.stream_id << ","
        << sample.recording_frame_id << ","
        << sample.local_frame_id << ","
        << sample.gop_index << ","
        << sample.frame_index_within_gop << ","
        << sample.source_gpu_id << ","
        << sample.assigned_gpu_id << ","
        << sample.assigned_shard_id << ","
        << sample.routing_policy << ","
        << sample.bytes << ","
        << std::fixed << std::setprecision(6)
        << sample.enqueue_age_ms << ","
        << sample.prepare_ms << ","
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
        << sample.returned_bytes << "\n";
}

struct DeviceSlot {
    void* ptr = nullptr;
    uint64_t bytes = 0;
    bool allocated = false;
};

class MergedGopOutput {
public:
    explicit MergedGopOutput(Options options)
        : options_(std::move(options)),
          gop_length_(std::max<uint32_t>(1, options_.gop)),
          mp4_path_(options_.mp4_out_path),
          mp4_keyframe_path_(
              options_.mp4_keyframe_path.empty()
                  ? derive_keyframe_path(options_.mp4_out_path)
                  : options_.mp4_keyframe_path) {}

    void note_submitted(const FrameDescriptor& desc)
    {
        if (mp4_path_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_writer_locked(desc);
        mark_older_gops_complete_locked(desc.gop_index);
        PendingGop& gop = pending_gops_[desc.gop_index];
        gop.gop_index = desc.gop_index;
        gop.submitted_count++;
        if (desc.frame_index_within_gop + 1 >= gop_length_) {
            gop.submitted_complete = true;
        }
        refresh_complete_locked(gop);
        flush_ready_locked(false);
    }

    void submit_packets(const std::vector<std::vector<uint8_t>>& packets,
                        const std::vector<uint64_t>& output_timestamps,
                        const FrameDescriptor& fallback_desc)
    {
        if (mp4_path_.empty() || packets.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_writer_locked(fallback_desc);
        for (size_t i = 0; i < packets.size(); ++i) {
            const uint64_t output_timestamp =
                i < output_timestamps.size() ? output_timestamps[i] : fallback_desc.recording_frame_id;
            const uint64_t zero_based_frame =
                output_timestamp > 0 ? output_timestamp - 1 :
                (fallback_desc.recording_frame_id > 0 ? fallback_desc.recording_frame_id - 1 : 0);
            const uint64_t gop_index = zero_based_frame / static_cast<uint64_t>(gop_length_);
            PendingGop& gop = pending_gops_[gop_index];
            gop.gop_index = gop_index;
            BufferedPacket packet;
            packet.bytes = packets[i];
            packet.zero_based_frame = zero_based_frame;
            gop.total_bytes += packet.bytes.size();
            pending_bytes_ += packet.bytes.size();
            gop.packets.push_back(std::move(packet));
            gop.emitted_count++;
            refresh_complete_locked(gop);
        }
        flush_ready_locked(false);
    }

    void finish()
    {
        if (mp4_path_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        flush_ready_locked(true);
        finish_writer_locked();
    }

    MergedOutputSummary summary() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        MergedOutputSummary out;
        out.enabled = !mp4_path_.empty();
        out.failed = failed_;
        out.packets_written = packets_written_;
        out.bytes_written = bytes_written_;
        out.gops_released = gops_released_;
        out.pending_gops = pending_gops_.size();
        out.pending_bytes = pending_bytes_;
        out.mp4_queue_overflowed = mp4_queue_overflowed_;
        out.mp4_queue_overflow_events = mp4_queue_overflow_events_;
        out.mp4_peak_queued_packets = mp4_peak_queued_packets_;
        out.mp4_peak_queued_bytes = mp4_peak_queued_bytes_;
        out.mp4_push_mean_ms = writer_latency_.push_packet_total.mean_ms();
        out.mp4_push_max_ms = writer_latency_.push_packet_total.max_ms();
        out.mp4_write_mean_ms = writer_latency_.packet_write.mean_ms();
        out.mp4_write_max_ms = writer_latency_.packet_write.max_ms();
        out.mp4_path = mp4_path_;
        out.mp4_keyframe_path = mp4_keyframe_path_;
        out.error_message = error_message_;
        return out;
    }

private:
    struct BufferedPacket {
        std::vector<uint8_t> bytes;
        uint64_t zero_based_frame = 0;
    };

    struct PendingGop {
        uint64_t gop_index = 0;
        uint64_t submitted_count = 0;
        uint64_t emitted_count = 0;
        uint64_t total_bytes = 0;
        bool submitted_complete = false;
        bool complete = false;
        std::vector<BufferedPacket> packets;
    };

    void ensure_writer_locked(const FrameDescriptor& desc)
    {
        if (writer_ || mp4_path_.empty()) {
            return;
        }
        ensure_parent_directory(mp4_path_);
        ensure_parent_directory(mp4_keyframe_path_);
        const AVCodecID codec_id =
            options_.codec == "h264" ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
        const std::vector<std::pair<std::string, std::string>> metadata_tags = {
            {"producer", "external_recorder_ipc_probe"},
            {"camera_serial", desc.camera_serial},
            {"codec", options_.codec},
            {"preset", options_.preset},
            {"tuning", options_.tuning},
            {"external_recorder", "true"},
            {"merged_gop_output", "true"},
            {"routing_policy", options_.routing_policy}
        };
        writer_ = std::make_unique<FFmpegWriter>(
            codec_id,
            desc.width,
            desc.height,
            static_cast<int>(std::max<uint32_t>(1, options_.fps)),
            mp4_path_.c_str(),
            mp4_keyframe_path_.c_str(),
            metadata_tags);
        writer_->create_thread();
    }

    void refresh_complete_locked(PendingGop& gop)
    {
        if (gop.submitted_complete &&
            gop.submitted_count > 0 &&
            gop.emitted_count >= gop.submitted_count) {
            gop.complete = true;
        }
    }

    void mark_older_gops_complete_locked(uint64_t current_gop_index)
    {
        for (auto& [gop_index, gop] : pending_gops_) {
            if (gop_index >= current_gop_index) {
                break;
            }
            gop.submitted_complete = true;
            refresh_complete_locked(gop);
        }
    }

    void flush_ready_locked(bool flush_all)
    {
        while (true) {
            auto it = pending_gops_.find(next_gop_to_release_);
            if (it == pending_gops_.end()) {
                if (flush_all && !pending_gops_.empty()) {
                    it = pending_gops_.begin();
                    next_gop_to_release_ = it->first;
                } else {
                    break;
                }
            }
            if (!flush_all && !it->second.complete) {
                break;
            }
            PendingGop gop = std::move(it->second);
            pending_gops_.erase(it);
            pending_bytes_ -= gop.total_bytes;
            release_gop_locked(gop);
            next_gop_to_release_++;
        }
    }

    void release_gop_locked(PendingGop& gop)
    {
        if (!writer_) {
            return;
        }
        std::stable_sort(
            gop.packets.begin(),
            gop.packets.end(),
            [](const BufferedPacket& lhs, const BufferedPacket& rhs) {
                return lhs.zero_based_frame < rhs.zero_based_frame;
            });
        const uint64_t release_started_ns = steady_clock_now_ns();
        for (size_t i = 0; i < gop.packets.size(); ++i) {
            const BufferedPacket& packet = gop.packets[i];
            writer_->push_packet(
                const_cast<uint8_t*>(packet.bytes.data()),
                static_cast<int>(packet.bytes.size()),
                static_cast<int64_t>(merged_pts_counter_++),
                gop.gop_index,
                i + 1 == gop.packets.size(),
                release_started_ns);
            packets_written_++;
            bytes_written_ += packet.bytes.size();
        }
        gops_released_++;
    }

    void finish_writer_locked()
    {
        if (!writer_) {
            return;
        }
        writer_->quit_thread();
        writer_->join_thread();
        writer_latency_ = writer_->latency_stats();
        mp4_queue_overflowed_ = writer_->has_queue_overflowed();
        mp4_queue_overflow_events_ = writer_->queue_overflow_events();
        mp4_peak_queued_packets_ = writer_->peak_queued_packets();
        mp4_peak_queued_bytes_ = writer_->peak_queued_bytes();
        failed_ = failed_ || mp4_queue_overflowed_;
        if (mp4_queue_overflowed_ && error_message_.empty()) {
            error_message_ = "merged MP4 writer queue overflowed";
        }
        writer_.reset();
    }

    Options options_;
    uint32_t gop_length_ = 1;
    std::string mp4_path_;
    std::string mp4_keyframe_path_;
    mutable std::mutex mutex_;
    std::map<uint64_t, PendingGop> pending_gops_;
    std::unique_ptr<FFmpegWriter> writer_;
    uint64_t next_gop_to_release_ = 0;
    uint64_t merged_pts_counter_ = 0;
    uint64_t packets_written_ = 0;
    uint64_t bytes_written_ = 0;
    uint64_t gops_released_ = 0;
    uint64_t pending_bytes_ = 0;
    bool failed_ = false;
    bool mp4_queue_overflowed_ = false;
    uint64_t mp4_queue_overflow_events_ = 0;
    size_t mp4_peak_queued_packets_ = 0;
    size_t mp4_peak_queued_bytes_ = 0;
    FFmpegWriterLatencyStats writer_latency_;
    std::string error_message_;
};

struct EncodeWorkItem {
    uint64_t source_frame_index = 0;
    FrameDescriptor desc;
    size_t slot_index = 0;
    std::chrono::steady_clock::time_point enqueued_at;
};

class ExternalEncodeWorker {
public:
    explicit ExternalEncodeWorker(Options options, MergedGopOutput* merged_output = nullptr)
        : options_(std::move(options)),
          merged_output_(merged_output) {}

    ~ExternalEncodeWorker()
    {
        stop();
        release_slots();
    }

    void start()
    {
        if (running_) {
            return;
        }
        running_ = true;
        worker_ = std::thread(&ExternalEncodeWorker::run, this);
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        running_ = false;
    }

    bool should_encode(std::chrono::steady_clock::time_point now)
    {
        if (!options_.encode) {
            return false;
        }
        if (options_.encode_max_fps == 0) {
            return true;
        }
        const auto period =
            std::chrono::nanoseconds(1000000000LL / static_cast<int64_t>(options_.encode_max_fps));
        if (!have_next_encode_time_) {
            next_encode_time_ = now;
            have_next_encode_time_ = true;
        }
        if (now < next_encode_time_) {
            return false;
        }
        do {
            next_encode_time_ += period;
        } while (next_encode_time_ < now - period);
        return true;
    }

    bool detach_and_enqueue(const FrameDescriptor& desc,
                            void* imported_ptr,
                            uint64_t source_frame_index,
                            Sample* sample)
    {
        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(external detach shard)");
        const size_t slot_index = acquire_free_slot(desc.bytes);
        if (slot_index == kInvalidSlot) {
            if (sample) {
                sample->encode_dropped = true;
                sample->detach_copied = false;
                sample->encode_queue_depth = queue_depth();
            }
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        check_cuda(
            cudaMemcpy(
                slots_[slot_index].ptr,
                imported_ptr,
                desc.bytes,
                cudaMemcpyDeviceToDevice),
            "cudaMemcpy(external encode detach slot)");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(
                EncodeWorkItem{
                    source_frame_index,
                    desc,
                    slot_index,
                    std::chrono::steady_clock::now()});
            if (sample) {
                sample->encode_enqueued = true;
                sample->encode_queue_depth = queue_.size();
            }
            if (merged_output_) {
                merged_output_->note_submitted(desc);
            }
        }
        cv_.notify_one();
        return true;
    }

    uint64_t frames_encoded() const { return frames_encoded_.load(std::memory_order_relaxed); }
    uint64_t frames_dropped() const { return frames_dropped_.load(std::memory_order_relaxed); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    int gpu_id() const { return options_.gpu_id; }
    int shard_id() const { return options_.shard_id; }

    EncodeSummary summary() const
    {
        EncodeSummary out;
        out.assigned_gpu_id = options_.gpu_id;
        out.assigned_shard_id = options_.shard_id;
        out.routing_policy = options_.routing_policy;
        out.frames_encoded = frames_encoded();
        out.frames_dropped = frames_dropped();
        out.returned_packets = returned_packets_;
        out.returned_bytes = returned_bytes_;
        out.raw_packets = raw_packets_;
        out.raw_bytes = raw_bytes_;
        out.mp4_packets = mp4_packets_;
        out.mp4_bytes = mp4_bytes_;
        out.flush_packets = flush_packets_;
        out.flush_bytes = flush_bytes_;
        out.failed = failed();
        out.mp4_queue_overflowed = mp4_queue_overflowed_;
        out.mp4_queue_overflow_events = mp4_queue_overflow_events_;
        out.mp4_peak_queued_packets = mp4_peak_queued_packets_;
        out.mp4_peak_queued_bytes = mp4_peak_queued_bytes_;
        out.enqueue_age_p95_ms = percentile_ms(enqueue_age_samples_, 95.0);
        out.prepare_p95_ms = percentile_ms(prepare_samples_, 95.0);
        out.encode_total_p95_ms = percentile_ms(encode_total_samples_, 95.0);
        out.encode_picture_p95_ms = percentile_ms(encode_picture_samples_, 95.0);
        out.lock_bitstream_p95_ms = percentile_ms(lock_bitstream_samples_, 95.0);
        out.bitstream_fetch_p95_ms = percentile_ms(bitstream_fetch_samples_, 95.0);
        out.mp4_push_mean_ms = writer_latency_.push_packet_total.mean_ms();
        out.mp4_push_max_ms = writer_latency_.push_packet_total.max_ms();
        out.mp4_write_mean_ms = writer_latency_.packet_write.mean_ms();
        out.mp4_write_max_ms = writer_latency_.packet_write.max_ms();
        out.encode_csv_path = options_.encode_csv_path;
        out.bitstream_out_path = options_.bitstream_out_path;
        out.mp4_path = options_.mp4_out_path;
        out.mp4_keyframe_path = resolved_mp4_keyframe_path_;
        return out;
    }

private:
    static constexpr size_t kInvalidSlot = std::numeric_limits<size_t>::max();

    size_t acquire_free_slot(uint64_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slots_.empty()) {
            slots_.resize(options_.encode_queue_depth);
            free_slots_.clear();
            free_slots_.reserve(slots_.size());
            for (size_t i = 0; i < slots_.size(); ++i) {
                free_slots_.push_back(i);
            }
        }
        if (free_slots_.empty()) {
            return kInvalidSlot;
        }
        const size_t slot_index = free_slots_.back();
        free_slots_.pop_back();
        DeviceSlot& slot = slots_[slot_index];
        if (!slot.allocated || slot.bytes < bytes) {
            if (slot.ptr) {
                cudaFree(slot.ptr);
                slot.ptr = nullptr;
            }
            check_cuda(cudaMalloc(&slot.ptr, bytes), "cudaMalloc(external encode slot)");
            slot.bytes = bytes;
            slot.allocated = true;
        }
        return slot_index;
    }

    void release_slot(size_t slot_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_slots_.push_back(slot_index);
    }

    uint64_t queue_depth() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void release_slots()
    {
        if (!slots_.empty()) {
            cudaSetDevice(options_.gpu_id);
        }
        for (auto& slot : slots_) {
            if (slot.ptr) {
                cudaFree(slot.ptr);
                slot.ptr = nullptr;
            }
            slot.bytes = 0;
            slot.allocated = false;
        }
    }

    void initialize_encoder(const FrameDescriptor& desc)
    {
        if (encoder_) {
            return;
        }
        if (desc.width <= 0 || desc.height <= 0 ||
            (desc.width % 2) != 0 || (desc.height % 2) != 0) {
            throw std::runtime_error("External NVENC requires positive even frame dimensions");
        }

        check_cu(cuInit(0), "cuInit");
        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(encode worker)");
        check_cuda(cudaFree(nullptr), "cudaFree(0)");
        check_cu(cuCtxGetCurrent(&cu_context_), "cuCtxGetCurrent");
        if (!cu_context_) {
            throw std::runtime_error("No current CUDA context in external encode worker");
        }
        check_cuda(
            cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
            "cudaStreamCreateWithFlags(external encode)");

        encoder_ = std::make_unique<NvEncoderCuda>(
            cu_context_,
            static_cast<uint32_t>(desc.width),
            static_cast<uint32_t>(desc.height),
            NV_ENC_BUFFER_FORMAT_NV12,
            options_.extra_output_delay);

        NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        configure_encoder_params(
            options_,
            static_cast<uint32_t>(desc.width),
            static_cast<uint32_t>(desc.height),
            &initialize_params,
            &encode_config,
            encoder_.get());
        encoder_->CreateEncoder(&initialize_params);
        encoder_->SetIOCudaStreams(
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream_),
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream_));

        if (!options_.bitstream_out_path.empty()) {
            ensure_parent_directory(options_.bitstream_out_path);
            bitstream_out_.open(options_.bitstream_out_path, std::ios::binary | std::ios::trunc);
            if (!bitstream_out_) {
                throw std::runtime_error("Failed to open bitstream output: " +
                                         options_.bitstream_out_path);
            }
        }
        if (!options_.mp4_out_path.empty()) {
            ensure_parent_directory(options_.mp4_out_path);
            resolved_mp4_keyframe_path_ = options_.mp4_keyframe_path.empty()
                ? derive_keyframe_path(options_.mp4_out_path)
                : options_.mp4_keyframe_path;
            ensure_parent_directory(resolved_mp4_keyframe_path_);
            const AVCodecID codec_id =
                options_.codec == "h264" ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
            const std::vector<std::pair<std::string, std::string>> metadata_tags = {
                {"producer", "external_recorder_ipc_probe"},
                {"camera_serial", desc.camera_serial},
                {"codec", options_.codec},
                {"preset", options_.preset},
                {"tuning", options_.tuning},
                {"external_recorder", "true"}
            };
            mp4_writer_ = std::make_unique<FFmpegWriter>(
                codec_id,
                desc.width,
                desc.height,
                static_cast<int>(std::max<uint32_t>(1, options_.fps)),
                options_.mp4_out_path.c_str(),
                resolved_mp4_keyframe_path_.c_str(),
                metadata_tags);
            mp4_writer_->create_thread();
        }
        if (!options_.encode_csv_path.empty()) {
            ensure_parent_directory(options_.encode_csv_path);
            encode_csv_.open(options_.encode_csv_path, std::ios::out | std::ios::trunc);
            if (!encode_csv_) {
                throw std::runtime_error("Failed to open encode CSV: " +
                                         options_.encode_csv_path);
            }
            write_encode_csv_header(encode_csv_);
        }

        std::cout << "external_recorder_ipc_probe encoder started"
                  << " gpu_id=" << options_.gpu_id
                  << " resolution=" << desc.width << "x" << desc.height
                  << " fps=" << options_.fps
                  << " encode_max_fps=" << options_.encode_max_fps
                  << " codec=" << options_.codec
                  << " preset=" << options_.preset
                  << " tuning=" << options_.tuning
                  << " queue_depth=" << options_.encode_queue_depth
                  << std::endl;
    }

    void prepare_input_frame(const EncodeWorkItem& item,
                             const NvEncInputFrame& frame)
    {
        auto* base = static_cast<uint8_t*>(frame.inputPtr);
        const auto* source = static_cast<const uint8_t*>(slots_[item.slot_index].ptr);
        const int width = item.desc.width;
        const int height = item.desc.height;

        check_cuda(
            cudaMemcpy2DAsync(
                base,
                frame.pitch,
                source,
                static_cast<size_t>(width),
                static_cast<size_t>(width),
                static_cast<size_t>(height),
                cudaMemcpyDeviceToDevice,
                stream_),
            "cudaMemcpy2DAsync(external Mono8 Y plane)");

        const uint32_t chroma_width =
            NvEncoder::GetChromaWidthInBytes(frame.bufferFormat, static_cast<uint32_t>(width));
        const uint32_t chroma_height =
            NvEncoder::GetChromaHeight(frame.bufferFormat, static_cast<uint32_t>(height));
        for (uint32_t plane = 0; plane < frame.numChromaPlanes; ++plane) {
            check_cuda(
                cudaMemset2DAsync(
                    base + frame.chromaOffsets[plane],
                    frame.chromaPitch,
                    128,
                    chroma_width,
                    chroma_height,
                    stream_),
                "cudaMemset2DAsync(external NV12 chroma)");
        }
    }

    void push_packet_to_outputs(const std::vector<uint8_t>& packet, bool flush_packet)
    {
        if (packet.empty()) {
            return;
        }
        if (bitstream_out_) {
            bitstream_out_.write(
                reinterpret_cast<const char*>(packet.data()),
                static_cast<std::streamsize>(packet.size()));
            raw_packets_++;
            raw_bytes_ += packet.size();
        }
        if (mp4_writer_) {
            mp4_writer_->push_packet(
                const_cast<uint8_t*>(packet.data()),
                static_cast<int>(packet.size()),
                mp4_pts_counter_++);
            mp4_packets_++;
            mp4_bytes_ += packet.size();
        }
        if (flush_packet) {
            flush_packets_++;
            flush_bytes_ += packet.size();
        }
    }

    void record_encode_sample(const EncodeSample& sample)
    {
        returned_packets_ += sample.returned_packets;
        returned_bytes_ += sample.returned_bytes;
        enqueue_age_samples_.push_back(sample.enqueue_age_ms);
        prepare_samples_.push_back(sample.prepare_ms);
        encode_total_samples_.push_back(sample.encode_total_ms);
        encode_picture_samples_.push_back(sample.encode_picture_ms);
        lock_bitstream_samples_.push_back(sample.lock_bitstream_ms);
        bitstream_fetch_samples_.push_back(sample.bitstream_fetch_ms);
    }

    void finish_mp4_writer()
    {
        if (!mp4_writer_) {
            return;
        }
        mp4_writer_->quit_thread();
        mp4_writer_->join_thread();
        writer_latency_ = mp4_writer_->latency_stats();
        mp4_queue_overflowed_ = mp4_writer_->has_queue_overflowed();
        mp4_queue_overflow_events_ = mp4_writer_->queue_overflow_events();
        mp4_peak_queued_packets_ = mp4_writer_->peak_queued_packets();
        mp4_peak_queued_bytes_ = mp4_writer_->peak_queued_bytes();
        mp4_writer_.reset();
    }

    void encode_one(const EncodeWorkItem& item)
    {
        initialize_encoder(item.desc);
        last_desc_ = item.desc;
        has_last_desc_ = true;
        EncodeSample sample;
        sample.encode_index = frames_encoded_.load(std::memory_order_relaxed);
        sample.source_frame_index = item.source_frame_index;
        sample.camera_serial = item.desc.camera_serial;
        sample.session_id = item.desc.session_id;
        sample.stream_id = item.desc.stream_id;
        sample.recording_frame_id = item.desc.recording_frame_id;
        sample.local_frame_id = item.desc.local_frame_id;
        sample.gop_index = item.desc.gop_index;
        sample.frame_index_within_gop = item.desc.frame_index_within_gop;
        sample.source_gpu_id = item.desc.source_gpu_id;
        sample.assigned_gpu_id = item.desc.assigned_gpu_id;
        sample.assigned_shard_id = item.desc.assigned_shard_id;
        sample.routing_policy = item.desc.routing_policy;
        sample.bytes = item.desc.bytes;
        sample.enqueue_age_ms = elapsed_ms(item.enqueued_at);

        const auto prepare_start = std::chrono::steady_clock::now();
        const NvEncInputFrame* input_frame = encoder_->GetNextInputFrame();
        if (!input_frame || !input_frame->inputPtr) {
            throw std::runtime_error("NvEncoder returned no input frame");
        }
        prepare_input_frame(item, *input_frame);
        sample.prepare_ms = ns_to_ms(elapsed_ns(prepare_start));

        NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
        pic_params.frameIdx = static_cast<uint32_t>(sample.encode_index & 0xffffffffu);
        pic_params.inputTimeStamp = item.desc.recording_frame_id;
        pic_params.inputDuration = 1;

        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint64_t> output_timestamps;
        NvEncoderEncodeFrameTiming timing;
        uint64_t fetch_ns = 0;
        const auto encode_start = std::chrono::steady_clock::now();
        encoder_->EncodeFrame(
            packets,
            &pic_params,
            nullptr,
            &output_timestamps,
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
        if (merged_output_) {
            merged_output_->submit_packets(packets, output_timestamps, item.desc);
        }
        for (const auto& packet : packets) {
            sample.returned_bytes += packet.size();
            push_packet_to_outputs(packet, false);
        }
        if (bitstream_out_ && !bitstream_out_) {
            throw std::runtime_error("Failed while writing bitstream output");
        }
        if (encode_csv_) {
            write_encode_csv_row(encode_csv_, sample);
            if ((sample.encode_index % 60) == 0) {
                encode_csv_.flush();
            }
        }
        record_encode_sample(sample);
        frames_encoded_.fetch_add(1, std::memory_order_relaxed);
    }

    void flush_encoder()
    {
        if (!encoder_) {
            return;
        }
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint64_t> output_timestamps;
        NvEncoderEncodeFrameTiming timing;
        uint64_t fetch_ns = 0;
        encoder_->EndEncode(packets, nullptr, &output_timestamps, &fetch_ns, &timing);
        if (merged_output_ && has_last_desc_) {
            merged_output_->submit_packets(packets, output_timestamps, last_desc_);
        }
        for (const auto& packet : packets) {
            push_packet_to_outputs(packet, true);
        }
        if (bitstream_out_) {
            bitstream_out_.flush();
        }
        if (encode_csv_) {
            encode_csv_.flush();
        }
        finish_mp4_writer();
    }

    void run()
    {
        try {
            while (true) {
                EncodeWorkItem item;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&]() { return stopping_ || !queue_.empty(); });
                    if (queue_.empty()) {
                        if (stopping_) {
                            break;
                        }
                        continue;
                    }
                    item = queue_.front();
                    queue_.pop_front();
                }
                encode_one(item);
                release_slot(item.slot_index);
            }
            flush_encoder();
            std::cout << "external_recorder_ipc_probe encoder complete"
                      << " encoded=" << frames_encoded()
                      << " dropped=" << frames_dropped()
                      << std::endl;
        } catch (const std::exception& ex) {
            failed_.store(true, std::memory_order_release);
            std::cerr << "external_recorder_ipc_probe encoder failed: "
                      << ex.what() << std::endl;
        }
    }

    Options options_;
    MergedGopOutput* merged_output_ = nullptr;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<EncodeWorkItem> queue_;
    std::vector<DeviceSlot> slots_;
    std::vector<size_t> free_slots_;
    bool have_next_encode_time_ = false;
    std::chrono::steady_clock::time_point next_encode_time_;
    CUcontext cu_context_ = nullptr;
    cudaStream_t stream_ = nullptr;
    std::unique_ptr<NvEncoderCuda> encoder_;
    std::unique_ptr<FFmpegWriter> mp4_writer_;
    std::ofstream bitstream_out_;
    std::ofstream encode_csv_;
    int64_t mp4_pts_counter_ = 0;
    uint64_t returned_packets_ = 0;
    uint64_t returned_bytes_ = 0;
    uint64_t raw_packets_ = 0;
    uint64_t raw_bytes_ = 0;
    uint64_t mp4_packets_ = 0;
    uint64_t mp4_bytes_ = 0;
    uint64_t flush_packets_ = 0;
    uint64_t flush_bytes_ = 0;
    bool mp4_queue_overflowed_ = false;
    uint64_t mp4_queue_overflow_events_ = 0;
    size_t mp4_peak_queued_packets_ = 0;
    size_t mp4_peak_queued_bytes_ = 0;
    FFmpegWriterLatencyStats writer_latency_;
    std::string resolved_mp4_keyframe_path_;
    std::vector<double> enqueue_age_samples_;
    std::vector<double> prepare_samples_;
    std::vector<double> encode_total_samples_;
    std::vector<double> encode_picture_samples_;
    std::vector<double> lock_bitstream_samples_;
    std::vector<double> bitstream_fetch_samples_;
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<bool> failed_{false};
    FrameDescriptor last_desc_;
    bool has_last_desc_ = false;
};

std::vector<int> effective_shard_gpu_ids(const Options& options)
{
    if (!options.shard_gpu_ids.empty()) {
        return options.shard_gpu_ids;
    }
    return {options.gpu_id};
}

Options make_shard_options(const Options& base,
                           size_t shard_index,
                           size_t shard_count,
                           int gpu_id)
{
    Options out = base;
    out.gpu_id = gpu_id;
    out.shard_id = static_cast<int>(shard_index);
    if (shard_count > 1) {
        out.routing_policy = "gop_modulo";
        const std::string suffix = shard_suffix(shard_index, gpu_id);
        out.encode_csv_path = add_suffix_to_path_stem(base.encode_csv_path, suffix);
        out.bitstream_out_path = add_suffix_to_path_stem(base.bitstream_out_path, suffix);
        out.mp4_out_path = add_suffix_to_path_stem(base.mp4_out_path, suffix);
        out.mp4_keyframe_path = add_suffix_to_path_stem(base.mp4_keyframe_path, suffix);
    }
    out.shard_gpu_ids.clear();
    return out;
}

void write_summary_json(const Options& options,
                        const std::string& session_id,
                        const std::string& stream_id,
                        uint64_t frames_received,
                        uint64_t acks_sent,
                        uint64_t detach_copied,
                        uint64_t opened_handles,
                        uint64_t encode_enqueued,
                        uint64_t encode_skipped,
                        uint64_t encode_dropped,
                        const std::vector<double>& detach_total_samples,
                        const std::vector<double>& detach_open_samples,
                        const std::vector<double>& detach_copy_samples,
                        const EncodeSummary* encode_summary,
                        const std::vector<EncodeSummary>& shard_summaries,
                        const MergedOutputSummary* merged_summary)
{
    if (options.summary_json_path.empty()) {
        return;
    }
    ensure_parent_directory(options.summary_json_path);
    std::ofstream out(options.summary_json_path, std::ios::out | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open summary JSON: " + options.summary_json_path);
    }

    const EncodeSummary empty_encode_summary;
    const EncodeSummary& enc = encode_summary ? *encode_summary : empty_encode_summary;
    const MergedOutputSummary empty_merged_summary;
    const MergedOutputSummary& merged =
        merged_summary ? *merged_summary : empty_merged_summary;
    const std::string output_mp4_path =
        merged.enabled ? merged.mp4_path : enc.mp4_path;
    const std::string output_mp4_keyframe_path =
        merged.enabled ? merged.mp4_keyframe_path : enc.mp4_keyframe_path;
    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"tool\": \"external_recorder_ipc_probe\",\n";
    out << "  \"session_id\": \"" << json_escape(session_id) << "\",\n";
    out << "  \"stream_id\": \"" << json_escape(stream_id) << "\",\n";
    out << "  \"socket_path\": \"" << json_escape(options.socket_path) << "\",\n";
    out << "  \"gpu_id\": " << options.gpu_id << ",\n";
    out << "  \"assigned_gpu_id\": " << enc.assigned_gpu_id << ",\n";
    out << "  \"assigned_shard_id\": " << enc.assigned_shard_id << ",\n";
    out << "  \"routing_policy\": \"" << json_escape(options.routing_policy) << "\",\n";
    out << "  \"shard_count\": " << shard_summaries.size() << ",\n";
    out << "  \"encode\": " << (options.encode ? "true" : "false") << ",\n";
    out << "  \"codec\": \"" << json_escape(options.codec) << "\",\n";
    out << "  \"preset\": \"" << json_escape(options.preset) << "\",\n";
    out << "  \"tuning\": \"" << json_escape(options.tuning) << "\",\n";
    out << "  \"fps\": " << options.fps << ",\n";
    out << "  \"encode_max_fps\": " << options.encode_max_fps << ",\n";
    out << "  \"encode_queue_depth\": " << options.encode_queue_depth << ",\n";
    out << "  \"frames_received\": " << frames_received << ",\n";
    out << "  \"acks_sent\": " << acks_sent << ",\n";
    out << "  \"detach_copied\": " << detach_copied << ",\n";
    out << "  \"opened_handles\": " << opened_handles << ",\n";
    out << "  \"encode_enqueued\": " << encode_enqueued << ",\n";
    out << "  \"encode_skipped\": " << encode_skipped << ",\n";
    out << "  \"encode_dropped\": " << encode_dropped << ",\n";
    out << "  \"frames_encoded\": " << enc.frames_encoded << ",\n";
    out << "  \"worker_failed\": " << ((enc.failed || merged.failed) ? "true" : "false") << ",\n";
    out << "  \"external_encode\": {\n";
    out << "    \"frames_dropped\": " << enc.frames_dropped << ",\n";
    out << "    \"returned_packets\": " << enc.returned_packets << ",\n";
    out << "    \"returned_bytes\": " << enc.returned_bytes << ",\n";
    out << "    \"raw_packets\": " << enc.raw_packets << ",\n";
    out << "    \"raw_bytes\": " << enc.raw_bytes << ",\n";
    out << "    \"mp4_packets\": " << enc.mp4_packets << ",\n";
    out << "    \"mp4_bytes\": " << enc.mp4_bytes << ",\n";
    out << "    \"flush_packets\": " << enc.flush_packets << ",\n";
    out << "    \"flush_bytes\": " << enc.flush_bytes << ",\n";
    out << "    \"enqueue_age_p95_ms\": " << enc.enqueue_age_p95_ms << ",\n";
    out << "    \"prepare_p95_ms\": " << enc.prepare_p95_ms << ",\n";
    out << "    \"encode_total_p95_ms\": " << enc.encode_total_p95_ms << ",\n";
    out << "    \"encode_picture_p95_ms\": " << enc.encode_picture_p95_ms << ",\n";
    out << "    \"lock_bitstream_p95_ms\": " << enc.lock_bitstream_p95_ms << ",\n";
    out << "    \"bitstream_fetch_p95_ms\": " << enc.bitstream_fetch_p95_ms << ",\n";
    out << "    \"mp4_push_mean_ms\": " << enc.mp4_push_mean_ms << ",\n";
    out << "    \"mp4_push_max_ms\": " << enc.mp4_push_max_ms << ",\n";
    out << "    \"mp4_write_mean_ms\": " << enc.mp4_write_mean_ms << ",\n";
    out << "    \"mp4_write_max_ms\": " << enc.mp4_write_max_ms << ",\n";
    out << "    \"mp4_queue_overflowed\": " << (enc.mp4_queue_overflowed ? "true" : "false") << ",\n";
    out << "    \"mp4_queue_overflow_events\": " << enc.mp4_queue_overflow_events << ",\n";
    out << "    \"mp4_peak_queued_packets\": " << enc.mp4_peak_queued_packets << ",\n";
    out << "    \"mp4_peak_queued_bytes\": " << enc.mp4_peak_queued_bytes << "\n";
    out << "  },\n";
    out << "  \"external_encode_shards\": [\n";
    for (size_t i = 0; i < shard_summaries.size(); ++i) {
        const EncodeSummary& shard = shard_summaries[i];
        out << "    {\n";
        out << "      \"assigned_gpu_id\": " << shard.assigned_gpu_id << ",\n";
        out << "      \"assigned_shard_id\": " << shard.assigned_shard_id << ",\n";
        out << "      \"routing_policy\": \"" << json_escape(shard.routing_policy) << "\",\n";
        out << "      \"frames_encoded\": " << shard.frames_encoded << ",\n";
        out << "      \"frames_dropped\": " << shard.frames_dropped << ",\n";
        out << "      \"returned_packets\": " << shard.returned_packets << ",\n";
        out << "      \"returned_bytes\": " << shard.returned_bytes << ",\n";
        out << "      \"mp4_packets\": " << shard.mp4_packets << ",\n";
        out << "      \"mp4_bytes\": " << shard.mp4_bytes << ",\n";
        out << "      \"encode_total_p95_ms\": " << shard.encode_total_p95_ms << ",\n";
        out << "      \"encode_picture_p95_ms\": " << shard.encode_picture_p95_ms << ",\n";
        out << "      \"lock_bitstream_p95_ms\": " << shard.lock_bitstream_p95_ms << ",\n";
        out << "      \"worker_failed\": " << (shard.failed ? "true" : "false") << ",\n";
        out << "      \"encode_csv\": \"" << json_escape(shard.encode_csv_path) << "\",\n";
        out << "      \"raw_bitstream\": \"" << json_escape(shard.bitstream_out_path) << "\",\n";
        out << "      \"mp4\": \"" << json_escape(shard.mp4_path) << "\",\n";
        out << "      \"mp4_keyframe\": \"" << json_escape(shard.mp4_keyframe_path) << "\"\n";
        out << "    }" << (i + 1 < shard_summaries.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"merged_output\": {\n";
    out << "    \"enabled\": " << (merged.enabled ? "true" : "false") << ",\n";
    out << "    \"failed\": " << (merged.failed ? "true" : "false") << ",\n";
    out << "    \"packets_written\": " << merged.packets_written << ",\n";
    out << "    \"bytes_written\": " << merged.bytes_written << ",\n";
    out << "    \"gops_released\": " << merged.gops_released << ",\n";
    out << "    \"pending_gops\": " << merged.pending_gops << ",\n";
    out << "    \"pending_bytes\": " << merged.pending_bytes << ",\n";
    out << "    \"mp4_queue_overflowed\": " << (merged.mp4_queue_overflowed ? "true" : "false") << ",\n";
    out << "    \"mp4_queue_overflow_events\": " << merged.mp4_queue_overflow_events << ",\n";
    out << "    \"mp4_peak_queued_packets\": " << merged.mp4_peak_queued_packets << ",\n";
    out << "    \"mp4_peak_queued_bytes\": " << merged.mp4_peak_queued_bytes << ",\n";
    out << "    \"mp4_push_mean_ms\": " << merged.mp4_push_mean_ms << ",\n";
    out << "    \"mp4_push_max_ms\": " << merged.mp4_push_max_ms << ",\n";
    out << "    \"mp4_write_mean_ms\": " << merged.mp4_write_mean_ms << ",\n";
    out << "    \"mp4_write_max_ms\": " << merged.mp4_write_max_ms << ",\n";
    out << "    \"mp4\": \"" << json_escape(merged.mp4_path) << "\",\n";
    out << "    \"mp4_keyframe\": \"" << json_escape(merged.mp4_keyframe_path) << "\",\n";
    out << "    \"error_message\": \"" << json_escape(merged.error_message) << "\"\n";
    out << "  },\n";
    out << "  \"detach_timing\": {\n";
    out << "    \"total_p95_ms\": " << percentile_ms(detach_total_samples, 95.0) << ",\n";
    out << "    \"open_handle_p95_ms\": " << percentile_ms(detach_open_samples, 95.0) << ",\n";
    out << "    \"copy_p95_ms\": " << percentile_ms(detach_copy_samples, 95.0) << "\n";
    out << "  },\n";
    out << "  \"outputs\": {\n";
    out << "    \"detach_csv\": \"" << json_escape(options.csv_path) << "\",\n";
    out << "    \"encode_csv\": \"" << json_escape(options.encode_csv_path) << "\",\n";
    out << "    \"gop_routing_csv\": \"" << json_escape(options.gop_routing_csv_path) << "\",\n";
    out << "    \"raw_bitstream\": \"" << json_escape(options.bitstream_out_path) << "\",\n";
    out << "    \"mp4\": \"" << json_escape(output_mp4_path) << "\",\n";
    out << "    \"mp4_keyframe\": \"" << json_escape(output_mp4_keyframe_path) << "\"\n";
    out << "  },\n";
    out << "  \"output_file_sizes\": {\n";
    out << "    \"detach_csv_bytes\": " << file_size_or_zero(options.csv_path) << ",\n";
    out << "    \"encode_csv_bytes\": " << file_size_or_zero(options.encode_csv_path) << ",\n";
    out << "    \"gop_routing_csv_bytes\": " << file_size_or_zero(options.gop_routing_csv_path) << ",\n";
    out << "    \"raw_bitstream_bytes\": " << file_size_or_zero(options.bitstream_out_path) << ",\n";
    out << "    \"mp4_bytes\": " << file_size_or_zero(output_mp4_path) << ",\n";
    out << "    \"mp4_keyframe_bytes\": " << file_size_or_zero(output_mp4_keyframe_path) << "\n";
    out << "  }\n";
    out << "}\n";
}

}  // namespace

int main(int argc, char** argv)
{
    int listen_fd = -1;
    int client_fd = -1;
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        check_cuda(cudaSetDevice(options.gpu_id), "cudaSetDevice");
        check_cuda(cudaFree(nullptr), "cudaFree(0)");

        listen_fd = create_listen_socket(options.socket_path);
        std::cout << "external_recorder_ipc_probe listening"
                  << " socket=" << options.socket_path
                  << " gpu_id=" << options.gpu_id << std::endl;

        client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            throw std::runtime_error("accept() failed: " + std::string(std::strerror(errno)));
        }
        std::cout << "external_recorder_ipc_probe connected" << std::endl;

        std::ofstream csv;
        if (!options.csv_path.empty()) {
            ensure_parent_directory(options.csv_path);
            csv.open(options.csv_path);
            if (!csv) {
                throw std::runtime_error("Failed to open CSV: " + options.csv_path);
            }
            write_csv_header(csv);
        }
        std::ofstream gop_routing_csv;
        if (!options.gop_routing_csv_path.empty()) {
            ensure_parent_directory(options.gop_routing_csv_path);
            gop_routing_csv.open(options.gop_routing_csv_path);
            if (!gop_routing_csv) {
                throw std::runtime_error("Failed to open GOP routing CSV: " +
                                         options.gop_routing_csv_path);
            }
            write_gop_routing_csv_header(gop_routing_csv);
        }

        std::unordered_map<std::string, ImportedHandle> imported_handles;
        void* owned_device_buffer = nullptr;
        uint64_t owned_device_buffer_bytes = 0;
        uint64_t frame_count = 0;
        uint64_t ack_count = 0;
        uint64_t detach_copied_count = 0;
        uint64_t opened_handles_count = 0;
        uint64_t encode_enqueued_count = 0;
        uint64_t encode_skipped_count = 0;
        uint64_t encode_dropped_count = 0;
        std::string observed_session_id = options.session_id;
        std::string observed_stream_id = options.stream_id;
        std::vector<double> detach_total_samples;
        std::vector<double> detach_open_samples;
        std::vector<double> detach_copy_samples;
        std::vector<std::unique_ptr<ExternalEncodeWorker>> encode_workers;
        std::unique_ptr<MergedGopOutput> merged_output;
        if (options.encode) {
            const std::vector<int> shard_gpu_ids = effective_shard_gpu_ids(options);
            if (shard_gpu_ids.size() > 1 && !options.mp4_out_path.empty()) {
                merged_output = std::make_unique<MergedGopOutput>(options);
            }
            encode_workers.reserve(shard_gpu_ids.size());
            for (size_t shard_index = 0; shard_index < shard_gpu_ids.size(); ++shard_index) {
                Options shard_options = make_shard_options(
                    options,
                    shard_index,
                    shard_gpu_ids.size(),
                    shard_gpu_ids[shard_index]);
                auto worker = std::make_unique<ExternalEncodeWorker>(
                    std::move(shard_options),
                    merged_output.get());
                worker->start();
                encode_workers.push_back(std::move(worker));
            }
        }

        while (!g_stop_requested.load(std::memory_order_acquire) &&
               (options.max_frames == 0 || frame_count < options.max_frames)) {
            std::string line;
            if (!read_line(client_fd, &line)) {
                break;
            }

            FrameDescriptor desc;
            if (!parse_frame_descriptor(line, &desc)) {
                std::cerr << "Malformed frame descriptor: " << line << std::endl;
                break;
            }
            apply_descriptor_defaults(options, &desc);
            ExternalEncodeWorker* target_encode_worker = nullptr;
            if (!encode_workers.empty()) {
                const size_t target_shard =
                    static_cast<size_t>(desc.gop_index % encode_workers.size());
                target_encode_worker = encode_workers[target_shard].get();
                desc.assigned_gpu_id = target_encode_worker->gpu_id();
                desc.assigned_shard_id = target_encode_worker->shard_id();
                desc.routing_policy = options.routing_policy;
            } else {
                desc.assigned_gpu_id = options.gpu_id;
                desc.assigned_shard_id = options.shard_id;
            }
            if (observed_session_id.empty()) {
                observed_session_id = desc.session_id;
            }
            if (observed_stream_id.empty()) {
                observed_stream_id = desc.stream_id;
            }

            Sample sample;
            sample.frame_index = frame_count;
            sample.camera_serial = desc.camera_serial;
            sample.session_id = desc.session_id;
            sample.stream_id = desc.stream_id;
            sample.recording_frame_id = desc.recording_frame_id;
            sample.local_frame_id = desc.local_frame_id;
            sample.gop_index = desc.gop_index;
            sample.frame_index_within_gop = desc.frame_index_within_gop;
            sample.source_gpu_id = desc.source_gpu_id;
            sample.assigned_gpu_id = desc.assigned_gpu_id;
            sample.assigned_shard_id = desc.assigned_shard_id;
            sample.routing_policy = desc.routing_policy;
            sample.bytes = desc.bytes;

            const auto total_start = std::chrono::steady_clock::now();
            const bool should_detach_copy =
                !target_encode_worker || target_encode_worker->should_encode(total_start);

            if (should_detach_copy) {
                auto handle_it = imported_handles.find(desc.handle_hex);
                if (handle_it == imported_handles.end()) {
                    const auto open_start = std::chrono::steady_clock::now();
                    cudaIpcMemHandle_t handle{};
                    if (!hex_to_ipc_handle(desc.handle_hex, &handle)) {
                        throw std::runtime_error("Invalid CUDA IPC handle hex");
                    }
                    void* imported_ptr = nullptr;
                    check_cuda(
                        cudaIpcOpenMemHandle(
                            &imported_ptr,
                            handle,
                            cudaIpcMemLazyEnablePeerAccess),
                        "cudaIpcOpenMemHandle");
                    sample.open_handle_ms = elapsed_ms(open_start);
                    sample.opened_handle = true;
                    handle_it = imported_handles.emplace(
                        desc.handle_hex,
                        ImportedHandle{imported_ptr}).first;
                }

                const auto copy_start = std::chrono::steady_clock::now();
                if (target_encode_worker) {
                    target_encode_worker->detach_and_enqueue(
                        desc,
                        handle_it->second.ptr,
                        frame_count,
                        &sample);
                } else {
                    if (desc.bytes > owned_device_buffer_bytes) {
                        if (owned_device_buffer) {
                            cudaFree(owned_device_buffer);
                            owned_device_buffer = nullptr;
                            owned_device_buffer_bytes = 0;
                        }
                        check_cuda(
                            cudaMalloc(&owned_device_buffer, desc.bytes),
                            "cudaMalloc(owned detach buffer)");
                        owned_device_buffer_bytes = desc.bytes;
                    }
                    check_cuda(
                        cudaMemcpy(
                            owned_device_buffer,
                            handle_it->second.ptr,
                            desc.bytes,
                            cudaMemcpyDeviceToDevice),
                        "cudaMemcpy(detach copy)");
                }
                sample.copy_ms = elapsed_ms(copy_start);
            } else {
                sample.detach_copied = false;
                sample.encode_skipped = true;
            }
            sample.total_ms = elapsed_ms(total_start);

            if (!write_all(client_fd,
                           "ACK " + std::to_string(desc.recording_frame_id) + " " +
                               std::to_string(desc.assigned_gpu_id) + " " +
                               std::to_string(desc.assigned_shard_id) + "\n")) {
                break;
            }
            ++ack_count;
            if (sample.detach_copied) {
                ++detach_copied_count;
            }
            if (sample.opened_handle) {
                ++opened_handles_count;
            }
            if (sample.encode_enqueued) {
                ++encode_enqueued_count;
            }
            if (sample.encode_skipped) {
                ++encode_skipped_count;
            }
            if (sample.encode_dropped) {
                ++encode_dropped_count;
            }
            detach_total_samples.push_back(sample.total_ms);
            detach_open_samples.push_back(sample.open_handle_ms);
            detach_copy_samples.push_back(sample.copy_ms);
            if (csv) {
                write_csv_row(csv, sample);
                if ((frame_count % 60) == 0) {
                    csv.flush();
                }
            }
            if (gop_routing_csv) {
                write_gop_routing_csv_row(gop_routing_csv, sample);
                if ((frame_count % 60) == 0) {
                    gop_routing_csv.flush();
                }
            }
            ++frame_count;
        }

        for (auto& worker : encode_workers) {
            if (worker) {
                worker->stop();
            }
        }
        if (merged_output) {
            merged_output->finish();
        }
        if (csv) {
            csv.flush();
        }
        if (gop_routing_csv) {
            gop_routing_csv.flush();
        }
        std::vector<EncodeSummary> shard_summaries;
        shard_summaries.reserve(encode_workers.size());
        for (const auto& worker : encode_workers) {
            if (worker) {
                shard_summaries.push_back(worker->summary());
            }
        }
        const EncodeSummary encode_summary = aggregate_encode_summaries(shard_summaries);
        const MergedOutputSummary merged_summary =
            merged_output ? merged_output->summary() : MergedOutputSummary{};
        write_summary_json(
            options,
            observed_session_id,
            observed_stream_id,
            frame_count,
            ack_count,
            detach_copied_count,
            opened_handles_count,
            encode_enqueued_count,
            encode_skipped_count,
            encode_dropped_count,
            detach_total_samples,
            detach_open_samples,
            detach_copy_samples,
            encode_workers.empty() ? nullptr : &encode_summary,
            shard_summaries,
            merged_output ? &merged_summary : nullptr);
        for (auto& [handle_hex, imported] : imported_handles) {
            (void)handle_hex;
            if (imported.ptr) {
                cudaIpcCloseMemHandle(imported.ptr);
            }
        }
        if (owned_device_buffer) {
            cudaFree(owned_device_buffer);
        }
        if (client_fd >= 0) {
            close(client_fd);
        }
        if (listen_fd >= 0) {
            close(listen_fd);
            unlink(options.socket_path.c_str());
        }
        std::cout << "external_recorder_ipc_probe complete frames=" << frame_count;
        bool any_worker_failed = false;
        if (merged_output && merged_summary.failed) {
            any_worker_failed = true;
        }
        if (!encode_workers.empty()) {
            uint64_t total_encoded = 0;
            uint64_t total_dropped = 0;
            for (const auto& worker : encode_workers) {
                if (!worker) {
                    continue;
                }
                total_encoded += worker->frames_encoded();
                total_dropped += worker->frames_dropped();
                any_worker_failed = any_worker_failed || worker->failed();
            }
            std::cout << " encoded=" << total_encoded
                      << " encode_dropped=" << total_dropped
                      << " worker_failed=" << (any_worker_failed ? "true" : "false");
        }
        std::cout << std::endl;
        return any_worker_failed ? 1 : 0;
    } catch (const std::exception& ex) {
        if (client_fd >= 0) {
            close(client_fd);
        }
        if (listen_fd >= 0) {
            close(listen_fd);
        }
        std::cerr << "external_recorder_ipc_probe failed: " << ex.what() << std::endl;
        return 1;
    }
}
