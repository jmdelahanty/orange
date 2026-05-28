#include "NvEncoder/NvEncoderCuda.h"
#include "FFmpegWriter.h"
#include "fsuid_guard.h"

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
    uint32_t encode_prewarm_slots = 0;
    uint64_t encode_prewarm_bytes = 0;
    bool encode_prewarm_peer_copy = false;
    bool direct_input_source = false;
    bool deferred_source_release = false;
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
    std::string status_json_path;
    std::string gop_routing_csv_path;
    std::string session_id;
    std::string stream_id;
    uint32_t record_for_seconds = 0;
    uint32_t clip_seconds = 0;
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
        << "  --prewarm-slots <int> Preallocate this many encode detach slots per shard. Default 0.\n"
        << "  --prewarm-bytes <int> Frame byte size for pre-listen detach slot allocation. Default 0.\n"
        << "  --prewarm-peer-copy   After first IPC import, copy 1 byte into each shard to warm peer paths.\n"
        << "  --direct-input-source Copy IPC source directly into NVENC input before ACK. Experimental.\n"
        << "  --deferred-source-release Send RELEASE after source consumption; ACK only accepts work. Experimental.\n"
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
        << "  --status-json <path>   Optional live status/heartbeat JSON.\n"
        << "  --gop-routing-csv <path> Optional per-frame route/shard CSV.\n"
        << "  --session-id <id>     Session id for artifacts. Defaults to first descriptor.\n"
        << "  --stream-id <id>      Stream id for artifacts. Defaults to camera serial.\n"
        << "  --record-for-seconds <int> Session recording duration intent. Default 0.\n"
        << "  --clip-seconds <int>  Enable GOP-aligned rolling clip MP4 outputs. Default 0.\n"
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

bool env_flag_enabled(const char* name, bool default_value = false)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return default_value;
    }
    const std::string normalized = lower_ascii(value);
    return normalized == "1" ||
           normalized == "true" ||
           normalized == "yes" ||
           normalized == "on";
}

Options parse_options(int argc, char** argv)
{
    Options options;
    options.direct_input_source =
        env_flag_enabled("ORANGE_EXTERNAL_RECORDER_DIRECT_INPUT", false);
    options.deferred_source_release =
        env_flag_enabled("ORANGE_EXTERNAL_RECORDER_DEFERRED_RELEASE", false);
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
        } else if (arg == "--prewarm-slots") {
            options.encode_prewarm_slots = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--prewarm-bytes") {
            options.encode_prewarm_bytes = parse_u64(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--prewarm-peer-copy") {
            options.encode_prewarm_peer_copy = true;
        } else if (arg == "--direct-input-source") {
            options.direct_input_source = true;
        } else if (arg == "--deferred-source-release") {
            options.deferred_source_release = true;
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
        } else if (arg == "--status-json") {
            options.status_json_path = consume(arg.c_str());
        } else if (arg == "--gop-routing-csv") {
            options.gop_routing_csv_path = consume(arg.c_str());
        } else if (arg == "--session-id") {
            options.session_id = consume(arg.c_str());
        } else if (arg == "--stream-id") {
            options.stream_id = consume(arg.c_str());
        } else if (arg == "--record-for-seconds") {
            options.record_for_seconds = parse_u32(consume(arg.c_str()), arg.c_str());
        } else if (arg == "--clip-seconds") {
            options.clip_seconds = parse_u32(consume(arg.c_str()), arg.c_str());
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
    if (options.encode_prewarm_slots > options.encode_queue_depth) {
        throw std::runtime_error("--prewarm-slots must be less than or equal to --encode-queue-depth");
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
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(parent);
    }
}

std::string derive_keyframe_path(const std::string& mp4_path)
{
    std::filesystem::path path(mp4_path);
    const std::string stem = path.stem().string();
    path.replace_filename(stem + "_keyframes.json");
    return path.string();
}

std::string normalize_keyframe_sidecar_path(const std::string& keyframe_path)
{
    if (keyframe_path.empty()) {
        return {};
    }
    std::filesystem::path path(keyframe_path);
    if (path.extension() == ".csv") {
        path.replace_extension(".json");
    } else if (path.extension().empty()) {
        path += ".json";
    }
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

std::string format_clip_id(int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
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

struct RollingStatusSnapshot {
    bool enabled = false;
    uint32_t record_for_seconds = 0;
    uint32_t clip_seconds = 0;
    uint64_t clip_span_frames = 0;
    uint64_t target_frame_count = 0;
    int current_clip_index = 0;
    uint64_t next_rollover_at_recording_frame_id = 0;
    uint64_t frames_until_next_rollover = 0;
    uint64_t completed_clip_count = 0;
    int last_completed_clip_index = -1;
    uint64_t last_completed_clip_last_recording_frame_id = 0;
    uint64_t last_completed_clip_frame_count = 0;
    std::string last_rollover_status = "none";
};

RollingStatusSnapshot rolling_status_from_progress(const Options& options,
                                                   const uint64_t frames_received)
{
    RollingStatusSnapshot status;
    status.enabled = options.clip_seconds > 0;
    status.record_for_seconds = options.record_for_seconds;
    status.clip_seconds = options.clip_seconds;
    if (!status.enabled) {
        return status;
    }

    const uint64_t fps = static_cast<uint64_t>(std::max<uint32_t>(1, options.fps));
    const uint64_t gop = static_cast<uint64_t>(std::max<uint32_t>(1, options.gop));
    const uint64_t requested_clip_frames =
        static_cast<uint64_t>(std::max<uint32_t>(1, options.clip_seconds)) * fps;
    status.clip_span_frames =
        ((requested_clip_frames + gop - 1) / gop) * gop;
    if (options.record_for_seconds > 0) {
        status.target_frame_count =
            static_cast<uint64_t>(options.record_for_seconds) * fps;
    }
    if (status.clip_span_frames == 0) {
        return status;
    }

    const uint64_t zero_based_frame =
        frames_received > 0 ? frames_received - 1 : 0;
    status.current_clip_index =
        static_cast<int>(zero_based_frame / status.clip_span_frames);
    if (status.target_frame_count > 0) {
        const uint64_t expected_clip_count =
            (status.target_frame_count + status.clip_span_frames - 1) /
            status.clip_span_frames;
        const int final_clip_index =
            static_cast<int>(expected_clip_count > 0 ? expected_clip_count - 1 : 0);
        const uint64_t overrun_frames =
            frames_received > status.target_frame_count
                ? frames_received - status.target_frame_count
                : 0;
        if (status.current_clip_index > final_clip_index &&
            overrun_frames > 0 &&
            overrun_frames <= gop) {
            status.current_clip_index = final_clip_index;
        }
    }

    const uint64_t current_clip_end_frame =
        (static_cast<uint64_t>(std::max(0, status.current_clip_index)) + 1) *
        status.clip_span_frames;
    status.next_rollover_at_recording_frame_id = current_clip_end_frame + 1;
    status.frames_until_next_rollover =
        frames_received < current_clip_end_frame
            ? current_clip_end_frame - frames_received
            : 0;
    return status;
}

std::vector<int> effective_shard_gpu_ids(const Options& options);

bool write_recorder_status_json(const Options& options,
                                const std::string& session_id,
                                const std::string& stream_id,
                                const std::string& status,
                                const uint64_t heartbeat_sequence,
                                const uint64_t frames_received,
                                const uint64_t acks_sent,
                                const uint64_t detach_copied,
                                const uint64_t encode_enqueued,
                                const uint64_t encode_skipped,
                                const uint64_t encode_dropped,
                                const uint64_t encode_queue_high_water,
                                const uint64_t frames_encoded,
                                const uint64_t frames_dropped,
                                const bool worker_failed,
                                const std::string& error_message = {},
                                const RollingStatusSnapshot& rolling_status = {})
{
    if (options.status_json_path.empty()) {
        return true;
    }

    try {
        ensure_parent_directory(options.status_json_path);
        const std::string temp_path = options.status_json_path + ".tmp";
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            std::ofstream out(temp_path, std::ios::out | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << "{\n";
            out << "  \"schema_id\": \"orange.external_recorder.status\",\n";
            out << "  \"schema_version\": 1,\n";
            out << "  \"tool\": \"external_recorder_ipc_probe\",\n";
            out << "  \"status\": \"" << json_escape(status) << "\",\n";
            out << "  \"session_id\": \"" << json_escape(session_id) << "\",\n";
            out << "  \"stream_id\": \"" << json_escape(stream_id) << "\",\n";
            out << "  \"socket_path\": \"" << json_escape(options.socket_path) << "\",\n";
            out << "  \"status_json\": \"" << json_escape(options.status_json_path) << "\",\n";
            out << "  \"steady_clock_ns\": " << steady_clock_now_ns() << ",\n";
            out << "  \"heartbeat_sequence\": " << heartbeat_sequence << ",\n";
            out << "  \"recorder_gpu_id\": " << options.gpu_id << ",\n";
            out << "  \"shard_gpu_ids\": [";
            const std::vector<int> shard_gpu_ids = effective_shard_gpu_ids(options);
            for (size_t i = 0; i < shard_gpu_ids.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << shard_gpu_ids[i];
            }
            out << "],\n";
            out << "  \"routing_policy\": \"" << json_escape(options.routing_policy) << "\",\n";
            out << "  \"recording_control\": {\n";
            out << "    \"record_for_seconds\": " << options.record_for_seconds << ",\n";
            out << "    \"clip_seconds\": " << options.clip_seconds << "\n";
            out << "  },\n";
            out << "  \"rolling\": {\n";
            out << "    \"enabled\": " << (rolling_status.enabled ? "true" : "false") << ",\n";
            out << "    \"implementation\": \""
                << (rolling_status.enabled
                        ? "external_recorder_gop_boundary_writer_rotation"
                        : "none") << "\",\n";
            out << "    \"record_for_seconds\": "
                << rolling_status.record_for_seconds << ",\n";
            out << "    \"clip_seconds\": " << rolling_status.clip_seconds << ",\n";
            out << "    \"clip_span_frames\": "
                << rolling_status.clip_span_frames << ",\n";
            out << "    \"target_frame_count\": "
                << rolling_status.target_frame_count << ",\n";
            out << "    \"current_clip_index\": "
                << rolling_status.current_clip_index << ",\n";
            out << "    \"next_rollover_at_recording_frame_id\": "
                << rolling_status.next_rollover_at_recording_frame_id << ",\n";
            out << "    \"frames_until_next_rollover\": "
                << rolling_status.frames_until_next_rollover << ",\n";
            out << "    \"completed_clip_count\": "
                << rolling_status.completed_clip_count << ",\n";
            out << "    \"last_completed_clip_index\": "
                << rolling_status.last_completed_clip_index << ",\n";
            out << "    \"last_completed_clip_last_recording_frame_id\": "
                << rolling_status.last_completed_clip_last_recording_frame_id << ",\n";
            out << "    \"last_completed_clip_frame_count\": "
                << rolling_status.last_completed_clip_frame_count << ",\n";
            out << "    \"last_rollover_status\": \""
                << json_escape(rolling_status.last_rollover_status) << "\"\n";
            out << "  },\n";
            out << "  \"frames_received\": " << frames_received << ",\n";
            out << "  \"acks_sent\": " << acks_sent << ",\n";
            out << "  \"detach_copied\": " << detach_copied << ",\n";
            out << "  \"encode_enqueued\": " << encode_enqueued << ",\n";
            out << "  \"encode_skipped\": " << encode_skipped << ",\n";
            out << "  \"encode_dropped\": " << encode_dropped << ",\n";
            out << "  \"encode_queue_high_water\": " << encode_queue_high_water << ",\n";
            out << "  \"frames_encoded\": " << frames_encoded << ",\n";
            out << "  \"frames_dropped\": " << frames_dropped << ",\n";
            out << "  \"worker_failed\": " << (worker_failed ? "true" : "false");
            if (!error_message.empty()) {
                out << ",\n  \"error\": \"" << json_escape(error_message) << "\"\n";
            } else {
                out << "\n";
            }
            out << "}\n";
            out.close();
            if (!out) {
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::rename(temp_path, options.status_json_path, ec);
        if (ec) {
            std::filesystem::remove(options.status_json_path, ec);
            ec.clear();
            std::filesystem::rename(temp_path, options.status_json_path, ec);
        }
        return !ec;
    } catch (...) {
        return false;
    }
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

bool write_protocol_line(int fd, std::mutex* mutex, const std::string& line)
{
    if (mutex) {
        std::lock_guard<std::mutex> lock(*mutex);
        return write_all(fd, line);
    }
    return write_all(fd, line);
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
    double slot_reuse_wait_ms = 0.0;
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
    uint64_t source_releases_sent = 0;
    uint64_t source_release_failures = 0;
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
    double slot_reuse_wait_p95_ms = 0.0;
    double encode_total_p95_ms = 0.0;
    double encode_picture_p95_ms = 0.0;
    double lock_bitstream_p95_ms = 0.0;
    double bitstream_fetch_p95_ms = 0.0;
    double mp4_push_mean_ms = 0.0;
    double mp4_push_max_ms = 0.0;
    double mp4_write_mean_ms = 0.0;
    double mp4_write_max_ms = 0.0;
    uint64_t prewarm_slots = 0;
    double prewarm_ms = 0.0;
    bool prewarm_peer_copy = false;
    std::string encode_csv_path;
    std::string bitstream_out_path;
    std::string mp4_path;
    std::string mp4_keyframe_path;
};

struct RollingClipOutputSummary {
    int clip_index = 0;
    std::string clip_id;
    std::string directory;
    std::string mp4_path;
    std::string metadata_path;
    std::string keyframe_path;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t frame_count = 0;
    uint64_t packets_written = 0;
    uint64_t bytes_written = 0;
    uint64_t gops_released = 0;
    bool failed = false;
};

struct RollingOutputSummary {
    bool enabled = false;
    std::string implementation;
    uint32_t record_for_seconds = 0;
    uint32_t clip_seconds = 0;
    uint64_t clip_span_frames = 0;
    uint64_t clip_span_gops = 0;
    uint64_t target_frame_count = 0;
    uint64_t terminal_tail_coalesce_frames = 0;
    uint64_t terminal_tail_coalesced_frames = 0;
    std::vector<RollingClipOutputSummary> clips;
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
    RollingOutputSummary rolling;
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
        out.source_releases_sent += summary.source_releases_sent;
        out.source_release_failures += summary.source_release_failures;
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
        out.slot_reuse_wait_p95_ms =
            std::max(out.slot_reuse_wait_p95_ms, summary.slot_reuse_wait_p95_ms);
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
        out.prewarm_slots += summary.prewarm_slots;
        out.prewarm_ms += summary.prewarm_ms;
        out.prewarm_peer_copy = out.prewarm_peer_copy || summary.prewarm_peer_copy;
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
           "enqueue_age_ms,prepare_ms,slot_reuse_wait_ms,encode_total_ms,"
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
        << sample.slot_reuse_wait_ms << ","
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
    cudaEvent_t detach_ready_event = nullptr;
    cudaEvent_t slot_reusable_event = nullptr;
    bool detach_ready_recorded = false;
    bool slot_reusable_recorded = false;
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
                  : normalize_keyframe_sidecar_path(options_.mp4_keyframe_path))
    {
        rolling_enabled_ = options_.clip_seconds > 0;
        if (rolling_enabled_) {
            const uint64_t requested_clip_frames =
                static_cast<uint64_t>(std::max<uint32_t>(1, options_.clip_seconds)) *
                static_cast<uint64_t>(std::max<uint32_t>(1, options_.fps));
            clip_span_gops_ = std::max<uint64_t>(
                1,
                (requested_clip_frames + static_cast<uint64_t>(gop_length_) - 1) /
                    static_cast<uint64_t>(gop_length_));
            clip_span_frames_ = clip_span_gops_ * static_cast<uint64_t>(gop_length_);
            if (options_.record_for_seconds > 0) {
                target_frame_count_ =
                    static_cast<uint64_t>(options_.record_for_seconds) *
                    static_cast<uint64_t>(std::max<uint32_t>(1, options_.fps));
                terminal_tail_coalesce_frames_ = static_cast<uint64_t>(gop_length_);
            }
        }
    }

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
        gop.frames.push_back(SubmittedFrame{
            desc.camera_serial,
            desc.session_id,
            desc.stream_id,
            desc.recording_frame_id,
            desc.local_frame_id,
            desc.gop_index,
            desc.frame_index_within_gop,
            desc.source_gpu_id,
            desc.assigned_gpu_id,
            desc.assigned_shard_id,
            desc.width,
            desc.height,
            desc.bytes,
            desc.timestamp,
            desc.timestamp_sys});
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
        finish_clip_writer_locked();
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
        out.rolling.enabled = rolling_enabled_;
        out.rolling.implementation =
            rolling_enabled_ ? "external_recorder_gop_boundary_writer_rotation" : "none";
        out.rolling.record_for_seconds = options_.record_for_seconds;
        out.rolling.clip_seconds = options_.clip_seconds;
        out.rolling.clip_span_frames = clip_span_frames_;
        out.rolling.clip_span_gops = clip_span_gops_;
        out.rolling.target_frame_count = target_frame_count_;
        out.rolling.terminal_tail_coalesce_frames = terminal_tail_coalesce_frames_;
        out.rolling.terminal_tail_coalesced_frames = terminal_tail_coalesced_frames_;
        out.rolling.clips = clip_summaries_;
        return out;
    }

private:
    struct BufferedPacket {
        std::vector<uint8_t> bytes;
        uint64_t zero_based_frame = 0;
    };

    struct SubmittedFrame {
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
        uint64_t bytes = 0;
        uint64_t timestamp = 0;
        uint64_t timestamp_sys = 0;
    };

    struct PendingGop {
        uint64_t gop_index = 0;
        uint64_t submitted_count = 0;
        uint64_t emitted_count = 0;
        uint64_t total_bytes = 0;
        bool submitted_complete = false;
        bool complete = false;
        std::vector<BufferedPacket> packets;
        std::vector<SubmittedFrame> frames;
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
        if (!writer_->is_open()) {
            failed_ = true;
            error_message_ = "failed to open merged MP4 output: " + mp4_path_;
            throw std::runtime_error(error_message_);
        }
        writer_->create_thread();
    }

    int raw_clip_index_for_zero_based_frame(uint64_t zero_based_frame) const
    {
        if (!rolling_enabled_ || clip_span_frames_ == 0) {
            return 0;
        }
        return static_cast<int>(zero_based_frame / clip_span_frames_);
    }

    int expected_final_clip_index() const
    {
        if (!rolling_enabled_ || clip_span_frames_ == 0 || target_frame_count_ == 0) {
            return -1;
        }
        const uint64_t expected_clip_count =
            (target_frame_count_ + clip_span_frames_ - 1) / clip_span_frames_;
        return static_cast<int>(expected_clip_count > 0 ? expected_clip_count - 1 : 0);
    }

    bool should_coalesce_terminal_tail(uint64_t zero_based_frame) const
    {
        if (target_frame_count_ == 0 || terminal_tail_coalesce_frames_ == 0) {
            return false;
        }
        if (zero_based_frame < target_frame_count_) {
            return false;
        }
        const uint64_t one_based_frame = zero_based_frame + 1;
        const uint64_t overrun_frames = one_based_frame - target_frame_count_;
        return overrun_frames > 0 && overrun_frames <= terminal_tail_coalesce_frames_;
    }

    int clip_index_for_zero_based_frame(uint64_t zero_based_frame) const
    {
        const int raw_clip_index = raw_clip_index_for_zero_based_frame(zero_based_frame);
        const int final_clip_index = expected_final_clip_index();
        if (final_clip_index >= 0 &&
            raw_clip_index > final_clip_index &&
            should_coalesce_terminal_tail(zero_based_frame)) {
            return final_clip_index;
        }
        return raw_clip_index;
    }

    std::filesystem::path clip_directory_path(int clip_index) const
    {
        const std::filesystem::path root =
            std::filesystem::path(mp4_path_).parent_path();
        return root / "clips" / format_clip_id(clip_index);
    }

    std::string clip_output_path(int clip_index, const std::string& suffix) const
    {
        const std::filesystem::path base(mp4_path_);
        return (clip_directory_path(clip_index) /
                (base.stem().string() + suffix)).string();
    }

    void finish_clip_writer_locked()
    {
        if (clip_writer_) {
            clip_writer_->quit_thread();
            clip_writer_->join_thread();
            const bool overflowed = clip_writer_->has_queue_overflowed();
            current_clip_summary_.failed = current_clip_summary_.failed || overflowed;
            failed_ = failed_ || overflowed;
            if (overflowed && error_message_.empty()) {
                error_message_ = "rolling clip MP4 writer queue overflowed";
            }
            clip_writer_.reset();
        }
        if (clip_metadata_) {
            clip_metadata_->flush();
            clip_metadata_.reset();
        }
        if (current_clip_index_ >= 0 &&
            (current_clip_summary_.frame_count > 0 ||
             current_clip_summary_.packets_written > 0)) {
            clip_summaries_.push_back(current_clip_summary_);
        }
        current_clip_summary_ = RollingClipOutputSummary{};
        current_clip_index_ = -1;
        current_clip_pts_counter_ = 0;
    }

    void ensure_clip_writer_locked(const FrameDescriptor& desc, int clip_index)
    {
        if (!rolling_enabled_ || mp4_path_.empty()) {
            return;
        }
        if (clip_writer_ && current_clip_index_ == clip_index) {
            return;
        }
        if (current_clip_index_ >= 0 && current_clip_index_ != clip_index) {
            finish_clip_writer_locked();
        }

        current_clip_index_ = clip_index;
        current_clip_pts_counter_ = 0;
        current_clip_summary_ = RollingClipOutputSummary{};
        current_clip_summary_.clip_index = clip_index;
        current_clip_summary_.clip_id = format_clip_id(clip_index);
        current_clip_summary_.directory = clip_directory_path(clip_index).string();
        current_clip_summary_.mp4_path = clip_output_path(clip_index, ".mp4");
        current_clip_summary_.metadata_path = clip_output_path(clip_index, "_meta.csv");
        current_clip_summary_.keyframe_path = clip_output_path(clip_index, "_keyframe.json");

        ensure_parent_directory(current_clip_summary_.mp4_path);
        ensure_parent_directory(current_clip_summary_.metadata_path);
        ensure_parent_directory(current_clip_summary_.keyframe_path);
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            clip_metadata_ = std::make_unique<std::ofstream>(
                current_clip_summary_.metadata_path,
                std::ios::out | std::ios::trunc);
        }
        if (!clip_metadata_ || !*clip_metadata_) {
            throw std::runtime_error(
                "failed to open rolling clip metadata: " +
                current_clip_summary_.metadata_path);
        }
        *clip_metadata_
            << "recording_frame_id,local_frame_id,gop_index,frame_index_within_gop,"
               "timestamp,timestamp_sys,source_gpu_id,assigned_gpu_id,assigned_shard_id,bytes\n";

        const AVCodecID codec_id =
            options_.codec == "h264" ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
        const std::vector<std::pair<std::string, std::string>> metadata_tags = {
            {"producer", "external_recorder_ipc_probe"},
            {"camera_serial", desc.camera_serial},
            {"codec", options_.codec},
            {"preset", options_.preset},
            {"tuning", options_.tuning},
            {"external_recorder", "true"},
            {"rolling_clip", "true"},
            {"clip_id", current_clip_summary_.clip_id},
            {"routing_policy", options_.routing_policy}
        };
        clip_writer_ = std::make_unique<FFmpegWriter>(
            codec_id,
            desc.width,
            desc.height,
            static_cast<int>(std::max<uint32_t>(1, options_.fps)),
            current_clip_summary_.mp4_path.c_str(),
            current_clip_summary_.keyframe_path.c_str(),
            metadata_tags);
        if (!clip_writer_->is_open()) {
            failed_ = true;
            error_message_ = "failed to open rolling clip MP4 output: " +
                current_clip_summary_.mp4_path;
            throw std::runtime_error(error_message_);
        }
        clip_writer_->create_thread();
    }

    void write_clip_metadata_rows_locked(PendingGop& gop)
    {
        if (!rolling_enabled_ || !clip_metadata_) {
            return;
        }
        std::stable_sort(
            gop.frames.begin(),
            gop.frames.end(),
            [](const SubmittedFrame& lhs, const SubmittedFrame& rhs) {
                return lhs.recording_frame_id < rhs.recording_frame_id;
            });
        for (const SubmittedFrame& frame : gop.frames) {
            *clip_metadata_
                << frame.recording_frame_id << ","
                << frame.local_frame_id << ","
                << frame.gop_index << ","
                << frame.frame_index_within_gop << ","
                << frame.timestamp << ","
                << frame.timestamp_sys << ","
                << frame.source_gpu_id << ","
                << frame.assigned_gpu_id << ","
                << frame.assigned_shard_id << ","
                << frame.bytes << "\n";
            if (current_clip_summary_.first_recording_frame_id == 0 ||
                frame.recording_frame_id <
                    current_clip_summary_.first_recording_frame_id) {
                current_clip_summary_.first_recording_frame_id =
                    frame.recording_frame_id;
            }
            current_clip_summary_.last_recording_frame_id =
                std::max(
                    current_clip_summary_.last_recording_frame_id,
                    frame.recording_frame_id);
            current_clip_summary_.frame_count++;
        }
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
        const uint64_t first_zero_based_frame =
            !gop.packets.empty()
                ? gop.packets.front().zero_based_frame
                : gop.gop_index * static_cast<uint64_t>(gop_length_);
        const int raw_clip_index =
            raw_clip_index_for_zero_based_frame(first_zero_based_frame);
        const int assigned_clip_index =
            clip_index_for_zero_based_frame(first_zero_based_frame);
        if (rolling_enabled_) {
            FrameDescriptor clip_desc;
            if (!gop.frames.empty()) {
                const SubmittedFrame& frame = gop.frames.front();
                clip_desc.camera_serial = frame.camera_serial;
                clip_desc.session_id = frame.session_id;
                clip_desc.stream_id = frame.stream_id;
                clip_desc.recording_frame_id = frame.recording_frame_id;
                clip_desc.local_frame_id = frame.local_frame_id;
                clip_desc.gop_index = frame.gop_index;
                clip_desc.frame_index_within_gop = frame.frame_index_within_gop;
                clip_desc.source_gpu_id = frame.source_gpu_id;
                clip_desc.assigned_gpu_id = frame.assigned_gpu_id;
                clip_desc.assigned_shard_id = frame.assigned_shard_id;
                clip_desc.width = frame.width;
                clip_desc.height = frame.height;
            }
            ensure_clip_writer_locked(
                clip_desc,
                assigned_clip_index);
            if (assigned_clip_index != raw_clip_index &&
                should_coalesce_terminal_tail(first_zero_based_frame)) {
                for (const SubmittedFrame& frame : gop.frames) {
                    if (frame.recording_frame_id > target_frame_count_) {
                        terminal_tail_coalesced_frames_++;
                    }
                }
            }
            write_clip_metadata_rows_locked(gop);
        }
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
            if (rolling_enabled_ && clip_writer_) {
                clip_writer_->push_packet(
                    const_cast<uint8_t*>(packet.bytes.data()),
                    static_cast<int>(packet.bytes.size()),
                    static_cast<int64_t>(current_clip_pts_counter_++),
                    gop.gop_index,
                    i + 1 == gop.packets.size(),
                    release_started_ns);
                current_clip_summary_.packets_written++;
                current_clip_summary_.bytes_written += packet.bytes.size();
            }
            packets_written_++;
            bytes_written_ += packet.bytes.size();
        }
        if (rolling_enabled_ && current_clip_index_ >= 0) {
            current_clip_summary_.gops_released++;
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
    std::unique_ptr<FFmpegWriter> clip_writer_;
    std::unique_ptr<std::ofstream> clip_metadata_;
    std::vector<RollingClipOutputSummary> clip_summaries_;
    RollingClipOutputSummary current_clip_summary_;
    int current_clip_index_ = -1;
    uint64_t current_clip_pts_counter_ = 0;
    bool rolling_enabled_ = false;
    uint64_t clip_span_frames_ = 0;
    uint64_t clip_span_gops_ = 0;
    uint64_t target_frame_count_ = 0;
    uint64_t terminal_tail_coalesce_frames_ = 0;
    uint64_t terminal_tail_coalesced_frames_ = 0;
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

struct DirectSourceWorkItem {
    uint64_t source_frame_index = 0;
    FrameDescriptor desc;
    void* source_ptr = nullptr;
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

    void set_protocol_writer(int fd, std::mutex* mutex)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_fd_ = fd;
        protocol_write_mutex_ = mutex;
    }

    bool uses_deferred_source_release() const
    {
        return options_.direct_input_source && options_.deferred_source_release;
    }

    void notify_deferred_ack_sent()
    {
        cv_.notify_one();
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
        if (options_.direct_input_source) {
            if (options_.deferred_source_release) {
                return enqueue_direct_source(desc, imported_ptr, source_frame_index, sample);
            }
            return submit_direct_input(desc, imported_ptr, source_frame_index, sample);
        }

        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(external detach shard)");
        ensure_copy_stream();
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

        DeviceSlot& slot = slots_[slot_index];
        check_cuda(
            cudaMemcpyAsync(
                slot.ptr,
                imported_ptr,
                desc.bytes,
                cudaMemcpyDeviceToDevice,
                copy_stream_),
            "cudaMemcpyAsync(external encode detach slot)");
        check_cuda(
            cudaEventRecord(slot.detach_ready_event, copy_stream_),
            "cudaEventRecord(external detach slot ready)");
        slot.detach_ready_recorded = true;
        check_cuda(
            cudaEventSynchronize(slot.detach_ready_event),
            "cudaEventSynchronize(external detach slot ready)");

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
        out.source_releases_sent =
            source_releases_sent_.load(std::memory_order_relaxed);
        out.source_release_failures =
            source_release_failures_.load(std::memory_order_relaxed);
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
        out.slot_reuse_wait_p95_ms = percentile_ms(slot_reuse_wait_samples_, 95.0);
        out.encode_total_p95_ms = percentile_ms(encode_total_samples_, 95.0);
        out.encode_picture_p95_ms = percentile_ms(encode_picture_samples_, 95.0);
        out.lock_bitstream_p95_ms = percentile_ms(lock_bitstream_samples_, 95.0);
        out.bitstream_fetch_p95_ms = percentile_ms(bitstream_fetch_samples_, 95.0);
        out.mp4_push_mean_ms = writer_latency_.push_packet_total.mean_ms();
        out.mp4_push_max_ms = writer_latency_.push_packet_total.max_ms();
        out.mp4_write_mean_ms = writer_latency_.packet_write.mean_ms();
        out.mp4_write_max_ms = writer_latency_.packet_write.max_ms();
        out.prewarm_slots = prewarmed_slots_;
        out.prewarm_ms = prewarm_ms_;
        out.prewarm_peer_copy = prewarm_peer_copy_;
        out.encode_csv_path = options_.encode_csv_path;
        out.bitstream_out_path = options_.bitstream_out_path;
        out.mp4_path = options_.mp4_out_path;
        out.mp4_keyframe_path = resolved_mp4_keyframe_path_;
        return out;
    }

    void prewarm_detach_slots(uint64_t bytes, const void* peer_source_ptr)
    {
        if (options_.direct_input_source) {
            return;
        }
        if (options_.encode_prewarm_slots == 0) {
            return;
        }
        if (prewarmed_) {
            prewarm_peer_source(peer_source_ptr);
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(external prewarm shard)");
        const uint32_t requested_slots =
            std::min(options_.encode_prewarm_slots, options_.encode_queue_depth);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_slot_pool_locked();
            for (uint32_t i = 0; i < requested_slots; ++i) {
                const size_t free_index = free_slots_.size() - 1 - i;
                const size_t slot_index = free_slots_[free_index];
                ensure_slot_allocated_locked(slot_index, bytes);
                if (i == 0) {
                    peer_warm_slot_index_ = slot_index;
                }
            }
        }
        prewarmed_slots_ = requested_slots;
        prewarm_ms_ = ns_to_ms(elapsed_ns(started));
        prewarmed_ = true;
        prewarm_peer_source(peer_source_ptr);
        std::cout << "external_recorder_ipc_probe prewarmed"
                  << " gpu_id=" << options_.gpu_id
                  << " shard_id=" << options_.shard_id
                  << " slots=" << prewarmed_slots_
                  << " bytes=" << bytes
                  << " peer_copy=" << (prewarm_peer_copy_ ? "true" : "false")
                  << " prewarm_ms=" << prewarm_ms_
                  << std::endl;
    }

private:
    static constexpr size_t kInvalidSlot = std::numeric_limits<size_t>::max();

    void ensure_copy_stream()
    {
        if (copy_stream_) {
            return;
        }
        check_cuda(
            cudaStreamCreateWithFlags(&copy_stream_, cudaStreamNonBlocking),
            "cudaStreamCreateWithFlags(external detach copy)");
    }

    void prewarm_peer_source(const void* peer_source_ptr)
    {
        if (!options_.encode_prewarm_peer_copy ||
            !peer_source_ptr ||
            prewarm_peer_copy_ ||
            peer_warm_slot_index_ == kInvalidSlot) {
            return;
        }
        void* peer_warm_slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peer_warm_slot = slots_[peer_warm_slot_index_].ptr;
        }
        if (!peer_warm_slot) {
            return;
        }
        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(external prewarm peer)");
        check_cuda(
            cudaMemcpy(
                peer_warm_slot,
                peer_source_ptr,
                1,
                cudaMemcpyDeviceToDevice),
            "cudaMemcpy(external prewarm peer)");
        prewarm_peer_copy_ = true;
    }

    void ensure_slot_pool_locked()
    {
        if (!slots_.empty()) {
            return;
        }
        slots_.resize(options_.encode_queue_depth);
        free_slots_.clear();
        free_slots_.reserve(slots_.size());
        for (size_t i = 0; i < slots_.size(); ++i) {
            free_slots_.push_back(i);
        }
    }

    void ensure_slot_allocated_locked(size_t slot_index, uint64_t bytes)
    {
        DeviceSlot& slot = slots_[slot_index];
        if (!slot.detach_ready_event) {
            check_cuda(
                cudaEventCreateWithFlags(&slot.detach_ready_event, cudaEventDisableTiming),
                "cudaEventCreateWithFlags(external detach ready)");
        }
        if (!slot.slot_reusable_event) {
            check_cuda(
                cudaEventCreateWithFlags(&slot.slot_reusable_event, cudaEventDisableTiming),
                "cudaEventCreateWithFlags(external slot reusable)");
        }
        if (!slot.allocated || slot.bytes < bytes) {
            if (slot.ptr) {
                cudaFree(slot.ptr);
                slot.ptr = nullptr;
            }
            check_cuda(cudaMalloc(&slot.ptr, bytes), "cudaMalloc(external encode slot)");
            slot.bytes = bytes;
            slot.allocated = true;
        }
    }

    size_t acquire_free_slot(uint64_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_slot_pool_locked();
        if (free_slots_.empty()) {
            return kInvalidSlot;
        }
        const size_t slot_index = free_slots_.back();
        free_slots_.pop_back();
        ensure_slot_allocated_locked(slot_index, bytes);
        return slot_index;
    }

    void release_slot(size_t slot_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DeviceSlot& slot = slots_[slot_index];
        slot.detach_ready_recorded = false;
        slot.slot_reusable_recorded = false;
        free_slots_.push_back(slot_index);
    }

    uint64_t queue_depth() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (options_.direct_input_source && options_.deferred_source_release) {
            return direct_source_queue_.size();
        }
        return queue_.size();
    }

    bool stopping_requested() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    void release_slots()
    {
        if (!slots_.empty() || copy_stream_ || direct_input_ready_event_) {
            cudaSetDevice(options_.gpu_id);
        }
        if (copy_stream_) {
            cudaStreamDestroy(copy_stream_);
            copy_stream_ = nullptr;
        }
        if (direct_input_ready_event_) {
            cudaEventDestroy(direct_input_ready_event_);
            direct_input_ready_event_ = nullptr;
        }
        for (auto& slot : slots_) {
            if (slot.ptr) {
                cudaFree(slot.ptr);
                slot.ptr = nullptr;
            }
            if (slot.detach_ready_event) {
                cudaEventDestroy(slot.detach_ready_event);
                slot.detach_ready_event = nullptr;
            }
            if (slot.slot_reusable_event) {
                cudaEventDestroy(slot.slot_reusable_event);
                slot.slot_reusable_event = nullptr;
            }
            slot.bytes = 0;
            slot.allocated = false;
            slot.detach_ready_recorded = false;
            slot.slot_reusable_recorded = false;
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
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                bitstream_out_.open(
                    options_.bitstream_out_path,
                    std::ios::binary | std::ios::trunc);
            }
            if (!bitstream_out_) {
                throw std::runtime_error("Failed to open bitstream output: " +
                                         options_.bitstream_out_path);
            }
        }
        if (!options_.mp4_out_path.empty()) {
            ensure_parent_directory(options_.mp4_out_path);
            resolved_mp4_keyframe_path_ = options_.mp4_keyframe_path.empty()
                ? derive_keyframe_path(options_.mp4_out_path)
                : normalize_keyframe_sidecar_path(options_.mp4_keyframe_path);
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
            if (!mp4_writer_->is_open()) {
                throw std::runtime_error(
                    "Failed to open MP4 output: " + options_.mp4_out_path);
            }
            mp4_writer_->create_thread();
        }
        if (!options_.encode_csv_path.empty()) {
            ensure_parent_directory(options_.encode_csv_path);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                encode_csv_.open(options_.encode_csv_path, std::ios::out | std::ios::trunc);
            }
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
        const auto* source = static_cast<const uint8_t*>(slots_[item.slot_index].ptr);
        prepare_input_frame_from_source(item.desc, source, frame);
    }

    void prepare_input_frame_from_source(const FrameDescriptor& desc,
                                         const void* source_ptr,
                                         const NvEncInputFrame& frame)
    {
        auto* base = static_cast<uint8_t*>(frame.inputPtr);
        const auto* source = static_cast<const uint8_t*>(source_ptr);
        if (!source) {
            throw std::runtime_error("External recorder source pointer is null");
        }
        const int width = desc.width;
        const int height = desc.height;

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

    void wait_for_slot_reusable(size_t slot_index)
    {
        DeviceSlot& slot = slots_[slot_index];
        if (!slot.slot_reusable_recorded || !slot.slot_reusable_event) {
            return;
        }
        check_cuda(
            cudaEventSynchronize(slot.slot_reusable_event),
            "cudaEventSynchronize(external slot reusable)");
    }

    void ensure_direct_input_ready_event()
    {
        if (direct_input_ready_event_) {
            return;
        }
        check_cuda(
            cudaEventCreateWithFlags(&direct_input_ready_event_, cudaEventDisableTiming),
            "cudaEventCreateWithFlags(external direct input ready)");
    }

    bool send_source_release(const FrameDescriptor& desc)
    {
        if (!options_.deferred_source_release) {
            return true;
        }
        int fd = -1;
        std::mutex* write_mutex = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd = protocol_fd_;
            write_mutex = protocol_write_mutex_;
        }
        if (fd < 0) {
            source_release_failures_.fetch_add(1, std::memory_order_relaxed);
            failed_.store(true, std::memory_order_release);
            return false;
        }
        const std::string line =
            "RELEASE " + std::to_string(desc.recording_frame_id) + " " +
            std::to_string(desc.assigned_gpu_id) + " " +
            std::to_string(desc.assigned_shard_id) + "\n";
        if (!write_protocol_line(fd, write_mutex, line)) {
            source_release_failures_.fetch_add(1, std::memory_order_relaxed);
            failed_.store(true, std::memory_order_release);
            return false;
        }
        source_releases_sent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool enqueue_direct_source(const FrameDescriptor& desc,
                               void* imported_ptr,
                               uint64_t source_frame_index,
                               Sample* sample)
    {
        if (!imported_ptr) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            if (sample) {
                sample->encode_dropped = true;
                sample->detach_copied = false;
                sample->encode_queue_depth = queue_depth();
            }
            return false;
        }
        if (stopping_requested() || failed()) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            if (sample) {
                sample->encode_dropped = true;
                sample->detach_copied = false;
                sample->encode_queue_depth = queue_depth();
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (direct_source_queue_.size() >= options_.encode_queue_depth) {
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                if (sample) {
                    sample->encode_dropped = true;
                    sample->detach_copied = false;
                    sample->encode_queue_depth = direct_source_queue_.size();
                }
                return false;
            }
            direct_source_queue_.push_back(DirectSourceWorkItem{
                source_frame_index,
                desc,
                imported_ptr,
                std::chrono::steady_clock::now()});
            if (sample) {
                sample->encode_enqueued = true;
                sample->encode_queue_depth = direct_source_queue_.size();
            }
        }
        return true;
    }

    bool submit_direct_input(const FrameDescriptor& desc,
                             void* imported_ptr,
                             uint64_t source_frame_index,
                             Sample* sample)
    {
        check_cuda(cudaSetDevice(options_.gpu_id), "cudaSetDevice(external direct input shard)");
        initialize_encoder(desc);
        ensure_direct_input_ready_event();

        EncodeSample encode_sample;
        encode_sample.encode_index = frames_encoded_.load(std::memory_order_relaxed);
        encode_sample.source_frame_index = source_frame_index;
        encode_sample.camera_serial = desc.camera_serial;
        encode_sample.session_id = desc.session_id;
        encode_sample.stream_id = desc.stream_id;
        encode_sample.recording_frame_id = desc.recording_frame_id;
        encode_sample.local_frame_id = desc.local_frame_id;
        encode_sample.gop_index = desc.gop_index;
        encode_sample.frame_index_within_gop = desc.frame_index_within_gop;
        encode_sample.source_gpu_id = desc.source_gpu_id;
        encode_sample.assigned_gpu_id = desc.assigned_gpu_id;
        encode_sample.assigned_shard_id = desc.assigned_shard_id;
        encode_sample.routing_policy = desc.routing_policy;
        encode_sample.bytes = desc.bytes;

        const auto prepare_start = std::chrono::steady_clock::now();
        while (!encoder_->WaitForNextInputFrameAvailable(100)) {
            if (stopping_requested() || failed()) {
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                if (sample) {
                    sample->encode_dropped = true;
                    sample->detach_copied = false;
                    sample->encode_queue_depth = queue_depth();
                }
                return false;
            }
        }

        const NvEncInputFrame* input_frame = encoder_->GetNextInputFrame();
        if (!input_frame || !input_frame->inputPtr) {
            throw std::runtime_error("NvEncoder returned no direct input frame");
        }
        prepare_input_frame_from_source(desc, imported_ptr, *input_frame);
        check_cuda(
            cudaEventRecord(direct_input_ready_event_, stream_),
            "cudaEventRecord(external direct input ready)");
        check_cuda(
            cudaEventSynchronize(direct_input_ready_event_),
            "cudaEventSynchronize(external direct input ready)");
        encode_sample.prepare_ms = ns_to_ms(elapsed_ns(prepare_start));

        if (merged_output_) {
            merged_output_->note_submitted(desc);
        }

        NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
        pic_params.frameIdx = static_cast<uint32_t>(encode_sample.encode_index & 0xffffffffu);
        pic_params.inputTimeStamp = desc.recording_frame_id;
        pic_params.inputDuration = 1;

        NvEncoderEncodeFrameTiming timing;
        const auto submit_start = std::chrono::steady_clock::now();
        encoder_->SubmitFrameOnly(&pic_params, &timing);
        encode_sample.encode_total_ms = ns_to_ms(elapsed_ns(submit_start));
        encode_sample.encode_picture_ms = ns_to_ms(timing.encode_picture_ns);
        encode_sample.completion_wait_ms = ns_to_ms(timing.completion_wait_ns);
        encode_sample.lock_bitstream_ms = ns_to_ms(timing.lock_bitstream_ns);
        encode_sample.bitstream_copy_ms = ns_to_ms(timing.bitstream_copy_ns);
        encode_sample.unlock_bitstream_ms = ns_to_ms(timing.unlock_bitstream_ns);
        encode_sample.unmap_input_resource_ms = ns_to_ms(timing.unmap_input_resource_ns);
        encode_sample.bitstream_fetch_ms = ns_to_ms(timing.bitstream_fetch_ns);
        encode_sample.output_packets = timing.output_packets;
        encode_sample.output_bytes = timing.output_bytes;

        if (encode_csv_) {
            write_encode_csv_row(encode_csv_, encode_sample);
            if ((encode_sample.encode_index % 60) == 0) {
                encode_csv_.flush();
            }
        }
        record_encode_sample(encode_sample);
        frames_encoded_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_desc_ = desc;
            has_last_desc_ = true;
            direct_harvest_wake_ = true;
            if (sample) {
                sample->encode_enqueued = true;
                sample->encode_queue_depth = queue_.size();
            }
        }
        cv_.notify_one();
        return true;
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
        slot_reuse_wait_samples_.push_back(sample.slot_reuse_wait_ms);
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

    void encode_one_direct_source(const DirectSourceWorkItem& item)
    {
        initialize_encoder(item.desc);
        last_desc_ = item.desc;
        has_last_desc_ = true;
        ensure_direct_input_ready_event();

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
            throw std::runtime_error("NvEncoder returned no direct source input frame");
        }
        prepare_input_frame_from_source(item.desc, item.source_ptr, *input_frame);
        check_cuda(
            cudaEventRecord(direct_input_ready_event_, stream_),
            "cudaEventRecord(external direct source copied)");
        check_cuda(
            cudaEventSynchronize(direct_input_ready_event_),
            "cudaEventSynchronize(external direct source copied)");
        sample.prepare_ms = ns_to_ms(elapsed_ns(prepare_start));

        if (!send_source_release(item.desc)) {
            throw std::runtime_error("failed to send external source RELEASE");
        }
        if (merged_output_) {
            merged_output_->note_submitted(item.desc);
        }

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

    bool harvest_direct_packets(bool output_delay)
    {
        if (!encoder_) {
            return false;
        }
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint64_t> output_timestamps;
        NvEncoderEncodeFrameTiming timing;
        uint64_t fetch_ns = 0;
        encoder_->HarvestEncodedPackets(
            packets,
            output_delay,
            nullptr,
            &output_timestamps,
            &fetch_ns,
            &timing);
        if (packets.empty()) {
            return false;
        }

        FrameDescriptor fallback_desc;
        bool has_fallback_desc = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fallback_desc = last_desc_;
            has_fallback_desc = has_last_desc_;
        }
        if (merged_output_ && has_fallback_desc) {
            merged_output_->submit_packets(packets, output_timestamps, fallback_desc);
        }
        for (const auto& packet : packets) {
            returned_bytes_ += packet.size();
            push_packet_to_outputs(packet, false);
        }
        returned_packets_ += packets.size();
        lock_bitstream_samples_.push_back(ns_to_ms(timing.lock_bitstream_ns));
        bitstream_fetch_samples_.push_back(
            ns_to_ms(fetch_ns > 0 ? fetch_ns : timing.bitstream_fetch_ns));
        return true;
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
        DeviceSlot& slot = slots_[item.slot_index];
        if (slot.detach_ready_recorded && slot.detach_ready_event) {
            check_cuda(
                cudaStreamWaitEvent(stream_, slot.detach_ready_event, 0),
                "cudaStreamWaitEvent(external detach ready)");
        }
        prepare_input_frame(item, *input_frame);
        if (slot.slot_reusable_event) {
            check_cuda(
                cudaEventRecord(slot.slot_reusable_event, stream_),
                "cudaEventRecord(external slot reusable)");
            slot.slot_reusable_recorded = true;
        }
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
        const auto slot_wait_start = std::chrono::steady_clock::now();
        wait_for_slot_reusable(item.slot_index);
        sample.slot_reuse_wait_ms = ns_to_ms(elapsed_ns(slot_wait_start));
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

    void run_direct_source()
    {
        try {
            while (true) {
                DirectSourceWorkItem item;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&]() {
                        return stopping_ || !direct_source_queue_.empty();
                    });
                    if (direct_source_queue_.empty()) {
                        if (stopping_) {
                            break;
                        }
                        continue;
                    }
                    item = direct_source_queue_.front();
                    direct_source_queue_.pop_front();
                }
                try {
                    encode_one_direct_source(item);
                } catch (...) {
                    (void)send_source_release(item.desc);
                    throw;
                }
            }
            flush_encoder();
            std::cout << "external_recorder_ipc_probe direct-source encoder complete"
                      << " encoded=" << frames_encoded()
                      << " dropped=" << frames_dropped()
                      << std::endl;
        } catch (const std::exception& ex) {
            failed_.store(true, std::memory_order_release);
            std::cerr << "external_recorder_ipc_probe direct-source encoder failed: "
                      << ex.what() << std::endl;
        }
    }

    void run_direct_harvest()
    {
        try {
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&]() {
                        return stopping_ || direct_harvest_wake_;
                    });
                    const bool stop_requested = stopping_;
                    direct_harvest_wake_ = false;
                    if (stop_requested) {
                        break;
                    }
                }
                while (harvest_direct_packets(true)) {
                }
            }
            while (harvest_direct_packets(true)) {
            }
            flush_encoder();
            std::cout << "external_recorder_ipc_probe direct-input encoder complete"
                      << " encoded=" << frames_encoded()
                      << " dropped=" << frames_dropped()
                      << std::endl;
        } catch (const std::exception& ex) {
            failed_.store(true, std::memory_order_release);
            std::cerr << "external_recorder_ipc_probe direct-input encoder failed: "
                      << ex.what() << std::endl;
        }
    }

    void run()
    {
        if (options_.direct_input_source && options_.deferred_source_release) {
            run_direct_source();
            return;
        }
        if (options_.direct_input_source) {
            run_direct_harvest();
            return;
        }
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
    std::deque<DirectSourceWorkItem> direct_source_queue_;
    std::vector<DeviceSlot> slots_;
    std::vector<size_t> free_slots_;
    bool have_next_encode_time_ = false;
    std::chrono::steady_clock::time_point next_encode_time_;
    CUcontext cu_context_ = nullptr;
    cudaStream_t stream_ = nullptr;
    cudaStream_t copy_stream_ = nullptr;
    cudaEvent_t direct_input_ready_event_ = nullptr;
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
    std::vector<double> slot_reuse_wait_samples_;
    std::vector<double> encode_total_samples_;
    std::vector<double> encode_picture_samples_;
    std::vector<double> lock_bitstream_samples_;
    std::vector<double> bitstream_fetch_samples_;
    bool prewarmed_ = false;
    uint64_t prewarmed_slots_ = 0;
    double prewarm_ms_ = 0.0;
    bool prewarm_peer_copy_ = false;
    size_t peer_warm_slot_index_ = kInvalidSlot;
    bool direct_harvest_wake_ = false;
    int protocol_fd_ = -1;
    std::mutex* protocol_write_mutex_ = nullptr;
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> source_releases_sent_{0};
    std::atomic<uint64_t> source_release_failures_{0};
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
                        uint64_t encode_queue_high_water,
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
    std::ofstream out;
    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        out.open(options.summary_json_path, std::ios::out | std::ios::trunc);
    }
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
    out << "  \"schema_id\": \"orange.external_recorder.summary\",\n";
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
    out << "  \"direct_input_source\": "
        << (options.direct_input_source ? "true" : "false") << ",\n";
    out << "  \"deferred_source_release\": "
        << (options.deferred_source_release ? "true" : "false") << ",\n";
    out << "  \"codec\": \"" << json_escape(options.codec) << "\",\n";
    out << "  \"preset\": \"" << json_escape(options.preset) << "\",\n";
    out << "  \"tuning\": \"" << json_escape(options.tuning) << "\",\n";
    out << "  \"fps\": " << options.fps << ",\n";
    out << "  \"encode_max_fps\": " << options.encode_max_fps << ",\n";
    out << "  \"encode_queue_depth\": " << options.encode_queue_depth << ",\n";
    out << "  \"encode_prewarm_slots\": " << options.encode_prewarm_slots << ",\n";
    out << "  \"encode_prewarm_bytes\": " << options.encode_prewarm_bytes << ",\n";
    out << "  \"encode_prewarm_peer_copy\": "
        << (options.encode_prewarm_peer_copy ? "true" : "false") << ",\n";
    out << "  \"recording_control\": {\n";
    out << "    \"record_for_seconds\": " << options.record_for_seconds << ",\n";
    out << "    \"clip_seconds\": " << options.clip_seconds << "\n";
    out << "  },\n";
    out << "  \"rollover\": {\n";
    out << "    \"requested\": " << (options.clip_seconds > 0 ? "true" : "false") << ",\n";
    out << "    \"status\": \""
        << (options.clip_seconds > 0 ? "completed" : "not_requested") << "\",\n";
    out << "    \"implementation\": \""
        << (options.clip_seconds > 0
                ? "external_recorder_gop_boundary_writer_rotation"
                : "none") << "\",\n";
    out << "    \"seamless_writer_switch\": "
        << (options.clip_seconds > 0 ? "true" : "false") << ",\n";
    out << "    \"records_during_rollover\": "
        << (options.clip_seconds > 0 ? "true" : "false") << ",\n";
    out << "    \"boundary\": \"gop_first_frame_id\"\n";
    out << "  },\n";
    out << "  \"frames_received\": " << frames_received << ",\n";
    out << "  \"acks_sent\": " << acks_sent << ",\n";
    out << "  \"detach_copied\": " << detach_copied << ",\n";
    out << "  \"opened_handles\": " << opened_handles << ",\n";
    out << "  \"encode_enqueued\": " << encode_enqueued << ",\n";
    out << "  \"encode_skipped\": " << encode_skipped << ",\n";
    out << "  \"encode_dropped\": " << encode_dropped << ",\n";
    out << "  \"encode_queue_high_water\": " << encode_queue_high_water << ",\n";
    out << "  \"frames_encoded\": " << enc.frames_encoded << ",\n";
    out << "  \"worker_failed\": " << ((enc.failed || merged.failed) ? "true" : "false") << ",\n";
    out << "  \"external_encode\": {\n";
    out << "    \"frames_dropped\": " << enc.frames_dropped << ",\n";
    out << "    \"source_releases_sent\": " << enc.source_releases_sent << ",\n";
    out << "    \"source_release_failures\": " << enc.source_release_failures << ",\n";
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
    out << "    \"slot_reuse_wait_p95_ms\": " << enc.slot_reuse_wait_p95_ms << ",\n";
    out << "    \"encode_total_p95_ms\": " << enc.encode_total_p95_ms << ",\n";
    out << "    \"encode_picture_p95_ms\": " << enc.encode_picture_p95_ms << ",\n";
    out << "    \"lock_bitstream_p95_ms\": " << enc.lock_bitstream_p95_ms << ",\n";
    out << "    \"bitstream_fetch_p95_ms\": " << enc.bitstream_fetch_p95_ms << ",\n";
    out << "    \"mp4_push_mean_ms\": " << enc.mp4_push_mean_ms << ",\n";
    out << "    \"mp4_push_max_ms\": " << enc.mp4_push_max_ms << ",\n";
    out << "    \"mp4_write_mean_ms\": " << enc.mp4_write_mean_ms << ",\n";
    out << "    \"mp4_write_max_ms\": " << enc.mp4_write_max_ms << ",\n";
    out << "    \"prewarm_slots\": " << enc.prewarm_slots << ",\n";
    out << "    \"prewarm_ms\": " << enc.prewarm_ms << ",\n";
    out << "    \"prewarm_peer_copy\": " << (enc.prewarm_peer_copy ? "true" : "false") << ",\n";
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
        out << "      \"source_releases_sent\": " << shard.source_releases_sent << ",\n";
        out << "      \"source_release_failures\": " << shard.source_release_failures << ",\n";
        out << "      \"returned_packets\": " << shard.returned_packets << ",\n";
        out << "      \"returned_bytes\": " << shard.returned_bytes << ",\n";
        out << "      \"mp4_packets\": " << shard.mp4_packets << ",\n";
        out << "      \"mp4_bytes\": " << shard.mp4_bytes << ",\n";
        out << "      \"slot_reuse_wait_p95_ms\": " << shard.slot_reuse_wait_p95_ms << ",\n";
        out << "      \"encode_total_p95_ms\": " << shard.encode_total_p95_ms << ",\n";
        out << "      \"encode_picture_p95_ms\": " << shard.encode_picture_p95_ms << ",\n";
        out << "      \"lock_bitstream_p95_ms\": " << shard.lock_bitstream_p95_ms << ",\n";
        out << "      \"prewarm_slots\": " << shard.prewarm_slots << ",\n";
        out << "      \"prewarm_ms\": " << shard.prewarm_ms << ",\n";
        out << "      \"prewarm_peer_copy\": "
            << (shard.prewarm_peer_copy ? "true" : "false") << ",\n";
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
    out << "  \"rolling_output\": {\n";
    out << "    \"enabled\": " << (merged.rolling.enabled ? "true" : "false") << ",\n";
    out << "    \"implementation\": \"" << json_escape(merged.rolling.implementation) << "\",\n";
    out << "    \"record_for_seconds\": " << merged.rolling.record_for_seconds << ",\n";
    out << "    \"clip_seconds\": " << merged.rolling.clip_seconds << ",\n";
    out << "    \"clip_span_frames\": " << merged.rolling.clip_span_frames << ",\n";
    out << "    \"clip_span_gops\": " << merged.rolling.clip_span_gops << ",\n";
    out << "    \"target_frame_count\": " << merged.rolling.target_frame_count << ",\n";
    out << "    \"terminal_tail_coalesce_frames\": "
        << merged.rolling.terminal_tail_coalesce_frames << ",\n";
    out << "    \"terminal_tail_coalesced_frames\": "
        << merged.rolling.terminal_tail_coalesced_frames << ",\n";
    out << "    \"clip_count\": " << merged.rolling.clips.size() << ",\n";
    out << "    \"clips\": [\n";
    for (size_t i = 0; i < merged.rolling.clips.size(); ++i) {
        const RollingClipOutputSummary& clip = merged.rolling.clips[i];
        out << "      {\n";
        out << "        \"clip_index\": " << clip.clip_index << ",\n";
        out << "        \"clip_id\": \"" << json_escape(clip.clip_id) << "\",\n";
        out << "        \"directory\": \"" << json_escape(clip.directory) << "\",\n";
        out << "        \"mp4\": \"" << json_escape(clip.mp4_path) << "\",\n";
        out << "        \"metadata\": \"" << json_escape(clip.metadata_path) << "\",\n";
        out << "        \"keyframes\": \"" << json_escape(clip.keyframe_path) << "\",\n";
        out << "        \"first_recording_frame_id\": "
            << clip.first_recording_frame_id << ",\n";
        out << "        \"last_recording_frame_id\": "
            << clip.last_recording_frame_id << ",\n";
        out << "        \"frame_count\": " << clip.frame_count << ",\n";
        out << "        \"packets_written\": " << clip.packets_written << ",\n";
        out << "        \"bytes_written\": " << clip.bytes_written << ",\n";
        out << "        \"gops_released\": " << clip.gops_released << ",\n";
        out << "        \"failed\": " << (clip.failed ? "true" : "false") << ",\n";
        out << "        \"file_sizes\": {\n";
        out << "          \"mp4_bytes\": " << file_size_or_zero(clip.mp4_path) << ",\n";
        out << "          \"metadata_bytes\": " << file_size_or_zero(clip.metadata_path) << ",\n";
        out << "          \"keyframe_bytes\": " << file_size_or_zero(clip.keyframe_path) << "\n";
        out << "        }\n";
        out << "      }" << (i + 1 < merged.rolling.clips.size() ? "," : "") << "\n";
    }
    out << "    ]\n";
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
    Options options;
    uint64_t status_heartbeat_sequence = 0;
    try {
        options = parse_options(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        check_cuda(cudaSetDevice(options.gpu_id), "cudaSetDevice");
        check_cuda(cudaFree(nullptr), "cudaFree(0)");

        std::vector<std::unique_ptr<ExternalEncodeWorker>> encode_workers;
        std::unique_ptr<MergedGopOutput> merged_output;
        std::mutex protocol_write_mutex;
        bool encode_workers_prewarmed = false;
        bool encode_workers_peer_prewarmed = false;
        if (options.encode) {
            const std::vector<int> shard_gpu_ids = effective_shard_gpu_ids(options);
            const bool rolling_requested = options.clip_seconds > 0;
            if ((shard_gpu_ids.size() > 1 || rolling_requested) &&
                !options.mp4_out_path.empty()) {
                merged_output = std::make_unique<MergedGopOutput>(options);
            }
            encode_workers.reserve(shard_gpu_ids.size());
            for (size_t shard_index = 0; shard_index < shard_gpu_ids.size(); ++shard_index) {
                Options shard_options = make_shard_options(
                    options,
                    shard_index,
                    shard_gpu_ids.size(),
                    shard_gpu_ids[shard_index]);
                if (merged_output && shard_gpu_ids.size() == 1) {
                    const std::string suffix =
                        shard_suffix(shard_index, shard_gpu_ids[shard_index]);
                    shard_options.mp4_out_path =
                        add_suffix_to_path_stem(options.mp4_out_path, suffix);
                    shard_options.mp4_keyframe_path =
                        add_suffix_to_path_stem(options.mp4_keyframe_path, suffix);
                }
                auto worker = std::make_unique<ExternalEncodeWorker>(
                    std::move(shard_options),
                    merged_output.get());
                worker->start();
                if (options.encode_prewarm_bytes > 0 &&
                    options.encode_prewarm_slots > 0) {
                    worker->prewarm_detach_slots(options.encode_prewarm_bytes, nullptr);
                    encode_workers_prewarmed = true;
                }
                encode_workers.push_back(std::move(worker));
            }
        }

        listen_fd = create_listen_socket(options.socket_path);
        std::cout << "external_recorder_ipc_probe listening"
                  << " socket=" << options.socket_path
                  << " gpu_id=" << options.gpu_id << std::endl;
        (void)write_recorder_status_json(
            options,
            options.session_id,
            options.stream_id,
            "listening",
            ++status_heartbeat_sequence,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            false,
            {},
            rolling_status_from_progress(options, 0));

        client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            throw std::runtime_error("accept() failed: " + std::string(std::strerror(errno)));
        }
        std::cout << "external_recorder_ipc_probe connected" << std::endl;
        for (auto& worker : encode_workers) {
            if (worker) {
                worker->set_protocol_writer(client_fd, &protocol_write_mutex);
            }
        }
        (void)write_recorder_status_json(
            options,
            options.session_id,
            options.stream_id,
            "connected",
            ++status_heartbeat_sequence,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            false,
            {},
            rolling_status_from_progress(options, 0));

        std::ofstream csv;
        if (!options.csv_path.empty()) {
            ensure_parent_directory(options.csv_path);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                csv.open(options.csv_path);
            }
            if (!csv) {
                throw std::runtime_error("Failed to open CSV: " + options.csv_path);
            }
            write_csv_header(csv);
        }
        std::ofstream gop_routing_csv;
        if (!options.gop_routing_csv_path.empty()) {
            ensure_parent_directory(options.gop_routing_csv_path);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                gop_routing_csv.open(options.gop_routing_csv_path);
            }
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
        uint64_t encode_queue_high_water = 0;
        std::string observed_session_id = options.session_id;
        std::string observed_stream_id = options.stream_id;
        std::vector<double> detach_total_samples;
        std::vector<double> detach_open_samples;
        std::vector<double> detach_copy_samples;
        auto collect_encode_progress =
            [&encode_workers](uint64_t* frames_encoded,
                              uint64_t* frames_dropped,
                              bool* worker_failed) {
                uint64_t encoded = 0;
                uint64_t dropped = 0;
                bool failed = false;
                for (const auto& worker : encode_workers) {
                    if (!worker) {
                        continue;
                    }
                    encoded += worker->frames_encoded();
                    dropped += worker->frames_dropped();
                    failed = failed || worker->failed();
                }
                if (frames_encoded) {
                    *frames_encoded = encoded;
                }
                if (frames_dropped) {
                    *frames_dropped = dropped;
                }
                if (worker_failed) {
                    *worker_failed = failed;
                }
            };
        auto write_status =
            [&](const std::string& status,
                const std::string& error_message = {}) {
                uint64_t frames_encoded = 0;
                uint64_t frames_dropped = 0;
                bool worker_failed = false;
                collect_encode_progress(&frames_encoded, &frames_dropped, &worker_failed);
                RollingStatusSnapshot rolling_status =
                    rolling_status_from_progress(options, frame_count);
                if (merged_output) {
                    const MergedOutputSummary merged_summary = merged_output->summary();
                    const std::vector<RollingClipOutputSummary>& clips =
                        merged_summary.rolling.clips;
                    rolling_status.completed_clip_count = clips.size();
                    if (!clips.empty()) {
                        const RollingClipOutputSummary& last_clip = clips.back();
                        rolling_status.last_completed_clip_index =
                            last_clip.clip_index;
                        rolling_status.last_completed_clip_last_recording_frame_id =
                            last_clip.last_recording_frame_id;
                        rolling_status.last_completed_clip_frame_count =
                            last_clip.frame_count;
                        rolling_status.last_rollover_status =
                            last_clip.failed ? "failed" : "completed";
                    }
                }
                (void)write_recorder_status_json(
                    options,
                    observed_session_id,
                    observed_stream_id,
                    status,
                    ++status_heartbeat_sequence,
                    frame_count,
                    ack_count,
                    detach_copied_count,
                    encode_enqueued_count,
                    encode_skipped_count,
                    encode_dropped_count,
                    encode_queue_high_water,
                    frames_encoded,
                    frames_dropped,
                    worker_failed,
                    error_message,
                    rolling_status);
            };
        auto last_status_write = std::chrono::steady_clock::now();

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
            bool release_deferred_by_worker = false;

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

                if (!encode_workers.empty() &&
                    options.encode_prewarm_slots > 0 &&
                    (!encode_workers_prewarmed ||
                     (options.encode_prewarm_peer_copy && !encode_workers_peer_prewarmed))) {
                    const void* peer_source_ptr =
                        options.encode_prewarm_peer_copy ? handle_it->second.ptr : nullptr;
                    for (auto& worker : encode_workers) {
                        if (worker) {
                            worker->prewarm_detach_slots(desc.bytes, peer_source_ptr);
                        }
                    }
                    encode_workers_prewarmed = true;
                    encode_workers_peer_prewarmed = peer_source_ptr != nullptr;
                }

                const auto copy_start = std::chrono::steady_clock::now();
                if (target_encode_worker) {
                    const bool worker_accepted = target_encode_worker->detach_and_enqueue(
                        desc,
                        handle_it->second.ptr,
                        frame_count,
                        &sample);
                    release_deferred_by_worker =
                        options.deferred_source_release &&
                        target_encode_worker->uses_deferred_source_release() &&
                        worker_accepted &&
                        sample.encode_enqueued &&
                        !sample.encode_dropped;
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

            if (!write_protocol_line(
                    client_fd,
                    &protocol_write_mutex,
                    "ACK " + std::to_string(desc.recording_frame_id) + " " +
                        std::to_string(desc.assigned_gpu_id) + " " +
                        std::to_string(desc.assigned_shard_id) +
                        (options.deferred_source_release ? " deferred_release" : "") +
                        "\n")) {
                break;
            }
            if (release_deferred_by_worker && target_encode_worker) {
                target_encode_worker->notify_deferred_ack_sent();
            }
            if (options.deferred_source_release && !release_deferred_by_worker) {
                if (!write_protocol_line(
                        client_fd,
                        &protocol_write_mutex,
                        "RELEASE " + std::to_string(desc.recording_frame_id) + " " +
                            std::to_string(desc.assigned_gpu_id) + " " +
                            std::to_string(desc.assigned_shard_id) + "\n")) {
                    break;
                }
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
            encode_queue_high_water =
                std::max<uint64_t>(encode_queue_high_water, sample.encode_queue_depth);
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
            const auto status_now = std::chrono::steady_clock::now();
            if (status_now - last_status_write >= std::chrono::seconds(1)) {
                write_status("running");
                last_status_write = status_now;
            }
        }

        write_status("finalizing");
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
            encode_queue_high_water,
            detach_total_samples,
            detach_open_samples,
            detach_copy_samples,
            encode_workers.empty() ? nullptr : &encode_summary,
            shard_summaries,
            merged_output ? &merged_summary : nullptr);
        bool any_worker_failed = false;
        if (merged_output && merged_summary.failed) {
            any_worker_failed = true;
        }
        if (!encode_workers.empty()) {
            for (const auto& worker : encode_workers) {
                if (!worker) {
                    continue;
                }
                any_worker_failed = any_worker_failed || worker->failed();
            }
        }
        write_status(any_worker_failed ? "failed" : "completed");
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
        (void)write_recorder_status_json(
            options,
            options.session_id,
            options.stream_id,
            "failed",
            ++status_heartbeat_sequence,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            true,
            ex.what(),
            rolling_status_from_progress(options, 0));
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
