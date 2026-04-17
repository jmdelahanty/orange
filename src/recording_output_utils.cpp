#include "recording_output_utils.h"

#include "project.h"

namespace {

bool is_supported_record_output_factor(const int factor)
{
    return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
}

}  // namespace

namespace orange::recording {

void sanitize_record_output_config(std::string* mode, int* factor, int* width, int* height)
{
    if (*mode != "exact_size") {
        *mode = "factor";
    }
    if (!is_supported_record_output_factor(*factor)) {
        *factor = 1;
    }
    if (*width < 1) {
        *width = 1024;
    }
    if (*height < 1) {
        *height = 1024;
    }
}

std::string record_output_summary(const std::string& mode, const int factor, const int width, const int height)
{
    if (mode == "exact_size") {
        return std::string("exact ") + std::to_string(width) + "x" + std::to_string(height);
    }
    return std::string("factor ") + std::to_string(factor) + "x";
}

std::string format_bitrate_mbps(const uint32_t bitrate_bps)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << (static_cast<double>(bitrate_bps) / 1000000.0);
    return oss.str();
}

RecordingOutputConfig resolve_recording_output_config(
    const CameraParams& camera_params,
    const EncoderConfig& encoder_config,
    const CameraEachSelect& camera_select,
    std::string* warning_out)
{
    std::string mode = camera_select.record_output_override
        ? camera_select.record_output_mode
        : encoder_config.record_output_mode;
    int factor = camera_select.record_output_override
        ? camera_select.record_downsample_factor
        : encoder_config.record_downsample_factor;
    int width = camera_select.record_output_override
        ? camera_select.record_output_width
        : encoder_config.record_output_width;
    int height = camera_select.record_output_override
        ? camera_select.record_output_height
        : encoder_config.record_output_height;
    sanitize_record_output_config(&mode, &factor, &width, &height);
    CameraRecordingOutputConfig requested_output;
    requested_output.mode = mode;
    requested_output.downsample_factor = factor;
    requested_output.requested_width = width;
    requested_output.requested_height = height;
    return resolve_effective_recording_output_config(camera_params, requested_output, warning_out);
}

}  // namespace orange::recording
