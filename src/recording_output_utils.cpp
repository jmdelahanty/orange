#include "recording_output_utils.h"

#include "project.h"

#include <iomanip>
#include <sstream>

namespace {

bool is_supported_record_output_factor(const int factor)
{
    return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
}

bool encode_defaults_match(const CameraRecordingEncodeConfig& lhs,
                           const CameraRecordingEncodeConfig& rhs)
{
    return lhs.codec == rhs.codec &&
           lhs.preset == rhs.preset &&
           lhs.tuning == rhs.tuning &&
           lhs.rate_control_mode == rhs.rate_control_mode &&
           lhs.quality_value == rhs.quality_value &&
           lhs.gop_length == rhs.gop_length &&
           lhs.nvenc_direct_input == rhs.nvenc_direct_input;
}

bool output_defaults_match(const CameraRecordingOutputConfig& lhs,
                           const CameraRecordingOutputConfig& rhs)
{
    return lhs.mode == rhs.mode &&
           lhs.downsample_factor == rhs.downsample_factor &&
           lhs.requested_width == rhs.requested_width &&
           lhs.requested_height == rhs.requested_height;
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

RecordingConfigSyncResult sync_encoder_config_from_camera_defaults(const CameraParams* cameras_params,
                                                                   const int num_cameras,
                                                                   EncoderConfig* encoder_config)
{
    RecordingConfigSyncResult result;
    if (!cameras_params || !encoder_config || num_cameras <= 0) {
        return result;
    }

    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_params[i].config_schema_version < 3) {
            result.warning = true;
            result.message =
                "Open cameras do not all use schema-3 recording configs; GUI recording controls were left unchanged.";
            return result;
        }
    }

    const CameraRecordingEncodeConfig& reference_encode = cameras_params[0].recording.encode;
    const CameraRecordingOutputConfig& reference_output = cameras_params[0].recording.output;
    for (int i = 1; i < num_cameras; ++i) {
        if (!encode_defaults_match(reference_encode, cameras_params[i].recording.encode) ||
            !output_defaults_match(reference_output, cameras_params[i].recording.output)) {
            result.warning = true;
            result.message =
                "Open cameras have mixed recording encode/output defaults; GUI recording controls were left unchanged.";
            return result;
        }
    }

    encoder_config->encoder_codec = reference_encode.codec;
    encoder_config->encoder_preset = reference_encode.preset;
    encoder_config->tuning_info = reference_encode.tuning;
    encoder_config->rate_control_mode = reference_encode.rate_control_mode;
    encoder_config->quality_value = reference_encode.quality_value;
    encoder_config->gop_length = reference_encode.gop_length;
    encoder_config->record_output_mode = reference_output.mode;
    encoder_config->record_downsample_factor = reference_output.downsample_factor;
    encoder_config->record_output_width = reference_output.requested_width;
    encoder_config->record_output_height = reference_output.requested_height;
    sanitize_record_output_config(
        &encoder_config->record_output_mode,
        &encoder_config->record_downsample_factor,
        &encoder_config->record_output_width,
        &encoder_config->record_output_height);

    result.applied = true;
    std::ostringstream status;
    status << "Loaded recording defaults from camera config: "
           << encoder_config->encoder_codec
           << ", GOP "
           << encoder_config->gop_length
           << ", "
           << record_output_summary(
                  encoder_config->record_output_mode,
                  encoder_config->record_downsample_factor,
                  encoder_config->record_output_width,
                  encoder_config->record_output_height);
    result.message = status.str();
    return result;
}

}  // namespace orange::recording
