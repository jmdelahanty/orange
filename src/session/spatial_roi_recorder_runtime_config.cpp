#include "session/spatial_roi_recorder_runtime_config.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <set>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxLogicalStreamIdLength = 64;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool exact_keys(const json& value,
                const std::set<std::string>& required,
                std::string* error_out)
{
    constexpr const char* kPath = "spatial_roi_recorder_runtime";
    if (!value.is_object()) {
        return fail(error_out, std::string(kPath) + " must be an object");
    }
    for (const std::string& key : required) {
        if (!value.contains(key)) {
            return fail(error_out,
                        std::string(kPath) + "." + key + " is required");
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (required.count(it.key()) == 0) {
            return fail(error_out,
                        std::string(kPath) + "." + it.key() +
                            " is not allowed by schema v1");
        }
    }
    return true;
}

bool is_safe_logical_stream_id(const std::string& value)
{
    if (value.empty() || value.size() > kMaxLogicalStreamIdLength) {
        return false;
    }
    const auto is_ascii_alnum = [](const unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') ||
               (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(
        value.begin() + 1,
        value.end(),
        [&](const unsigned char ch) {
            return is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.';
        });
}

bool read_gpu_id(const json& value, int* gpu_id_out)
{
    std::uint64_t gpu_id = 0;
    if (value.is_number_unsigned()) {
        gpu_id = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const std::int64_t signed_gpu_id = value.get<std::int64_t>();
        if (signed_gpu_id < 0) {
            return false;
        }
        gpu_id = static_cast<std::uint64_t>(signed_gpu_id);
    } else {
        return false;
    }
    if (gpu_id > static_cast<std::uint64_t>(kRecorderRuntimeMaxGpuId)) {
        return false;
    }
    *gpu_id_out = static_cast<int>(gpu_id);
    return true;
}

}  // namespace

bool validate_recorder_runtime_config(const RecorderRuntimeConfig& config,
                                      std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (config.mode != kRecorderRuntimeModeExplicitPerStream) {
        return fail(error_out,
                    "spatial_roi_recorder_runtime.mode must be "
                    "explicit_per_stream");
    }
    if (config.recorder_gpu_by_logical_stream_id.empty()) {
        return fail(
            error_out,
            "spatial_roi_recorder_runtime.recorder_gpu_by_logical_stream_id "
            "must be a nonempty object");
    }
    for (const auto& [logical_stream_id, gpu_id] :
         config.recorder_gpu_by_logical_stream_id) {
        if (!is_safe_logical_stream_id(logical_stream_id)) {
            return fail(
                error_out,
                "spatial_roi_recorder_runtime."
                "recorder_gpu_by_logical_stream_id contains an unsafe logical "
                "stream ID");
        }
        if (gpu_id < 0 || gpu_id > kRecorderRuntimeMaxGpuId) {
            return fail(
                error_out,
                "spatial_roi_recorder_runtime."
                "recorder_gpu_by_logical_stream_id." +
                    logical_stream_id + " must be an integer in [0,255]");
        }
    }
    return true;
}

bool parse_recorder_runtime_config(const nlohmann::json& value,
                                   RecorderRuntimeConfig* config_out,
                                   std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!config_out) {
        return fail(error_out,
                    "spatial ROI recorder runtime config destination is null");
    }

    try {
        if (!exact_keys(value,
                        {"schema_id",
                         "schema_version",
                         "mode",
                         "recorder_gpu_by_logical_stream_id"},
                        error_out)) {
            return false;
        }
        if (!value.at("schema_id").is_string() ||
            value.at("schema_id").get<std::string>() !=
                kRecorderRuntimeConfigSchemaId ||
            !value.at("schema_version").is_number_integer() ||
            value.at("schema_version").get<std::int64_t>() !=
                kRecorderRuntimeConfigSchemaVersion) {
            return fail(
                error_out,
                "spatial ROI recorder runtime config schema_id/schema_version "
                "mismatch");
        }
        if (!value.at("mode").is_string()) {
            return fail(error_out,
                        "spatial_roi_recorder_runtime.mode must be a string");
        }

        const json& mapping =
            value.at("recorder_gpu_by_logical_stream_id");
        if (!mapping.is_object()) {
            return fail(
                error_out,
                "spatial_roi_recorder_runtime."
                "recorder_gpu_by_logical_stream_id must be an object");
        }

        RecorderRuntimeConfig parsed;
        parsed.mode = value.at("mode").get<std::string>();
        for (auto it = mapping.begin(); it != mapping.end(); ++it) {
            int gpu_id = -1;
            if (!read_gpu_id(it.value(), &gpu_id)) {
                return fail(
                    error_out,
                    "spatial_roi_recorder_runtime."
                    "recorder_gpu_by_logical_stream_id." +
                        it.key() + " must be an integer in [0,255]");
            }
            parsed.recorder_gpu_by_logical_stream_id.emplace(it.key(), gpu_id);
        }
        if (!validate_recorder_runtime_config(parsed, error_out)) {
            return false;
        }
        *config_out = std::move(parsed);
        return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI recorder runtime config parse failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI recorder runtime config parse failed: unknown "
                    "exception");
    }
}

bool serialize_recorder_runtime_config(const RecorderRuntimeConfig& config,
                                       nlohmann::json* value_out,
                                       std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!value_out) {
        return fail(error_out,
                    "spatial ROI recorder runtime JSON destination is null");
    }
    if (!validate_recorder_runtime_config(config, error_out)) {
        return false;
    }

    try {
        json mapping = json::object();
        for (const auto& [logical_stream_id, gpu_id] :
             config.recorder_gpu_by_logical_stream_id) {
            mapping[logical_stream_id] = gpu_id;
        }
        json serialized = {
            {"schema_id", kRecorderRuntimeConfigSchemaId},
            {"schema_version", kRecorderRuntimeConfigSchemaVersion},
            {"mode", config.mode},
            {"recorder_gpu_by_logical_stream_id", std::move(mapping)},
        };
        *value_out = std::move(serialized);
        return true;
    } catch (const std::exception& ex) {
        return fail(
            error_out,
            std::string("spatial ROI recorder runtime config serialization failed: ") +
                ex.what());
    } catch (...) {
        return fail(
            error_out,
            "spatial ROI recorder runtime config serialization failed: unknown "
            "exception");
    }
}

}  // namespace orange::session::spatial_roi
