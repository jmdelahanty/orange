#pragma once

#include "encoder_pipeline.h"
#include "json.hpp"

#include <string>

namespace orange::headless {

struct RecordingProfile {
    std::string codec;
    std::string preset;
    std::string tuning;
    std::string rate_control_mode;
    int quality_value = 20;
    int gop_length = 0;
    EncoderControlOverrides control;
    ImportanceMapConfig importance_map;
};

bool ParseRecordingProfile(const nlohmann::json& value,
                           RecordingProfile* profile_out,
                           std::string* error_out = nullptr,
                           const std::string& context = "recording_profile");

nlohmann::json BuildRecordingProfileJson(const RecordingProfile& profile);

}  // namespace orange::headless
