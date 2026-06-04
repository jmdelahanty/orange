#include "camera.h"
#include "project.h"

#include <EmergentCameraAPIs.h>
#include <gigevisiondeviceinfo.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kMaxCameras = 32;

struct Options {
    std::vector<std::string> serials;
    bool all = false;
    bool list_only = false;
    std::string config_dir;
    int frames = 0;
    int buffer_count = 4;
    int timeout_ms = 1000;
    double measure_seconds = 0.0;
    int override_frame_rate = -1;
    int override_gpu_direct = -1;
    bool show_help = false;
};

std::string trim_copy(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string canonical_serial(std::string value)
{
    value = trim_copy(value);
    if (value.size() >= 3 &&
        (value.substr(0, 3) == "Cam" || value.substr(0, 3) == "cam")) {
        value = value.substr(3);
    }
    return value;
}

std::vector<std::string> split_csv(const std::string& value)
{
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = canonical_serial(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

bool parse_int(const std::string& value, int* out)
{
    try {
        size_t consumed = 0;
        int parsed = std::stoi(value, &consumed, 10);
        if (consumed != value.size()) {
            return false;
        }
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double(const std::string& value, double* out)
{
    try {
        size_t consumed = 0;
        double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            return false;
        }
        *out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void print_usage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Explicit camera hardware diagnostic. This is not a CI test.\n"
        << "\n"
        << "Options:\n"
        << "  --config-dir <dir>       Camera config directory. Defaults to ORANGE_GUI_CONFIG_DIR.\n"
        << "  --serial <serial>        Probe one camera serial. May be repeated.\n"
        << "  --serials <csv>          Probe comma-separated serials. Defaults to ORANGE_GUI_EXPECT_CAMERAS.\n"
        << "  --all                    Probe every discovered camera with a usable config/default.\n"
        << "  --list-only              List discovered cameras and config match state only.\n"
        << "  --frames <n>             After stream open, acquire n frames before close (default 0).\n"
        << "  --measure-seconds <s>    Timed raw acquisition FPS measurement after stream open.\n"
        << "  --buffer-count <n>       EVT frame buffers when grabbing frames (default 4).\n"
        << "  --timeout-ms <ms>        Frame wait timeout when grabbing frames (default 1000).\n"
        << "  --frame-rate <fps>       Diagnostic override for configured FrameRate.\n"
        << "  --gpu-direct <0|1>       Diagnostic override for configured GPUDirect.\n"
        << "  --help                   Show this message.\n";
}

bool parse_args(int argc, char** argv, Options* options)
{
    const char* env_config_dir = std::getenv("ORANGE_GUI_CONFIG_DIR");
    if (env_config_dir != nullptr) {
        options->config_dir = env_config_dir;
    }
    const char* env_expected_cameras = std::getenv("ORANGE_GUI_EXPECT_CAMERAS");
    if (env_expected_cameras != nullptr) {
        options->serials = split_csv(env_expected_cameras);
    }
    bool serials_set_by_cli = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (arg == "--help" || arg == "-h") {
            options->show_help = true;
            return true;
        } else if (arg == "--config-dir") {
            const char* value = require_next("--config-dir");
            if (!value) return false;
            options->config_dir = value;
        } else if (arg == "--serial") {
            const char* value = require_next("--serial");
            if (!value) return false;
            if (!serials_set_by_cli) {
                options->serials.clear();
                serials_set_by_cli = true;
            }
            std::string serial = canonical_serial(value);
            if (!serial.empty()) {
                options->serials.push_back(serial);
            }
        } else if (arg == "--serials") {
            const char* value = require_next("--serials");
            if (!value) return false;
            if (!serials_set_by_cli) {
                options->serials.clear();
                serials_set_by_cli = true;
            }
            std::vector<std::string> serials = split_csv(value);
            options->serials.insert(options->serials.end(), serials.begin(), serials.end());
        } else if (arg == "--all") {
            options->all = true;
        } else if (arg == "--list-only") {
            options->list_only = true;
        } else if (arg == "--frames") {
            const char* value = require_next("--frames");
            if (!value) return false;
            if (!parse_int(value, &options->frames) || options->frames < 0) {
                std::cerr << "Invalid --frames value: " << value << "\n";
                return false;
            }
        } else if (arg == "--buffer-count") {
            const char* value = require_next("--buffer-count");
            if (!value) return false;
            if (!parse_int(value, &options->buffer_count) || options->buffer_count <= 0) {
                std::cerr << "Invalid --buffer-count value: " << value << "\n";
                return false;
            }
        } else if (arg == "--measure-seconds") {
            const char* value = require_next("--measure-seconds");
            if (!value) return false;
            if (!parse_double(value, &options->measure_seconds) || options->measure_seconds < 0.0) {
                std::cerr << "Invalid --measure-seconds value: " << value << "\n";
                return false;
            }
        } else if (arg == "--timeout-ms") {
            const char* value = require_next("--timeout-ms");
            if (!value) return false;
            if (!parse_int(value, &options->timeout_ms) || options->timeout_ms <= 0) {
                std::cerr << "Invalid --timeout-ms value: " << value << "\n";
                return false;
            }
        } else if (arg == "--frame-rate") {
            const char* value = require_next("--frame-rate");
            if (!value) return false;
            if (!parse_int(value, &options->override_frame_rate) ||
                options->override_frame_rate <= 0) {
                std::cerr << "Invalid --frame-rate value: " << value << "\n";
                return false;
            }
        } else if (arg == "--gpu-direct") {
            const char* value = require_next("--gpu-direct");
            if (!value) return false;
            if (!parse_int(value, &options->override_gpu_direct) ||
                (options->override_gpu_direct != 0 && options->override_gpu_direct != 1)) {
                std::cerr << "Invalid --gpu-direct value: " << value << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }
    }

    std::sort(options->serials.begin(), options->serials.end());
    options->serials.erase(std::unique(options->serials.begin(), options->serials.end()), options->serials.end());
    return true;
}

bool serial_requested(const Options& options, const char* serial)
{
    if (options.all) {
        return true;
    }
    const std::string normalized = canonical_serial(serial == nullptr ? "" : serial);
    return std::find(options.serials.begin(), options.serials.end(), normalized) != options.serials.end();
}

bool load_params_for_device(
    GigEVisionDeviceInfo* device,
    int selected_index,
    int selected_count,
    std::vector<std::string>& camera_config_files,
    CameraParams* params)
{
    return set_camera_params(params, device, camera_config_files, selected_index, selected_count);
}

void print_device_line(
    int index,
    GigEVisionDeviceInfo* device,
    std::vector<std::string>& camera_config_files)
{
    CameraParams params{};
    const bool params_ok = load_params_for_device(device, index, 1, camera_config_files, &params);
    std::cout << "[" << index << "] serial=" << canonical_serial(device->serialNumber)
              << " model=" << device->modelName
              << " ip=" << device->currentIp
              << " nic=" << device->nic.ip4Address
              << " config=" << (params.config_path.empty() ? "<default-or-missing>" : params.config_path)
              << " params=" << (params_ok ? "ok" : "missing")
              << " lens_control_enabled=" << (params.lens_control_enabled ? "true" : "false")
              << " gpu_direct=" << (params.gpu_direct ? "true" : "false")
              << "\n";
}

struct ProbeResult {
    bool ok = false;
    std::string error;
};

ProbeResult probe_camera(
    GigEVisionDeviceInfo* device,
    int selected_index,
    int selected_count,
    std::vector<std::string>& camera_config_files,
    const Options& options)
{
    ProbeResult result;
    CameraParams params{};
    CameraEmergent ecam{};
    bool camera_opened = false;
    bool camera_open_attempted = false;
    bool stream_opened = false;
    int buffers_allocated = 0;
    bool acquisition_started = false;

    try {
        if (!load_params_for_device(device, selected_index, selected_count, camera_config_files, &params)) {
            throw std::runtime_error("No usable camera config/default parameters for serial " +
                                     canonical_serial(device->serialNumber));
        }
        const unsigned int configured_frame_rate = params.frame_rate;
        const bool configured_gpu_direct = params.gpu_direct;
        if (options.override_frame_rate > 0) {
            params.frame_rate = static_cast<unsigned int>(options.override_frame_rate);
        }
        if (options.override_gpu_direct >= 0) {
            params.gpu_direct = options.override_gpu_direct != 0;
            if (!params.gpu_direct) {
                params.gpu_id_runtime_overridden = false;
            }
        }
        const unsigned int requested_frame_rate = params.frame_rate;

        std::cout << "\n[CAMERA] serial=" << params.camera_serial
                  << " model=" << params.device_model
                  << " config=" << (params.config_path.empty() ? "<default>" : params.config_path)
                  << "\n"
                  << "         size=" << params.width << "x" << params.height
                  << " fps=" << params.frame_rate
                  << " pixel_format=" << params.pixel_format
                  << " gpu_direct=" << (params.gpu_direct ? "true" : "false")
                  << " source_gpu_id=" << params.gpu_id
                  << " sync_mode=" << params.sync_mode
                  << " lens_control_enabled=" << (params.lens_control_enabled ? "true" : "false")
                  << "\n";

        camera_open_attempted = true;
        open_camera_with_params(&ecam.camera, device, &params, "evt_stream_smoke");
        camera_opened = true;
        std::cout << "[PASS] " << params.camera_serial << " open/apply config\n";
        std::cout << "[INFO] " << params.camera_serial
                  << " FrameRate configured=" << configured_frame_rate
                  << " requested=" << requested_frame_rate
                  << " applied_readback=" << params.frame_rate
                  << " range=[" << params.frame_rate_min << "," << params.frame_rate_max << "]";
        if (params.frame_rate_inc > 0) {
            std::cout << " inc=" << params.frame_rate_inc;
        }
        std::cout << "\n";
        if (options.override_gpu_direct >= 0) {
            std::cout << "[INFO] " << params.camera_serial
                      << " GPUDirect configured=" << (configured_gpu_direct ? "true" : "false")
                      << " requested=" << (params.gpu_direct ? "true" : "false")
                      << "\n";
        }

        camera_open_stream(&ecam.camera, &params, "evt_stream_smoke");
        stream_opened = true;
        std::cout << "[PASS] " << params.camera_serial << " stream open\n";

        if (options.frames > 0 || options.measure_seconds > 0.0) {
            const int buffer_count = std::max(options.buffer_count, 2);
            ecam.evt_frame = new Emergent::CEmergentFrame[buffer_count]();
            ecam.evt_frame_count = buffer_count;
            for (int buffer_idx = 0; buffer_idx < buffer_count; ++buffer_idx) {
                set_frame_buffer(&ecam.evt_frame[buffer_idx], &params);
                check_camera_errors(
                    Emergent::EVT_AllocateFrameBuffer(
                        &ecam.camera,
                        &ecam.evt_frame[buffer_idx],
                        EVT_FRAME_BUFFER_ZERO_COPY),
                    params.camera_serial.c_str());
                ++buffers_allocated;
                check_camera_errors(
                    Emergent::EVT_CameraQueueFrame(&ecam.camera, &ecam.evt_frame[buffer_idx]),
                    params.camera_serial.c_str());
            }

            check_camera_errors(
                Emergent::EVT_CameraExecuteCommand(&ecam.camera, "AcquisitionStart"),
                params.camera_serial.c_str());
            acquisition_started = true;
            std::cout << "[PASS] " << params.camera_serial << " acquisition start\n";

            auto consume_frame = [&](int frame_number, bool print_each_frame) {
                Emergent::CEmergentFrame frame{};
                const EVT_ERROR get_err = Emergent::EVT_CameraGetFrame(
                    &ecam.camera,
                    &frame,
                    static_cast<unsigned int>(options.timeout_ms));
                if (get_err != EVT_SUCCESS) {
                    return get_err;
                }
                if (print_each_frame) {
                    std::cout << "[PASS] " << params.camera_serial
                              << " frame " << frame_number
                              << " frame_id=" << frame.frame_id
                              << " timestamp=" << frame.timestamp
                              << " bytes=" << frame.bufferSize
                              << "\n";
                }
                check_camera_errors(
                    Emergent::EVT_CameraQueueFrame(&ecam.camera, &frame),
                    params.camera_serial.c_str());
                return EVT_SUCCESS;
            };

            for (int frame_idx = 0; frame_idx < options.frames; ++frame_idx) {
                const EVT_ERROR get_err = consume_frame(frame_idx + 1, true);
                if (get_err != EVT_SUCCESS) {
                    throw std::runtime_error(
                        "EVT_CameraGetFrame failed: " + get_evt_error_string(get_err));
                }
            }

            if (options.measure_seconds > 0.0) {
                const auto start = std::chrono::steady_clock::now();
                const auto deadline =
                    start +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(options.measure_seconds));
                auto end = start;
                int received = 0;
                int timeouts = 0;
                std::uint64_t first_frame_id = 0;
                std::uint64_t last_frame_id = 0;
                std::uint64_t frame_id_gaps = 0;

                while (std::chrono::steady_clock::now() < deadline) {
                    Emergent::CEmergentFrame frame{};
                    const EVT_ERROR get_err = Emergent::EVT_CameraGetFrame(
                        &ecam.camera,
                        &frame,
                        static_cast<unsigned int>(options.timeout_ms));
                    end = std::chrono::steady_clock::now();
                    if (get_err == EVT_ERROR_TIMEDOUT) {
                        ++timeouts;
                        continue;
                    }
                    if (get_err != EVT_SUCCESS) {
                        throw std::runtime_error(
                            "EVT_CameraGetFrame failed during measurement: " +
                            get_evt_error_string(get_err));
                    }

                    const std::uint64_t frame_id = static_cast<std::uint64_t>(frame.frame_id);
                    if (received == 0) {
                        first_frame_id = frame_id;
                    } else if (frame_id > last_frame_id + 1) {
                        frame_id_gaps += frame_id - last_frame_id - 1;
                    }
                    last_frame_id = frame_id;
                    ++received;

                    check_camera_errors(
                        Emergent::EVT_CameraQueueFrame(&ecam.camera, &frame),
                        params.camera_serial.c_str());
                }

                const double elapsed =
                    std::chrono::duration<double>(end - start).count();
                const double measured_fps = elapsed > 0.0 ? received / elapsed : 0.0;
                std::cout << std::fixed << std::setprecision(3)
                          << "[MEASURE] " << params.camera_serial
                          << " seconds=" << elapsed
                          << " received=" << received
                          << " fps=" << measured_fps
                          << " timeouts=" << timeouts
                          << " frame_id_gaps=" << frame_id_gaps
                          << " first_frame_id=" << first_frame_id
                          << " last_frame_id=" << last_frame_id
                          << std::defaultfloat
                          << "\n";
            }
        }

        result.ok = true;
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = ex.what();
    }

    if (acquisition_started) {
        const EVT_ERROR stop_err = Emergent::EVT_CameraExecuteCommand(&ecam.camera, "AcquisitionStop");
        if (stop_err != EVT_SUCCESS) {
            std::cerr << "[WARN] " << params.camera_serial
                      << " AcquisitionStop failed: " << get_evt_error_string(stop_err) << "\n";
        }
    }
    if (buffers_allocated > 0 && ecam.evt_frame != nullptr) {
        for (int buffer_idx = 0; buffer_idx < buffers_allocated; ++buffer_idx) {
            const EVT_ERROR release_err =
                Emergent::EVT_ReleaseFrameBuffer(&ecam.camera, &ecam.evt_frame[buffer_idx]);
            if (release_err != EVT_SUCCESS) {
                std::cerr << "[WARN] " << params.camera_serial
                          << " frame-buffer " << buffer_idx
                          << " release failed: " << get_evt_error_string(release_err) << "\n";
                result.ok = false;
                if (result.error.empty()) {
                    result.error = "EVT_ReleaseFrameBuffer failed: " +
                                   get_evt_error_string(release_err);
                }
            }
        }
    }
    delete[] ecam.evt_frame;
    ecam.evt_frame = nullptr;
    ecam.evt_frame_count = 0;

    if (stream_opened) {
        const EVT_ERROR close_stream_err = Emergent::EVT_CameraCloseStream(&ecam.camera);
        if (close_stream_err == EVT_SUCCESS) {
            std::cout << "[PASS] " << params.camera_serial << " stream close\n";
        } else {
            std::cerr << "[FAIL] " << params.camera_serial
                      << " stream close: " << get_evt_error_string(close_stream_err) << "\n";
            result.ok = false;
            if (result.error.empty()) {
                result.error = "EVT_CameraCloseStream failed: " + get_evt_error_string(close_stream_err);
            }
        }
    }

    if (camera_opened || camera_open_attempted) {
        const EVT_ERROR close_err = Emergent::EVT_CameraClose(&ecam.camera);
        if (close_err == EVT_SUCCESS) {
            std::cout << "\nClose Camera: \t\tCamera Closed\n";
            std::cout << "[PASS] " << params.camera_serial << " camera close\n";
        } else if (camera_opened) {
            std::cerr << "[FAIL] " << params.camera_serial
                      << " camera close: " << get_evt_error_string(close_err) << "\n";
            result.ok = false;
            if (result.error.empty()) {
                result.error = "EVT_CameraClose failed: " + get_evt_error_string(close_err);
            }
        } else {
            std::cerr << "[WARN] " << params.camera_serial
                      << " camera close after failed open/apply-config returned: "
                      << get_evt_error_string(close_err) << "\n";
        }
    }

    if (result.ok) {
        std::cout << "[RESULT] " << params.camera_serial << " PASS\n";
    } else {
        std::cout << "[RESULT] " << (params.camera_serial.empty() ? canonical_serial(device->serialNumber) : params.camera_serial)
                  << " FAIL";
        if (!result.error.empty()) {
            std::cout << ": " << result.error;
        }
        std::cout << "\n";
    }

    return result;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_args(argc, argv, &options)) {
        return 2;
    }
    if (options.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (options.config_dir.empty() && !options.list_only) {
        std::cerr << "Missing --config-dir or ORANGE_GUI_CONFIG_DIR.\n";
        return 2;
    }
    if (!options.list_only && !options.all && options.serials.empty()) {
        std::cerr << "Choose --serial, --serials, or --all. Use --list-only to inspect devices first.\n";
        return 2;
    }

    std::vector<std::string> camera_config_files;
    if (!options.config_dir.empty()) {
        update_camera_configs(camera_config_files, options.config_dir);
    }
    std::cout << "Config dir: " << (options.config_dir.empty() ? "<none>" : options.config_dir)
              << " configs=" << camera_config_files.size()
              << "\n";

    GigEVisionDeviceInfo devices[kMaxCameras]{};
    const int camera_count = scan_cameras(kMaxCameras, devices);
    if (camera_count <= 0) {
        std::cerr << "No EVT cameras discovered.\n";
        return 1;
    }
    std::cout << "Discovered cameras: " << camera_count << "\n";

    if (options.list_only) {
        for (int i = 0; i < camera_count; ++i) {
            print_device_line(i, &devices[i], camera_config_files);
        }
        return 0;
    }

    std::vector<int> selected_indices;
    for (int i = 0; i < camera_count; ++i) {
        if (serial_requested(options, devices[i].serialNumber)) {
            selected_indices.push_back(i);
        }
    }
    if (selected_indices.empty()) {
        std::cerr << "No discovered camera matched requested serials.\n";
        return 1;
    }

    int failures = 0;
    for (size_t selected_pos = 0; selected_pos < selected_indices.size(); ++selected_pos) {
        const int device_index = selected_indices[selected_pos];
        ProbeResult result = probe_camera(
            &devices[device_index],
            static_cast<int>(selected_pos),
            static_cast<int>(selected_indices.size()),
            camera_config_files,
            options);
        if (!result.ok) {
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "\nResult: PASS\n";
        return 0;
    }
    std::cout << "\nResult: FAIL (" << failures << " camera"
              << (failures == 1 ? "" : "s") << " failed)\n";
    return 1;
}
