#include "spatial_roi_finalized_session_receipt.h"

#include "gui/spatial_layout/sha256.h"
#include "session/spatial_roi_recording_config.h"
#include "session/spatial_roi_recorder_contract.h"
#include "spatial_roi_recorder_evidence.h"
#include "spatial_roi_recorder_video_sanity.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <unistd.h>

namespace orange::spatial_roi::recording {

// The production probe keeps this constructor private. This test-only
// capability is the same descriptor-retaining shape used by the existing
// evidence tests; it lets this suite exercise the complete-manifest path
// without invoking FFmpeg or a GPU.
class SpatialRoiRecorderVideoSanityTestFactory final {
public:
    static std::shared_ptr<const SpatialRoiRecorderVideoSanityResult> Create(
        const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& root,
        const std::string& relative_path,
        const std::uint64_t frame_count,
        std::string* error_out)
    {
        std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
        if (!root || frame_count == 0 ||
            !root->OpenExistingFile(
                relative_path, SpatialRoiRecorderArtifactFileAccess::kReadOnly,
                &file, error_out)) {
            return nullptr;
        }
        struct stat status {};
        if (::fstat(file->borrowed_fd(), &status) != 0 || status.st_size <= 0) {
            if (error_out) *error_out = "test video stat failed";
            return nullptr;
        }
        std::vector<SpatialRoiRecorderVideoSanitySample> samples;
        samples.push_back({0, 64.0, 10.0, 0, 200, 0.1, 0.0, 16});
        auto shared_file = std::shared_ptr<SpatialRoiRecorderArtifactFile>(
            std::move(file));
        return std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>(
            new SpatialRoiRecorderVideoSanityResult(
                root, shared_file, root->artifact_root_identity(),
                shared_file->identity(), relative_path,
                static_cast<std::uint64_t>(status.st_size),
                "sha256:" +
                    orange::gui::spatial_layout::checksum::sha256_hex(
                        read_file(shared_file->borrowed_fd())),
                "0.01", "100/1", "1/10000", true, 0, 0,
                "mov,mp4,m4a,3gp,3g2,mj2", "hevc", "hevc@test",
                "yuvj420p", "pc", 8, "4:2:0", 4, 4, frame_count,
                std::move(samples)));
    }

private:
    static std::string read_file(const int fd)
    {
        if (::lseek(fd, 0, SEEK_SET) < 0) return {};
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return bytes;
    }
};

}  // namespace orange::spatial_roi::recording

namespace {

namespace fs = std::filesystem;
namespace contract = orange::session::spatial_roi;
namespace evidence = orange::spatial_roi::recording;
namespace encoder = orange::spatial_roi::encoder;
namespace checksum = orange::gui::spatial_layout::checksum;
using json = nlohmann::json;

constexpr char kCameraSerial[] = "CAM001";
constexpr std::uint64_t kFrameCount = 1;
constexpr std::uint64_t kEncodedBytes = 101;
constexpr std::uint64_t kMaxFrames = 8;
constexpr std::uint64_t kMaxMediaBytes = 1024 * 1024;
constexpr std::uint64_t kMaxEvidenceBytes = 8 * 1024 * 1024;

void expect(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

fs::path make_root()
{
    std::string pattern = "/tmp/orange_spatial_roi_receipt_test_XXXXXX";
    std::vector<char> value(pattern.begin(), pattern.end());
    value.push_back('\0');
    char* result = ::mkdtemp(value.data());
    expect(result != nullptr, "mkdtemp failed");
    return fs::path(result);
}

struct Fixture final {
    fs::path recording_root;
    json plan;
    json recorder_contract;
    contract::SpatialRoiRecorderRuntimeGpuMapping gpu_mapping;
    contract::SpatialRoiRecorderCameraContractView camera_contract;
    std::vector<evidence::SpatialRoiRecorderEvidenceBinding> bindings;
    std::shared_ptr<evidence::SpatialRoiRecorderArtifactRoot> authority;

    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(recording_root, ignored);
    }
};

std::string video_bytes(const std::string& stream_id)
{
    return "fake-lossless-video-" + stream_id;
}

void write_bytes(Fixture& fixture,
                 const std::string& relative_path,
                 const std::string& bytes)
{
    std::unique_ptr<evidence::SpatialRoiRecorderArtifactFile> file;
    std::string error;
    expect(fixture.authority->CreateFile(relative_path, &file, &error),
           "authorized artifact creation failed: " + error);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(file->borrowed_fd(), bytes.data() + offset,
                                      bytes.size() - offset);
        expect(count > 0, "authorized artifact write failed");
        offset += static_cast<std::size_t>(count);
    }
    expect(file->Seal(&error), "authorized artifact seal failed: " + error);
}

std::string metadata(const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    std::ostringstream output;
    output << "recording_frame_id,roi_stream_frame_index,output_frame_index,"
              "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,"
              "assigned_gpu_id,assigned_shard_id\n"
           << "1001,1,1,1000001,2000001," << binding.source_gpu_id << ','
           << binding.assigned_gpu_id << ',' << binding.assigned_shard_id << '\n';
    return output.str();
}

json pending_candidate(const char* schema_id,
                       const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    return {{"schema_id", schema_id},
            {"schema_version", 1},
            {"state", "pending_manifest"},
            {"certifying", false},
            {"requires_finalized_evidence_manifest", true},
            {"commit_marker", "evidence_manifest"},
            {"commit_marker_state", "required_finalized"},
            {"logical_stream_id", binding.logical_stream_id},
            {"frame_count", kFrameCount}};
}

json keyframes(const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    const std::uint64_t gop_length =
        binding.encode_profile.at("gop_length").get<std::uint64_t>();
    const std::uint64_t keyframe_count =
        kFrameCount == 0 ? 0 : 1U + ((kFrameCount - 1U) / gop_length);
    return {{"schema_id", "orange.spatial_roi_keyframe_summary"},
            {"schema_version", 1},
            {"terminal", true},
            {"codec", "hevc"},
            {"fps", 100},
            {"total_frames", kFrameCount},
            {"frame_index_sequence", {{"first", 0}, {"last", 0},
                                       {"zero_based_contiguous", true}}},
            {"keyframe_policy", {{"name", gop_length == 1
                                                   ? "all_frames_idr"
                                                   : "fixed_gop_" +
                                                         std::to_string(gop_length) +
                                                         "_idr"},
                                  {"keyframe_frames", keyframe_count},
                                  {"non_keyframe_frames",
                                   kFrameCount - keyframe_count},
                                  {"satisfied", true}}}};
}

json finalization(const evidence::SpatialRoiRecorderEvidenceBinding& binding,
                  const std::uint64_t video_size)
{
    return {{"schema_id", "orange.video_container_finalization"},
            {"schema_version", 2},
            {"generated_at_utc", "2026-09-01T12:01:00Z"},
            {"status", "complete"},
            {"terminal", true},
            {"video_path", binding.expected_artifacts.at("video")},
            {"sidecar_path", binding.expected_artifacts.at("finalization")},
            {"recording_fps", 100},
            {"packet_writes",
             {{"submissions_accepted", kFrameCount},
              {"submission_bytes_accepted", kEncodedBytes},
              {"submissions_rejected", 0},
              {"write_attempts", kFrameCount},
              {"packets_written", kFrameCount},
              {"bytes_written", kEncodedBytes},
              {"write_failures", 0},
              {"first_write_error_code", nullptr},
              {"writer_error_latched", false},
              {"muxer_flush_attempted", true},
              {"muxer_flush_succeeded", true},
              {"muxer_flush_error_code", nullptr},
              {"muxer_flush_error", nullptr},
              {"complete", true}}},
            {"container", {{"header_written", true},
                            {"trailer_attempted", true},
                            {"trailer_written", true},
                            {"output_close_attempted", true},
                            {"output_closed", true},
                            {"finalized", true},
                            {"trailer_error_code", nullptr},
                            {"trailer_error", nullptr},
                            {"output_close_error_code", nullptr},
                            {"output_close_error", nullptr},
                            {"file_size_bytes", video_size},
                            {"file_size_error", nullptr}}},
            {"quicktime_full_frame_rate_playback_intent",
             {{"key", "com.apple.quicktime.full-frame-rate-playback-intent"},
              {"requested_value", 1},
              {"required_data_type", "UInt8"},
              {"quicktime_data_atom_type", 22},
              {"patch_attempted", true},
              {"patch_applied", true},
              {"error", nullptr}}}};
}

json sanity(const evidence::SpatialRoiRecorderEvidenceBinding& binding,
            const std::uint64_t video_size,
            const fs::path& absolute_video_path,
            const std::string& video_sha)
{
    struct stat video_status {};
    expect(::stat(absolute_video_path.c_str(), &video_status) == 0,
           "could not stat video for sanity receipt");
    return {{"schema_id", "orange.spatial_roi_recorder.video_sanity"},
            {"schema_version", 1},
            {"state", "pending_manifest"},
            {"certifying", false},
            {"requires_finalized_evidence_manifest", true},
            {"commit_marker", "evidence_manifest"},
            {"commit_marker_state", "required_finalized"},
            {"logical_stream_id", binding.logical_stream_id},
            {"frame_count", kFrameCount},
            {"video_path", binding.expected_artifacts.at("video")},
            {"video_size_bytes", video_size},
            {"video_sha256", video_sha},
            {"video_device", static_cast<std::uint64_t>(video_status.st_dev)},
            {"video_inode", static_cast<std::uint64_t>(video_status.st_ino)},
            {"content_checked", true},
            {"content_valid", true},
            {"status", "pass"},
            {"width", 4},
            {"height", 4},
            {"nb_frames", kFrameCount},
            {"container", {{"size", std::to_string(video_size)},
                            {"duration", "0.01"}}},
            {"container_name", "mov,mp4,m4a,3gp,3g2,mj2"},
            {"codec", "hevc"},
            {"decoder", "hevc@test"},
            {"timeline", {{"frame_rate", "100/1"},
                           {"time_base", "1/10000"},
                           {"has_decoded_pts", true},
                           {"first_decoded_pts", 0},
                           {"last_decoded_pts", 0}}},
            {"pixel_semantics", {{"pixel_format", "yuvj420p"},
                                  {"color_range", "pc"},
                                  {"bit_depth", 8},
                                  {"chroma_subsampling", "4:2:0"}}},
            {"sampled_frame_count", 1},
            {"mean_luma", 64.0},
            {"max_stddev", 10.0},
            {"max_black_fraction_lt8", 0.1},
            {"thresholds", {{"max_black_fraction_lt8", 0.98},
                            {"min_max_stddev", 5.0}}},
            {"sampled_frames", {{{"requested_frame_index", 0},
                                  {"mean", 64.0},
                                  {"stddev", 10.0},
                                  {"min", 0},
                                  {"max", 200},
                                  {"black_fraction_lt8", 0.1},
                                  {"white_fraction_gt247", 0.0},
                                  {"decoded_bytes", 16}}}}};
}

evidence::SpatialRoiRecorderFrameEvidence frame(
    const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    evidence::SpatialRoiRecorderFrameEvidence value;
    value.frame.recording_id = binding.recording_id;
    value.frame.recording_identity_token = binding.recording_identity_token;
    value.frame.producer_generation = binding.producer_generation;
    value.frame.camera_id = binding.camera_id;
    value.frame.camera_serial = binding.camera_serial;
    value.frame.local_frame_id = 101;
    value.frame.camera_frame_id = 201;
    value.frame.recording_frame_id = 1001;
    value.frame.roi_stream_frame_index = 1;
    value.frame.camera_timestamp_ns = 1000001;
    value.frame.timestamp_sys_ns = 2000001;
    value.frame.roi_id = binding.roi_id;
    value.frame.region_id = binding.region_id;
    value.frame.arena_group_id = binding.arena_group_id;
    value.frame.arena_id = binding.has_arena_id ? binding.arena_id : "";
    value.frame.logical_stream_id = binding.logical_stream_id;
    value.frame.spatial_roi_plan_sha256 = binding.plan_sha256;
    const auto& geometry = binding.geometry_identity;
    value.frame.native_raster = {
        geometry.at("native_raster").at("width").get<std::uint32_t>(),
        geometry.at("native_raster").at("height").get<std::uint32_t>()};
    value.frame.content_rect = {
        geometry.at("content_rect").at("x").get<std::uint32_t>(),
        geometry.at("content_rect").at("y").get<std::uint32_t>(),
        geometry.at("content_rect").at("width").get<std::uint32_t>(),
        geometry.at("content_rect").at("height").get<std::uint32_t>()};
    value.frame.encoded_raster = {
        geometry.at("encoded_raster").at("width").get<std::uint32_t>(),
        geometry.at("encoded_raster").at("height").get<std::uint32_t>()};
    value.frame.encoded_content_rect = {
        geometry.at("encoded_content_rect").at("x").get<std::uint32_t>(),
        geometry.at("encoded_content_rect").at("y").get<std::uint32_t>(),
        geometry.at("encoded_content_rect").at("width").get<std::uint32_t>(),
        geometry.at("encoded_content_rect").at("height").get<std::uint32_t>()};
    value.frame.padding = {
        geometry.at("padding").at("left").get<std::uint32_t>(),
        geometry.at("padding").at("top").get<std::uint32_t>(),
        geometry.at("padding").at("right").get<std::uint32_t>(),
        geometry.at("padding").at("bottom").get<std::uint32_t>(),
        geometry.at("padding").at("value_mono8").get<std::uint32_t>()};
    value.frame.source_pixel_format = "mono8";
    value.frame.bytes =
        value.frame.encoded_raster.width * value.frame.encoded_raster.height;
    value.frame.source_gpu_id = binding.source_gpu_id;
    value.frame.assigned_gpu_id = binding.assigned_gpu_id;
    value.frame.assigned_shard_id = binding.assigned_shard_id;
    value.frame.routing_policy = binding.routing_policy;
    value.detach_status = "detached";
    value.source_release_safe = true;
    value.dispatch_admitted = true;
    value.ack_attempted = true;
    value.ack_sent = true;
    value.ack_accepted = true;
    value.release_attempted = true;
    value.release_sent = true;
    value.release_reason = "source_detached";
    value.encode_status = "encoded";
    value.output_frame_index = 1;
    value.packet_count = 1;
    value.encoded_bytes = kEncodedBytes;
    value.keyframe = true;
    return value;
}

std::shared_ptr<const encoder::SpatialRoiLosslessEncoderTerminalSnapshot>
snapshot(const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    auto value =
        std::make_shared<encoder::SpatialRoiLosslessEncoderTerminalSnapshot>();
    value->stream.recording_id = binding.recording_id;
    value->stream.recording_identity_token = binding.recording_identity_token;
    value->stream.producer_generation = binding.producer_generation;
    value->stream.camera_id = binding.camera_id;
    value->stream.camera_serial = binding.camera_serial;
    value->stream.roi_id = binding.roi_id;
    value->stream.region_id = binding.region_id;
    value->stream.arena_group_id = binding.arena_group_id;
    value->stream.arena_id = binding.has_arena_id ? binding.arena_id : "";
    value->stream.logical_stream_id = binding.logical_stream_id;
    value->stream.spatial_roi_plan_sha256 = binding.plan_sha256;
    value->terminal = true;
    value->successful = true;
    value->drain_completed = true;
    value->metadata_flushed = true;
    value->media_finalization_validated = true;
    value->artifacts_sealed = true;
    value->all_admitted_results_emitted = true;
    value->all_enqueue_attempts_accounted = true;
    value->nonempty_stream = true;
    value->source_release_safe = true;
    value->terminal_reason = "complete";
    value->counts.enqueue_attempted = 1;
    value->counts.enqueued = 1;
    value->counts.dequeued = 1;
    value->counts.copy_completed = 1;
    value->counts.source_releases = 1;
    value->counts.encoded_frames = 1;
    value->counts.encoded_packets = 1;
    value->counts.encoded_bytes = kEncodedBytes;
    value->counts.frame_results_emitted = 1;
    value->counts.encoded_results = 1;
    value->counts.peak_queue_depth = 1;
    value->counts.finalize_calls = 1;
    value->counts.finalized = true;
    value->counts.source_release_safe = true;
    value->counts.metadata_flushed = true;
    value->counts.media_finalization_validated = true;
    value->counts.artifacts_sealed = true;
    value->writer.observed = true;
    value->writer.close_finalization_validated = true;
    return value;
}

void write_stream_artifacts(Fixture& fixture,
                            const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    const std::string bytes = video_bytes(binding.logical_stream_id);
    const std::string video_sha = "sha256:" + checksum::sha256_hex(bytes);
    const fs::path video_path =
        fixture.recording_root / "external_spatial_roi_recorder" /
        binding.expected_artifacts.at("video");
    write_bytes(fixture, binding.expected_artifacts.at("video"), bytes);
    write_bytes(fixture, binding.expected_artifacts.at("metadata"), metadata(binding));
    write_bytes(fixture, binding.expected_artifacts.at("keyframes"),
                keyframes(binding).dump() + "\n");
    const std::string perf =
        "metric,value\n"
        "schema_id,orange.spatial_roi_recorder.perf\n"
        "schema_version,1\n"
        "state,pending_manifest\n"
        "certifying,false\n"
        "requires_finalized_evidence_manifest,true\n"
        "commit_marker,evidence_manifest\n"
        "commit_marker_state,required_finalized\nlogical_stream_id," +
        binding.logical_stream_id + "\nframe_count,1\n";
    write_bytes(fixture, binding.expected_artifacts.at("perf"),
                perf);
    json summary = pending_candidate("orange.spatial_roi_recorder.summary", binding);
    summary["status"] = "pending_manifest";
    write_bytes(fixture, binding.expected_artifacts.at("summary"),
                summary.dump() + "\n");
    json status = pending_candidate("orange.spatial_roi_recorder.status", binding);
    status["terminal"] = false;
    write_bytes(fixture, binding.expected_artifacts.at("status"), status.dump() + "\n");
    write_bytes(
        fixture, binding.expected_artifacts.at("recorder_log"),
        "schema_id=orange.spatial_roi_recorder.log schema_version=1 "
        "state=pending_manifest certifying=false "
        "requires_finalized_evidence_manifest=true commit_marker=evidence_manifest "
        "commit_marker_state=required_finalized logical_stream_id=" +
            binding.logical_stream_id + " frame_count=1\n");
    write_bytes(fixture, binding.expected_artifacts.at("transport_sidecar"),
                pending_candidate("orange.spatial_roi_recorder.transport", binding)
                    .dump() +
                    "\n");
    write_bytes(fixture, binding.expected_artifacts.at("finalization"),
                finalization(binding, bytes.size()).dump() + "\n");
    write_bytes(fixture, binding.expected_artifacts.at("video_sanity"),
                sanity(binding, bytes.size(), video_path, video_sha).dump() + "\n");
}

void finalize_stream(Fixture& fixture,
                     const evidence::SpatialRoiRecorderEvidenceBinding& binding)
{
    write_stream_artifacts(fixture, binding);
    evidence::SpatialRoiRecorderEvidenceWriterConfig config;
    config.artifact_root = fixture.authority;
    config.evidence_relative_path = binding.expected_artifacts.at("evidence");
    config.manifest_relative_path =
        binding.expected_artifacts.at("evidence_manifest");
    config.max_frames = binding.max_frames_per_stream;
    std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter> writer;
    std::string error;
    expect(evidence::SpatialRoiRecorderEvidenceWriter::Open(
               config, binding, &writer, &error),
           "evidence writer open failed: " + error);
    auto frame_value = frame(binding);
    expect(orange::spatial_roi::validate_spatial_roi_frame_descriptor(
               frame_value.frame, &error),
           "test frame descriptor invalid: " + error);
    error.clear();
    expect(writer->AppendFrame(frame_value, &error),
           "evidence frame append failed: " + error +
               " writer=" + writer->error());
    evidence::SpatialRoiRecorderFinalizeRequest request;
    request.encoder_terminal_snapshot = snapshot(binding);
    request.video_sanity_result =
        evidence::SpatialRoiRecorderVideoSanityTestFactory::Create(
            fixture.authority, binding.expected_artifacts.at("video"),
            kFrameCount, &error);
    expect(request.video_sanity_result != nullptr,
           "test decoder capability creation failed: " + error);
    for (const char* kind : {"transport_sidecar", "recorder_log", "finalization",
                             "video_sanity", "status", "summary", "perf",
                             "keyframes", "metadata", "video"}) {
        request.artifacts.push_back(
            {kind, binding.expected_artifacts.at(kind)});
    }
    json manifest;
    expect(writer->Finalize(request, &manifest, &error),
           "evidence finalization failed: " + error +
               " writer=" + writer->error());
}

std::unique_ptr<Fixture> make_fixture()
{
    auto fixture = std::make_unique<Fixture>();
    fixture->recording_root = make_root();
    contract::Config config = contract::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = kMaxFrames;
    config.recording_limits.max_media_bytes_per_stream = kMaxMediaBytes;
    config.recording_limits.max_evidence_bytes_per_stream = kMaxEvidenceBytes;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 32;
    config.admission.max_total_media_bytes = 4 * kMaxMediaBytes;
    config.admission.max_total_evidence_bytes = 4 * kMaxEvidenceBytes;

    contract::CameraConfig camera;
    camera.camera_id = 0;
    camera.camera_serial = kCameraSerial;
    camera.native_raster = {8, 8};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group0";
    camera.layout = {"layout_v1", "sha256:" + std::string(64, '1')};
    camera.materialization = {"materialization_v1", "sha256:" + std::string(64, '2')};
    camera.registration = {"registration_v1", "sha256:" + std::string(64, '3')};
    for (std::uint32_t index = 0; index < 4; ++index) {
        contract::RoiConfig roi;
        roi.roi_id = "roi" + std::to_string(index);
        roi.region_id = "region" + std::to_string(index);
        roi.has_arena_id = true;
        roi.arena_id = "arena" + std::to_string(index);
        roi.required = true;
        roi.content_rect = {(index % 2U) * 4U, (index / 2U) * 4U, 4, 4};
        roi.logical_stream_id =
            contract::expected_logical_stream_id(kCameraSerial, roi.roi_id);
        roi.artifact_stem =
            contract::expected_artifact_stem(kCameraSerial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(kCameraSerial, std::move(camera));

    contract::PlanContext context;
    context.recording_id = "receipt-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-09-01T12:00:00Z";
    context.producer_generation = "generation_receipt_001";
    std::string error;
    expect(contract::build_plan(config, context, &fixture->plan, nullptr, &error),
           "plan build failed: " + error);
    fixture->gpu_mapping.analytics_gpu_by_camera_serial.emplace(kCameraSerial, 0);
    for (std::uint32_t index = 0; index < 4; ++index) {
        const std::string stream = contract::expected_logical_stream_id(
            kCameraSerial, "roi" + std::to_string(index));
        fixture->gpu_mapping.recorder_gpu_by_logical_stream_id.emplace(
            stream, static_cast<int>(index + 1));
    }
    expect(contract::build_spatial_roi_recorder_contract(
               fixture->plan, fixture->recording_root.generic_string(),
               fixture->gpu_mapping, &fixture->recorder_contract, &error),
           "contract build failed: " + error);
    expect(contract::parse_spatial_roi_recorder_camera_contract(
               fixture->recorder_contract, fixture->plan,
               fixture->recording_root.generic_string(), fixture->gpu_mapping,
               &fixture->camera_contract, &error),
           "camera contract parse failed: " + error);

    std::vector<std::string> allowed_paths;
    std::set<std::string> unique_paths;
    for (const auto& stream : fixture->camera_contract.streams) {
        evidence::SpatialRoiRecorderEvidenceBinding binding;
        const bool binding_built =
            evidence::make_spatial_roi_recorder_evidence_binding(
                fixture->recorder_contract, fixture->plan,
                fixture->recording_root.generic_string(), fixture->gpu_mapping,
                stream.logical_stream_id, &binding, &error);
        expect(binding_built, "binding build failed: " + error);
        for (const auto& [kind, path] : binding.expected_artifacts) {
            (void)kind;
            expect(unique_paths.insert(path).second,
                   "fixture has duplicate artifact path");
            allowed_paths.push_back(path);
        }
        fixture->bindings.push_back(std::move(binding));
    }
    std::unique_ptr<evidence::SpatialRoiRecorderArtifactRoot> authority;
    expect(evidence::SpatialRoiRecorderArtifactRoot::Open(
               fixture->recording_root, allowed_paths, &authority, &error),
           "artifact root open failed: " + error);
    fixture->authority =
        std::shared_ptr<evidence::SpatialRoiRecorderArtifactRoot>(
            std::move(authority));
    for (const auto& binding : fixture->bindings) {
        finalize_stream(*fixture, binding);
    }
    return fixture;
}

evidence::SpatialRoiFinalizedSessionReceiptRequest request_for(
    const Fixture& fixture)
{
    evidence::SpatialRoiFinalizedSessionReceiptRequest request;
    request.recorder_contract = fixture.recorder_contract;
    request.verified_plan = fixture.plan;
    request.expected_recording_root = fixture.recording_root.generic_string();
    request.expected_gpu_mapping = fixture.gpu_mapping;
    request.camera_contract = fixture.camera_contract;
    return request;
}

void test_positive_plan_order_and_twelve_artifacts()
{
    auto fixture = make_fixture();
    json receipt;
    std::string error;
    expect(evidence::build_spatial_roi_finalized_session_receipt(
               request_for(*fixture), &receipt, &error),
           "receipt build failed: " + error);
    expect(receipt.at("schema_id") ==
               evidence::kSpatialRoiFinalizedSessionReceiptSchemaId,
           "receipt schema id is incorrect");
    expect(receipt.at("status") == "complete" &&
               receipt.at("stream_count") == 4U &&
               receipt.at("stream_order") == fixture->camera_contract.stream_order,
           "receipt completion/order envelope is incomplete");
    expect(receipt.at("identity").is_object() &&
               receipt.at("identity").size() == 9U &&
               receipt.at("identity").contains("recording_id") &&
               receipt.at("identity").contains("session_id") &&
               receipt.at("identity").contains("stream_order"),
           "receipt identity is not the closed object shape");
    expect(receipt.at("identity").at("stream_order") ==
               fixture->camera_contract.stream_order,
           "receipt stream order is not plan order");
    expect(receipt.at("streams").size() == 4U,
           "receipt does not contain exactly four streams");
    const std::array<const char*, 12> kinds = {
        "video", "metadata", "keyframes", "perf", "summary", "status",
        "video_sanity", "finalization", "recorder_log", "transport_sidecar",
        "evidence", "evidence_manifest"};
    for (std::size_t stream_index = 0; stream_index < 4U; ++stream_index) {
        const json& stream = receipt.at("streams").at(stream_index);
        expect(stream.at("identity").at("logical_stream_id") ==
                   fixture->camera_contract.stream_order.at(stream_index),
               "receipt stream identity is not plan ordered");
        expect(stream.at("counts").at("failed_frames") == 0,
               "receipt reports failed frames");
        expect(stream.at("ranges").at("frame_count") == kFrameCount,
               "receipt range count is incorrect");
        expect(stream.at("finalized_receipt_digest").is_string(),
               "receipt omitted finalized manifest digest");
        expect(stream.at("artifacts").size() == kinds.size(),
               "receipt does not contain exactly twelve artifact receipts");
        for (std::size_t artifact_index = 0; artifact_index < kinds.size();
             ++artifact_index) {
            const json& artifact = stream.at("artifacts").at(artifact_index);
            expect(artifact.at("kind") == kinds.at(artifact_index),
                   "receipt artifact order is not closed and deterministic");
            expect(artifact.at("relative_path").is_string() &&
                       artifact.at("size_bytes").is_number_unsigned() &&
                       artifact.at("sha256").is_string(),
                   "receipt artifact reference is incomplete");
        }
    }
    expect(receipt.at("root_authority").at("root_continuity").at("not_proven")
                   .size() == 3U,
           "receipt did not state the root continuity limitation");
    const std::string serialized = receipt.dump();
    expect(serialized.find(fixture->recording_root.generic_string()) ==
               std::string::npos &&
               serialized.find(
                   (fixture->recording_root /
                    "external_spatial_roi_recorder")
                       .generic_string()) == std::string::npos,
           "receipt leaked an absolute recording or artifact root");
}

void test_tamper_is_rejected()
{
    auto fixture = make_fixture();
    const auto& binding = fixture->bindings.front();
    std::ofstream output(
        fixture->recording_root / "external_spatial_roi_recorder" /
            binding.expected_artifacts.at("video"),
        std::ios::binary | std::ios::app);
    output << "tamper";
    output.close();
    json receipt;
    std::string error;
    expect(!evidence::build_spatial_roi_finalized_session_receipt(
               request_for(*fixture), &receipt, &error),
           "tampered artifact was accepted");
}

void test_omission_is_rejected()
{
    auto fixture = make_fixture();
    const auto& binding = fixture->bindings.at(2);
    std::error_code ignored;
    fs::remove(fixture->recording_root / "external_spatial_roi_recorder" /
                   binding.expected_artifacts.at("evidence_manifest"),
               ignored);
    json receipt;
    std::string error;
    expect(!evidence::build_spatial_roi_finalized_session_receipt(
               request_for(*fixture), &receipt, &error),
           "omitted evidence manifest was accepted");
}

void test_substituted_order_is_rejected()
{
    auto fixture = make_fixture();
    auto request = request_for(*fixture);
    std::swap(request.camera_contract.stream_order.at(0),
              request.camera_contract.stream_order.at(1));
    json receipt;
    std::string error;
    expect(!evidence::build_spatial_roi_finalized_session_receipt(
               request, &receipt, &error),
           "substituted camera stream order was accepted");
}

}  // namespace

int main()
{
    try {
        test_positive_plan_order_and_twelve_artifacts();
        test_tamper_is_rejected();
        test_omission_is_rejected();
        test_substituted_order_is_rejected();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
    std::cout << "[PASS] spatial ROI finalized session receipt tests\n";
    return 0;
}
