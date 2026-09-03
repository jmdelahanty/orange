// Focused host coverage for FFmpegWriter's fail-closed packet boundary.
// This test deliberately sends a packet with an invalid stream index to make
// av_interleaved_write_frame return a negative error without requiring a GPU.

#include "FFmpegWriter.h"
#include "json.hpp"
#include "video_container_finalization.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

class FFmpegWriterTestAccess {
public:
    static bool write_one_pkt(FFmpegWriter& writer, AVPacket* packet)
    {
        return writer.write_one_pkt(packet);
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
        (std::filesystem::temp_directory_path() /
         "ffmpeg_writer_error_state_tests_XXXXXX").string();
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
    return value;
}

void test_null_packet_data_latches_enqueue_failure()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "null_data.mp4").string();
    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 640, 480, 30, out_path.c_str(), nullptr);
        writer.push_packet(nullptr, 1, 0);
        expect(!writer.finalize(),
               "enqueue failure must fail explicit finalization");
        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(failures.failed, "null packet data must latch writer failure");
        expect(failures.packet_enqueue_failures == 1,
               "null packet data must count as one enqueue failure");
        expect(failures.total_failures == 1,
               "null packet data must count exactly one failure");
        expect(failures.last_error_code == AVERROR(EINVAL),
               "null packet data must preserve EINVAL");
    }
    const nlohmann::json finalization = read_json(
        OrangeVideoContainerFinalization::SidecarPathFor(out_path));
    expect(finalization.at("status") == "container_finalization_failed",
           "enqueue failure must make finalization terminal-failed");
    std::filesystem::remove_all(dir);
}

void test_negative_packet_write_latches_and_fails_finalization()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "bad_stream.mp4").string();
    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 640, 480, 30, out_path.c_str(), nullptr);
        AVPacket* packet = av_packet_alloc();
        expect(packet != nullptr, "test packet allocation failed");
        expect(av_new_packet(packet, 1) >= 0, "test packet payload allocation failed");
        packet->stream_index = 99;
        FFmpegWriterTestAccess::write_one_pkt(writer, packet);
        av_packet_free(&packet);

        expect(!writer.finalize(),
               "packet-write failure must fail explicit finalization");
        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(failures.failed, "negative packet write must latch writer failure");
        expect(failures.packet_write_failures == 1,
               "negative packet write must count exactly once");
        expect(failures.total_failures == 1,
               "negative packet write must count exactly one failure");
        expect(failures.last_error_code < 0,
               "negative packet write must preserve the FFmpeg error code");
    }
    const nlohmann::json finalization = read_json(
        OrangeVideoContainerFinalization::SidecarPathFor(out_path));
    expect(finalization.at("status") == "container_finalization_failed",
           "packet write failure must make finalization terminal-failed");
    expect(!finalization.at("container").at("finalized").get<bool>(),
           "packet write failure must not claim a finalized container");
    std::filesystem::remove_all(dir);
}

void test_requested_keyframe_sidecar_failure_is_terminal()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "sidecar_failure.mp4").string();
    const std::string sidecar_path =
        (dir / "missing_parent" / "keyframes.csv").string();
    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264,
            640,
            480,
            30,
            out_path.c_str(),
            sidecar_path.c_str());
        writer.create_thread();
        expect(!writer.finalize(),
               "requested sidecar failure must fail explicit finalization");
        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(failures.failed && failures.sidecar_write_failures >= 1,
               "requested sidecar failure must be visible before destruction");
    }
    const nlohmann::json finalization = read_json(
        OrangeVideoContainerFinalization::SidecarPathFor(out_path));
    expect(finalization.at("status") == "container_finalization_failed",
           "requested keyframe-sidecar failure must make finalization terminal-failed");
    std::filesystem::remove_all(dir);
}

void test_rescale_rounding_overflow_is_rejected_before_duration_math()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string out_path = (dir / "pts_overflow.mp4").string();
    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 640, 480, 229, out_path.c_str(), nullptr);
        uint8_t payload = 0;
        // floor((frame + 1) * 90000 / 229) is INT64_MAX, but FFmpeg's
        // round-to-nearest rescale overflows. This exact boundary previously
        // allowed signed overflow while deriving packet duration.
        constexpr int64_t frame_index = 23468357738219373LL;
        expect(!writer.push_packet(&payload, 1, frame_index),
               "rounded PTS overflow must be rejected before enqueue");
        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(failures.failed && failures.packet_enqueue_failures == 1,
               "rounded PTS overflow must latch one enqueue failure");
        expect(failures.last_error_code == AVERROR(EOVERFLOW),
               "rounded PTS overflow must preserve EOVERFLOW");
        expect(!writer.finalize(),
               "rounded PTS overflow must fail terminal finalization");
    }
    std::filesystem::remove_all(dir);
}

}  // namespace

int main()
{
    try {
        test_null_packet_data_latches_enqueue_failure();
        test_negative_packet_write_latches_and_fails_finalization();
        test_requested_keyframe_sidecar_failure_is_terminal();
        test_rescale_rounding_overflow_is_rejected_before_duration_math();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "FFmpeg writer error-state tests passed.\n";
    return EXIT_SUCCESS;
}
