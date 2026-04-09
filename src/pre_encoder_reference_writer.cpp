// src/pre_encoder_reference_writer.cpp

#include "pre_encoder_reference_writer.h"

#include <filesystem>

#include "fsuid_guard.h"
#include "project.h"

namespace {
constexpr const char* kCaptureMode = "pre_encoder_reference";
}

PreEncoderReferenceWriter::~PreEncoderReferenceWriter()
{
    Close();
}

void PreEncoderReferenceWriter::Configure(const PreEncoderReferenceCaptureConfig& config)
{
    Close();
    config_ = config;
    params_ = OpenParams{};
    reset_runtime_state();
    status_ = config_.enabled ? "configured" : "disabled";
}

bool PreEncoderReferenceWriter::Open(const OpenParams& params, std::string* error_out)
{
    reset_runtime_state();
    params_ = params;

    if (!config_.enabled) {
        status_ = "disabled";
        return true;
    }
    if (!config_.has_valid_bound()) {
        const std::string message =
            "pre_encoder_reference_capture requires exactly one positive bound: max_frames or max_seconds";
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }
    if (params_.camera_serial.empty() || params_.output_dir.empty() ||
        params_.width <= 0 || params_.height <= 0 || params_.pitch == 0 || params_.frame_size == 0) {
        const std::string message = "pre-encoder reference capture open parameters are incomplete";
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }

    const std::filesystem::path output_dir(params_.output_dir);
    raw_dump_path_ = (output_dir / ("Cam" + params_.camera_serial + "_preenc_ref.bin")).string();
    index_path_ = (output_dir / ("Cam" + params_.camera_serial + "_preenc_ref_index.csv")).string();
    metadata_path_ = (output_dir / ("Cam" + params_.camera_serial + "_preenc_ref.json")).string();

    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::error_code create_error;
        std::filesystem::create_directories(output_dir, create_error);
        if (create_error && !std::filesystem::exists(output_dir)) {
            const std::string message =
                "Failed to create pre-encoder reference output directory " + output_dir.string() +
                ": " + create_error.message();
            if (error_out) {
                *error_out = message;
            }
            SetError(message);
            return false;
        }

        raw_dump_.open(raw_dump_path_, std::ios::binary | std::ios::trunc);
        if (!raw_dump_) {
            const std::string message = "Failed to open " + raw_dump_path_ + " for writing";
            if (error_out) {
                *error_out = message;
            }
            SetError(message);
            return false;
        }

        index_.open(index_path_, std::ios::trunc);
        if (!index_) {
            const std::string message = "Failed to open " + index_path_ + " for writing";
            if (error_out) {
                *error_out = message;
            }
            SetError(message);
            return false;
        }
    }

    index_ << "reference_frame_index,recording_frame_id,timestamp,timestamp_sys,byte_offset,byte_size\n";
    if (!index_) {
        const std::string message = "Failed to write header to " + index_path_;
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }

    started_at_utc_ = get_current_utc_timestamp();
    capture_start_time_ = std::chrono::steady_clock::now();
    capture_start_time_valid_ = true;
    status_ = "capturing";
    is_open_ = true;
    return true;
}

bool PreEncoderReferenceWriter::ShouldCaptureNextFrame()
{
    if (!config_.enabled || !is_open_) {
        return false;
    }
    if (budget_exhausted()) {
        budget_reached_ = true;
        close_internal("budget_reached");
        return false;
    }
    return true;
}

bool PreEncoderReferenceWriter::AppendFrame(const void* bytes,
                                           size_t size,
                                           uint64_t recording_frame_id,
                                           uint64_t timestamp,
                                           uint64_t timestamp_sys,
                                           std::string* error_out)
{
    if (!config_.enabled || !is_open_) {
        return false;
    }
    if (!bytes || size != params_.frame_size) {
        const std::string message = "Pre-encoder reference frame size mismatch";
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }

    raw_dump_.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!raw_dump_) {
        const std::string message = "Failed to append frame bytes to " + raw_dump_path_;
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }

    index_ << frames_captured_ << ","
           << recording_frame_id << ","
           << timestamp << ","
           << timestamp_sys << ","
           << next_byte_offset_ << ","
           << size << "\n";
    if (!index_) {
        const std::string message = "Failed to append frame index to " + index_path_;
        if (error_out) {
            *error_out = message;
        }
        SetError(message);
        return false;
    }

    next_byte_offset_ += static_cast<uint64_t>(size);
    bytes_written_ += static_cast<uint64_t>(size);
    frames_captured_++;

    if (budget_exhausted()) {
        budget_reached_ = true;
        close_internal("budget_reached");
    }
    return true;
}

void PreEncoderReferenceWriter::SetError(const std::string& error_message)
{
    if (!error_message.empty()) {
        error_ = error_message;
    }
    close_internal("error");
}

void PreEncoderReferenceWriter::Close()
{
    if (status_ == "disabled" || status_ == "configured") {
        return;
    }
    close_internal(status_ == "error" ? "error" : "completed");
}

nlohmann::json PreEncoderReferenceWriter::BuildSummaryJson() const
{
    nlohmann::json info = {
        {"capture_mode", kCaptureMode},
        {"enabled", config_.enabled},
        {"max_frames", config_.max_frames},
        {"max_seconds", config_.max_seconds},
        {"status", status_},
        {"frames_captured", frames_captured_},
        {"bytes_written", bytes_written_},
        {"budget_reached", budget_reached_}
    };

    if (!params_.output_dir.empty()) {
        info["output_dir"] = params_.output_dir;
    }
    if (params_.width > 0 && params_.height > 0) {
        info["width"] = params_.width;
        info["height"] = params_.height;
        info["pitch"] = params_.pitch;
        info["frame_size"] = params_.frame_size;
        info["pixel_format"] = params_.pixel_format;
        info["path_type"] = params_.path_type;
        info["source_path_flavor"] = params_.source_path_flavor;
        info["resize_enabled"] = params_.resize_enabled;
    }
    if (!started_at_utc_.empty()) {
        info["started_at_utc"] = started_at_utc_;
    }
    if (!stopped_at_utc_.empty()) {
        info["stopped_at_utc"] = stopped_at_utc_;
    }
    if (!error_.empty()) {
        info["error"] = error_;
    }

    info["artifacts"] = {
        {"raw_dump", raw_dump_path_},
        {"index", index_path_},
        {"metadata", metadata_path_}
    };
    return info;
}

bool PreEncoderReferenceWriter::budget_exhausted() const
{
    if (!config_.enabled) {
        return false;
    }
    if (config_.max_frames > 0 &&
        frames_captured_ >= static_cast<uint64_t>(config_.max_frames)) {
        return true;
    }
    if (config_.max_seconds > 0 && capture_start_time_valid_) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - capture_start_time_);
        if (elapsed.count() >= config_.max_seconds) {
            return true;
        }
    }
    return false;
}

bool PreEncoderReferenceWriter::write_metadata_file(std::string* error_out)
{
    if (metadata_written_ || metadata_path_.empty()) {
        return true;
    }

    nlohmann::json metadata = {
        {"capture_mode", kCaptureMode},
        {"camera_serial", params_.camera_serial},
        {"width", params_.width},
        {"height", params_.height},
        {"pitch", params_.pitch},
        {"frame_size", params_.frame_size},
        {"pixel_format", params_.pixel_format},
        {"path_type", params_.path_type},
        {"source_path_flavor", params_.source_path_flavor},
        {"resize_enabled", params_.resize_enabled},
        {"capture", BuildSummaryJson()},
        {"encoder_snapshot", params_.encoder_snapshot}
    };

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream metadata_file(metadata_path_, std::ios::trunc);
    if (!metadata_file) {
        if (error_out) {
            *error_out = "Failed to open " + metadata_path_ + " for writing";
        }
        return false;
    }
    metadata_file << metadata.dump(2) << "\n";
    if (!metadata_file) {
        if (error_out) {
            *error_out = "Failed to write metadata to " + metadata_path_;
        }
        return false;
    }
    metadata_written_ = true;
    return true;
}

void PreEncoderReferenceWriter::close_internal(const std::string& final_status)
{
    if (is_open_) {
        raw_dump_.flush();
        index_.flush();
        raw_dump_.close();
        index_.close();
        is_open_ = false;
    }

    if (stopped_at_utc_.empty() && !started_at_utc_.empty()) {
        stopped_at_utc_ = get_current_utc_timestamp();
    }
    status_ = final_status;

    std::string metadata_error;
    if (!write_metadata_file(&metadata_error) && error_.empty()) {
        error_ = metadata_error;
        status_ = "error";
    }
}

void PreEncoderReferenceWriter::reset_runtime_state()
{
    if (raw_dump_.is_open()) {
        raw_dump_.close();
    }
    if (index_.is_open()) {
        index_.close();
    }
    is_open_ = false;
    metadata_written_ = false;
    budget_reached_ = false;
    frames_captured_ = 0;
    bytes_written_ = 0;
    next_byte_offset_ = 0;
    error_.clear();
    started_at_utc_.clear();
    stopped_at_utc_.clear();
    raw_dump_path_.clear();
    index_path_.clear();
    metadata_path_.clear();
    capture_start_time_valid_ = false;
}
