#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "network_base.h"
#include "thread.h"
#include "types.h"
#include <cstring>
#include "video_capture.h"
#include "NvEncoder/NvCodecUtils.h"
#include "project.h"
#include "video_capture.h"
#include "fetch_generated.h"
#include "acquire_frames.h"
#include "modern_recording_pipeline.h"
#include <signal.h>

#define evt_buffer_size 100
#define max_cameras 20

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

enum class HeadlessMode {
    Remote,
    Local,
};

struct HeadlessEncoderSettings {
    std::string codec = "h264";
    std::string preset = "p1";
    std::string tuning = "ll";
    std::string rate_control_mode = "vbr";
    int quality_value = 20;
    int gop_length = 0;
    bool select_all_cameras = true;
    std::vector<std::string> camera_serials;
};

struct HeadlessCliOptions {
    HeadlessMode mode = HeadlessMode::Remote;
    bool show_help = false;
    bool list_cameras = false;
    std::string config_folder;
    std::string record_folder;
    std::string experiment_spec_path;
    int duration_seconds = 0;
    HeadlessEncoderSettings encoder_settings;
};

std::vector<std::string> split_headless_encoder_setup(const std::string& setup)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : setup) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == ';' || ch == '|') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

void append_camera_selection(HeadlessEncoderSettings* settings, const std::string& value)
{
    if (!settings || value.empty()) {
        return;
    }
    if (value == "all" || value == "*") {
        settings->select_all_cameras = true;
        settings->camera_serials.clear();
        return;
    }
    if (settings->select_all_cameras) {
        settings->select_all_cameras = false;
        settings->camera_serials.clear();
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t plus = value.find('+', start);
        const std::string serial = value.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (!serial.empty()) {
            settings->camera_serials.push_back(serial);
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
}

HeadlessEncoderSettings parse_headless_encoder_setup(const std::string& setup)
{
    HeadlessEncoderSettings settings;
    const std::vector<std::string> tokens = split_headless_encoder_setup(setup);

    int positional_index = 0;
    auto assign_positional = [&](const std::string& token) {
        switch (positional_index++) {
            case 0: settings.codec = token; break;
            case 1: settings.preset = token; break;
            case 2: settings.tuning = token; break;
            case 3: settings.rate_control_mode = token; break;
            case 4: settings.quality_value = std::atoi(token.c_str()); break;
            case 5: settings.gop_length = std::atoi(token.c_str()); break;
            default: break;
        }
    };

    for (const std::string& token : tokens) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            assign_positional(token);
            continue;
        }

        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (value.empty()) {
            continue;
        }

        if (key == "codec") {
            settings.codec = value;
        } else if (key == "preset") {
            settings.preset = value;
        } else if (key == "tuning" || key == "tune") {
            settings.tuning = value;
        } else if (key == "camera" || key == "cameras" ||
                   key == "camera_serial" || key == "camera_serials") {
            append_camera_selection(&settings, value);
        } else if (key == "rc" || key == "rate_control" || key == "rate_control_mode") {
            settings.rate_control_mode = value;
        } else if (key == "quality" || key == "cq" || key == "qp") {
            settings.quality_value = std::atoi(value.c_str());
        } else if (key == "gop" || key == "gop_length") {
            settings.gop_length = std::atoi(value.c_str());
        }
    }

    if (settings.codec.empty()) {
        settings.codec = "h264";
    }
    if (settings.preset.empty()) {
        settings.preset = "p1";
    }
    if (settings.tuning.empty()) {
        settings.tuning = "ll";
    }
    if (settings.rate_control_mode.empty()) {
        settings.rate_control_mode = "vbr";
    }
    if (settings.quality_value < 1) {
        settings.quality_value = 20;
    }
    if (settings.gop_length < 0) {
        settings.gop_length = 0;
    }

    return settings;
}

std::string format_selected_camera_serials(const HeadlessEncoderSettings& settings)
{
    if (settings.select_all_cameras || settings.camera_serials.empty()) {
        return "all";
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < settings.camera_serials.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << settings.camera_serials[i];
    }
    return out.str();
}

std::string build_headless_encoder_setup_string(const HeadlessEncoderSettings& settings)
{
    std::ostringstream out;
    out << "codec=" << settings.codec
        << " preset=" << settings.preset
        << " tuning=" << settings.tuning
        << " rc=" << settings.rate_control_mode
        << " quality=" << settings.quality_value
        << " gop=" << settings.gop_length
        << " camera=" << format_selected_camera_serials(settings);
    return out.str();
}

void print_headless_usage(const char* argv0)
{
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " --mode remote\n"
        << "  " << argv0 << " --mode local --record-folder <path> [options]\n"
        << "  " << argv0 << " --mode local --list-cameras\n\n"
        << "Local mode options:\n"
        << "  --camera <serial|all>        Repeatable. Defaults to all.\n"
        << "  --config-folder <path>       Optional camera config folder.\n"
        << "  --record-folder <path>       Required for local recording runs.\n"
        << "  --codec <h264|hevc>\n"
        << "  --preset <p1..p7>\n"
        << "  --tuning <ull|ll|hq>\n"
        << "  --rate-control <vbr|cbr|cqp>\n"
        << "  --quality <int>\n"
        << "  --gop <int>\n"
        << "  --duration <seconds>         Optional. Otherwise runs until Ctrl+C.\n"
        << "  --experiment-spec <path>     Reserved for future work.\n"
        << "  --list-cameras               List local cameras and exit.\n"
        << "  --help\n";
}

bool parse_non_negative_int(const std::string& value, int* out)
{
    if (!out || value.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool parse_headless_cli_options(int argc, char* argv[], HeadlessCliOptions* options, std::string* error_out)
{
    if (!options) {
        if (error_out) {
            *error_out = "Internal error: null CLI options";
        }
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string inline_value;
        const std::size_t equals = arg.find('=');
        if (arg.rfind("--", 0) == 0 && equals != std::string::npos) {
            inline_value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }

        auto consume_value = [&](const char* flag_name) -> std::string {
            if (!inline_value.empty()) {
                return inline_value;
            }
            if (i + 1 >= argc) {
                if (error_out) {
                    *error_out = std::string("Missing value for ") + flag_name;
                }
                return {};
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            options->show_help = true;
            continue;
        }
        if (arg == "--mode") {
            const std::string value = consume_value("--mode");
            if (value.empty() && options->show_help == false && error_out && !error_out->empty()) {
                return false;
            }
            if (value == "remote") {
                options->mode = HeadlessMode::Remote;
            } else if (value == "local") {
                options->mode = HeadlessMode::Local;
            } else {
                if (error_out) {
                    *error_out = "Unsupported --mode value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--camera") {
            const std::string value = consume_value("--camera");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            append_camera_selection(&options->encoder_settings, value);
            continue;
        }
        if (arg == "--config-folder") {
            options->config_folder = consume_value("--config-folder");
            if (options->config_folder.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--record-folder") {
            options->record_folder = consume_value("--record-folder");
            if (options->record_folder.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--codec") {
            options->encoder_settings.codec = consume_value("--codec");
            if (options->encoder_settings.codec.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--preset") {
            options->encoder_settings.preset = consume_value("--preset");
            if (options->encoder_settings.preset.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--tuning") {
            options->encoder_settings.tuning = consume_value("--tuning");
            if (options->encoder_settings.tuning.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--rate-control" || arg == "--rate-control-mode" || arg == "--rc") {
            options->encoder_settings.rate_control_mode = consume_value("--rate-control");
            if (options->encoder_settings.rate_control_mode.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--quality" || arg == "--cq" || arg == "--qp") {
            const std::string value = consume_value("--quality");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.quality_value) ||
                options->encoder_settings.quality_value < 1) {
                if (error_out) {
                    *error_out = "Invalid --quality value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--gop") {
            const std::string value = consume_value("--gop");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.gop_length)) {
                if (error_out) {
                    *error_out = "Invalid --gop value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--duration") {
            const std::string value = consume_value("--duration");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->duration_seconds)) {
                if (error_out) {
                    *error_out = "Invalid --duration value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--list-cameras") {
            options->list_cameras = true;
            continue;
        }
        if (arg == "--experiment-spec") {
            options->experiment_spec_path = consume_value("--experiment-spec");
            if (options->experiment_spec_path.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }

        if (error_out) {
            *error_out = "Unknown argument: " + arg;
        }
        return false;
    }

    return true;
}

std::vector<int> resolve_selected_camera_indices(const CameraParams* cameras_params,
                                                 int num_cameras,
                                                 const HeadlessEncoderSettings& settings)
{
    std::vector<int> indices;
    if (!cameras_params || num_cameras <= 0) {
        return indices;
    }

    if (settings.select_all_cameras || settings.camera_serials.empty()) {
        indices.reserve(num_cameras);
        for (int i = 0; i < num_cameras; ++i) {
            indices.push_back(i);
        }
        return indices;
    }

    std::unordered_map<std::string, int> index_by_serial;
    index_by_serial.reserve(static_cast<std::size_t>(num_cameras));
    for (int i = 0; i < num_cameras; ++i) {
        index_by_serial.emplace(cameras_params[i].camera_serial, i);
    }

    std::unordered_set<int> seen_indices;
    for (const std::string& serial : settings.camera_serials) {
        auto it = index_by_serial.find(serial);
        if (it == index_by_serial.end()) {
            std::ostringstream available;
            for (int i = 0; i < num_cameras; ++i) {
                if (i != 0) {
                    available << ",";
                }
                available << cameras_params[i].camera_serial;
            }
            throw std::runtime_error(
                "Requested camera serial " + serial +
                " was not found on this host. Available serials: " + available.str());
        }
        if (seen_indices.insert(it->second).second) {
            indices.push_back(it->second);
        }
    }

    return indices;
}

void allocate_selected_camera_frame_buffers(CameraEmergent* ecams,
                                            CameraParams* cameras_params,
                                            const std::vector<int>& selected_indices)
{
    for (int idx : selected_indices) {
        camera_open_stream(&ecams[idx].camera, &cameras_params[idx]);
        ecams[idx].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
        allocate_frame_buffer(&ecams[idx].camera, ecams[idx].evt_frame, &cameras_params[idx], evt_buffer_size);
        if (cameras_params[idx].need_reorder && cameras_params[idx].gpu_direct) {
            allocate_frame_reorder_buffer(&ecams[idx].camera, &ecams[idx].frame_reorder, &cameras_params[idx]);
        }
    }
}

void print_available_cameras(GigEVisionDeviceInfo* device_info, int cam_count)
{
    std::cout << "Available cameras: " << cam_count << std::endl;
    for (int i = 0; i < cam_count; ++i) {
        std::cout << "  [" << i << "] serial=" << device_info[i].serialNumber
                  << " ip=" << device_info[i].currentIp
                  << " nic_ip=" << device_info[i].nic.ip4Address
                  << " model=" << device_info[i].modelName
                  << std::endl;
    }
}

void cleanup_selected_camera_buffers(const std::vector<int>& active_camera_indices,
                                     CameraEmergent* ecams,
                                     CameraParams* cameras_params,
                                     std::vector<CameraResources>& camera_resources)
{
    for (int idx : active_camera_indices) {
        if (ecams[idx].evt_frame) {
            destroy_frame_buffer(&ecams[idx].camera, ecams[idx].evt_frame, evt_buffer_size, &cameras_params[idx]);
            delete[] ecams[idx].evt_frame;
            ecams[idx].evt_frame = nullptr;
            check_camera_errors(EVT_CameraCloseStream(&ecams[idx].camera), cameras_params[idx].camera_serial.c_str());
        }
        if (idx >= 0 && idx < static_cast<int>(camera_resources.size())) {
            camera_resources[idx].cleanup();
        }
    }
}

void close_all_cameras(CameraEmergent* ecams,
                       CameraParams* cameras_params,
                       int num_cameras)
{
    for (int i = 0; i < num_cameras; ++i) {
        close_camera(&ecams[i].camera, &cameras_params[i]);
    }
}

void reset_ptp_params(PTPParams* ptp_params)
{
    if (!ptp_params) {
        return;
    }
    ptp_params->ptp_global_time = 0;
    ptp_params->ptp_stop_time = 0;
    ptp_params->ptp_counter = 0;
    ptp_params->ptp_stop_counter = 0;
    ptp_params->network_sync = true;
    ptp_params->network_set_start_ptp = false;
    ptp_params->ptp_stop_reached = false;
    ptp_params->ptp_start_reached = false;
    ptp_params->network_set_stop_ptp = false;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control);

void shutdown_headless_run(std::vector<std::thread>& camera_threads,
                           std::vector<CameraResources>& camera_resources,
                           std::vector<int>& active_camera_indices,
                           std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                           CameraEmergent* ecams,
                           CameraParams* cameras_params,
                           int num_cameras,
                           CameraControl* camera_control,
                           PTPParams* ptp_params,
                           bool reset_ptp_state)
{
    if (camera_control) {
        camera_control->subscribe = false;
    }

    for (auto& t : camera_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    camera_threads.clear();

    if (camera_control) {
        drain_and_shutdown_recording(recording_pipelines, camera_control);
        if (camera_control->sync_camera) {
            for (int idx : active_camera_indices) {
                ptp_sync_off(&ecams[idx].camera, &cameras_params[idx]);
            }
        }
        camera_control->sync_camera = false;
    }
    recording_pipelines.clear();

    cleanup_selected_camera_buffers(active_camera_indices, ecams, cameras_params, camera_resources);
    active_camera_indices.clear();
    camera_resources.clear();

    if (reset_ptp_state) {
        reset_ptp_params(ptp_params);
    }

    close_all_cameras(ecams, cameras_params, num_cameras);
}

RecordingOutputConfig build_native_recording_output_config(const CameraParams& camera_params)
{
    RecordingOutputConfig output;
    output.mode = "factor";
    output.downsample_factor = 1;
    output.requested_width = static_cast<int>(camera_params.width);
    output.requested_height = static_cast<int>(camera_params.height);
    output.resolved_width = static_cast<int>(camera_params.width);
    output.resolved_height = static_cast<int>(camera_params.height);
    output.resize_enabled = false;
    return output;
}

bool prepare_headless_recording_artifacts(const std::string& record_folder,
                                          CameraControl* camera_control,
                                          CameraParams* cameras_params,
                                          int num_cameras,
                                          PTPParams* ptp_params)
{
    if (record_folder.empty()) {
        std::cerr << "Headless recording folder is empty." << std::endl;
        return false;
    }

    const std::filesystem::path recording_path(record_folder);
    std::error_code create_error;
    std::filesystem::create_directories(recording_path, create_error);
    if (create_error && !std::filesystem::exists(recording_path)) {
        std::cerr << "Failed to create recording folder " << record_folder
                  << ": " << create_error.message() << std::endl;
        return false;
    }

    const std::string recording_id = recording_path.filename().string();
    const std::filesystem::path base_path = recording_path.parent_path().empty()
        ? recording_path
        : recording_path.parent_path();

    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->recording_folder = record_folder;
    }

    if (!write_recording_snapshot(
            record_folder,
            recording_id,
            cameras_params,
            num_cameras,
            base_path.string(),
            camera_control->sync_camera,
            ptp_params)) {
        std::cerr << "Failed to write headless recording snapshot for " << record_folder << std::endl;
        return false;
    }

    if (!initialize_ptp_sync_summary(
            record_folder,
            recording_id,
            num_cameras,
            camera_control->sync_camera,
            ptp_params)) {
        std::cerr << "Failed to initialize headless PTP summary for " << record_folder << std::endl;
        return false;
    }

    std::cout << "Recorded video saves to : " << record_folder << std::endl;
    return true;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control)
{
    camera_control->record_video = false;
    camera_control->recording_draining = true;
    camera_control->stop_record = true;

    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (camera_control->active_recorders.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
        usleep(1000);
    }

    if (camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
        std::cerr << "Headless recording drain timed out with "
                  << camera_control->active_recorders.load(std::memory_order_relaxed)
                  << " active recorder(s)." << std::endl;
    }

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->request_stop();
    }

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->shutdown();
        pipeline.reset();
    }

    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    camera_control->recording_folder.clear();
}

} // namespace

void quit_process(bool error = false, const std::string &reason = "")
{
    enet_deinitialize();
    // Show console reason before exit
    if (error)
    {
        std::cout << reason << std::endl;
        system("PAUSE");
        exit(-1);
    }
}

bool open_cameras(CameraParams *cameras_params, CameraEmergent *ecams, CameraEachSelect *cameras_select, GigEVisionDeviceInfo *device_info, int num_cameras, std::string config_folder)
{
    std::vector<std::string> camera_config_files;
    if (!config_folder.empty()) {
        if (!std::filesystem::exists(config_folder)) {
            std::cerr << "Config folder does not exist: " << config_folder << std::endl;
            return false;
        }
        update_camera_configs(camera_config_files, config_folder);
    }

    for (int i = 0; i < num_cameras; i++)
    {
        ecams[i].evt_frame = nullptr;
        set_camera_params(&cameras_params[i], &device_info[i], camera_config_files, i, num_cameras);
        open_camera_with_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id], &cameras_params[i]);
    }
    return true;
}


bool start_camera_thread(std::vector<std::thread> &camera_threads,
    std::vector<CameraResources>& camera_resources,
    std::vector<int>& active_camera_indices,
    std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
    CameraParams *cameras_params, CameraEmergent *ecams, CameraControl *camera_control, CameraEachSelect *cameras_select,
    GigEVisionDeviceInfo *device_info, int num_cameras, PTPParams *ptp_params, std::string record_folder, std::string encoder_basic_setup)
{
    std::cout << "start camera sthread..." << std::endl;
    const HeadlessEncoderSettings encoder_settings = parse_headless_encoder_setup(encoder_basic_setup);
    std::cout << "Headless encoder config: codec=" << encoder_settings.codec
              << " preset=" << encoder_settings.preset
              << " tuning=" << encoder_settings.tuning
              << " rc=" << encoder_settings.rate_control_mode
              << " quality=" << encoder_settings.quality_value
              << " gop=" << encoder_settings.gop_length
              << " cameras=" << format_selected_camera_serials(encoder_settings)
              << std::endl;

    std::vector<int> selected_indices;
    try {
        selected_indices = resolve_selected_camera_indices(cameras_params, num_cameras, encoder_settings);
        if (selected_indices.empty()) {
            throw std::runtime_error("No cameras selected for headless run.");
        }
        allocate_selected_camera_frame_buffers(ecams, cameras_params, selected_indices);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to start thread: " << ex.what() << std::endl;
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        return false;
    }

    camera_control->record_video = true;
    camera_control->subscribe = true;
    int ptp_camera_count = 0;
    for (int idx : selected_indices) {
        if (camera_sync_mode_uses_ptp(&cameras_params[idx])) {
            ptp_camera_count++;
        }
    }
    if (ptp_camera_count != 0 && ptp_camera_count != static_cast<int>(selected_indices.size())) {
        std::cerr << "Headless mode does not support mixed ptp_gate and non-PTP camera sync modes in one run." << std::endl;
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        return false;
    }
    const bool use_ptp_sync = (ptp_camera_count == static_cast<int>(selected_indices.size()) &&
                               !selected_indices.empty());
    camera_control->sync_camera = use_ptp_sync;
    camera_control->recording_draining = false;
    camera_control->stop_record = false;

    std::vector<CameraParams> selected_camera_params;
    selected_camera_params.reserve(selected_indices.size());
    for (int idx : selected_indices) {
        selected_camera_params.push_back(cameras_params[idx]);
    }

    if (!prepare_headless_recording_artifacts(
            record_folder,
            camera_control,
            selected_camera_params.data(),
            static_cast<int>(selected_camera_params.size()),
            ptp_params)) {
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        return false;
    }

    size_t max_frame_size_bytes = 0;
    for (int idx : selected_indices) {
        const size_t current_size =
            static_cast<size_t>(cameras_params[idx].width) * static_cast<size_t>(cameras_params[idx].height);
        if (current_size > max_frame_size_bytes) {
            max_frame_size_bytes = current_size;
        }
    }

    camera_resources.clear();
    camera_resources.resize(num_cameras);
    recording_pipelines.clear();
    recording_pipelines.resize(num_cameras);

    try {
        for (int idx : selected_indices) {
            camera_resources[idx].initialize(
                cameras_params[idx].gpu_id,
                max_frame_size_bytes,
                cameras_select[idx].yolo);
            cameras_select[idx].stream_on = false;
            cameras_select[idx].record = true;
            cameras_select[idx].yolo = false;
            cameras_select[idx].crop_and_encode = false;
            cameras_select[idx].send_frame_ipc = false;

            recording_pipelines[idx] = std::make_unique<ModernRecordingPipeline>(
                &cameras_params[idx],
                build_native_recording_output_config(cameras_params[idx]),
                encoder_settings.codec,
                encoder_settings.preset,
                encoder_settings.tuning,
                encoder_settings.rate_control_mode,
                encoder_settings.quality_value,
                encoder_settings.gop_length,
                record_folder,
                *camera_resources[idx].recycle_queue,
                camera_control);
            recording_pipelines[idx]->start();
        }
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize headless recording pipelines: " << ex.what() << std::endl;
        drain_and_shutdown_recording(recording_pipelines, camera_control);
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        camera_resources.clear();
        return false;
    }

    if (use_ptp_sync) {
        for (int idx : selected_indices)
        {
            ptp_camera_sync(&ecams[idx].camera, &cameras_params[idx]);
        }
        std::cout << "Headless sync mode: ptp_gate" << std::endl;
    } else {
        std::cout << "Headless sync mode: free_run / explicit trigger" << std::endl;
    }

    for (int idx : selected_indices)
    {
        camera_threads.push_back(std::thread(
            &acquire_frames,
            &ecams[idx],
            &cameras_params[idx],
            &cameras_select[idx],
            camera_control,
            ptp_params,
            nullptr,
            nullptr,
            recording_pipelines[idx] ? recording_pipelines[idx]->preprocess_worker() : nullptr,
            nullptr,
            nullptr,
            &camera_resources[idx],
            nullptr));
    }

    // wait for all camera ready
    if (use_ptp_sync) {
        while(ptp_params->ptp_counter != static_cast<int>(selected_indices.size())) {
            usleep(10);
        }
    }

    active_camera_indices = std::move(selected_indices);
    return true;
}

bool quit_server = false;


static void interruptHandler(const int signal)
{
    (void)signal;
    printf("\nQuit Orange.\n");
    quit_server = true;
}

struct ManagerContext
{
    FetchGame::ManagerState state;
    bool quit;
};

struct RecordingContext {
    std::string record_folder;
    std::string encoder_basic_setup;
};

void create_camera_manager(int* cam_count, ManagerContext* manager_context, GigEVisionDeviceInfo* unsorted_device_info, GigEVisionDeviceInfo* device_info, std::string* config_folder, RecordingContext* recording_setup, PTPParams *ptp_params) 
{
    CameraEmergent *ecams = nullptr;
    CameraParams *cameras_params = nullptr;
    std::vector<std::thread> camera_threads;
    std::vector<CameraResources> camera_resources;
    std::vector<int> active_camera_indices;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    CameraEachSelect *cameras_select = nullptr;
    CameraControl *camera_control = new CameraControl;

    manager_context->state = FetchGame::ManagerState_IDLE;
    while(!manager_context->quit) {
        switch (manager_context->state) {
            case FetchGame::ManagerState_CONNECT:
                *cam_count = scan_cameras(max_cameras, unsorted_device_info);
                std::cout << *cam_count << std::endl;
                sort_cameras_ip(unsorted_device_info, device_info, *cam_count);
                manager_context->state = FetchGame::ManagerState_CONNECTED;
                break;
            case FetchGame::ManagerState_OPENCAMERA:
                ecams = new CameraEmergent[*cam_count];
                cameras_params = new CameraParams[*cam_count];
                cameras_select = new CameraEachSelect[*cam_count];
                if (open_cameras(cameras_params, ecams, cameras_select, device_info, *cam_count, *config_folder)) 
                {
                    manager_context->state = FetchGame::ManagerState_CAMERAOPENED;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_STARTCAMTHREAD:
                if (start_camera_thread(camera_threads, camera_resources, active_camera_indices, recording_pipelines, cameras_params, ecams, camera_control, cameras_select, device_info, *cam_count, ptp_params, recording_setup->record_folder, recording_setup->encoder_basic_setup))
                {
                    manager_context->state = FetchGame::ManagerState_THREADREADY;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_ERROR:
                if (ecams && cameras_params) {
                    shutdown_headless_run(
                        camera_threads,
                        camera_resources,
                        active_camera_indices,
                        recording_pipelines,
                        ecams,
                        cameras_params,
                        *cam_count,
                        camera_control,
                        ptp_params,
                        true);
                    delete[] ecams;
                    ecams = nullptr;
                    delete[] cameras_params;
                    cameras_params = nullptr;
                    delete[] cameras_select;
                    cameras_select = nullptr;
                }
                quit_server = true;
                break;
        }

        if (ptp_params->network_set_stop_ptp && ptp_params->ptp_stop_reached) {
            ptp_params->network_set_stop_ptp = false;
            shutdown_headless_run(
                camera_threads,
                camera_resources,
                active_camera_indices,
                recording_pipelines,
                ecams,
                cameras_params,
                *cam_count,
                camera_control,
                ptp_params,
                true);
            delete[] ecams;
            ecams = nullptr;
            delete[] cameras_params;
            cameras_params = nullptr;
            delete[] cameras_select;
            cameras_select = nullptr;
            manager_context->state = FetchGame::ManagerState_RECORDSTOPPED;
        }
        usleep(1000);
    }

    delete camera_control;
}

bool validate_headless_cli_options(const HeadlessCliOptions& options, std::string* error_out)
{
    if (options.mode == HeadlessMode::Remote) {
        if (!options.record_folder.empty() || !options.config_folder.empty() ||
            options.list_cameras || !options.experiment_spec_path.empty() ||
            options.duration_seconds > 0 || !options.encoder_settings.select_all_cameras ||
            !options.encoder_settings.camera_serials.empty()) {
            if (error_out) {
                *error_out =
                    "Direct run flags are only supported with --mode local. "
                    "Remote mode still uses the network control path.";
            }
            return false;
        }
        return true;
    }

    if (!options.experiment_spec_path.empty()) {
        if (error_out) {
            *error_out =
                "--experiment-spec is not implemented yet. Use direct local flags for now.";
        }
        return false;
    }

    if (!options.list_cameras && options.record_folder.empty()) {
        if (error_out) {
            *error_out = "--record-folder is required in --mode local unless using --list-cameras.";
        }
        return false;
    }

    return true;
}

int run_local_mode(const HeadlessCliOptions& options)
{
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    const int cam_count = scan_cameras(max_cameras, unsorted_device_info);
    if (cam_count <= 0) {
        std::cerr << "No cameras found." << std::endl;
        return 1;
    }

    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);
    print_available_cameras(device_info, cam_count);

    if (options.list_cameras) {
        return 0;
    }

    std::unique_ptr<CameraEmergent[]> ecams(new CameraEmergent[cam_count]);
    std::unique_ptr<CameraParams[]> cameras_params(new CameraParams[cam_count]);
    std::unique_ptr<CameraEachSelect[]> cameras_select(new CameraEachSelect[cam_count]);

    if (!open_cameras(cameras_params.get(), ecams.get(), cameras_select.get(), device_info, cam_count, options.config_folder)) {
        std::cerr << "Failed to open cameras." << std::endl;
        return 1;
    }

    CameraControl camera_control;
    PTPParams ptp_params{};
    std::vector<std::thread> camera_threads;
    std::vector<CameraResources> camera_resources;
    std::vector<int> active_camera_indices;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;

    const std::string encoder_setup = build_headless_encoder_setup_string(options.encoder_settings);
    const bool started = start_camera_thread(
        camera_threads,
        camera_resources,
        active_camera_indices,
        recording_pipelines,
        cameras_params.get(),
        ecams.get(),
        &camera_control,
        cameras_select.get(),
        device_info,
        cam_count,
        &ptp_params,
        options.record_folder,
        encoder_setup);

    if (!started) {
        close_all_cameras(ecams.get(), cameras_params.get(), cam_count);
        return 1;
    }

    std::cout << "Local headless run started."
              << " folder=" << options.record_folder
              << " cameras=" << format_selected_camera_serials(options.encoder_settings)
              << std::endl;

    const auto deadline = (options.duration_seconds > 0)
        ? std::chrono::steady_clock::now() + std::chrono::seconds(options.duration_seconds)
        : std::chrono::steady_clock::time_point::max();

    while (!quit_server && std::chrono::steady_clock::now() < deadline) {
        usleep(100000);
    }

    shutdown_headless_run(
        camera_threads,
        camera_resources,
        active_camera_indices,
        recording_pipelines,
        ecams.get(),
        cameras_params.get(),
        cam_count,
        &camera_control,
        &ptp_params,
        false);

    return 0;
}

int run_remote_mode()
{
    if (enet_initialize() != 0)
    {
        quit_process(true, "ENET failed to initialize!");
    }

    EnetContext client;
    if (enet_initialize(&client, 3333, 1))
    {
        printf("Network Initialized!\n");
    }

    f32 last_time = tick();
    f32 current_time = tick();

    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);
    std::cout << "available no of cameras: " << cam_count << std::endl;

    flatbuffers::FlatBufferBuilder* fb_builder = new flatbuffers::FlatBufferBuilder(1024);
    std::string config_folder;
    RecordingContext recording_setup;
    ManagerContext manager_context{FetchGame::ManagerState_IDLE, false};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, true, false, false, false};

    std::thread* manager_thread = new std::thread(&create_camera_manager, &cam_count, &manager_context, unsorted_device_info, device_info, &config_folder, &recording_setup, ptp_params);
    
    while (!quit_server)
    {
        current_time = tick();
        // Handle All Incoming Packets and Send any enqued packets, does this need to be on another thread?
        service_network(&client, current_time - last_time, [&](const ENetEvent &evnt)
        {
            switch (evnt.type) 
            {
                //New connection request or an existing peer accepted our connection request
                case ENET_EVENT_TYPE_CONNECT:
                    {
                        if (manager_context.state == FetchGame::ManagerState_IDLE) {
                            printf("Network: Successfully connected! Rescaning cameras. \n");
                            manager_context.state = FetchGame::ManagerState_CONNECT; // rescan number of cams
                        } else {
                            printf("Network: Successfully connected! \n");
                            client_send_bringup_message(&client, fb_builder, evnt.peer, cam_count, manager_context.state);
                        }
                    }
                    break;
                //Server has sent us a new packet
                case ENET_EVENT_TYPE_RECEIVE:
                    {
                        printf ("\n A packet of length %u was received from %s on channel %u.\n",
                            evnt.packet -> dataLength,
                            evnt.peer -> data,
                            evnt.channelID);

                        uint8_t* buffer_pointer = evnt.packet->data;
                        auto server_control = FetchGame::GetServer(buffer_pointer);
                        auto server_signal = server_control->control();

                        if (server_signal == FetchGame::ServerControl_OPENCAMERA) {
                            config_folder = server_control->config_folder()->c_str();
                            manager_context.state = FetchGame::ManagerState_OPENCAMERA;
                        }
                        else if (server_signal == FetchGame::ServerControl_STARTTHREAD)
                        {
                            recording_setup.record_folder = server_control->record_folder()->c_str();
                            recording_setup.encoder_basic_setup = server_control->encoder_setup()->c_str();
                            manager_context.state = FetchGame::ManagerState_STARTCAMTHREAD;
                        } else if (server_signal == FetchGame::ServerControl_QUIT) {
                            printf("Exit \n");
                            quit_server = true;
                        } else if (server_signal == FetchGame::ServerControl_STARTRECORDING) {
                            ptp_params->ptp_global_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_global_time << std::endl;
                            ptp_params->network_set_start_ptp = true;
                            manager_context.state = FetchGame::ManagerState_WAITSTOP;
                            client_send_state_update_message(&client, fb_builder, evnt.peer, manager_context.state);
                        } else if (server_signal == FetchGame::ServerControl_STOPRECORDING) {
                            // stop recording
                            printf("stop signal\n");
                            std::cout << server_control->ptp_global_time() << std::endl;
                            ptp_params->ptp_stop_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_stop_time << std::endl;
                            ptp_params->network_set_stop_ptp = true;
                        }
                        enet_packet_destroy(evnt.packet);
                    }
                    break;

                //Server has disconnected
                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("Network: Server has disconnected!\n");
                    break;
            } });

        // coordinate with other thread
        if (manager_context.state == FetchGame::ManagerState_CONNECTED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_bringup_message(&client, fb_builder, &client.m_pNetwork->peers[0], cam_count, manager_context.state);
        }
        if (manager_context.state == FetchGame::ManagerState_CAMERAOPENED) {
            manager_context.state = FetchGame::ManagerState_WAITTHREAD;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_THREADREADY)
        {
            manager_context.state = FetchGame::ManagerState_WAITSTART;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_RECORDSTOPPED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        }
    
        usleep(1000);
        last_time = current_time;
    }

    manager_context.quit = true;
    manager_thread->join();

    // Disconnect
    enet_peer_disconnect(&client.m_pNetwork->peers[0], 0);
    uint8_t disconnected = false;
    /* Allow up to 3 seconds for the disconnect to succeed
     * and drop any packets received packets.
     */
    ENetEvent evnt;
    while (enet_host_service(client.m_pNetwork, &evnt, 3000) > 0)
    {
        switch (evnt.type)
        {
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(evnt.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            puts("Disconnection succeeded.");
            disconnected = true;
            break;
        }
    }
    

    // Drop connection, since disconnection didn't successed
    if (!disconnected)
    {
        enet_peer_reset(&client.m_pNetwork->peers[0]);
    }
    enet_host_destroy(client.m_pNetwork);
    delete manager_thread;
    delete ptp_params;
    delete fb_builder;
    enet_deinitialize();
    return 0;
}


int main(int argc, char *argv[])
{
    signal(SIGINT, interruptHandler);
    HeadlessCliOptions options;
    std::string parse_error;
    if (!parse_headless_cli_options(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << std::endl;
        print_headless_usage(argv[0]);
        return 2;
    }

    if (options.show_help) {
        print_headless_usage(argv[0]);
        return 0;
    }

    std::string validation_error;
    if (!validate_headless_cli_options(options, &validation_error)) {
        std::cerr << validation_error << std::endl;
        if (options.mode == HeadlessMode::Local) {
            print_headless_usage(argv[0]);
        }
        return 2;
    }

    if (options.mode == HeadlessMode::Local) {
        return run_local_mode(options);
    }

    return run_remote_mode();
}
