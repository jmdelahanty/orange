#include "yolo_event_log.h"
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
std::size_t rows(const std::string& path) {
    std::ifstream input(path); std::string line; std::size_t n = 0;
    while (std::getline(input, line)) { check(nlohmann::json::parse(line).is_object(), "invalid log row"); ++n; }
    return n;
}
}
int main() {
    try {
        Fixture f;
        yolo_event_log::YoloEventLogger logger("02010093", 0, "drain-test");
        const auto timeout = std::chrono::seconds(2);
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
