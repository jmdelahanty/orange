#include <cuda_runtime.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
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
};

struct FrameDescriptor {
    std::string camera_serial;
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    int source_gpu_id = -1;
    int width = 0;
    int height = 0;
    int pixel_format = 0;
    uint64_t bytes = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    std::string handle_hex;
};

struct ImportedHandle {
    void* ptr = nullptr;
};

struct Sample {
    uint64_t frame_index = 0;
    std::string camera_serial;
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t bytes = 0;
    double total_ms = 0.0;
    double open_handle_ms = 0.0;
    double copy_ms = 0.0;
    bool opened_handle = false;
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
    return options;
}

void check_cuda(cudaError_t status, const char* call)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(call) + " failed: " + cudaGetErrorString(status));
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
    return static_cast<bool>(in) && !desc->handle_hex.empty() && desc->bytes > 0;
}

void write_csv_header(std::ofstream& csv)
{
    csv << "frame_index,camera_serial,recording_frame_id,local_frame_id,bytes,"
           "total_ms,open_handle_ms,copy_ms,opened_handle\n";
}

void write_csv_row(std::ofstream& csv, const Sample& sample)
{
    csv << sample.frame_index << ","
        << sample.camera_serial << ","
        << sample.recording_frame_id << ","
        << sample.local_frame_id << ","
        << sample.bytes << ","
        << std::fixed << std::setprecision(6)
        << sample.total_ms << ","
        << sample.open_handle_ms << ","
        << sample.copy_ms << ","
        << (sample.opened_handle ? "true" : "false") << "\n";
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
            csv.open(options.csv_path);
            if (!csv) {
                throw std::runtime_error("Failed to open CSV: " + options.csv_path);
            }
            write_csv_header(csv);
        }

        std::unordered_map<std::string, ImportedHandle> imported_handles;
        void* owned_device_buffer = nullptr;
        uint64_t owned_device_buffer_bytes = 0;
        uint64_t frame_count = 0;

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

            Sample sample;
            sample.frame_index = frame_count;
            sample.camera_serial = desc.camera_serial;
            sample.recording_frame_id = desc.recording_frame_id;
            sample.local_frame_id = desc.local_frame_id;
            sample.bytes = desc.bytes;

            const auto total_start = std::chrono::steady_clock::now();

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

            if (desc.bytes > owned_device_buffer_bytes) {
                if (owned_device_buffer) {
                    cudaFree(owned_device_buffer);
                    owned_device_buffer = nullptr;
                    owned_device_buffer_bytes = 0;
                }
                check_cuda(cudaMalloc(&owned_device_buffer, desc.bytes), "cudaMalloc(owned detach buffer)");
                owned_device_buffer_bytes = desc.bytes;
            }

            const auto copy_start = std::chrono::steady_clock::now();
            check_cuda(
                cudaMemcpy(
                    owned_device_buffer,
                    handle_it->second.ptr,
                    desc.bytes,
                    cudaMemcpyDeviceToDevice),
                "cudaMemcpy(detach copy)");
            sample.copy_ms = elapsed_ms(copy_start);
            sample.total_ms = elapsed_ms(total_start);

            if (!write_all(client_fd, "ACK " + std::to_string(desc.recording_frame_id) + "\n")) {
                break;
            }
            if (csv) {
                write_csv_row(csv, sample);
                if ((frame_count % 60) == 0) {
                    csv.flush();
                }
            }
            ++frame_count;
        }

        if (csv) {
            csv.flush();
        }
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
        std::cout << "external_recorder_ipc_probe complete frames=" << frame_count << std::endl;
        return 0;
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
