// tools/camera_stream_event_log_tests.cpp
//
// The per-stream camera loss log (src/camera_stream_event_log.h): file name
// and place, header-only file for a clean stream, one row per GetFrame error
// and per frame-id gap, written through the real process log writer.

#include "camera_stream_event_log.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

std::vector<std::string> lines_of(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

int main()
{
    namespace fs = std::filesystem;
    using orange::camera_stream::StreamEventLog;
    const fs::path root = fs::temp_directory_path() / ("orange_stream_event_log_tests_" + std::to_string(::getpid()));
    const fs::path dir = StreamEventLog::DefaultDirectory(root.string());
    expect(dir == root / "diagnostics" / "camera_stream", "directory under <root>/diagnostics/camera_stream");
    expect(StreamEventLog::DefaultDirectory("").empty(), "no root, no directory");

    std::string clean_path;
    {
        StreamEventLog log(dir, "2010095");
        clean_path = log.path();
    }
    const fs::path clean(clean_path);
    expect(clean.parent_path() == dir, "file in the stream directory: " + clean_path);
    const std::string name = clean.filename().string();
    expect(name.rfind("stream_events_", 0) == 0 && name.find("_Cam2010095.csv") != std::string::npos,
           "file name stream_events_<utc>_<pid>_Cam<serial>.csv: " + name);
    auto lines = lines_of(clean);
    expect(lines.size() == 1 && lines[0] + "\n" == StreamEventLog::kHeader, "a clean stream leaves only the header");

    std::string path;
    {
        StreamEventLog log(dir, "2010096");
        path = log.path();
        StreamEventLog::Row error;
        error.event = StreamEventLog::Event::kGetFrameError;
        error.host_realtime_ns = 1794349654719508301ULL;
        error.stream_frames_received = 669400;
        error.previous_camera_frame_id = 26782;
        error.get_frame_return_code = 11;
        error.get_frame_wait_ns = 212000;
        error.get_frame_timeout_ms = 1000;
        error.get_frame_errors_total = 2;
        error.sdk_dropped_frames = 0;
        error.record_active = false;
        log.Emit(error);
        StreamEventLog::Row gap;
        gap.event = StreamEventLog::Event::kCameraFrameGap;
        gap.host_realtime_ns = 1794351526075077685ULL;
        gap.stream_frames_received = 1137384;
        gap.previous_camera_frame_id = 23289;
        gap.camera_frame_id = 23291;
        gap.missing_camera_frames = 1;
        gap.get_frame_errors_total = 2;
        gap.camera_dropped_frames_total = 1;
        gap.sdk_dropped_frames = 1;
        gap.record_active = true;
        log.Emit(gap);
    }
    lines = lines_of(path);
    expect(lines.size() == 3, "header plus two rows, got " + std::to_string(lines.size()));
    if (lines.size() == 3) {
        expect(lines[1] == "get_frame_error,1794349654719508301,669400,26782,0,11,212000,1000,0,2,0,0,0",
               "GetFrame error row: " + lines[1]);
        expect(lines[2] == "camera_frame_gap,1794351526075077685,1137384,23289,23291,0,0,0,1,2,1,1,1",
               "gap row: " + lines[2]);
    }

    {
        StreamEventLog disabled(fs::path(), "2010093");
        expect(disabled.path().empty(), "an empty directory disables the log");
        disabled.Emit(StreamEventLog::Row{});
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::cerr << "camera_stream_event_log_tests: " << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "camera_stream_event_log_tests: PASS" << std::endl;
    return 0;
}
