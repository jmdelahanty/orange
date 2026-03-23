#include "aperture_characterization.h"
#include "camera.h"
#include "project.h"

#include <EvtParamAttribute.h>
#include <emergenterrors.h>
#include <gigevisiondeviceinfo.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace Emergent;

namespace {

struct Options {
    std::string serial;
    int index = 0;
    bool index_set = false;
    bool list_only = false;

    std::string output_dir = ".";
    std::string output_prefix = "aperture_characterization";

    bool exposure_set = false;
    unsigned int exposure = 0;
    bool gain_set = false;
    unsigned int gain = 0;
    bool frame_rate_set = false;
    unsigned int frame_rate = 0;
    bool focus_set = false;
    unsigned int focus = 0;
    bool pixel_format_set = false;
    std::string pixel_format = "Mono8";
    bool focus_uart_bootstrap = false;

    std::vector<unsigned int> iris_values;
    bool iris_start_set = false;
    unsigned int iris_start = 0;
    bool iris_stop_set = false;
    unsigned int iris_stop = 0;
    unsigned int iris_step_multiple = 1;

    unsigned int frames_per_step = 3;
    unsigned int settle_frames = 30;
    unsigned int buffer_count = 4;
    unsigned int grab_timeout_ms = 1000;
    unsigned int grid_rows = 8;
    unsigned int grid_cols = 8;
    bool restore_original_iris = true;
    bool save_representative_frames = true;

    bool has_reference_iris = false;
    unsigned int reference_iris = 0;
    bool has_reference_f_number = false;
    double reference_f_number = 0.0;

    ApertureCharacterizationThresholds thresholds;
    bool show_help = false;
};

struct CameraGuard {
    CEmergentCamera camera;
    CameraParams* params = nullptr;
    bool opened = false;

    ~CameraGuard()
    {
        if (opened && params != nullptr) {
            try {
                close_camera(&camera, params);
            } catch (...) {
            }
        }
    }
};

bool parse_int_arg(const std::string& s, int* out)
{
    try {
        size_t consumed = 0;
        int v = std::stoi(s, &consumed, 10);
        if (consumed != s.size()) {
            return false;
        }
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_uint_arg(const std::string& s, unsigned int* out)
{
    try {
        size_t consumed = 0;
        unsigned long v = std::stoul(s, &consumed, 10);
        if (consumed != s.size() || v > std::numeric_limits<unsigned int>::max()) {
            return false;
        }
        *out = static_cast<unsigned int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double_arg(const std::string& s, double* out)
{
    try {
        size_t consumed = 0;
        double v = std::stod(s, &consumed);
        if (consumed != s.size()) {
            return false;
        }
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::string trim_copy(const std::string& s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

bool parse_uint_csv(const std::string& s, std::vector<unsigned int>* out)
{
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const std::string trimmed = trim_copy(item);
        if (trimmed.empty()) {
            continue;
        }
        unsigned int value = 0;
        if (!parse_uint_arg(trimmed, &value)) {
            return false;
        }
        out->push_back(value);
    }
    return !out->empty();
}

bool get_enum_value(CEmergentCamera* camera, const char* name, std::string* out_value)
{
    std::vector<char> buffer(4096, '\0');
    unsigned long value_size = 0;
    EVT_ERROR err = EVT_CameraGetEnumParam(camera, name, buffer.data(), buffer.size(), &value_size);
    if (err != EVT_SUCCESS) {
        return false;
    }
    *out_value = trim_copy(std::string(buffer.data()));
    return true;
}

bool get_string_value(CEmergentCamera* camera, const char* name, std::string* out_value)
{
    int max_length = 0;
    EVT_ERROR len_err = EVT_CameraGetStringParamMaxLength(camera, name, &max_length);
    if (len_err != EVT_SUCCESS || max_length <= 0) {
        max_length = 512;
    }

    const unsigned long buf_size = static_cast<unsigned long>(std::max(256, max_length + 8));
    std::vector<char> buffer(buf_size, '\0');
    unsigned long value_size = 0;
    EVT_ERROR err = EVT_CameraGetStringParam(camera, name, buffer.data(), buf_size, &value_size, 0);
    if (err != EVT_SUCCESS) {
        return false;
    }
    *out_value = trim_copy(std::string(buffer.data()));
    return true;
}

void print_usage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --serial <serial>                  Select camera by serial number.\n"
        << "  --index <n>                        Select camera by discovery index (default 0).\n"
        << "  --list-only                        Only list discovered cameras.\n"
        << "  --output-dir <path>                Directory for JSON/CSV artifacts (default .).\n"
        << "  --output-prefix <name>             Prefix for artifact filenames.\n"
        << "  --exposure <value>                 Override Exposure before the sweep.\n"
        << "  --gain <value>                     Override Gain before the sweep.\n"
        << "  --frame-rate <value>               Override FrameRate before the sweep.\n"
        << "  --focus <value>                    Override Focus before the sweep.\n"
        << "  --pixel-format <enum>              Override PixelFormat before the sweep (default Mono8 if set).\n"
        << "  --focus-uart-bootstrap             Enable UART bootstrap fallback while setting focus.\n"
        << "  --iris-values <csv>                Explicit iris list, e.g. 0,4,8,12.\n"
        << "  --iris-start <value>               Start iris for generated sweep.\n"
        << "  --iris-stop <value>                End iris for generated sweep.\n"
        << "  --iris-step-multiple <n>           Multiply camera iris increment for generated sweep.\n"
        << "  --frames-per-step <n>              Measurement frames per iris value (default 3).\n"
        << "  --settle-frames <n>                Frames to discard after each iris change (default 30).\n"
        << "  --buffer-count <n>                 Stream buffer count (default 4).\n"
        << "  --grab-timeout-ms <ms>             Frame grab timeout (default 1000).\n"
        << "  --grid-rows <n>                    Brightness-grid rows (default 8, 0 disables).\n"
        << "  --grid-cols <n>                    Brightness-grid cols (default 8, 0 disables).\n"
        << "  --no-restore-original-iris         Leave the final iris in place.\n"
        << "  --no-save-representative-frames    Skip per-iris saved representative frames.\n"
        << "  --reference-iris <value>           Explicit normalization reference iris.\n"
        << "  --reference-f-number <value>       Optional f-number at the reference iris.\n"
        << "  --saturated-white-fraction <f>     Saturation threshold on pure white fraction.\n"
        << "  --saturated-p99-min <value>        Saturation threshold on p99 brightness.\n"
        << "  --dim-mean-max <value>             Too-dim threshold on mean brightness.\n"
        << "  --dim-p95-max <value>              Too-dim threshold on p95 brightness.\n"
        << "  --dim-black-fraction-min <f>       Too-dim threshold on black fraction.\n"
        << "  --help                             Show this message.\n";
}

bool parse_args(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_next = [&](const char* flag_name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag_name << "\n";
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (arg == "--help" || arg == "-h") {
            options->show_help = true;
            return true;
        } else if (arg == "--serial") {
            const char* v = require_next("--serial");
            if (!v) return false;
            options->serial = v;
        } else if (arg == "--index") {
            const char* v = require_next("--index");
            if (!v) return false;
            int parsed = 0;
            if (!parse_int_arg(v, &parsed) || parsed < 0) {
                std::cerr << "Invalid --index value: " << v << "\n";
                return false;
            }
            options->index = parsed;
            options->index_set = true;
        } else if (arg == "--list-only") {
            options->list_only = true;
        } else if (arg == "--output-dir") {
            const char* v = require_next("--output-dir");
            if (!v) return false;
            options->output_dir = v;
        } else if (arg == "--output-prefix") {
            const char* v = require_next("--output-prefix");
            if (!v) return false;
            options->output_prefix = v;
        } else if (arg == "--exposure") {
            const char* v = require_next("--exposure");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->exposure)) {
                std::cerr << "Invalid --exposure value: " << v << "\n";
                return false;
            }
            options->exposure_set = true;
        } else if (arg == "--gain") {
            const char* v = require_next("--gain");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->gain)) {
                std::cerr << "Invalid --gain value: " << v << "\n";
                return false;
            }
            options->gain_set = true;
        } else if (arg == "--frame-rate") {
            const char* v = require_next("--frame-rate");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->frame_rate)) {
                std::cerr << "Invalid --frame-rate value: " << v << "\n";
                return false;
            }
            options->frame_rate_set = true;
        } else if (arg == "--focus") {
            const char* v = require_next("--focus");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->focus)) {
                std::cerr << "Invalid --focus value: " << v << "\n";
                return false;
            }
            options->focus_set = true;
        } else if (arg == "--pixel-format") {
            const char* v = require_next("--pixel-format");
            if (!v) return false;
            options->pixel_format = v;
            options->pixel_format_set = true;
        } else if (arg == "--focus-uart-bootstrap") {
            options->focus_uart_bootstrap = true;
        } else if (arg == "--iris-values") {
            const char* v = require_next("--iris-values");
            if (!v) return false;
            options->iris_values.clear();
            if (!parse_uint_csv(v, &options->iris_values)) {
                std::cerr << "Invalid --iris-values CSV: " << v << "\n";
                return false;
            }
        } else if (arg == "--iris-start") {
            const char* v = require_next("--iris-start");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->iris_start)) {
                std::cerr << "Invalid --iris-start value: " << v << "\n";
                return false;
            }
            options->iris_start_set = true;
        } else if (arg == "--iris-stop") {
            const char* v = require_next("--iris-stop");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->iris_stop)) {
                std::cerr << "Invalid --iris-stop value: " << v << "\n";
                return false;
            }
            options->iris_stop_set = true;
        } else if (arg == "--iris-step-multiple") {
            const char* v = require_next("--iris-step-multiple");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->iris_step_multiple) || options->iris_step_multiple == 0) {
                std::cerr << "Invalid --iris-step-multiple value: " << v << "\n";
                return false;
            }
        } else if (arg == "--frames-per-step") {
            const char* v = require_next("--frames-per-step");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->frames_per_step) || options->frames_per_step == 0) {
                std::cerr << "Invalid --frames-per-step value: " << v << "\n";
                return false;
            }
        } else if (arg == "--settle-frames") {
            const char* v = require_next("--settle-frames");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->settle_frames)) {
                std::cerr << "Invalid --settle-frames value: " << v << "\n";
                return false;
            }
        } else if (arg == "--buffer-count") {
            const char* v = require_next("--buffer-count");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->buffer_count) || options->buffer_count == 0) {
                std::cerr << "Invalid --buffer-count value: " << v << "\n";
                return false;
            }
        } else if (arg == "--grab-timeout-ms") {
            const char* v = require_next("--grab-timeout-ms");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->grab_timeout_ms) || options->grab_timeout_ms == 0) {
                std::cerr << "Invalid --grab-timeout-ms value: " << v << "\n";
                return false;
            }
        } else if (arg == "--grid-rows") {
            const char* v = require_next("--grid-rows");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->grid_rows)) {
                std::cerr << "Invalid --grid-rows value: " << v << "\n";
                return false;
            }
        } else if (arg == "--grid-cols") {
            const char* v = require_next("--grid-cols");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->grid_cols)) {
                std::cerr << "Invalid --grid-cols value: " << v << "\n";
                return false;
            }
        } else if (arg == "--no-restore-original-iris") {
            options->restore_original_iris = false;
        } else if (arg == "--no-save-representative-frames") {
            options->save_representative_frames = false;
        } else if (arg == "--reference-iris") {
            const char* v = require_next("--reference-iris");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->reference_iris)) {
                std::cerr << "Invalid --reference-iris value: " << v << "\n";
                return false;
            }
            options->has_reference_iris = true;
        } else if (arg == "--reference-f-number") {
            const char* v = require_next("--reference-f-number");
            if (!v) return false;
            if (!parse_double_arg(v, &options->reference_f_number) || options->reference_f_number <= 0.0) {
                std::cerr << "Invalid --reference-f-number value: " << v << "\n";
                return false;
            }
            options->has_reference_f_number = true;
        } else if (arg == "--saturated-white-fraction") {
            const char* v = require_next("--saturated-white-fraction");
            if (!v) return false;
            if (!parse_double_arg(v, &options->thresholds.saturated_white_fraction) ||
                options->thresholds.saturated_white_fraction < 0.0) {
                std::cerr << "Invalid --saturated-white-fraction value: " << v << "\n";
                return false;
            }
        } else if (arg == "--saturated-p99-min") {
            const char* v = require_next("--saturated-p99-min");
            if (!v) return false;
            if (!parse_double_arg(v, &options->thresholds.saturated_p99_min)) {
                std::cerr << "Invalid --saturated-p99-min value: " << v << "\n";
                return false;
            }
        } else if (arg == "--dim-mean-max") {
            const char* v = require_next("--dim-mean-max");
            if (!v) return false;
            if (!parse_double_arg(v, &options->thresholds.dim_mean_max)) {
                std::cerr << "Invalid --dim-mean-max value: " << v << "\n";
                return false;
            }
        } else if (arg == "--dim-p95-max") {
            const char* v = require_next("--dim-p95-max");
            if (!v) return false;
            if (!parse_double_arg(v, &options->thresholds.dim_p95_max)) {
                std::cerr << "Invalid --dim-p95-max value: " << v << "\n";
                return false;
            }
        } else if (arg == "--dim-black-fraction-min") {
            const char* v = require_next("--dim-black-fraction-min");
            if (!v) return false;
            if (!parse_double_arg(v, &options->thresholds.dim_black_fraction_min) ||
                options->thresholds.dim_black_fraction_min < 0.0) {
                std::cerr << "Invalid --dim-black-fraction-min value: " << v << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }
    }

    if ((options->grid_rows == 0) != (options->grid_cols == 0)) {
        std::cerr << "--grid-rows and --grid-cols must both be zero or both be greater than zero.\n";
        return false;
    }

    return true;
}

std::vector<GigEVisionDeviceInfo> list_devices()
{
    std::vector<GigEVisionDeviceInfo> devices(32);
    const int count = scan_cameras(static_cast<int>(devices.size()), devices.data());
    devices.resize(std::max(0, count));
    return devices;
}

void print_devices(const std::vector<GigEVisionDeviceInfo>& devices)
{
    std::cout << "\n[Discovered Cameras]\n";
    if (devices.empty()) {
        std::cout << "  none\n";
        return;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  [" << i << "]"
                  << " serial=" << devices[i].serialNumber
                  << " model=" << devices[i].modelName
                  << " ip=" << devices[i].currentIp
                  << " nic=" << devices[i].nic.ip4Address << "\n";
    }
}

int select_camera_index(const std::vector<GigEVisionDeviceInfo>& devices, const Options& options)
{
    if (!options.serial.empty()) {
        for (size_t i = 0; i < devices.size(); ++i) {
            if (options.serial == devices[i].serialNumber) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    if (devices.empty()) {
        return -1;
    }

    if (options.index_set) {
        return options.index >= 0 && options.index < static_cast<int>(devices.size()) ? options.index : -1;
    }
    return 0;
}

void require_success(EVT_ERROR err, const char* serial, const char* op)
{
    if (err != EVT_SUCCESS) {
        std::cerr << op << " failed: " << get_evt_error_string(err) << "\n";
        check_camera_errors(err, serial);
    }
}

void populate_camera_params(CEmergentCamera* camera, CameraParams* camera_params, std::string* lens_name)
{
    require_success(EVT_CameraGetUInt32Param(camera, "Width", &camera_params->width), camera_params->camera_serial.c_str(), "Width");
    require_success(EVT_CameraGetUInt32Param(camera, "Height", &camera_params->height), camera_params->camera_serial.c_str(), "Height");
    require_success(EVT_CameraGetUInt32Param(camera, "FrameRate", &camera_params->frame_rate), camera_params->camera_serial.c_str(), "FrameRate");
    require_success(EVT_CameraGetUInt32Param(camera, "Exposure", &camera_params->exposure), camera_params->camera_serial.c_str(), "Exposure");
    require_success(EVT_CameraGetUInt32Param(camera, "Gain", &camera_params->gain), camera_params->camera_serial.c_str(), "Gain");
    require_success(EVT_CameraGetUInt32Param(camera, "Iris", &camera_params->iris), camera_params->camera_serial.c_str(), "Iris");
    require_success(EVT_CameraGetUInt32Param(camera, "Focus", &camera_params->focus), camera_params->camera_serial.c_str(), "Focus");

    EVT_CameraGetUInt32ParamMin(camera, "Iris", &camera_params->iris_min);
    EVT_CameraGetUInt32ParamMax(camera, "Iris", &camera_params->iris_max);
    EVT_CameraGetUInt32ParamInc(camera, "Iris", &camera_params->iris_inc);
    EVT_CameraGetUInt32ParamMin(camera, "Focus", &camera_params->focus_min);
    EVT_CameraGetUInt32ParamMax(camera, "Focus", &camera_params->focus_max);
    EVT_CameraGetUInt32ParamInc(camera, "Focus", &camera_params->focus_inc);
    EVT_CameraGetUInt32ParamMin(camera, "Gain", &camera_params->gain_min);
    EVT_CameraGetUInt32ParamMax(camera, "Gain", &camera_params->gain_max);
    EVT_CameraGetUInt32ParamInc(camera, "Gain", &camera_params->gain_inc);
    EVT_CameraGetUInt32ParamMin(camera, "Exposure", &camera_params->exposure_min);
    EVT_CameraGetUInt32ParamMax(camera, "Exposure", &camera_params->exposure_max);
    EVT_CameraGetUInt32ParamInc(camera, "Exposure", &camera_params->exposure_inc);
    EVT_CameraGetUInt32ParamMin(camera, "FrameRate", &camera_params->frame_rate_min);
    EVT_CameraGetUInt32ParamMax(camera, "FrameRate", &camera_params->frame_rate_max);
    EVT_CameraGetUInt32ParamInc(camera, "FrameRate", &camera_params->frame_rate_inc);

    if (!get_enum_value(camera, "PixelFormat", &camera_params->pixel_format)) {
        camera_params->pixel_format = "Mono8";
    }
    camera_params->color = camera_params->pixel_format != "Mono8";

    if (lens_name != nullptr) {
        if (!get_string_value(camera, "LensName", lens_name)) {
            *lens_name = "";
        }
    }
}

void apply_overrides(CEmergentCamera* camera, CameraParams* camera_params, const Options& options)
{
    camera_params->focus_uart_bootstrap = options.focus_uart_bootstrap;

    if (options.pixel_format_set) {
        require_success(
            EVT_CameraSetEnumParam(camera, "PixelFormat", options.pixel_format.c_str()),
            camera_params->camera_serial.c_str(),
            "PixelFormat");
        camera_params->pixel_format = options.pixel_format;
        camera_params->color = camera_params->pixel_format != "Mono8";
    }
    if (options.gain_set) {
        update_gain_value(camera, static_cast<int>(options.gain), camera_params);
    }
    if (options.exposure_set) {
        update_exposure_value(camera, static_cast<int>(options.exposure), camera_params);
    }
    if (options.frame_rate_set) {
        update_frame_rate_value(camera, static_cast<int>(options.frame_rate), camera_params);
    }
    if (options.focus_set) {
        update_focus_value(camera, static_cast<int>(options.focus), camera_params);
    }

    populate_camera_params(camera, camera_params, nullptr);
}

std::vector<unsigned int> build_requested_iris_values(const Options& options, const CameraParams& camera_params)
{
    if (!options.iris_values.empty()) {
        return options.iris_values;
    }
    const unsigned int iris_start = options.iris_start_set ? options.iris_start : camera_params.iris_min;
    const unsigned int iris_stop = options.iris_stop_set ? options.iris_stop : camera_params.iris_max;
    return build_iris_sweep(iris_start, iris_stop, camera_params.iris_inc, options.iris_step_multiple);
}

std::string get_current_utc_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time = *std::gmtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string get_current_date_time()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y_%m_%d_%H_%M_%S");
    return oss.str();
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_args(argc, argv, &options)) {
        return EXIT_FAILURE;
    }
    if (options.show_help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    try {
        std::vector<GigEVisionDeviceInfo> devices = list_devices();
        print_devices(devices);
        if (options.list_only) {
            return EXIT_SUCCESS;
        }

        const int selected_index = select_camera_index(devices, options);
        if (selected_index < 0) {
            std::cerr << "Unable to select a camera from discovery results.\n";
            return EXIT_FAILURE;
        }

        const GigEVisionDeviceInfo& selected = devices[selected_index];
        std::cout << "\n[Selected Camera]\n"
                  << "  index=" << selected_index
                  << " serial=" << selected.serialNumber
                  << " model=" << selected.modelName
                  << " ip=" << selected.currentIp << "\n";

        CameraParams camera_params{};
        camera_params.camera_serial = selected.serialNumber;
        camera_params.camera_name = selected.modelName;
        camera_params.gpu_direct = false;
        camera_params.focus_uart_bootstrap = options.focus_uart_bootstrap;

        CameraGuard camera_guard;
        camera_guard.params = &camera_params;
        require_success(EVT_CameraOpen(&camera_guard.camera, &selected), camera_params.camera_serial.c_str(), "EVT_CameraOpen");
        camera_guard.opened = true;
        std::cout << "Camera opened successfully.\n";

        std::string lens_name;
        populate_camera_params(&camera_guard.camera, &camera_params, &lens_name);
        apply_overrides(&camera_guard.camera, &camera_params, options);

        const std::vector<unsigned int> iris_values = build_requested_iris_values(options, camera_params);
        if (iris_values.empty()) {
            std::cerr << "No iris values were generated for the sweep.\n";
            return EXIT_FAILURE;
        }

        for (unsigned int iris_value : iris_values) {
            if (iris_value < camera_params.iris_min || iris_value > camera_params.iris_max) {
                std::cerr << "Requested iris value " << iris_value
                          << " is outside the camera range ["
                          << camera_params.iris_min << "," << camera_params.iris_max << "].\n";
                return EXIT_FAILURE;
            }
        }

        const std::string timestamp = ::get_current_date_time();
        const std::string created_utc = ::get_current_utc_timestamp();
        const std::string artifact_id =
            build_aperture_characterization_artifact_id(options.output_prefix, camera_params, timestamp);
        const ApertureCharacterizationArtifactPaths artifact_paths =
            make_aperture_characterization_artifact_paths(options.output_dir, artifact_id);
        std::filesystem::create_directories(artifact_paths.artifact_dir);

        ApertureCharacterizationRequest request;
        request.iris_values = iris_values;
        request.frames_per_step = options.frames_per_step;
        request.settle_frames = options.settle_frames;
        request.grab_timeout_ms = options.grab_timeout_ms;
        request.grid_rows = options.grid_rows;
        request.grid_cols = options.grid_cols;
        request.manage_acquisition = true;
        request.restore_original_iris = options.restore_original_iris;
        request.has_reference_iris = options.has_reference_iris;
        request.reference_iris = options.reference_iris;
        request.has_reference_f_number = options.has_reference_f_number;
        request.reference_f_number = options.reference_f_number;
        request.thresholds = options.thresholds;
        request.save_representative_frames = options.save_representative_frames;
        request.representative_frame_dir = artifact_paths.representative_frames_dir;
        request.representative_frame_prefix = artifact_id;

        const ApertureCharacterizationResult result =
            characterize_aperture_with_stream(&camera_guard.camera, &camera_params, request, options.buffer_count);

        std::string error;
        const nlohmann::json measurement_json =
            aperture_characterization_to_json(
                result,
                request,
                camera_params,
                lens_name,
                artifact_id,
                created_utc,
                "",
                artifact_paths);
        const std::string fingerprint =
            compute_aperture_characterization_fingerprint(measurement_json, artifact_paths, &error);
        if (fingerprint.empty()) {
            std::cerr << (error.empty() ? "Failed to compute aperture artifact fingerprint." : error) << "\n";
            return EXIT_FAILURE;
        }
        const nlohmann::json measurement_json_with_fingerprint =
            aperture_characterization_to_json(
                result,
                request,
                camera_params,
                lens_name,
                artifact_id,
                created_utc,
                fingerprint,
                artifact_paths);
        const nlohmann::json manifest_json =
            aperture_characterization_manifest_to_json(
                result,
                request,
                camera_params,
                lens_name,
                artifact_id,
                created_utc,
                fingerprint,
                artifact_paths);
        if (!write_aperture_characterization_json(artifact_paths.manifest_path, manifest_json, &error)) {
            std::cerr << error << "\n";
            return EXIT_FAILURE;
        }
        if (!write_aperture_characterization_json(artifact_paths.measurement_json_path, measurement_json_with_fingerprint, &error)) {
            std::cerr << error << "\n";
            return EXIT_FAILURE;
        }
        if (!write_aperture_characterization_step_csv(artifact_paths.steps_csv_path, result, artifact_paths, &error)) {
            std::cerr << error << "\n";
            return EXIT_FAILURE;
        }
        if (!write_aperture_characterization_frame_csv(artifact_paths.frames_csv_path, result, &error)) {
            std::cerr << error << "\n";
            return EXIT_FAILURE;
        }
        if (!update_calibration_artifact_registry(options.output_dir, manifest_json, &error)) {
            std::cerr << error << "\n";
            return EXIT_FAILURE;
        }

        std::cout << "\n[Summary]\n"
                  << "  artifact_id=" << artifact_id << "\n"
                  << "  artifact_dir=" << artifact_paths.artifact_dir << "\n"
                  << "  fingerprint=" << fingerprint << "\n"
                  << "  manifest_json=" << artifact_paths.manifest_path << "\n"
                  << "  measurement_json=" << artifact_paths.measurement_json_path << "\n"
                  << "  output_steps_csv=" << artifact_paths.steps_csv_path << "\n"
                  << "  output_frames_csv=" << artifact_paths.frames_csv_path << "\n"
                  << "  reference_iris=" << (result.has_reference_iris ? std::to_string(result.reference_iris) : "none")
                  << " reference_mean=" << result.reference_mean << "\n";
        if (options.save_representative_frames) {
            std::cout << "  representative_frames_dir=" << artifact_paths.representative_frames_dir << "\n";
        }
        if (result.has_saturation_boundary) {
            std::cout << "  saturation_limited_through_iris=" << result.saturation_limited_through_iris << "\n";
        }
        if (result.has_usable_window) {
            std::cout << "  usable_iris_range=[" << result.usable_iris_min << "," << result.usable_iris_max << "]\n";
        }
        if (result.has_dim_boundary) {
            std::cout << "  dim_limited_from_iris=" << result.dim_limited_from_iris << "\n";
        }
        for (const std::string& warning : result.warnings) {
            std::cout << "  warning=" << warning << "\n";
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Fatal: unknown error\n";
        return EXIT_FAILURE;
    }
}
