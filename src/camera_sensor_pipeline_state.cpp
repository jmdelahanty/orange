#include "camera_sensor_pipeline_state.h"

#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace orange::camera_sensor_pipeline {
namespace {

std::string trim_copy(const std::string& value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string utc_now()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

nlohmann::json error_json(EVT_ERROR error)
{
    return {
        {"code", static_cast<int>(error)},
        {"name", evt_error_name(error)},
    };
}

bool get_enum_value(
    Emergent::CEmergentCamera* camera,
    const char* name,
    std::string* value_out,
    EVT_ERROR* error_out)
{
    std::vector<char> buffer(4096, '\0');
    unsigned long value_size = 0;
    const EVT_ERROR error = Emergent::EVT_CameraGetEnumParam(
        camera, name, buffer.data(), buffer.size(), &value_size);
    if (error_out) *error_out = error;
    if (error != EVT_SUCCESS || value_out == nullptr) return false;
    *value_out = trim_copy(std::string(buffer.data()));
    return true;
}

nlohmann::json get_enum_options(Emergent::CEmergentCamera* camera, const char* name)
{
    std::vector<char> buffer(16384, '\0');
    unsigned long value_size = 0;
    const EVT_ERROR error = Emergent::EVT_CameraGetEnumParamRange(
        camera, name, buffer.data(), buffer.size(), &value_size);
    if (error != EVT_SUCCESS) return nlohmann::json::array();

    nlohmann::json options = nlohmann::json::array();
    std::stringstream input(std::string(buffer.data()));
    std::string option;
    while (std::getline(input, option, ',')) {
        option = trim_copy(option);
        if (!option.empty()) options.push_back(option);
    }
    return options;
}

bool get_string_value(
    Emergent::CEmergentCamera* camera,
    const char* name,
    std::string* value_out,
    EVT_ERROR* error_out)
{
    int max_length = 0;
    if (Emergent::EVT_CameraGetStringParamMaxLength(camera, name, &max_length) !=
            EVT_SUCCESS ||
        max_length <= 0) {
        max_length = 512;
    }
    std::vector<char> buffer(static_cast<std::size_t>(max_length) + 8u, '\0');
    unsigned long value_size = 0;
    const EVT_ERROR error = Emergent::EVT_CameraGetStringParam(
        camera,
        name,
        buffer.data(),
        static_cast<unsigned long>(buffer.size()),
        &value_size,
        0);
    if (error_out) *error_out = error;
    if (error != EVT_SUCCESS || value_out == nullptr) return false;
    *value_out = trim_copy(std::string(buffer.data()));
    return true;
}

bool json_values_equal(const nlohmann::json& left, const nlohmann::json& right)
{
    if (left.is_number() && right.is_number()) {
        const long double lhs = left.get<long double>();
        const long double rhs = right.get<long double>();
        return std::fabs(lhs - rhs) <= 1e-9L;
    }
    return left == right;
}

}  // namespace

const std::vector<FeatureSpec>& feature_specs()
{
    // The aliases are intentional. EVT firmware generations may expose either
    // vendor-specific names (ADC/Offset) or SFNC-style names (AdcBitDepth/
    // BlackLevel). Recording both makes absence itself useful evidence.
    static const std::vector<FeatureSpec> specs = {
        {"Width", "image_geometry", "Applied ROI width in native camera pixels."},
        {"Height", "image_geometry", "Applied ROI height in native camera pixels."},
        {"OffsetX", "image_geometry", "ROI origin X; not the analog black-level offset."},
        {"OffsetY", "image_geometry", "ROI origin Y; not the analog black-level offset."},
        {"FrameRate", "exposure_timing", "Applied camera frame rate."},
        {"Exposure", "exposure_timing", "Applied exposure time in the camera node's units."},
        {"Gain", "gain", "Emergent digital gain; linear multiplier is Gain/256."},
        {"AutoGain", "gain", "Automatic digital-gain enable state."},
        {"AutoGainSet", "gain", "Automatic-gain target, if supported."},
        {"AutoGainIGain", "gain", "Automatic-gain integrator parameter, if supported."},
        {"PGAGain", "gain", "Programmable analog gain, if exposed by this firmware."},
        {"HCG", "gain", "High-conversion-gain mode, if exposed by this firmware."},
        {"Offset", "black_level", "Vendor black-level/analog offset; not ROI OffsetX/OffsetY."},
        {"OffsetSigned", "black_level", "Signed vendor black-level offset, if supported."},
        {"BlackLevel", "black_level", "SFNC-style black-level control alias, if supported."},
        {"BlackLevelRaw", "black_level", "Raw black-level control alias, if supported."},
        {"LUTEnable", "tone_mapping", "Lookup-table enable state."},
        {"LUTIndex", "tone_mapping", "Current LUT selector only; the probe never changes it."},
        {"LUTValue", "tone_mapping", "Value at the current LUT selector only."},
        {"GammaEnable", "tone_mapping", "Gamma stage enable state, if supported."},
        {"Gamma", "tone_mapping", "Gamma value, if supported."},
        {"ADC", "conversion", "Vendor ADC mode or bit-depth node, if supported."},
        {"AdcBitDepth", "conversion", "SFNC-style ADC bit-depth alias, if supported."},
        {"SensorBitDepth", "conversion", "Sensor conversion bit depth, if supported."},
        {"TransferBitDepth", "conversion", "Transfer bit depth before output packing, if supported."},
        {"DualADC", "conversion", "Dual-ADC enable state, if supported."},
        {"DualADC_MODE", "conversion", "Dual-ADC operating mode, if supported."},
        {"PixelFormat", "conversion", "Transport/output pixel format; distinct from ADC mode."},
    };
    return specs;
}

std::string evt_error_name(EVT_ERROR error)
{
    switch (error) {
        case EVT_SUCCESS: return "EVT_SUCCESS";
        case EVT_ERROR_SRCH: return "EVT_ERROR_SRCH";
        case EVT_ERROR_DEVICE_NOT_CONNECTED: return "EVT_ERROR_DEVICE_NOT_CONNECTED";
        case EVT_ERROR_DEVICE_LOST_CONNECTION: return "EVT_ERROR_DEVICE_LOST_CONNECTION";
        case EVT_ERROR_GENICAM_ERROR: return "EVT_ERROR_GENICAM_ERROR";
        case EVT_ERROR_GENICAM_NOT_MATCH: return "EVT_ERROR_GENICAM_NOT_MATCH";
        case EVT_ERROR_GENICAM_OUT_OF_RANGE: return "EVT_ERROR_GENICAM_OUT_OF_RANGE";
        case EVT_ERROR_SOCK: return "EVT_ERROR_SOCK";
        case EVT_ERROR_GVCP_ACK: return "EVT_ERROR_GVCP_ACK";
        case EVT_ERROR_TIMEDOUT: return "EVT_ERROR_TIMEDOUT";
        case EVT_ERROR_NOT_SUPPORTED: return "EVT_ERROR_NOT_SUPPORTED";
        default: return "EVT_ERROR(" + std::to_string(static_cast<int>(error)) + ")";
    }
}

std::string data_type_name(EvtParamDataType data_type)
{
    switch (data_type) {
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

nlohmann::json read_feature(
    Emergent::CEmergentCamera* camera,
    const FeatureSpec& feature)
{
    nlohmann::json result = {
        {"node_name", feature.name},
        {"category", feature.category},
        {"interpretation", feature.interpretation},
        {"supported", false},
        {"readable", false},
    };

    if (camera == nullptr) {
        result["status"] = "read_error";
        result["attribute_error"] = "camera_pointer_is_null";
        return result;
    }

    EvtParamAttribute attribute{};
    const EVT_ERROR attribute_error =
        Emergent::EVT_CameraGetParamAttr(camera, feature.name, &attribute);
    if (attribute_error != EVT_SUCCESS) {
        result["status"] =
            attribute_error == EVT_ERROR_SRCH ? "unsupported" : "read_error";
        result["attribute_error"] = error_json(attribute_error);
        return result;
    }

    result["supported"] = true;
    result["node_type"] = data_type_name(attribute.dataType);
    EVT_ERROR value_error = EVT_SUCCESS;

    switch (attribute.dataType) {
        case EDataTypeUInt32: {
            unsigned int value = 0;
            value_error = Emergent::EVT_CameraGetUInt32Param(camera, feature.name, &value);
            if (value_error == EVT_SUCCESS) result["value"] = value;
            unsigned int min_value = 0;
            unsigned int max_value = 0;
            unsigned int increment = 0;
            if (Emergent::EVT_CameraGetUInt32ParamMin(camera, feature.name, &min_value) == EVT_SUCCESS)
                result["min"] = min_value;
            if (Emergent::EVT_CameraGetUInt32ParamMax(camera, feature.name, &max_value) == EVT_SUCCESS)
                result["max"] = max_value;
            if (Emergent::EVT_CameraGetUInt32ParamInc(camera, feature.name, &increment) == EVT_SUCCESS)
                result["increment"] = increment;
            break;
        }
        case EDataTypeInt32: {
            int value = 0;
            value_error = Emergent::EVT_CameraGetInt32Param(camera, feature.name, &value);
            if (value_error == EVT_SUCCESS) result["value"] = value;
            int min_value = 0;
            int max_value = 0;
            unsigned int increment = 0;
            if (Emergent::EVT_CameraGetInt32ParamMin(camera, feature.name, &min_value) == EVT_SUCCESS)
                result["min"] = min_value;
            if (Emergent::EVT_CameraGetInt32ParamMax(camera, feature.name, &max_value) == EVT_SUCCESS)
                result["max"] = max_value;
            if (Emergent::EVT_CameraGetInt32ParamInc(camera, feature.name, &increment) == EVT_SUCCESS)
                result["increment"] = increment;
            break;
        }
        case EDataTypeBoolean: {
            bool value = false;
            value_error = Emergent::EVT_CameraGetBoolParam(camera, feature.name, &value);
            if (value_error == EVT_SUCCESS) result["value"] = value;
            break;
        }
        case EDataTypeFloat: {
            float value = 0.0F;
            value_error = Emergent::EVT_CameraGetFloatParam(camera, feature.name, &value);
            if (value_error == EVT_SUCCESS) result["value"] = value;
            float min_value = 0.0F;
            float max_value = 0.0F;
            if (Emergent::EVT_CameraGetFloatParamMin(camera, feature.name, &min_value) == EVT_SUCCESS)
                result["min"] = min_value;
            if (Emergent::EVT_CameraGetFloatParamMax(camera, feature.name, &max_value) == EVT_SUCCESS)
                result["max"] = max_value;
            break;
        }
        case EDataTypeString: {
            std::string value;
            if (get_string_value(camera, feature.name, &value, &value_error)) {
                result["value"] = value;
            }
            break;
        }
        case EDataTypeEnumeration: {
            std::string value;
            if (get_enum_value(camera, feature.name, &value, &value_error)) {
                result["value"] = value;
            }
            const nlohmann::json options = get_enum_options(camera, feature.name);
            if (!options.empty()) result["options"] = options;
            break;
        }
        case EDataTypeCommand:
        case EDataTypeEnumEntry:
        case EDataTypeCategory:
        case EDataTypeUnsupported:
        default:
            result["status"] = "unreadable_type";
            result["read_error"] = "node_does_not_expose_a_passive_value";
            return result;
    }

    if (value_error == EVT_SUCCESS) {
        result["status"] = "readable";
        result["readable"] = true;
    } else {
        result["status"] = "read_error";
        result["read_error"] = error_json(value_error);
    }
    return result;
}

bool read_genicam_xml(
    Emergent::CEmergentCamera* camera,
    std::string* xml_out,
    std::string* error_out)
{
    if (camera == nullptr || xml_out == nullptr) {
        if (error_out) *error_out = "camera or output pointer is null";
        return false;
    }
    const char* xml = nullptr;
    const EVT_ERROR error = Emergent::EVT_GetGenICamXml(camera, &xml);
    if (error != EVT_SUCCESS || xml == nullptr) {
        if (error_out) {
            *error_out = error != EVT_SUCCESS
                ? evt_error_name(error)
                : "EVT_GetGenICamXml returned a null document";
        }
        return false;
    }
    *xml_out = xml;
    return true;
}

void add_requested_readbacks(
    nlohmann::json* state,
    const nlohmann::json& requested_feature_values,
    const nlohmann::json& requested_feature_sources)
{
    if (state == nullptr) return;
    if (!requested_feature_values.is_object() || requested_feature_values.empty()) {
        (*state)["requested_feature_values"] = nlohmann::json::object();
        (*state)["requested_feature_sources"] = nlohmann::json::object();
        (*state)["requested_readbacks"] = nlohmann::json::object();
        (*state)["all_requested_features_readable"] = nullptr;
        (*state)["all_requested_readbacks_match"] = nullptr;
        (*state)["applied_state_status"] = "not_requested";
        return;
    }
    nlohmann::json comparisons = nlohmann::json::object();
    bool all_match = true;
    bool all_readable = true;

    for (auto iterator = requested_feature_values.begin();
         iterator != requested_feature_values.end();
         ++iterator) {
        const std::string name = iterator.key();
        nlohmann::json comparison = {{"requested_value", iterator.value()}};
        if (requested_feature_sources.contains(name) &&
            requested_feature_sources[name].is_string()) {
            comparison["request_source"] = requested_feature_sources[name];
        }

        const nlohmann::json* feature = nullptr;
        if (state->contains("features") && (*state)["features"].is_object()) {
            const auto found = (*state)["features"].find(name);
            if (found != (*state)["features"].end()) feature = &(*found);
        }

        if (feature == nullptr) {
            comparison["status"] = "not_in_inventory";
            all_match = false;
            all_readable = false;
        } else if (feature->value("status", std::string()) == "unsupported") {
            comparison["status"] = "unsupported";
            all_match = false;
            all_readable = false;
        } else if (!feature->value("readable", false) || !feature->contains("value")) {
            comparison["status"] = "read_error";
            all_match = false;
            all_readable = false;
        } else {
            comparison["applied_value"] = (*feature)["value"];
            const bool match = json_values_equal(iterator.value(), (*feature)["value"]);
            comparison["status"] = match ? "match" : "mismatch";
            comparison["matches"] = match;
            all_match = all_match && match;
        }
        comparisons[name] = std::move(comparison);
    }

    (*state)["requested_feature_values"] = requested_feature_values;
    (*state)["requested_feature_sources"] = requested_feature_sources;
    (*state)["requested_readbacks"] = std::move(comparisons);
    (*state)["all_requested_features_readable"] = all_readable;
    (*state)["all_requested_readbacks_match"] = all_match;
    (*state)["applied_state_status"] = all_match
        ? "confirmed"
        : (all_readable ? "mismatch" : "incomplete");
}

nlohmann::json capture_state(
    Emergent::CEmergentCamera* camera,
    const CameraIdentity& identity,
    const std::string& capture_stage,
    const nlohmann::json& requested_feature_values,
    const nlohmann::json& requested_feature_sources)
{
    nlohmann::json state = {
        {"schema_id", "orange.camera.sensor_pipeline_state"},
        {"schema_version", 1},
        {"captured_at_utc", utc_now()},
        {"capture_stage", capture_stage},
        {"read_only", true},
        {"camera", {
            {"serial", identity.serial},
            {"model", identity.model},
            {"firmware", identity.firmware},
            {"evt_sdk_version", std::string(Emergent::EVT_SDKVersion())},
        }},
        {"features", nlohmann::json::object()},
        {"lut_evidence", {
            {"selector_writes_performed", false},
            {"complete_table_collected", false},
            {"scope", "current_selector_value_only"},
        }},
    };

    int readable_count = 0;
    int unsupported_count = 0;
    int error_count = 0;
    for (const FeatureSpec& feature : feature_specs()) {
        nlohmann::json observation = read_feature(camera, feature);
        const std::string status = observation.value("status", std::string());
        if (status == "readable") {
            ++readable_count;
        } else if (status == "unsupported") {
            ++unsupported_count;
        } else {
            ++error_count;
        }
        state["features"][feature.name] = std::move(observation);
    }
    state["inventory_summary"] = {
        {"candidate_node_count", static_cast<int>(feature_specs().size())},
        {"readable_count", readable_count},
        {"unsupported_count", unsupported_count},
        {"error_count", error_count},
        {"status", error_count == 0 ? "complete" : "partial"},
    };

    std::string xml;
    std::string xml_error;
    if (read_genicam_xml(camera, &xml, &xml_error)) {
        state["genicam_xml"] = {
            {"status", "available"},
            {"sha256", "sha256:" +
                orange::gui::spatial_layout::checksum::sha256_hex(xml)},
            {"byte_count", static_cast<std::uint64_t>(xml.size())},
            {"embedded", false},
        };
    } else {
        state["genicam_xml"] = {
            {"status", "read_error"},
            {"error", xml_error},
            {"embedded", false},
        };
    }

    add_requested_readbacks(
        &state, requested_feature_values, requested_feature_sources);
    return state;
}

}  // namespace orange::camera_sensor_pipeline
