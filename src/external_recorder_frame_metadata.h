#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace orange::external_recorder {

struct FrameMetadataRecord {
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = 0;
    uint64_t bytes = 0;
};

struct FrameMetadataSummary {
    std::string path;
    uint64_t rows_written = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
    uint64_t zero_camera_timestamp_rows = 0;
    uint64_t zero_system_timestamp_rows = 0;
};

// Writes the authoritative frame-clock join table for a non-rolling external
// recorder video. Rows are accepted only in strictly increasing recording
// frame order so a successful summary proves both row count and continuity.
class FrameMetadataCsvWriter {
public:
    FrameMetadataCsvWriter() = default;
    ~FrameMetadataCsvWriter();

    FrameMetadataCsvWriter(const FrameMetadataCsvWriter&) = delete;
    FrameMetadataCsvWriter& operator=(const FrameMetadataCsvWriter&) = delete;

    bool Open(const std::string& path, std::string* error);
    bool Write(const FrameMetadataRecord& record, std::string* error);
    bool Flush(std::string* error);
    bool Close(std::string* error);

    bool is_open() const { return stream_.is_open(); }
    const FrameMetadataSummary& summary() const { return summary_; }

private:
    std::ofstream stream_;
    FrameMetadataSummary summary_;
};

std::string DeriveFrameMetadataPath(const std::string& mp4_path);

// Validates the terminal join-table proof against the authoritative encoded
// video. This is intentionally a finalization gate rather than a per-row
// rejection so the CSV and summary retain diagnostic evidence about gaps or
// missing clocks when a recording fails.
bool ValidateAuthoritativeFrameMetadata(
    const FrameMetadataSummary& summary,
    uint64_t frames_encoded,
    uint64_t packets_written,
    std::string* error);

}  // namespace orange::external_recorder
