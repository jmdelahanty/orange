#pragma once

#include "json.hpp"
#include "recording_output_descriptor.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace orange::session {

inline constexpr const char* kRecordingEncodingBudgetSchemaId =
    "orange.recording_encoding_budget";
inline constexpr int kRecordingEncodingBudgetSchemaVersion = 1;

// Recording-level averages. These values intentionally do not claim that a
// rate controller assigns the same number of bits to every encoded frame.
// Keyframes and difficult frames may consume substantially more than the
// average while easy predicted frames consume less.
struct RecordingEncodingBudgetInputs {
    std::string rate_control_strategy;
    bool target_bitrate_applicable = true;
    uint64_t target_average_bitrate_bps = 0;
    uint64_t target_maximum_bitrate_bps = 0;
    std::string target_source;

    double nominal_frame_rate_fps = 0.0;
    uint32_t width_px = 0;
    uint32_t height_px = 0;

    uint64_t encoded_frame_count = 0;
    uint64_t encoded_payload_bytes = 0;
    std::string encoded_payload_bytes_source;
    uint64_t container_bytes = 0;
    std::string container_bytes_source;
    double encoded_duration_s = 0.0;
    std::string encoded_duration_source;
};

inline nlohmann::json recording_encoding_nullable_number(
    const bool available,
    const long double value)
{
    if (!available || !std::isfinite(static_cast<double>(value))) {
        return nullptr;
    }
    return static_cast<double>(value);
}

inline nlohmann::json recording_encoding_nullable_u64(
    const bool available,
    const uint64_t value)
{
    return available ? nlohmann::json(value) : nlohmann::json(nullptr);
}

inline nlohmann::json build_recording_encoding_budget_json(
    const RecordingEncodingBudgetInputs& inputs)
{
    const bool fps_available = inputs.nominal_frame_rate_fps > 0.0;
    const uint64_t pixels_per_frame =
        static_cast<uint64_t>(inputs.width_px) *
        static_cast<uint64_t>(inputs.height_px);
    const bool pixels_available = pixels_per_frame > 0;

    const bool target_available =
        inputs.target_bitrate_applicable &&
        inputs.target_average_bitrate_bps > 0 &&
        fps_available;
    const long double target_bits_per_frame = target_available
        ? static_cast<long double>(inputs.target_average_bitrate_bps) /
              static_cast<long double>(inputs.nominal_frame_rate_fps)
        : 0.0L;
    const bool target_maximum_available =
        inputs.target_bitrate_applicable &&
        inputs.target_maximum_bitrate_bps > 0 &&
        fps_available;
    const long double target_maximum_bits_per_frame =
        target_maximum_available
            ? static_cast<long double>(inputs.target_maximum_bitrate_bps) /
                  static_cast<long double>(inputs.nominal_frame_rate_fps)
            : 0.0L;

    uint64_t achieved_basis_bytes = inputs.encoded_payload_bytes;
    std::string achieved_basis = "encoded_payload_bytes";
    std::string achieved_bytes_source = inputs.encoded_payload_bytes_source;
    if (achieved_basis_bytes == 0 && inputs.container_bytes > 0) {
        achieved_basis_bytes = inputs.container_bytes;
        achieved_basis = "mp4_container_bytes";
        achieved_bytes_source = inputs.container_bytes_source;
    }
    const bool achieved_frame_average_available =
        achieved_basis_bytes > 0 && inputs.encoded_frame_count > 0;
    const long double achieved_bits_per_frame =
        achieved_frame_average_available
            ? static_cast<long double>(achieved_basis_bytes) * 8.0L /
                  static_cast<long double>(inputs.encoded_frame_count)
            : 0.0L;

    double resolved_duration_s = inputs.encoded_duration_s;
    std::string duration_source = inputs.encoded_duration_source;
    if (resolved_duration_s <= 0.0 &&
        inputs.encoded_frame_count > 0 && fps_available) {
        resolved_duration_s =
            static_cast<double>(inputs.encoded_frame_count) /
            inputs.nominal_frame_rate_fps;
        duration_source = "encoded_frame_count_over_nominal_frame_rate";
    }
    const bool achieved_bitrate_available =
        achieved_basis_bytes > 0 && resolved_duration_s > 0.0;
    const long double achieved_average_bitrate_bps =
        achieved_bitrate_available
            ? static_cast<long double>(achieved_basis_bytes) * 8.0L /
                  static_cast<long double>(resolved_duration_s)
            : 0.0L;

    const std::string target_status = !inputs.target_bitrate_applicable
        ? "not_applicable"
        : (target_available ? "available" : "unavailable");
    const std::string achieved_status = achieved_frame_average_available
        ? "available"
        : "unavailable";

    return {
        {"schema_id", kRecordingEncodingBudgetSchemaId},
        {"schema_version", kRecordingEncodingBudgetSchemaVersion},
        {"semantics",
         {
             {"scope", "recording_level_average"},
             {"per_frame_allocation_is_uniform", false},
             {"description",
              "Average encoding budget; individual encoded frames may use more or fewer bits."}
         }},
        {"rate_control_strategy",
         inputs.rate_control_strategy.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(inputs.rate_control_strategy)},
        {"geometry",
         {
             {"nominal_frame_rate_fps",
              recording_encoding_nullable_number(
                  fps_available, inputs.nominal_frame_rate_fps)},
             {"width_px",
              recording_encoding_nullable_u64(inputs.width_px > 0, inputs.width_px)},
             {"height_px",
              recording_encoding_nullable_u64(inputs.height_px > 0, inputs.height_px)},
             {"pixels_per_frame",
              recording_encoding_nullable_u64(pixels_available, pixels_per_frame)}
         }},
        {"target",
         {
             {"status", target_status},
             {"average_bitrate_bps",
              recording_encoding_nullable_u64(
                  inputs.target_bitrate_applicable &&
                      inputs.target_average_bitrate_bps > 0,
                  inputs.target_average_bitrate_bps)},
             {"maximum_bitrate_bps",
              recording_encoding_nullable_u64(
                  inputs.target_bitrate_applicable &&
                      inputs.target_maximum_bitrate_bps > 0,
                  inputs.target_maximum_bitrate_bps)},
             {"average_bits_per_frame",
              recording_encoding_nullable_number(
                  target_available, target_bits_per_frame)},
             {"maximum_bits_per_frame",
              recording_encoding_nullable_number(
                  target_maximum_available, target_maximum_bits_per_frame)},
             {"bits_per_pixel_per_frame",
              recording_encoding_nullable_number(
                  target_available && pixels_available,
                  pixels_available
                      ? target_bits_per_frame /
                            static_cast<long double>(pixels_per_frame)
                      : 0.0L)},
             {"source",
              inputs.target_source.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(inputs.target_source)}
         }},
        {"achieved",
         {
             {"status", achieved_status},
             {"average_bitrate_bps",
              recording_encoding_nullable_number(
                  achieved_bitrate_available, achieved_average_bitrate_bps)},
             {"average_bits_per_frame",
              recording_encoding_nullable_number(
                  achieved_frame_average_available, achieved_bits_per_frame)},
             {"bits_per_pixel_per_frame",
              recording_encoding_nullable_number(
                  achieved_frame_average_available && pixels_available,
                  pixels_available
                      ? achieved_bits_per_frame /
                            static_cast<long double>(pixels_per_frame)
                      : 0.0L)},
             {"encoded_frame_count",
              recording_encoding_nullable_u64(
                  inputs.encoded_frame_count > 0,
                  inputs.encoded_frame_count)},
             {"encoded_payload_bytes",
              recording_encoding_nullable_u64(
                  inputs.encoded_payload_bytes > 0,
                  inputs.encoded_payload_bytes)},
             {"encoded_payload_bytes_source",
              inputs.encoded_payload_bytes_source.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(inputs.encoded_payload_bytes_source)},
             {"container_bytes",
              recording_encoding_nullable_u64(
                  inputs.container_bytes > 0,
                  inputs.container_bytes)},
             {"container_bytes_source",
              inputs.container_bytes_source.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(inputs.container_bytes_source)},
             {"average_basis", achieved_basis_bytes > 0
                  ? nlohmann::json(achieved_basis)
                  : nlohmann::json(nullptr)},
             {"average_basis_source", achieved_bytes_source.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(achieved_bytes_source)},
             {"encoded_duration_s",
              recording_encoding_nullable_number(
                  resolved_duration_s > 0.0, resolved_duration_s)},
             {"encoded_duration_source", duration_source.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(duration_source)}
         }}
    };
}

// Adds a finalized budget contract to any descriptor that does not already
// carry recorder-authored budget evidence. The fallback resolves encoder
// intent from recording_snapshot.json and uses the authoritative MP4 file size
// for achieved averages when exact encoded payload bytes are unavailable.
void populate_recording_output_encoding_budgets(
    std::vector<RecordingOutputDescriptor>* outputs,
    const std::string& recording_folder);

}  // namespace orange::session
