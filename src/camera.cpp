#include "camera.h"
#include <iostream>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>
#include <cuda_runtime_api.h>

namespace {
constexpr useconds_t kFocusPollIntervalUs = 200 * 1000;  // 200ms
constexpr int kFocusPollAttemptsBeforeUart = 5;          // 1s total
constexpr int kFocusPollAttemptsAfterUart = 10;          // 2s total
constexpr useconds_t kIrisPrimeSettleUs = 150 * 1000;    // 150ms

bool has_param(Emergent::CEmergentCamera* camera, const char* name)
{
    EvtParamAttribute attr{};
    return EVT_CameraGetParamAttr(camera, name, &attr) == EVT_SUCCESS;
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_sync_mode(const CameraParams* camera_params)
{
    if (!camera_params) {
        return "free_run";
    }

    const std::string mode = lower_ascii(camera_params->sync_mode);
    if (mode == "ptp_gate" || mode == "free_run" || mode == "external_trigger" || mode == "software_trigger") {
        return mode;
    }
    return "free_run";
}

std::string normalize_gpio_connector_variant(const CameraParams* camera_params)
{
    if (!camera_params) {
        return "unknown";
    }
    const std::string value = lower_ascii(camera_params->gpio_connector_variant);
    if (value == "area_scan_12_pin" || value == "area_scan_8_pin" ||
        value == "line_scan_12_pin" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_gpio_pinout_access(const CameraParams* camera_params)
{
    if (!camera_params) {
        return "unknown";
    }
    const std::string value = lower_ascii(camera_params->gpio_pinout_access);
    if (value == "exposed" || value == "not_exposed" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_gpio_recipe(const CameraParams* camera_params)
{
    if (!camera_params) {
        return {};
    }
    const std::string value = lower_ascii(camera_params->gpio_recipe);
    if (value == "area_scan_hw_trigger_internal_gpi4" ||
        value == "area_scan_hw_trigger_external_gpi4" ||
        value == "line_scan_hw_frame_gpi1_internal_line" ||
        value == "line_scan_hw_frame_gpi1_encoder_line" ||
        value == "line_scan_encoder_frame_encoder_line" ||
        value == "line_scan_hw_gate_gpi1_encoder_frame_encoder_line") {
        return value;
    }
    return {};
}

std::string abbreviate_env_value(const char* value, std::size_t max_len = 240)
{
    if (!value) {
        return {};
    }
    std::string s(value);
    if (s.size() <= max_len) {
        return s;
    }
    return s.substr(0, max_len) + "...";
}

std::string resolved_ptp_mode(const CameraParams* camera_params)
{
    if (!camera_params) {
        return "TwoStep";
    }
    return camera_params->ptp_mode.empty() ? "TwoStep" : camera_params->ptp_mode;
}

bool get_camera_enum_param_string(Emergent::CEmergentCamera* camera,
                                  const char* name,
                                  std::string* out_value)
{
    if (camera == nullptr || name == nullptr || out_value == nullptr) {
        return false;
    }

    char buffer[256] = {};
    unsigned long value_size = 0;
    if (EVT_CameraGetEnumParam(camera, name, buffer, sizeof(buffer), &value_size) != EVT_SUCCESS) {
        return false;
    }
    *out_value = std::string(buffer);
    return true;
}

bool get_camera_uint32_param_value(Emergent::CEmergentCamera* camera,
                                   const char* name,
                                   unsigned int* out_value)
{
    if (camera == nullptr || name == nullptr || out_value == nullptr) {
        return false;
    }
    return EVT_CameraGetUInt32Param(camera, name, out_value) == EVT_SUCCESS;
}

bool get_camera_bool_param_value(Emergent::CEmergentCamera* camera,
                                 const char* name,
                                 bool* out_value)
{
    if (camera == nullptr || name == nullptr || out_value == nullptr) {
        return false;
    }
    return EVT_CameraGetBoolParam(camera, name, out_value) == EVT_SUCCESS;
}

bool parse_gpo_line_index(const std::string& camera_line, int* index_out)
{
    if (index_out) {
        *index_out = -1;
    }
    const std::string line = lower_ascii(camera_line);
    constexpr const char* kPrefix = "gpo_";
    if (line.rfind(kPrefix, 0) != 0 || line.size() <= std::strlen(kPrefix)) {
        return false;
    }
    int parsed = 0;
    for (std::size_t i = std::strlen(kPrefix); i < line.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(line[i]);
        if (!std::isdigit(ch)) {
            return false;
        }
        parsed = parsed * 10 + (line[i] - '0');
    }
    if (parsed < 0 || parsed > 9) {
        return false;
    }
    if (index_out) {
        *index_out = parsed;
    }
    return true;
}

bool rig_io_level_is_high(const std::string& value, bool default_value)
{
    const std::string normalized = lower_ascii(value);
    if (normalized == "high" || normalized == "rising_edge" || normalized == "pulse") {
        return true;
    }
    if (normalized == "low" || normalized == "falling_edge") {
        return false;
    }
    return default_value;
}

void log_ptp_camera_sync_readback(Emergent::CEmergentCamera* camera, const CameraParams* camera_params)
{
    if (camera == nullptr || camera_params == nullptr) {
        return;
    }

    std::string trigger_selector = "<unavailable>";
    std::string trigger_source = "<unavailable>";
    std::string trigger_mode = "<unavailable>";
    std::string acquisition_mode = "<unavailable>";
    std::string ptp_mode = "<unavailable>";
    unsigned int acquisition_frame_count = 0;

    get_camera_enum_param_string(camera, "TriggerSelector", &trigger_selector);
    get_camera_enum_param_string(camera, "TriggerSource", &trigger_source);
    get_camera_enum_param_string(camera, "TriggerMode", &trigger_mode);
    get_camera_enum_param_string(camera, "AcquisitionMode", &acquisition_mode);
    get_camera_enum_param_string(camera, "PtpMode", &ptp_mode);
    const bool has_acquisition_frame_count =
        get_camera_uint32_param_value(camera, "AcquisitionFrameCount", &acquisition_frame_count);

    std::cout << camera_params->camera_serial
              << " [ptp_camera_sync] Readback"
              << " requested_acquisition_mode=" << camera_params->ptp_gate_acquisition_mode
              << " trigger_selector=" << trigger_selector
              << " trigger_source=" << trigger_source
              << " trigger_mode=" << trigger_mode
              << " acquisition_mode=" << acquisition_mode
              << " acquisition_frame_count="
              << (has_acquisition_frame_count ? std::to_string(acquisition_frame_count) : std::string("<unavailable>"))
              << " ptp_mode=" << ptp_mode
              << " gate_offset_ns=" << camera_params->ptp_gate_offset_ns
              << std::endl;
}

void log_camera_gpudirect_state(const char* stage,
                                const char* context,
                                Emergent::CEmergentCamera* camera,
                                const CameraParams* camera_params,
                                EVT_ERROR err = EVT_SUCCESS)
{
    const char* safe_stage = stage ? stage : "unknown_stage";
    const char* safe_context = context ? context : "unspecified";
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");
    const char* emergent_dir = std::getenv("EMERGENT_DIR");
    const char* ld_library_path = std::getenv("LD_LIBRARY_PATH");
    const char* path_env = std::getenv("PATH");
    const char* rivermax_log_level = std::getenv("RIVERMAX_LOG_LEVEL");
    const char* vma_tracelevel = std::getenv("VMA_TRACELEVEL");

    std::ostringstream out;
    out << "[CAMERA][GPUDIRECT]"
        << " stage=" << safe_stage
        << " context=" << safe_context;
    if (camera_params) {
        out << " serial=" << camera_params->camera_serial
            << " name=" << camera_params->camera_name
            << " camera_id=" << camera_params->camera_id
            << " gpu_direct=" << (camera_params->gpu_direct ? "true" : "false")
            << " runtime_gpu_id=" << camera_params->gpu_id
            << " configured_gpu_id=" << camera_params->configured_gpu_id
            << " overridden=" << (camera_params->gpu_id_runtime_overridden ? "true" : "false")
            << " resolution=" << camera_params->width << "x" << camera_params->height
            << " pixel_format=" << camera_params->pixel_format
            << " sync_mode=" << camera_params->sync_mode;
    }
    if (camera) {
        out << " sdk_gpu_direct_device_id=" << camera->gpuDirectDeviceId;
    }
    int current_cuda_device = -1;
    const cudaError_t cuda_device_err = cudaGetDevice(&current_cuda_device);
    out << " current_cuda_device=";
    if (cuda_device_err == cudaSuccess) {
        out << current_cuda_device;
    } else {
        out << "err(" << cudaGetErrorString(cuda_device_err) << ")";
    }
    out << " uid=" << getuid()
        << " euid=" << geteuid()
        << " sudo_uid=" << (sudo_uid ? sudo_uid : "")
        << " sudo_gid=" << (sudo_gid ? sudo_gid : "")
        << " emergent_dir=" << abbreviate_env_value(emergent_dir)
        << " ld_library_path=" << abbreviate_env_value(ld_library_path)
        << " path_env=" << abbreviate_env_value(path_env)
        << " rivermax_log_level=" << (rivermax_log_level ? rivermax_log_level : "")
        << " vma_tracelevel=" << (vma_tracelevel ? vma_tracelevel : "");
    if (err != EVT_SUCCESS) {
        out << " evt_error=" << static_cast<int>(err)
            << " evt_error_string=\"" << get_evt_error_string(err) << "\"";
    }
    std::cout << out.str() << std::endl;
}

CameraGpioNodeConfig make_enum_node(const char* name, const char* value)
{
    CameraGpioNodeConfig node;
    node.name = name;
    node.type = "enum";
    node.value_string = value;
    return node;
}

CameraGpioNodeConfig make_bool_node(const char* name, bool value)
{
    CameraGpioNodeConfig node;
    node.name = name;
    node.type = "bool";
    node.value_bool = value;
    return node;
}

CameraGpioNodeConfig make_uint_node(const char* name, uint32_t value)
{
    CameraGpioNodeConfig node;
    node.name = name;
    node.type = "uint";
    node.value_uint = value;
    return node;
}

[[noreturn]] void throw_camera_config_error(
    const std::string& camera_serial,
    const std::string& message)
{
    const std::string full = camera_serial + " camera config error: " + message;
    std::cerr << full << std::endl;
    throw std::runtime_error(full);
}

void ensure_param_exists(
    Emergent::CEmergentCamera* camera,
    const std::string& camera_serial,
    const char* name,
    const char* context)
{
    if (!has_param(camera, name)) {
        throw_camera_config_error(
            camera_serial,
            std::string("[") + context + "] required GenICam node missing: " + name);
    }
}

void apply_trigger_config(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    if (!camera_params->trigger_enabled) {
        return;
    }

    ensure_param_exists(camera, camera_params->camera_serial, "TriggerSelector", context);
    ensure_param_exists(camera, camera_params->camera_serial, "TriggerSource", context);
    ensure_param_exists(camera, camera_params->camera_serial, "TriggerMode", context);

    check_camera_errors(
        EVT_CameraSetEnumParam(camera, "TriggerSelector", camera_params->trigger_selector.c_str()),
        camera_params->camera_serial.c_str());
    check_camera_errors(
        EVT_CameraSetEnumParam(camera, "TriggerSource", camera_params->trigger_source.c_str()),
        camera_params->camera_serial.c_str());
    if (has_param(camera, "TriggerActivation")) {
        check_camera_errors(
            EVT_CameraSetEnumParam(camera, "TriggerActivation", camera_params->trigger_activation.c_str()),
            camera_params->camera_serial.c_str());
    } else {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] TriggerActivation node not present; skipping activation config."
                  << std::endl;
    }
    check_camera_errors(
        EVT_CameraSetEnumParam(camera, "TriggerMode", "On"),
        camera_params->camera_serial.c_str());

    std::cout << camera_params->camera_serial
              << " [" << context << "] Applied trigger config: selector=" << camera_params->trigger_selector
              << " source=" << camera_params->trigger_source
              << " activation=" << camera_params->trigger_activation
              << std::endl;
}

void apply_ptp_mode_config(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    if (!camera_sync_mode_uses_ptp(camera_params)) {
        return;
    }

    const std::string ptp_mode = resolved_ptp_mode(camera_params);
    ensure_param_exists(camera, camera_params->camera_serial, "PtpMode", context);
    check_camera_errors(
        EVT_CameraSetEnumParam(camera, "PtpMode", ptp_mode.c_str()),
        camera_params->camera_serial.c_str());

    std::cout << camera_params->camera_serial
              << " [" << context << "] Prepared PTP mode: " << ptp_mode
              << " (gate programming happens when streaming starts)."
              << std::endl;
}

void apply_gpio_node_config(
    Emergent::CEmergentCamera* camera,
    const CameraParams* camera_params,
    const CameraGpioNodeConfig& node,
    const char* context)
{
    ensure_param_exists(camera, camera_params->camera_serial, node.name.c_str(), context);

    const std::string node_type = lower_ascii(node.type);
    if (node_type == "enum") {
        check_camera_errors(
            EVT_CameraSetEnumParam(camera, node.name.c_str(), node.value_string.c_str()),
            camera_params->camera_serial.c_str());
        std::string readback;
        if (!get_camera_enum_param_string(camera, node.name.c_str(), &readback) ||
            readback != node.value_string) {
            throw_camera_config_error(
                camera_params->camera_serial,
                std::string("[") + context + "] GPIO enum readback mismatch for " +
                    node.name + ": requested=" + node.value_string +
                    " readback=" + (readback.empty() ? "<unavailable>" : readback));
        }
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Verified GPIO enum " << node.name
                  << "=" << node.value_string
                  << std::endl;
        return;
    }
    if (node_type == "bool") {
        check_camera_errors(
            EVT_CameraSetBoolParam(camera, node.name.c_str(), node.value_bool),
            camera_params->camera_serial.c_str());
        bool readback = false;
        if (!get_camera_bool_param_value(camera, node.name.c_str(), &readback) ||
            readback != node.value_bool) {
            throw_camera_config_error(
                camera_params->camera_serial,
                std::string("[") + context + "] GPIO bool readback mismatch for " +
                    node.name + ": requested=" + (node.value_bool ? "true" : "false") +
                    " readback=" + (readback ? "true" : "false"));
        }
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Verified GPIO bool " << node.name
                  << "=" << (node.value_bool ? "true" : "false")
                  << std::endl;
        return;
    }
    if (node_type == "uint") {
        check_camera_errors(
            EVT_CameraSetUInt32Param(camera, node.name.c_str(), node.value_uint),
            camera_params->camera_serial.c_str());
        unsigned int readback = 0;
        if (!get_camera_uint32_param_value(camera, node.name.c_str(), &readback) ||
            readback != node.value_uint) {
            throw_camera_config_error(
                camera_params->camera_serial,
                std::string("[") + context + "] GPIO uint readback mismatch for " +
                    node.name + ": requested=" + std::to_string(node.value_uint) +
                    " readback=" + std::to_string(readback));
        }
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Verified GPIO uint " << node.name
                  << "=" << node.value_uint
                  << std::endl;
        return;
    }

    throw_camera_config_error(
        camera_params->camera_serial,
        std::string("[") + context + "] unsupported GPIO node type `" + node.type +
            "` for node `" + node.name + "`");
}

void apply_gpio_node_configs(Emergent::CEmergentCamera* camera, const CameraParams* camera_params, const char* context)
{
    if (normalize_gpio_pinout_access(camera_params) == "not_exposed") {
        if (!camera_params->gpio_nodes.empty()) {
            throw_camera_config_error(
                camera_params->camera_serial,
                std::string("[") + context +
                    "] gpio.nodes are configured but gpio_pinout_access=not_exposed");
        }
        return;
    }

    for (const auto& node : camera_params->gpio_nodes) {
        apply_gpio_node_config(camera, camera_params, node, context);
    }
}

void apply_gpio_recipe_nodes(
    Emergent::CEmergentCamera* camera,
    const CameraParams* camera_params,
    const std::vector<CameraGpioNodeConfig>& nodes,
    const char* context)
{
    for (const auto& node : nodes) {
        apply_gpio_node_config(camera, camera_params, node, context);
    }
}

bool build_gpio_recipe_preview_nodes_impl(const CameraParams* camera_params,
                                          std::vector<CameraGpioNodeConfig>* nodes_out,
                                          std::string* error_out)
{
    if (nodes_out) {
        nodes_out->clear();
    }
    if (error_out) {
        error_out->clear();
    }
    if (!camera_params) {
        if (error_out) {
            *error_out = "camera params missing";
        }
        return false;
    }

    const std::string recipe = normalize_gpio_recipe(camera_params);
    if (recipe.empty()) {
        if (!camera_params->gpio_recipe.empty()) {
            if (error_out) {
                *error_out = std::string("unsupported gpio_recipe `") + camera_params->gpio_recipe + "`";
            }
        }
        return false;
    }

    if (normalize_gpio_pinout_access(camera_params) == "not_exposed") {
        if (error_out) {
            *error_out = std::string("gpio_recipe `") + recipe +
                         "` requires exposed GPIO pinout access";
        }
        return false;
    }

    if (camera_sync_mode_uses_ptp(camera_params)) {
        if (error_out) {
            *error_out = std::string("gpio_recipe `") + recipe + "` conflicts with sync_mode=ptp_gate";
        }
        return false;
    }

    const std::string connector_variant = normalize_gpio_connector_variant(camera_params);
    const std::string scan_type = lower_ascii(camera_params->camera_scan_type);
    std::vector<CameraGpioNodeConfig> nodes;

    if (recipe == "area_scan_hw_trigger_internal_gpi4") {
        if (scan_type != "area_scan" || connector_variant != "area_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=area_scan and gpio_connector_variant=area_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("AcquisitionMode", "MultiFrame"),
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Exp_Mode", "GPI_4"),
            make_enum_node("GPI_Start_Exp_Event", "Rising_Edge"),
            make_uint_node("GPI_4_Debounce_Count", 50),
            make_enum_node("GPI_End_Exp_Mode", "Internal")
        };
    } else if (recipe == "area_scan_hw_trigger_external_gpi4") {
        if (scan_type != "area_scan" || connector_variant != "area_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=area_scan and gpio_connector_variant=area_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("AcquisitionMode", "MultiFrame"),
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Exp_Mode", "GPI_4"),
            make_enum_node("GPI_Start_Exp_Event", "Rising_Edge"),
            make_uint_node("GPI_4_Debounce_Count", 50),
            make_enum_node("GPI_End_Exp_Mode", "GPI_4"),
            make_enum_node("GPI_End_Exp_Event", "Falling_Edge")
        };
    } else if (recipe == "line_scan_hw_frame_gpi1_internal_line") {
        if (scan_type != "line_scan" || connector_variant != "line_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=line_scan and gpio_connector_variant=line_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Frame_Mode", "GPI_1"),
            make_enum_node("GPI_Start_Frame_Event", "Rising_Edge"),
            make_uint_node("GPI_1_Debounce_Count", 50),
            make_uint_node("LineTime", 1000)
        };
    } else if (recipe == "line_scan_hw_frame_gpi1_encoder_line") {
        if (scan_type != "line_scan" || connector_variant != "line_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=line_scan and gpio_connector_variant=line_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Frame_Mode", "GPI_1"),
            make_enum_node("GPI_Start_Frame_Event", "Rising_Edge"),
            make_uint_node("GPI_1_Debounce_Count", 50),
            make_bool_node("GP_ENC_MODE", true),
            make_uint_node("GP_ENC_LINE_Multiplier", 1),
            make_uint_node("GP_ENC_LINE_DIVIDER", 4)
        };
    } else if (recipe == "line_scan_encoder_frame_encoder_line") {
        if (scan_type != "line_scan" || connector_variant != "line_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=line_scan and gpio_connector_variant=line_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Frame_Event", "Encoder_Frame_Divider"),
            make_bool_node("GP_ENC_MODE", true),
            make_uint_node("GP_ENC_LINE_Multiplier", 1),
            make_uint_node("GP_ENC_LINE_DIVIDER", 4),
            make_uint_node("GP_ENC_FRAME_DIVIDER", 24000)
        };
    } else if (recipe == "line_scan_hw_gate_gpi1_encoder_frame_encoder_line") {
        if (scan_type != "line_scan" || connector_variant != "line_scan_12_pin") {
            if (error_out) {
                *error_out = std::string("gpio_recipe `") + recipe +
                             "` requires camera_scan_type=line_scan and gpio_connector_variant=line_scan_12_pin";
            }
            return false;
        }
        nodes = {
            make_enum_node("TriggerMode", "On"),
            make_enum_node("TriggerSource", "Hardware"),
            make_enum_node("GPI_Start_Frame_Mode", "GPI_1"),
            make_enum_node("GPI_Start_Frame_Event", "Pulse_High"),
            make_uint_node("GPI_1_Debounce_Count", 50),
            make_bool_node("GP_ENC_MODE", true),
            make_uint_node("GP_ENC_LINE_Multiplier", 1),
            make_uint_node("GP_ENC_LINE_DIVIDER", 4),
            make_uint_node("GP_ENC_FRAME_DIVIDER", 24000)
        };
    }

    if (nodes.empty()) {
        if (error_out) {
            *error_out = std::string("gpio_recipe `") + recipe + "` did not resolve to any node writes";
        }
        return false;
    }

    if (nodes_out) {
        *nodes_out = std::move(nodes);
    }
    return true;
}

bool apply_configured_gpio_recipe(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    std::vector<CameraGpioNodeConfig> nodes;
    std::string recipe_error;
    if (!build_gpio_recipe_preview_nodes_impl(camera_params, &nodes, &recipe_error)) {
        if (!camera_params->gpio_recipe.empty() && !recipe_error.empty()) {
            throw_camera_config_error(
                camera_params->camera_serial,
                std::string("[") + context + "] " + recipe_error);
        }
        return false;
    }

    const std::string recipe = normalize_gpio_recipe(camera_params);
    const std::string connector_variant = normalize_gpio_connector_variant(camera_params);

    std::cout << camera_params->camera_serial
              << " [" << context << "] Applying gpio_recipe=" << recipe
              << " for device_model=" << (camera_params->device_model.empty() ? "unknown" : camera_params->device_model)
              << " scan_type=" << (camera_params->camera_scan_type.empty() ? "unknown" : camera_params->camera_scan_type)
              << " connector=" << connector_variant
              << std::endl;
    apply_gpio_recipe_nodes(camera, camera_params, nodes, context);
    return true;
}

void apply_configured_runtime_mode(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    const bool recipe_applied = apply_configured_gpio_recipe(camera, camera_params, context);
    const std::string sync_mode = normalize_sync_mode(camera_params);
    if (sync_mode == "ptp_gate") {
        apply_ptp_mode_config(camera, camera_params, context);
    } else if (sync_mode == "free_run") {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Sync mode=free_run"
                  << std::endl;
    } else {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Sync mode=" << sync_mode
                  << " using explicit trigger/GPIO config."
                  << std::endl;
    }

    if (!camera_sync_mode_uses_ptp(camera_params) && !recipe_applied) {
        apply_trigger_config(camera, camera_params, context);
    }

    apply_gpio_node_configs(camera, camera_params, context);
}

bool refresh_focus_range(Emergent::CEmergentCamera* camera, CameraParams* camera_params)
{
    EVT_ERROR max_err = EVT_CameraGetUInt32ParamMax(camera, "Focus", &camera_params->focus_max);
    EVT_ERROR min_err = EVT_CameraGetUInt32ParamMin(camera, "Focus", &camera_params->focus_min);
    EVT_ERROR inc_err = EVT_CameraGetUInt32ParamInc(camera, "Focus", &camera_params->focus_inc);
    return max_err == EVT_SUCCESS && min_err == EVT_SUCCESS && inc_err == EVT_SUCCESS;
}

bool has_usable_focus_range(const CameraParams* camera_params)
{
    return camera_params->focus_max > camera_params->focus_min;
}

bool refresh_iris_range(Emergent::CEmergentCamera* camera, CameraParams* camera_params)
{
    EVT_ERROR max_err = EVT_CameraGetUInt32ParamMax(camera, "Iris", &camera_params->iris_max);
    EVT_ERROR min_err = EVT_CameraGetUInt32ParamMin(camera, "Iris", &camera_params->iris_min);
    EVT_ERROR inc_err = EVT_CameraGetUInt32ParamInc(camera, "Iris", &camera_params->iris_inc);
    return max_err == EVT_SUCCESS && min_err == EVT_SUCCESS && inc_err == EVT_SUCCESS;
}

void log_uart_step_result(const std::string& camera_serial, const char* context, const char* step, EVT_ERROR err)
{
    if (err == EVT_SUCCESS)
    {
        std::cout << camera_serial
                  << " [" << context << "] " << step << " ok"
                  << std::endl;
    }
    else
    {
        std::cout << camera_serial
                  << " [" << context << "] " << step << " failed: " << get_evt_error_string(err)
                  << std::endl;
    }
}

void try_enable_lens_uart_path(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    const bool has_uart_nodes =
        has_param(camera, "UartEnable") &&
        has_param(camera, "UartBaud") &&
        has_param(camera, "UartDataBits") &&
        has_param(camera, "UartStopBits") &&
        has_param(camera, "GPO_3_Mode");

    if (!has_uart_nodes)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus range is degenerate and UART nodes are unavailable."
                  << std::endl;
        return;
    }

    std::cout << camera_params->camera_serial
              << " [" << context << "] Focus range is degenerate. Attempting UART lens bootstrap."
              << std::endl;

    EVT_ERROR err = EVT_CameraSetEnumParam(camera, "GPO_3_Mode", "Test_Gen_Uart_Txd");
    log_uart_step_result(camera_params->camera_serial, context, "GPO_3_Mode=Test_Gen_Uart_Txd", err);

    err = EVT_CameraSetBoolParam(camera, "UartEnable", true);
    log_uart_step_result(camera_params->camera_serial, context, "UartEnable=true", err);

    err = EVT_CameraSetEnumParam(camera, "UartBaud", "B_9600");
    log_uart_step_result(camera_params->camera_serial, context, "UartBaud=B_9600", err);

    err = EVT_CameraSetUInt32Param(camera, "UartDataBits", 8);
    log_uart_step_result(camera_params->camera_serial, context, "UartDataBits=8", err);

    err = EVT_CameraSetUInt32Param(camera, "UartStopBits", 1);
    log_uart_step_result(camera_params->camera_serial, context, "UartStopBits=1", err);
}

void ensure_focus_range_ready(Emergent::CEmergentCamera* camera, CameraParams* camera_params, const char* context)
{
    if (!refresh_focus_range(camera, camera_params))
    {
        return;
    }

    if (has_usable_focus_range(camera_params))
    {
        return;
    }

    // Lens initialization can lag camera open.
    for (int i = 0; i < kFocusPollAttemptsBeforeUart; ++i)
    {
        usleep(kFocusPollIntervalUs);
        if (refresh_focus_range(camera, camera_params) && has_usable_focus_range(camera_params))
        {
            std::cout << camera_params->camera_serial
                      << " [" << context << "] Focus range became available without UART bootstrap: ["
                      << camera_params->focus_min << "," << camera_params->focus_max << "]"
                      << std::endl;
            return;
        }
    }

    if (!camera_params->focus_uart_bootstrap)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus range remained [" << camera_params->focus_min
                  << "," << camera_params->focus_max
                  << "] and focus_uart_bootstrap is disabled."
                  << std::endl;
        return;
    }

    bool lens_mount_present = false;
    bool lens_present = false;
    EVT_CameraGetBoolParam(camera, "LensMountPresent", &lens_mount_present);
    EVT_CameraGetBoolParam(camera, "LensPresent", &lens_present);

    if (!lens_mount_present || !lens_present)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus range remained [" << camera_params->focus_min
                  << "," << camera_params->focus_max << "] with no detected lens/mount."
                  << std::endl;
        return;
    }

    try_enable_lens_uart_path(camera, camera_params, context);

    for (int i = 0; i < kFocusPollAttemptsAfterUart; ++i)
    {
        usleep(kFocusPollIntervalUs);
        if (refresh_focus_range(camera, camera_params) && has_usable_focus_range(camera_params))
        {
            std::cout << camera_params->camera_serial
                      << " [" << context << "] Focus range after UART bootstrap: ["
                      << camera_params->focus_min << "," << camera_params->focus_max << "]"
                      << std::endl;
            return;
        }
    }

    std::cout << camera_params->camera_serial
              << " [" << context << "] Focus range still degenerate after bootstrap: ["
              << camera_params->focus_min << "," << camera_params->focus_max << "]"
              << std::endl;
}

bool set_focus_value_checked(
    Emergent::CEmergentCamera* camera,
    int focus_value,
    CameraParams* camera_params,
    const char* context)
{
    ensure_focus_range_ready(camera, camera_params, context);
    if (!refresh_focus_range(camera, camera_params))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus set FAIL: unable to query focus range."
                  << std::endl;
        return false;
    }

    if (focus_value < static_cast<int>(camera_params->focus_min) ||
        focus_value > static_cast<int>(camera_params->focus_max))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus set FAIL: target=" << focus_value
                  << " out of range=[" << camera_params->focus_min << "," << camera_params->focus_max << "]"
                  << std::endl;
        return false;
    }

    EVT_ERROR set_err = EVT_CameraSetUInt32Param(camera, "Focus", static_cast<unsigned int>(focus_value));
    if (set_err != EVT_SUCCESS)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus set FAIL: " << get_evt_error_string(set_err)
                  << std::endl;
        return false;
    }

    camera_params->focus = static_cast<unsigned int>(focus_value);

    unsigned int readback = 0;
    EVT_ERROR get_err = EVT_CameraGetUInt32Param(camera, "Focus", &readback);
    if (get_err != EVT_SUCCESS)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus set WARN: set ok, readback failed: "
                  << get_evt_error_string(get_err)
                  << std::endl;
        return false;
    }

    if (readback != static_cast<unsigned int>(focus_value))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Focus set WARN: target=" << focus_value
                  << " readback=" << readback
                  << std::endl;
        return false;
    }

    std::cout << camera_params->camera_serial
              << " [" << context << "] Focus set PASS: target=" << focus_value
              << " readback=" << readback
              << " range=[" << camera_params->focus_min << "," << camera_params->focus_max << "]"
              << std::endl;
    return true;
}

bool set_iris_value_checked(
    Emergent::CEmergentCamera* camera,
    int iris_value,
    CameraParams* camera_params,
    const char* context)
{
    if (!refresh_iris_range(camera, camera_params))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris set FAIL: unable to query iris range."
                  << std::endl;
        return false;
    }

    if (iris_value < static_cast<int>(camera_params->iris_min) ||
        iris_value > static_cast<int>(camera_params->iris_max))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris set FAIL: target=" << iris_value
                  << " out of range=[" << camera_params->iris_min << "," << camera_params->iris_max << "]"
                  << std::endl;
        return false;
    }

    EVT_ERROR set_err = EVT_CameraSetUInt32Param(camera, "Iris", static_cast<unsigned int>(iris_value));
    if (set_err != EVT_SUCCESS)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris set FAIL: " << get_evt_error_string(set_err)
                  << std::endl;
        return false;
    }

    camera_params->iris = static_cast<unsigned int>(iris_value);

    unsigned int readback = 0;
    EVT_ERROR get_err = EVT_CameraGetUInt32Param(camera, "Iris", &readback);
    if (get_err != EVT_SUCCESS)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris set WARN: set ok, readback failed: "
                  << get_evt_error_string(get_err)
                  << std::endl;
        return false;
    }

    if (readback != static_cast<unsigned int>(iris_value))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris set WARN: target=" << iris_value
                  << " readback=" << readback
                  << std::endl;
        return false;
    }

    std::cout << camera_params->camera_serial
              << " [" << context << "] Iris set PASS: target=" << iris_value
              << " readback=" << readback
              << " range=[" << camera_params->iris_min << "," << camera_params->iris_max << "]"
              << std::endl;
    return true;
}

bool set_startup_iris_value_checked(
    Emergent::CEmergentCamera* camera,
    int configured_iris_value,
    CameraParams* camera_params,
    const char* context)
{
    if (!camera_params->focus_uart_bootstrap)
    {
        return set_iris_value_checked(camera, configured_iris_value, camera_params, context);
    }

    if (!refresh_iris_range(camera, camera_params))
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris startup prime skipped: unable to query iris range."
                  << std::endl;
        return set_iris_value_checked(camera, configured_iris_value, camera_params, context);
    }

    const int iris_prime_value = static_cast<int>(camera_params->iris_min);
    if (configured_iris_value != iris_prime_value)
    {
        std::cout << camera_params->camera_serial
                  << " [" << context << "] Iris startup prime: iris_min=" << iris_prime_value
                  << " then configured=" << configured_iris_value
                  << std::endl;
        (void)set_iris_value_checked(camera, iris_prime_value, camera_params, "open_camera_with_params iris_prime");
        usleep(kIrisPrimeSettleUs);
    }

    return set_iris_value_checked(camera, configured_iris_value, camera_params, context);
}
}  // namespace

bool get_camera_string_param(Emergent::CEmergentCamera* camera, const char* name, std::string* out_value)
{
    if (camera == nullptr || name == nullptr || out_value == nullptr) {
        return false;
    }

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

    *out_value = std::string(buffer.data());
    return true;
}

bool get_camera_uint32_param_range(Emergent::CEmergentCamera* camera,
                                   const char* name,
                                   unsigned int* min_out,
                                   unsigned int* max_out,
                                   unsigned int* inc_out)
{
    if (camera == nullptr || name == nullptr || min_out == nullptr || max_out == nullptr) {
        return false;
    }

    unsigned int min_value = 0;
    unsigned int max_value = 0;
    EVT_ERROR min_err = EVT_CameraGetUInt32ParamMin(camera, name, &min_value);
    EVT_ERROR max_err = EVT_CameraGetUInt32ParamMax(camera, name, &max_value);
    if (min_err != EVT_SUCCESS || max_err != EVT_SUCCESS) {
        return false;
    }

    *min_out = min_value;
    *max_out = max_value;
    if (inc_out != nullptr) {
        unsigned int inc_value = 0;
        if (EVT_CameraGetUInt32ParamInc(camera, name, &inc_value) == EVT_SUCCESS) {
            *inc_out = inc_value;
        }
    }
    return true;
}

bool build_gpio_recipe_preview_nodes(const CameraParams* camera_params,
                                     std::vector<CameraGpioNodeConfig>* nodes_out,
                                     std::string* error_out)
{
    return build_gpio_recipe_preview_nodes_impl(camera_params, nodes_out, error_out);
}

bool resolve_rig_io_gpo_nodes(const CameraRigIoConnection& connection,
                              int* gpo_index_out,
                              std::string* mode_node_out,
                              std::string* polarity_node_out,
                              std::string* status_out)
{
    int gpo_index = -1;
    if (!parse_gpo_line_index(connection.camera_line, &gpo_index)) {
        if (status_out) {
            *status_out = "Rig I/O diagnostic failed: camera_line must be GPO_N.";
        }
        return false;
    }
    if (gpo_index_out) {
        *gpo_index_out = gpo_index;
    }
    if (mode_node_out) {
        *mode_node_out = "GPO_" + std::to_string(gpo_index) + "_Mode";
    }
    if (polarity_node_out) {
        *polarity_node_out = "GPO_" + std::to_string(gpo_index) + "_Polarity";
    }
    return true;
}

bool read_rig_io_output_diagnostic_state(Emergent::CEmergentCamera* camera,
                                         const CameraParams* camera_params,
                                         const CameraRigIoConnection& connection,
                                         CameraRigIoOutputState* state_out,
                                         std::string* status_out)
{
    auto set_status = [&](const std::string& message) {
        if (status_out) {
            *status_out = message;
        }
    };

    if (state_out) {
        *state_out = CameraRigIoOutputState{};
    }
    if (!camera || !camera_params) {
        set_status("Rig I/O diagnostic readback failed: camera is not open.");
        return false;
    }
    if (lower_ascii(connection.direction) != "output") {
        set_status("Rig I/O diagnostic readback failed: mapping direction is not output.");
        return false;
    }

    int gpo_index = -1;
    std::string mode_node;
    std::string polarity_node;
    if (!resolve_rig_io_gpo_nodes(
            connection, &gpo_index, &mode_node, &polarity_node, status_out)) {
        return false;
    }
    if (!has_param(camera, mode_node.c_str())) {
        set_status("Rig I/O diagnostic readback failed: missing GenICam node " + mode_node + ".");
        return false;
    }

    CameraRigIoOutputState state;
    state.camera_line = "GPO_" + std::to_string(gpo_index);
    state.mode_node = mode_node;
    state.polarity_node = polarity_node;
    if (!get_camera_enum_param_string(camera, mode_node.c_str(), &state.mode)) {
        set_status("Rig I/O diagnostic readback failed: unable to read " + mode_node + ".");
        return false;
    }

    if (has_param(camera, polarity_node.c_str())) {
        bool polarity = false;
        const EVT_ERROR err = EVT_CameraGetBoolParam(camera, polarity_node.c_str(), &polarity);
        if (err != EVT_SUCCESS) {
            set_status("Rig I/O diagnostic readback failed: " + polarity_node +
                       " read failed: " + get_evt_error_string(err));
            return false;
        }
        state.has_polarity = true;
        state.polarity = polarity;
    }
    state.valid = true;

    std::ostringstream oss;
    oss << "Captured " << state.camera_line << " state: "
        << state.mode_node << "=" << state.mode;
    if (state.has_polarity) {
        oss << ", " << state.polarity_node << "="
            << (state.polarity ? "true/high" : "false/low");
    }
    set_status(oss.str());
    std::cout << camera_params->camera_serial << " [rig_io_output_diagnostic] "
              << oss.str() << std::endl;
    if (state_out) {
        *state_out = std::move(state);
    }
    return true;
}

bool set_rig_io_output_diagnostic(Emergent::CEmergentCamera* camera,
                                  const CameraParams* camera_params,
                                  const CameraRigIoConnection& connection,
                                  const bool active,
                                  std::string* status_out)
{
    auto set_status = [&](const std::string& message) {
        if (status_out) {
            *status_out = message;
        }
    };

    if (!camera || !camera_params) {
        set_status("Rig I/O diagnostic failed: camera is not open.");
        return false;
    }
    if (lower_ascii(connection.direction) != "output") {
        set_status("Rig I/O diagnostic failed: mapping direction is not output.");
        return false;
    }

    int gpo_index = -1;
    std::string mode_node;
    std::string polarity_node;
    if (!resolve_rig_io_gpo_nodes(
            connection, &gpo_index, &mode_node, &polarity_node, status_out)) {
        return false;
    }
    if (!has_param(camera, mode_node.c_str())) {
        set_status("Rig I/O diagnostic failed: missing GenICam node " + mode_node + ".");
        return false;
    }
    if (!has_param(camera, polarity_node.c_str())) {
        set_status("Rig I/O diagnostic failed: missing GenICam node " + polarity_node + ".");
        return false;
    }

    EVT_ERROR err = EVT_CameraSetEnumParam(camera, mode_node.c_str(), "GPO");
    if (err != EVT_SUCCESS) {
        set_status("Rig I/O diagnostic failed: " + mode_node + "=GPO write failed: " +
                   get_evt_error_string(err));
        return false;
    }

    const bool active_high = rig_io_level_is_high(connection.active_level, true);
    const bool inactive_high = rig_io_level_is_high(connection.inactive_level, !active_high);
    const bool requested_high = active ? active_high : inactive_high;
    err = EVT_CameraSetBoolParam(camera, polarity_node.c_str(), requested_high);
    if (err != EVT_SUCCESS) {
        set_status("Rig I/O diagnostic failed: " + polarity_node + " write failed: " +
                   get_evt_error_string(err));
        return false;
    }

    std::ostringstream oss;
    oss << "Rig I/O diagnostic set " << connection.camera_line
        << " manual " << (active ? "active" : "inactive")
        << " via " << mode_node << "=GPO, " << polarity_node << "="
        << (requested_high ? "true/high" : "false/low")
        << ". Restore captured state or reopen/reapply camera config before normal experiments if this line normally pulses.";
    set_status(oss.str());
    std::cout << camera_params->camera_serial << " [rig_io_output_diagnostic] "
              << oss.str() << std::endl;
    return true;
}

bool restore_rig_io_output_diagnostic_state(Emergent::CEmergentCamera* camera,
                                            const CameraParams* camera_params,
                                            const CameraRigIoOutputState& state,
                                            std::string* status_out)
{
    auto set_status = [&](const std::string& message) {
        if (status_out) {
            *status_out = message;
        }
    };

    if (!camera || !camera_params) {
        set_status("Rig I/O diagnostic restore failed: camera is not open.");
        return false;
    }
    if (!state.valid || state.mode_node.empty() || state.mode.empty()) {
        set_status("Rig I/O diagnostic restore failed: no captured GPO state.");
        return false;
    }
    if (!has_param(camera, state.mode_node.c_str())) {
        set_status("Rig I/O diagnostic restore failed: missing GenICam node " +
                   state.mode_node + ".");
        return false;
    }

    EVT_ERROR err = EVT_CameraSetEnumParam(camera, state.mode_node.c_str(), state.mode.c_str());
    if (err != EVT_SUCCESS) {
        set_status("Rig I/O diagnostic restore failed: " + state.mode_node +
                   "=" + state.mode + " write failed: " + get_evt_error_string(err));
        return false;
    }

    if (state.has_polarity && !state.polarity_node.empty()) {
        if (!has_param(camera, state.polarity_node.c_str())) {
            set_status("Rig I/O diagnostic restore failed: missing GenICam node " +
                       state.polarity_node + ".");
            return false;
        }
        err = EVT_CameraSetBoolParam(camera, state.polarity_node.c_str(), state.polarity);
        if (err != EVT_SUCCESS) {
            set_status("Rig I/O diagnostic restore failed: " + state.polarity_node +
                       " write failed: " + get_evt_error_string(err));
            return false;
        }
    }

    std::ostringstream oss;
    oss << "Restored " << (state.camera_line.empty() ? "GPO state" : state.camera_line)
        << ": " << state.mode_node << "=" << state.mode;
    if (state.has_polarity) {
        oss << ", " << state.polarity_node << "="
            << (state.polarity ? "true/high" : "false/low");
    }
    set_status(oss.str());
    std::cout << camera_params->camera_serial << " [rig_io_output_diagnostic] "
              << oss.str() << std::endl;
    return true;
}

bool restore_rig_io_output_normal_mode(Emergent::CEmergentCamera* camera,
                                       const CameraParams* camera_params,
                                       const CameraRigIoConnection& connection,
                                       std::string* status_out)
{
    auto set_status = [&](const std::string& message) {
        if (status_out) {
            *status_out = message;
        }
    };

    if (!camera || !camera_params) {
        set_status("Rig I/O normal-mode restore failed: camera is not open.");
        return false;
    }
    if (lower_ascii(connection.direction) != "output") {
        set_status("Rig I/O normal-mode restore failed: mapping direction is not output.");
        return false;
    }

    int gpo_index = -1;
    std::string mode_node;
    std::string polarity_node;
    if (!resolve_rig_io_gpo_nodes(
            connection, &gpo_index, &mode_node, &polarity_node, status_out)) {
        return false;
    }
    if (!has_param(camera, mode_node.c_str())) {
        set_status("Rig I/O normal-mode restore failed: missing GenICam node " + mode_node + ".");
        return false;
    }

    std::string output_mode = connection.normal_output_mode;
    bool normal_polarity = connection.normal_polarity;
    if (output_mode.empty() && lower_ascii(connection.purpose) == "nir_strobe_trigger") {
        output_mode = "Exposure";
        normal_polarity = false;
    }
    if (output_mode.empty()) {
        set_status("Rig I/O normal-mode restore failed: mapping has no normal_output_mode.");
        return false;
    }

    EVT_ERROR err = EVT_CameraSetEnumParam(camera, mode_node.c_str(), output_mode.c_str());
    if (err != EVT_SUCCESS) {
        set_status("Rig I/O normal-mode restore failed: " + mode_node +
                   "=" + output_mode + " write failed: " + get_evt_error_string(err));
        return false;
    }
    std::string mode_readback;
    if (!get_camera_enum_param_string(camera, mode_node.c_str(), &mode_readback) ||
        mode_readback != output_mode) {
        set_status("Rig I/O normal-mode restore failed: " + mode_node +
                   " readback mismatch; requested=" + output_mode +
                   ", readback=" +
                   (mode_readback.empty() ? "<unavailable>" : mode_readback) + ".");
        return false;
    }

    bool wrote_polarity = false;
    if (has_param(camera, polarity_node.c_str())) {
        err = EVT_CameraSetBoolParam(camera, polarity_node.c_str(), normal_polarity);
        if (err != EVT_SUCCESS) {
            set_status("Rig I/O normal-mode restore failed: " + polarity_node +
                       " write failed: " + get_evt_error_string(err));
            return false;
        }
        bool polarity_readback = false;
        if (!get_camera_bool_param_value(
                camera, polarity_node.c_str(), &polarity_readback) ||
            polarity_readback != normal_polarity) {
            set_status("Rig I/O normal-mode restore failed: " + polarity_node +
                       " readback mismatch; requested=" +
                       (normal_polarity ? "true" : "false") +
                       ", readback=" + (polarity_readback ? "true" : "false") + ".");
            return false;
        }
        wrote_polarity = true;
    }

    std::ostringstream oss;
    oss << "Restored and verified mapped normal output mode for " << connection.camera_line
        << ": " << mode_node << "=" << output_mode;
    if (wrote_polarity) {
        oss << ", " << polarity_node << "="
            << (normal_polarity ? "true/high" : "false/low");
    }
    set_status(oss.str());
    std::cout << camera_params->camera_serial << " [rig_io_output_diagnostic] "
              << oss.str() << std::endl;
    return true;
}

std::string get_evt_error_string(EVT_ERROR error)
{
    std::string error_string; 
    switch (error)
    {
        case EVT_ENOENT:
            error_string = "No such file or directory.";
            break;
        case EVT_ERROR_SRCH: 
            error_string = "No such process.";
            break;
        case EVT_ERROR_INTR:
            error_string = "Interrupted system call.";
            break;
        case EVT_ERROR_IO:
            error_string = "I/O error";
            break;
        case EVT_ERROR_BADF:
            error_string = "Driver error getting camera packets.";
            break;
        case EVT_ERROR_ECHILD:
            error_string = "Child process or thread create error.";
            break;
        case EVT_ERROR_AGAIN:
            error_string = "Try again";
            break;
        case EVT_ERROR_NOMEM:
            error_string = "Out of memory.";
            break;
        case EVT_ERROR_ACCES:
            error_string = "No access or permission.";
            break;
        case EVT_ERROR_FAULT:
            error_string = "Bad address.";
            break;
        case EVT_ERROR_EXIST:
            error_string = "File exists.";
            break;
        case EVT_ERROR_ENODEV:
            error_string = "No such device.";
            break;
        case EVT_ERROR_INVAL:
            error_string = "Invalid argument.";
            break;
        case EVT_ERROR_FBIG:
            error_string = "File too large.";
            break;
        case EVT_ERROR_BADFD:
            error_string = "Frame data was overwritten by newly received data.";
            break;
        case EVT_ERROR_TIMEDOUT:
            error_string = "Connection timed out.";
            break;
        case EVT_ERROR_ALREADY:
            error_string = "Operation already in progress.";
            break;
        case EVT_ERROR_NOBUFS:
            error_string = "No stream buffer was available for the next GVSP block.";
            break;
        case EVT_ERROR_NOT_SUPPORTED:
            error_string = "Not supported.";
            break;
        case EVT_ERROR_DEVICE_CONNECTED_ALRD: 
            error_string = "Camera has been opened.";
            break;
        case EVT_ERROR_DEVICE_NOT_CONNECTED: 
            error_string = "Camera has not been opened.";
            break;
        case EVT_ERROR_DEVICE_LOST_CONNECTION:  
            error_string = "Camera lost connection due to disconnected, powered off, crashing etc.";
            break;
        case EVT_ERROR_GENICAM_ERROR:
            error_string = "Generic GeniCam error from GeniCam lib.";
            break;
        case EVT_ERROR_GENICAM_NOT_MATCH:
            error_string =  "Parameter not matched.";
            break;
        case EVT_ERROR_GENICAM_OUT_OF_RANGE:
            error_string = "Parameter out of range.";
            break;        
        case EVT_ERROR_SOCK: 
            error_string = "Socket operation failed.";
            break;        
        case EVT_ERROR_GVCP_ACK: 
            error_string = "GVCP ACK error.";
            break;        
        case EVT_ERROR_GVSP_DATA_CORRUPT:
            error_string = "Gvsp stream data corrupted, would cause block dropped.";
            break;        
        case EVT_ERROR_NIC_LIB_INIT:
            error_string = "Fail to initialize NIC's SDK library.";
            break;    
        case EVT_ERROR_OS_OBTAIN_ADAPTER:
            error_string = "Failed to get host adapter info.";
            break;
        case EVT_ERROR_SDK:
            error_string = "SDK error, should not occur. Can be removed if sdk is proved to be correct.";
            break;
        case EVT_GENERAL_ERROR:
            error_string = "General error.";
            break;
        default:
            error_string = "Unknown EVT error " + std::to_string(static_cast<int>(error)) + ".";
            break;
    }
    return error_string;
}


void print_camera_device_struct(GigEVisionDeviceInfo* device_info, int camera_idx)
{
    std::cout << "Camera: " << camera_idx << std::endl;
    std::cout << "userDefinedName: " << device_info[camera_idx].userDefinedName << std::endl;
    std::cout << "macAddress: " << device_info[camera_idx].macAddress << std::endl;
    std::cout << "deviceMode: " << device_info[camera_idx].deviceMode << std::endl;
    std::cout << "serialNumber: " << device_info[camera_idx].serialNumber << std::endl;
    std::cout << "macAddress: " << device_info[camera_idx].macAddress << std::endl;
    std::cout << "currentIp: " << device_info[camera_idx].currentIp << std::endl;
    std::cout << "currentSubnetMask: " << device_info[camera_idx].currentSubnetMask << std::endl;
    std::cout << "defaultGateway: " << device_info[camera_idx].defaultGateway << std::endl;
    std::cout << "nic.ip4Address: " << device_info[camera_idx].nic.ip4Address << std::endl;
}

// A function to reset to factory defaults for running eSDK examples
//  TODO: many thing doesn't work with this emergent native code
void configure_factory_defaults(Emergent::CEmergentCamera *camera, CameraParams* camera_params)
{
    unsigned int width_max, height_max, param_val_max;
    // const unsigned long enumBufferSize = 1000;
    // unsigned long enumBufferSizeReturn = 0;
    // char enumBuffer[enumBufferSize];
    // char* next_token;
    // char* enumMember = strtok_s(enumBuffer, ",", &next_token);

    // Order is important as param max/mins get updated.
    // check_camera_errors(Emergent::EVT_CameraGetEnumParamRange(camera, "PixelFormat", enumBuffer, enumBufferSize, &enumBufferSizeReturn));
    // check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "PixelFormat", enumMember));
    //  check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "FrameRate", 30));

    check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "OffsetX", 0), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "OffsetY", 0), camera_params->camera_serial.c_str());

    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Width", &width_max), camera_params->camera_serial.c_str());
    // check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera,    "Width", width_max));

    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Height", &height_max), camera_params->camera_serial.c_str());
    // check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera,    "Height", height_max));

    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "AcquisitionMode", "Continuous"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "AcquisitionFrameCount", 1), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerSelector", "AcquisitionStart"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerMode", "Off"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerSource", "Software"), camera_params->camera_serial.c_str());
    // check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "BufferMode", "Off"));
    // check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "BufferNum", 0));

    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "GevSCPSPacketSize", &param_val_max), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "GevSCPSPacketSize", param_val_max), camera_params->camera_serial.c_str());

    // check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "Gain", 1000));
    // check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "Offset", 0));

    check_camera_errors(Emergent::EVT_CameraSetBoolParam(camera, "LUTEnable", false), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetBoolParam(camera, "AutoGain", false), camera_params->camera_serial.c_str());
}

void get_senstemp_range(Emergent::CEmergentCamera *camera, CameraParams *camera_params)
{
    EVT_CameraGetInt32ParamMax(camera, "SensTemp", &camera_params->sens_temp_max);
    EVT_CameraGetInt32ParamMin(camera, "SensTemp", &camera_params->sens_temp_min);
}

void get_senstemp_value(Emergent::CEmergentCamera *camera, CameraParams *camera_params)
{
    int return_value = EVT_CameraGetInt32Param(camera, "SensTemp", &camera_params->sens_temp);
    if(return_value != 0)
    {
        printf("get_senstemp_value: Error\n");
    }
}

void update_gain_value(Emergent::CEmergentCamera *camera, int gain_val, CameraParams *camera_params)
{
    EVT_CameraGetUInt32ParamMax(camera, "Gain", &camera_params->gain_max);
    EVT_CameraGetUInt32ParamMin(camera, "Gain", &camera_params->gain_min);
    EVT_CameraGetUInt32ParamInc(camera, "Gain", &camera_params->gain_inc);
    if (gain_val >= static_cast<int>(camera_params->gain_min) && gain_val <= static_cast<int>(camera_params->gain_max))
    {
        EVT_CameraSetUInt32Param(camera, "Gain", gain_val);
        camera_params->gain = gain_val;
    }
}

void update_color_temperature(Emergent::CEmergentCamera *camera, std::string color_string, CameraParams *camera_params)
{
    const char *color_temp = color_string.c_str();
    check_camera_errors(EVT_CameraSetEnumParam(camera, "ColorTemp", color_temp), camera_params->camera_serial.c_str());
    camera_params->color_temp = color_string;
}

void update_focus_value(Emergent::CEmergentCamera *camera, int focus_value, CameraParams *camera_params)
{
    if (camera_params && !camera_params->lens_control_enabled) {
        std::cout << camera_params->camera_serial
                  << " [update_focus_value] Lens control disabled; skipping focus write."
                  << std::endl;
        return;
    }
    (void)set_focus_value_checked(camera, focus_value, camera_params, "update_focus_value");
}

void update_iris_value(Emergent::CEmergentCamera *camera, int iris_value, CameraParams *camera_params)
{
    if (camera_params && !camera_params->lens_control_enabled) {
        std::cout << camera_params->camera_serial
                  << " [update_iris_value] Lens control disabled; skipping iris write."
                  << std::endl;
        return;
    }
    (void)set_iris_value_checked(camera, iris_value, camera_params, "update_iris_value");
}


void update_width_value(Emergent::CEmergentCamera *camera, int width_val, CameraParams *camera_params)
{
    EVT_CameraGetUInt32ParamMax(camera, "Width", &camera_params->width_max);
    EVT_CameraGetUInt32ParamMin(camera, "Width", &camera_params->width_min);
    EVT_CameraGetUInt32ParamInc(camera, "Width", &camera_params->width_inc);
    if (width_val >= static_cast<int>(camera_params->width_min) && width_val <= static_cast<int>(camera_params->width_max))
    {
        EVT_CameraSetUInt32Param(camera, "Width", width_val);
        camera_params->width = width_val;
    }
}

void update_height_value(Emergent::CEmergentCamera *camera, int height_val, CameraParams *camera_params)
{
    EVT_CameraGetUInt32ParamMax(camera, "Height", &camera_params->height_max);
    EVT_CameraGetUInt32ParamMin(camera, "Height", &camera_params->height_min);
    EVT_CameraGetUInt32ParamInc(camera, "Height", &camera_params->height_inc);
    if (height_val >= static_cast<int>(camera_params->height_min) && height_val <= static_cast<int>(camera_params->height_max))
    {
        EVT_CameraSetUInt32Param(camera, "Height", height_val);
        camera_params->height = height_val;
    }
}


void update_exposure_value(Emergent::CEmergentCamera *camera, int exposure_val, CameraParams *camera_params)
{
    EVT_CameraGetUInt32ParamMax(camera, "Exposure", &camera_params->exposure_max);
    EVT_CameraGetUInt32ParamMin(camera, "Exposure", &camera_params->exposure_min);
    EVT_CameraGetUInt32ParamInc(camera, "Exposure", &camera_params->exposure_inc);

    if (exposure_val >= static_cast<int>(camera_params->exposure_min) && exposure_val <= static_cast<int>(camera_params->exposure_max))
    {
        EVT_CameraSetUInt32Param(camera, "Exposure", exposure_val);
        camera_params->exposure = exposure_val;
    }
}


void update_exposure_framerate_value(Emergent::CEmergentCamera *camera, int exposure_val, int* frame_rate_val, CameraParams *camera_params)
{
    get_camera_uint32_param_range(
        camera,
        "Exposure",
        &camera_params->exposure_min,
        &camera_params->exposure_max,
        &camera_params->exposure_inc);

    if (exposure_val >= static_cast<int>(camera_params->exposure_min) && exposure_val <= static_cast<int>(camera_params->exposure_max))
    {
        EVT_CameraSetUInt32Param(camera, "Exposure", exposure_val);
        camera_params->exposure = exposure_val;
    
        // framerate is correlated with exposure
        get_camera_uint32_param_range(
            camera,
            "FrameRate",
            &camera_params->frame_rate_min,
            &camera_params->frame_rate_max,
            &camera_params->frame_rate_inc);

        if (*frame_rate_val < static_cast<int>(camera_params->frame_rate_min)) {
            *frame_rate_val = camera_params->frame_rate_min;
        } else if (*frame_rate_val > static_cast<int>(camera_params->frame_rate_max)) {
            *frame_rate_val = camera_params->frame_rate_max;
        }

        EVT_CameraSetUInt32Param(camera, "FrameRate", *frame_rate_val);
        camera_params->frame_rate = *frame_rate_val;
    }
}


void update_frame_rate_value(Emergent::CEmergentCamera *camera, int frame_rate_val, CameraParams *camera_params)
{
    if (camera == nullptr || camera_params == nullptr) {
        return;
    }

    const unsigned int requested =
        frame_rate_val < 0 ? 0u : static_cast<unsigned int>(frame_rate_val);
    const bool have_range = get_camera_uint32_param_range(
        camera,
        "FrameRate",
        &camera_params->frame_rate_min,
        &camera_params->frame_rate_max,
        &camera_params->frame_rate_inc);
    if (!have_range) {
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set FAIL: unable to query range."
                  << std::endl;
        return;
    }

    auto range_string = [&]() {
        std::ostringstream oss;
        oss << "[" << camera_params->frame_rate_min << ","
            << camera_params->frame_rate_max << "]";
        if (camera_params->frame_rate_inc > 0) {
            oss << " inc=" << camera_params->frame_rate_inc;
        }
        return oss.str();
    };

    auto update_readback = [&]() -> bool {
        unsigned int readback = 0;
        const EVT_ERROR get_err = EVT_CameraGetUInt32Param(camera, "FrameRate", &readback);
        if (get_err != EVT_SUCCESS) {
            std::cout << camera_params->camera_serial
                      << " [update_frame_rate_value] FrameRate readback failed: "
                      << get_evt_error_string(get_err)
                      << std::endl;
            return false;
        }
        camera_params->frame_rate = readback;
        return true;
    };

    if (requested < camera_params->frame_rate_min ||
        requested > camera_params->frame_rate_max) {
        const bool readback_ok = update_readback();
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set WARN: requested="
                  << requested
                  << " out of range=" << range_string();
        if (readback_ok) {
            std::cout << " current=" << camera_params->frame_rate;
        }
        std::cout << std::endl;
        return;
    }

    const EVT_ERROR set_err = EVT_CameraSetUInt32Param(camera, "FrameRate", requested);
    if (set_err != EVT_SUCCESS) {
        const bool readback_ok = update_readback();
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set FAIL: requested="
                  << requested
                  << " error=" << get_evt_error_string(set_err)
                  << " range=" << range_string();
        if (readback_ok) {
            std::cout << " current=" << camera_params->frame_rate;
        }
        std::cout << std::endl;
        return;
    }

    unsigned int readback = 0;
    const EVT_ERROR get_err = EVT_CameraGetUInt32Param(camera, "FrameRate", &readback);
    if (get_err != EVT_SUCCESS) {
        camera_params->frame_rate = requested;
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set WARN: requested="
                  << requested
                  << " set ok, readback failed: " << get_evt_error_string(get_err)
                  << " range=" << range_string()
                  << std::endl;
        return;
    }

    camera_params->frame_rate = readback;
    if (readback == requested) {
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set PASS: requested="
                  << requested
                  << " readback=" << readback
                  << " range=" << range_string()
                  << std::endl;
    } else {
        std::cout << camera_params->camera_serial
                  << " [update_frame_rate_value] FrameRate set WARN: requested="
                  << requested
                  << " readback=" << readback
                  << " range=" << range_string()
                  << std::endl;
    }
}

void update_offsetX_value(Emergent::CEmergentCamera *camera, int OFFSET_X_VAL, CameraParams *camera_params)
{
    // Set ROI OffsetX. Now that Width changed we need to check new OffsetX limits
    EVT_CameraGetUInt32ParamMax(camera, "OffsetX", &camera_params->offsetx_max);
    printf("OffsetX Max: \t\t%d\n", camera_params->offsetx_max);
    EVT_CameraGetUInt32ParamMin(camera, "OffsetX", &camera_params->offsetx_min);
    printf("OffsetX Min: \t\t%d\n", camera_params->offsetx_min);
    EVT_CameraGetUInt32ParamInc(camera, "OffsetX", &camera_params->offsetx_inc);
    printf("OffsetX Inc: \t\t%d\n", camera_params->offsetx_inc);

    if (OFFSET_X_VAL >= static_cast<int>(camera_params->offsetx_min) && OFFSET_X_VAL <= static_cast<int>(camera_params->offsetx_max))
    {
        EVT_CameraSetUInt32Param(camera, "OffsetX", OFFSET_X_VAL);
        camera_params->offsetx = OFFSET_X_VAL;
        printf("OffsetX Set: \t\t%d\n", OFFSET_X_VAL);
    }
}

void update_offsetY_value(Emergent::CEmergentCamera *camera, int OFFSET_Y_VAL, CameraParams *camera_params)
{
    // Set ROI OffsetY. Now that Height changed we need to check new OffsetY limits
    EVT_CameraGetUInt32ParamMax(camera, "OffsetY", &camera_params->offsety_max);
    printf("OffsetY Max: \t\t%d\n", camera_params->offsety_max);
    EVT_CameraGetUInt32ParamMin(camera, "OffsetY", &camera_params->offsety_min);
    printf("OffsetY Min: \t\t%d\n", camera_params->offsety_min);
    EVT_CameraGetUInt32ParamInc(camera, "OffsetY", &camera_params->offsety_inc);
    printf("OffsetY Inc: \t\t%d\n", camera_params->offsety_inc);

    if (OFFSET_Y_VAL >= static_cast<int>(camera_params->offsety_min) && OFFSET_Y_VAL <= static_cast<int>(camera_params->offsety_max))
    {
        EVT_CameraSetUInt32Param(camera, "OffsetY", OFFSET_Y_VAL);
        camera_params->offsety = OFFSET_Y_VAL;
        printf("OffsetY Set: \t\t%d\n", OFFSET_Y_VAL);
    }
}

void open_camera_with_params(Emergent::CEmergentCamera *camera,
                             GigEVisionDeviceInfo *device_info,
                             CameraParams *camera_params,
                             const char* context)
{
    // TODO: open camera using xml file after explored on camera settings
    // EVT_CameraOpen(&camera, &deviceInfo[camera_index], XML_FILE);

    if(camera_params->gpu_direct){
        camera->gpuDirectDeviceId = camera_params->gpu_id;
    }

    log_camera_gpudirect_state("camera_open", context, camera, camera_params);

    check_camera_errors(EVT_CameraOpen(camera, device_info), camera_params->camera_serial.c_str());

    configure_factory_defaults(camera, camera_params);

    unsigned int width_max, height_max;
    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Height", &height_max), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Width", &width_max), camera_params->camera_serial.c_str());
    printf("Resolution: \t\t%d x %d\n", width_max, height_max);

    update_width_value(camera, camera_params->width, camera_params);
    update_height_value(camera, camera_params->height, camera_params);

    update_offsetX_value(camera, camera_params->offsetx, camera_params);
    update_offsetY_value(camera, camera_params->offsety, camera_params);

    const char *pixel_format = camera_params->pixel_format.c_str();
    check_camera_errors(EVT_CameraSetEnumParam(camera, "PixelFormat", pixel_format), camera_params->camera_serial.c_str());
    printf("PixelFormat: \t\t%s\n", pixel_format);

    if (camera_params->color)
    {
        const char *color_temp = camera_params->color_temp.c_str();
        check_camera_errors(EVT_CameraSetEnumParam(camera, "ColorTemp", color_temp), camera_params->camera_serial.c_str());
    }

    // check_camera_errors(EVT_CameraSetUInt32Param(camera, "Gain", camera_params.gain));
    update_gain_value(camera, camera_params->gain, camera_params);

    // check_camera_errors(EVT_CameraSetUInt32Param(camera, "Exposure", camera_params->exposure));
    update_exposure_value(camera, camera_params->exposure, camera_params);

    // unsigned int frame_rate_max;
    // check_camera_errors(EVT_CameraGetUInt32ParamMax(camera, "FrameRate", &frame_rate_max));
    // printf("FrameRate Max: \t\t%d\n", frame_rate_max);

    // check_camera_errors(EVT_CameraSetUInt32Param(camera, "FrameRate", camera_params->frame_rate));
    // printf("FrameRate Set to: \t%d\n", camera_params.frame_rate);
    update_frame_rate_value(camera, camera_params->frame_rate, camera_params);
    if (camera_params->lens_control_enabled) {
        const bool focus_ok = set_focus_value_checked(camera, static_cast<int>(camera_params->focus), camera_params, "open_camera_with_params");
        const int configured_iris = static_cast<int>(camera_params->iris);
        const bool iris_ok = set_startup_iris_value_checked(camera, configured_iris, camera_params, "open_camera_with_params");
        std::cout << camera_params->camera_serial
                  << " [open_camera_with_params] Lens init summary: focus=" << (focus_ok ? "PASS" : "FAIL")
                  << " iris=" << (iris_ok ? "PASS" : "FAIL")
                  << std::endl;
    } else {
        std::cout << camera_params->camera_serial
                  << " [open_camera_with_params] Lens control disabled; skipping startup focus/iris writes."
                  << std::endl;
    }

    apply_configured_runtime_mode(camera, camera_params, "open_camera_with_params");
}

void update_camera_params(Emergent::CEmergentCamera *camera, GigEVisionDeviceInfo *device_info, CameraParams *camera_params)
{
    camera_params->gpu_direct= false;
    camera_params->gpu_id = 0;
    check_camera_errors(EVT_CameraOpen(camera, device_info), camera_params->camera_serial.c_str());
    configure_factory_defaults(camera, camera_params);
    unsigned int width_max, height_max;
    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Height", &height_max), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "Height", &camera_params->height_max);
    EVT_CameraGetUInt32ParamMin(camera, "Height", &camera_params->height_min);
    EVT_CameraGetUInt32ParamInc(camera, "Height", &camera_params->height_inc);
    check_camera_errors(Emergent::EVT_CameraGetUInt32ParamMax(camera, "Width", &width_max), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "Width", &camera_params->width_max);
    EVT_CameraGetUInt32ParamMin(camera, "Width", &camera_params->width_min);
    EVT_CameraGetUInt32ParamInc(camera, "Width", &camera_params->width_inc);
    printf("Resolution: \t\t%d x %d\n", width_max, height_max);
    camera_params->width = width_max;
    camera_params->height = height_max;
    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "FrameRate", &camera_params->frame_rate), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "FrameRate", &camera_params->frame_rate_max);
    EVT_CameraGetUInt32ParamMin(camera, "FrameRate", &camera_params->frame_rate_min);
    EVT_CameraGetUInt32ParamInc(camera, "FrameRate", &camera_params->frame_rate_inc);

    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "Exposure", &camera_params->exposure), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "Exposure", &camera_params->exposure_max);
    EVT_CameraGetUInt32ParamMin(camera, "Exposure", &camera_params->exposure_min);
    EVT_CameraGetUInt32ParamInc(camera, "Exposure", &camera_params->exposure_inc);
    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "Gain", &camera_params->gain), camera_params->camera_serial.c_str());
    std::cout << "Gain: " << camera_params->gain << std::endl;
    EVT_CameraGetUInt32ParamMax(camera, "Gain", &camera_params->gain_max);
    std::cout << "Gain max: " << camera_params->gain_max << std::endl;
    EVT_CameraGetUInt32ParamMin(camera, "Gain", &camera_params->gain_min);
    EVT_CameraGetUInt32ParamInc(camera, "Gain", &camera_params->gain_inc);
    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "Iris", &camera_params->iris), camera_params->camera_serial.c_str());
    std::cout << "Iris: " << camera_params->iris << std::endl;
    EVT_CameraGetUInt32ParamMax(camera, "Iris", &camera_params->iris_max);
    std::cout << "Iris max: " << camera_params->iris_max << std::endl;
    EVT_CameraGetUInt32ParamMin(camera, "Iris", &camera_params->iris_min);
    std::cout << "Iris min: " << camera_params->iris_min << std::endl;
    EVT_CameraGetUInt32ParamInc(camera, "Iris", &camera_params->iris_inc);
    std::cout << "Iris inc: " << camera_params->iris_inc << std::endl;
    ensure_focus_range_ready(camera, camera_params, "update_camera_params");
    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "Focus", &camera_params->focus), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "Focus", &camera_params->focus_max);
    EVT_CameraGetUInt32ParamMin(camera, "Focus", &camera_params->focus_min);
    EVT_CameraGetUInt32ParamInc(camera, "Focus", &camera_params->focus_inc);
    std::cout << camera_params->camera_serial
              << " [update_camera_params] Lens range summary: focus=[" << camera_params->focus_min
              << "," << camera_params->focus_max
              << "] iris=[" << camera_params->iris_min
              << "," << camera_params->iris_max
              << "] focus_uart_bootstrap=" << (camera_params->focus_uart_bootstrap ? "true" : "false")
              << std::endl;

    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "OffsetY", &camera_params->offsety), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "OffsetY", &camera_params->offsety_max);
    EVT_CameraGetUInt32ParamMin(camera, "OffsetY", &camera_params->offsety_min);
    EVT_CameraGetUInt32ParamInc(camera, "OffsetY", &camera_params->offsety_inc);

    check_camera_errors(Emergent::EVT_CameraGetUInt32Param(camera, "OffsetX", &camera_params->offsetx), camera_params->camera_serial.c_str());
    EVT_CameraGetUInt32ParamMax(camera, "OffsetX", &camera_params->offsetx_max);
    EVT_CameraGetUInt32ParamMin(camera, "OffsetX", &camera_params->offsetx_min);
    EVT_CameraGetUInt32ParamInc(camera, "OffsetX", &camera_params->offsetx_inc);

    const unsigned long enum_buffer_size = 1000;
    unsigned long enum_buffer_size_return = 0;
    char enumBuffer[enum_buffer_size];

    EVT_CameraGetEnumParamRange(camera, "PixelFormat", enumBuffer, enum_buffer_size, &enum_buffer_size_return);
    std::cout << "PixelFormat: " << enumBuffer << std::endl;
    char* enum_member = strtok_s(enumBuffer, ",", &next_token);
    check_camera_errors(EVT_CameraSetEnumParam(camera, "PixelFormat", enum_member), camera_params->camera_serial.c_str());
    camera_params->pixel_format = std::string(enum_member);

    if (camera_params->pixel_format == "Mono8") {
        camera_params->color = false;
    } else {
        camera_params->color = true;
    }

    if (camera_params->color) {
        EVT_CameraGetEnumParamRange(camera, "ColorTemp", enumBuffer, enum_buffer_size, &enum_buffer_size_return);
        std::cout << "ColorTemp: " << enumBuffer << std::endl;
        char* enum_member = strtok_s(enumBuffer, ",", &next_token);
        check_camera_errors(EVT_CameraSetEnumParam(camera, "ColorTemp", enum_member), camera_params->camera_serial.c_str());
        camera_params->color_temp = std::string(enum_member);
    }
}

// **********************************************sync*****************************************************
bool camera_sync_mode_uses_ptp(const CameraParams* camera_params)
{
    return normalize_sync_mode(camera_params) == "ptp_gate";
}

void ptp_camera_sync(Emergent::CEmergentCamera *camera, CameraParams *camera_params)
{
    // ptp triggering configuration settings
    const std::string ptp_mode = resolved_ptp_mode(camera_params);
    std::string acquisition_mode = camera_params->ptp_gate_acquisition_mode;
    std::transform(acquisition_mode.begin(), acquisition_mode.end(), acquisition_mode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (acquisition_mode != "continuous") {
        acquisition_mode = "multiframe";
    }
    check_camera_errors(EVT_CameraSetEnumParam(camera, "TriggerSelector", camera_params->trigger_selector.c_str()), camera_params->camera_serial.c_str());
    check_camera_errors(EVT_CameraSetEnumParam(camera, "TriggerSource", "Software"), camera_params->camera_serial.c_str());
    check_camera_errors(
        EVT_CameraSetEnumParam(
            camera,
            "AcquisitionMode",
            acquisition_mode == "continuous" ? "Continuous" : "MultiFrame"),
        camera_params->camera_serial.c_str());
    check_camera_errors(EVT_CameraSetUInt32Param(camera, "AcquisitionFrameCount", 1), camera_params->camera_serial.c_str());
    check_camera_errors(EVT_CameraSetEnumParam(camera, "TriggerMode", "On"), camera_params->camera_serial.c_str());
    check_camera_errors(EVT_CameraSetEnumParam(camera, "PtpMode", ptp_mode.c_str()), camera_params->camera_serial.c_str());
    log_ptp_camera_sync_readback(camera, camera_params);
}

void ptp_sync_off(Emergent::CEmergentCamera *camera, CameraParams *camera_params)
{
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "AcquisitionMode", "Continuous"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetUInt32Param(camera, "AcquisitionFrameCount", 1), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerSelector", "AcquisitionStart"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerMode", "Off"), camera_params->camera_serial.c_str());
    check_camera_errors(Emergent::EVT_CameraSetEnumParam(camera, "TriggerSource", "Software"), camera_params->camera_serial.c_str());
}

// use one camera to get the PTP time, TODO: use linux to get current GMT time in seconds
unsigned long long get_current_PTP_time(Emergent::CEmergentCamera *camera)
{

    char ptp_status[100];
    unsigned long ptp_status_sz_ret;
    unsigned int ptp_time_high, ptp_time_low;
    // need to open the camera to get ptp time?
    EVT_CameraGetEnumParam(camera, "PtpStatus", ptp_status, sizeof(ptp_status), &ptp_status_sz_ret);
    printf("PTP Status: %s\n", ptp_status);

    // Get and print current time.
    EVT_CameraExecuteCommand(camera, "GevTimestampControlLatch");
    EVT_CameraGetUInt32Param(camera, "GevTimestampValueHigh", &ptp_time_high);
    EVT_CameraGetUInt32Param(camera, "GevTimestampValueLow", &ptp_time_low);
    unsigned long long ptp_time = (((unsigned long long)(ptp_time_high)) << 32) | ((unsigned long long)(ptp_time_low));
    return ptp_time;
}

// test GPO by toggling polarity in manual mode, after open camera, before open streaming
void test_gpo_manual_toggle(Emergent::CEmergentCamera *camera)
{
    unsigned int count;
    char gpo_str[20];
    bool gpo_polarity = 1;

    // Test GPOs by toggling polarity in manual mode.
    for (count = 0; count < 4; count++)
    {

        sprintf(gpo_str, "GPO_%d_Mode", count);
        EVT_CameraSetEnumParam(camera, gpo_str, "GPO");
    }

    for (count = 0; count < 4; count++)
    {
        printf("Toggling GPO %d\t\t", count);
        sprintf(gpo_str, "GPO_%d_Polarity", count);

        for (int blink_count = 0; blink_count < 20; blink_count++)
        {
            EVT_CameraSetBoolParam(camera, gpo_str, gpo_polarity);
            gpo_polarity = !gpo_polarity;
            usleep(100 * 1000);
            printf(".");
            fflush(stdout);
        }
        printf("\n");
    }
}

void close_camera(Emergent::CEmergentCamera *camera, CameraParams *camera_params)
{
    check_camera_errors(EVT_CameraClose(camera), camera_params->camera_serial.c_str());
    printf("\nClose Camera: \t\tCamera Closed\n");
}

void set_frame_buffer(Emergent::CEmergentFrame *evt_frame, CameraParams *camera_params)
{
    // Three params used for memory allocation. Worst case covers all models so no recompilation required.
    evt_frame->size_x = camera_params->width;
    evt_frame->size_y = camera_params->height;

    std::string pixel_format = camera_params->pixel_format;
    if (pixel_format == "BayerRG8")
    {
        evt_frame->pixel_type = GVSP_PIX_BAYRG8;
    }
    else if (pixel_format == "RGB8Packed")
    {
        evt_frame->pixel_type = GVSP_PIX_RGB8;
    }
    else if (pixel_format == "BGR8Packed")
    {
        evt_frame->pixel_type = GVSP_PIX_BGR8;
    }
    else if (pixel_format == "YUV411Packed")
    {
        evt_frame->pixel_type = GVSP_PIX_YUV411_PACKED;
    }
    else if (pixel_format == "YUV422Packed")
    {
        evt_frame->pixel_type = GVSP_PIX_YUV422_PACKED;
    }
    else if (pixel_format == "YUV444Packed")
    {
        evt_frame->pixel_type = GVSP_PIX_YUV444_PACKED;
    }
    else if (pixel_format == "BayerGB8")
    {
        evt_frame->pixel_type = GVSP_PIX_BAYGB8;
    }
    else // Good for default case which covers color and mono as same size bytes/pixel.
    {    // Note that these settings are used for memory alloc only.
        evt_frame->pixel_type = GVSP_PIX_MONO8;
    }
}

void camera_open_stream(Emergent::CEmergentCamera *camera, CameraParams *camera_params, const char* context)
{
    if (camera_params && camera_params->gpu_direct) {
        const cudaError_t set_device_err = cudaSetDevice(camera_params->gpu_id);
        if (set_device_err != cudaSuccess) {
            std::cerr << "[CAMERA][GPUDIRECT] failed to set CUDA device "
                      << camera_params->gpu_id
                      << " before stream open: "
                      << cudaGetErrorString(set_device_err)
                      << std::endl;
        } else {
            // Force the primary context to exist on the thread that opens the
            // GPUDirect stream, matching the GUI's main-thread CUDA setup more
            // closely.
            const cudaError_t init_err = cudaFree(nullptr);
            if (init_err != cudaSuccess) {
                std::cerr << "[CAMERA][GPUDIRECT] failed to initialize CUDA context on device "
                          << camera_params->gpu_id
                          << " before stream open: "
                          << cudaGetErrorString(init_err)
                          << std::endl;
            }
        }
    }
    log_camera_gpudirect_state("camera_open_stream_attempt", context, camera, camera_params);
    const EVT_ERROR err = EVT_CameraOpenStream(camera);
    if (err != EVT_SUCCESS) {
        log_camera_gpudirect_state("camera_open_stream_failure", context, camera, camera_params, err);
    }
    check_camera_errors(err, camera_params->camera_serial.c_str());
}

void allocate_frame_buffer(
    Emergent::CEmergentCamera *camera,
    Emergent::CEmergentFrame *evt_frame,
    CameraParams *camera_params,
    int buffer_size,
    int buffer_mode)
{
    for (int frame_count = 0; frame_count < buffer_size; frame_count++)
    {
        set_frame_buffer(&evt_frame[frame_count], camera_params);
        check_camera_errors(EVT_AllocateFrameBuffer(camera, &evt_frame[frame_count], buffer_mode), camera_params->camera_serial.c_str());
        check_camera_errors(EVT_CameraQueueFrame(camera, &evt_frame[frame_count]), camera_params->camera_serial.c_str());
    }
}

void allocate_frame_reorder_buffer(Emergent::CEmergentCamera *camera, Emergent::CEmergentFrame *frame_reorder, CameraParams *camera_params)
{
    set_frame_buffer(frame_reorder, camera_params);
    frame_reorder->convertColor = EVT_COLOR_CONVERT_NONE;
    frame_reorder->convertBitDepth = EVT_CONVERT_NONE;
    check_camera_errors(EVT_AllocateFrameBuffer(camera, frame_reorder, EVT_FRAME_BUFFER_DEFAULT), camera_params->camera_serial.c_str());
}

void destroy_frame_buffer(Emergent::CEmergentCamera *camera, Emergent::CEmergentFrame *evt_frame, int buffer_size, CameraParams *camera_params)
{
    for (int frame_count = 0; frame_count < buffer_size; frame_count++)
    {
        check_camera_errors(EVT_ReleaseFrameBuffer(camera, &evt_frame[frame_count]), camera_params->camera_serial.c_str());
    }

    // Host side tear down for stream.
    // EVT_CameraCloseStream(camera);
}

// Use this function with caution, need to reintiate the GigEVisionDeviceInfo after changing the camera ip. non persistent
void change_camera_ip(GigEVisionDeviceInfo *device_info, const char *new_ip, CameraParams *camera_params)
{
    const char *mac_address = device_info->macAddress;
    const char *subnet_mask = device_info->currentSubnetMask;
    const char *default_gateway = device_info->defaultGateway;
    check_camera_errors(Emergent::EVT_ForceIPEx(mac_address, new_ip, subnet_mask, default_gateway), camera_params->camera_serial.c_str());
}

// Use this function with caution, need to reintiate the GigEVisionDeviceInfo after changing the camera ip.
void change_camera_ip_persistent(GigEVisionDeviceInfo *device_info, Emergent::CEmergentCamera *camera, const char *new_ip, CameraParams *camera_params)
{
    const char *subnet_mask = device_info->currentSubnetMask;
    const char *default_gateway = device_info->defaultGateway;
    check_camera_errors(Emergent::EVT_IPConfig(camera, true, new_ip, subnet_mask, default_gateway), camera_params->camera_serial.c_str());
}

void quick_print_camera(GigEVisionDeviceInfo *device_info, int camera_idx)
{
    std::cout << "camera: " << camera_idx << ", serialNumber: " << device_info[camera_idx].serialNumber << ", currentIp: " << device_info[camera_idx].currentIp << ", nicIp: " << device_info[camera_idx].nic.ip4Address << std::endl;
}

int scan_cameras(int max_cameras, GigEVisionDeviceInfo *device_info)
{
    unsigned int listcam_buf_size = max_cameras;
    unsigned int count;

    Emergent::EVT_ListDevices(device_info, &listcam_buf_size, &count);
    if (count == 0)
    {
        printf("Enumerate Cameras: \tNo cameras found.\n");
        return 0;
    }
    else
    {
        return count;
    }
}

template <typename T>
std::vector<size_t> sort_indexes(const std::vector<T> &v)
{
    // initialize original index locations
    std::vector<size_t> idx(v.size());
    std::iota(idx.begin(), idx.end(), 0);

    // sort indexes based on comparing values in v
    // using std::stable_sort instead of std::sort
    // to avoid unnecessary index re-orderings
    // when v contains elements of equal values
    stable_sort(idx.begin(), idx.end(),
                [&v](size_t i1, size_t i2)
                { return v[i1] < v[i2]; });

    return idx;
}

void sort_cameras_ip(GigEVisionDeviceInfo *device_info, GigEVisionDeviceInfo *sorted_device_info, int cam_count)
{
    std::vector<std::string> camera_ips;
    for (int i = 0; i < cam_count; i++)
    {
        camera_ips.push_back(std::string(device_info[i].currentIp));
    }

    int j = 0;
    for (auto i : sort_indexes(camera_ips))
    {
        sorted_device_info[j] = device_info[i];
        j++;
    }
}

int order_for_test_rig(int max_cameras, GigEVisionDeviceInfo *device_info, GigEVisionDeviceInfo *ordered_device_info)
{
    unsigned int listcam_buf_size = max_cameras;
    unsigned int count;

    Emergent::EVT_ListDevices(device_info, &listcam_buf_size, &count);

    if (count == 0)
    {
        printf("Enumerate Cameras: \tNo cameras found. Exiting program.\n");
        return 0;
    }
    else
    {
        for (unsigned int i = 0; i < count; i++)
        {

            if (strcmp(device_info[i].serialNumber, "2002490") == 0)
            {
                ordered_device_info[0] = device_info[i];
            }
            else if (strcmp(device_info[i].serialNumber, "2002496") == 0)
            {
                ordered_device_info[1] = device_info[i];
            }
            else if (strcmp(device_info[i].serialNumber, "2002488") == 0)
            {
                ordered_device_info[2] = device_info[i];
            }
            else if (strcmp(device_info[i].serialNumber, "2002489") == 0)
            {
                ordered_device_info[3] = device_info[i];
            }

            // center one
            else if (strcmp(device_info[i].serialNumber, "2002494") == 0)
            {
                ordered_device_info[4] = device_info[i];
            }
        }

        printf("Found %d cameras. \n", count);
        for (unsigned int i = 0; i < count; i++)
        {
            quick_print_camera(device_info, i);
        }
        return count;
    }
}
