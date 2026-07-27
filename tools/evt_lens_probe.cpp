#include <EmergentCameraAPIs.h>
#include <EvtParamAttribute.h>
#include <emergenterrors.h>
#include <gigevisiondeviceinfo.h>

#include "camera_sensor_pipeline_state.h"
#include "fsuid_guard.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Emergent;

namespace {

struct Options {
    std::string serial;
    int index = 0;
    bool index_set = false;
    std::string xml_path;
    bool list_only = false;
    bool sensor_pipeline = false;
    std::string sensor_pipeline_json_path;
    std::string genicam_xml_output_path;

    bool exercise_genicam = false;
    bool set_focus_target = false;
    unsigned int focus_target = 0;
    bool set_iris_target = false;
    unsigned int iris_target = 0;
    bool restore_nir_strobe_pulse = false;

    bool enable_uart = false;
    std::string uart_baud = "B_9600";
    unsigned int uart_data_bits = 8;
    unsigned int uart_stop_bits = 1;
    std::string uart_tx_mode = "Test_Gen_Uart_Txd";
    bool uart_loopback = false;
    int loopback_timeout_ms = 3000;
    bool keep_uart_config = false;
    bool show_help = false;
};

enum class Status {
    kNotRun,
    kPass,
    kFail
};

struct NodeProbeResult {
    bool exists = false;
    EVT_ERROR attr_err = EVT_SUCCESS;
    EvtParamDataType data_type = EDataTypeUnsupported;
};

struct ProbeSummary {
    bool focus_found = false;
    bool iris_found = false;
    bool uart_enable_found = false;
    bool uart_baud_found = false;
    bool uart_data_bits_found = false;
    bool uart_stop_bits_found = false;

    Status genicam_exercise = Status::kNotRun;
    Status focus_target = Status::kNotRun;
    Status iris_target = Status::kNotRun;
    Status nir_strobe_pulse_restore = Status::kNotRun;
    Status uart_enable_sequence = Status::kNotRun;
    Status uart_loopback = Status::kNotRun;
};

struct UartRestoreState {
    bool captured_uart_enable = false;
    bool uart_enable_value = false;
    bool captured_gpo3_mode = false;
    std::string gpo3_mode;
};

std::string evt_error_to_string(EVT_ERROR err) {
    switch (err) {
        case EVT_SUCCESS: return "EVT_SUCCESS";
        case EVT_ENOENT: return "EVT_ENOENT";
        case EVT_ERROR_SRCH: return "EVT_ERROR_SRCH";
        case EVT_ERROR_INTR: return "EVT_ERROR_INTR";
        case EVT_ERROR_IO: return "EVT_ERROR_IO";
        case EVT_ERROR_BADF: return "EVT_ERROR_BADF";
        case EVT_ERROR_ECHILD: return "EVT_ERROR_ECHILD";
        case EVT_ERROR_AGAIN: return "EVT_ERROR_AGAIN";
        case EVT_ERROR_NOMEM: return "EVT_ERROR_NOMEM";
        case EVT_ERROR_ACCES: return "EVT_ERROR_ACCES";
        case EVT_ERROR_FAULT: return "EVT_ERROR_FAULT";
        case EVT_ERROR_EXIST: return "EVT_ERROR_EXIST";
        case EVT_ERROR_ENODEV: return "EVT_ERROR_ENODEV";
        case EVT_ERROR_INVAL: return "EVT_ERROR_INVAL";
        case EVT_ERROR_FBIG: return "EVT_ERROR_FBIG";
        case EVT_ERROR_BADFD: return "EVT_ERROR_BADFD";
        case EVT_ERROR_TIMEDOUT: return "EVT_ERROR_TIMEDOUT";
        case EVT_ERROR_ALREADY: return "EVT_ERROR_ALREADY";
        case EVT_ERROR_NOBUFS: return "EVT_ERROR_NOBUFS";
        case EVT_ERROR_NOT_SUPPORTED: return "EVT_ERROR_NOT_SUPPORTED";
        case EVT_ERROR_DEVICE_CONNECTED_ALRD: return "EVT_ERROR_DEVICE_CONNECTED_ALRD";
        case EVT_ERROR_DEVICE_NOT_CONNECTED: return "EVT_ERROR_DEVICE_NOT_CONNECTED";
        case EVT_ERROR_DEVICE_LOST_CONNECTION: return "EVT_ERROR_DEVICE_LOST_CONNECTION";
        case EVT_ERROR_GENICAM_ERROR: return "EVT_ERROR_GENICAM_ERROR";
        case EVT_ERROR_GENICAM_NOT_MATCH: return "EVT_ERROR_GENICAM_NOT_MATCH";
        case EVT_ERROR_GENICAM_OUT_OF_RANGE: return "EVT_ERROR_GENICAM_OUT_OF_RANGE";
        case EVT_ERROR_SOCK: return "EVT_ERROR_SOCK";
        case EVT_ERROR_GVCP_ACK: return "EVT_ERROR_GVCP_ACK";
        case EVT_ERROR_GVSP_DATA_CORRUPT: return "EVT_ERROR_GVSP_DATA_CORRUPT";
        case EVT_ERROR_NIC_LIB_INIT: return "EVT_ERROR_NIC_LIB_INIT";
        case EVT_ERROR_OS_OBTAIN_ADAPTER: return "EVT_ERROR_OS_OBTAIN_ADAPTER";
        case EVT_ERROR_SDK: return "EVT_ERROR_SDK";
        case EVT_GENERAL_ERROR: return "EVT_GENERAL_ERROR";
        default: {
            std::ostringstream oss;
            oss << "EVT_ERROR(" << static_cast<int>(err) << ")";
            return oss.str();
        }
    }
}

std::string data_type_to_string(EvtParamDataType type) {
    switch (type) {
        case EDataTypeUInt32: return "UInt32";
        case EDataTypeInt32: return "Int32";
        case EDataTypeBoolean: return "Boolean";
        case EDataTypeCommand: return "Command";
        case EDataTypeFloat: return "Float";
        case EDataTypeString: return "String";
        case EDataTypeEnumeration: return "Enumeration";
        case EDataTypeEnumEntry: return "EnumEntry";
        case EDataTypeCategory: return "Category";
        case EDataTypeUnsupported:
        default: return "Unsupported";
    }
}

std::string status_to_string(Status status) {
    switch (status) {
        case Status::kPass: return "PASS";
        case Status::kFail: return "FAIL";
        case Status::kNotRun:
        default: return "NOT_RUN";
    }
}

bool parse_int_arg(const std::string& s, int* out) {
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

bool parse_uint_arg(const std::string& s, unsigned int* out) {
    try {
        size_t consumed = 0;
        unsigned long v = std::stoul(s, &consumed, 10);
        if (consumed != s.size()) {
            return false;
        }
        if (v > static_cast<unsigned long>(std::numeric_limits<unsigned int>::max())) {
            return false;
        }
        *out = static_cast<unsigned int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --serial <serial>             Select camera by serial number.\n"
        << "  --index <n>                   Select camera by index from device list (default 0).\n"
        << "  --xml <path>                  Optional local XML path for EVT_CameraOpen.\n"
        << "  --list-only                   Only list discovered cameras.\n"
        << "  --sensor-pipeline             Getter-only sensor/ADC/pixel-pipeline inventory.\n"
        << "  --sensor-pipeline-json <path> Write the inventory as immutable JSON evidence.\n"
        << "  --genicam-xml-out <path>      Write the exact camera GenICam XML document.\n"
        << "  --exercise-genicam            Write Focus/Iris current values back and read back latency.\n"
        << "  --focus-target <value>        Set Focus to a specific value.\n"
        << "  --iris-target <value>         Set Iris to a specific value.\n"
        << "  --restore-nir-strobe-pulse   Set GPO_0 to active-low Exposure pulse mode and verify readback.\n"
        << "  --enable-uart                 Apply UART setup sequence (GPO_3 mode + Uart* nodes).\n"
        << "  --uart-baud <enum>            UART baud enum (default B_9600).\n"
        << "  --uart-data-bits <n>          UART data bits (default 8).\n"
        << "  --uart-stop-bits <n>          UART stop bits (default 1).\n"
        << "  --uart-tx-mode <enum>         GPO_3_Mode value for UART TX (default Test_Gen_Uart_Txd).\n"
        << "  --uart-loopback               Send 0x30-0x39 and read back UART RX fifo.\n"
        << "  --loopback-timeout-ms <ms>    UART loopback timeout in ms (default 3000).\n"
        << "  --keep-uart-config            Do not restore previous UartEnable/GPO_3_Mode on exit.\n"
        << "  --help                        Show this message.\n";
}

bool parse_args(int argc, char** argv, Options* options) {
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
        } else if (arg == "--xml") {
            const char* v = require_next("--xml");
            if (!v) return false;
            options->xml_path = v;
        } else if (arg == "--list-only") {
            options->list_only = true;
        } else if (arg == "--sensor-pipeline") {
            options->sensor_pipeline = true;
        } else if (arg == "--sensor-pipeline-json") {
            const char* v = require_next("--sensor-pipeline-json");
            if (!v) return false;
            options->sensor_pipeline_json_path = v;
            options->sensor_pipeline = true;
        } else if (arg == "--genicam-xml-out") {
            const char* v = require_next("--genicam-xml-out");
            if (!v) return false;
            options->genicam_xml_output_path = v;
            options->sensor_pipeline = true;
        } else if (arg == "--exercise-genicam") {
            options->exercise_genicam = true;
        } else if (arg == "--focus-target") {
            const char* v = require_next("--focus-target");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->focus_target)) {
                std::cerr << "Invalid --focus-target value: " << v << "\n";
                return false;
            }
            options->set_focus_target = true;
        } else if (arg == "--iris-target") {
            const char* v = require_next("--iris-target");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->iris_target)) {
                std::cerr << "Invalid --iris-target value: " << v << "\n";
                return false;
            }
            options->set_iris_target = true;
        } else if (arg == "--restore-nir-strobe-pulse") {
            options->restore_nir_strobe_pulse = true;
        } else if (arg == "--enable-uart") {
            options->enable_uart = true;
        } else if (arg == "--uart-baud") {
            const char* v = require_next("--uart-baud");
            if (!v) return false;
            options->uart_baud = v;
        } else if (arg == "--uart-data-bits") {
            const char* v = require_next("--uart-data-bits");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->uart_data_bits)) {
                std::cerr << "Invalid --uart-data-bits value: " << v << "\n";
                return false;
            }
            options->enable_uart = true;
        } else if (arg == "--uart-stop-bits") {
            const char* v = require_next("--uart-stop-bits");
            if (!v) return false;
            if (!parse_uint_arg(v, &options->uart_stop_bits)) {
                std::cerr << "Invalid --uart-stop-bits value: " << v << "\n";
                return false;
            }
            options->enable_uart = true;
        } else if (arg == "--uart-tx-mode") {
            const char* v = require_next("--uart-tx-mode");
            if (!v) return false;
            options->uart_tx_mode = v;
            options->enable_uart = true;
        } else if (arg == "--uart-loopback") {
            options->uart_loopback = true;
            options->enable_uart = true;
        } else if (arg == "--loopback-timeout-ms") {
            const char* v = require_next("--loopback-timeout-ms");
            if (!v) return false;
            int parsed = 0;
            if (!parse_int_arg(v, &parsed) || parsed <= 0) {
                std::cerr << "Invalid --loopback-timeout-ms value: " << v << "\n";
                return false;
            }
            options->loopback_timeout_ms = parsed;
        } else if (arg == "--keep-uart-config") {
            options->keep_uart_config = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

std::string trim_copy(const std::string& s) {
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

std::string clip_for_log(const std::string& value, size_t max_len = 120) {
    if (value.size() <= max_len) {
        return value;
    }
    std::ostringstream oss;
    oss << value.substr(0, max_len) << "...(truncated)";
    return oss.str();
}

bool get_enum_value(CEmergentCamera* camera, const char* name, std::string* out_value) {
    std::vector<char> buffer(4096, '\0');
    unsigned long value_size = 0;
    EVT_ERROR err = EVT_CameraGetEnumParam(camera, name, buffer.data(), buffer.size(), &value_size);
    if (err != EVT_SUCCESS) {
        return false;
    }
    *out_value = trim_copy(std::string(buffer.data()));
    return true;
}

bool get_enum_range(CEmergentCamera* camera, const char* name, std::string* out_value) {
    std::vector<char> buffer(4096, '\0');
    unsigned long value_size = 0;
    EVT_ERROR err = EVT_CameraGetEnumParamRange(camera, name, buffer.data(), buffer.size(), &value_size);
    if (err != EVT_SUCCESS) {
        return false;
    }
    *out_value = trim_copy(std::string(buffer.data()));
    return true;
}

bool get_string_value(CEmergentCamera* camera, const char* name, std::string* out_value) {
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

bool write_new_evidence_file(
    const std::filesystem::path& path,
    const std::string& contents,
    std::string* error_out)
{
    // Match Orange recording/calibration ownership behavior when the camera
    // probe itself is launched through sudo.
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::error_code filesystem_error;
    if (std::filesystem::exists(path, filesystem_error)) {
        if (error_out) *error_out = "refusing to overwrite existing evidence: " + path.string();
        return false;
    }
    if (filesystem_error) {
        if (error_out) *error_out = "could not inspect output path: " + filesystem_error.message();
        return false;
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            if (error_out) *error_out = "could not create output directory: " + filesystem_error.message();
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) {
        if (error_out) *error_out = "could not open output: " + path.string();
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        if (error_out) *error_out = "could not finish output: " + path.string();
        return false;
    }
    return true;
}

void print_sensor_pipeline_state(const nlohmann::json& state)
{
    std::cout << "\n[Sensor/ADC/Pixel Pipeline — Read Only]\n";
    const nlohmann::json& features = state.at("features");
    for (const auto& spec : orange::camera_sensor_pipeline::feature_specs()) {
        const nlohmann::json& feature = features.at(spec.name);
        std::cout << "  - " << std::left << std::setw(28) << spec.name;
        std::cout << feature.value("status", std::string("unknown"));
        if (feature.contains("node_type")) {
            std::cout << " [" << feature["node_type"].get<std::string>() << "]";
        }
        if (feature.contains("value")) {
            std::cout << " value=" << feature["value"].dump();
        }
        if (feature.contains("min") && feature.contains("max")) {
            std::cout << " range=[" << feature["min"].dump()
                      << "," << feature["max"].dump() << "]";
        }
        std::cout << "\n";
    }
    const nlohmann::json& summary = state.at("inventory_summary");
    std::cout << "  readable=" << summary.value("readable_count", 0)
              << " unsupported=" << summary.value("unsupported_count", 0)
              << " errors=" << summary.value("error_count", 0) << "\n";
    const nlohmann::json& xml = state.at("genicam_xml");
    std::cout << "  GenICam XML: " << xml.value("status", std::string("unknown"));
    if (xml.contains("sha256")) std::cout << " " << xml["sha256"].get<std::string>();
    if (xml.contains("byte_count")) std::cout << " bytes=" << xml["byte_count"].dump();
    std::cout << "\n";
}

void print_node_probe_prefix(const std::string& name) {
    std::cout << "  - " << std::left << std::setw(28) << name;
}

NodeProbeResult probe_single_node(CEmergentCamera* camera, const std::string& name) {
    NodeProbeResult result;
    EvtParamAttribute attr{};
    result.attr_err = EVT_CameraGetParamAttr(camera, name.c_str(), &attr);

    print_node_probe_prefix(name);
    if (result.attr_err != EVT_SUCCESS) {
        std::cout << "missing (" << evt_error_to_string(result.attr_err) << ")\n";
        return result;
    }

    result.exists = true;
    result.data_type = attr.dataType;
    std::cout << "found [" << data_type_to_string(attr.dataType) << "]";

    switch (attr.dataType) {
        case EDataTypeUInt32: {
            unsigned int value = 0;
            unsigned int min_v = 0;
            unsigned int max_v = 0;
            unsigned int inc_v = 0;
            EVT_ERROR e1 = EVT_CameraGetUInt32Param(camera, name.c_str(), &value);
            EVT_ERROR e2 = EVT_CameraGetUInt32ParamMin(camera, name.c_str(), &min_v);
            EVT_ERROR e3 = EVT_CameraGetUInt32ParamMax(camera, name.c_str(), &max_v);
            EVT_ERROR e4 = EVT_CameraGetUInt32ParamInc(camera, name.c_str(), &inc_v);
            if (e1 == EVT_SUCCESS) {
                std::cout << " value=" << value;
            }
            if (e2 == EVT_SUCCESS && e3 == EVT_SUCCESS) {
                std::cout << " range=[" << min_v << "," << max_v << "]";
            }
            if (e4 == EVT_SUCCESS) {
                std::cout << " inc=" << inc_v;
            }
            break;
        }
        case EDataTypeInt32: {
            int value = 0;
            int min_v = 0;
            int max_v = 0;
            unsigned int inc_v = 0;
            EVT_ERROR e1 = EVT_CameraGetInt32Param(camera, name.c_str(), &value);
            EVT_ERROR e2 = EVT_CameraGetInt32ParamMin(camera, name.c_str(), &min_v);
            EVT_ERROR e3 = EVT_CameraGetInt32ParamMax(camera, name.c_str(), &max_v);
            EVT_ERROR e4 = EVT_CameraGetInt32ParamInc(camera, name.c_str(), &inc_v);
            if (e1 == EVT_SUCCESS) {
                std::cout << " value=" << value;
            }
            if (e2 == EVT_SUCCESS && e3 == EVT_SUCCESS) {
                std::cout << " range=[" << min_v << "," << max_v << "]";
            }
            if (e4 == EVT_SUCCESS) {
                std::cout << " inc=" << inc_v;
            }
            break;
        }
        case EDataTypeBoolean: {
            bool value = false;
            EVT_ERROR e = EVT_CameraGetBoolParam(camera, name.c_str(), &value);
            if (e == EVT_SUCCESS) {
                std::cout << " value=" << (value ? "true" : "false");
            }
            break;
        }
        case EDataTypeFloat: {
            float value = 0.0f;
            float min_v = 0.0f;
            float max_v = 0.0f;
            EVT_ERROR e1 = EVT_CameraGetFloatParam(camera, name.c_str(), &value);
            EVT_ERROR e2 = EVT_CameraGetFloatParamMin(camera, name.c_str(), &min_v);
            EVT_ERROR e3 = EVT_CameraGetFloatParamMax(camera, name.c_str(), &max_v);
            if (e1 == EVT_SUCCESS) {
                std::cout << " value=" << value;
            }
            if (e2 == EVT_SUCCESS && e3 == EVT_SUCCESS) {
                std::cout << " range=[" << min_v << "," << max_v << "]";
            }
            break;
        }
        case EDataTypeString: {
            std::string value;
            if (get_string_value(camera, name.c_str(), &value)) {
                std::cout << " value=\"" << clip_for_log(value) << "\"";
            }
            break;
        }
        case EDataTypeEnumeration: {
            std::string value;
            std::string range;
            if (get_enum_value(camera, name.c_str(), &value)) {
                std::cout << " value=" << value;
            }
            if (get_enum_range(camera, name.c_str(), &range)) {
                std::cout << " options=" << clip_for_log(range);
            }
            break;
        }
        case EDataTypeCommand:
        case EDataTypeEnumEntry:
        case EDataTypeCategory:
        case EDataTypeUnsupported:
        default:
            break;
    }

    std::cout << "\n";
    return result;
}

void probe_group(
    CEmergentCamera* camera,
    const std::string& group_name,
    const std::vector<std::string>& names,
    std::map<std::string, NodeProbeResult>* node_results) {
    std::cout << "\n[" << group_name << "]\n";
    for (const std::string& name : names) {
        if (node_results->find(name) != node_results->end()) {
            continue;
        }
        (*node_results)[name] = probe_single_node(camera, name);
    }
}

bool get_uint_range_and_value(
    CEmergentCamera* camera,
    const char* name,
    unsigned int* current,
    unsigned int* min_v,
    unsigned int* max_v,
    unsigned int* inc_v) {
    EVT_ERROR e1 = EVT_CameraGetUInt32Param(camera, name, current);
    EVT_ERROR e2 = EVT_CameraGetUInt32ParamMin(camera, name, min_v);
    EVT_ERROR e3 = EVT_CameraGetUInt32ParamMax(camera, name, max_v);
    EVT_ERROR e4 = EVT_CameraGetUInt32ParamInc(camera, name, inc_v);
    return e1 == EVT_SUCCESS && e2 == EVT_SUCCESS && e3 == EVT_SUCCESS && e4 == EVT_SUCCESS;
}

bool write_uint_with_readback(CEmergentCamera* camera, const char* name, unsigned int value, bool print_prefix) {
    if (print_prefix) {
        std::cout << "  - " << name << ": ";
    }
    const auto t0 = std::chrono::steady_clock::now();
    EVT_ERROR set_err = EVT_CameraSetUInt32Param(camera, name, value);
    const auto t1 = std::chrono::steady_clock::now();
    const long long latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (set_err != EVT_SUCCESS) {
        std::cout << "set failed (" << evt_error_to_string(set_err) << ")\n";
        return false;
    }

    unsigned int readback = 0;
    EVT_ERROR get_err = EVT_CameraGetUInt32Param(camera, name, &readback);
    if (get_err != EVT_SUCCESS) {
        std::cout << "set ok, readback failed (" << evt_error_to_string(get_err)
                  << "), set_latency_ms=" << latency_ms << "\n";
        return false;
    }

    std::cout << "set ok, readback=" << readback << ", set_latency_ms=" << latency_ms;
    if (readback != value) {
        std::cout << " (mismatch from requested " << value << ")";
    }
    std::cout << "\n";
    return readback == value;
}

bool exercise_node_noop_write(CEmergentCamera* camera, const char* name) {
    unsigned int current = 0;
    unsigned int min_v = 0;
    unsigned int max_v = 0;
    unsigned int inc_v = 0;
    if (!get_uint_range_and_value(camera, name, &current, &min_v, &max_v, &inc_v)) {
        std::cout << "  - " << name << ": unable to read value/range for no-op write test\n";
        return false;
    }

    std::cout << "  - " << name << ": current=" << current
              << " range=[" << min_v << "," << max_v << "] inc=" << inc_v << "\n";
    return write_uint_with_readback(camera, name, current, false);
}

bool set_target_with_range_check(CEmergentCamera* camera, const char* name, unsigned int target) {
    unsigned int current = 0;
    unsigned int min_v = 0;
    unsigned int max_v = 0;
    unsigned int inc_v = 0;
    if (!get_uint_range_and_value(camera, name, &current, &min_v, &max_v, &inc_v)) {
        std::cout << "  - " << name << ": unable to read value/range before target set\n";
        return false;
    }

    std::cout << "  - " << name << ": current=" << current
              << " target=" << target
              << " range=[" << min_v << "," << max_v << "] inc=" << inc_v << "\n";
    if (target < min_v || target > max_v) {
        std::cout << "    target out of range; not writing\n";
        return false;
    }
    return write_uint_with_readback(camera, name, target, true);
}

bool try_set_enum(CEmergentCamera* camera, const char* name, const std::string& value) {
    EVT_ERROR err = EVT_CameraSetEnumParam(camera, name, value.c_str());
    if (err != EVT_SUCCESS) {
        std::cout << "  - " << name << "=" << value << " failed (" << evt_error_to_string(err) << ")\n";
        return false;
    }
    std::cout << "  - " << name << "=" << value << " ok\n";
    return true;
}

bool try_set_bool(CEmergentCamera* camera, const char* name, bool value) {
    EVT_ERROR err = EVT_CameraSetBoolParam(camera, name, value);
    if (err != EVT_SUCCESS) {
        std::cout << "  - " << name << "=" << (value ? "true" : "false")
                  << " failed (" << evt_error_to_string(err) << ")\n";
        return false;
    }
    std::cout << "  - " << name << "=" << (value ? "true" : "false") << " ok\n";
    return true;
}

bool try_set_uint(CEmergentCamera* camera, const char* name, unsigned int value) {
    EVT_ERROR err = EVT_CameraSetUInt32Param(camera, name, value);
    if (err != EVT_SUCCESS) {
        std::cout << "  - " << name << "=" << value << " failed (" << evt_error_to_string(err) << ")\n";
        return false;
    }
    std::cout << "  - " << name << "=" << value << " ok\n";
    return true;
}

bool restore_nir_strobe_pulse_with_readback(CEmergentCamera* camera) {
    constexpr const char* kModeNode = "GPO_0_Mode";
    constexpr const char* kPolarityNode = "GPO_0_Polarity";
    constexpr const char* kExpectedMode = "Exposure";
    constexpr bool kExpectedPolarity = false;

    std::cout << "\n[NIR Strobe Pulse Recovery]\n";
    bool ok = try_set_bool(camera, kPolarityNode, kExpectedPolarity);
    ok = try_set_enum(camera, kModeNode, kExpectedMode) && ok;

    std::string mode_readback;
    const bool mode_read_ok = get_enum_value(camera, kModeNode, &mode_readback);
    bool polarity_readback = true;
    const EVT_ERROR polarity_read_err =
        EVT_CameraGetBoolParam(camera, kPolarityNode, &polarity_readback);

    if (!mode_read_ok) {
        std::cout << "  - " << kModeNode << " readback failed\n";
        ok = false;
    } else {
        std::cout << "  - " << kModeNode << " readback=" << mode_readback << "\n";
        ok = (mode_readback == kExpectedMode) && ok;
    }
    if (polarity_read_err != EVT_SUCCESS) {
        std::cout << "  - " << kPolarityNode << " readback failed ("
                  << evt_error_to_string(polarity_read_err) << ")\n";
        ok = false;
    } else {
        std::cout << "  - " << kPolarityNode << " readback="
                  << (polarity_readback ? "true" : "false") << "\n";
        ok = (polarity_readback == kExpectedPolarity) && ok;
    }

    std::cout << "  - recovery result: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}

UartRestoreState capture_uart_restore_state(CEmergentCamera* camera) {
    UartRestoreState state;
    bool enabled = false;
    if (EVT_CameraGetBoolParam(camera, "UartEnable", &enabled) == EVT_SUCCESS) {
        state.captured_uart_enable = true;
        state.uart_enable_value = enabled;
    }
    std::string gpo3_mode;
    if (get_enum_value(camera, "GPO_3_Mode", &gpo3_mode)) {
        state.captured_gpo3_mode = true;
        state.gpo3_mode = gpo3_mode;
    }
    return state;
}

bool configure_uart(CEmergentCamera* camera, const Options& options) {
    bool ok = true;
    ok = try_set_enum(camera, "GPO_3_Mode", options.uart_tx_mode) && ok;
    ok = try_set_bool(camera, "UartEnable", true) && ok;
    ok = try_set_enum(camera, "UartBaud", options.uart_baud) && ok;
    ok = try_set_uint(camera, "UartDataBits", options.uart_data_bits) && ok;
    ok = try_set_uint(camera, "UartStopBits", options.uart_stop_bits) && ok;
    return ok;
}

void restore_uart(CEmergentCamera* camera, const UartRestoreState& state) {
    std::cout << "\n[UART Restore]\n";
    if (state.captured_uart_enable) {
        try_set_bool(camera, "UartEnable", state.uart_enable_value);
    } else {
        std::cout << "  - UartEnable original state unavailable; skipping restore\n";
    }
    if (state.captured_gpo3_mode) {
        try_set_enum(camera, "GPO_3_Mode", state.gpo3_mode);
    } else {
        std::cout << "  - GPO_3_Mode original state unavailable; skipping restore\n";
    }
}

bool uart_loopback_test(CEmergentCamera* camera, int timeout_ms) {
    constexpr unsigned int kStartByte = 0x30;
    constexpr unsigned int kCount = 10;

    std::cout << "\n[UART Loopback Test]\n";
    std::cout << "  - sending:";
    for (unsigned int b = kStartByte; b < kStartByte + kCount; ++b) {
        EVT_ERROR err = EVT_CameraSetUInt32Param(camera, "UartTxData", b);
        if (err != EVT_SUCCESS) {
            std::cout << "\n  - UartTxData write failed at byte 0x" << std::hex << b << std::dec
                      << " (" << evt_error_to_string(err) << ")\n";
            return false;
        }
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0') << b << std::dec;
    }
    std::cout << std::setfill(' ') << "\n";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    unsigned int tx_fifo = 0;
    bool tx_empty = false;
    while (std::chrono::steady_clock::now() < deadline) {
        EVT_ERROR err = EVT_CameraGetUInt32Param(camera, "UartTxFifoCnt", &tx_fifo);
        if (err == EVT_SUCCESS && tx_fifo == 0) {
            tx_empty = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!tx_empty) {
        std::cout << "  - timeout waiting for UartTxFifoCnt to reach 0\n";
        return false;
    }
    std::cout << "  - tx fifo drained\n";

    unsigned int rx_fifo = 0;
    bool rx_ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        EVT_ERROR err = EVT_CameraGetUInt32Param(camera, "UartRxFifoCnt", &rx_fifo);
        if (err == EVT_SUCCESS && rx_fifo >= kCount) {
            rx_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!rx_ready) {
        std::cout << "  - timeout waiting for UartRxFifoCnt >= " << kCount << "\n";
        return false;
    }
    std::cout << "  - rx fifo count=" << rx_fifo << "\n";

    std::vector<unsigned int> received;
    received.reserve(kCount);
    for (unsigned int i = 0; i < kCount; ++i) {
        unsigned int val = 0;
        EVT_ERROR err = EVT_CameraGetUInt32Param(camera, "UartRxData", &val);
        if (err != EVT_SUCCESS) {
            std::cout << "  - failed reading UartRxData index " << i
                      << " (" << evt_error_to_string(err) << ")\n";
            return false;
        }
        received.push_back(val);
    }

    bool pass = true;
    std::cout << "  - received:";
    for (size_t i = 0; i < received.size(); ++i) {
        const unsigned int expected = kStartByte + static_cast<unsigned int>(i);
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0') << received[i] << std::dec;
        if (received[i] != expected) {
            pass = false;
        }
    }
    std::cout << std::setfill(' ') << "\n";
    return pass;
}

std::vector<GigEVisionDeviceInfo> list_devices(EVT_ERROR* err_out, unsigned int* discovered_out) {
    const size_t max_devices = 64;
    std::vector<GigEVisionDeviceInfo> devices(max_devices);

    unsigned int buf_size = static_cast<unsigned int>(devices.size());
    unsigned int discovered = 0;
    EVT_ERROR err = EVT_ListDevices(devices.data(), &buf_size, &discovered);
    if (err_out) {
        *err_out = err;
    }
    if (discovered_out) {
        *discovered_out = discovered;
    }

    if (err != EVT_SUCCESS) {
        devices.clear();
        return devices;
    }

    if (discovered > devices.size()) {
        discovered = static_cast<unsigned int>(devices.size());
    }
    devices.resize(discovered);
    return devices;
}

int select_camera_index(const std::vector<GigEVisionDeviceInfo>& devices, const Options& options) {
    if (!options.serial.empty()) {
        for (size_t i = 0; i < devices.size(); ++i) {
            if (options.serial == devices[i].serialNumber) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int idx = options.index_set ? options.index : 0;
    if (idx < 0 || idx >= static_cast<int>(devices.size())) {
        return -1;
    }
    return idx;
}

void print_device_list(const std::vector<GigEVisionDeviceInfo>& devices) {
    std::cout << "\n[Discovered Cameras]\n";
    for (size_t i = 0; i < devices.size(); ++i) {
        const GigEVisionDeviceInfo& d = devices[i];
        std::cout << "  [" << i << "] serial=" << d.serialNumber
                  << " model=" << d.modelName
                  << " ip=" << d.currentIp
                  << " nic=" << d.nic.adapterName << "\n";
    }
}

class CameraGuard {
public:
    CEmergentCamera camera;
    bool opened = false;

    ~CameraGuard() {
        if (opened) {
            EVT_CameraClose(&camera);
        }
    }
};

int run_probe(const Options& options) {
    std::cout << "EVT SDK: " << EVT_SDKVersion() << std::endl;

    EVT_ERROR list_err = EVT_SUCCESS;
    unsigned int discovered = 0;
    std::vector<GigEVisionDeviceInfo> devices = list_devices(&list_err, &discovered);
    if (list_err != EVT_SUCCESS) {
        std::cerr << "EVT_ListDevices failed: " << evt_error_to_string(list_err) << "\n";
        return 2;
    }

    if (devices.empty()) {
        std::cerr << "No cameras discovered.\n";
        return 3;
    }

    print_device_list(devices);

    if (options.list_only) {
        return 0;
    }

    const int camera_index = select_camera_index(devices, options);
    if (camera_index < 0) {
        if (!options.serial.empty()) {
            std::cerr << "Camera serial not found: " << options.serial << "\n";
        } else {
            std::cerr << "Camera index out of range.\n";
        }
        return 4;
    }

    const GigEVisionDeviceInfo& selected = devices[camera_index];
    std::cout << "\n[Selected Camera]\n";
    std::cout << "  index=" << camera_index
              << " serial=" << selected.serialNumber
              << " model=" << selected.modelName
              << " ip=" << selected.currentIp
              << " firmware=" << selected.deviceVersion << "\n";

    CameraGuard cam_guard;
    EVT_ERROR open_err = options.xml_path.empty()
        ? EVT_CameraOpen(&cam_guard.camera, &selected)
        : EVT_CameraOpen(&cam_guard.camera, &selected, options.xml_path.c_str());

    if (open_err != EVT_SUCCESS) {
        std::cerr << "EVT_CameraOpen failed: " << evt_error_to_string(open_err) << "\n";
        return 5;
    }
    cam_guard.opened = true;
    std::cout << "Camera opened successfully.\n";

    // Keep this evidence path isolated from the legacy lens/UART probe below.
    // Some nominal getters (for example UartRxData) can consume device state,
    // so sensor-pipeline mode performs only the explicitly audited getter set.
    if (options.sensor_pipeline) {
        nlohmann::json sensor_state =
            orange::camera_sensor_pipeline::capture_state(
                &cam_guard.camera,
                {
                    selected.serialNumber,
                    selected.modelName,
                    selected.deviceVersion,
                },
                "standalone_pre_orange_read_only");
        if (!options.genicam_xml_output_path.empty()) {
            std::string xml;
            std::string xml_error;
            if (!orange::camera_sensor_pipeline::read_genicam_xml(
                    &cam_guard.camera, &xml, &xml_error)) {
                std::cerr << "GenICam XML read failed: " << xml_error << "\n";
                return 7;
            }
            std::string write_error;
            if (!write_new_evidence_file(
                    options.genicam_xml_output_path, xml, &write_error)) {
                std::cerr << "GenICam XML write failed: " << write_error << "\n";
                return 8;
            }
            sensor_state["genicam_xml"]["evidence_path"] =
                std::filesystem::absolute(options.genicam_xml_output_path).string();
            sensor_state["genicam_xml"]["external_evidence_written"] = true;
            std::cout << "  XML evidence: " << options.genicam_xml_output_path << "\n";
        }

        if (!options.sensor_pipeline_json_path.empty()) {
            std::string write_error;
            if (!write_new_evidence_file(
                    options.sensor_pipeline_json_path,
                    sensor_state.dump(2) + "\n",
                    &write_error)) {
                std::cerr << "Sensor-pipeline JSON write failed: " << write_error << "\n";
                return 6;
            }
            std::cout << "  JSON evidence: " << options.sensor_pipeline_json_path << "\n";
        }
        print_sensor_pipeline_state(sensor_state);
        return 0;
    }

    std::map<std::string, NodeProbeResult> node_results;
    const std::vector<std::string> core_nodes = {
        "Focus", "Iris", "LensMountPresent", "LensPresent", "LensBusy",
        "LensFocalLength", "LensName", "LensMountFirmwareVersion",
    };
    std::vector<std::string> uart_nodes = {
        "UartEnable", "UartBaud", "UartDataBits", "UartStopBits",
        "UartTxData", "UartTxFifoCnt", "UartRxData", "UartRxFifoCnt",
        "LineSelector", "LineMode", "LineSource", "LineStatus",
        "GPO_3_Mode", "GPI_5_Mode",
    };
    for (int i = 0; i <= 7; ++i) {
        uart_nodes.push_back("GPO_" + std::to_string(i) + "_Mode");
        uart_nodes.push_back("GPO_" + std::to_string(i) + "_Polarity");
        uart_nodes.push_back("GPI_" + std::to_string(i) + "_Mode");
        uart_nodes.push_back("GPI_" + std::to_string(i) + "_Polarity");
    }
    const std::vector<std::string> transport_nodes = {
        "GevSCPSPacketSize", "AcquisitionMode", "TriggerMode", "TriggerSource"
    };

    probe_group(&cam_guard.camera, "Lens/Focus Nodes", core_nodes, &node_results);
    probe_group(&cam_guard.camera, "UART/GPIO Nodes", uart_nodes, &node_results);
    probe_group(&cam_guard.camera, "Transport/Trigger Nodes", transport_nodes, &node_results);

    ProbeSummary summary;
    summary.focus_found = node_results["Focus"].exists;
    summary.iris_found = node_results["Iris"].exists;
    summary.uart_enable_found = node_results["UartEnable"].exists;
    summary.uart_baud_found = node_results["UartBaud"].exists;
    summary.uart_data_bits_found = node_results["UartDataBits"].exists;
    summary.uart_stop_bits_found = node_results["UartStopBits"].exists;

    if (options.exercise_genicam) {
        std::cout << "\n[GenICam Focus/Iris Exercise]\n";
        bool ok = true;

        if (summary.focus_found && node_results["Focus"].data_type == EDataTypeUInt32) {
            ok = exercise_node_noop_write(&cam_guard.camera, "Focus") && ok;
        } else {
            std::cout << "  - Focus: node missing or not UInt32; skipped\n";
            ok = false;
        }

        if (summary.iris_found && node_results["Iris"].data_type == EDataTypeUInt32) {
            ok = exercise_node_noop_write(&cam_guard.camera, "Iris") && ok;
        } else {
            std::cout << "  - Iris: node missing or not UInt32; skipped\n";
            ok = false;
        }
        summary.genicam_exercise = ok ? Status::kPass : Status::kFail;
    }

    if (options.set_focus_target) {
        std::cout << "\n[Focus Target Write]\n";
        const bool ok = set_target_with_range_check(&cam_guard.camera, "Focus", options.focus_target);
        summary.focus_target = ok ? Status::kPass : Status::kFail;
    }

    if (options.set_iris_target) {
        std::cout << "\n[Iris Target Write]\n";
        const bool ok = set_target_with_range_check(&cam_guard.camera, "Iris", options.iris_target);
        summary.iris_target = ok ? Status::kPass : Status::kFail;
    }

    if (options.restore_nir_strobe_pulse) {
        const bool ok = restore_nir_strobe_pulse_with_readback(&cam_guard.camera);
        summary.nir_strobe_pulse_restore = ok ? Status::kPass : Status::kFail;
    }

    UartRestoreState restore_state;
    if (options.enable_uart) {
        restore_state = capture_uart_restore_state(&cam_guard.camera);
        std::cout << "\n[UART Enable Sequence]\n";
        const bool ok = configure_uart(&cam_guard.camera, options);
        summary.uart_enable_sequence = ok ? Status::kPass : Status::kFail;

        if (options.uart_loopback) {
            const bool loopback_ok = uart_loopback_test(&cam_guard.camera, options.loopback_timeout_ms);
            summary.uart_loopback = loopback_ok ? Status::kPass : Status::kFail;
        }

        if (!options.keep_uart_config) {
            restore_uart(&cam_guard.camera, restore_state);
        }
    }

    std::cout << "\n[Summary]\n";
    std::cout << "  Focus node available: " << (summary.focus_found ? "yes" : "no") << "\n";
    std::cout << "  Iris node available: " << (summary.iris_found ? "yes" : "no") << "\n";
    const bool uart_core_present = summary.uart_enable_found
        && summary.uart_baud_found
        && summary.uart_data_bits_found
        && summary.uart_stop_bits_found;
    std::cout << "  UART core nodes present: " << (uart_core_present ? "yes" : "no") << "\n";
    std::cout << "  GenICam exercise: " << status_to_string(summary.genicam_exercise) << "\n";
    std::cout << "  Focus target write: " << status_to_string(summary.focus_target) << "\n";
    std::cout << "  Iris target write: " << status_to_string(summary.iris_target) << "\n";
    std::cout << "  NIR strobe pulse restore: "
              << status_to_string(summary.nir_strobe_pulse_restore) << "\n";
    std::cout << "  UART enable sequence: " << status_to_string(summary.uart_enable_sequence) << "\n";
    std::cout << "  UART loopback: " << status_to_string(summary.uart_loopback) << "\n";

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, &options)) {
        return 1;
    }
    if (options.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (options.restore_nir_strobe_pulse && options.serial.empty()) {
        std::cerr << "--restore-nir-strobe-pulse requires an explicit --serial.\n";
        return 1;
    }
    const bool mutation_requested = options.exercise_genicam ||
        options.set_focus_target || options.set_iris_target ||
        options.restore_nir_strobe_pulse || options.enable_uart ||
        options.uart_loopback || options.keep_uart_config;
    if (options.sensor_pipeline && mutation_requested) {
        std::cerr
            << "--sensor-pipeline is a read-only evidence mode and cannot be combined "
               "with camera mutation/exercise flags.\n";
        return 1;
    }
    return run_probe(options);
}
