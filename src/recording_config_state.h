#pragma once

#include <string>

struct EncoderConfig {
    std::string encoder_codec;
    std::string encoder_preset;
    std::string tuning_info;
    std::string rate_control_mode;
    int quality_value;
    int gop_length;
    std::string record_output_mode;
    int record_downsample_factor;
    int record_output_width;
    int record_output_height;
    std::string folder_name;
};
