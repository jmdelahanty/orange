#include "spatial_roi_recorder_frame_journal.h"

#include "session/spatial_roi_recording_config.h"
#include "session/spatial_roi_recorder_contract.h"
#include "shaman_v2_recording_identity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
namespace contract = orange::session::spatial_roi;
namespace ipc = orange::spatial_roi::ipc;
namespace recording = orange::spatial_roi::recording;
namespace encoder = orange::spatial_roi::encoder;

namespace {

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

fs::path temporary_root()
{
    std::string pattern = "/tmp/orange_spatial_roi_frame_journal_XXXXXX";
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    char* result = ::mkdtemp(bytes.data());
    expect(result != nullptr, "mkdtemp failed");
    return fs::path(result);
}

struct Fixture {
    fs::path root;
    recording::SpatialRoiRecorderEvidenceBinding binding;
    std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot> authority;
    std::unique_ptr<recording::SpatialRoiRecorderEvidenceWriter> writer;

    explicit Fixture(const std::size_t writer_max_frames = 8)
        : root(temporary_root())
    {
        contract::Config config = contract::default_config();
        config.enabled = true;
        config.buffering.pool_frames_per_stream = 4;
        config.buffering.queue_frames_per_stream = 8;
        config.recording_limits.max_frames_per_stream = writer_max_frames;
        config.recording_limits.max_media_bytes_per_stream = 1024 * 1024;
        config.recording_limits.max_evidence_bytes_per_stream = 8 * 1024 * 1024;
        config.admission.max_rois_per_camera = 1;
        config.admission.max_total_rois = 1;
        config.admission.max_total_encoder_streams = 1;
        config.admission.max_total_pixel_rate = 1000000;
        config.admission.max_total_pool_bytes = 1000000;
        config.admission.max_total_queue_frames = 32;
        config.admission.max_total_media_bytes = 1024 * 1024;
        config.admission.max_total_evidence_bytes = 8 * 1024 * 1024;

        contract::CameraConfig camera;
        camera.camera_id = 0;
        camera.camera_serial = "CAM001";
        camera.native_raster = {8, 8};
        camera.source_frame_rate = 100;
        camera.arena_group_id = "group0";
        camera.layout = {"layout_v1", digest('1')};
        camera.materialization = {"materialization_v1", digest('2')};
        camera.registration = {"registration_v1", digest('3')};
        contract::RoiConfig roi;
        roi.roi_id = "roi0";
        roi.region_id = "region0";
        roi.has_arena_id = true;
        roi.arena_id = "arena0";
        roi.required = true;
        roi.content_rect = {0, 0, 4, 4};
        roi.logical_stream_id =
            contract::expected_logical_stream_id(camera.camera_serial, roi.roi_id);
        roi.artifact_stem =
            contract::expected_artifact_stem(camera.camera_serial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
        config.cameras.emplace(camera.camera_serial, std::move(camera));

        contract::PlanContext context;
        context.recording_id = "frame-journal-test-recording";
        context.recording_identity_token =
            orange::shaman_v2_recording_identity::token_for_recording_id(
                context.recording_id);
        context.generated_at_utc = "2026-08-31T12:00:00Z";
        context.producer_generation = "generation_001";
        nlohmann::json plan;
        std::string error;
        expect(contract::build_plan(config, context, &plan, nullptr, &error),
               "plan build failed: " + error);

        const std::string stream =
            contract::expected_logical_stream_id("CAM001", "roi0");
        contract::SpatialRoiRecorderRuntimeGpuMapping mapping;
        mapping.analytics_gpu_by_camera_serial.emplace("CAM001", 0);
        mapping.recorder_gpu_by_logical_stream_id.emplace(stream, 1);
        nlohmann::json recorder_contract;
        expect(contract::build_spatial_roi_recorder_contract(
                   plan, root.generic_string(), mapping, &recorder_contract,
                   &error),
               "contract build failed: " + error);
        expect(recording::make_spatial_roi_recorder_evidence_binding(
                   recorder_contract, plan, root.generic_string(), mapping, stream,
                   &binding, &error),
               "binding failed: " + error);

        std::vector<std::string> allowed;
        for (const auto& item : binding.expected_artifacts) {
            allowed.push_back(item.second);
        }
        std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> opened;
        expect(recording::SpatialRoiRecorderArtifactRoot::Open(
                   root, allowed, &opened, &error),
               "artifact root open failed: " + error);
        authority = std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot>(
            std::move(opened));
        recording::SpatialRoiRecorderEvidenceWriterConfig writer_config;
        writer_config.artifact_root = authority;
        writer_config.evidence_relative_path =
            binding.expected_artifacts.at("evidence");
        writer_config.manifest_relative_path =
            binding.expected_artifacts.at("evidence_manifest");
        writer_config.max_frames = writer_max_frames;
        expect(recording::SpatialRoiRecorderEvidenceWriter::Open(
                   writer_config, binding, &writer, &error),
               "writer open failed: " + error);
    }

    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

orange::spatial_roi::SpatialRoiFrameDescriptor descriptor(
    const recording::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t index)
{
    orange::spatial_roi::SpatialRoiFrameDescriptor value;
    value.recording_id = binding.recording_id;
    value.recording_identity_token = binding.recording_identity_token;
    value.producer_generation = binding.producer_generation;
    value.camera_id = binding.camera_id;
    value.camera_serial = binding.camera_serial;
    value.local_frame_id = 100 + index;
    value.camera_frame_id = 200 + index;
    value.recording_frame_id = 1000 + index;
    value.roi_stream_frame_index = index;
    value.camera_timestamp_ns = 1000000 + index;
    value.timestamp_sys_ns = 2000000 + index;
    value.roi_id = binding.roi_id;
    value.region_id = binding.region_id;
    value.arena_group_id = binding.arena_group_id;
    value.arena_id = binding.has_arena_id ? binding.arena_id : "";
    value.logical_stream_id = binding.logical_stream_id;
    value.spatial_roi_plan_sha256 = binding.plan_sha256;
    value.native_raster = {8, 8};
    value.content_rect = {0, 0, 4, 4};
    value.encoded_raster = {4, 4};
    value.encoded_content_rect = {0, 0, 4, 4};
    value.padding = {0, 0, 0, 0, 0};
    value.source_pixel_format = "mono8";
    value.bytes = 16;
    value.source_gpu_id = binding.source_gpu_id;
    value.assigned_gpu_id = binding.assigned_gpu_id;
    value.assigned_shard_id = binding.assigned_shard_id;
    value.routing_policy = binding.routing_policy;
    return value;
}

recording::SpatialRoiRecorderFrameTransportOutcome transport(
    const recording::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t index,
    const bool accepted = true)
{
    recording::SpatialRoiRecorderFrameTransportOutcome value;
    value.descriptor = descriptor(binding, index);
    value.detach_status = "detached";
    value.source_release_safe = true;
    value.dispatch_admitted = accepted;
    value.dispatch_reason = accepted ? "" : "queue_full";
    value.ack_attempted = true;
    value.ack_sent = true;
    value.ack_accepted = accepted;
    value.ack_reason = accepted ? "" : "queue_full";
    value.release_attempted = true;
    value.release_sent = true;
    value.release_reason = accepted ? "source_detached" : "source_rejected";
    return value;
}

encoder::SpatialRoiLosslessFrameResult result(
    const recording::SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t index,
    const bool encoded = true)
{
    const auto frame = descriptor(binding, index);
    encoder::SpatialRoiLosslessFrameResult value;
    value.status = encoded ? encoder::SpatialRoiLosslessFrameResultStatus::Encoded
                           : encoder::SpatialRoiLosslessFrameResultStatus::Failed;
    value.correlation = ipc::spatial_roi_ipc_correlation_from_descriptor(frame);
    value.geometry.native_raster = frame.native_raster;
    value.geometry.content_rect = frame.content_rect;
    value.geometry.encoded_raster = frame.encoded_raster;
    value.geometry.encoded_content_rect = frame.encoded_content_rect;
    value.geometry.padding = frame.padding;
    value.geometry.routing_policy = frame.routing_policy;
    value.camera_timestamp_ns = frame.camera_timestamp_ns;
    value.timestamp_sys_ns = frame.timestamp_sys_ns;
    value.source_gpu_id = frame.source_gpu_id;
    value.assigned_gpu_id = frame.assigned_gpu_id;
    value.assigned_shard_id = frame.assigned_shard_id;
    if (encoded) {
        value.output_frame_index = index;
        value.nvenc_pts_assigned = true;
        value.nvenc_pts = index - 1;
        value.packet_count = 1;
        value.encoded_bytes = 100 + index;
        const std::uint64_t gop_length = binding.encode_profile.at("gop_length")
                                             .get<std::uint64_t>();
        value.keyframe = gop_length != 0 && ((index - 1U) % gop_length) == 0;
    } else {
        value.failure_reason = "encode_failed";
    }
    return value;
}

ipc::SpatialRoiIpcStreamIdentity stream_of(
    const recording::SpatialRoiRecorderEvidenceBinding& binding)
{
    ipc::SpatialRoiIpcStreamIdentity value;
    value.recording_id = binding.recording_id;
    value.recording_identity_token = binding.recording_identity_token;
    value.producer_generation = binding.producer_generation;
    value.camera_id = binding.camera_id;
    value.camera_serial = binding.camera_serial;
    value.roi_id = binding.roi_id;
    value.region_id = binding.region_id;
    value.arena_group_id = binding.arena_group_id;
    value.arena_id = binding.has_arena_id ? binding.arena_id : "";
    value.logical_stream_id = binding.logical_stream_id;
    value.spatial_roi_plan_sha256 = binding.plan_sha256;
    return value;
}

void test_transport_and_encoder_orders()
{
    Fixture fixture;
    recording::SpatialRoiRecorderFrameJournal journal(
        *fixture.writer, stream_of(fixture.binding), 8, 4);
    std::string error;
    expect(journal.AcceptTransportOutcome(transport(fixture.binding, 1), &error),
           error);
    expect(!journal.final_ready() && journal.pending_entries() == 1,
           "transport-first entry should await its result");
    expect(journal.AcceptEncoderResult(result(fixture.binding, 1), &error), error);
    expect(journal.Drain(0, &error), error);
    expect(journal.final_ready() && journal.appended_count() == 1,
           "transport-first frame did not flush");

    Fixture out_of_order;
    recording::SpatialRoiRecorderFrameJournal second(
        *out_of_order.writer, stream_of(out_of_order.binding), 8, 4);
    expect(second.AcceptEncoderResult(result(out_of_order.binding, 2), &error),
           error);
    expect(second.AcceptTransportOutcome(transport(out_of_order.binding, 2),
                                         &error),
           error);
    expect(second.AcceptTransportOutcome(transport(out_of_order.binding, 1),
                                         &error),
           error);
    expect(second.AcceptEncoderResult(result(out_of_order.binding, 1), &error),
           error);
    expect(second.Drain(0, &error), error);
    expect(second.final_ready() && second.appended_count() == 2,
           "out-of-order callbacks did not densely flush");
}

void test_gop_interior_non_keyframe_is_truthful_journal_evidence()
{
    Fixture fixture;
    recording::SpatialRoiRecorderFrameJournal journal(
        *fixture.writer, stream_of(fixture.binding), 8, 2);
    std::string error;
    expect(journal.AcceptTransportOutcome(transport(fixture.binding, 1), &error),
           error);
    expect(journal.AcceptEncoderResult(result(fixture.binding, 1), &error), error);
    auto interior = result(fixture.binding, 2);
    interior.keyframe = false;
    expect(journal.AcceptTransportOutcome(transport(fixture.binding, 2), &error),
           error);
    expect(journal.AcceptEncoderResult(interior, &error), error);
    expect(journal.Drain(0, &error), error);
    expect(journal.final_ready() && journal.appended_count() == 2,
           "journal rejected a truthful non-keyframe encode result");
}

void test_continuous_in_order_drain_beyond_sixty_four_frames()
{
    constexpr std::size_t kFrameCount = 128;
    Fixture fixture(kFrameCount);
    recording::SpatialRoiRecorderFrameJournal journal(
        *fixture.writer, stream_of(fixture.binding), kFrameCount, 2);
    std::string error;
    for (std::uint64_t index = 1; index <= kFrameCount; ++index) {
        expect(journal.AcceptTransportOutcome(
                   transport(fixture.binding, index), &error),
               error);
        expect(journal.AcceptEncoderResult(result(fixture.binding, index), &error),
               error);
        expect(journal.pending_entries() == 1,
               "in-order completed frame did not stage exactly one pending entry");
        expect(journal.Drain(1, &error), error);
        expect(journal.pending_entries() == 0 &&
                   journal.appended_count() == index,
               "continuous in-order drain did not release the completed entry");
    }
    const auto counters = journal.counters();
    expect(journal.final_ready() && journal.transport_count() == kFrameCount &&
               journal.appended_count() == kFrameCount &&
               counters.pending_overflow_rejections == 0,
           "more than 64 continuously drained in-order frames overflowed or remained pending");
}

void test_rejection_and_failed_encode()
{
    Fixture rejected_fixture;
    recording::SpatialRoiRecorderFrameJournal rejected(
        *rejected_fixture.writer, stream_of(rejected_fixture.binding), 8, 2);
    std::string error;
    expect(rejected.AcceptTransportOutcome(
               transport(rejected_fixture.binding, 1, false), &error),
           error);
    expect(rejected.Drain(0, &error), error);
    expect(rejected.final_ready() && rejected.appended_count() == 1,
           "safe rejection did not become not_attempted evidence");

    Fixture failed_fixture;
    recording::SpatialRoiRecorderFrameJournal failed(
        *failed_fixture.writer, stream_of(failed_fixture.binding), 8, 2);
    expect(failed.AcceptTransportOutcome(transport(failed_fixture.binding, 1),
                                          &error),
           error);
    expect(failed.AcceptEncoderResult(
               result(failed_fixture.binding, 1, false), &error),
           error);
    expect(failed.Drain(0, &error), error);
    expect(failed.final_ready() && failed.appended_count() == 1,
           "failed encode did not become terminal evidence");
}

void test_encoder_queue_pending_bound_and_terminal_rejection_headroom()
{
    std::string error;
    std::size_t bound = 0;
    expect(recording::derive_spatial_roi_frame_journal_pending_bound(
               100, 64, &bound, &error) && bound == 66,
           "queue-64 journal bound should be 66: " + error);
    expect(recording::derive_spatial_roi_frame_journal_pending_bound(
               1, 64, &bound, &error) && bound == 1,
           "journal bound should clamp to max_frames_per_stream");
    expect(recording::derive_spatial_roi_frame_journal_pending_bound(
               65, 64, &bound, &error) && bound == 65,
           "journal bound should clamp a one-frame remainder");
    expect(!recording::derive_spatial_roi_frame_journal_pending_bound(
               0, 64, &bound, &error),
           "zero max_frames_per_stream was accepted");
    expect(!recording::derive_spatial_roi_frame_journal_pending_bound(
               100, 0, &bound, &error),
           "zero encoder queue capacity was accepted");
    expect(!recording::derive_spatial_roi_frame_journal_pending_bound(
               100, std::numeric_limits<std::size_t>::max(), &bound, &error),
           "overflowing encoder queue capacity was accepted");
    expect(!recording::derive_spatial_roi_frame_journal_pending_bound(
               100, 2, nullptr, &error),
           "null pending-bound output was accepted");

    // A queue of two can legally own two waiting frames plus one frame on the
    // encoder owner. A fourth FRAME then receives a terminal queue-full
    // rejection. q+1 cannot preserve that rejection behind the admitted
    // prefix, while q+2 can preserve and later drain all four outcomes.
    Fixture too_small_fixture;
    recording::SpatialRoiRecorderFrameJournal too_small(
        *too_small_fixture.writer, stream_of(too_small_fixture.binding), 8, 3);
    for (std::uint64_t index = 1; index <= 3; ++index) {
        expect(too_small.AcceptTransportOutcome(
                   transport(too_small_fixture.binding, index), &error),
               error);
    }
    expect(!too_small.AcceptTransportOutcome(
               transport(too_small_fixture.binding, 4, false), &error) &&
               too_small.counters().pending_overflow_rejections == 1,
           "q+1 unexpectedly retained the terminal rejection");

    Fixture sufficient_fixture;
    expect(recording::derive_spatial_roi_frame_journal_pending_bound(
               8, 2, &bound, &error) && bound == 4,
           "queue-2 journal bound should be 4");
    recording::SpatialRoiRecorderFrameJournal sufficient(
        *sufficient_fixture.writer, stream_of(sufficient_fixture.binding), 8,
        bound);
    const auto initial_snapshot = sufficient.operational_snapshot();
    expect(initial_snapshot.observation_succeeded && initial_snapshot.valid &&
               !initial_snapshot.fatal_latched &&
               initial_snapshot.final_ready &&
               initial_snapshot.max_frames == 8 &&
               initial_snapshot.pending_entries_limit == 4 &&
               initial_snapshot.pending_entries_current == 0 &&
               initial_snapshot.transport_count == 0 &&
               initial_snapshot.appended_count == 0 &&
               initial_snapshot.next_flush_index == 1 &&
               initial_snapshot.counters.pending_entries_high_water == 0,
           "initial operational snapshot is inconsistent");
    for (std::uint64_t index = 1; index <= 3; ++index) {
        expect(sufficient.AcceptTransportOutcome(
                   transport(sufficient_fixture.binding, index), &error),
               error);
    }
    expect(sufficient.AcceptTransportOutcome(
               transport(sufficient_fixture.binding, 4, false), &error),
           error);
    const auto pending_snapshot = sufficient.operational_snapshot();
    expect(pending_snapshot.observation_succeeded && pending_snapshot.valid &&
               !pending_snapshot.final_ready &&
               pending_snapshot.pending_entries_limit == 4 &&
               pending_snapshot.pending_entries_current == 4 &&
               pending_snapshot.transport_count == 4 &&
               pending_snapshot.appended_count == 0 &&
               pending_snapshot.next_flush_index == 1 &&
               pending_snapshot.counters.pending_entries_high_water == 4 &&
               pending_snapshot.counters.transport_outcomes == 4 &&
               pending_snapshot.counters.dispatch_admitted == 3 &&
               pending_snapshot.counters.dispatch_rejected == 1 &&
               pending_snapshot.counters.encoder_results == 0 &&
               pending_snapshot.counters.frames_appended == 0 &&
               pending_snapshot.counters.ack_attempted == 4 &&
               pending_snapshot.counters.ack_sent == 4 &&
               pending_snapshot.counters.release_attempted == 4 &&
               pending_snapshot.counters.release_sent == 4 &&
               pending_snapshot.counters.pending_overflow_rejections == 0 &&
               pending_snapshot.counters.append_failures == 0,
           "pending operational snapshot is inconsistent");
    for (std::uint64_t index = 1; index <= 3; ++index) {
        expect(sufficient.AcceptEncoderResult(
                   result(sufficient_fixture.binding, index), &error),
               error);
    }
    expect(sufficient.Drain(0, &error), error);
    const auto drained_snapshot = sufficient.operational_snapshot();
    expect(drained_snapshot.observation_succeeded &&
               drained_snapshot.final_ready &&
               drained_snapshot.pending_entries_current == 0 &&
               drained_snapshot.counters.pending_entries_high_water == 4 &&
               drained_snapshot.transport_count == 4 &&
               drained_snapshot.appended_count == 4 &&
               drained_snapshot.next_flush_index == 5 &&
               drained_snapshot.counters.transport_outcomes == 4 &&
               drained_snapshot.counters.encoder_results == 3 &&
               drained_snapshot.counters.frames_appended == 4 &&
               drained_snapshot.counters.dispatch_rejected == 1,
           "q+2 journal did not prove the admitted prefix and rejection");
}

void test_constructor_requires_one_writer_form()
{
    Fixture fixture;
    recording::SpatialRoiRecorderFrameJournalConfig config;
    config.expected_stream = stream_of(fixture.binding);
    config.writer = fixture.writer.get();
    config.shared_writer =
        std::shared_ptr<recording::SpatialRoiRecorderEvidenceWriter>(
            fixture.writer.get(), [](recording::SpatialRoiRecorderEvidenceWriter*) {});
    config.max_frames = 8;
    config.max_pending_entries = 2;
    recording::SpatialRoiRecorderFrameJournal journal(std::move(config));
    expect(!journal.valid() && journal.fatal_latched(),
           "journal accepted both raw and shared writer forms");
    expect(journal.error().find("exactly one evidence writer form") !=
               std::string::npos,
           "journal did not explain the duplicate writer forms");
}

void test_source_safe_lifecycle_requires_ack_and_release_attempts()
{
    std::string error;

    Fixture missing_ack_fixture;
    recording::SpatialRoiRecorderFrameJournal missing_ack(
        *missing_ack_fixture.writer, stream_of(missing_ack_fixture.binding), 8, 2);
    auto missing_ack_outcome = transport(missing_ack_fixture.binding, 1);
    missing_ack_outcome.ack_attempted = false;
    missing_ack_outcome.ack_sent = false;
    missing_ack_outcome.ack_accepted = false;
    missing_ack_outcome.ack_reason.clear();
    missing_ack_outcome.release_attempted = false;
    missing_ack_outcome.release_sent = false;
    missing_ack_outcome.release_reason.clear();
    expect(!missing_ack.AcceptTransportOutcome(missing_ack_outcome, &error) &&
               missing_ack.fatal_latched(),
           "source-safe transport without ACK attempt was accepted");

    Fixture missing_release_fixture;
    recording::SpatialRoiRecorderFrameJournal missing_release(
        *missing_release_fixture.writer,
        stream_of(missing_release_fixture.binding), 8, 2);
    auto missing_release_outcome = transport(missing_release_fixture.binding, 1);
    missing_release_outcome.release_attempted = false;
    missing_release_outcome.release_sent = false;
    missing_release_outcome.release_reason.clear();
    expect(!missing_release.AcceptTransportOutcome(missing_release_outcome,
                                                   &error) &&
               missing_release.fatal_latched(),
           "source-safe transport without RELEASE attempt was accepted");
}

void test_transport_lifecycle_truth_and_nonblocking_ingest()
{
    std::string error;

    // A safe dispatch rejection is terminal at the transport seam and never
    // consumes an encoder result.
    Fixture rejection_fixture;
    recording::SpatialRoiRecorderFrameJournal rejection(
        *rejection_fixture.writer, stream_of(rejection_fixture.binding), 8, 2);
    auto rejected = transport(rejection_fixture.binding, 1, false);
    expect(rejection.AcceptTransportOutcome(rejected, &error), error);
    expect(rejection.appended_count() == 0 && rejection.pending_entries() == 1,
           "transport ingestion performed evidence I/O");
    expect(rejection.Drain(0, &error), error);
    expect(rejection.final_ready() && rejection.counters().dispatch_rejected == 1,
           "safe rejection lifecycle was not drained");

    // Admission succeeded, but the ACK write failed. This frame is still
    // represented with its internal admission and local write error; a late
    // encoder result remains valid and is drained without writer I/O in the
    // callback path.
    Fixture ack_failure_fixture;
    recording::SpatialRoiRecorderFrameJournal ack_failure(
        *ack_failure_fixture.writer, stream_of(ack_failure_fixture.binding), 8, 2);
    auto ack_failed = transport(ack_failure_fixture.binding, 1);
    ack_failed.ack_attempted = true;
    ack_failed.ack_sent = false;
    ack_failed.ack_reason.clear();
    ack_failed.ack_error = "EPIPE";
    ack_failed.release_attempted = false;
    ack_failed.release_sent = false;
    ack_failed.release_reason.clear();
    expect(ack_failure.AcceptTransportOutcome(ack_failed, &error), error);
    expect(ack_failure.AcceptEncoderResult(result(ack_failure_fixture.binding, 1),
                                           &error),
           error);
    const bool ack_drained = ack_failure.Drain(0, &error);
    expect(ack_drained, "ACK write failure drain failed: " + error);
    expect(ack_failure.final_ready() &&
               ack_failure.counters().ack_write_failures == 1,
           "ACK write failure lifecycle was not preserved");

    // ACK reached the wire, while RELEASE did not. The wire reason remains
    // distinct from the local RELEASE write error.
    Fixture release_failure_fixture;
    recording::SpatialRoiRecorderFrameJournal release_failure(
        *release_failure_fixture.writer,
        stream_of(release_failure_fixture.binding), 8, 2);
    auto release_failed = transport(release_failure_fixture.binding, 1);
    release_failed.release_attempted = true;
    release_failed.release_sent = false;
    release_failed.release_reason = "source_detached";
    release_failed.release_error = "EPIPE";
    expect(release_failure.AcceptTransportOutcome(release_failed, &error), error);
    expect(release_failure.AcceptEncoderResult(
               result(release_failure_fixture.binding, 1), &error),
           error);
    expect(release_failure.Drain(0, &error), error);
    expect(release_failure.final_ready() &&
               release_failure.counters().release_write_failures == 1,
           "RELEASE write failure lifecycle was not preserved");

    // RELEASE is not legal after an ACK write failure because the peer has
    // not observed the admission decision.
    Fixture invalid_release_fixture;
    recording::SpatialRoiRecorderFrameJournal invalid_release(
        *invalid_release_fixture.writer,
        stream_of(invalid_release_fixture.binding), 8, 2);
    auto invalid = transport(invalid_release_fixture.binding, 1);
    invalid.ack_sent = false;
    invalid.ack_error = "EPIPE";
    invalid.release_attempted = true;
    invalid.release_sent = false;
    invalid.release_reason = "source_detached";
    invalid.release_error = "EPIPE";
    expect(!invalid_release.AcceptTransportOutcome(invalid, &error) &&
               invalid_release.fatal_latched(),
           "RELEASE after ACK failure was accepted");

    // The ordinary source-detached path still waits for and joins the encoder
    // result, then drains in strict dense order.
    Fixture normal_fixture;
    recording::SpatialRoiRecorderFrameJournal normal(
        *normal_fixture.writer, stream_of(normal_fixture.binding), 8, 2);
    expect(normal.AcceptTransportOutcome(transport(normal_fixture.binding, 1),
                                         &error),
           error);
    expect(normal.AcceptEncoderResult(result(normal_fixture.binding, 1), &error),
           error);
    expect(normal.appended_count() == 0,
           "encoder callback path synchronously appended evidence");
    expect(normal.Drain(0, &error), error);
    expect(normal.final_ready() && normal.appended_count() == 1,
           "source-detached lifecycle did not drain");
}

void test_rejections_and_bounds()
{
    std::string error;
    Fixture duplicate_fixture;
    recording::SpatialRoiRecorderFrameJournal duplicate(
        *duplicate_fixture.writer, stream_of(duplicate_fixture.binding), 8, 2);
    expect(duplicate.AcceptTransportOutcome(
               transport(duplicate_fixture.binding, 1), &error),
           error);
    expect(!duplicate.AcceptTransportOutcome(
               transport(duplicate_fixture.binding, 1), &error) &&
               duplicate.fatal_latched(),
           "duplicate transport was accepted");
    expect(error == "duplicate transport outcome",
           "duplicate transport diagnostic changed: " + error);

    Fixture conflict_fixture;
    recording::SpatialRoiRecorderFrameJournal conflict(
        *conflict_fixture.writer, stream_of(conflict_fixture.binding), 8, 2);
    auto first = result(conflict_fixture.binding, 1);
    expect(conflict.AcceptEncoderResult(first, &error), error);
    first.camera_timestamp_ns++;
    expect(!conflict.AcceptEncoderResult(first, &error) &&
               conflict.fatal_latched(),
           "conflicting encoder result was accepted");
    expect(error.find("conflicting encoder result for frame index 1") !=
               std::string::npos,
           "conflicting encoder diagnostic omitted frame index: " + error);

    Fixture gap_fixture;
    recording::SpatialRoiRecorderFrameJournal gap(
        *gap_fixture.writer, stream_of(gap_fixture.binding), 8, 2);
    expect(gap.AcceptTransportOutcome(transport(gap_fixture.binding, 2, false),
                                      &error),
           error);
    expect(!gap.final_ready() && gap.pending_entries() == 1,
           "a gap should not report final readiness");

    Fixture cross_fixture;
    recording::SpatialRoiRecorderFrameJournal cross(
        *cross_fixture.writer, stream_of(cross_fixture.binding), 8, 2);
    auto wrong = transport(cross_fixture.binding, 1);
    wrong.descriptor.roi_id = "other_roi";
    expect(!cross.AcceptTransportOutcome(wrong, &error) &&
               cross.fatal_latched(),
           "cross-stream transport was accepted");

    Fixture overflow_fixture;
    recording::SpatialRoiRecorderFrameJournal overflow(
        *overflow_fixture.writer, stream_of(overflow_fixture.binding), 8, 1);
    expect(overflow.AcceptEncoderResult(result(overflow_fixture.binding, 1),
                                        &error),
           error);
    expect(!overflow.AcceptEncoderResult(result(overflow_fixture.binding, 2),
                                         &error) &&
               overflow.counters().pending_overflow_rejections == 1,
           "pending overflow was not latched");
    const auto overflow_snapshot = overflow.operational_snapshot();
    expect(overflow_snapshot.observation_succeeded &&
               !overflow_snapshot.valid &&
               overflow_snapshot.fatal_latched &&
               !overflow_snapshot.final_ready &&
               overflow_snapshot.pending_entries_limit == 1 &&
               overflow_snapshot.pending_entries_current == 1 &&
               overflow_snapshot.next_flush_index == 1 &&
               overflow_snapshot.counters.encoder_results == 1 &&
               overflow_snapshot.counters.pending_entries_high_water == 1 &&
               overflow_snapshot.counters.pending_overflow_rejections == 1 &&
               overflow_snapshot.counters.append_failures == 0,
           "overflow operational snapshot is inconsistent");

    Fixture append_fixture(1);
    recording::SpatialRoiRecorderFrameJournal append(
        *append_fixture.writer, stream_of(append_fixture.binding), 8, 2);
    expect(append.AcceptTransportOutcome(transport(append_fixture.binding, 1),
                                         &error),
           error);
    expect(append.AcceptEncoderResult(result(append_fixture.binding, 1), &error),
           error);
    expect(append.Drain(0, &error), error);
    expect(append.AcceptTransportOutcome(transport(append_fixture.binding, 2),
                                          &error),
           error);
    expect(append.AcceptEncoderResult(result(append_fixture.binding, 2), &error),
           error);
    expect(!append.Drain(0, &error) &&
               append.fatal_latched() && append.counters().append_failures == 1,
           "AppendFrame rejection was not propagated");
    const auto append_snapshot = append.operational_snapshot();
    expect(append_snapshot.observation_succeeded &&
               append_snapshot.fatal_latched &&
               append_snapshot.pending_entries_limit == 2 &&
               append_snapshot.pending_entries_current == 1 &&
               append_snapshot.transport_count == 2 &&
               append_snapshot.appended_count == 1 &&
               append_snapshot.next_flush_index == 2 &&
               append_snapshot.counters.transport_outcomes == 2 &&
               append_snapshot.counters.encoder_results == 2 &&
               append_snapshot.counters.frames_appended == 1 &&
               append_snapshot.counters.append_failures == 1,
           "append-failure operational snapshot is inconsistent");
}

void test_callback_no_throw_and_thread_safety()
{
    Fixture fixture;
    recording::SpatialRoiRecorderFrameJournal journal(
        *fixture.writer, stream_of(fixture.binding), 8, 4);
    auto invalid = result(fixture.binding, 1);
    invalid.status = static_cast<encoder::SpatialRoiLosslessFrameResultStatus>(99);
    bool callback_returned = false;
    try {
        callback_returned = !journal.AcceptEncoderResult(invalid, nullptr);
        const auto snapshot = journal.operational_snapshot();
        expect(snapshot.observation_succeeded && snapshot.fatal_latched &&
                   snapshot.counters.invalid_input_rejections == 1,
               "no-throw operational snapshot lost callback failure state");
    } catch (...) {
        expect(false, "callback-facing API or operational snapshot threw");
    }
    expect(callback_returned, "invalid callback result was accepted");

    Fixture threaded_fixture;
    recording::SpatialRoiRecorderFrameJournal threaded(
        *threaded_fixture.writer, stream_of(threaded_fixture.binding), 8, 4);
    std::string first_error;
    std::string second_error;
    std::thread first([&] {
        threaded.AcceptEncoderResult(result(threaded_fixture.binding, 1),
                                     &first_error);
    });
    std::thread second([&] {
        threaded.AcceptTransportOutcome(transport(threaded_fixture.binding, 1),
                                        &second_error);
    });
    first.join();
    second.join();
    std::string error;
    expect(threaded.Drain(0, &error), error);
    expect(threaded.final_ready() && threaded.appended_count() == 1,
           "thread-safe join failed");
}

}  // namespace

int main()
{
    try {
        test_transport_and_encoder_orders();
        test_gop_interior_non_keyframe_is_truthful_journal_evidence();
        test_continuous_in_order_drain_beyond_sixty_four_frames();
        test_rejection_and_failed_encode();
        test_encoder_queue_pending_bound_and_terminal_rejection_headroom();
        test_constructor_requires_one_writer_form();
        test_source_safe_lifecycle_requires_ack_and_release_attempts();
        test_transport_lifecycle_truth_and_nonblocking_ingest();
        test_rejections_and_bounds();
        test_callback_no_throw_and_thread_safety();
        std::cout << "spatial ROI recorder frame journal tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial ROI recorder frame journal tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
