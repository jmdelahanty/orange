#pragma once

// Stream-level camera loss log (2026-11-10): one CSV per camera per stream
// under <orange_data>/diagnostics/camera_stream/, open from acquisition start
// to stream stop whether or not a recording runs. One row per failed
// EVT_CameraGetFrame and per camera frame-id gap. A file with only the header
// means the stream had neither; outside a recording nothing else keeps them
// except the terminal. Rows go through the process log writer
// (timing/log_writer.h), so the acquisition thread does no file I/O.

#include "timing/clock.h"
#include "timing/log_writer.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

namespace orange::camera_stream {

class StreamEventLog {
public:
    enum class Event : uint8_t { kGetFrameError, kCameraFrameGap };
    struct Row {
        Event event = Event::kGetFrameError;
        uint64_t host_realtime_ns = 0;
        uint64_t stream_frames_received = 0;
        uint32_t previous_camera_frame_id = 0;
        uint32_t camera_frame_id = 0;
        int get_frame_return_code = 0;
        uint64_t get_frame_wait_ns = 0;
        uint32_t get_frame_timeout_ms = 0;
        uint64_t missing_camera_frames = 0;
        uint64_t get_frame_errors_total = 0;
        uint64_t camera_dropped_frames_total = 0;
        uint64_t sdk_dropped_frames = 0;
        bool record_active = false;
    };

    static constexpr const char* kHeader =
        "event,host_realtime_ns,stream_frames_received,previous_camera_frame_id,camera_frame_id,"
        "get_frame_return_code,get_frame_wait_ns,get_frame_timeout_ms,missing_camera_frames,"
        "get_frame_errors_total,camera_dropped_frames_total,sdk_dropped_frames,record_active\n";

    static std::filesystem::path DefaultDirectory(const std::string& orange_root)
    {
        if (orange_root.empty()) {
            return {};
        }
        return std::filesystem::path(orange_root) / "diagnostics" / "camera_stream";
    }

    // Not a hot path: once at acquisition start. An empty directory disables it.
    StreamEventLog(const std::filesystem::path& directory, const std::string& camera_serial)
    {
        if (directory.empty()) {
            return;
        }
        orange::timing::LogChannelOptions options;
        options.name = "camera_stream_events";
        options.capacity = 1024;
        channel_ = orange::timing::LogChannel<Row>::Create(options, &FormatRow);
        path_ = (directory / ("stream_events_" + CompactUtcNow() + "_" + std::to_string(getpid()) + "_Cam" +
                              camera_serial + ".csv"))
                    .string();
        channel_->Open(path_, kHeader);
    }
    ~StreamEventLog()
    {
        if (channel_) {
            channel_->Close();
            channel_->Retire();
        }
    }
    StreamEventLog(const StreamEventLog&) = delete;
    StreamEventLog& operator=(const StreamEventLog&) = delete;

    // Hot thread: copies the row into the channel's ring; no I/O, no allocation.
    void Emit(const Row& row)
    {
        if (channel_) {
            channel_->Emit(row);
        }
    }
    const std::string& path() const { return path_; }

    static void FormatRow(const Row& r, std::string& out)
    {
        char buf[320];
        const int n = std::snprintf(
            buf, sizeof(buf), "%s,%llu,%llu,%u,%u,%d,%llu,%u,%llu,%llu,%llu,%llu,%d\n",
            r.event == Event::kGetFrameError ? "get_frame_error" : "camera_frame_gap",
            static_cast<unsigned long long>(r.host_realtime_ns),
            static_cast<unsigned long long>(r.stream_frames_received), r.previous_camera_frame_id,
            r.camera_frame_id, r.get_frame_return_code, static_cast<unsigned long long>(r.get_frame_wait_ns),
            r.get_frame_timeout_ms, static_cast<unsigned long long>(r.missing_camera_frames),
            static_cast<unsigned long long>(r.get_frame_errors_total),
            static_cast<unsigned long long>(r.camera_dropped_frames_total),
            static_cast<unsigned long long>(r.sdk_dropped_frames), r.record_active ? 1 : 0);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
        }
    }

private:
    static std::string CompactUtcNow()
    {
        const uint64_t ns = orange::timing::realtime_now().value;
        const std::time_t seconds = static_cast<std::time_t>(ns / 1000000000ULL);
        std::tm utc{};
        gmtime_r(&seconds, &utc);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
        return buf;
    }

    std::string path_;
    std::shared_ptr<orange::timing::LogChannel<Row>> channel_;
};

}  // namespace orange::camera_stream
