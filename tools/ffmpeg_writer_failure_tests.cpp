// Tests for the FFmpegWriter construction failure boundary.
//
// docs/error_handling_convention.md: construction-time failures throw, so a
// recording can never silently "record to nowhere". These tests exercise the
// open/close container paths only; no real encoding is required.

#include "FFmpegWriter.h"
#include "json.hpp"
#include "video_container_finalization.h"

#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

class FFmpegWriterTestAccess {
public:
    static bool record_packet_write_result(
        FFmpegWriter& writer, int result, size_t packet_bytes)
    {
        return writer.record_packet_write_result(result, packet_bytes);
    }
};

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

nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream input(path);
    expect(input.is_open(), "could not open JSON sidecar: " + path.string());
    nlohmann::json value;
    input >> value;
    expect(input.good() || input.eof(),
           "could not parse JSON sidecar: " + path.string());
    return value;
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
    const std::filesystem::path finalization_path =
        OrangeVideoContainerFinalization::SidecarPathFor(out_path);

    {
        const std::vector<std::pair<std::string, std::string>> metadata_tags = {
            {"title", "Orange 700 fps container test"},
            {"comment", "high-frame-rate scientific acquisition"},
        };
        FFmpegWriter writer(AV_CODEC_ID_H264, 640, 480, 700,
                            out_path.c_str(), nullptr, metadata_tags);
        expect(writer.is_open(), "a constructed writer must be open");
        expect(!writer.writer_thread_failed(),
               "a fresh writer must not report a writer-thread failure");
        const nlohmann::json open_status = read_json(finalization_path);
        expect(open_status.at("status") == "recording_open" &&
                   !open_status.at("terminal").get<bool>(),
               "an open writer must have a nonterminal lifecycle sidecar");
        writer.create_thread();
        writer.quit_thread();
        writer.join_thread();
    } // destructor writes the trailer

    expect(std::filesystem::exists(out_path), "output container must exist");
    expect(std::filesystem::file_size(out_path) > 0,
           "output container must be finalized (non-empty)");

    const nlohmann::json finalization = read_json(finalization_path);
    expect(finalization.at("schema_version") == 2,
           "new Orange finalization evidence must use schema version 2");
    expect(finalization.at("schema_id") ==
               "orange.video_container_finalization",
           "finalization sidecar has the wrong schema identity");
    expect(finalization.at("status") == "complete" &&
               finalization.at("terminal").get<bool>(),
           "finalization sidecar must reach terminal-complete");
    expect(finalization.at("recording_fps") == 700,
           "finalization sidecar must preserve the 700 fps rate");
    expect(finalization.at("container").at("finalized").get<bool>(),
           "finalization sidecar must prove trailer and close success");
    expect(finalization.at("packet_writes").at("complete").get<bool>() &&
               finalization.at("packet_writes")
                   .at("muxer_flush_succeeded")
                   .get<bool>() &&
               finalization.at("packet_writes").at("packets_written") == 0 &&
               finalization.at("packet_writes").at("write_failures") == 0,
           "empty-container finalization must carry complete zero-packet write proof");
    expect(finalization.at("quicktime_full_frame_rate_playback_intent")
               .at("patch_applied")
               .get<bool>(),
           "finalization sidecar must prove the typed playback-intent patch");

    AVFormatContext* input = nullptr;
    expect(avformat_open_input(&input, out_path.c_str(), nullptr, nullptr) >= 0,
           "FFmpeg could not reopen the finalized MP4");
    const AVDictionaryEntry* intent = av_dict_get(
        input->metadata,
        OrangeVideoContainerFinalization::kFullFrameRatePlaybackIntentKey,
        nullptr, 0);
    expect(intent != nullptr && std::string(intent->value) == "1",
           "demuxer did not recover full-frame-rate playback intent");
    const AVDictionaryEntry* title =
        av_dict_get(input->metadata, "title", nullptr, 0);
    const AVDictionaryEntry* comment =
        av_dict_get(input->metadata, "comment", nullptr, 0);
    expect(title != nullptr &&
               std::string(title->value) == "Orange 700 fps container test",
           "existing title metadata was not preserved through mdta encoding");
    expect(comment != nullptr &&
               std::string(comment->value) ==
                   "high-frame-rate scientific acquisition",
           "existing comment metadata was not preserved through mdta encoding");
    avformat_close_input(&input);

    std::filesystem::remove_all(dir);
}

void test_finalization_status_classification()
{
    using OrangeVideoContainerFinalization::ClassifyTerminalStatus;
    using OrangeVideoContainerFinalization::Outcome;
    using OrangeVideoContainerFinalization::Status;

    Outcome complete;
    complete.header_written = true;
    complete.muxer_flush_attempted = true;
    complete.muxer_flush_succeeded = true;
    complete.trailer_written = true;
    complete.output_closed = true;
    complete.playback_intent_patch_applied = true;
    expect(ClassifyTerminalStatus(complete) == Status::Complete,
           "successful finalization must classify complete");

    Outcome degraded = complete;
    degraded.playback_intent_patch_applied = false;
    expect(ClassifyTerminalStatus(degraded) ==
               Status::DegradedPlaybackIntentUnpatched,
           "patch-only failure must classify as degraded");

    Outcome failed = complete;
    failed.trailer_written = false;
    expect(ClassifyTerminalStatus(failed) ==
               Status::ContainerFinalizationFailed,
           "trailer failure must classify as container failure");

    Outcome mux_failed = complete;
    mux_failed.writer_error_latched = true;
    mux_failed.packet_submissions_accepted = 1;
    mux_failed.packet_write_attempts = 1;
    mux_failed.packet_write_failures = 1;
    mux_failed.first_packet_write_error_code = AVERROR(EIO);
    expect(ClassifyTerminalStatus(mux_failed) ==
               Status::ContainerFinalizationFailed,
           "a mux packet-write failure must prevent complete finalization");
}

void test_mux_write_failure_is_latched_and_persisted()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "mux_failure.mp4").string();
    const std::filesystem::path finalization_path =
        OrangeVideoContainerFinalization::SidecarPathFor(out_path);

    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 640, 480, 30, out_path.c_str(), nullptr);
        const int injected_error = AVERROR(EIO);
        expect(
            !FFmpegWriterTestAccess::record_packet_write_result(
                writer, injected_error, 4096),
            "a negative mux result must be reported as failure");
        const FFmpegWriterPacketWriteStats stats = writer.packet_write_stats();
        expect(writer.writer_thread_failed(),
               "a mux failure must latch the writer failure boundary");
        expect(stats.write_attempts == 1 && stats.packets_written == 0 &&
                   stats.write_failures == 1 &&
                   stats.first_write_error_code == injected_error,
               "mux failure counters must preserve the exact failed attempt");
        writer.create_thread();
        writer.quit_thread();
        writer.join_thread();
    }

    const nlohmann::json finalization = read_json(finalization_path);
    const nlohmann::json packet_writes = finalization.at("packet_writes");
    expect(finalization.at("status") == "container_finalization_failed",
           "a mux write failure must prevent terminal-complete evidence");
    expect(packet_writes.at("complete") == false &&
               packet_writes.at("writer_error_latched") == true &&
               packet_writes.at("write_attempts") == 1 &&
               packet_writes.at("packets_written") == 0 &&
               packet_writes.at("write_failures") == 1 &&
               packet_writes.at("first_write_error_code") == AVERROR(EIO),
           "finalization evidence must retain mux write failure details");

    std::filesystem::remove_all(dir);
}

void test_queue_byte_limit_fails_closed_before_enqueuing()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "bounded.mp4").string();
    FFmpegWriterQueueConfig queue_config;
    queue_config.max_queued_packets = 1;
    queue_config.max_queued_bytes = 3;

    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264,
            640,
            480,
            30,
            out_path.c_str(),
            nullptr,
            {},
            queue_config);
        const uint8_t oversized_packet[] = {0, 0, 1, 0x65};
        expect(!writer.push_packet(
            const_cast<uint8_t*>(oversized_packet),
            static_cast<int>(sizeof(oversized_packet)),
            0),
            "an over-limit packet submission must report rejection");
        expect(writer.has_queue_overflowed(),
               "an encoded packet above the byte ceiling must latch overflow");
        expect(writer.queue_overflow_events() == 1,
               "the rejected packet must produce one overflow event");
        const FFmpegWriterPacketWriteStats packet_stats =
            writer.packet_write_stats();
        expect(packet_stats.submissions_accepted == 0 &&
                   packet_stats.submissions_rejected == 1,
               "submission accounting must distinguish rejected packets");
        expect(writer.queued_packets() == 0 && writer.queued_bytes() == 0,
               "an over-limit packet must not enter the writer queue");
        expect(writer.peak_queued_packets() == 0 &&
                   writer.peak_queued_bytes() == 0,
               "a rejected packet must not inflate queue peaks");
        expect(writer.queue_config().max_queued_packets == 1 &&
                   writer.queue_config().max_queued_bytes == 3,
               "the writer must preserve its explicit hard queue contract");
        writer.create_thread();
        writer.quit_thread();
        writer.join_thread();
    }

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
        {"finalization_status_classification",
         &test_finalization_status_classification},
        {"mux_write_failure_is_latched_and_persisted",
         &test_mux_write_failure_is_latched_and_persisted},
        {"queue_byte_limit_fails_closed_before_enqueuing",
         &test_queue_byte_limit_fails_closed_before_enqueuing},
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
