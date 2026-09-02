#include "spatial_roi_recorder_ipc_session.h"

#include "shaman_v2_recording_identity.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ipc = orange::spatial_roi::ipc;
using orange::spatial_roi::SpatialRoiFrameDescriptor;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScriptedTransport final : public ipc::SpatialRoiIpcLineTransport {
public:
    bool write_ok = true;
    std::string write_error = "scripted write failed";
    std::size_t fail_on_write_number = 0;
    std::vector<std::string> writes;
    std::vector<ipc::SpatialRoiIpcTransportReadResult> reads;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::vector<std::size_t> read_bounds;

    bool WriteLine(const std::string& line, std::string* error_out) override
    {
        writes.push_back(line);
        if (!write_ok ||
            (fail_on_write_number != 0 &&
             writes.size() >= fail_on_write_number)) {
            if (error_out) {
                *error_out = write_error;
            }
            return false;
        }
        return true;
    }

    ipc::SpatialRoiIpcTransportReadResult ReadLine(
        const std::chrono::milliseconds timeout,
        const std::size_t max_wire_bytes) override
    {
        read_timeouts.push_back(timeout);
        read_bounds.push_back(max_wire_bytes);
        if (reads.empty()) {
            return {ipc::SpatialRoiIpcTransportReadStatus::kEof,
                    {},
                    "script exhausted"};
        }
        ipc::SpatialRoiIpcTransportReadResult result = std::move(reads.front());
        reads.erase(reads.begin());
        if (result.status == ipc::SpatialRoiIpcTransportReadStatus::kLine &&
            result.line.size() > max_wire_bytes) {
            result.line.clear();
            result.status = ipc::SpatialRoiIpcTransportReadStatus::kTooLarge;
            result.error = "scripted line exceeds bound";
        }
        return result;
    }
};

std::string wire(const ipc::SpatialRoiIpcMessage& message)
{
    std::string error;
    const std::string result =
        ipc::serialize_spatial_roi_ipc_message(message, &error);
    require(!result.empty(), "message serialization failed: " + error);
    return result;
}

SpatialRoiFrameDescriptor make_descriptor(const std::uint64_t frame_index,
                                          const std::string& camera_serial =
                                              "camera_1")
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = "recorder-session-test";
    descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            descriptor.recording_id);
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 0;
    descriptor.camera_serial = camera_serial;
    descriptor.local_frame_id = frame_index;
    descriptor.camera_frame_id = 100 + frame_index;
    descriptor.recording_frame_id = frame_index;
    descriptor.roi_stream_frame_index = frame_index;
    descriptor.camera_timestamp_ns = 1000 + frame_index;
    descriptor.timestamp_sys_ns = 2000 + frame_index;
    descriptor.roi_id = "roi_1";
    descriptor.region_id = "region_1";
    descriptor.arena_group_id = "group_1";
    descriptor.arena_id = "";
    descriptor.logical_stream_id =
        camera_serial + "_spatial_roi_roi_1";
    descriptor.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'a');
    descriptor.native_raster = {8, 8};
    descriptor.content_rect = {0, 0, 4, 4};
    descriptor.encoded_raster = {4, 4};
    descriptor.encoded_content_rect = {0, 0, 4, 4};
    descriptor.padding = {0, 0, 0, 0, 0};
    descriptor.source_pixel_format = "mono8";
    descriptor.bytes = 16;
    descriptor.source_gpu_id = 0;
    descriptor.assigned_gpu_id = 0;
    descriptor.assigned_shard_id = 0;
    descriptor.routing_policy = "single_shard";
    return descriptor;
}

ipc::SpatialRoiIpcFrame make_frame(const SpatialRoiFrameDescriptor& descriptor)
{
    ipc::SpatialRoiIpcFrame frame;
    frame.descriptor = descriptor;
    frame.cuda_buffer.memory_handle_hex = std::string(128, 'a');
    frame.cuda_buffer.ready_event_handle_hex = std::string(128, 'b');
    frame.cuda_buffer.byte_offset = 0;
    frame.cuda_buffer.byte_length = descriptor.bytes;
    frame.cuda_buffer.row_pitch_bytes = descriptor.encoded_raster.width;
    frame.cuda_buffer.pixel_format = "mono8";
    frame.cuda_buffer.layout = "packed_row_major";
    return frame;
}

ipc::SpatialRoiRecorderIpcDispatchResult enqueued_dispatch()
{
    return {ipc::SpatialRoiRecorderIpcDispatchStatus::kEnqueued,
            "detached", true, {}};
}

ipc::SpatialRoiRecorderIpcDispatchResult rejected_dispatch(
    const std::string& reason)
{
    return {ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected,
            reason == "detach_pool_exhausted" ? "pool_exhausted" : "detached",
            true,
            reason};
}

ipc::SpatialRoiRecorderIpcDispatchResult uncertain_dispatch()
{
    return {ipc::SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain,
            "source_quarantined", false,
            "cuda_source_ownership_uncertain"};
}

ipc::SpatialRoiIpcHello make_producer_hello(
    const ipc::SpatialRoiIpcStreamIdentity& stream)
{
    return {stream,
            ipc::kSpatialRoiIpcProducerRole,
            2,
            ipc::spatial_roi_ipc_required_features()};
}

std::unique_ptr<ipc::SpatialRoiRecorderIpcSession> make_session(
    ScriptedTransport* transport,
    const SpatialRoiFrameDescriptor& descriptor,
    ipc::SpatialRoiRecorderIpcDispatch dispatch,
    ipc::SpatialRoiRecorderIpcFrameOutcomeObserver observer = {})
{
    ipc::SpatialRoiRecorderIpcSessionConfig config;
    config.expected_stream =
        ipc::spatial_roi_ipc_stream_identity_from_descriptor(descriptor);
    config.queue_capacity_frames = 2;
    config.response_timeout = std::chrono::milliseconds(17);
    auto session = std::make_unique<ipc::SpatialRoiRecorderIpcSession>(
        *transport,
        std::move(config),
        std::move(dispatch),
        std::move(observer));
    require(session->valid(), "session construction failed: " + session->error());
    return session;
}

void prime_hello(ScriptedTransport* transport,
                 const SpatialRoiFrameDescriptor& descriptor)
{
    transport->reads.push_back(
        {ipc::SpatialRoiIpcTransportReadStatus::kLine,
         wire(make_producer_hello(
             ipc::spatial_roi_ipc_stream_identity_from_descriptor(descriptor))),
         {}});
}

void append_frame(ScriptedTransport* transport,
                  const SpatialRoiFrameDescriptor& descriptor)
{
    transport->reads.push_back(
        {ipc::SpatialRoiIpcTransportReadStatus::kLine,
         wire(make_frame(descriptor)),
         {}});
}

void append_eof(ScriptedTransport* transport)
{
    transport->reads.push_back(
        {ipc::SpatialRoiIpcTransportReadStatus::kEof, {}, "clean EOF"});
}

ipc::SpatialRoiIpcMessage parse_write(const std::string& value)
{
    ipc::SpatialRoiIpcMessage message;
    std::string error;
    require(ipc::parse_spatial_roi_ipc_message(value, &message, &error),
            "write parse failed: " + error);
    return message;
}

void require_terminal_error(const ScriptedTransport& transport)
{
    if (transport.writes.empty()) {
        std::string detail = "fatal session did not write terminal error (writes=" +
                             std::to_string(transport.writes.size()) + ")";
        if (!transport.writes.empty()) {
            detail += " first=" + transport.writes.front();
        }
        throw std::runtime_error(detail);
    }
    const ipc::SpatialRoiIpcMessage terminal =
        parse_write(transport.writes.back());
    require(std::holds_alternative<ipc::SpatialRoiIpcTerminalError>(terminal),
            "last write was not TERMINAL_ERROR");
}

void test_valid_multiple_frames_and_clean_eof()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    prime_hello(&transport, descriptor);
    append_frame(&transport, descriptor);
    append_frame(&transport, make_descriptor(2));
    append_eof(&transport);
    std::uint64_t dispatched = 0;
    auto session = make_session(
        &transport,
        descriptor,
        [&dispatched](const ipc::SpatialRoiIpcFrame&) {
            ++dispatched;
            return enqueued_dispatch();
        });
    std::string error;
    require(session->Negotiate(&error), "Negotiate failed: " + error);
    const auto result = session->Run();
    require(result.clean_eof(), "valid stream did not end at clean EOF");
    require(dispatched == 2, "valid stream dispatch count mismatch");
    require(transport.writes.size() == 5,
            "expected recorder HELLO plus two ACK/RELEASE pairs");
    require(session->counters().frames_enqueued == 2,
            "enqueued counter mismatch");
    require(session->counters().acks_sent == 2 &&
                session->counters().releases_sent == 2,
            "ACK/RELEASE counters mismatch");
    require(session->Run().clean_eof(), "clean EOF was not sticky");
}

void test_hello_mismatch_is_fatal()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    auto session = make_session(
        &transport,
        descriptor,
        [](const ipc::SpatialRoiIpcFrame&) {
            return enqueued_dispatch();
        });
    auto hello = make_producer_hello(
        ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            make_descriptor(1, "camera_2")));
    transport.reads.push_back(
        {ipc::SpatialRoiIpcTransportReadStatus::kLine, wire(hello), {}});
    std::string error;
    require(!session->Negotiate(&error), "mismatched HELLO was accepted");
    require(session->fatal_latched(), "HELLO mismatch did not latch fatal");
    require_terminal_error(transport);
}

void test_queue_mismatch_and_eof_before_hello_are_fatal()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    const auto dispatch = [](const ipc::SpatialRoiIpcFrame&) {
        return enqueued_dispatch();
    };
    {
        ScriptedTransport transport;
        auto session = make_session(&transport, descriptor, dispatch);
        auto hello = make_producer_hello(
            ipc::spatial_roi_ipc_stream_identity_from_descriptor(descriptor));
        hello.queue_capacity_frames = 1;
        transport.reads.push_back(
            {ipc::SpatialRoiIpcTransportReadStatus::kLine, wire(hello), {}});
        std::string error;
        require(!session->Negotiate(&error),
                "mismatched producer queue capacity was accepted");
        require(session->fatal_latched(),
                "mismatched producer queue capacity was not fatal");
        require_terminal_error(transport);
    }
    {
        ScriptedTransport transport;
        auto session = make_session(&transport, descriptor, dispatch);
        append_eof(&transport);
        std::string error;
        require(!session->Negotiate(&error), "EOF before HELLO was accepted");
        require(session->fatal_latched() && !session->clean_eof(),
                "EOF before HELLO was classified as clean");
        require_terminal_error(transport);
    }
}

void test_gap_and_duplicate_are_fatal()
{
    for (const std::uint64_t bad_index : {3ULL, 1ULL}) {
        ScriptedTransport transport;
        const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
        prime_hello(&transport, descriptor);
        append_frame(&transport, descriptor);
        append_frame(&transport, make_descriptor(bad_index));
        auto session = make_session(
            &transport,
            descriptor,
            [](const ipc::SpatialRoiIpcFrame&) {
                return enqueued_dispatch();
            });
        std::string error;
        require(session->Negotiate(&error), "gap/duplicate HELLO failed");
        const auto result = session->Run();
        require(result.fatal(), "gap/duplicate was accepted");
        require_terminal_error(transport);
        if (bad_index == 3) {
            require(session->counters().frame_gaps == 1,
                    "gap was not classified as a gap");
        } else {
            require(session->counters().duplicate_frames == 1,
                    "duplicate was not classified as a duplicate");
        }
    }
}

void test_safe_detach_rejection_and_enqueue_rejection_release_source()
{
    for (const std::string reason : {"detach_pool_exhausted",
                                     "encoder_queue_full"}) {
        ScriptedTransport transport;
        const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
        prime_hello(&transport, descriptor);
        append_frame(&transport, descriptor);
        append_eof(&transport);
        auto session = make_session(
            &transport,
            descriptor,
            [reason](const ipc::SpatialRoiIpcFrame&) {
                return rejected_dispatch(reason);
            });
        std::string error;
        require(session->Negotiate(&error), "rejection HELLO failed");
        require(session->Run().clean_eof(), "safe rejection was fatal");
        require(session->counters().frames_rejected == 1,
                "rejection counter mismatch");
        require(transport.writes.size() == 3,
                "safe rejection did not write HELLO/ACK/RELEASE");
        const auto ack = parse_write(transport.writes[1]);
        require(std::holds_alternative<ipc::SpatialRoiIpcAck>(ack),
                "safe rejection did not send ACK");
        require(!std::get<ipc::SpatialRoiIpcAck>(ack).accepted,
                "safe rejection ACK was accepted");
        require(std::holds_alternative<ipc::SpatialRoiIpcRelease>(
                    parse_write(transport.writes[2])),
                "safe rejection did not RELEASE source");
    }
}

void test_invalid_dispatch_status_combinations_are_fatal()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    struct InvalidDispatchCase {
        ipc::SpatialRoiRecorderIpcDispatchResult result;
        const char* name;
    };
    const InvalidDispatchCase cases[] = {
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kEnqueued,
          "pool_exhausted", true, {}},
         "enqueued with non-detached status"},
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected,
          "source_quarantined", true, "source was quarantined"},
         "rejected with quarantined source marked safe"},
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected,
          "pool_exhausted", false, "pool exhausted"},
         "rejected with unsafe source marked safe"},
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain,
          "detached", false, "ownership uncertain"},
         "uncertain with detached status"},
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain,
          "source_quarantined", true, "ownership uncertain"},
         "uncertain with quarantined source marked safe"},
        {{ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected,
          "future_status", true, "rejected"},
         "unknown detach status"},
    };
    for (const auto& test : cases) {
        ScriptedTransport transport;
        prime_hello(&transport, descriptor);
        append_frame(&transport, descriptor);
        auto session = make_session(
            &transport,
            descriptor,
            [result = test.result](const ipc::SpatialRoiIpcFrame&) {
                return result;
            });
        std::string error;
        require(session->Negotiate(&error),
                std::string(test.name) + ": HELLO failed: " + error);
        require(session->Run().fatal(),
                std::string(test.name) + ": invalid dispatch was accepted");
        require(transport.writes.size() == 2,
                std::string(test.name) +
                    ": invalid dispatch emitted ACK/RELEASE");
        require_terminal_error(transport);
        require(session->counters().dispatch_failures == 1,
                std::string(test.name) +
                    ": invalid dispatch was not counted as a failure");
    }
}

void test_uncertain_source_ownership_is_fatal_without_ack_release()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    prime_hello(&transport, descriptor);
    append_frame(&transport, descriptor);
    std::vector<ipc::SpatialRoiRecorderIpcFrameOutcome> outcomes;
    auto session = make_session(
        &transport,
        descriptor,
        [](const ipc::SpatialRoiIpcFrame&) {
            return uncertain_dispatch();
        },
        [&outcomes](const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
                    std::string*) {
            outcomes.push_back(outcome);
            return true;
        });
    std::string error;
    require(session->Negotiate(&error), "uncertain ownership HELLO failed");
    require(session->Run().fatal(), "uncertain ownership was accepted");
    require(transport.writes.size() == 2,
            "uncertain ownership wrote ACK/RELEASE");
    require_terminal_error(transport);
    require(session->counters().ownership_uncertain == 1,
            "ownership uncertainty counter mismatch");
    require(outcomes.size() == 1 &&
                outcomes[0].dispatch.detach_status == "source_quarantined" &&
                !outcomes[0].dispatch.source_release_safe &&
                !outcomes[0].dispatch.accepted() &&
                !outcomes[0].ack_attempted && !outcomes[0].ack_sent &&
                !outcomes[0].release_attempted && !outcomes[0].release_sent,
            "uncertain ownership outcome invented ACK/RELEASE state");
}

void test_malformed_and_timeout_are_fatal()
{
    for (const auto& read : {
             ipc::SpatialRoiIpcTransportReadResult{
                 ipc::SpatialRoiIpcTransportReadStatus::kLine,
                 "not json\n",
                 {}},
             ipc::SpatialRoiIpcTransportReadResult{
                 ipc::SpatialRoiIpcTransportReadStatus::kTimeout,
                 {},
                 "script timeout"}}) {
        ScriptedTransport transport;
        const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
        prime_hello(&transport, descriptor);
        auto session = make_session(
            &transport,
            descriptor,
            [](const ipc::SpatialRoiIpcFrame&) {
                return enqueued_dispatch();
            });
        std::string error;
        require(session->Negotiate(&error), "malformed/timeout HELLO failed");
        transport.reads.push_back(read);
        require(session->Run().fatal(), "malformed/timeout was accepted");
        require_terminal_error(transport);
    }
}

void test_producer_eof_after_hello_is_clean()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    prime_hello(&transport, descriptor);
    append_eof(&transport);
    auto session = make_session(
        &transport,
        descriptor,
        [](const ipc::SpatialRoiIpcFrame&) {
            return enqueued_dispatch();
        });
    std::string error;
    require(session->Negotiate(&error), "EOF HELLO failed");
    require(session->Run().clean_eof(), "producer EOF was not clean");
    require(transport.writes.size() == 1,
            "clean EOF wrote a terminal error or unexpected message");
}

void test_frame_outcome_observer_sees_exact_wire_truth()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    prime_hello(&transport, descriptor);
    append_frame(&transport, descriptor);
    append_eof(&transport);
    std::vector<ipc::SpatialRoiRecorderIpcFrameOutcome> outcomes;
    auto session = make_session(
        &transport,
        descriptor,
        [](const ipc::SpatialRoiIpcFrame&) {
            return enqueued_dispatch();
        },
        [&outcomes](const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
                    std::string*) {
            outcomes.push_back(outcome);
            return true;
        });
    std::string error;
    require(session->Negotiate(&error), "observer HELLO failed: " + error);
    require(session->Run().clean_eof(), "observer stream was not clean");
    require(outcomes.size() == 1 &&
                outcomes[0].descriptor.roi_stream_frame_index == 1 &&
                outcomes[0].dispatch.accepted() &&
                outcomes[0].dispatch.detach_status == "detached" &&
                outcomes[0].dispatch.source_release_safe &&
                outcomes[0].ack_attempted && outcomes[0].ack_sent &&
                outcomes[0].ack_accepted && outcomes[0].ack_reason.empty() &&
                outcomes[0].ack_error.empty() &&
                outcomes[0].release_attempted && outcomes[0].release_sent &&
                outcomes[0].release_reason == "source_detached",
            "observer did not receive exact accepted ACK/RELEASE outcome");
    require(session->counters().frame_outcomes_reported == 1 &&
                session->counters().frame_outcome_failures == 0,
            "observer success counters mismatch");
}

void test_frame_outcome_observer_sees_write_failures()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    for (const std::size_t failing_write : {2U, 3U}) {
        ScriptedTransport transport;
        transport.fail_on_write_number = failing_write;
        prime_hello(&transport, descriptor);
        append_frame(&transport, descriptor);
        std::vector<ipc::SpatialRoiRecorderIpcFrameOutcome> outcomes;
        auto session = make_session(
            &transport,
            descriptor,
            [](const ipc::SpatialRoiIpcFrame&) { return enqueued_dispatch(); },
            [&outcomes](const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
                        std::string*) {
                outcomes.push_back(outcome);
                return true;
            });
        std::string error;
        require(session->Negotiate(&error),
                "write-failure HELLO failed: " + error);
        require(session->Run().fatal(), "write failure was not terminal");
        require(outcomes.size() == 1,
                "write failure did not report exactly one frame outcome");
        const auto& outcome = outcomes.front();
        require(outcome.dispatch.accepted() && outcome.ack_attempted &&
                    outcome.ack_accepted && outcome.ack_reason.empty(),
                "write failure lost dispatch/ACK payload truth");
        if (failing_write == 2) {
            require(!outcome.ack_sent && !outcome.ack_error.empty() &&
                        !outcome.release_attempted && !outcome.release_sent &&
                        outcome.release_reason.empty() &&
                        outcome.release_error.empty(),
                    "ACK write failure invented a RELEASE lifecycle");
        } else {
            require(outcome.ack_sent && outcome.ack_error.empty() &&
                        outcome.release_attempted && !outcome.release_sent &&
                        outcome.release_reason == "source_detached" &&
                        !outcome.release_error.empty(),
                    "RELEASE write failure did not preserve exact wire truth");
        }
    }
}

void test_frame_outcome_observer_failure_is_terminal_after_release()
{
    ScriptedTransport transport;
    const SpatialRoiFrameDescriptor descriptor = make_descriptor(1);
    prime_hello(&transport, descriptor);
    append_frame(&transport, descriptor);
    auto session = make_session(
        &transport,
        descriptor,
        [](const ipc::SpatialRoiIpcFrame&) {
            return enqueued_dispatch();
        },
        [](const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
           std::string* error_out) {
            require(outcome.ack_sent && outcome.release_sent,
                    "observer failure test ran before source release");
            if (error_out) {
                *error_out = "journal rejected outcome";
            }
            return false;
        });
    std::string error;
    require(session->Negotiate(&error), "observer-failure HELLO failed");
    require(session->Run().fatal(), "observer failure was not terminal");
    require(session->counters().frame_outcome_failures == 1 &&
                session->counters().releases_sent == 1,
            "observer failure lost release/counter truth");
    require_terminal_error(transport);
}

}  // namespace

int main()
{
    try {
        test_valid_multiple_frames_and_clean_eof();
        test_hello_mismatch_is_fatal();
        test_queue_mismatch_and_eof_before_hello_are_fatal();
        test_gap_and_duplicate_are_fatal();
        test_safe_detach_rejection_and_enqueue_rejection_release_source();
        test_invalid_dispatch_status_combinations_are_fatal();
        test_uncertain_source_ownership_is_fatal_without_ack_release();
        test_malformed_and_timeout_are_fatal();
        test_producer_eof_after_hello_is_clean();
        test_frame_outcome_observer_sees_exact_wire_truth();
        test_frame_outcome_observer_sees_write_failures();
        test_frame_outcome_observer_failure_is_terminal_after_release();
        std::cout << "spatial_roi_recorder_ipc_session_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_recorder_ipc_session_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
