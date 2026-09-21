#include "external_recorder_ipc_protocol.h"

#include <cuda_runtime.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string socket_path = "/tmp/orange_external_recorder_ipc_replay.sock";
    std::string session_id = "ipc_replay";
    std::string stream_id = "ipc_replay";
    std::string camera_serial = "ipc_replay_camera";
    std::string raw_file;
    std::string csv_path = "/tmp/orange_ipc_replay_frames.csv";
    std::string reference_prefix;
    int source_gpu_id = 1;
    std::vector<int> route_gpu_ids;
    uint64_t frames = 200;
    uint32_t width = 4512;
    uint32_t height = 4512;
    uint32_t fps = 100;
    uint32_t gop = 25;
    uint32_t pool_size = 32;
    uint32_t source_frames = 4;
    uint64_t raw_pitch = 0;
    uint64_t raw_frame_bytes = 0;
    int pixel_format = 0;
    int timeout_ms = 30000;
};

enum class SlotState { free, preparing, in_flight };

struct PoolSlot {
    unsigned char* ptr = nullptr;
    std::string handle_hex;
    SlotState state = SlotState::free;
    uint64_t current_frame_id = 0;
    uint64_t generation = 0;
};

struct SourceFrame {
    std::vector<unsigned char> host_y;
    unsigned char* device_y = nullptr;
    size_t device_pitch = 0;
};

struct FrameRecord {
    uint64_t frame_id = 0;
    uint64_t local_frame_id = 0;
    size_t slot_index = 0;
    uint64_t slot_generation = 0;
    uint32_t source_index = 0;
    uint64_t raw_offset = 0;
    uint64_t target_ns = 0;
    uint64_t send_ns = 0;
    uint64_t ack_ns = 0;
    uint64_t release_ns = 0;
    int expected_gpu_id = -1;
    int expected_shard_id = -1;
    int ack_gpu_id = -1;
    int ack_shard_id = -1;
    int release_gpu_id = -1;
    int release_shard_id = -1;
    unsigned int ack_count = 0;
    unsigned int release_count = 0;
    bool deferred_ack = false;
};

struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<PoolSlot>* slots = nullptr;
    std::vector<FrameRecord>* records = nullptr;
    std::string expected_session;
    std::string expected_stream;
    std::string error;
    bool eof = false;
    uint64_t status_messages = 0;
    uint64_t last_status_frames_received = 0;
    uint64_t last_status_frames_encoded = 0;
};

std::atomic<bool> g_interrupted{false};

void signal_handler(int)
{
    g_interrupted.store(true, std::memory_order_relaxed);
}

uint64_t steady_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

uint64_t realtime_ns()
{
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_REALTIME) failed: " +
                                 std::string(std::strerror(errno)));
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

void check_cuda(cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 cudaGetErrorString(status));
    }
}

uint64_t parse_u64(const std::string& text, const char* option)
{
    if (text.empty() || text[0] == '-') {
        throw std::runtime_error(std::string(option) + " requires a nonnegative integer");
    }
    size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option) + " requires an integer: " + text);
    }
    if (consumed != text.size()) {
        throw std::runtime_error(std::string(option) + " requires an integer: " + text);
    }
    return static_cast<uint64_t>(value);
}

int parse_int(const std::string& text, const char* option)
{
    size_t consumed = 0;
    long long value = 0;
    try {
        value = std::stoll(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option) + " requires an integer: " + text);
    }
    if (consumed != text.size() || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string(option) + " requires a valid int: " + text);
    }
    return static_cast<int>(value);
}

std::vector<int> parse_gpu_list(const std::string& text)
{
    std::vector<int> values;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const std::string token = text.substr(
            begin,
            comma == std::string::npos ? std::string::npos : comma - begin);
        if (token.empty()) {
            throw std::runtime_error("--route-gpu-ids contains an empty entry");
        }
        const int value = parse_int(token, "--route-gpu-ids");
        if (value < 0) {
            throw std::runtime_error("--route-gpu-ids entries must be nonnegative");
        }
        values.push_back(value);
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    if (values.empty()) {
        throw std::runtime_error("--route-gpu-ids must contain at least one GPU");
    }
    return values;
}

void print_usage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " --raw-file <Mono8 sequence> [options]\n"
        << "  --socket <path>             Recorder Unix socket\n"
        << "  --session-id <token>        Must match recorder --session-id\n"
        << "  --stream-id <token>         Must match recorder --stream-id\n"
        << "  --camera-serial <token>     Descriptor camera identity\n"
        << "  --source-gpu <id>           CUDA 12.2 producer GPU (default 1)\n"
        << "  --route-gpu-ids <csv>       Expected recorder shard GPUs (default source GPU)\n"
        << "  --frames <n>                Frames to replay (default 200)\n"
        << "  --width <pixels>            Visible Mono8 width (default 4512)\n"
        << "  --height <pixels>           Visible Mono8 height (default 4512)\n"
        << "  --fps <n>                   Source and protocol FPS (default 100)\n"
        << "  --gop <n>                   Resolved GOP length (default 25)\n"
        << "  --pool <n>                  Exported tight NV12 slots (default 32)\n"
        << "  --source-frames <n>         Cached raw frames cycled by replay (default 4)\n"
        << "  --raw-pitch <bytes>         Input row stride (default width)\n"
        << "  --raw-frame-bytes <bytes>   Input record stride (default pitch*height)\n"
        << "  --pixel-format <value>      Descriptor metadata value (default 0)\n"
        << "  --csv <path>                Per-frame protocol/reference CSV\n"
        << "  --reference-prefix <path>   Optional tight .y8 files, suffix NNNN.y8\n"
        << "  --timeout-ms <ms>           Connect/progress/finalize timeout (default 30000)\n";
}

Options parse_options(int argc, char** argv)
{
    Options options;
    auto consume = [&](int* index, const char* name) -> std::string {
        if (*index + 1 >= argc) {
            throw std::runtime_error(std::string(name) + " requires a value");
        }
        return argv[++(*index)];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--socket") {
            options.socket_path = consume(&i, arg.c_str());
        } else if (arg == "--session-id") {
            options.session_id = consume(&i, arg.c_str());
        } else if (arg == "--stream-id") {
            options.stream_id = consume(&i, arg.c_str());
        } else if (arg == "--camera-serial") {
            options.camera_serial = consume(&i, arg.c_str());
        } else if (arg == "--raw-file") {
            options.raw_file = consume(&i, arg.c_str());
        } else if (arg == "--csv") {
            options.csv_path = consume(&i, arg.c_str());
        } else if (arg == "--reference-prefix") {
            options.reference_prefix = consume(&i, arg.c_str());
        } else if (arg == "--source-gpu" || arg == "--gpu") {
            options.source_gpu_id = parse_int(consume(&i, arg.c_str()), arg.c_str());
        } else if (arg == "--route-gpu-ids") {
            options.route_gpu_ids = parse_gpu_list(consume(&i, arg.c_str()));
        } else if (arg == "--frames") {
            options.frames = parse_u64(consume(&i, arg.c_str()), arg.c_str());
        } else if (arg == "--width") {
            options.width = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--height") {
            options.height = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--fps") {
            options.fps = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--gop") {
            options.gop = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--pool") {
            options.pool_size = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--source-frames") {
            options.source_frames = static_cast<uint32_t>(parse_u64(consume(&i, arg.c_str()), arg.c_str()));
        } else if (arg == "--raw-pitch") {
            options.raw_pitch = parse_u64(consume(&i, arg.c_str()), arg.c_str());
        } else if (arg == "--raw-frame-bytes") {
            options.raw_frame_bytes = parse_u64(consume(&i, arg.c_str()), arg.c_str());
        } else if (arg == "--pixel-format") {
            options.pixel_format = parse_int(consume(&i, arg.c_str()), arg.c_str());
        } else if (arg == "--timeout-ms") {
            options.timeout_ms = parse_int(consume(&i, arg.c_str()), arg.c_str());
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (options.raw_file.empty()) {
        throw std::runtime_error("--raw-file is required; this gate must replay real cached content");
    }
    if (options.socket_path.empty() || options.session_id.empty() ||
        options.stream_id.empty() || options.camera_serial.empty() ||
        options.csv_path.empty()) {
        throw std::runtime_error("socket, protocol identities, and CSV path must be nonempty");
    }
    if (options.source_gpu_id < 0 || options.frames == 0 || options.width == 0 ||
        options.height == 0 || options.fps == 0 || options.gop == 0 ||
        options.pool_size == 0 || options.source_frames == 0 || options.timeout_ms <= 0) {
        throw std::runtime_error("GPU, dimensions, rates, counts, pool, and timeout must be positive");
    }
    if (options.route_gpu_ids.empty()) {
        options.route_gpu_ids.push_back(options.source_gpu_id);
    }
    if ((options.width & 1U) != 0 || (options.height & 1U) != 0) {
        throw std::runtime_error("NV12 width and height must be even");
    }
    if (options.raw_pitch == 0) {
        options.raw_pitch = options.width;
    }
    if (options.raw_pitch < options.width) {
        throw std::runtime_error("--raw-pitch cannot be smaller than width");
    }
    const uint64_t minimum_frame_bytes = options.raw_pitch * options.height;
    if (options.height != 0 && minimum_frame_bytes / options.height != options.raw_pitch) {
        throw std::runtime_error("raw pitch*height overflow");
    }
    if (options.raw_frame_bytes == 0) {
        options.raw_frame_bytes = minimum_frame_bytes;
    }
    if (options.raw_frame_bytes < minimum_frame_bytes) {
        throw std::runtime_error("--raw-frame-bytes is smaller than raw-pitch*height");
    }
    return options;
}

std::string handle_to_hex(const cudaIpcMemHandle_t& handle)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&handle);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < sizeof(handle); ++i) {
        out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return out.str();
}

std::string csv_quote(const std::string& text)
{
    if (text.find_first_of(",\"\r\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (char c : text) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    out += '"';
    return out;
}

void load_sources(const Options& options, std::vector<SourceFrame>* sources)
{
    if (!sources) {
        throw std::runtime_error("null source destination");
    }
    std::ifstream input(options.raw_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open raw source: " + options.raw_file);
    }
    input.seekg(0, std::ios::end);
    const std::streamoff file_size_signed = input.tellg();
    if (file_size_signed < 0) {
        throw std::runtime_error("cannot determine raw source size: " + options.raw_file);
    }
    const uint64_t file_size = static_cast<uint64_t>(file_size_signed);
    const uint64_t required = options.raw_frame_bytes * options.source_frames;
    if (options.source_frames != 0 && required / options.source_frames != options.raw_frame_bytes) {
        throw std::runtime_error("raw source size computation overflow");
    }
    if (file_size < required) {
        throw std::runtime_error("raw source has " + std::to_string(file_size) +
                                 " bytes; at least " + std::to_string(required) + " required");
    }
    input.seekg(0, std::ios::beg);

    const size_t tight_y_bytes = static_cast<size_t>(options.width) * options.height;
    std::vector<unsigned char> raw(static_cast<size_t>(options.raw_frame_bytes));
    sources->resize(options.source_frames);
    for (uint32_t source_index = 0; source_index < options.source_frames; ++source_index) {
        input.read(reinterpret_cast<char*>(raw.data()),
                   static_cast<std::streamsize>(raw.size()));
        if (!input) {
            throw std::runtime_error("short read for raw source frame " +
                                     std::to_string(source_index));
        }
        SourceFrame& source = (*sources)[source_index];
        source.host_y.resize(tight_y_bytes);
        for (uint32_t row = 0; row < options.height; ++row) {
            std::memcpy(source.host_y.data() + static_cast<size_t>(row) * options.width,
                        raw.data() + static_cast<size_t>(row) * options.raw_pitch,
                        options.width);
        }
        check_cuda(cudaMallocPitch(reinterpret_cast<void**>(&source.device_y),
                                   &source.device_pitch,
                                   options.width,
                                   options.height),
                   "cudaMallocPitch(cached source Y)");
        check_cuda(cudaMemcpy2D(source.device_y,
                                source.device_pitch,
                                source.host_y.data(),
                                options.width,
                                options.width,
                                options.height,
                                cudaMemcpyHostToDevice),
                   "cudaMemcpy2D(cache raw source Y)");

        if (!options.reference_prefix.empty()) {
            std::ostringstream path;
            path << options.reference_prefix << std::setfill('0') << std::setw(4)
                 << source_index << ".y8";
            std::ofstream reference(path.str(), std::ios::binary | std::ios::trunc);
            if (!reference) {
                throw std::runtime_error("cannot open reference output: " + path.str());
            }
            reference.write(reinterpret_cast<const char*>(source.host_y.data()),
                            static_cast<std::streamsize>(source.host_y.size()));
            if (!reference) {
                throw std::runtime_error("failed writing reference output: " + path.str());
            }
        }
    }
}

void allocate_pool(const Options& options, std::vector<PoolSlot>* slots)
{
    const size_t y_bytes = static_cast<size_t>(options.width) * options.height;
    const size_t pool_bytes = y_bytes + y_bytes / 2;
    slots->resize(options.pool_size);
    for (PoolSlot& slot : *slots) {
        check_cuda(cudaMalloc(reinterpret_cast<void**>(&slot.ptr), pool_bytes),
                   "cudaMalloc(tight exported NV12 slot)");
        check_cuda(cudaMemset(slot.ptr + y_bytes, 128, y_bytes / 2),
                   "cudaMemset(neutral UV)");
        cudaIpcMemHandle_t handle{};
        check_cuda(cudaIpcGetMemHandle(&handle, slot.ptr), "cudaIpcGetMemHandle");
        slot.handle_hex = handle_to_hex(handle);
    }
    // Publish no IPC handle to the recorder until every slot's persistent UV
    // plane is initialized. The per-frame copy intentionally updates Y only.
    check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(NV12 pool initialization)");
}

bool send_all(int fd, const std::string& data)
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

int connect_with_deadline(const Options& options)
{
    if (options.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Unix socket path is too long: " + options.socket_path);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
    int last_error = 0;
    do {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("socket(AF_UNIX) failed: " +
                                     std::string(std::strerror(errno)));
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, options.socket_path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        last_error = errno;
        close(fd);
        if (g_interrupted.load(std::memory_order_relaxed)) {
            throw std::runtime_error("interrupted while connecting");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    throw std::runtime_error("connect(" + options.socket_path + ") timed out: " +
                             std::string(std::strerror(last_error)));
}

bool read_line_until(int fd,
                     const Clock::time_point& deadline,
                     std::string* buffer,
                     std::string* line,
                     std::string* error)
{
    while (true) {
        const size_t newline = buffer->find('\n');
        if (newline != std::string::npos) {
            *line = buffer->substr(0, newline);
            buffer->erase(0, newline + 1);
            return true;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            *error = "timed out waiting for protocol line";
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int poll_ms = std::max(1, static_cast<int>(std::min<int64_t>(remaining.count(), 1000)));
        const int result = poll(&pfd, 1, poll_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            *error = "poll failed: " + std::string(std::strerror(errno));
            return false;
        }
        if (result == 0) {
            continue;
        }
        char chunk[4096];
        const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            *error = received == 0 ? "peer closed before protocol line" :
                                     "recv failed: " + std::string(std::strerror(errno));
            return false;
        }
        buffer->append(chunk, static_cast<size_t>(received));
        if (buffer->size() > 65536) {
            *error = "protocol receive buffer exceeded 64 KiB";
            return false;
        }
    }
}

void set_protocol_error(SharedState* shared, const std::string& error)
{
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->error.empty()) {
        shared->error = error;
    }
    shared->cv.notify_all();
}

bool parse_ack_or_release(const std::string& line,
                          const char* expected_kind,
                          uint64_t* frame_id,
                          int* gpu_id,
                          int* shard_id,
                          bool* deferred)
{
    std::istringstream in(line);
    std::string kind;
    in >> kind >> *frame_id >> *gpu_id >> *shard_id;
    if (!in || kind != expected_kind || *frame_id == 0) {
        return false;
    }
    std::string token;
    if (deferred) {
        *deferred = false;
    }
    while (in >> token) {
        if (deferred && token == "deferred_release") {
            *deferred = true;
        } else {
            return false;
        }
    }
    return true;
}

bool process_protocol_line(const std::string& line, SharedState* shared)
{
    if (line.rfind("ACK ", 0) == 0) {
        uint64_t frame_id = 0;
        int gpu_id = -1;
        int shard_id = -1;
        bool deferred = false;
        if (!parse_ack_or_release(line, "ACK", &frame_id, &gpu_id, &shard_id, &deferred)) {
            set_protocol_error(shared, "malformed ACK: " + line);
            return false;
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (frame_id >= shared->records->size()) {
            shared->error = "ACK references unknown frame " + std::to_string(frame_id);
        } else {
            FrameRecord& record = (*shared->records)[frame_id];
            if (record.send_ns == 0) {
                shared->error = "ACK arrived before frame was marked sent: " +
                                std::to_string(frame_id);
            } else if (record.ack_count != 0) {
                shared->error = "duplicate ACK for frame " + std::to_string(frame_id);
            } else if (!deferred) {
                shared->error = "ACK lacks deferred_release for frame " +
                                std::to_string(frame_id);
            } else if (gpu_id != record.expected_gpu_id ||
                       shard_id != record.expected_shard_id) {
                shared->error = "ACK route mismatch for frame " + std::to_string(frame_id) +
                    ": peer gpu=" + std::to_string(gpu_id) +
                    " shard=" + std::to_string(shard_id) +
                    " expected gpu=" + std::to_string(record.expected_gpu_id) +
                    " shard=" + std::to_string(record.expected_shard_id);
            } else {
                record.ack_count = 1;
                record.deferred_ack = true;
                record.ack_gpu_id = gpu_id;
                record.ack_shard_id = shard_id;
                record.ack_ns = steady_ns();
            }
        }
        shared->cv.notify_all();
        return shared->error.empty();
    }
    if (line.rfind("RELEASE ", 0) == 0) {
        uint64_t frame_id = 0;
        int gpu_id = -1;
        int shard_id = -1;
        if (!parse_ack_or_release(line, "RELEASE", &frame_id, &gpu_id, &shard_id, nullptr)) {
            set_protocol_error(shared, "malformed RELEASE: " + line);
            return false;
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (frame_id >= shared->records->size()) {
            shared->error = "RELEASE references unknown frame " + std::to_string(frame_id);
        } else {
            FrameRecord& record = (*shared->records)[frame_id];
            if (record.ack_count != 1 || record.ack_ns == 0) {
                shared->error = "RELEASE arrived before ACK for frame " +
                                std::to_string(frame_id);
            } else if (record.release_count != 0) {
                shared->error = "duplicate RELEASE for frame " + std::to_string(frame_id);
            } else if (gpu_id != record.expected_gpu_id ||
                       shard_id != record.expected_shard_id) {
                shared->error = "RELEASE route mismatch for frame " + std::to_string(frame_id);
            } else if (record.slot_index >= shared->slots->size()) {
                shared->error = "invalid slot index for frame " + std::to_string(frame_id);
            } else {
                PoolSlot& slot = (*shared->slots)[record.slot_index];
                if (slot.state != SlotState::in_flight ||
                    slot.current_frame_id != frame_id ||
                    slot.generation != record.slot_generation) {
                    shared->error = "slot ownership mismatch at RELEASE for frame " +
                                    std::to_string(frame_id);
                } else {
                    record.release_count = 1;
                    record.release_gpu_id = gpu_id;
                    record.release_shard_id = shard_id;
                    record.release_ns = steady_ns();
                    slot.state = SlotState::free;
                    slot.current_frame_id = 0;
                }
            }
        }
        shared->cv.notify_all();
        return shared->error.empty();
    }
    if (orange::external_recorder::ipc::starts_with_kind(
            line, orange::external_recorder::ipc::kRecorderStatusKind)) {
        orange::external_recorder::ipc::RecorderStatusFields status;
        if (!orange::external_recorder::ipc::parse_recorder_status_line(line, &status)) {
            set_protocol_error(shared, "invalid RECORDER_STATUS: " + status.error);
            return false;
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (status.session_id != shared->expected_session ||
            status.stream_id != shared->expected_stream) {
            shared->error = "RECORDER_STATUS identity mismatch";
        } else if (status.worker_failed) {
            shared->error = "recorder reported worker_failed in status=" + status.status;
        } else {
            ++shared->status_messages;
            shared->last_status_frames_received = status.frames_received;
            shared->last_status_frames_encoded = status.frames_encoded;
        }
        shared->cv.notify_all();
        return shared->error.empty();
    }
    set_protocol_error(shared, "unexpected protocol line: " + line);
    return false;
}

void protocol_reader(int fd, std::string initial_buffer, SharedState* shared)
{
    std::string buffer = std::move(initial_buffer);
    while (true) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && !process_protocol_line(line, shared)) {
                return;
            }
            continue;
        }
        char chunk[4096];
        const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received == 0) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (!buffer.empty() && shared->error.empty()) {
                shared->error = "peer closed with an unterminated protocol line";
            }
            shared->eof = true;
            shared->cv.notify_all();
            return;
        }
        if (received < 0) {
            set_protocol_error(shared, "protocol recv failed: " +
                                       std::string(std::strerror(errno)));
            return;
        }
        buffer.append(chunk, static_cast<size_t>(received));
        if (buffer.size() > 65536) {
            set_protocol_error(shared, "protocol receive buffer exceeded 64 KiB");
            return;
        }
    }
}

size_t reserve_slot(SharedState* shared,
                    uint64_t frame_id,
                    const Clock::time_point& deadline)
{
    std::unique_lock<std::mutex> lock(shared->mutex);
    while (true) {
        if (!shared->error.empty()) {
            throw std::runtime_error(shared->error);
        }
        if (g_interrupted.load(std::memory_order_relaxed)) {
            throw std::runtime_error("interrupted while waiting for a released pool slot");
        }
        for (size_t i = 0; i < shared->slots->size(); ++i) {
            PoolSlot& slot = (*shared->slots)[i];
            if (slot.state == SlotState::free) {
                if (slot.current_frame_id != 0) {
                    throw std::runtime_error("free slot retains prior frame ownership");
                }
                slot.state = SlotState::preparing;
                slot.current_frame_id = frame_id;
                ++slot.generation;
                return i;
            }
        }
        if (shared->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            throw std::runtime_error("timed out waiting for recorder RELEASE/free pool slot");
        }
    }
}

bool all_acked(const std::vector<FrameRecord>& records)
{
    return std::all_of(records.begin() + 1, records.end(), [](const FrameRecord& record) {
        return record.ack_count == 1;
    });
}

bool all_released(const std::vector<FrameRecord>& records)
{
    return std::all_of(records.begin() + 1, records.end(), [](const FrameRecord& record) {
        return record.release_count == 1;
    });
}

void wait_for_acks(SharedState* shared, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(shared->mutex);
    while (!all_acked(*shared->records)) {
        if (!shared->error.empty()) {
            throw std::runtime_error(shared->error);
        }
        if (shared->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            throw std::runtime_error("timed out waiting for all ACKs");
        }
    }
}

void wait_for_terminal_eof(SharedState* shared, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(shared->mutex);
    while (!shared->eof) {
        if (!shared->error.empty()) {
            throw std::runtime_error(shared->error);
        }
        if (shared->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            throw std::runtime_error("timed out waiting for recorder finalization/EOF");
        }
    }
    if (!shared->error.empty()) {
        throw std::runtime_error(shared->error);
    }
}

void write_csv(const Options& options, const std::vector<FrameRecord>& records)
{
    std::ofstream out(options.csv_path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open CSV output: " + options.csv_path);
    }
    out << "recording_frame_id,local_frame_id,slot_index,slot_generation,source_index,"
           "raw_file,raw_offset,raw_pitch,raw_frame_bytes,width,height,target_steady_ns,"
           "sent_steady_ns,ack_steady_ns,release_steady_ns,send_to_ack_ns,ack_to_release_ns,"
           "expected_gpu_id,expected_shard_id,ack_gpu_id,ack_shard_id,release_gpu_id,"
           "release_shard_id,ack_count,release_count,deferred_ack\n";
    for (size_t i = 1; i < records.size(); ++i) {
        const FrameRecord& record = records[i];
        const uint64_t send_to_ack =
            record.ack_ns >= record.send_ns ? record.ack_ns - record.send_ns : 0;
        const uint64_t ack_to_release =
            record.release_ns >= record.ack_ns ? record.release_ns - record.ack_ns : 0;
        out << record.frame_id << ',' << record.local_frame_id << ','
            << record.slot_index << ',' << record.slot_generation << ','
            << record.source_index << ',' << csv_quote(options.raw_file) << ','
            << record.raw_offset << ',' << options.raw_pitch << ','
            << options.raw_frame_bytes << ',' << options.width << ',' << options.height << ','
            << record.target_ns << ',' << record.send_ns << ',' << record.ack_ns << ','
            << record.release_ns << ',' << send_to_ack << ',' << ack_to_release << ','
            << record.expected_gpu_id << ',' << record.expected_shard_id << ','
            << record.ack_gpu_id << ',' << record.ack_shard_id << ','
            << record.release_gpu_id << ',' << record.release_shard_id << ','
            << record.ack_count << ',' << record.release_count << ','
            << (record.deferred_ack ? 1 : 0) << '\n';
    }
    if (!out) {
        throw std::runtime_error("failed writing CSV output: " + options.csv_path);
    }
}

void free_cuda_resources(std::vector<PoolSlot>* slots,
                         std::vector<SourceFrame>* sources,
                         cudaStream_t* stream)
{
    if (*stream) {
        cudaStreamDestroy(*stream);
        *stream = nullptr;
    }
    for (PoolSlot& slot : *slots) {
        if (slot.ptr) {
            cudaFree(slot.ptr);
            slot.ptr = nullptr;
        }
    }
    for (SourceFrame& source : *sources) {
        if (source.device_y) {
            cudaFree(source.device_y);
            source.device_y = nullptr;
        }
    }
}

int run(const Options& options)
{
    std::vector<SourceFrame> sources;
    std::vector<PoolSlot> slots;
    std::vector<FrameRecord> records(options.frames + 1);
    cudaStream_t copy_stream = nullptr;
    int socket_fd = -1;
    std::thread reader;
    std::string failure;

    SharedState shared;
    shared.slots = &slots;
    shared.records = &records;
    shared.expected_session = orange::external_recorder::ipc::token_value(options.session_id);
    shared.expected_stream = orange::external_recorder::ipc::token_value(options.stream_id);

    try {
        check_cuda(cudaSetDevice(options.source_gpu_id), "cudaSetDevice(source GPU)");
        load_sources(options, &sources);
        allocate_pool(options, &slots);
        check_cuda(cudaStreamCreateWithFlags(&copy_stream, cudaStreamNonBlocking),
                   "cudaStreamCreateWithFlags(copy stream)");

        socket_fd = connect_with_deadline(options);
        std::string receive_buffer;
        std::string hello_line;
        std::string read_error;
        if (!read_line_until(socket_fd,
                             Clock::now() + std::chrono::milliseconds(options.timeout_ms),
                             &receive_buffer,
                             &hello_line,
                             &read_error)) {
            throw std::runtime_error("recorder hello failed: " + read_error);
        }
        orange::external_recorder::ipc::HelloFields hello;
        if (!orange::external_recorder::ipc::parse_recorder_hello_line(hello_line, &hello)) {
            throw std::runtime_error("invalid RECORDER_HELLO: " + hello.error);
        }
        if (hello.role != "recorder" || hello.session_id != shared.expected_session ||
            hello.stream_id != shared.expected_stream) {
            throw std::runtime_error("RECORDER_HELLO identity mismatch: peer session=" +
                hello.session_id + " stream=" + hello.stream_id +
                " expected session=" + shared.expected_session +
                " stream=" + shared.expected_stream);
        }
        std::string identity_error;
        if (!orange::external_recorder::ipc::validate_recording_config_identity(
                hello,
                static_cast<int>(options.fps),
                static_cast<int>(options.gop),
                &identity_error)) {
            throw std::runtime_error("RECORDER_HELLO recording config mismatch: " +
                                     identity_error);
        }
        if (hello.features.find("frame_ack_release") == std::string::npos) {
            throw std::runtime_error("recorder does not advertise frame_ack_release");
        }
        const std::string client_hello =
            orange::external_recorder::ipc::build_client_hello_line(
                options.camera_serial,
                options.session_id,
                options.stream_id,
                "orange_full_frame",
                static_cast<int>(options.fps),
                static_cast<int>(options.gop),
                static_cast<int>(options.width),
                static_cast<int>(options.height),
                options.source_gpu_id,
                true);
        if (!send_all(socket_fd, client_hello)) {
            throw std::runtime_error("failed sending CLIENT_HELLO: " +
                                     std::string(std::strerror(errno)));
        }
        reader = std::thread(protocol_reader, socket_fd, receive_buffer, &shared);

        const size_t y_bytes = static_cast<size_t>(options.width) * options.height;
        const size_t pool_bytes = y_bytes + y_bytes / 2;
        const uint64_t frame_period_ns = 1000000000ULL / options.fps;
        const auto replay_start = Clock::now() + std::chrono::milliseconds(10);
        const uint64_t replay_start_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                replay_start.time_since_epoch()).count());

        for (uint64_t frame_id = 1; frame_id <= options.frames; ++frame_id) {
            if (g_interrupted.load(std::memory_order_relaxed)) {
                throw std::runtime_error("interrupted during replay");
            }
            const auto target = replay_start +
                std::chrono::nanoseconds((frame_id - 1) * frame_period_ns);
            std::this_thread::sleep_until(target);
            const size_t slot_index = reserve_slot(
                &shared,
                frame_id,
                Clock::now() + std::chrono::milliseconds(options.timeout_ms));
            const uint32_t source_index = static_cast<uint32_t>(
                (frame_id - 1) % options.source_frames);
            PoolSlot& slot = slots[slot_index];
            const SourceFrame& source = sources[source_index];
            check_cuda(cudaMemcpy2DAsync(slot.ptr,
                                         options.width,
                                         source.device_y,
                                         source.device_pitch,
                                         options.width,
                                         options.height,
                                         cudaMemcpyDeviceToDevice,
                                         copy_stream),
                       "cudaMemcpy2DAsync(cached Y to tight IPC slot)");
            check_cuda(cudaStreamSynchronize(copy_stream),
                       "cudaStreamSynchronize(before FRAME export)");

            const uint64_t zero_based = frame_id - 1;
            const uint64_t gop_index = zero_based / options.gop;
            const uint32_t frame_index_within_gop = static_cast<uint32_t>(zero_based % options.gop);
            const size_t route_index = static_cast<size_t>(gop_index % options.route_gpu_ids.size());
            const int expected_gpu = options.route_gpu_ids[route_index];
            const int expected_shard = static_cast<int>(route_index);
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (slot.state != SlotState::preparing || slot.current_frame_id != frame_id) {
                    throw std::runtime_error("slot ownership changed while preparing frame");
                }
                FrameRecord& record = records[frame_id];
                record.frame_id = frame_id;
                record.local_frame_id = frame_id;
                record.slot_index = slot_index;
                record.slot_generation = slot.generation;
                record.source_index = source_index;
                record.raw_offset = static_cast<uint64_t>(source_index) * options.raw_frame_bytes;
                record.target_ns = replay_start_ns + zero_based * frame_period_ns;
                record.send_ns = steady_ns();
                record.expected_gpu_id = expected_gpu;
                record.expected_shard_id = expected_shard;
                slot.state = SlotState::in_flight;
            }

            std::ostringstream descriptor;
            descriptor << "FRAME "
                       << options.camera_serial << ' '
                       << frame_id << ' '
                       << frame_id << ' '
                       << options.source_gpu_id << ' '
                       << options.width << ' '
                       << options.height << ' '
                       << options.pixel_format << ' '
                       << y_bytes << ' '
                       << (1000000000ULL + zero_based * frame_period_ns) << ' '
                       << realtime_ns() << ' '
                       << slot.handle_hex << ' '
                       << orange::external_recorder::ipc::token_value(options.session_id) << ' '
                       << orange::external_recorder::ipc::token_value(options.stream_id) << ' '
                       << gop_index << ' '
                       << frame_index_within_gop << ' '
                       // Match the production producer: this is only a route
                       // hint. The recorder derives the actual GPU/shard from
                       // gop_index and reports that decision in ACK/RELEASE.
                       << options.route_gpu_ids.front() << ' '
                       << 0 << ' '
                       << "single_shard"
                       << " nv12_pool pool_bytes=" << pool_bytes << '\n';
            if (!send_all(socket_fd, descriptor.str())) {
                throw std::runtime_error("failed sending FRAME " + std::to_string(frame_id) +
                                         ": " + std::string(std::strerror(errno)));
            }
        }

        wait_for_acks(&shared, options.timeout_ms);
        const std::string drain = orange::external_recorder::ipc::build_client_control_line(
            options.camera_serial,
            options.session_id,
            options.stream_id,
            "orange_full_frame",
            orange::external_recorder::ipc::kClientControlDrain,
            "ipc_replay_complete");
        if (!send_all(socket_fd, drain)) {
            throw std::runtime_error("failed sending CLIENT_CONTROL drain");
        }
        const std::string finalize = orange::external_recorder::ipc::build_client_control_line(
            options.camera_serial,
            options.session_id,
            options.stream_id,
            "orange_full_frame",
            orange::external_recorder::ipc::kClientControlFinalize,
            "ipc_replay_complete");
        if (!send_all(socket_fd, finalize)) {
            throw std::runtime_error("failed sending CLIENT_CONTROL finalize");
        }
        if (shutdown(socket_fd, SHUT_WR) != 0 && errno != ENOTCONN) {
            throw std::runtime_error("shutdown(SHUT_WR) failed: " +
                                     std::string(std::strerror(errno)));
        }
        wait_for_terminal_eof(&shared, options.timeout_ms);
        if (!all_released(records)) {
            throw std::runtime_error("recorder EOF arrived before every frame was RELEASEd");
        }
        for (const PoolSlot& slot : slots) {
            if (slot.state != SlotState::free || slot.current_frame_id != 0) {
                throw std::runtime_error("pool retains an in-flight frame after terminal EOF");
            }
        }
    } catch (const std::exception& e) {
        failure = e.what();
    }

    if (socket_fd >= 0) {
        shutdown(socket_fd, SHUT_RDWR);
    }
    if (reader.joinable()) {
        reader.join();
    }
    if (socket_fd >= 0) {
        close(socket_fd);
    }

    try {
        write_csv(options, records);
    } catch (const std::exception& e) {
        if (failure.empty()) {
            failure = e.what();
        } else {
            failure += "; additionally, " + std::string(e.what());
        }
    }
    free_cuda_resources(&slots, &sources, &copy_stream);

    if (!failure.empty()) {
        std::cerr << "ipc_replay_probe: FAIL: " << failure
                  << " (partial protocol CSV: " << options.csv_path << ")\n";
        return 1;
    }
    std::cout << "ipc_replay_probe: PASS frames=" << options.frames
              << " pool=" << options.pool_size
              << " source_frames=" << options.source_frames
              << " fps=" << options.fps
              << " source_gpu=" << options.source_gpu_id
              << " route_gpus=";
    for (size_t i = 0; i < options.route_gpu_ids.size(); ++i) {
        std::cout << (i == 0 ? "" : ",") << options.route_gpu_ids[i];
    }
    std::cout << " status_messages=" << shared.status_messages
              << " last_status_received=" << shared.last_status_frames_received
              << " last_status_encoded=" << shared.last_status_frames_encoded
              << " csv=" << options.csv_path << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "ipc_replay_probe: " << e.what() << '\n';
        return 2;
    }
}
