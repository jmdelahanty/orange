#include "yolo_event_log.h"
#include "timing/log_writer.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

// Filesystem seam normally provided by the application-wide project.cpp.
bool make_folder(std::string path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::string root = (std::filesystem::temp_directory_path() / "yolo_drain_XXXXXX").string();
    Fixture() { check(mkdtemp(root.data()), "temporary directory"); }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
yolo_event_log::YoloResultRecord record(const std::string& folder, uint64_t id) {
    yolo_event_log::YoloResultRecord row;
    row.recording_folder = folder; row.recording_frame_id = id;
    row.record_active = true; row.status = "zero_detections";
    return row;
}
// Frame rows only: v2 logs lead with a session_header line.
std::size_t rows(const std::string& path) {
    std::ifstream input(path); std::string line; std::size_t n = 0;
    while (std::getline(input, line)) {
        const auto row = nlohmann::json::parse(line);
        check(row.is_object(), "invalid log row");
        if (row.value("event_kind", "") == "yolo_result") ++n;
    }
    return n;
}
}
int main() {
    try {
        Fixture f;
        yolo_event_log::YoloEventLogger logger("02010093", 0, "drain-test");
        const auto timeout = std::chrono::seconds(2);
        // FIFO barrier through the process log writer (2026-10-11: finalize
        // digested a buffered prefix of this file): after FlushAll every row
        // enqueued before it is on disk, with no FlushThrough and no close.
        {
            const auto plain = f.root + "/plain";
            yolo_event_log::YoloEventLogger flushed("02010094", 1, "flush-test");
            for (uint64_t id = 1; id <= 50; ++id) flushed.Enqueue(record(plain, id));
            check(orange::timing::LogWriter::Instance().FlushAll(timeout), "LogWriter FlushAll covered the event log");
            check(rows(plain + "/Cam02010094_yolo_events.jsonl") == 50, "FlushAll left rows in the buffer");
            const auto size = std::filesystem::file_size(plain + "/Cam02010094_yolo_events.jsonl");
            flushed.Stop();
            check(std::filesystem::file_size(plain + "/Cam02010094_yolo_events.jsonl") == size,
                  "bytes appeared after the flush barrier");
        }
        logger.Enqueue(record(f.root, 1));
        check(logger.FlushThrough(f.root, 1, timeout), "first-frame flush");
        check(rows(f.root + "/Cam02010093_yolo_events.jsonl") == 1, "buffer not visible");
        auto late = std::async(std::launch::async, [&] { return logger.FlushThrough(f.root, 2, timeout); });
        check(late.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout, "flush guessed an unwritten tail");
        logger.Enqueue(record(f.root, 2));
        check(late.get(), "late detector tail did not satisfy fence");
        check(rows(f.root + "/Cam02010093_yolo_events.jsonl") == 2, "late tail not visible");
        check(!logger.FlushThrough(f.root, 3, std::chrono::milliseconds(30)), "missing tail accepted");
        const auto next = f.root + "/next";
        logger.Enqueue(record(next, 1));
        check(logger.FlushThrough(next, 1, timeout), "next-session ID reset");
        check(!logger.FlushThrough(next, 2, std::chrono::milliseconds(30)), "previous session leaked across fence");
        check(logger.FlushThrough(f.root, 2, timeout), "closed session not flushed");
        auto stopped = std::async(std::launch::async, [&] { return logger.FlushThrough(next, 2, timeout); });
        logger.Stop();
        check(!stopped.get(), "stop fabricated missing tail");
        check(logger.FlushThrough(next, 1, timeout), "stopped writer lost flushed evidence");
        check(!logger.FlushThrough(next, 2, timeout), "stopped writer fabricated tail");
        std::cout << "YOLO finalization drain tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
