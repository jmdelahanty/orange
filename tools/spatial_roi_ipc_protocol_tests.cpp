#include "spatial_roi_ipc_protocol.h"

#include "shaman_v2_recording_identity.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

namespace {

using orange::spatial_roi::SpatialRoiFrameDescriptor;
using orange::spatial_roi::ipc::SpatialRoiCudaIpcBuffer;
using orange::spatial_roi::ipc::SpatialRoiIpcAck;
using orange::spatial_roi::ipc::SpatialRoiIpcCorrelation;
using orange::spatial_roi::ipc::SpatialRoiIpcCorrelationRegistry;
using orange::spatial_roi::ipc::SpatialRoiIpcControlPhase;
using orange::spatial_roi::ipc::SpatialRoiIpcControlState;
using orange::spatial_roi::ipc::SpatialRoiIpcDrainRequest;
using orange::spatial_roi::ipc::SpatialRoiIpcDrainStatus;
using orange::spatial_roi::ipc::SpatialRoiIpcFrame;
using orange::spatial_roi::ipc::SpatialRoiIpcFinalizeRequest;
using orange::spatial_roi::ipc::SpatialRoiIpcFinalizeStatus;
using orange::spatial_roi::ipc::SpatialRoiIpcHello;
using orange::spatial_roi::ipc::SpatialRoiIpcMessage;
using orange::spatial_roi::ipc::SpatialRoiIpcRelease;
using orange::spatial_roi::ipc::SpatialRoiIpcTerminalError;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

SpatialRoiFrameDescriptor make_descriptor()
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = "recording-20260831T120000Z";
    descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            descriptor.recording_id);
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 3;
    descriptor.camera_serial = "2010096";
    descriptor.local_frame_id = 91;
    descriptor.camera_frame_id = 7001;
    descriptor.recording_frame_id = 17;
    descriptor.roi_stream_frame_index = 12;
    descriptor.camera_timestamp_ns = 123456789;
    descriptor.timestamp_sys_ns = 987654321;
    descriptor.roi_id = "roi_1";
    descriptor.region_id = "region_1";
    descriptor.arena_group_id = "group_1";
    descriptor.arena_id = "arena_1";
    descriptor.logical_stream_id = "2010096_spatial_roi_roi_1";
    descriptor.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'a');
    descriptor.native_raster = {100, 80};
    descriptor.content_rect = {5, 7, 13, 11};
    descriptor.encoded_raster = {16, 16};
    descriptor.encoded_content_rect = {0, 0, 13, 11};
    descriptor.padding = {0, 0, 3, 5, 0};
    descriptor.bytes = 16 * 16;
    descriptor.source_gpu_id = 5;
    descriptor.assigned_gpu_id = 6;
    descriptor.assigned_shard_id = 0;
    descriptor.routing_policy = "single_shard";
    return descriptor;
}

SpatialRoiCudaIpcBuffer make_buffer(const SpatialRoiFrameDescriptor& descriptor)
{
    SpatialRoiCudaIpcBuffer buffer;
    buffer.memory_handle_hex = std::string(128, 'a');
    buffer.ready_event_handle_hex = std::string(128, 'b');
    buffer.byte_offset = 0;
    buffer.byte_length = descriptor.bytes;
    buffer.row_pitch_bytes = descriptor.encoded_raster.width;
    return buffer;
}

orange::spatial_roi::ipc::SpatialRoiIpcStreamIdentity make_stream_identity()
{
    return orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
        make_descriptor());
}

bool parse(const std::string& wire, SpatialRoiIpcMessage* message)
{
    std::string error;
    const bool result =
        orange::spatial_roi::ipc::parse_spatial_roi_ipc_message(
            wire, message, &error);
    if (!result) {
        std::cerr << "unexpected parse failure: " << error << '\n';
    }
    return result;
}

void test_hello_round_trip_and_closed_schema()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    SpatialRoiIpcHello source;
    source.stream =
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor);
    source.role = "producer";
    source.queue_capacity_frames = 8;
    source.features = {"cuda_ipc", "packed_mono8", "ack_release"};

    std::string error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(source, &error);
    expect(!wire.empty(), "HELLO serializes: " + error);
    expect(!wire.empty() && wire.back() == '\n', "HELLO is a JSON line");

    SpatialRoiIpcMessage parsed;
    expect(parse(wire, &parsed), "HELLO parses");
    expect(std::holds_alternative<SpatialRoiIpcHello>(parsed),
           "HELLO preserves message kind");
    if (std::holds_alternative<SpatialRoiIpcHello>(parsed)) {
        const auto& value = std::get<SpatialRoiIpcHello>(parsed);
        expect(value.stream.logical_stream_id == source.stream.logical_stream_id,
               "HELLO preserves canonical stream identity");
        expect(value.queue_capacity_frames == 8,
               "HELLO preserves bounded queue capacity");
        expect(value.features == source.features,
               "HELLO preserves feature list");
    }

    nlohmann::json unknown =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(source);
    unknown["future_field"] = true;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               unknown, &parsed, &error),
           "unknown top-level field fails closed");
}

void test_frame_round_trip_and_packed_cuda_contract()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    SpatialRoiIpcFrame source{descriptor, make_buffer(descriptor)};
    std::string error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(source, &error);
    expect(!wire.empty(), "FRAME serializes: " + error);

    SpatialRoiIpcMessage parsed;
    expect(parse(wire, &parsed), "FRAME parses");
    expect(std::holds_alternative<SpatialRoiIpcFrame>(parsed),
           "FRAME preserves message kind");
    if (std::holds_alternative<SpatialRoiIpcFrame>(parsed)) {
        const auto& value = std::get<SpatialRoiIpcFrame>(parsed);
        expect(value.descriptor.key() == descriptor.key(),
               "FRAME preserves descriptor collision key");
        expect(value.cuda_buffer.byte_length == descriptor.bytes,
               "FRAME preserves packed raster byte length");
        expect(value.cuda_buffer.row_pitch_bytes == 16,
               "FRAME preserves packed row pitch");
        expect(value.cuda_buffer.memory_handle_hex.size() == 128,
               "FRAME carries fixed-size memory handle encoding");
    }

    SpatialRoiIpcFrame invalid = source;
    invalid.cuda_buffer.row_pitch_bytes = 15;
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "non-packed row pitch is rejected");
    invalid = source;
    invalid.cuda_buffer.memory_handle_hex[0] = 'A';
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "ambiguous uppercase handle encoding is rejected");
    invalid = source;
    invalid.cuda_buffer.byte_length = descriptor.bytes - 1;
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "truncated packed raster span is rejected");
    invalid = source;
    invalid.cuda_buffer.byte_offset = 1;
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "nonzero opaque-allocation offset is rejected");

    nlohmann::json malformed =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(source);
    malformed["payload"]["cuda_buffer"]["future_field"] = 1;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               malformed, &parsed, &error),
           "unknown CUDA buffer field fails closed");
}

void test_ack_release_exact_correlation()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    const SpatialRoiIpcCorrelation correlation =
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor);
    std::string error;
    expect(orange::spatial_roi::ipc::spatial_roi_ipc_correlation_matches_descriptor(
               correlation, descriptor, &error),
           "descriptor-derived correlation matches exactly: " + error);

    SpatialRoiIpcAck ack{correlation, true, ""};
    SpatialRoiIpcMessage parsed;
    const std::string ack_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(ack, &error);
    expect(!ack_wire.empty(), "ACK serializes: " + error);
    expect(parse(ack_wire, &parsed), "ACK parses");
    expect(std::holds_alternative<SpatialRoiIpcAck>(parsed),
           "ACK preserves message kind");
    if (std::holds_alternative<SpatialRoiIpcAck>(parsed)) {
        const auto& value = std::get<SpatialRoiIpcAck>(parsed);
        expect(value.correlation.key() == correlation.key(),
               "ACK includes logical stream, source frame, and ROI index");
    }

    SpatialRoiIpcAck rejected_without_reason{correlation, false, ""};
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               rejected_without_reason, &error).empty(),
           "rejected ACK without a reason is rejected");
    SpatialRoiIpcAck rejected_with_reason{
        correlation, false, "queue_full"};
    expect(!orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
                rejected_with_reason, &error).empty(),
           "rejected ACK with an explicit reason is valid");

    SpatialRoiIpcRelease release{correlation, "source_consumed"};
    const std::string release_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            release, &error);
    expect(!release_wire.empty(), "RELEASE serializes: " + error);
    expect(parse(release_wire, &parsed), "RELEASE parses");
    expect(std::holds_alternative<SpatialRoiIpcRelease>(parsed),
           "RELEASE preserves message kind");

    SpatialRoiIpcCorrelation mismatch = correlation;
    mismatch.roi_stream_frame_index++;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_correlation_matches_descriptor(
               mismatch, descriptor, &error),
           "ROI stream index mismatch is rejected");
    mismatch = correlation;
    mismatch.recording_frame_id++;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_correlation_matches_descriptor(
               mismatch, descriptor, &error),
           "recording frame mismatch is rejected");
    mismatch = correlation;
    mismatch.stream.logical_stream_id = "2010096_spatial_roi_other";
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_correlation_matches_descriptor(
               mismatch, descriptor, &error),
           "logical stream mismatch is rejected");
}

void test_control_exchange_round_trip_and_closed_schema()
{
    const auto stream = make_stream_identity();
    std::string error;
    SpatialRoiIpcMessage parsed;

    SpatialRoiIpcDrainRequest drain_request{
        stream, "producer", 7, "operator_stop"};
    const std::string drain_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            drain_request, &error);
    expect(!drain_wire.empty(), "DRAIN_REQUEST serializes: " + error);
    expect(parse(drain_wire, &parsed), "DRAIN_REQUEST parses");
    expect(std::holds_alternative<SpatialRoiIpcDrainRequest>(parsed),
           "DRAIN_REQUEST preserves message kind");
    if (std::holds_alternative<SpatialRoiIpcDrainRequest>(parsed)) {
        const auto& value = std::get<SpatialRoiIpcDrainRequest>(parsed);
        expect(value.stream.recording_id == stream.recording_id,
               "DRAIN_REQUEST preserves exact recording identity");
        expect(value.drain_sequence == 7,
               "DRAIN_REQUEST preserves producer sequence");
    }

    SpatialRoiIpcDrainStatus draining{
        stream, "recorder", 7, 1, "draining", "frames_in_flight"};
    const std::string draining_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            draining, &error);
    expect(!draining_wire.empty(), "DRAIN_STATUS serializes: " + error);
    expect(parse(draining_wire, &parsed), "DRAIN_STATUS parses");
    expect(std::holds_alternative<SpatialRoiIpcDrainStatus>(parsed),
           "DRAIN_STATUS preserves message kind");

    SpatialRoiIpcDrainStatus drained{
        stream, "recorder", 7, 2, "drained", "all frames released"};
    expect(!orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
                drained, &error)
                .empty(),
           "terminal DRAIN_STATUS serializes");

    SpatialRoiIpcFinalizeRequest finalize_request{
        stream, "producer", 7, std::string(32, 'c'), "finalize_recording"};
    const std::string finalize_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            finalize_request, &error);
    expect(!finalize_wire.empty(), "FINALIZE_REQUEST serializes: " + error);
    expect(parse(finalize_wire, &parsed), "FINALIZE_REQUEST parses");
    expect(std::holds_alternative<SpatialRoiIpcFinalizeRequest>(parsed),
           "FINALIZE_REQUEST preserves message kind");

    SpatialRoiIpcFinalizeStatus finalized{
        stream, "recorder", 7, std::string(32, 'c'), 3, "finalized", "complete"};
    const std::string finalized_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            finalized, &error);
    expect(!finalized_wire.empty(), "FINALIZE_STATUS serializes: " + error);
    expect(parse(finalized_wire, &parsed), "FINALIZE_STATUS parses");
    expect(std::holds_alternative<SpatialRoiIpcFinalizeStatus>(parsed),
           "FINALIZE_STATUS preserves message kind");

    nlohmann::json unknown =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(drain_request);
    unknown["payload"]["future_field"] = true;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               unknown, &parsed, &error),
           "unknown DRAIN_REQUEST field fails closed");

    nlohmann::json wrong_direction =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(drain_request);
    wrong_direction["payload"]["sender_role"] = "recorder";
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               wrong_direction, &parsed, &error),
           "producer control with recorder direction fails closed");

    SpatialRoiIpcFinalizeRequest bad_nonce = finalize_request;
    bad_nonce.finalize_nonce = std::string(32, 'C');
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               bad_nonce, &error)
               .empty(),
           "uppercase finalize nonce is rejected");

    nlohmann::json bad_state =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(finalized);
    bad_state["payload"]["state"] = "draining";
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               bad_state, &parsed, &error),
           "unsupported FINALIZE_STATUS state fails closed");
}

void test_control_exchange_state_machine()
{
    const auto stream = make_stream_identity();
    std::string error;
    SpatialRoiIpcControlState state;

    const SpatialRoiIpcDrainStatus premature_status{
        stream, "recorder", 1, 1, "drained", "nothing pending"};
    expect(!state.accept(premature_status, &error),
           "recorder status before producer request is rejected");
    expect(!state.stream_identity(),
           "rejected premature status does not bind session identity");

    const SpatialRoiIpcDrainRequest request{
        stream, "producer", 11, "operator_stop"};
    expect(state.accept(request, &error),
           "first producer DRAIN_REQUEST is accepted: " + error);
    expect(state.phase() == SpatialRoiIpcControlPhase::kDrainRequested,
           "DRAIN_REQUEST enters requested phase");
    expect(state.drain_sequence() == 11,
           "state retains exact drain sequence");
    expect(!state.accept(request, &error),
           "duplicate DRAIN_REQUEST is rejected");

    const SpatialRoiIpcDrainStatus draining{
        stream, "recorder", 11, 1, "draining", "one frame pending"};
    expect(state.accept(draining, &error),
           "first recorder draining status is accepted: " + error);
    expect(state.phase() == SpatialRoiIpcControlPhase::kDraining,
           "draining status enters draining phase");
    expect(state.last_status_sequence() == 1,
           "first recorder status has sequence one");
    expect(!state.accept(draining, &error),
           "duplicate recorder status is rejected");

    const SpatialRoiIpcDrainStatus drained{
        stream, "recorder", 11, 2, "drained", "all frames released"};
    expect(state.accept(drained, &error),
           "terminal drain status is accepted: " + error);
    expect(state.phase() == SpatialRoiIpcControlPhase::kDrained,
           "drained status enters drained phase");

    const SpatialRoiIpcFinalizeRequest finalize_request{
        stream, "producer", 11, std::string(32, 'd'), "finalize"};
    expect(state.accept(finalize_request, &error),
           "FINALIZE_REQUEST after drain is accepted: " + error);
    expect(state.phase() == SpatialRoiIpcControlPhase::kFinalizeRequested,
           "FINALIZE_REQUEST enters finalize-requested phase");
    expect(state.finalize_nonce() == std::string(32, 'd'),
           "state retains finalize nonce");
    expect(!state.accept(finalize_request, &error),
           "duplicate FINALIZE_REQUEST is rejected");

    const SpatialRoiIpcFinalizeStatus wrong_nonce{
        stream, "recorder", 11, std::string(32, 'e'), 3, "finalized", "complete"};
    expect(!state.accept(wrong_nonce, &error),
           "FINALIZE_STATUS with wrong nonce is rejected");
    expect(state.phase() == SpatialRoiIpcControlPhase::kFinalizeRequested,
           "wrong nonce does not advance control state");

    const SpatialRoiIpcFinalizeStatus finalized{
        stream, "recorder", 11, std::string(32, 'd'), 3, "finalized", "complete"};
    expect(state.accept(finalized, &error),
           "matching FINALIZE_STATUS is accepted: " + error);
    expect(state.phase() == SpatialRoiIpcControlPhase::kFinalized,
           "finalized status enters terminal phase");
    expect(state.last_status_sequence() == 3,
           "finalize status continues recorder sequence");
    expect(!state.accept(finalized, &error),
           "duplicate FINALIZE_STATUS is rejected");

    SpatialRoiIpcControlState wrong_sequence_state;
    expect(wrong_sequence_state.accept(request, &error),
           "second state accepts initial request");
    const SpatialRoiIpcDrainStatus skipped_sequence{
        stream, "recorder", 11, 2, "drained", "sequence one was skipped"};
    expect(!wrong_sequence_state.accept(skipped_sequence, &error),
           "non-monotonic recorder status sequence is rejected");
}

void test_terminal_error_and_registry()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    const SpatialRoiIpcCorrelation correlation =
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor);
    SpatialRoiIpcTerminalError source;
    source.stream = correlation.stream;
    source.error_code = "cuda_import_failed";
    source.message = "CUDA IPC memory import failed";
    source.correlation = correlation;

    std::string error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(source, &error);
    expect(!wire.empty(), "terminal error serializes: " + error);
    SpatialRoiIpcMessage parsed;
    expect(parse(wire, &parsed), "terminal error parses");
    expect(std::holds_alternative<SpatialRoiIpcTerminalError>(parsed),
           "terminal error preserves message kind");

    SpatialRoiIpcTerminalError session_error = source;
    session_error.correlation.reset();
    const std::string session_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            session_error, &error);
    expect(!session_wire.empty(), "session terminal error serializes without correlation");
    expect(parse(session_wire, &parsed), "session terminal error parses");

    SpatialRoiIpcTerminalError invalid = source;
    invalid.fatal = false;
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "non-terminal fatal=false error is rejected");
    invalid = source;
    invalid.correlation->stream.logical_stream_id =
        "2010096_spatial_roi_other";
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               invalid, &error).empty(),
           "terminal correlation stream mismatch is rejected");

    SpatialRoiIpcCorrelationRegistry registry;
    expect(registry.note(correlation, &error), "first correlation is admitted: " + error);
    expect(!registry.note(correlation, &error),
           "duplicate full correlation is rejected");
    auto different_roi_index = correlation.key();
    ++different_roi_index.roi_stream_frame_index;
    expect(registry.note(different_roi_index, &error),
           "same source frame with a different ROI index is distinct");
    auto different_stream = correlation.key();
    different_stream.logical_stream_id = "2010096_spatial_roi_roi_2";
    expect(registry.note(different_stream, &error),
           "same source frame on another ROI stream is distinct");
    auto different_generation = correlation.key();
    different_generation.producer_generation = "generation_2";
    expect(registry.note(different_generation, &error),
           "same stream/frame/index in another producer generation is distinct");
    auto different_recording = correlation.key();
    different_recording.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            "recording-20260831T130000Z");
    expect(registry.note(different_recording, &error),
           "same stream/frame/index in another recording is distinct");
}

void test_malformed_wire_rejection()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    SpatialRoiIpcHello hello{
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor),
        "recorder",
        8,
        {"cuda_ipc"}};
    std::string error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(hello, &error);
    SpatialRoiIpcMessage parsed;
    expect(!parse(wire + "garbage", &parsed),
           "trailing non-JSON bytes are rejected");
    expect(!parse("{", &parsed), "malformed JSON is rejected");

    nlohmann::json json_wire =
        orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(hello);
    json_wire["version"] = 1;
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               json_wire, &parsed, &error),
           "old protocol version is rejected");
    json_wire = orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(hello);
    json_wire["payload"]["queue_capacity_frames"] = "8";
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               json_wire, &parsed, &error),
           "numeric field with string type is rejected");

    const SpatialRoiIpcCorrelation correlation =
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor);
    const SpatialRoiIpcAck ack{correlation, true, ""};
    json_wire = orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(ack);
    json_wire["payload"]["correlation"].erase("roi_stream_frame_index");
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               json_wire, &parsed, &error),
           "ACK without exact ROI stream index fails closed");
    json_wire = orange::spatial_roi::ipc::spatial_roi_ipc_message_to_json(ack);
    json_wire["payload"]["correlation"]["recording_frame_id"] = "17";
    expect(!orange::spatial_roi::ipc::spatial_roi_ipc_message_from_json(
               json_wire, &parsed, &error),
           "ACK correlation numeric type mismatch fails closed");

    SpatialRoiIpcTerminalError long_error;
    long_error.stream =
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor);
    long_error.error_code = "too_long";
    long_error.message = std::string(
        orange::spatial_roi::ipc::kSpatialRoiIpcMaxTextBytes + 1, 'x');
    expect(orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
               long_error, &error).empty(),
           "terminal error text length bound is enforced");
    expect(!parse(std::string(
                       orange::spatial_roi::ipc::kSpatialRoiIpcMaxWireMessageBytes + 1,
                       'x'),
                  &parsed),
           "overlong wire message is rejected");
    expect(!parse(wire + "\n", &parsed),
           "more than one trailing newline is rejected");
}

}  // namespace

int main()
{
    test_hello_round_trip_and_closed_schema();
    test_frame_round_trip_and_packed_cuda_contract();
    test_ack_release_exact_correlation();
    test_control_exchange_round_trip_and_closed_schema();
    test_control_exchange_state_machine();
    test_terminal_error_and_registry();
    test_malformed_wire_rejection();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "spatial ROI IPC protocol tests passed\n";
    return 0;
}
