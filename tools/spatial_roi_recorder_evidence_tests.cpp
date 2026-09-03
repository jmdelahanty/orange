#include "spatial_roi_recorder_evidence.h"
#include "spatial_roi_recorder_video_sanity.h"

#include "gui/spatial_layout/sha256.h"
#include "session/spatial_roi_recording_config.h"
#include "session/spatial_roi_recorder_contract.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <unistd.h>

namespace orange::spatial_roi::recording {

#if defined(ORANGE_SPATIAL_ROI_VIDEO_SANITY_TESTING)
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
        const auto add_sample = [&](const std::uint64_t index) {
            samples.push_back({index, 64.0, 10.0, 0, 200, 0.1, 0.0, 16});
        };
        add_sample(0);
        if (frame_count > 2) add_sample(frame_count / 2);
        if (frame_count > 1) add_sample(frame_count - 1);
        auto shared_file = std::shared_ptr<SpatialRoiRecorderArtifactFile>(
            std::move(file));
        return std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>(
            new SpatialRoiRecorderVideoSanityResult(
                root, shared_file, root->artifact_root_identity(),
                shared_file->identity(), relative_path,
                static_cast<std::uint64_t>(status.st_size),
                "sha256:" +
                    orange::gui::spatial_layout::checksum::sha256_hex(
                        std::string("fake-lossless-video")),
                std::to_string(static_cast<double>(frame_count) / 100.0),
                "100/1", "1/10000", true, 0,
                static_cast<std::int64_t>((frame_count - 1) * 100),
                "mov,mp4,m4a,3gp,3g2,mj2", "hevc", "hevc@test",
                "yuvj420p", "pc", 8, "4:2:0", 4, 4, frame_count,
                std::move(samples)));
    }
};
#endif

}  // namespace orange::spatial_roi::recording

namespace {

namespace fs = std::filesystem;
namespace contract = orange::session::spatial_roi;
namespace evidence = orange::spatial_roi::recording;
namespace encoder = orange::spatial_roi::encoder;
namespace checksum = orange::gui::spatial_layout::checksum;
using json = nlohmann::json;

constexpr char kCameraSerial[] = "CAM001";
constexpr char kRoiId[] = "roi0";
constexpr char kVideoBytes[] = "fake-lossless-video";

void expect(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

fs::path make_root()
{
    std::string pattern = "/tmp/orange_spatial_roi_evidence_test_XXXXXX";
    std::vector<char> value(pattern.begin(), pattern.end());
    value.push_back('\0');
    char* result = ::mkdtemp(value.data());
    expect(result != nullptr, "mkdtemp failed");
    return fs::path(result);
}

void write_bytes(const fs::path& path, const std::string& bytes)
{
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    expect(static_cast<bool>(output), "could not create " + path.generic_string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    expect(static_cast<bool>(output), "could not write " + path.generic_string());
}

std::string read_bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    expect(static_cast<bool>(input), "could not read " + path.generic_string());
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

struct Fixture {
    fs::path recording_root;
    fs::path artifact_root;
    json plan;
    json recorder_contract;
    contract::SpatialRoiRecorderRuntimeGpuMapping gpu_mapping;
    evidence::SpatialRoiRecorderEvidenceBinding binding;
    std::shared_ptr<evidence::SpatialRoiRecorderArtifactRoot> artifact_authority;

    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(recording_root, ignored);
    }
};

std::unique_ptr<Fixture> make_fixture(
    const std::uint64_t max_media_bytes = 1024 * 1024,
    const std::uint64_t max_evidence_bytes = 8 * 1024 * 1024,
    const std::uint64_t max_frames = 32)
{
    auto fixture = std::make_unique<Fixture>();
    fixture->recording_root = make_root();
    contract::Config config = contract::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = max_frames;
    config.recording_limits.max_media_bytes_per_stream = max_media_bytes;
    config.recording_limits.max_evidence_bytes_per_stream = max_evidence_bytes;
    config.admission.max_rois_per_camera = 1;
    config.admission.max_total_rois = 1;
    config.admission.max_total_encoder_streams = 1;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 32;
    config.admission.max_total_media_bytes = max_media_bytes;
    config.admission.max_total_evidence_bytes = max_evidence_bytes;

    contract::CameraConfig camera;
    camera.camera_id = 0;
    camera.camera_serial = kCameraSerial;
    camera.native_raster = {8, 8};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group0";
    camera.layout = {"layout_v1", digest('1')};
    camera.materialization = {"materialization_v1", digest('2')};
    camera.registration = {"registration_v1", digest('3')};
    contract::RoiConfig roi;
    roi.roi_id = kRoiId;
    roi.region_id = "region0";
    roi.has_arena_id = true;
    roi.arena_id = "arena0";
    roi.required = true;
    roi.content_rect = {0, 0, 4, 4};
    roi.logical_stream_id =
        contract::expected_logical_stream_id(kCameraSerial, kRoiId);
    roi.artifact_stem = contract::expected_artifact_stem(kCameraSerial, kRoiId);
    camera.rois.push_back(std::move(roi));
    config.cameras.emplace(kCameraSerial, std::move(camera));

    contract::PlanContext context;
    context.recording_id = "evidence-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T12:00:00Z";
    context.producer_generation = "generation_001";
    std::string error;
    expect(contract::build_plan(config, context, &fixture->plan, nullptr, &error),
           "plan build failed: " + error);
    const std::string stream =
        contract::expected_logical_stream_id(kCameraSerial, kRoiId);
    fixture->gpu_mapping.analytics_gpu_by_camera_serial.emplace(kCameraSerial, 0);
    fixture->gpu_mapping.recorder_gpu_by_logical_stream_id.emplace(stream, 1);
    expect(contract::build_spatial_roi_recorder_contract(
               fixture->plan, fixture->recording_root.generic_string(),
               fixture->gpu_mapping, &fixture->recorder_contract, &error),
           "contract build failed: " + error);
    const bool binding_built =
        evidence::make_spatial_roi_recorder_evidence_binding(
            fixture->recorder_contract, fixture->plan,
            fixture->recording_root.generic_string(), fixture->gpu_mapping,
            stream, &fixture->binding, &error);
    expect(binding_built, "binding failed: " + error);
    fixture->artifact_root = fixture->binding.artifact_root;
    std::vector<std::string> allowed_artifacts;
    for (const auto& [kind, path] : fixture->binding.expected_artifacts) {
        (void)kind;
        allowed_artifacts.push_back(path);
    }
    std::unique_ptr<evidence::SpatialRoiRecorderArtifactRoot> authority;
    expect(evidence::SpatialRoiRecorderArtifactRoot::Open(
               fixture->recording_root, allowed_artifacts, &authority, &error),
           "artifact authority open failed: " + error);
    fixture->artifact_authority =
        std::shared_ptr<evidence::SpatialRoiRecorderArtifactRoot>(
            std::move(authority));
    return fixture;
}

fs::path artifact_path(const Fixture& fixture, const std::string& kind)
{
    return fixture.artifact_root / fixture.binding.expected_artifacts.at(kind);
}

evidence::SpatialRoiRecorderEvidenceWriterConfig writer_config(
    const Fixture& fixture)
{
    evidence::SpatialRoiRecorderEvidenceWriterConfig value;
    value.artifact_root = fixture.artifact_authority;
    value.evidence_relative_path =
        fixture.binding.expected_artifacts.at("evidence");
    value.manifest_relative_path =
        fixture.binding.expected_artifacts.at("evidence_manifest");
    value.max_frames =
        static_cast<std::size_t>(fixture.binding.max_frames_per_stream);
    return value;
}

void write_artifact_bytes(Fixture& fixture,
                          const std::string& kind,
                          const std::string& bytes)
{
    const std::string& relative_path =
        fixture.binding.expected_artifacts.at(kind);
    std::unique_ptr<evidence::SpatialRoiRecorderArtifactFile> file;
    std::string error;
    expect(fixture.artifact_authority->CreateFile(relative_path, &file, &error),
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

orange::spatial_roi::SpatialRoiFrameDescriptor descriptor(
    const evidence::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t index)
{
    orange::spatial_roi::SpatialRoiFrameDescriptor frame;
    frame.recording_id = binding.recording_id;
    frame.recording_identity_token = binding.recording_identity_token;
    frame.producer_generation = binding.producer_generation;
    frame.camera_id = binding.camera_id;
    frame.camera_serial = binding.camera_serial;
    frame.local_frame_id = 100 + index;
    frame.camera_frame_id = 200 + index;
    frame.recording_frame_id = 1000 + index;
    frame.roi_stream_frame_index = index;
    frame.camera_timestamp_ns = 1000000 + index;
    frame.timestamp_sys_ns = 2000000 + index;
    frame.roi_id = binding.roi_id;
    frame.region_id = binding.region_id;
    frame.arena_group_id = binding.arena_group_id;
    frame.arena_id = binding.has_arena_id ? binding.arena_id : "";
    frame.logical_stream_id = binding.logical_stream_id;
    frame.spatial_roi_plan_sha256 = binding.plan_sha256;
    frame.native_raster = {8, 8};
    frame.content_rect = {0, 0, 4, 4};
    frame.encoded_raster = {4, 4};
    frame.encoded_content_rect = {0, 0, 4, 4};
    frame.padding = {0, 0, 0, 0, 0};
    frame.source_pixel_format = "mono8";
    frame.bytes = 16;
    frame.source_gpu_id = binding.source_gpu_id;
    frame.assigned_gpu_id = binding.assigned_gpu_id;
    frame.assigned_shard_id = binding.assigned_shard_id;
    frame.routing_policy = binding.routing_policy;
    return frame;
}

evidence::SpatialRoiRecorderFrameEvidence success_frame(
    const evidence::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t index)
{
    evidence::SpatialRoiRecorderFrameEvidence value;
    value.frame = descriptor(binding, index);
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
    value.output_frame_index = index;
    value.packet_count = 1;
    value.encoded_bytes = 100 + index;
    const std::uint64_t gop_length =
        binding.encode_profile.at("gop_length").get<std::uint64_t>();
    value.keyframe = ((index - 1U) % gop_length) == 0;
    return value;
}

std::uint64_t expected_encoded_bytes(const std::uint64_t frames)
{
    // success_frame() emits one packet per frame with 100 + one-based index
    // bytes. Keep every finalization fixture tied to that exact evidence.
    return frames * 100U + (frames * (frames + 1U)) / 2U;
}

std::shared_ptr<const encoder::SpatialRoiLosslessEncoderTerminalSnapshot>
success_snapshot(const evidence::SpatialRoiRecorderEvidenceBinding& binding,
                 const std::uint64_t frames)
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
    value->nonempty_stream = frames != 0;
    value->source_release_safe = true;
    value->terminal_reason = "complete";
    value->counts.enqueue_attempted = frames;
    value->counts.enqueued = frames;
    value->counts.dequeued = frames;
    value->counts.copy_completed = frames;
    value->counts.source_releases = frames;
    value->counts.encoded_frames = frames;
    value->counts.encoded_packets = frames;
    value->counts.encoded_bytes = expected_encoded_bytes(frames);
    value->counts.frame_results_emitted = frames;
    value->counts.encoded_results = frames;
    value->counts.peak_queue_depth = frames == 0 ? 0 : 1;
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

json finalization_sidecar(const Fixture& fixture,
                          const std::uint64_t size,
                          const std::uint64_t frames)
{
    const std::uint64_t encoded_bytes = expected_encoded_bytes(frames);
    return {
        {"schema_id", "orange.video_container_finalization"},
        {"schema_version", 2},
        {"generated_at_utc", "2026-08-31T12:01:00Z"},
        {"status", "complete"},
        {"terminal", true},
        {"video_path", fixture.binding.expected_artifacts.at("video")},
        {"sidecar_path", fixture.binding.expected_artifacts.at("finalization")},
        {"recording_fps", 100},
        {"packet_writes", {
            {"submissions_accepted", frames},
            {"submission_bytes_accepted", encoded_bytes},
            {"submissions_rejected", 0},
            {"write_attempts", frames},
            {"packets_written", frames},
            {"bytes_written", encoded_bytes},
            {"write_failures", 0},
            {"first_write_error_code", nullptr},
            {"writer_error_latched", false},
            {"muxer_flush_attempted", true},
            {"muxer_flush_succeeded", true},
            {"muxer_flush_error_code", nullptr},
            {"muxer_flush_error", nullptr},
            {"complete", true},
        }},
        {"container", {
            {"header_written", true}, {"trailer_attempted", true},
            {"trailer_written", true}, {"output_close_attempted", true},
            {"output_closed", true}, {"finalized", true},
            {"trailer_error_code", nullptr}, {"trailer_error", nullptr},
            {"output_close_error_code", nullptr},
            {"output_close_error", nullptr}, {"file_size_bytes", size},
            {"file_size_error", nullptr},
        }},
        {"quicktime_full_frame_rate_playback_intent", {
            {"key", "com.apple.quicktime.full-frame-rate-playback-intent"},
            {"requested_value", 1}, {"required_data_type", "UInt8"},
            {"quicktime_data_atom_type", 22}, {"patch_attempted", true},
            {"patch_applied", true}, {"error", nullptr},
        }},
    };
}

json sanity_sidecar(const Fixture& fixture,
                    const std::uint64_t size,
                    const std::uint64_t frames)
{
    struct stat video_status {};
    expect(::stat(artifact_path(fixture, "video").c_str(), &video_status) == 0,
           "could not stat video for sanity receipt");
    json samples = json::array();
    for (std::uint64_t index = 0; index < std::min<std::uint64_t>(frames, 2);
         ++index) {
        samples.push_back({
            {"requested_frame_index", index}, {"mean", 64.0},
            {"stddev", 10.0}, {"min", 0}, {"max", 200},
            {"black_fraction_lt8", 0.1}, {"white_fraction_gt247", 0.0},
            {"decoded_bytes", 16},
        });
    }
    return {
        {"schema_id", "orange.spatial_roi_recorder.video_sanity"},
        {"schema_version", 1},
        {"state", "pending_manifest"},
        {"certifying", false},
        {"requires_finalized_evidence_manifest", true},
        {"commit_marker", "evidence_manifest"},
        {"commit_marker_state", "required_finalized"},
        {"logical_stream_id", fixture.binding.logical_stream_id},
        {"frame_count", frames},
        {"video_path", fixture.binding.expected_artifacts.at("video")},
        {"video_size_bytes", size},
        {"video_sha256",
         "sha256:" + checksum::sha256_hex(
                         std::string(kVideoBytes, sizeof(kVideoBytes) - 1))},
        {"video_device", static_cast<std::uint64_t>(video_status.st_dev)},
        {"video_inode", static_cast<std::uint64_t>(video_status.st_ino)},
        {"content_checked", true}, {"content_valid", true},
        {"status", "pass"}, {"width", 4}, {"height", 4},
        {"nb_frames", frames},
        {"container", {{"size", std::to_string(size)},
                        {"duration", std::to_string(
                                         static_cast<double>(frames) / 100.0)}}},
        {"container_name", "mov,mp4,m4a,3gp,3g2,mj2"},
        {"codec", "hevc"},
        {"decoder", "hevc@test"},
        {"timeline", {
            {"frame_rate", "100/1"},
            {"time_base", "1/10000"},
            {"has_decoded_pts", true},
            {"first_decoded_pts", 0},
            {"last_decoded_pts", static_cast<std::int64_t>(
                                     frames == 0 ? 0 : (frames - 1) * 100)},
        }},
        {"pixel_semantics", {
            {"pixel_format", "yuvj420p"},
            {"color_range", "pc"},
            {"bit_depth", 8},
            {"chroma_subsampling", "4:2:0"},
        }},
        {"sampled_frame_count", samples.size()}, {"mean_luma", 64.0},
        {"max_stddev", 10.0}, {"max_black_fraction_lt8", 0.1},
        {"thresholds", {{"max_black_fraction_lt8", 0.98},
                        {"min_max_stddev", 5.0}}},
        {"sampled_frames", std::move(samples)},
    };
}

json keyframe_summary(
    const evidence::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t frames)
{
    const std::uint64_t gop_length =
        binding.encode_profile.at("gop_length").get<std::uint64_t>();
    const std::uint64_t keyframes =
        frames == 0 ? 0 : 1U + ((frames - 1U) / gop_length);
    const std::string policy_name = gop_length == 1
        ? "all_frames_idr"
        : "fixed_gop_" + std::to_string(gop_length) + "_idr";
    return {
        {"schema_id", "orange.spatial_roi_keyframe_summary"},
        {"schema_version", 1},
        {"terminal", true},
        {"codec", "hevc"},
        {"fps", 100},
        {"total_frames", frames},
        {"frame_index_sequence", {
            {"first", frames == 0 ? json(nullptr) : json(0)},
            {"last", frames == 0 ? json(nullptr) : json(frames - 1)},
            {"zero_based_contiguous", true},
        }},
        {"keyframe_policy", {
            {"name", policy_name},
            {"keyframe_frames", keyframes},
            {"non_keyframe_frames", frames - keyframes},
            {"satisfied", true},
        }},
    };
}

void write_artifacts(Fixture& fixture,
                     const std::uint64_t frames,
                     const std::string* raw_keyframes = nullptr,
                     const std::uint64_t* declared_size = nullptr,
                     const std::uint64_t* sanity_frames = nullptr)
{
    const std::uint64_t size = sizeof(kVideoBytes) - 1;
    write_artifact_bytes(fixture, "video", kVideoBytes);
    std::ostringstream metadata;
    metadata << "recording_frame_id,roi_stream_frame_index,output_frame_index,"
                "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,"
                "assigned_gpu_id,assigned_shard_id\n";
    for (std::uint64_t index = 1; index <= frames; ++index) {
        metadata << 1000 + index << ',' << index << ',' << index << ','
                 << 1000000 + index << ',' << 2000000 + index << ','
                 << fixture.binding.source_gpu_id << ','
                 << fixture.binding.assigned_gpu_id << ','
                 << fixture.binding.assigned_shard_id << '\n';
    }
    write_artifact_bytes(fixture, "metadata", metadata.str());
    if (raw_keyframes) {
        write_artifact_bytes(fixture, "keyframes", *raw_keyframes);
    } else {
        write_artifact_bytes(fixture, "keyframes",
                             keyframe_summary(fixture.binding, frames).dump() + "\n");
    }
    const auto candidate_base = [&](const char* schema_id) {
        return json{
            {"schema_id", schema_id},
            {"schema_version", 1},
            {"state", "pending_manifest"},
            {"certifying", false},
            {"requires_finalized_evidence_manifest", true},
            {"commit_marker", "evidence_manifest"},
            {"commit_marker_state", "required_finalized"},
            {"logical_stream_id", fixture.binding.logical_stream_id},
            {"frame_count", frames},
        };
    };
    write_artifact_bytes(
        fixture, "perf",
        "metric,value\n"
        "schema_id,orange.spatial_roi_recorder.perf\n"
        "schema_version,1\n"
        "state,pending_manifest\n"
        "certifying,false\n"
        "requires_finalized_evidence_manifest,true\n"
        "commit_marker,evidence_manifest\n"
        "commit_marker_state,required_finalized\n"
        "logical_stream_id," + fixture.binding.logical_stream_id +
        "\nframe_count," + std::to_string(frames) + "\n");
    auto summary = candidate_base("orange.spatial_roi_recorder.summary");
    summary["status"] = "pending_manifest";
    write_artifact_bytes(
        fixture, "summary", summary.dump() + "\n");
    auto status = candidate_base("orange.spatial_roi_recorder.status");
    status["terminal"] = false;
    write_artifact_bytes(
        fixture, "status", status.dump() + "\n");
    write_artifact_bytes(
        fixture, "recorder_log",
        "schema_id=orange.spatial_roi_recorder.log schema_version=1"
        " state=pending_manifest certifying=false"
        " requires_finalized_evidence_manifest=true"
        " commit_marker=evidence_manifest"
        " commit_marker_state=required_finalized logical_stream_id=" +
        fixture.binding.logical_stream_id + " frame_count=" +
        std::to_string(frames) + "\n");
    const auto transport = candidate_base("orange.spatial_roi_recorder.transport");
    write_artifact_bytes(
        fixture, "transport_sidecar", transport.dump() + "\n");
    write_artifact_bytes(
        fixture, "finalization",
        finalization_sidecar(fixture,
            declared_size ? *declared_size : size, frames).dump() + "\n");
    write_artifact_bytes(
        fixture, "video_sanity",
        sanity_sidecar(fixture, size,
            sanity_frames ? *sanity_frames : frames).dump() + "\n");
}

evidence::SpatialRoiRecorderFinalizeRequest complete_request(
    const Fixture& fixture, const std::uint64_t frames)
{
    evidence::SpatialRoiRecorderFinalizeRequest request;
    request.encoder_terminal_snapshot = success_snapshot(fixture.binding, frames);
#if defined(ORANGE_SPATIAL_ROI_VIDEO_SANITY_TESTING)
    std::string capability_error;
    request.video_sanity_result =
        evidence::SpatialRoiRecorderVideoSanityTestFactory::Create(
            fixture.artifact_authority,
            fixture.binding.expected_artifacts.at("video"), frames,
            &capability_error);
#endif
    for (const std::string kind : {
             "transport_sidecar", "recorder_log", "finalization",
             "video_sanity", "status", "summary", "perf", "keyframes",
             "metadata", "video"}) {
        request.artifacts.push_back(
            {kind, fixture.binding.expected_artifacts.at(kind)});
    }
    return request;
}

std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter> open_writer(
    const Fixture& fixture)
{
    std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter> writer;
    std::string error;
    expect(evidence::SpatialRoiRecorderEvidenceWriter::Open(
               writer_config(fixture), fixture.binding, &writer, &error),
           "writer open failed: " + error);
    return writer;
}

void append_successes(evidence::SpatialRoiRecorderEvidenceWriter* writer,
                      const Fixture& fixture,
                      const std::uint64_t frames)
{
    std::string error;
    for (std::uint64_t index = 1; index <= frames; ++index) {
        expect(writer->AppendFrame(success_frame(fixture.binding, index), &error),
               "frame append failed: " + error);
    }
}

json finalize_success(Fixture& fixture,
    std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter>* retained = nullptr)
{
    write_artifacts(fixture, 2);
    auto writer = open_writer(fixture);
    append_successes(writer.get(), fixture, 2);
    json manifest;
    std::string error;
    const bool finalized =
        writer->Finalize(complete_request(fixture, 2), &manifest, &error);
    expect(finalized,
           "finalize failed: " + error + " writer=" + writer->error());
    if (retained) *retained = std::move(writer);
    return manifest;
}

void test_authoritative_binding()
{
    auto fixture = make_fixture();
    expect(fixture->binding.contract_schema_version ==
               contract::kSpatialRoiRecorderContractSchemaVersion &&
               fixture->binding.expected_artifacts.size() == 12 &&
               fixture->binding.max_frames_per_stream == 32 &&
               fixture->binding.max_media_bytes_per_stream == 1024 * 1024 &&
               fixture->binding.max_evidence_bytes_per_stream == 8 * 1024 * 1024,
           "authoritative binding projection is incomplete");
    evidence::SpatialRoiRecorderEvidenceBinding ignored;
    std::string error;
    expect(!evidence::make_spatial_roi_recorder_evidence_binding(
               fixture->recorder_contract, fixture->plan,
               (fixture->recording_root / "wrong").generic_string(),
               fixture->gpu_mapping, fixture->binding.logical_stream_id,
               &ignored, &error),
           "substituted recording root was accepted");
    auto mapping = fixture->gpu_mapping;
    mapping.recorder_gpu_by_logical_stream_id.at(
        fixture->binding.logical_stream_id) = 9;
    expect(!evidence::make_spatial_roi_recorder_evidence_binding(
               fixture->recorder_contract, fixture->plan,
               fixture->recording_root.generic_string(), mapping,
               fixture->binding.logical_stream_id, &ignored, &error),
           "substituted runtime GPU mapping was accepted");
    auto config = writer_config(*fixture);
    --config.max_frames;
    std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter> writer;
    expect(!evidence::SpatialRoiRecorderEvidenceWriter::Open(
               config, fixture->binding, &writer, &error),
           "caller-controlled frame bound was accepted");
    auto tiny = make_fixture(1024 * 1024, 64, 1);
    expect(!evidence::SpatialRoiRecorderEvidenceWriter::Open(
               writer_config(*tiny), tiny->binding, &writer, &error),
           "authenticated evidence byte bound was ignored");

    auto other = make_fixture();
    config = writer_config(*fixture);
    config.artifact_root = other->artifact_authority;
    expect(!evidence::SpatialRoiRecorderEvidenceWriter::Open(
               config, fixture->binding, &writer, &error),
           "artifact authority from another recording root was accepted");

    std::unique_ptr<evidence::SpatialRoiRecorderArtifactRoot> incomplete;
    expect(evidence::SpatialRoiRecorderArtifactRoot::OpenExisting(
               fixture->recording_root,
               {fixture->binding.expected_artifacts.at("evidence"),
                fixture->binding.expected_artifacts.at("evidence_manifest")},
               &incomplete, &error),
           "incomplete authority fixture could not be opened: " + error);
    config = writer_config(*fixture);
    config.artifact_root =
        std::shared_ptr<evidence::SpatialRoiRecorderArtifactRoot>(
            std::move(incomplete));
    expect(!evidence::SpatialRoiRecorderEvidenceWriter::Open(
               config, fixture->binding, &writer, &error),
           "artifact authority with an incomplete contract allow-list was accepted");
}

void test_roundtrip_and_complete_adoption()
{
    auto fixture = make_fixture();
    std::unique_ptr<evidence::SpatialRoiRecorderEvidenceWriter> writer;
    const json manifest = finalize_success(*fixture, &writer);
    std::string error;
    expect(manifest.at("encoder_terminal").at("writer")
               .at("close_finalization_validated") == true,
           "encoder close truth was not persisted");
    expect(manifest.at("encoder_terminal").at("snapshot_schema") ==
               "spatial_roi_lossless_terminal_v2" &&
               manifest.at("encoder_terminal").at("counts").size() == 28 &&
               manifest.at("encoder_terminal").at("counts")
                   .at("enqueue_attempted") == 2 &&
               manifest.at("encoder_terminal").at("counts")
                   .at("copy_completed") == 2 &&
               manifest.at("encoder_terminal").at("counts")
                   .at("source_releases") == 2 &&
               manifest.at("encoder_terminal").at("counts")
                   .at("peak_queue_depth") == 1 &&
               manifest.at("encoder_terminal")
                   .at("all_enqueue_attempts_accounted") == true &&
               manifest.at("encoder_terminal").at("nonempty_stream") == true &&
               manifest.at("encoder_terminal").at("artifacts_sealed") == true &&
               manifest.at("encoder_terminal").at("counts")
                   .at("artifacts_sealed") == true &&
               manifest.at("encoder_terminal").at("writer")
                   .at("video_size_limit_failures") == 0,
           "encoder terminal-v2 durability truth was not persisted");
    const json finalization = json::parse(read_bytes(
        artifact_path(*fixture, "finalization")));
    const json& packet_writes = finalization.at("packet_writes");
    expect(finalization.size() == 11 &&
               finalization.at("schema_version") == 2 &&
               packet_writes.size() == 14 &&
               packet_writes.at("submissions_accepted") == 2 &&
               packet_writes.at("submission_bytes_accepted") ==
                   expected_encoded_bytes(2) &&
               packet_writes.at("submissions_rejected") == 0 &&
               packet_writes.at("write_attempts") == 2 &&
               packet_writes.at("packets_written") == 2 &&
               packet_writes.at("bytes_written") == expected_encoded_bytes(2) &&
               packet_writes.at("write_failures") == 0 &&
               packet_writes.at("first_write_error_code").is_null() &&
               packet_writes.at("writer_error_latched") == false &&
               packet_writes.at("muxer_flush_attempted") == true &&
               packet_writes.at("muxer_flush_succeeded") == true &&
               packet_writes.at("muxer_flush_error_code").is_null() &&
               packet_writes.at("muxer_flush_error").is_null() &&
               packet_writes.at("complete") == true,
           "schema-v2 packet proof is not exact or does not match frame evidence");
    expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, manifest, &error),
           "manifest validation failed: " + error);
    json reread;
    expect(evidence::read_and_validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority,
               fixture->binding.expected_artifacts.at("evidence_manifest"),
               fixture->binding, &reread, &error),
           "fd manifest read failed: " + error);
    expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, manifest, &error),
           "retained artifact-root recovery authority failed: " + error);
    auto retry = complete_request(*fixture, 2);
    std::reverse(retry.artifacts.begin(), retry.artifacts.end());
    retry.terminal_reason = "complete";
    expect(writer->Finalize(retry, &reread, &error),
           "canonical reordered retry failed: " + error);
    writer.reset();
    expect(evidence::SpatialRoiRecorderEvidenceWriter::Open(
               writer_config(*fixture), fixture->binding, &writer, &error) &&
               writer->finalized(),
           "complete immutable pair was not adopted: " + error);
    expect(writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
           "adopted pair was not idempotent: " + error);
    json changed = manifest;
    changed["terminal"]["reason"] = "forged";
    expect(!evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, changed, &error),
           "manifest receipt mutation was accepted");
}

void test_complete_requires_decoder_probe_capability()
{
    auto fixture = make_fixture();
    write_artifacts(*fixture, 2);
    auto writer = open_writer(*fixture);
    append_successes(writer.get(), *fixture, 2);
    auto request = complete_request(*fixture, 2);
    expect(request.video_sanity_result != nullptr,
           "test fixture did not create its descriptor-bound probe capability");
    request.video_sanity_result.reset();
    std::string error;
    expect(!writer->Finalize(request, nullptr, &error),
           "complete finalization trusted a forgeable disk sidecar without a decoder probe");
    expect(error.find("decoder-probe capability") != std::string::npos,
           "missing decoder-probe capability failed for the wrong reason: " + error);
}

void test_encoder_terminal_v2_completion_truth()
{
    const auto rejected = [](auto mutation, const std::string& label) {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        auto request = complete_request(*fixture, 2);
        auto snapshot =
            std::make_shared<encoder::SpatialRoiLosslessEncoderTerminalSnapshot>(
                *request.encoder_terminal_snapshot);
        mutation(*snapshot);
        request.encoder_terminal_snapshot = snapshot;
        std::string error;
        expect(!writer->Finalize(request, nullptr, &error),
               label + " was accepted as completed encoder evidence");
    };

    rejected([](auto& snapshot) { snapshot.artifacts_sealed = false; },
             "unsealed terminal snapshot");
    rejected([](auto& snapshot) {
                 snapshot.counts.artifacts_sealed = false;
             },
             "unsealed terminal count");
    rejected([](auto& snapshot) {
                 snapshot.artifacts_sealed = false;
                 snapshot.counts.artifacts_sealed = false;
             },
             "consistently unsealed terminal state");
    rejected([](auto& snapshot) {
                 snapshot.writer.video_size_limit_failures = 1;
             },
             "nonzero video-size-limit failure count");
    rejected([](auto& snapshot) { ++snapshot.counts.enqueue_attempted; },
             "unaccounted enqueue attempt");
    rejected([](auto& snapshot) {
                 ++snapshot.counts.enqueue_attempted;
                 ++snapshot.counts.rejected;
             },
             "rejected enqueue attempt");
    rejected([](auto& snapshot) {
                 ++snapshot.counts.enqueue_attempted;
                 ++snapshot.counts.rejected;
                 ++snapshot.counts.queue_overflows;
             },
             "input queue overflow");
    rejected([](auto& snapshot) { --snapshot.counts.dequeued; },
             "incomplete dequeue count");
    rejected([](auto& snapshot) { --snapshot.counts.copy_completed; },
             "incomplete copy count");
    rejected([](auto& snapshot) { --snapshot.counts.source_releases; },
             "incomplete source release count");
    rejected([](auto& snapshot) { ++snapshot.counts.copy_failures; },
             "copy failure count");
    rejected([](auto& snapshot) { ++snapshot.counts.writer_queue_overflows; },
             "writer queue overflow count");
    rejected([](auto& snapshot) { snapshot.counts.peak_queue_depth = 0; },
             "missing queue high-water evidence");
    rejected([](auto& snapshot) { snapshot.counts.peak_queue_depth = 3; },
             "impossible queue high-water evidence");
    rejected([](auto& snapshot) {
                 snapshot.all_enqueue_attempts_accounted = false;
             },
             "unaccounted enqueue terminal flag");
    rejected([](auto& snapshot) { snapshot.nonempty_stream = false; },
             "empty-stream terminal flag");
}

void test_leaf_replacement_is_not_redirected()
{
    auto fixture = make_fixture();
    const json manifest = finalize_success(*fixture);
    const std::string evidence_relative =
        fixture->binding.expected_artifacts.at("evidence");
    std::unique_ptr<evidence::SpatialRoiRecorderArtifactFile> retained;
    std::string error;
    expect(fixture->artifact_authority->OpenExistingFile(
               evidence_relative,
               evidence::SpatialRoiRecorderArtifactFileAccess::kReadOnly,
               &retained, &error),
           "could not retain evidence handle: " + error);
    expect(retained->artifact_root_identity() ==
               fixture->artifact_authority->artifact_root_identity() &&
               retained->relative_path() == evidence_relative,
           "retained evidence handle lost its root/path identity");

    const fs::path evidence_path = artifact_path(*fixture, "evidence");
    const fs::path displaced_path = evidence_path.string() + ".displaced";
    fs::rename(evidence_path, displaced_path);
    write_bytes(evidence_path, "forged replacement\n");
    expect(!retained->VerifyCurrentBinding(&error),
           "retained evidence handle accepted a replacement leaf");
    expect(!evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, manifest, &error),
           "replacement evidence leaf redirected manifest validation");
}

void test_frame_truth_tables()
{
    const auto rejected = [](auto mutation, const std::string& label) {
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        auto frame = success_frame(fixture->binding, 1);
        mutation(frame);
        std::string error;
        expect(!writer->AppendFrame(frame, &error), label + " was accepted");
    };
    rejected([](auto& f) { f.frame.encoded_raster.width = 6; }, "geometry drift");
    rejected([](auto& f) { f.output_frame_index = 2; }, "sparse output index");
    rejected([](auto& f) { f.encode_status = "failed"; f.output_frame_index = 0; },
             "nonencoded packet truth");
    rejected([](auto& f) { f.ack_reason = "reason"; }, "accepted ACK reason");
    rejected([](auto& f) { f.release_sent = false; }, "ACK without RELEASE");
    rejected([](auto& f) { f.frame.roi_stream_frame_index = 2;
                            f.output_frame_index = 2; },
             "non-one-based ROI output");
}

void test_lifecycle_json_roundtrip_and_counters()
{
    auto fixture = make_fixture();
    auto writer = open_writer(*fixture);

    auto ack_failure = success_frame(fixture->binding, 1);
    ack_failure.ack_sent = false;
    ack_failure.ack_accepted = true;
    ack_failure.ack_error = "EPIPE";
    ack_failure.release_attempted = false;
    ack_failure.release_sent = false;
    ack_failure.release_reason.clear();
    ack_failure.encode_status = "encoded";
    ack_failure.output_frame_index = 1;
    expect(writer->AppendFrame(ack_failure), "ACK failure append failed");

    auto release_failure = success_frame(fixture->binding, 2);
    release_failure.release_sent = false;
    release_failure.release_reason = "source_detached";
    release_failure.release_error = "EPIPE";
    release_failure.encode_status = "encoded";
    release_failure.output_frame_index = 2;
    expect(writer->AppendFrame(release_failure), "RELEASE failure append failed");

    auto rejection = success_frame(fixture->binding, 3);
    rejection.dispatch_admitted = false;
    rejection.dispatch_reason = "queue_full";
    rejection.ack_accepted = false;
    rejection.ack_reason = "queue_full";
    rejection.release_reason = "source_rejected";
    rejection.encode_status = "not_attempted";
    rejection.output_frame_index = 0;
    rejection.packet_count = 0;
    rejection.encoded_bytes = 0;
    rejection.keyframe = false;
    expect(writer->AppendFrame(rejection), "safe rejection append failed");

    evidence::SpatialRoiRecorderFinalizeRequest request;
    request.terminal_state = "failed";
    request.terminal_reason = "transport failure";
    json manifest;
    std::string error;
    expect(writer->Finalize(request, &manifest, &error),
           "lifecycle finalization failed: " + error);
    const auto evidence_path = artifact_path(*fixture, "evidence");
    std::istringstream lines(read_bytes(evidence_path));
    std::string line;
    json header;
    json first;
    json second;
    json third;
    json terminal;
    expect(static_cast<bool>(std::getline(lines, line)), "missing evidence header");
    header = json::parse(line);
    expect(static_cast<bool>(std::getline(lines, line)), "missing ACK failure row");
    first = json::parse(line);
    expect(static_cast<bool>(std::getline(lines, line)), "missing RELEASE failure row");
    second = json::parse(line);
    expect(static_cast<bool>(std::getline(lines, line)), "missing rejection row");
    third = json::parse(line);
    expect(static_cast<bool>(std::getline(lines, line)), "missing terminal row");
    terminal = json::parse(line);
    expect(first.at("dispatch").at("admitted") == true &&
               first.at("ack").at("attempted") == true &&
               first.at("ack").at("sent") == false &&
               first.at("ack").at("accepted") == true &&
               first.at("ack").at("error") == "EPIPE" &&
               first.at("release").at("attempted") == false,
           "ACK write failure lifecycle JSON was not retained");
    expect(second.at("release").at("attempted") == true &&
               second.at("release").at("sent") == false &&
               second.at("release").at("reason") == "source_detached" &&
               second.at("release").at("error") == "EPIPE",
           "RELEASE write failure lifecycle JSON was not retained");
    expect(third.at("dispatch").at("admitted") == false &&
               third.at("dispatch").at("reason") == "queue_full" &&
               third.at("ack").at("attempted") == true &&
               third.at("ack").at("sent") == true &&
               third.at("ack").at("accepted") == false &&
               third.at("release").at("reason") == "source_rejected",
           "safe rejection lifecycle JSON was not retained");
    const auto& counts = terminal.at("counts");
    expect(counts.at("dispatch_admitted") == 2 &&
               counts.at("dispatch_rejected") == 1 &&
               counts.at("ack_attempted") == 3 &&
               counts.at("ack_sent") == 2 &&
               counts.at("ack_accepted") == 2 &&
               counts.at("release_attempted") == 2 &&
               counts.at("release_sent") == 1 &&
               counts.at("ack_write_failures") == 1 &&
               counts.at("release_write_failures") == 1 &&
               counts.at("lifecycle_failures") == 3,
           "lifecycle counters were not exact: " + counts.dump());
    expect(header.at("record_type") == "stream_header" &&
               terminal.at("record_type") == "stream_terminal",
           "evidence JSONL record order was not closed");
    std::string reread_error;
    expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, manifest,
               &reread_error),
           "lifecycle JSON round-trip did not validate: " + reread_error);
}

void test_closed_artifact_proofs()
{
    {
        auto fixture = make_fixture();
        const std::string duplicate =
            "{\"schema_id\":\"orange.spatial_roi_keyframe_summary\","
            "\"schema_version\":1,\"terminal\":true,\"codec\":\"hevc\","
            "\"fps\":100,\"total_frames\":2,\"total_frames\":2,"
            "\"frame_index_sequence\":{\"first\":0,\"last\":1,"
            "\"zero_based_contiguous\":true},\"keyframe_policy\":{"
            "\"name\":\"fixed_gop_25_idr\",\"keyframe_frames\":1,"
            "\"non_keyframe_frames\":1,\"satisfied\":true}}\n";
        write_artifacts(*fixture, 2, &duplicate);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "duplicate-key keyframe sidecar was accepted");
    }
    using KeyframeMutation = void (*)(json&);
    for (const KeyframeMutation mutation : {
             +[](json& value) { value["codec"] = "h264"; },
             +[](json& value) { value["fps"] = 99; },
             +[](json& value) {
                 value["frame_index_sequence"]["last"] = 2;
             },
             +[](json& value) {
                 value["keyframe_policy"]["non_keyframe_frames"] = 0;
             }}) {
        auto fixture = make_fixture();
        json invalid = keyframe_summary(fixture->binding, 2);
        mutation(invalid);
        const std::string invalid_bytes = invalid.dump() + "\n";
        write_artifacts(*fixture, 2, &invalid_bytes);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "keyframe codec/fps drift was accepted");
    }
    {
        auto fixture = make_fixture();
        const std::uint64_t wrong = 1;
        write_artifacts(*fixture, 2, nullptr, &wrong);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "finalization video-size lie was accepted");
    }
    struct PacketProofMutation {
        const char* label;
        void (*apply)(json&);
    };
    const PacketProofMutation packet_proof_mutations[] = {
        {"schema-v1 downgrade", +[](json& value) {
             value["schema_version"] = 1;
         }},
        {"missing packet proof", +[](json& value) {
             value.erase("packet_writes");
         }},
        {"unexpected packet-proof field", +[](json& value) {
             value["packet_writes"]["unexpected"] = true;
         }},
        {"internally consistent wrong packet count", +[](json& value) {
             json& proof = value["packet_writes"];
             proof["submissions_accepted"] = 1;
             proof["write_attempts"] = 1;
             proof["packets_written"] = 1;
         }},
        {"rejected packet submission", +[](json& value) {
             value["packet_writes"]["submissions_rejected"] = 1;
         }},
        {"failed packet write", +[](json& value) {
             json& proof = value["packet_writes"];
             proof["write_failures"] = 1;
             proof["first_write_error_code"] = -5;
         }},
        {"failed muxer flush", +[](json& value) {
             json& proof = value["packet_writes"];
             proof["muxer_flush_succeeded"] = false;
             proof["muxer_flush_error_code"] = -5;
             proof["muxer_flush_error"] = "injected flush failure";
             proof["complete"] = false;
         }},
        {"internally consistent wrong byte count", +[](json& value) {
             json& proof = value["packet_writes"];
             proof["submission_bytes_accepted"] = 204;
             proof["bytes_written"] = 204;
         }},
    };
    for (const PacketProofMutation& mutation : packet_proof_mutations) {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        json invalid = json::parse(read_bytes(
            artifact_path(*fixture, "finalization")));
        mutation.apply(invalid);
        write_bytes(artifact_path(*fixture, "finalization"),
                    invalid.dump() + "\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               std::string("mutated schema-v2 packet proof was accepted: ") +
                   mutation.label);
    }
    {
        auto fixture = make_fixture();
        const std::uint64_t wrong = 1;
        write_artifacts(*fixture, 2, nullptr, nullptr, &wrong);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "video sanity frame-count lie was accepted");
    }
    using SanityMutation = void (*)(json&);
    for (const SanityMutation mutation : {
             +[](json& value) { value["video_sha256"] = digest('f'); },
             +[](json& value) {
                 value["video_inode"] =
                     value.at("video_inode").get<std::uint64_t>() + 1;
             },
             +[](json& value) {
                 value["pixel_semantics"]["pixel_format"] = "yuv420p10le";
             },
             +[](json& value) {
                 value["timeline"]["frame_rate"] = "99/1";
             },
             +[](json& value) {
                 value["timeline"]["last_decoded_pts"] = 1000;
             },
             +[](json& value) { value["mean_luma"] = 65.0; },
             +[](json& value) {
                 value["video_path"] = "/tmp/substituted.mp4";
             }}) {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        json invalid = json::parse(read_bytes(
            artifact_path(*fixture, "video_sanity")));
        mutation(invalid);
        write_bytes(artifact_path(*fixture, "video_sanity"),
                    invalid.dump() + "\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "mutated video sanity receipt was accepted");
    }
    {
        auto fixture = make_fixture(8);
        write_artifacts(*fixture, 2);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "video exceeded authenticated media bound");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        fs::remove(artifact_path(*fixture, "metadata"));
        fs::create_hard_link(artifact_path(*fixture, "video"),
                             artifact_path(*fixture, "metadata"));
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "artifact inode alias was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(
            artifact_path(*fixture, "metadata"),
            "recording_frame_id,roi_stream_frame_index,output_frame_index,"
            "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,"
            "assigned_gpu_id,assigned_shard_id\n"
            "1001,1,1,1000001,2000001,0,1,0\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "metadata CSV row-count mismatch was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(
            artifact_path(*fixture, "metadata"),
            "recording_frame_id,roi_stream_frame_index,output_frame_index,"
            "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,"
            "assigned_gpu_id,assigned_shard_id\n"
            "9001,1,1,8000001,7000001,0,1,0\n"
            "9002,2,2,8000002,7000002,0,1,0\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "monotonic substituted metadata identities were accepted");
        expect(error.find("exactly match frame evidence") != std::string::npos,
               "substituted metadata failed for the wrong reason: " + error);
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(artifact_path(*fixture, "perf"),
                    "metric,value\nframes,1\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "perf CSV frame-count mismatch was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(
            artifact_path(*fixture, "summary"),
            json{{"status", "complete"},
                 {"frame_count", 1},
                 {"logical_stream_id", fixture->binding.logical_stream_id}}.dump() +
                "\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "summary frame-count mismatch was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(
            artifact_path(*fixture, "status"),
            json{{"terminal", false},
                 {"state", "pending"},
                 {"frame_count", 2},
                 {"logical_stream_id", fixture->binding.logical_stream_id}}.dump() +
                "\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "nonterminal status sidecar was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(
            artifact_path(*fixture, "transport_sidecar"),
            json{{"terminal", true},
                 {"state", "complete"},
                 {"frame_count", 2},
                 {"logical_stream_id", "wrong_stream"}}.dump() + "\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "wrong-stream transport sidecar was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        write_bytes(artifact_path(*fixture, "recorder_log"), "incomplete\n");
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "recorder log without terminal completion was accepted");
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        fs::resize_file(artifact_path(*fixture, "metadata"),
                        256 + fixture->binding.max_frames_per_stream * 256 + 1);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "metadata artifact exceeded its frame-derived hash bound");
    }
    {
        auto fixture = make_fixture(1024 * 1024, 16 * 1024, 32);
        write_artifacts(*fixture, 2);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        std::string error;
        expect(!writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "aggregate evidence budget was multiplied across sidecars");
        expect(error.find("aggregate") != std::string::npos,
               "aggregate evidence rejection reason was lost: " + error);
    }
}

void test_zero_frame_completion_fails_closed()
{
    auto fixture = make_fixture();
    write_artifacts(*fixture, 0);
    auto writer = open_writer(*fixture);
    std::string error;
    expect(!writer->Finalize(complete_request(*fixture, 0), nullptr, &error),
           "zero-frame stream was accepted as complete");
    expect(error.find("nonempty") != std::string::npos ||
               error.find("successful evidence") != std::string::npos,
           "zero-frame completion failed for the wrong reason: " + error);
}

void test_duplicate_json_keys()
{
    auto fixture = make_fixture();
    const json manifest = finalize_success(*fixture);
    const fs::path manifest_path = artifact_path(*fixture, "evidence_manifest");
    const std::string original = read_bytes(manifest_path);
    std::string duplicate = original;
    duplicate.insert(duplicate.find('{') + 1, "\"schema_id\":\"forged\",");
    write_bytes(manifest_path, duplicate);
    json ignored;
    std::string error;
    expect(!evidence::read_and_validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority,
               fixture->binding.expected_artifacts.at("evidence_manifest"),
               fixture->binding, &ignored, &error),
           "duplicate-key manifest was accepted");
    write_bytes(manifest_path, original);
    const fs::path evidence_path = artifact_path(*fixture, "evidence");
    duplicate = read_bytes(evidence_path);
    duplicate.insert(duplicate.find('{') + 1,
                     "\"record_type\":\"forged\",");
    write_bytes(evidence_path, duplicate);
    expect(!evidence::validate_spatial_roi_recorder_finalized_manifest(
               fixture->artifact_authority, fixture->binding, manifest, &error),
           "duplicate-key evidence row was accepted");
}

void test_crash_adoption_and_root_swap()
{
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        const auto request = complete_request(*fixture, 2);
        const fs::path manifest_path = artifact_path(*fixture, "evidence_manifest");
        fs::create_directory(manifest_path);
        std::string error;
        expect(!writer->Finalize(request, nullptr, &error),
               "manifest publication collision unexpectedly succeeded");
        expect(fs::is_regular_file(artifact_path(*fixture, "evidence")),
               "evidence was not durable before manifest publication");
        writer.reset();
        fs::remove(manifest_path);
        expect(evidence::SpatialRoiRecorderEvidenceWriter::Open(
                   writer_config(*fixture), fixture->binding, &writer, &error),
               "evidence-only residue was not adopted: " + error);
        expect(!writer->finalized(), "evidence-only residue invented a manifest");
        expect(writer->Finalize(request, nullptr, &error),
               "identical evidence-only retry failed: " + error);
    }
    {
        auto fixture = make_fixture();
        write_artifacts(*fixture, 2);
        auto writer = open_writer(*fixture);
        append_successes(writer.get(), *fixture, 2);
        const fs::path original = fixture->artifact_root;
        const fs::path moved = fixture->recording_root / "opened-artifact-root";
        fs::rename(original, moved);
        fs::create_directories(original);
        std::string error;
        expect(writer->Finalize(complete_request(*fixture, 2), nullptr, &error),
               "fd-rooted finalize failed after root swap: " + error);
        expect(fs::is_regular_file(
                   moved / fixture->binding.expected_artifacts.at("evidence")) &&
                   !fs::exists(original /
                       fixture->binding.expected_artifacts.at("evidence")),
               "root-path replacement captured evidence publication");
    }
}

void test_failed_terminal()
{
    {
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        evidence::SpatialRoiRecorderFrameEvidence frame;
        frame.frame = descriptor(fixture->binding, 1);
        frame.detach_status = "cuda_error";
        frame.dispatch_reason = "CUDA detach failed";
        std::string error;
        expect(writer->AppendFrame(frame, &error),
               "failed frame evidence was rejected: " + error);
        evidence::SpatialRoiRecorderFinalizeRequest request;
        request.terminal_state = "failed";
        request.terminal_reason = "CUDA detach failed";
        json manifest;
        expect(writer->Finalize(request, &manifest, &error),
               "failed finalization failed: " + error);
        expect(manifest.at("artifacts").empty() &&
                   manifest.at("encoder_terminal").is_null(),
               "failed finalization invented success evidence");
        expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
                   fixture->artifact_authority, fixture->binding, manifest, &error),
               "failed manifest did not revalidate: " + error);
    }

    {
        // A packet can be accepted by the writer before a later metadata-row
        // failure turns that admitted frame into a Failed result. Preserve the
        // authoritative orphan-packet counters in failed terminal evidence;
        // they must not be rewritten to match the frame-result projection.
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        auto frame = success_frame(fixture->binding, 1);
        frame.encode_status = "failed";
        frame.output_frame_index = 0;
        frame.packet_count = 0;
        frame.encoded_bytes = 0;
        frame.keyframe = false;
        std::string error;
        expect(writer->AppendFrame(frame, &error),
               "post-packet failed frame evidence was rejected: " + error);

        auto snapshot = std::make_shared<
            encoder::SpatialRoiLosslessEncoderTerminalSnapshot>(
                *success_snapshot(fixture->binding, 1));
        snapshot->successful = false;
        snapshot->terminal_reason = "metadata mapping write failed";
        snapshot->counts.finalized = false;
        snapshot->counts.failed = true;
        snapshot->counts.encoded_frames = 0;
        snapshot->counts.encoded_results = 0;
        snapshot->counts.failed_results = 1;
        // encoded_packets=1 and encoded_bytes=101 deliberately remain: the
        // writer accepted the packet before the metadata/result failure.

        evidence::SpatialRoiRecorderFinalizeRequest request;
        request.terminal_state = "failed";
        request.terminal_reason = "metadata mapping write failed";
        request.encoder_terminal_snapshot = std::move(snapshot);
        json manifest;
        expect(writer->Finalize(request, &manifest, &error),
               "authoritative post-packet failure snapshot was rejected: " + error);
        expect(manifest.at("encoder_terminal").at("counts").at("encoded_packets") ==
                   1 &&
                   manifest.at("encoder_terminal").at("counts").at("encoded_results") ==
                       0,
               "failed manifest erased orphan-packet terminal truth");
        expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
                   fixture->artifact_authority, fixture->binding, manifest, &error),
               "failed snapshot manifest did not revalidate: " + error);
    }

    {
        // If construction of the terminal frame result itself fails, the
        // encoder cannot invent a callback row. Its failed snapshot must still
        // preserve the admitted frame and already accepted packet.
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        auto snapshot = std::make_shared<
            encoder::SpatialRoiLosslessEncoderTerminalSnapshot>(
                *success_snapshot(fixture->binding, 1));
        snapshot->successful = false;
        snapshot->all_admitted_results_emitted = false;
        snapshot->terminal_reason = "frame result construction failed";
        snapshot->counts.finalized = false;
        snapshot->counts.failed = true;
        snapshot->counts.encoded_frames = 0;
        snapshot->counts.frame_results_emitted = 0;
        snapshot->counts.encoded_results = 0;
        snapshot->counts.failed_results = 0;
        snapshot->counts.result_callback_failures = 1;

        evidence::SpatialRoiRecorderFinalizeRequest request;
        request.terminal_state = "failed";
        request.terminal_reason = "frame result construction failed";
        request.encoder_terminal_snapshot = std::move(snapshot);
        json manifest;
        std::string error;
        expect(writer->Finalize(request, &manifest, &error),
               "missing-result failure snapshot was rejected: " + error);
        expect(manifest.at("encoder_terminal")
                       .at("all_admitted_results_emitted") == false &&
                   manifest.at("encoder_terminal")
                           .at("counts")
                           .at("frame_results_emitted") == 0 &&
                   manifest.at("encoder_terminal")
                           .at("counts")
                           .at("encoded_packets") == 1,
               "failed manifest erased missing-result/orphan-packet truth");
        expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
                   fixture->artifact_authority, fixture->binding, manifest, &error),
               "missing-result failed manifest did not revalidate: " + error);
    }

    {
        // The encoder counts a valid Encoded result before invoking the
        // durable callback. Callback rejection can therefore leave terminal
        // encoded counts with no corresponding evidence row.
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        auto snapshot = std::make_shared<
            encoder::SpatialRoiLosslessEncoderTerminalSnapshot>(
                *success_snapshot(fixture->binding, 1));
        snapshot->successful = false;
        snapshot->terminal_reason = "frame result callback rejected completion";
        snapshot->counts.finalized = false;
        snapshot->counts.failed = true;
        snapshot->counts.result_callback_failures = 1;

        evidence::SpatialRoiRecorderFinalizeRequest request;
        request.terminal_state = "failed";
        request.terminal_reason =
            "frame result callback rejected completion";
        request.encoder_terminal_snapshot = std::move(snapshot);
        json manifest;
        std::string error;
        expect(writer->Finalize(request, &manifest, &error),
               "callback-failure terminal snapshot was rejected: " + error);
        expect(manifest.at("encoder_terminal")
                       .at("counts")
                       .at("encoded_frames") == 1 &&
                   manifest.at("counts").at("encoded_frames") == 0,
               "failed manifest conflated encoder and durable-evidence counts");
        expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
                   fixture->artifact_authority, fixture->binding, manifest, &error),
               "callback-failure manifest did not revalidate: " + error);
    }

    {
        // Pre-admission failures are accounted attempts even though there is
        // no nonempty encoder stream or frame evidence.
        auto fixture = make_fixture();
        auto writer = open_writer(*fixture);
        auto snapshot = std::make_shared<
            encoder::SpatialRoiLosslessEncoderTerminalSnapshot>(
                *success_snapshot(fixture->binding, 0));
        snapshot->successful = false;
        snapshot->terminal_reason = "work item allocation failed";
        snapshot->counts.enqueue_attempted = 1;
        snapshot->counts.rejected = 1;
        snapshot->counts.encoded_bytes = 0;
        snapshot->counts.finalized = false;
        snapshot->counts.failed = true;

        evidence::SpatialRoiRecorderFinalizeRequest request;
        request.terminal_state = "failed";
        request.terminal_reason = "work item allocation failed";
        request.encoder_terminal_snapshot = std::move(snapshot);
        json manifest;
        std::string error;
        expect(writer->Finalize(request, &manifest, &error),
               "accounted pre-admission failure snapshot was rejected: " + error);
        expect(manifest.at("encoder_terminal")
                       .at("all_enqueue_attempts_accounted") == true &&
                   manifest.at("encoder_terminal").at("counts").at("rejected") ==
                       1,
               "failed manifest erased accounted rejection truth");
        expect(evidence::validate_spatial_roi_recorder_finalized_manifest(
                   fixture->artifact_authority, fixture->binding, manifest, &error),
               "pre-admission failed manifest did not revalidate: " + error);
    }
}

}  // namespace

int main()
{
    try {
        test_authoritative_binding();
        test_roundtrip_and_complete_adoption();
        test_complete_requires_decoder_probe_capability();
        test_encoder_terminal_v2_completion_truth();
        test_frame_truth_tables();
        test_lifecycle_json_roundtrip_and_counters();
        test_closed_artifact_proofs();
        test_zero_frame_completion_fails_closed();
        test_duplicate_json_keys();
        test_crash_adoption_and_root_swap();
        test_leaf_replacement_is_not_redirected();
        test_failed_terminal();
        std::cout << "spatial_roi_recorder_evidence_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_recorder_evidence_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
