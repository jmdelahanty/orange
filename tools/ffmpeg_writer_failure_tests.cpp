// Tests for the FFmpegWriter construction failure boundary.
//
// docs/error_handling_convention.md: construction-time failures throw, so a
// recording can never silently "record to nowhere". These tests exercise the
// open/close container paths only; no real encoding is required.

#include "FFmpegWriter.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_dir()
{
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "ffmpeg_writer_tests_XXXXXX").string();
    char* created = mkdtemp(tmpl.data());
    expect(created != nullptr, "mkdtemp failed");
    return std::filesystem::path(created);
}

void test_open_failure_throws()
{
    const std::string bad_path =
        (std::filesystem::temp_directory_path() /
         "ffmpeg_writer_tests_no_such_dir" / "out.mp4").string();
    expect(!std::filesystem::exists(std::filesystem::path(bad_path).parent_path()),
           "test precondition: parent directory must not exist");

    bool threw = false;
    try {
        FFmpegWriter writer(AV_CODEC_ID_H264, 640, 480, 30, bad_path.c_str(), nullptr);
    } catch (const std::runtime_error& e) {
        threw = true;
        expect(std::string(e.what()).find(bad_path) != std::string::npos,
               "exception message should carry the output path");
    }
    expect(threw, "constructor must throw when the output file cannot be opened");
}

void test_successful_open_and_finalize()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "out.mp4").string();

    {
        FFmpegWriter writer(AV_CODEC_ID_H264, 640, 480, 30, out_path.c_str(), nullptr);
        expect(writer.is_open(), "a constructed writer must be open");
        expect(!writer.writer_thread_failed(),
               "a fresh writer must not report a writer-thread failure");
        writer.create_thread();
        writer.quit_thread();
        writer.join_thread();
    } // destructor writes the trailer

    expect(std::filesystem::exists(out_path), "output container must exist");
    expect(std::filesystem::file_size(out_path) > 0,
           "output container must be finalized (non-empty)");

    std::filesystem::remove_all(dir);
}

void test_failed_construction_leaves_no_file_behind_null_path_guard()
{
    // A failed construction must not leave a partially constructed object;
    // unique_ptr stays empty, so there is no writer to ignore.
    const std::string bad_path =
        (std::filesystem::temp_directory_path() /
         "ffmpeg_writer_tests_no_such_dir" / "out2.mp4").string();
    std::unique_ptr<FFmpegWriter> writer;
    try {
        writer = std::make_unique<FFmpegWriter>(
            AV_CODEC_ID_HEVC, 320, 240, 60, bad_path.c_str(), nullptr);
    } catch (const std::runtime_error&) {
    }
    expect(writer == nullptr, "failed construction must not yield an object");
}

} // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"open_failure_throws", &test_open_failure_throws},
        {"successful_open_and_finalize", &test_successful_open_and_finalize},
        {"failed_construction_leaves_no_object",
         &test_failed_construction_leaves_no_file_behind_null_path_guard},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All FFmpeg writer failure-boundary tests passed.\n";
    return EXIT_SUCCESS;
}
