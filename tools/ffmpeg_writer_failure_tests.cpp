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
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class FFmpegWriterTestAccess {
public:
    static bool record_packet_write_result(
        FFmpegWriter& writer, int result, size_t packet_bytes)
    {
        return writer.record_packet_write_result(result, packet_bytes);
    }
};

namespace {

static_assert(!std::is_copy_constructible_v<
                  FFmpegWriterDescriptorOutputConfig>,
              "descriptor output authority must remain move-only");
static_assert(std::is_move_constructible_v<
                  FFmpegWriterDescriptorOutputConfig>,
              "descriptor output authority must be movable");

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

int open_rw_artifact(const std::filesystem::path& path)
{
    const int fd = ::open(
        path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    expect(fd >= 0, "could not create artifact descriptor: " + path.string());
    return fd;
}

std::string read_fd(int fd)
{
    struct stat descriptor_stat {};
    expect(::fstat(fd, &descriptor_stat) == 0 && descriptor_stat.st_size >= 0,
           "could not stat descriptor");
    std::string bytes(static_cast<size_t>(descriptor_stat.st_size), '\0');
    size_t completed = 0;
    while (completed < bytes.size()) {
        const ssize_t count = ::pread(
            fd,
            bytes.data() + completed,
            bytes.size() - completed,
            static_cast<off_t>(completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        expect(count > 0, "could not read descriptor contents");
        completed += static_cast<size_t>(count);
    }
    return bytes;
}

struct DescriptorInput {
    int fd = -1;
    int64_t offset = 0;
};

int descriptor_input_read(void* opaque, uint8_t* buffer, int buffer_size)
{
    auto* input = static_cast<DescriptorInput*>(opaque);
    if (!input || input->fd < 0 || !buffer || buffer_size <= 0) {
        return AVERROR(EINVAL);
    }
    while (true) {
        const ssize_t count = ::pread(
            input->fd, buffer, static_cast<size_t>(buffer_size),
            static_cast<off_t>(input->offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return AVERROR(errno);
        }
        if (count == 0) {
            return AVERROR_EOF;
        }
        input->offset += count;
        return static_cast<int>(count);
    }
}

int64_t descriptor_input_seek(void* opaque, int64_t offset, int whence)
{
    auto* input = static_cast<DescriptorInput*>(opaque);
    if (!input || input->fd < 0) {
        return AVERROR(EINVAL);
    }
    struct stat descriptor_stat {};
    if (::fstat(input->fd, &descriptor_stat) != 0 ||
        descriptor_stat.st_size < 0) {
        return AVERROR(errno == 0 ? EIO : errno);
    }
    const int base_whence = whence & ~AVSEEK_FORCE;
    if (base_whence == AVSEEK_SIZE) {
        return descriptor_stat.st_size;
    }
    int64_t base = 0;
    if (base_whence == SEEK_CUR) {
        base = input->offset;
    } else if (base_whence == SEEK_END) {
        base = descriptor_stat.st_size;
    } else if (base_whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset)) {
        return AVERROR(EOVERFLOW);
    }
    const int64_t next = base + offset;
    if (next < 0) {
        return AVERROR(EINVAL);
    }
    input->offset = next;
    return next;
}

void expect_reopenable_from_held_fd(int fd, bool require_video_stream)
{
    DescriptorInput descriptor_input{fd, 0};
    constexpr int kBufferSize = 4096;
    auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferSize));
    expect(buffer != nullptr, "could not allocate descriptor input buffer");
    AVIOContext* io = avio_alloc_context(
        buffer, kBufferSize, 0, &descriptor_input,
        &descriptor_input_read, nullptr, &descriptor_input_seek);
    if (!io) {
        av_free(buffer);
        throw std::runtime_error("could not allocate descriptor input AVIO");
    }
    io->seekable = AVIO_SEEKABLE_NORMAL;
    AVFormatContext* input = avformat_alloc_context();
    expect(input != nullptr, "could not allocate descriptor input format context");
    input->pb = io;
    input->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int open_result =
        avformat_open_input(&input, nullptr, nullptr, nullptr);
    if (open_result < 0) {
        if (input) {
            avformat_free_context(input);
            input = nullptr;
        }
        av_freep(&io->buffer);
        avio_context_free(&io);
        throw std::runtime_error(
            "FFmpeg could not reopen descriptor-backed MP4: " +
            std::to_string(open_result));
    }
    if (require_video_stream) {
        expect(input->nb_streams == 1,
               "descriptor-backed packet MP4 must retain its video stream");
    }
    const AVDictionaryEntry* intent = av_dict_get(
        input->metadata,
        OrangeVideoContainerFinalization::kFullFrameRatePlaybackIntentKey,
        nullptr, 0);
    expect(intent != nullptr && std::string(intent->value) == "1",
           "descriptor reopen did not recover playback intent");
    avformat_close_input(&input);
    av_freep(&io->buffer);
    avio_context_free(&io);
}

void write_decoy(const std::filesystem::path& path,
                 const std::string& sentinel)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    expect(output.is_open(), "could not create decoy: " + path.string());
    output << sentinel;
    output.flush();
    expect(static_cast<bool>(output), "could not write decoy");
}

std::string read_path(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "could not read path: " + path.string());
    return std::string(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
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
        expect(writer.finalize(),
               "explicit legacy finalization must succeed");
        expect(writer.finalized(),
               "explicit legacy finalization must reach a terminal state");
        expect(writer.finalize(),
               "successful explicit finalization must be idempotent");
        expect(!writer.failure_stats().failed,
               "success stats must be terminal before destruction");
    } // destructor is an idempotent fallback

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

void test_descriptor_empty_container_is_seekable_and_reopenable()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::filesystem::path video_path = dir / "empty.mp4";
    const std::filesystem::path keyframe_path = dir / "empty_keyframe.json";
    const std::filesystem::path finalization_path =
        dir / "empty_finalization.json";
    const int video_fd = open_rw_artifact(video_path);
    const int keyframe_fd = open_rw_artifact(keyframe_path);
    const int finalization_fd = open_rw_artifact(finalization_path);

    {
        FFmpegWriterDescriptorOutputConfig output(
            video_fd,
            keyframe_fd,
            finalization_fd,
            16 * 1024 * 1024,
            "camera_0/empty.mp4",
            "camera_0/empty_keyframe.json",
            "camera_0/empty_finalization.json");
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 640, 480, 700, std::move(output));
        expect(writer.is_open(), "descriptor writer must open");
        writer.create_thread();
        expect(writer.finalize(),
               "empty descriptor output must explicitly finalize");
        expect(writer.finalize(),
               "descriptor finalization must be idempotent");
        expect(!writer.failure_stats().failed,
               "empty descriptor terminal stats must report success");
    }

    expect_reopenable_from_held_fd(video_fd, false);
    const nlohmann::json keyframes = nlohmann::json::parse(read_fd(keyframe_fd));
    expect(keyframes.size() == 8 &&
               keyframes.at("schema_id") ==
                   "orange.spatial_roi_keyframe_summary" &&
               keyframes.at("schema_version") == 1 &&
               keyframes.at("terminal").get<bool>() &&
               keyframes.at("codec") == "h264" &&
               keyframes.at("fps") == 700 &&
               keyframes.at("total_frames") == 0 &&
               keyframes.at("frame_index_sequence").size() == 3 &&
               keyframes.at("frame_index_sequence").at("first").is_null() &&
               keyframes.at("frame_index_sequence").at("last").is_null() &&
               keyframes.at("frame_index_sequence")
                   .at("zero_based_contiguous")
                   .get<bool>() &&
               keyframes.at("keyframe_policy").at("name") ==
                   "all_frames_idr" &&
               keyframes.at("keyframe_policy").size() == 4 &&
               keyframes.at("keyframe_policy").at("keyframe_frames") == 0 &&
               keyframes.at("keyframe_policy")
                       .at("non_keyframe_frames") == 0 &&
               keyframes.at("keyframe_policy").at("satisfied").get<bool>(),
           "empty descriptor output must write the closed compact keyframe summary");
    const nlohmann::json finalization =
        nlohmann::json::parse(read_fd(finalization_fd));
    expect(finalization.at("status") == "complete" &&
               finalization.at("terminal").get<bool>(),
           "empty descriptor output must finalize completely");
    expect(finalization.at("video_path") == "camera_0/empty.mp4" &&
               finalization.at("sidecar_path") ==
                   "camera_0/empty_finalization.json",
           "descriptor lifecycle evidence must preserve display labels");

    expect(::close(video_fd) == 0, "could not close held video fd");
    expect(::close(keyframe_fd) == 0, "could not close held keyframe fd");
    expect(::close(finalization_fd) == 0,
           "could not close held finalization fd");
    std::filesystem::remove_all(dir);
}

void test_descriptor_packet_and_sidecars_survive_leaf_replacement()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::filesystem::path video_path = dir / "roi.mp4";
    const std::filesystem::path keyframe_path = dir / "roi_keyframe.json";
    const std::filesystem::path finalization_path =
        dir / "roi_finalization.json";
    const std::filesystem::path metadata_derived_trap = dir / "roi_meta.json";
    const std::filesystem::path held_video = dir / "held_video.inode";
    const std::filesystem::path held_keyframe = dir / "held_keyframe.inode";
    const std::filesystem::path held_finalization =
        dir / "held_finalization.inode";
    const std::string video_decoy = "VIDEO_PATH_REPLACEMENT";
    const std::string keyframe_decoy = "KEYFRAME_PATH_REPLACEMENT";
    const std::string finalization_decoy = "FINALIZATION_PATH_REPLACEMENT";
    const std::string metadata_decoy = "METADATA_DERIVATION_TRAP";

    const int video_fd = open_rw_artifact(video_path);
    const int keyframe_fd = open_rw_artifact(keyframe_path);
    const int finalization_fd = open_rw_artifact(finalization_path);

    {
        FFmpegWriterDescriptorOutputConfig output(
            video_fd,
            keyframe_fd,
            finalization_fd,
            16 * 1024 * 1024,
            "camera_0/region_0/roi.mp4",
            "camera_0/region_0/roi_keyframe.json",
            "camera_0/region_0/roi_finalization.json");
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 64, 64, 30, std::move(output));

        std::filesystem::rename(video_path, held_video);
        std::filesystem::rename(keyframe_path, held_keyframe);
        std::filesystem::rename(finalization_path, held_finalization);
        write_decoy(video_path, video_decoy);
        write_decoy(keyframe_path, keyframe_decoy);
        write_decoy(finalization_path, finalization_decoy);
        write_decoy(metadata_derived_trap, metadata_decoy);

        // A compact Annex-B IDR-shaped payload is sufficient to exercise the
        // MP4 packet/mux path; decoding is deliberately outside this host test.
        std::vector<uint8_t> packet = {
            0x00, 0x00, 0x00, 0x01,
            0x65, 0x88, 0x84, 0x21, 0xa0,
        };
        expect(writer.push_packet(
                   packet.data(), static_cast<int>(packet.size()), 0),
               "descriptor packet must enter the writer queue");
        writer.create_thread();
        expect(writer.finalize(),
               "descriptor packet output must explicitly finalize");
        expect(!writer.failure_stats().failed,
               "descriptor packet terminal stats must report success");
    }

    expect(read_fd(video_fd).size() > video_decoy.size(),
           "held video inode did not receive the finalized MP4");
    const nlohmann::json keyframe_summary =
        nlohmann::json::parse(read_fd(keyframe_fd));
    expect(keyframe_summary.at("total_frames") == 1 &&
               keyframe_summary.at("frame_index_sequence").at("first") == 0 &&
               keyframe_summary.at("frame_index_sequence").at("last") == 0 &&
               keyframe_summary.at("frame_index_sequence")
                   .at("zero_based_contiguous")
                   .get<bool>() &&
               keyframe_summary.at("keyframe_policy")
                       .at("keyframe_frames") == 1 &&
               keyframe_summary.at("keyframe_policy")
                       .at("non_keyframe_frames") == 0 &&
               keyframe_summary.at("keyframe_policy")
                   .at("satisfied")
                   .get<bool>(),
           "explicit keyframe inode did not receive compact IDR evidence");
    const nlohmann::json finalization =
        nlohmann::json::parse(read_fd(finalization_fd));
    expect(finalization.at("status") == "complete",
           "descriptor packet output did not finalize completely");
    expect(finalization.at("video_path") ==
               "camera_0/region_0/roi.mp4" &&
               finalization.at("sidecar_path") ==
                   "camera_0/region_0/roi_finalization.json",
           "descriptor lifecycle evidence used something other than labels");
    expect_reopenable_from_held_fd(video_fd, true);

    expect(read_path(video_path) == video_decoy,
           "writer redirected media through the replacement pathname");
    expect(read_path(keyframe_path) == keyframe_decoy,
           "writer redirected keyframe evidence through the replacement pathname");
    expect(read_path(finalization_path) == finalization_decoy,
           "writer redirected lifecycle evidence through the replacement pathname");
    expect(read_path(metadata_derived_trap) == metadata_decoy,
           "writer derived a metadata sidecar instead of using the explicit keyframe fd");

    expect(::close(video_fd) == 0, "could not close held video fd");
    expect(::close(keyframe_fd) == 0, "could not close held keyframe fd");
    expect(::close(finalization_fd) == 0,
           "could not close held finalization fd");
    std::filesystem::remove_all(dir);
}

void test_descriptor_keyframe_summary_is_constant_size()
{
    const std::filesystem::path dir = make_temp_dir();
    const int video_fd = open_rw_artifact(dir / "many_frames.mp4");
    const int keyframe_fd =
        open_rw_artifact(dir / "many_frames_keyframe.json");
    const int finalization_fd =
        open_rw_artifact(dir / "many_frames_finalization.json");
    constexpr int64_t kFrameCount = 4096;
    {
        FFmpegWriterDescriptorOutputConfig output(
            video_fd, keyframe_fd, finalization_fd, 16 * 1024 * 1024,
            "camera_0/many_frames.mp4",
            "camera_0/many_frames_keyframe.json",
            "camera_0/many_frames_finalization.json");
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 64, 64, 700, std::move(output));
        std::vector<uint8_t> packet = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
        };
        for (int64_t frame = 0; frame < kFrameCount; ++frame) {
            expect(writer.push_packet(
                       packet.data(), static_cast<int>(packet.size()), frame),
                   "descriptor IDR packet admission unexpectedly failed");
        }
        expect(writer.finalize(),
               "many-frame descriptor output must explicitly finalize");
    }
    const std::string bytes = read_fd(keyframe_fd);
    expect(bytes.size() < 1024,
           "descriptor keyframe summary must remain constant-size");
    const nlohmann::json summary = nlohmann::json::parse(bytes);
    expect(summary.at("total_frames") == kFrameCount &&
               summary.at("frame_index_sequence").at("first") == 0 &&
               summary.at("frame_index_sequence").at("last") ==
                   kFrameCount - 1 &&
               summary.at("frame_index_sequence")
                   .at("zero_based_contiguous")
                   .get<bool>() &&
               summary.at("keyframe_policy").at("keyframe_frames") ==
                   kFrameCount &&
               summary.at("keyframe_policy").at("satisfied").get<bool>(),
           "constant-size keyframe summary counters are inconsistent");
    expect(::close(video_fd) == 0,
           "could not close many-frame video fd");
    expect(::close(keyframe_fd) == 0,
           "could not close many-frame keyframe fd");
    expect(::close(finalization_fd) == 0,
           "could not close many-frame finalization fd");
    std::filesystem::remove_all(dir);
}

void test_finalize_linearizes_against_concurrent_admission()
{
    const std::filesystem::path dir = make_temp_dir();
    const int video_fd = open_rw_artifact(dir / "concurrent.mp4");
    const int keyframe_fd =
        open_rw_artifact(dir / "concurrent_keyframe.json");
    const int finalization_fd =
        open_rw_artifact(dir / "concurrent_finalization.json");
    std::atomic<uint64_t> accepted{0};
    {
        FFmpegWriterDescriptorOutputConfig output(
            video_fd, keyframe_fd, finalization_fd, 16 * 1024 * 1024,
            "camera_0/concurrent.mp4",
            "camera_0/concurrent_keyframe.json",
            "camera_0/concurrent_finalization.json");
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 64, 64, 700, std::move(output));
        writer.create_thread();
        std::vector<uint8_t> packet = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
        };
        for (int64_t frame = 0; frame < 16; ++frame) {
            expect(writer.push_packet(
                       packet.data(), static_cast<int>(packet.size()), frame),
                   "concurrency prefill packet was rejected");
            accepted.fetch_add(1, std::memory_order_relaxed);
        }
        std::thread producer([&]() {
            for (int64_t frame = 16; frame < 10000; ++frame) {
                if (!writer.push_packet(
                        packet.data(), static_cast<int>(packet.size()), frame)) {
                    break;
                }
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
        expect(writer.finalize(),
               "concurrent close-of-admission must finalize successfully");
        producer.join();
        const FFmpegWriterPacketWriteStats packet_stats_before_late_call =
            writer.packet_write_stats();
        const FFmpegWriterFailureStats failures_before_late_call =
            writer.failure_stats();
        expect(!writer.push_packet(
                   packet.data(), static_cast<int>(packet.size()), 10000),
               "a packet submitted after finalization must be rejected");
        const FFmpegWriterPacketWriteStats packet_stats_after_late_call =
            writer.packet_write_stats();
        const FFmpegWriterFailureStats failures_after_late_call =
            writer.failure_stats();
        expect(packet_stats_after_late_call.submissions_accepted ==
                   packet_stats_before_late_call.submissions_accepted &&
                   packet_stats_after_late_call.submissions_rejected ==
                       packet_stats_before_late_call.submissions_rejected &&
                   packet_stats_after_late_call.write_attempts ==
                       packet_stats_before_late_call.write_attempts &&
                   packet_stats_after_late_call.packets_written ==
                       packet_stats_before_late_call.packets_written &&
                   packet_stats_after_late_call.bytes_written ==
                       packet_stats_before_late_call.bytes_written &&
                   failures_after_late_call.failed ==
                       failures_before_late_call.failed &&
                   failures_after_late_call.total_failures ==
                       failures_before_late_call.total_failures,
               "late public submission changed immutable terminal accounting");
        expect(writer.finalize(),
               "late rejected submission must not change cached finalization");
    }
    const uint64_t accepted_count = accepted.load(std::memory_order_relaxed);
    const nlohmann::json summary =
        nlohmann::json::parse(read_fd(keyframe_fd));
    expect(summary.at("total_frames") == accepted_count &&
               summary.at("keyframe_policy").at("keyframe_frames") ==
                   accepted_count &&
               summary.at("frame_index_sequence").at("last") ==
                   accepted_count - 1 &&
               summary.at("frame_index_sequence")
                   .at("zero_based_contiguous")
                   .get<bool>(),
           "terminal keyframe summary raced accepted packet accounting");
    expect(::close(video_fd) == 0,
           "could not close concurrent video fd");
    expect(::close(keyframe_fd) == 0,
           "could not close concurrent keyframe fd");
    expect(::close(finalization_fd) == 0,
           "could not close concurrent finalization fd");
    std::filesystem::remove_all(dir);
}

void test_descriptor_video_byte_ceiling_fails_before_crossing()
{
    const std::filesystem::path dir = make_temp_dir();
    const int video_fd = open_rw_artifact(dir / "bounded_roi.mp4");
    const int keyframe_fd =
        open_rw_artifact(dir / "bounded_roi_keyframe.json");
    const int finalization_fd =
        open_rw_artifact(dir / "bounded_roi_finalization.json");

    bool rejected_zero_limit = false;
    try {
        FFmpegWriterDescriptorOutputConfig invalid(
            video_fd,
            keyframe_fd,
            finalization_fd,
            0,
            "camera_0/region_0/bounded_roi.mp4",
            "camera_0/region_0/bounded_roi_keyframe.json",
            "camera_0/region_0/bounded_roi_finalization.json");
    } catch (const std::invalid_argument&) {
        rejected_zero_limit = true;
    }
    expect(rejected_zero_limit,
           "descriptor output must reject an unset video byte ceiling");

    constexpr uint64_t kVideoLimit = 64 * 1024;
    {
        FFmpegWriterDescriptorOutputConfig output(
            video_fd,
            keyframe_fd,
            finalization_fd,
            kVideoLimit,
            "camera_0/region_0/bounded_roi.mp4",
            "camera_0/region_0/bounded_roi_keyframe.json",
            "camera_0/region_0/bounded_roi_finalization.json");
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 64, 64, 30, std::move(output));
        expect(writer.max_video_bytes() == kVideoLimit,
               "writer did not retain its authenticated video byte ceiling");

        std::vector<uint8_t> oversized_packet(256 * 1024, 0x80);
        oversized_packet[0] = 0x00;
        oversized_packet[1] = 0x00;
        oversized_packet[2] = 0x00;
        oversized_packet[3] = 0x01;
        oversized_packet[4] = 0x65;
        expect(writer.push_packet(
                   oversized_packet.data(),
                   static_cast<int>(oversized_packet.size()),
                   0),
               "oversized encoded packet must enter the unbounded test queue");
        writer.create_thread();
        expect(!writer.finalize(),
               "video byte-ceiling failure must fail explicit finalization");

        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(writer.video_size_limit_exceeded() && failures.failed,
               "custom AVIO must latch the video byte-ceiling failure");
        expect(failures.video_size_limit_failures == 1,
               "video byte-ceiling rejection must latch exactly once");
        expect(failures.last_error_code == AVERROR(ENOSPC),
               "video byte-ceiling rejection must preserve ENOSPC");
        struct stat video_stat {};
        expect(::fstat(video_fd, &video_stat) == 0 &&
                   video_stat.st_size >= 0 &&
                   static_cast<uint64_t>(video_stat.st_size) <= kVideoLimit,
               "custom AVIO wrote beyond max_video_bytes");
    }

    struct stat terminal_video_stat {};
    expect(::fstat(video_fd, &terminal_video_stat) == 0 &&
               terminal_video_stat.st_size >= 0 &&
               static_cast<uint64_t>(terminal_video_stat.st_size) <=
                   kVideoLimit,
           "container finalization wrote beyond max_video_bytes");
    const nlohmann::json finalization =
        nlohmann::json::parse(read_fd(finalization_fd));
    expect(finalization.at("status") == "container_finalization_failed",
           "byte-ceiling failure must not publish a complete container");

    expect(::close(video_fd) == 0, "could not close bounded video fd");
    expect(::close(keyframe_fd) == 0, "could not close bounded keyframe fd");
    expect(::close(finalization_fd) == 0,
           "could not close bounded finalization fd");
    std::filesystem::remove_all(dir);
}

void test_invalid_fps_and_pts_overflow_fail_closed()
{
    const std::filesystem::path dir = make_temp_dir();
    const std::string invalid_fps_path = (dir / "invalid_fps.mp4").string();
    bool fps_rejected = false;
    try {
        FFmpegWriter writer(
            AV_CODEC_ID_H264,
            64,
            64,
            0,
            invalid_fps_path.c_str(),
            nullptr);
    } catch (const std::invalid_argument&) {
        fps_rejected = true;
    }
    expect(fps_rejected,
           "zero FPS must be rejected before opening an output");
    expect(!std::filesystem::exists(invalid_fps_path),
           "invalid FPS must not leave a media artifact");

    const std::string overflow_path = (dir / "pts_overflow.mp4").string();
    {
        FFmpegWriter writer(
            AV_CODEC_ID_H264, 64, 64, 30, overflow_path.c_str(), nullptr);
        std::vector<uint8_t> packet = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
        };
        expect(!writer.push_packet(
                   packet.data(),
                   static_cast<int>(packet.size()),
                   std::numeric_limits<int64_t>::max()),
               "unrepresentable PTS must be rejected before enqueue");
        expect(!writer.finalize(),
               "PTS overflow must fail explicit finalization");
        const FFmpegWriterFailureStats failures = writer.failure_stats();
        expect(failures.failed && failures.packet_enqueue_failures == 1 &&
                   failures.last_error_code == AVERROR(EOVERFLOW),
               "PTS overflow must be terminal and preserve EOVERFLOW");
    }
    std::filesystem::remove_all(dir);
}

void test_descriptor_terminal_io_failures_are_observable()
{
    {
        const std::filesystem::path dir = make_temp_dir();
        const int video_fd = open_rw_artifact(dir / "close.mp4");
        const int keyframe_fd = open_rw_artifact(dir / "close_keyframe.json");
        const int finalization_fd =
            open_rw_artifact(dir / "close_finalization.json");
        {
            FFmpegWriterDescriptorOutputConfig output(
                video_fd, keyframe_fd, finalization_fd, 16 * 1024 * 1024,
                "camera_0/close.mp4",
                "camera_0/close_keyframe.json",
                "camera_0/close_finalization.json");
            FFmpegWriter writer(
                AV_CODEC_ID_H264, 64, 64, 30, std::move(output));
            writer.test_invalidate_descriptor_video_io_for_finalize();
            expect(!writer.finalize(),
                   "injected descriptor close/fsync failure must fail finalize");
            const FFmpegWriterFailureStats failures = writer.failure_stats();
            expect(failures.failed && failures.muxer_flush_failures >= 1,
                   "descriptor close/fsync failure must be visible in terminal stats");
            expect(!writer.finalize(),
                   "failed descriptor finalization must be idempotent");
        }
        const nlohmann::json terminal =
            nlohmann::json::parse(read_fd(finalization_fd));
        expect(terminal.at("status") == "container_finalization_failed" &&
                   terminal.at("terminal").get<bool>(),
               "descriptor close failure must persist terminal-failed evidence");
        expect(::close(video_fd) == 0, "could not close close-test video fd");
        expect(::close(keyframe_fd) == 0,
               "could not close close-test keyframe fd");
        expect(::close(finalization_fd) == 0,
               "could not close close-test finalization fd");
        std::filesystem::remove_all(dir);
    }

    {
        const std::filesystem::path dir = make_temp_dir();
        const int video_fd = open_rw_artifact(dir / "keyframe.mp4");
        const int keyframe_fd =
            open_rw_artifact(dir / "keyframe_summary.json");
        const int finalization_fd =
            open_rw_artifact(dir / "keyframe_finalization.json");
        {
            FFmpegWriterDescriptorOutputConfig output(
                video_fd, keyframe_fd, finalization_fd, 16 * 1024 * 1024,
                "camera_0/keyframe.mp4",
                "camera_0/keyframe_summary.json",
                "camera_0/keyframe_finalization.json");
            FFmpegWriter writer(
                AV_CODEC_ID_H264, 64, 64, 30, std::move(output));
            writer.test_invalidate_descriptor_keyframe_for_finalize();
            expect(!writer.finalize(),
                   "injected keyframe-sidecar failure must fail finalize");
            const FFmpegWriterFailureStats failures = writer.failure_stats();
            expect(failures.failed && failures.sidecar_write_failures >= 1,
                   "keyframe-sidecar failure must be visible in terminal stats");
        }
        expect(read_fd(keyframe_fd).empty(),
               "failed keyframe rewrite must not certify a terminal summary");
        const nlohmann::json terminal =
            nlohmann::json::parse(read_fd(finalization_fd));
        expect(terminal.at("status") == "container_finalization_failed",
               "keyframe-sidecar failure must persist terminal-failed evidence");
        expect(::close(video_fd) == 0,
               "could not close keyframe-test video fd");
        expect(::close(keyframe_fd) == 0,
               "could not close keyframe-test keyframe fd");
        expect(::close(finalization_fd) == 0,
               "could not close keyframe-test finalization fd");
        std::filesystem::remove_all(dir);
    }

    {
        const std::filesystem::path dir = make_temp_dir();
        const int video_fd = open_rw_artifact(dir / "terminal.mp4");
        const int keyframe_fd =
            open_rw_artifact(dir / "terminal_keyframe.json");
        const int finalization_fd =
            open_rw_artifact(dir / "terminal_finalization.json");
        {
            FFmpegWriterDescriptorOutputConfig output(
                video_fd, keyframe_fd, finalization_fd, 16 * 1024 * 1024,
                "camera_0/terminal.mp4",
                "camera_0/terminal_keyframe.json",
                "camera_0/terminal_finalization.json");
            FFmpegWriter writer(
                AV_CODEC_ID_H264, 64, 64, 30, std::move(output));
            expect(::ftruncate(finalization_fd, 0) == 0 &&
                       ::lseek(finalization_fd, 0, SEEK_SET) == 0 &&
                       ::write(finalization_fd, "{", 1) == 1 &&
                       ::fsync(finalization_fd) == 0,
                   "could not inject a partial finalization-sidecar rewrite");
            writer.test_invalidate_descriptor_finalization_for_finalize();
            expect(!writer.finalize(),
                   "injected finalization-sidecar failure must fail finalize");
            const FFmpegWriterFailureStats failures = writer.failure_stats();
            expect(failures.failed && failures.sidecar_write_failures >= 2,
                   "both finalizing and terminal sidecar failures must be observable");
        }
        const std::string residue = read_fd(finalization_fd);
        bool residue_parsed = false;
        try {
            const nlohmann::json parsed = nlohmann::json::parse(residue);
            (void)parsed;
            residue_parsed = true;
        } catch (const nlohmann::json::parse_error&) {
        }
        expect(!residue_parsed,
               "partial in-place terminal rewrite must remain unparseable, not certified");
        expect(::close(video_fd) == 0,
               "could not close terminal-test video fd");
        expect(::close(keyframe_fd) == 0,
               "could not close terminal-test keyframe fd");
        expect(::close(finalization_fd) == 0,
               "could not close terminal-test finalization fd");
        std::filesystem::remove_all(dir);
    }
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
        expect(writer.failed(),
               "a mux failure must latch the general writer failure boundary");
        expect(stats.write_attempts == 1 && stats.packets_written == 0 &&
                   stats.write_failures == 1 &&
                   stats.first_write_error_code == injected_error,
               "mux failure counters must preserve the exact failed attempt");
        writer.create_thread();
        expect(!writer.finalize(),
               "a latched mux failure must fail explicit finalization");
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
        expect(!writer.finalize(),
               "a rejected packet must fail explicit finalization");
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
        {"descriptor_empty_container_is_seekable_and_reopenable",
         &test_descriptor_empty_container_is_seekable_and_reopenable},
        {"descriptor_packet_and_sidecars_survive_leaf_replacement",
         &test_descriptor_packet_and_sidecars_survive_leaf_replacement},
        {"descriptor_keyframe_summary_is_constant_size",
         &test_descriptor_keyframe_summary_is_constant_size},
        {"finalize_linearizes_against_concurrent_admission",
         &test_finalize_linearizes_against_concurrent_admission},
        {"descriptor_video_byte_ceiling_fails_before_crossing",
         &test_descriptor_video_byte_ceiling_fails_before_crossing},
        {"invalid_fps_and_pts_overflow_fail_closed",
         &test_invalid_fps_and_pts_overflow_fail_closed},
        {"descriptor_terminal_io_failures_are_observable",
         &test_descriptor_terminal_io_failures_are_observable},
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
