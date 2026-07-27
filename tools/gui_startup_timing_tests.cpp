#include "gui/startup_timing.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(const double actual, const double expected, const double tolerance,
                  const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected));
    }
}

std::vector<orange::gui::GuiStartupTimingCamera> cameras()
{
    return {
        {"2010093", 0, 5, {{"stream_on", true}}},
        {"2010094", 1, 6, {{"stream_on", true}}},
    };
}

void test_completed_camera_open_report(const std::filesystem::path& output_dir)
{
    orange::gui::GuiStartupTimingRecorder recorder;
    const uint64_t base = orange::gui::GuiStartupTimingRecorder::NowNs();
    recorder.Begin(
        "camera_open", output_dir, cameras(), {{"config_directory", "test"}}, base);
    recorder.RecordGlobalInterval(
        "config_discovery_and_camera_selection", base, base + 2'000'000);
    recorder.RecordCameraInterval(
        "2010093", "open_and_configure_camera", base + 2'000'000,
        base + 8'000'000);
    recorder.RecordCameraInterval(
        "2010094", "open_and_configure_camera", base + 8'000'000,
        base + 15'000'000);
    recorder.MarkHandlerComplete(base + 16'000'000);
    recorder.MarkOperationComplete(base + 16'000'000);

    const nlohmann::json snapshot = recorder.Snapshot();
    require(snapshot.value("status", std::string()) == "complete",
            "camera-open operation must complete explicitly");
    require_near(snapshot.value("gui_handler_duration_ms", 0.0), 16.0, 0.001,
                 "camera-open handler duration");
    require(snapshot["slowest_camera_stage"].value("camera_serial", std::string()) ==
                "2010094",
            "slowest camera stage must identify the correct camera");

    recorder.FlushPending();
    const std::filesystem::path artifact = recorder.artifact_path();
    require(std::filesystem::is_regular_file(artifact),
            "completed report must be written atomically");
    std::ifstream input(artifact);
    nlohmann::json persisted;
    input >> persisted;
    require(persisted == snapshot, "persisted camera-open report must equal snapshot");
}

void test_stream_report_completes_on_all_first_frames(
    const std::filesystem::path& output_dir)
{
    orange::gui::GuiStartupTimingRecorder recorder;
    const uint64_t base = orange::gui::GuiStartupTimingRecorder::NowNs();
    recorder.Begin("stream_start", output_dir, cameras(), {{"ptp_stream_sync", true}}, base);
    recorder.RecordCameraInterval(
        "2010093", "stream_open", base + 1'000'000, base + 4'000'000);
    recorder.RecordCameraInterval(
        "2010094", "stream_open", base + 4'000'000, base + 8'000'000);
    recorder.MarkHandlerComplete(base + 20'000'000);

    std::thread first([&]() {
        recorder.MarkFirstFrame(
            "2010093", 1, 101, 1'000'000'000ULL, base + 100'000'000);
    });
    std::thread second([&]() {
        recorder.MarkFirstFrame(
            "2010094", 1, 201, 1'000'000'025ULL, base + 102'000'000);
    });
    first.join();
    second.join();

    const nlohmann::json snapshot = recorder.Snapshot();
    require(snapshot.value("status", std::string()) == "complete",
            "stream report must complete only after all first frames");
    require(snapshot.value("observed_first_frame_camera_count", 0) == 2,
            "both first frames must be counted once");
    require_near(snapshot.value("time_to_all_first_frames_ms", 0.0), 102.0, 0.001,
                 "time to all first frames");
    require_near(snapshot.value("first_frame_spread_ms", 0.0), 2.0, 0.001,
                 "host first-frame spread");
    const orange::gui::GuiStartupTimingStatus control_status = recorder.Status();
    require(control_status.available,
            "control-plane status must be available after Begin");
    require(control_status.operation == "stream_start" &&
                control_status.status == "complete",
            "control-plane status must expose stream completion");
    require(control_status.expected_first_frame_camera_count == 2 &&
                control_status.observed_first_frame_camera_count == 2,
            "control-plane status must expose first-frame readiness counts");

    // A repeated report for the same camera is intentionally ignored.
    recorder.MarkFirstFrame(
        "2010093", 2, 102, 1'010'000'000ULL, base + 120'000'000);
    require(recorder.Snapshot().value("observed_first_frame_camera_count", 0) == 2,
            "first-frame reporting must be idempotent per camera");

    recorder.FlushPending();
    require(std::filesystem::is_regular_file(recorder.artifact_path()),
            "completed stream report must be written");
}

void test_failed_attempt_is_preserved(const std::filesystem::path& output_dir)
{
    orange::gui::GuiStartupTimingRecorder recorder;
    const uint64_t base = orange::gui::GuiStartupTimingRecorder::NowNs();
    recorder.Begin("stream_start", output_dir, cameras(), {}, base);
    recorder.RecordGlobalInterval("recording_preflight", base, base + 500'000);
    recorder.MarkFailed("recording_preflight_failed", base + 600'000);
    recorder.FlushPending();

    const nlohmann::json snapshot = recorder.Snapshot();
    require(snapshot.value("status", std::string()) == "failed",
            "failed attempt must retain a terminal status");
    require(snapshot.value("failure_reason", std::string()) ==
                "recording_preflight_failed",
            "failed attempt must retain its reason");
    require(std::filesystem::is_regular_file(recorder.artifact_path()),
            "failed attempt must still write evidence");
}

void test_first_frames_can_precede_handler_return(
    const std::filesystem::path& output_dir)
{
    orange::gui::GuiStartupTimingRecorder recorder;
    const uint64_t base = orange::gui::GuiStartupTimingRecorder::NowNs();
    recorder.Begin("stream_start", output_dir, cameras(), {}, base);
    recorder.MarkFirstFrame(
        "2010093", 1, 101, 1'000'000'000ULL, base + 10'000'000);
    recorder.MarkFirstFrame(
        "2010094", 1, 201, 1'000'000'025ULL, base + 12'000'000);
    recorder.MarkHandlerComplete(base + 20'000'000);

    const nlohmann::json snapshot = recorder.Snapshot();
    require(snapshot.value("status", std::string()) == "complete",
            "handler completion must not overwrite an already-complete stream");
    require_near(snapshot.value("gui_handler_duration_ms", 0.0), 20.0, 0.001,
                 "late handler completion must still be recorded");
    require_near(snapshot.value("time_to_all_first_frames_ms", 0.0), 12.0, 0.001,
                 "first-frame completion time must remain unchanged");
}

}  // namespace

int main()
{
    const std::filesystem::path output_dir =
        std::filesystem::path("/tmp") /
        ("orange_gui_startup_timing_tests_" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::remove_all(output_dir, ec);
    std::filesystem::create_directories(output_dir);

    try {
        test_completed_camera_open_report(output_dir);
        test_stream_report_completes_on_all_first_frames(output_dir);
        test_failed_attempt_is_preserved(output_dir);
        test_first_frames_can_precede_handler_return(output_dir);
        std::filesystem::remove_all(output_dir, ec);
        std::cout << "gui_startup_timing_tests: PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gui_startup_timing_tests: FAIL: " << error.what() << std::endl;
        return 1;
    }
}
