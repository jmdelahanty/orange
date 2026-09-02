#include "spatial_roi_recorder_frame_journal_bridge.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace ipc = orange::spatial_roi::ipc;
namespace recording = orange::spatial_roi::recording;

namespace {

void expect(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

ipc::SpatialRoiIpcFrame make_frame()
{
    ipc::SpatialRoiIpcFrame frame;
    frame.descriptor.recording_id = "recording";
    frame.descriptor.recording_identity_token = "sha256:" + std::string(64, 'a');
    frame.descriptor.producer_generation = "generation";
    frame.descriptor.camera_id = 3;
    frame.descriptor.camera_serial = "CAM003";
    frame.descriptor.local_frame_id = 11;
    frame.descriptor.camera_frame_id = 12;
    frame.descriptor.recording_frame_id = 13;
    frame.descriptor.roi_stream_frame_index = 1;
    frame.descriptor.camera_timestamp_ns = 14;
    frame.descriptor.timestamp_sys_ns = 15;
    frame.descriptor.roi_id = "roi";
    frame.descriptor.region_id = "region";
    frame.descriptor.arena_group_id = "group";
    frame.descriptor.arena_id = "arena";
    frame.descriptor.logical_stream_id = "stream";
    frame.descriptor.spatial_roi_plan_sha256 =
        "sha256:" + std::string(64, 'b');
    frame.descriptor.native_raster = {8, 8};
    frame.descriptor.content_rect = {0, 0, 8, 8};
    frame.descriptor.encoded_raster = {8, 8};
    frame.descriptor.encoded_content_rect = {0, 0, 8, 8};
    frame.descriptor.padding = {0, 0, 0, 0, 0};
    frame.descriptor.bytes = 64;
    frame.descriptor.source_gpu_id = 0;
    frame.descriptor.assigned_gpu_id = 1;
    frame.descriptor.assigned_shard_id = 2;
    frame.descriptor.routing_policy = "single_shard";
    frame.cuda_buffer.memory_handle_hex = std::string(128, 'a');
    frame.cuda_buffer.ready_event_handle_hex = std::string(128, 'b');
    frame.cuda_buffer.byte_length = 64;
    frame.cuda_buffer.row_pitch_bytes = 8;
    return frame;
}

ipc::SpatialRoiRecorderIpcFrameOutcome accepted_outcome(
    const ipc::SpatialRoiIpcFrame& frame)
{
    ipc::SpatialRoiRecorderIpcFrameOutcome outcome;
    outcome.descriptor = frame.descriptor;
    outcome.dispatch.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kEnqueued;
    outcome.dispatch.detach_status = "detached";
    outcome.dispatch.source_release_safe = true;
    outcome.ack_attempted = true;
    outcome.ack_sent = true;
    outcome.ack_accepted = true;
    outcome.release_attempted = true;
    outcome.release_sent = true;
    outcome.release_reason = "source_detached";
    return outcome;
}

void test_normal_mapping()
{
    const auto frame = make_frame();
    const auto source = accepted_outcome(frame);
    recording::SpatialRoiRecorderFrameTransportOutcome mapped;
    std::string error;
    expect(recording::make_spatial_roi_recorder_frame_transport_outcome(
               source, &mapped, &error),
           error);
    expect(mapped.descriptor.recording_frame_id == 13 &&
               mapped.descriptor.logical_stream_id == "stream" &&
               mapped.detach_status == "detached" && mapped.source_release_safe &&
               mapped.dispatch_admitted && mapped.dispatch_reason.empty() &&
               mapped.ack_attempted && mapped.ack_sent && mapped.ack_accepted &&
               mapped.release_attempted && mapped.release_sent &&
               mapped.release_reason == "source_detached" &&
               mapped.ack_reason.empty() && mapped.ack_error.empty() &&
               mapped.release_error.empty(),
           "normal session outcome was not mapped exactly");
}

void test_uncertain_and_write_failures()
{
    const auto frame = make_frame();
    std::string error;

    auto uncertain = accepted_outcome(frame);
    uncertain.dispatch.status =
        ipc::SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain;
    uncertain.dispatch.detach_status = "source_quarantined";
    uncertain.dispatch.source_release_safe = false;
    uncertain.dispatch.reason = "source completion uncertain";
    uncertain.ack_attempted = false;
    uncertain.ack_sent = false;
    uncertain.ack_accepted = false;
    uncertain.release_attempted = false;
    uncertain.release_sent = false;
    uncertain.release_reason.clear();
    recording::SpatialRoiRecorderFrameTransportOutcome mapped;
    expect(recording::make_spatial_roi_recorder_frame_transport_outcome(
               uncertain, &mapped, &error),
           error);
    expect(mapped.detach_status == "source_quarantined" &&
               !mapped.source_release_safe && !mapped.dispatch_admitted &&
               mapped.dispatch_reason == "source completion uncertain" &&
               !mapped.ack_attempted && !mapped.ack_sent &&
               !mapped.ack_accepted && !mapped.release_attempted &&
               !mapped.release_sent && mapped.release_reason.empty(),
           "uncertain ownership state was not preserved");

    auto ack_failure = accepted_outcome(frame);
    ack_failure.ack_sent = false;
    ack_failure.ack_error = "EPIPE";
    ack_failure.release_attempted = false;
    ack_failure.release_sent = false;
    ack_failure.release_reason.clear();
    expect(recording::make_spatial_roi_recorder_frame_transport_outcome(
               ack_failure, &mapped, &error),
           error);
    expect(mapped.dispatch_admitted && mapped.ack_attempted &&
               !mapped.ack_sent && mapped.ack_accepted &&
               mapped.ack_error == "EPIPE" && !mapped.release_attempted,
           "ACK write failure state was not preserved");

    auto release_failure = accepted_outcome(frame);
    release_failure.release_sent = false;
    release_failure.release_error = "EPIPE";
    expect(recording::make_spatial_roi_recorder_frame_transport_outcome(
               release_failure, &mapped, &error),
           error);
    expect(mapped.ack_sent && mapped.ack_accepted &&
               mapped.release_attempted && !mapped.release_sent &&
               mapped.release_reason == "source_detached" &&
               mapped.release_error == "EPIPE",
           "RELEASE write failure state was not preserved");
}

void test_null_output()
{
    const auto frame = make_frame();
    const auto source = accepted_outcome(frame);
    std::string error;
    expect(!recording::make_spatial_roi_recorder_frame_transport_outcome(
               source, nullptr, &error) &&
               error == "journal bridge output is null",
           "null bridge output was accepted");
}

}  // namespace

int main()
{
    try {
        test_normal_mapping();
        test_uncertain_and_write_failures();
        test_null_output();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr,
                     "spatial ROI recorder frame journal bridge tests failed: %s\n",
                     exception.what());
        return 1;
    }
}
