#include "shared_recording_output.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "camera.h"
#include "fsuid_guard.h"
#include "project.h"

namespace {
void initialize_writer_locked(Writer* writer,
                              const SharedRecordingOutputOpenParams& params)
{
    writer->video_file = params.folder_name + "/Cam" + params.camera_params->camera_serial + ".mp4";
    writer->metadata_file = params.folder_name + "/Cam" + params.camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = params.folder_name + "/Cam" + params.camera_params->camera_serial + "_keyframe.csv";

    const AVCodecID codec_id =
        params.codec.find("h264") != std::string::npos ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    writer->video = new FFmpegWriter(
        codec_id,
        params.recording_output_config.resolved_width,
        params.recording_output_config.resolved_height,
        params.camera_params->frame_rate,
        writer->video_file.c_str(),
        writer->keyframe_file.c_str(),
        params.metadata_tags,
        params.queue_config);
    writer->metadata = new std::ofstream();
    writer->metadata->open(writer->metadata_file.c_str());
    if (!(*writer->metadata)) {
        delete writer->metadata;
        writer->metadata = nullptr;
        throw std::runtime_error("Failed to open recording metadata file");
    }
    *writer->metadata << "frame_id,timestamp,timestamp_sys\n";
    writer->video->create_thread();
}

void close_writer_locked(Writer* writer)
{
    if (writer->video) {
        writer->video->quit_thread();
        writer->video->join_thread();
        delete writer->video;
        writer->video = nullptr;
    }
    if (writer->metadata) {
        if (writer->metadata->is_open()) {
            writer->metadata->close();
        }
        delete writer->metadata;
        writer->metadata = nullptr;
    }
}
} // namespace

SharedRecordingOutput::~SharedRecordingOutput()
{
    close();
}

void SharedRecordingOutput::open_if_needed(const SharedRecordingOutputOpenParams& params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_open_) {
        if (active_folder_ != params.folder_name) {
            throw std::runtime_error(
                "SharedRecordingOutput already open for " + active_folder_ +
                " but requested to open " + params.folder_name);
        }
        active_worker_sessions_++;
        return;
    }
    open_locked(params);
    active_worker_sessions_++;
}

void SharedRecordingOutput::open_locked(const SharedRecordingOutputOpenParams& params)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    make_folder(params.folder_name);

    active_folder_ = params.folder_name;
    active_worker_sessions_ = 0;
    close_requested_ = false;
    split_gop_config_ = params.split_gop_config;
    recording_gop_length_ = std::max<uint32_t>(1u, params.recording_gop_length);
    reset_pending_state_locked();
    writer_queue_overflowed_ = false;
    writer_queue_overflow_events_ = 0;
    writer_queue_peak_packets_ = 0;
    writer_queue_peak_bytes_ = 0;

    initialize_writer_locked(&writer_, params);
    is_open_ = true;
}

void SharedRecordingOutput::submit_frame_output(
    const std::vector<std::vector<uint8_t>>& packets,
    const std::vector<uint64_t>& output_timestamps,
    int64_t fallback_sample_index,
    uint64_t completion_gop_index,
    bool mark_complete,
    const std::optional<RecordingMetadataRow>& metadata_row)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        throw std::runtime_error("SharedRecordingOutput received packets before open");
    }
    buffer_packets_locked(
        packets,
        output_timestamps,
        fallback_sample_index,
        completion_gop_index,
        mark_complete,
        metadata_row);
}

void SharedRecordingOutput::buffer_packets_locked(
    const std::vector<std::vector<uint8_t>>& packets,
    const std::vector<uint64_t>& output_timestamps,
    int64_t fallback_sample_index,
    uint64_t completion_gop_index,
    bool mark_complete,
    const std::optional<RecordingMetadataRow>& metadata_row)
{
    if (!split_gop_config_.enabled) {
        for (size_t i = 0; i < packets.size(); ++i) {
            const int64_t sample_index = i < output_timestamps.size()
                ? static_cast<int64_t>(output_timestamps[i])
                : fallback_sample_index;
            if (writer_.video) {
                writer_.video->push_packet(
                    const_cast<uint8_t*>(packets[i].data()),
                    static_cast<int>(packets[i].size()),
                    sample_index);
            }
        }
        if (metadata_row.has_value()) {
            write_metadata_row_locked(*metadata_row);
        }
        refresh_writer_queue_metrics_locked();
        return;
    }

    const auto append_metadata_row = [&](uint64_t gop_index, const RecordingMetadataRow& row) {
        auto [it, inserted] = pending_gops_.try_emplace(gop_index);
        PendingGop& pending = it->second;
        if (inserted) {
            pending.gop_index = gop_index;
            pending.created_at = std::chrono::steady_clock::now();
            update_pending_gop_peaks_locked();
        }
        pending.metadata_rows.push_back(row);
    };

    for (size_t i = 0; i < packets.size(); ++i) {
        const int64_t sample_index = i < output_timestamps.size()
            ? static_cast<int64_t>(output_timestamps[i])
            : fallback_sample_index;
        const uint64_t gop_index = sample_index >= 0
            ? static_cast<uint64_t>(sample_index) / recording_gop_length_
            : completion_gop_index;

        auto [it, inserted] = pending_gops_.try_emplace(gop_index);
        PendingGop& pending = it->second;
        if (inserted) {
            pending.gop_index = gop_index;
            pending.created_at = std::chrono::steady_clock::now();
            update_pending_gop_peaks_locked();
        }

        const size_t packet_size = packets[i].size();
        if (split_gop_config_.max_buffered_bytes > 0 &&
            pending_gop_buffered_bytes_ + packet_size > split_gop_config_.max_buffered_bytes) {
            record_pending_gop_overflow_locked(
                "bytes",
                completion_gop_index,
                split_gop_config_.max_buffered_bytes);
            throw std::runtime_error("split_gop pending GOP bytes exceeded configured limit");
        }

        BufferedEncodedPacket buffered_packet;
        buffered_packet.bytes = packets[i];
        buffered_packet.sample_index = sample_index;
        pending.total_bytes += packet_size;
        pending_gop_buffered_bytes_ += packet_size;
        pending.packets.push_back(std::move(buffered_packet));

        update_pending_gop_peaks_locked();
        pending_gop_peak_bytes_ = std::max(pending_gop_peak_bytes_, pending_gop_buffered_bytes_);
    }

    if (metadata_row.has_value()) {
        const uint64_t metadata_gop_index =
            metadata_row->frame_id > 0 ? (metadata_row->frame_id - 1) / recording_gop_length_
                                       : completion_gop_index;
        append_metadata_row(metadata_gop_index, *metadata_row);
    }

    if (mark_complete) {
        auto [it, inserted] = pending_gops_.try_emplace(completion_gop_index);
        PendingGop& pending = it->second;
        if (inserted) {
            pending.gop_index = completion_gop_index;
            pending.created_at = std::chrono::steady_clock::now();
            update_pending_gop_peaks_locked();
        }
        pending.complete = true;
    }

    flush_pending_gops_locked(false);
    if (split_gop_config_.max_inflight_gops > 0 &&
        pending_gop_backlog_count_locked() > split_gop_config_.max_inflight_gops) {
        record_pending_gop_overflow_locked(
            "backlog",
            completion_gop_index,
            split_gop_config_.max_inflight_gops);
        throw std::runtime_error("split_gop pending GOP backlog exceeded configured limit");
    }
}

void SharedRecordingOutput::flush_pending_gops_locked(bool flush_all)
{
    if (!split_gop_config_.enabled) {
        return;
    }

    while (true) {
        auto it = pending_gops_.find(next_gop_to_flush_);
        if (it == pending_gops_.end()) {
            break;
        }
        if (!flush_all && !it->second.complete) {
            break;
        }

        PendingGop pending = std::move(it->second);
        pending_gops_.erase(it);
        pending_gop_buffered_bytes_ -= pending.total_bytes;

        std::sort(
            pending.metadata_rows.begin(),
            pending.metadata_rows.end(),
            [](const RecordingMetadataRow& lhs, const RecordingMetadataRow& rhs) {
                return lhs.frame_id < rhs.frame_id;
            });
        for (const auto& row : pending.metadata_rows) {
            write_metadata_row_locked(row);
        }
        for (const auto& packet : pending.packets) {
            if (writer_.video) {
                writer_.video->push_packet(
                    const_cast<uint8_t*>(packet.bytes.data()),
                    static_cast<int>(packet.bytes.size()),
                    packet.sample_index);
            }
        }
        next_gop_to_flush_++;
    }
    refresh_writer_queue_metrics_locked();
}

void SharedRecordingOutput::refresh_writer_queue_metrics_locked()
{
    if (!writer_.video) {
        return;
    }
    writer_queue_overflowed_ =
        writer_queue_overflowed_ || writer_.video->has_queue_overflowed();
    writer_queue_overflow_events_ = std::max<uint64_t>(
        writer_queue_overflow_events_,
        writer_.video->queue_overflow_events());
    writer_queue_peak_packets_ = std::max<size_t>(
        writer_queue_peak_packets_,
        writer_.video->peak_queued_packets());
    writer_queue_peak_bytes_ = std::max<size_t>(
        writer_queue_peak_bytes_,
        writer_.video->peak_queued_bytes());
}

void SharedRecordingOutput::write_metadata_row_locked(const RecordingMetadataRow& metadata_row)
{
    if (writer_.metadata && writer_.metadata->is_open()) {
        *writer_.metadata << metadata_row.frame_id << ","
                          << metadata_row.timestamp << ","
                          << metadata_row.timestamp_sys << '\n';
    }
}

void SharedRecordingOutput::record_pending_gop_overflow_locked(const char* reason,
                                                               uint64_t completion_gop_index,
                                                               size_t limit)
{
    pending_gop_overflowed_ = true;
    pending_gop_overflow_events_++;
    pending_gop_overflow_reason_ = reason ? reason : "";
    pending_gop_overflow_completion_gop_index_ = completion_gop_index;
    pending_gop_overflow_next_gop_to_flush_ = next_gop_to_flush_;
    pending_gop_overflow_limit_ = limit;
    pending_gop_overflow_pending_count_ = pending_gops_.size();
    pending_gop_overflow_backlog_count_ = pending_gop_backlog_count_locked();

    const auto frontier_it = pending_gops_.find(next_gop_to_flush_);
    pending_gop_overflow_frontier_present_ = frontier_it != pending_gops_.end();
    pending_gop_overflow_frontier_complete_ =
        frontier_it != pending_gops_.end() && frontier_it->second.complete;

    pending_gop_overflow_pending_keys_.clear();
    pending_gop_overflow_pending_keys_.reserve(pending_gops_.size());
    for (const auto& entry : pending_gops_) {
        pending_gop_overflow_pending_keys_.push_back(entry.first);
    }

    if (pending_gop_overflow_events_ <= 4 || (pending_gop_overflow_events_ % 60) == 0) {
        std::ostringstream keys_stream;
        keys_stream << "[";
        for (size_t i = 0; i < pending_gop_overflow_pending_keys_.size(); ++i) {
            if (i > 0) {
                keys_stream << ",";
            }
            keys_stream << pending_gop_overflow_pending_keys_[i];
        }
        keys_stream << "]";

        std::cerr << "[SharedRecordingOutput] split_gop overflow"
                  << " reason=" << pending_gop_overflow_reason_
                  << " completion_gop=" << pending_gop_overflow_completion_gop_index_
                  << " next_gop_to_flush=" << pending_gop_overflow_next_gop_to_flush_
                  << " backlog=" << pending_gop_overflow_backlog_count_
                  << " pending=" << pending_gop_overflow_pending_count_
                  << " limit=" << pending_gop_overflow_limit_
                  << " frontier_present=" << (pending_gop_overflow_frontier_present_ ? "yes" : "no")
                  << " frontier_complete="
                  << (pending_gop_overflow_frontier_complete_ ? "yes" : "no")
                  << " keys=" << keys_stream.str()
                  << std::endl;
    }
}

void SharedRecordingOutput::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

bool SharedRecordingOutput::close_worker_session(bool request_close_when_idle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return false;
    }
    if (request_close_when_idle) {
        close_requested_ = true;
    }
    if (active_worker_sessions_ > 0) {
        active_worker_sessions_--;
    }
    if (close_requested_ && active_worker_sessions_ == 0) {
        close_locked();
        return true;
    }
    return false;
}

void SharedRecordingOutput::close_locked()
{
    if (!is_open_) {
        return;
    }
    flush_pending_gops_locked(true);
    refresh_writer_queue_metrics_locked();
    close_writer_locked(&writer_);
    active_folder_.clear();
    is_open_ = false;
    active_worker_sessions_ = 0;
    close_requested_ = false;
}

SharedRecordingOutputStats SharedRecordingOutput::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    SharedRecordingOutputStats out;
    out.is_open = is_open_;
    out.writer_queue_overflowed = writer_queue_overflowed_;
    out.writer_queue_overflow_events = writer_queue_overflow_events_;
    out.writer_queue_peak_packets = writer_queue_peak_packets_;
    out.writer_queue_peak_bytes = writer_queue_peak_bytes_;
    out.next_gop_to_flush = next_gop_to_flush_;
    out.pending_gop_count = pending_gops_.size();
    out.pending_gop_backlog_count = pending_gop_backlog_count_locked();
    out.pending_gop_bytes = pending_gop_buffered_bytes_;
    out.pending_gop_peak_count = pending_gop_peak_count_;
    out.pending_gop_peak_backlog_count = pending_gop_peak_backlog_count_;
    out.pending_gop_peak_bytes = pending_gop_peak_bytes_;
    out.pending_gop_overflowed = pending_gop_overflowed_;
    out.pending_gop_overflow_events = pending_gop_overflow_events_;
    out.oldest_pending_gop_age_ms = oldest_pending_gop_age_ms_locked();
    out.pending_gop_overflow_reason = pending_gop_overflow_reason_;
    out.pending_gop_overflow_completion_gop_index = pending_gop_overflow_completion_gop_index_;
    out.pending_gop_overflow_next_gop_to_flush = pending_gop_overflow_next_gop_to_flush_;
    out.pending_gop_overflow_limit = pending_gop_overflow_limit_;
    out.pending_gop_overflow_pending_count = pending_gop_overflow_pending_count_;
    out.pending_gop_overflow_backlog_count = pending_gop_overflow_backlog_count_;
    out.pending_gop_overflow_frontier_present = pending_gop_overflow_frontier_present_;
    out.pending_gop_overflow_frontier_complete = pending_gop_overflow_frontier_complete_;
    out.pending_gop_overflow_pending_keys = pending_gop_overflow_pending_keys_;
    return out;
}

bool SharedRecordingOutput::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

std::string SharedRecordingOutput::active_folder() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_folder_;
}

size_t SharedRecordingOutput::active_worker_sessions() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_worker_sessions_;
}

size_t SharedRecordingOutput::pending_gop_backlog_count_locked() const
{
    size_t backlog = pending_gops_.size();
    if (backlog == 0) {
        return 0;
    }
    if (pending_gops_.find(next_gop_to_flush_) != pending_gops_.end()) {
        backlog--;
    }
    return backlog;
}

void SharedRecordingOutput::update_pending_gop_peaks_locked()
{
    pending_gop_peak_count_ = std::max(pending_gop_peak_count_, pending_gops_.size());
    pending_gop_peak_backlog_count_ = std::max(
        pending_gop_peak_backlog_count_,
        pending_gop_backlog_count_locked());
}

int64_t SharedRecordingOutput::oldest_pending_gop_age_ms_locked() const
{
    if (pending_gops_.empty()) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto oldest = std::min_element(
        pending_gops_.begin(),
        pending_gops_.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second.created_at < rhs.second.created_at;
        });
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest->second.created_at).count();
}

void SharedRecordingOutput::reset_pending_state_locked()
{
    pending_gops_.clear();
    next_gop_to_flush_ = 0;
    pending_gop_buffered_bytes_ = 0;
    pending_gop_peak_count_ = 0;
    pending_gop_peak_backlog_count_ = 0;
    pending_gop_peak_bytes_ = 0;
    pending_gop_overflowed_ = false;
    pending_gop_overflow_events_ = 0;
    pending_gop_overflow_reason_.clear();
    pending_gop_overflow_completion_gop_index_ = 0;
    pending_gop_overflow_next_gop_to_flush_ = 0;
    pending_gop_overflow_limit_ = 0;
    pending_gop_overflow_pending_count_ = 0;
    pending_gop_overflow_backlog_count_ = 0;
    pending_gop_overflow_frontier_present_ = false;
    pending_gop_overflow_frontier_complete_ = false;
    pending_gop_overflow_pending_keys_.clear();
}
