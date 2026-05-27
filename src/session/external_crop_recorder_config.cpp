#include "session/external_crop_recorder_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

namespace orange::session {
namespace {

std::string trim_ascii_copy(std::string value)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) {
        return !is_space(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) {
        return !is_space(c);
    }).base(), value.end());
    return value;
}

bool read_nonnegative_int_env(const char* name, const int max_value, int* value_out)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return false;
    }
    std::string text = trim_ascii_copy(raw);
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)) != 0) {
        ++end;
    }
    if (end == text.c_str() || (end && *end) || value < 0 || value > max_value) {
        std::cerr << "[recording_session] Ignoring invalid " << name << "='"
                  << raw << "'" << std::endl;
        return false;
    }
    if (value_out) {
        *value_out = static_cast<int>(value);
    }
    return true;
}

}  // namespace

int resolve_external_crop_recorder_gpu_id_from_env(const std::string& serial,
                                                   const int analytics_gpu_id)
{
    constexpr int kMaxGpuId = 255;
    const int fallback = analytics_gpu_id >= 0 ? analytics_gpu_id : 0;
    const std::string per_camera_env =
        "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_" + serial;

    int gpu_id = fallback;
    if (read_nonnegative_int_env(per_camera_env.c_str(), kMaxGpuId, &gpu_id)) {
        std::cout << "[recording_session] External crop recorder GPU for Cam"
                  << serial << ": " << gpu_id
                  << " (source=" << per_camera_env << ")" << std::endl;
        return gpu_id;
    }

    constexpr const char* kGlobalEnv = "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID";
    if (read_nonnegative_int_env(kGlobalEnv, kMaxGpuId, &gpu_id)) {
        std::cout << "[recording_session] External crop recorder GPU for Cam"
                  << serial << ": " << gpu_id
                  << " (source=" << kGlobalEnv << ")" << std::endl;
        return gpu_id;
    }

    return fallback;
}

}  // namespace orange::session
