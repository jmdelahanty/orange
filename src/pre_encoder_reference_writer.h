// src/pre_encoder_reference_writer.h

#ifndef ORANGE_PRE_ENCODER_REFERENCE_WRITER_H
#define ORANGE_PRE_ENCODER_REFERENCE_WRITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

#include "encoder_pipeline.h"
#include "json.hpp"

class PreEncoderReferenceWriter {
public:
    struct OpenParams {
        std::string camera_serial;
        std::string output_dir;
        std::string pixel_format = "nv12";
        std::string path_type;
        std::string source_path_flavor;
        bool resize_enabled = false;
        int width = 0;
        int height = 0;
        size_t pitch = 0;
        size_t frame_size = 0;
        nlohmann::json encoder_snapshot = nlohmann::json::object();
    };

    PreEncoderReferenceWriter() = default;
    ~PreEncoderReferenceWriter();

    void Configure(const PreEncoderReferenceCaptureConfig& config);
    bool Open(const OpenParams& params, std::string* error_out);
    bool ShouldCaptureNextFrame();
    bool AppendFrame(const void* bytes,
                     size_t size,
                     uint64_t recording_frame_id,
                     uint64_t timestamp,
                     uint64_t timestamp_sys,
                     std::string* error_out);
    void SetError(const std::string& error_message);
    void Close();

    bool enabled() const { return config_.enabled; }
    bool is_open() const { return is_open_; }
    nlohmann::json BuildSummaryJson() const;

private:
    bool budget_exhausted() const;
    bool write_metadata_file(std::string* error_out);
    void close_internal(const std::string& final_status);
    void reset_runtime_state();

    PreEncoderReferenceCaptureConfig config_;
    OpenParams params_;
    std::ofstream raw_dump_;
    std::ofstream index_;
    bool is_open_ = false;
    bool metadata_written_ = false;
    bool budget_reached_ = false;
    uint64_t frames_captured_ = 0;
    uint64_t bytes_written_ = 0;
    uint64_t next_byte_offset_ = 0;
    std::string status_ = "disabled";
    std::string error_;
    std::string started_at_utc_;
    std::string stopped_at_utc_;
    std::string raw_dump_path_;
    std::string index_path_;
    std::string metadata_path_;
    std::chrono::steady_clock::time_point capture_start_time_{};
    bool capture_start_time_valid_ = false;
};

#endif
