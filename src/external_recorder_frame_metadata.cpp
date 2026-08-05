#include "external_recorder_frame_metadata.h"

#include <filesystem>
#include <sstream>

namespace orange::external_recorder {

namespace {

void set_error(std::string* error, const std::string& message)
{
    if (error) {
        *error = message;
    }
}

}  // namespace

FrameMetadataCsvWriter::~FrameMetadataCsvWriter()
{
    if (stream_.is_open()) {
        stream_.close();
    }
}

bool FrameMetadataCsvWriter::Open(const std::string& path, std::string* error)
{
    if (path.empty()) {
        set_error(error, "frame metadata path is empty");
        return false;
    }
    if (stream_.is_open()) {
        set_error(error, "frame metadata writer is already open");
        return false;
    }

    summary_ = FrameMetadataSummary{};
    stream_.open(path, std::ios::out | std::ios::trunc);
    if (!stream_) {
        set_error(error, "failed to open frame metadata CSV: " + path);
        return false;
    }
    summary_.path = path;
    stream_
        << "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id,"
           "gop_index,frame_index_within_gop,source_gpu_id,assigned_gpu_id,"
           "assigned_shard_id,bytes\n";
    if (!stream_) {
        set_error(error, "failed to write frame metadata CSV header: " + path);
        stream_.close();
        return false;
    }
    return true;
}

bool FrameMetadataCsvWriter::Write(const FrameMetadataRecord& record,
                                   std::string* error)
{
    if (!stream_.is_open()) {
        set_error(error, "frame metadata writer is not open");
        return false;
    }
    if (record.recording_frame_id == 0) {
        set_error(error, "frame metadata recording_frame_id must be positive");
        return false;
    }
    if (summary_.rows_written > 0 &&
        record.recording_frame_id <= summary_.last_recording_frame_id) {
        std::ostringstream message;
        message << "frame metadata recording_frame_id is not strictly increasing: previous="
                << summary_.last_recording_frame_id
                << " current=" << record.recording_frame_id;
        set_error(error, message.str());
        return false;
    }

    stream_
        << record.recording_frame_id << ','
        << record.timestamp << ','
        << record.timestamp_sys << ','
        << record.recording_frame_id << ','
        << record.local_frame_id << ','
        << record.gop_index << ','
        << record.frame_index_within_gop << ','
        << record.source_gpu_id << ','
        << record.assigned_gpu_id << ','
        << record.assigned_shard_id << ','
        << record.bytes << '\n';
    if (!stream_) {
        set_error(error, "failed while writing frame metadata CSV: " + summary_.path);
        return false;
    }

    if (summary_.rows_written == 0) {
        summary_.first_recording_frame_id = record.recording_frame_id;
    } else if (record.recording_frame_id > summary_.last_recording_frame_id + 1) {
        summary_.recording_frame_id_gaps +=
            record.recording_frame_id - summary_.last_recording_frame_id - 1;
    }
    summary_.last_recording_frame_id = record.recording_frame_id;
    summary_.rows_written++;
    if (record.timestamp == 0) {
        summary_.zero_camera_timestamp_rows++;
    }
    if (record.timestamp_sys == 0) {
        summary_.zero_system_timestamp_rows++;
    }
    return true;
}

bool FrameMetadataCsvWriter::Flush(std::string* error)
{
    if (!stream_.is_open()) {
        return true;
    }
    stream_.flush();
    if (!stream_) {
        set_error(error, "failed to flush frame metadata CSV: " + summary_.path);
        return false;
    }
    return true;
}

bool FrameMetadataCsvWriter::Close(std::string* error)
{
    if (!stream_.is_open()) {
        return true;
    }
    if (!Flush(error)) {
        stream_.close();
        return false;
    }
    stream_.close();
    if (stream_.fail()) {
        set_error(error, "failed to close frame metadata CSV: " + summary_.path);
        return false;
    }
    return true;
}

std::string DeriveFrameMetadataPath(const std::string& mp4_path)
{
    if (mp4_path.empty()) {
        return {};
    }
    std::filesystem::path path(mp4_path);
    path.replace_filename(path.stem().string() + "_meta.csv");
    return path.string();
}

bool ValidateAuthoritativeFrameMetadata(
    const FrameMetadataSummary& summary,
    const uint64_t frames_encoded,
    const uint64_t packets_written,
    std::string* error)
{
    auto fail = [&](const std::string& reason) {
        set_error(error, "authoritative frame metadata is incomplete: " + reason);
        return false;
    };
    if (frames_encoded == 0) {
        return fail("frames_encoded is zero");
    }
    if (packets_written != frames_encoded) {
        return fail(
            "packets_written=" + std::to_string(packets_written) +
            " frames_encoded=" + std::to_string(frames_encoded));
    }
    if (summary.path.empty()) {
        return fail("path is empty");
    }
    if (summary.rows_written != frames_encoded) {
        return fail(
            "rows_written=" + std::to_string(summary.rows_written) +
            " frames_encoded=" + std::to_string(frames_encoded));
    }
    if (summary.first_recording_frame_id == 0 ||
        summary.last_recording_frame_id == 0) {
        return fail("first or last recording_frame_id is zero");
    }
    if (summary.zero_camera_timestamp_rows != 0) {
        return fail(
            "zero_camera_timestamp_rows=" +
            std::to_string(summary.zero_camera_timestamp_rows));
    }
    if (summary.zero_system_timestamp_rows != 0) {
        return fail(
            "zero_system_timestamp_rows=" +
            std::to_string(summary.zero_system_timestamp_rows));
    }
    return true;
}

}  // namespace orange::external_recorder
