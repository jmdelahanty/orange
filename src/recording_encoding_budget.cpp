#include "recording_encoding_budget.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace orange::session {

namespace {

nlohmann::json read_recording_snapshot_for_encoding_budget(
    const std::string& recording_folder)
{
    if (recording_folder.empty()) {
        return nlohmann::json::object();
    }
    std::ifstream input(
        std::filesystem::path(recording_folder) / "recording_snapshot.json");
    if (!input) {
        return nlohmann::json::object();
    }
    try {
        nlohmann::json snapshot;
        input >> snapshot;
        return snapshot.is_object() ? snapshot : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

uint64_t positive_json_u64(const nlohmann::json& object,
                           const char* key)
{
    if (!object.is_object()) {
        return 0;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    if (it->is_number_integer()) {
        const int64_t value = it->get<int64_t>();
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }
    const double value = it->get<double>();
    return value > 0.0 ? static_cast<uint64_t>(std::llround(value)) : 0;
}

void populate_recording_output_encoding_budget(
    RecordingOutputDescriptor* output,
    const std::string& recording_folder,
    const nlohmann::json& snapshot)
{
    if (!output ||
        (output->encoding_budget.is_object() &&
         !output->encoding_budget.empty())) {
        return;
    }

    RecordingEncodingBudgetInputs budget;
    budget.encoded_frame_count = output->frame_count;
    budget.nominal_frame_rate_fps = output->frame_rate;
    budget.width_px = output->width > 0
        ? static_cast<uint32_t>(output->width)
        : 0U;
    budget.height_px = output->height > 0
        ? static_cast<uint32_t>(output->height)
        : 0U;
    budget.rate_control_strategy = output->tuning == "lossless"
        ? "lossless"
        : std::string();
    budget.target_bitrate_applicable = output->tuning != "lossless";

    const nlohmann::json camera_runtime =
        snapshot.value("camera_runtime", nlohmann::json::object())
            .value(output->camera_serial, nlohmann::json::object());
    const nlohmann::json authority = camera_runtime.value(
        "recording_config_authority", nlohmann::json::object());
    const nlohmann::json authority_encode = authority.value(
        "encode", nlohmann::json::object());
    const nlohmann::json authority_output = authority.value(
        "output", nlohmann::json::object());
    if (budget.nominal_frame_rate_fps <= 0.0) {
        budget.nominal_frame_rate_fps =
            static_cast<double>(positive_json_u64(authority, "frame_rate"));
    }
    if (budget.width_px == 0) {
        budget.width_px = static_cast<uint32_t>(
            positive_json_u64(authority_output, "resolved_width"));
    }
    if (budget.height_px == 0) {
        budget.height_px = static_cast<uint32_t>(
            positive_json_u64(authority_output, "resolved_height"));
    }
    const std::string authority_tuning = authority_encode.value(
        "tuning", output->tuning);
    const std::string authority_rate_control = authority_encode.value(
        "rate_control_mode", std::string());
    if (!authority_tuning.empty()) {
        output->tuning = output->tuning.empty()
            ? authority_tuning
            : output->tuning;
    }
    if (authority_tuning == "lossless") {
        budget.rate_control_strategy = "lossless";
        budget.target_bitrate_applicable = false;
    } else if (authority_rate_control == "cqp") {
        budget.rate_control_strategy = "cqp";
        budget.target_bitrate_applicable = false;
    } else if (!authority_rate_control.empty()) {
        budget.rate_control_strategy = authority_rate_control;
    }
    if (budget.target_bitrate_applicable) {
        budget.target_average_bitrate_bps =
            positive_json_u64(authority_encode, "target_bitrate_bps");
        budget.target_maximum_bitrate_bps =
            positive_json_u64(authority_encode, "max_bitrate_bps");
        if (budget.target_average_bitrate_bps > 0 ||
            budget.target_maximum_bitrate_bps > 0) {
            budget.target_source =
                "recording_snapshot.camera_runtime.recording_config_authority.encode";
        }
    }

    // In-process encoder snapshots carry the fully resolved NVENC settings,
    // including automatically derived bitrate values that do not appear as
    // explicit recording-config overrides.
    nlohmann::json encoder = snapshot.value(
        "encoders", nlohmann::json::object())
            .value(output->camera_serial, nlohmann::json::object());
    if (encoder.contains("outputs") && encoder["outputs"].is_object()) {
        const nlohmann::json candidate = encoder["outputs"].value(
            output->output_kind.empty() ? "full" : output->output_kind,
            nlohmann::json::object());
        if (candidate.is_object() && candidate.contains("rc")) {
            encoder = candidate;
        }
    }
    const nlohmann::json rc = encoder.value("rc", nlohmann::json::object());
    if (budget.target_bitrate_applicable &&
        budget.target_average_bitrate_bps == 0) {
        budget.target_average_bitrate_bps =
            positive_json_u64(rc, "average_bitrate");
        budget.target_maximum_bitrate_bps =
            positive_json_u64(rc, "max_bitrate");
        if (budget.target_average_bitrate_bps > 0) {
            budget.target_source = "recording_snapshot.encoders.rc";
        }
    }
    if (budget.nominal_frame_rate_fps <= 0.0) {
        budget.nominal_frame_rate_fps =
            static_cast<double>(positive_json_u64(encoder, "fps"));
    }
    const nlohmann::json resolution = encoder.value(
        "resolution", nlohmann::json::object());
    if (budget.width_px == 0) {
        budget.width_px = static_cast<uint32_t>(
            positive_json_u64(resolution, "width"));
    }
    if (budget.height_px == 0) {
        budget.height_px = static_cast<uint32_t>(
            positive_json_u64(resolution, "height"));
    }

    if (!output->video_path.empty()) {
        std::filesystem::path video_path(output->video_path);
        if (video_path.is_relative()) {
            video_path = std::filesystem::path(recording_folder) / video_path;
        }
        std::error_code size_error;
        const uintmax_t size = std::filesystem::file_size(video_path, size_error);
        if (!size_error && size > 0 &&
            size <= std::numeric_limits<uint64_t>::max()) {
            budget.container_bytes = static_cast<uint64_t>(size);
            budget.container_bytes_source = "authoritative_mp4_file_size";
        }
    }
    budget.encoded_duration_source =
        "encoded_frame_count_over_nominal_frame_rate";
    output->encoding_budget = build_recording_encoding_budget_json(budget);
}

}  // namespace

void populate_recording_output_encoding_budgets(
    std::vector<RecordingOutputDescriptor>* outputs,
    const std::string& recording_folder)
{
    if (!outputs) {
        return;
    }
    const nlohmann::json snapshot =
        read_recording_snapshot_for_encoding_budget(recording_folder);
    for (RecordingOutputDescriptor& output : *outputs) {
        populate_recording_output_encoding_budget(
            &output, recording_folder, snapshot);
    }
}

}  // namespace orange::session
