#include "spatial_roi_ipc_handoff.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace api = orange::session::spatial_roi;
using orange::spatial_roi::SpatialRoiFrameDescriptor;
using orange::spatial_roi::ipc::SpatialRoiCudaIpcBuffer;
using orange::spatial_roi::ipc::SpatialRoiIpcAck;
using orange::spatial_roi::ipc::SpatialRoiIpcFrame;
using orange::spatial_roi::ipc::SpatialRoiIpcHandoff;
using orange::spatial_roi::ipc::SpatialRoiIpcHandoffConfig;
using orange::spatial_roi::ipc::SpatialRoiIpcHandoffResult;
using orange::spatial_roi::ipc::SpatialRoiIpcHandoffResultStatus;
using orange::spatial_roi::ipc::SpatialRoiIpcHello;
using orange::spatial_roi::ipc::SpatialRoiIpcLineTransport;
using orange::spatial_roi::ipc::SpatialRoiIpcRelease;
using orange::spatial_roi::ipc::SpatialRoiIpcTransportReadResult;
using orange::spatial_roi::ipc::SpatialRoiIpcTransportReadStatus;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScriptedTransport final : public SpatialRoiIpcLineTransport {
public:
    bool write_ok = true;
    std::string write_error = "write failed";
    std::size_t fail_on_write_number = 0;
    std::vector<std::string> writes;
    std::vector<SpatialRoiIpcTransportReadResult> reads;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::vector<std::size_t> read_max_wire_bytes;

    bool WriteLine(const std::string& line, std::string* error_out) override
    {
        writes.push_back(line);
        if ((!write_ok ||
             (fail_on_write_number != 0 &&
              writes.size() >= fail_on_write_number))) {
            if (error_out) {
                *error_out = write_error;
            }
            return false;
        }
        return true;
    }

    SpatialRoiIpcTransportReadResult ReadLine(
        const std::chrono::milliseconds timeout,
        const std::size_t max_wire_bytes) override
    {
        read_timeouts.push_back(timeout);
        read_max_wire_bytes.push_back(max_wire_bytes);
        if (reads.empty()) {
            return {SpatialRoiIpcTransportReadStatus::kEof, {}, "script exhausted"};
        }
        SpatialRoiIpcTransportReadResult result = std::move(reads.front());
        reads.erase(reads.begin());
        // A real adapter enforces this before buffering/materializing. Keep
        // the fake adapter honest as well, while the handoff retains a
        // defensive postcondition check for adapters that violate the seam.
        if (result.status == SpatialRoiIpcTransportReadStatus::kLine &&
            result.line.size() > max_wire_bytes) {
            result.line.clear();
            result.error = "fake adapter rejected oversized line";
            result.status = SpatialRoiIpcTransportReadStatus::kTooLarge;
        }
        return result;
    }
};

api::Config make_config()
{
    api::Config config = api::default_config();
    config.enabled = true;
    config.output_alignment_px = 2;
    config.buffering.pool_frames_per_stream = 2;
    config.buffering.queue_frames_per_stream = 2;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 1000000;

    api::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "10000";
    camera.native_raster = {8, 6};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "arena_group_1";
    camera.layout = {"layout_1", "sha256:" + std::string(64, '1')};
    camera.materialization = {
        "materialization_1", "sha256:" + std::string(64, '2')};
    camera.registration = {
        "registration_1", "sha256:" + std::string(64, '3')};
    const std::vector<api::Rect> rectangles = {
        {0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, {2, 2, 2, 2}};
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
        api::RoiConfig roi;
        roi.roi_id = "roi_" + std::to_string(index + 1);
        roi.region_id = "region_" + std::to_string(index + 1);
        roi.content_rect = rectangles[index];
        roi.logical_stream_id =
            api::expected_logical_stream_id(camera.camera_serial, roi.roi_id);
        roi.artifact_stem =
            api::expected_artifact_stem(camera.camera_serial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));
    return config;
}

nlohmann::json make_plan()
{
    api::PlanContext context;
    context.recording_id = "ipc-handoff-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_1";
    nlohmann::json plan;
    std::string error;
    require(api::build_plan(make_config(), context, &plan, nullptr, &error),
            "build_plan failed: " + error);
    require(api::verify_plan(plan, &error), "verify_plan failed: " + error);
    return plan;
}

SpatialRoiFrameDescriptor make_descriptor(const std::uint64_t frame_id = 1,
                                          const std::uint64_t roi_index = 1)
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = "ipc-handoff-test-recording";
    descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            descriptor.recording_id);
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 3;
    descriptor.camera_serial = "10000";
    descriptor.local_frame_id = frame_id;
    descriptor.camera_frame_id = frame_id + 100;
    descriptor.recording_frame_id = frame_id;
    descriptor.roi_stream_frame_index = roi_index;
    descriptor.camera_timestamp_ns = frame_id + 1000;
    descriptor.timestamp_sys_ns = frame_id + 2000;
    descriptor.roi_id = "roi_1";
    descriptor.region_id = "region_1";
    descriptor.arena_group_id = "arena_group_1";
    descriptor.arena_id = "";
    descriptor.logical_stream_id = "10000_spatial_roi_roi_1";
    descriptor.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'a');
    descriptor.native_raster = {8, 6};
    descriptor.content_rect = {0, 0, 2, 2};
    descriptor.encoded_raster = {2, 2};
    descriptor.encoded_content_rect = {0, 0, 2, 2};
    descriptor.padding = {0, 0, 0, 0, 0};
    descriptor.source_pixel_format = "mono8";
    descriptor.bytes = 4;
    descriptor.source_gpu_id = 0;
    descriptor.assigned_gpu_id = 0;
    descriptor.assigned_shard_id = 0;
    descriptor.routing_policy = "single_shard";
    return descriptor;
}

SpatialRoiIpcFrame make_frame(const SpatialRoiFrameDescriptor& descriptor)
{
    SpatialRoiIpcFrame frame;
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

std::string serialize_ack(const SpatialRoiFrameDescriptor& descriptor,
                          const bool accepted)
{
    std::string error;
    const SpatialRoiIpcAck ack{
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor),
        accepted,
        accepted ? "" : "queue_full"};
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(ack, &error);
    require(!wire.empty(), "ACK serialization failed: " + error);
    return wire;
}

std::string serialize_release(const SpatialRoiFrameDescriptor& descriptor)
{
    std::string error;
    const SpatialRoiIpcRelease release{
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor),
        "source_consumed"};
    const std::string wire = orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
        release, &error);
    require(!wire.empty(), "RELEASE serialization failed: " + error);
    return wire;
}

std::string serialize_peer_hello(const SpatialRoiFrameDescriptor& descriptor)
{
    std::string error;
    const SpatialRoiIpcHello hello{
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor),
        "recorder",
        2,
        {"cuda_ipc",
         "packed_mono8",
         "ack_release",
         "terminal_error"}};
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(hello, &error);
    require(!wire.empty(), "peer HELLO serialization failed: " + error);
    return wire;
}

std::unique_ptr<SpatialRoiIpcHandoff> make_handoff(
    ScriptedTransport* transport,
    const SpatialRoiFrameDescriptor& descriptor,
    const bool negotiate = true)
{
    static nlohmann::json plan = make_plan();
    // The exporter owns no CUDA allocation during construction and is safe to
    // use for host protocol tests.  It must outlive the handoff.
    static orange::spatial_roi::ipc::SpatialRoiIpcFrameExporter exporter(
        plan, "10000", 0);
    SpatialRoiIpcHandoffConfig config;
    config.expected_stream =
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor);
    config.max_outstanding_frames = 2;
    config.response_timeout = std::chrono::milliseconds(37);
    auto handoff = std::make_unique<SpatialRoiIpcHandoff>(
        exporter, *transport, std::move(config));
    require(handoff->valid(),
            "host handoff construction failed: " + handoff->error());
    if (negotiate) {
        transport->reads.insert(transport->reads.begin(),
                                {SpatialRoiIpcTransportReadStatus::kLine,
                                 serialize_peer_hello(descriptor),
                                 {}});
        std::string error;
        require(handoff->Negotiate(&error), "HELLO negotiation failed: " + error);
        require(handoff->negotiated(), "HELLO did not set negotiated state");
    }
    return handoff;
}

SpatialRoiIpcHandoffResult submit_prepared(
    const std::unique_ptr<SpatialRoiIpcHandoff>& handoff,
    const SpatialRoiFrameDescriptor& descriptor,
    std::shared_ptr<void> owner)
{
    return handoff->SubmitPreparedForTest({make_frame(descriptor), {}},
                                          std::move(owner));
}

SpatialRoiIpcHandoffResult submit_prepared(
    const std::unique_ptr<SpatialRoiIpcHandoff>& handoff,
    const SpatialRoiFrameDescriptor& descriptor)
{
    return submit_prepared(handoff, descriptor, std::make_shared<int>(7));
}

void test_completed_and_rejected()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    for (const bool accepted : {true, false}) {
        ScriptedTransport transport;
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kLine,
             serialize_ack(descriptor, accepted),
             {}});
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kLine,
             serialize_release(descriptor),
             {}});
        auto handoff = make_handoff(&transport, descriptor);
        require(handoff->valid(),
                "host handoff construction failed: " + handoff->error());

        SpatialRoiIpcHandoffResult result = submit_prepared(handoff, descriptor);
        require(result.status == (accepted
                                      ? SpatialRoiIpcHandoffResultStatus::kCompleted
                                      : SpatialRoiIpcHandoffResultStatus::kRejected),
                "ACK/RELEASE did not produce expected terminal result");
        require(transport.writes.size() == 2, "HELLO or FRAME was retransmitted");
        require(handoff->outstanding_count() == 0,
                "exact RELEASE did not release the export");
        require(handoff->counters().frames_inserted == 1,
                "FRAME was not inserted before write");
        require(handoff->counters().frames_written == 1,
                "FRAME write counter mismatch");
        require(handoff->counters().releases == 1,
                "RELEASE counter mismatch");
        require(transport.read_max_wire_bytes.size() == 3,
                "HELLO/ACK/RELEASE read count mismatch");
        require(std::all_of(transport.read_max_wire_bytes.begin(),
                            transport.read_max_wire_bytes.end(),
                            [](const std::size_t value) {
                                return value ==
                                       orange::spatial_roi::ipc::
                                           kSpatialRoiIpcMaxWireMessageBytes;
                            }),
                "handoff did not pass the bounded wire size to transport");
    }
}

void test_negotiation_is_required_and_exact()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();

    ScriptedTransport not_negotiated_transport;
    auto not_negotiated =
        make_handoff(&not_negotiated_transport, descriptor, false);
    const SpatialRoiIpcHandoffResult before_hello =
        submit_prepared(not_negotiated, descriptor);
    require(before_hello.status ==
                SpatialRoiIpcHandoffResultStatus::kNotNegotiated,
            "Submit before HELLO was not rejected");
    require(not_negotiated_transport.writes.empty(),
            "Submit before HELLO wrote a FRAME");
    require(!not_negotiated->fatal_latched(),
            "Submit before HELLO unexpectedly latched fatal");

    struct HelloCase {
        std::string name;
        std::string role;
        std::uint32_t queue_capacity;
        std::vector<std::string> features;
    };
    const std::vector<HelloCase> cases = {
        {"wrong_role", "producer", 2,
         {"cuda_ipc", "packed_mono8", "ack_release", "terminal_error"}},
        {"missing_feature", "recorder", 2,
         {"cuda_ipc", "packed_mono8", "ack_release"}},
        // drain_finalize remains part of the broader wire grammar, but is not
        // activated by this handoff's HELLO contract.
        {"extra_feature", "recorder", 2,
         {"cuda_ipc", "packed_mono8", "ack_release", "terminal_error",
          "drain_finalize"}},
        {"reordered_features", "recorder", 2,
         {"packed_mono8", "cuda_ipc", "ack_release", "terminal_error"}},
        {"mismatched_queue", "recorder", 1,
         {"cuda_ipc", "packed_mono8", "ack_release", "terminal_error"}},
    };
    for (const HelloCase& hello_case : cases) {
        ScriptedTransport transport;
        auto handoff =
            make_handoff(&transport, descriptor, false);
        const SpatialRoiIpcHello hello{
            orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
                descriptor),
            hello_case.role,
            hello_case.queue_capacity,
            hello_case.features};
        std::string error;
        const std::string wire =
            orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(hello,
                                                                          &error);
        require(!wire.empty(), hello_case.name + " HELLO setup failed: " + error);
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kLine, wire, {}});
        require(!handoff->Negotiate(&error),
                hello_case.name + " HELLO was accepted");
        require(handoff->fatal_latched(), hello_case.name + " was not fatal");
        require(transport.writes.size() == 1,
                hello_case.name + " sent more than producer HELLO");
        require(handoff->counters().hello_sent == 1,
                hello_case.name + " HELLO sent counter mismatch");

        orange::spatial_roi::ipc::SpatialRoiIpcMessage producer_message;
        std::string producer_error;
        require(orange::spatial_roi::ipc::parse_spatial_roi_ipc_message(
                    transport.writes.front(), &producer_message, &producer_error),
                hello_case.name + " producer HELLO was malformed: " +
                    producer_error);
        require(std::holds_alternative<SpatialRoiIpcHello>(producer_message),
                hello_case.name + " producer did not send HELLO");
        const SpatialRoiIpcHello& producer_hello =
            std::get<SpatialRoiIpcHello>(producer_message);
        require(producer_hello.role == "producer",
                hello_case.name + " producer HELLO role mismatch");
        require(producer_hello.queue_capacity_frames == 2,
                hello_case.name + " producer HELLO queue mismatch");
        require(producer_hello.features ==
                    std::vector<std::string>{"cuda_ipc", "packed_mono8",
                                             "ack_release", "terminal_error"},
                hello_case.name + " producer HELLO feature vector mismatch");
    }

    ScriptedTransport stream_transport;
    auto stream_mismatch =
        make_handoff(&stream_transport, descriptor, false);
    SpatialRoiFrameDescriptor other_stream = descriptor;
    other_stream.camera_serial = "10001";
    other_stream.logical_stream_id = "10001_spatial_roi_roi_1";
    const SpatialRoiIpcHello mismatched_hello{
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            other_stream),
        "recorder",
        2,
        {"cuda_ipc", "packed_mono8", "ack_release", "terminal_error"}};
    std::string mismatch_error;
    const std::string mismatch_wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            mismatched_hello, &mismatch_error);
    require(!mismatch_wire.empty(),
            "stream mismatch HELLO setup failed: " + mismatch_error);
    stream_transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine, mismatch_wire, {}});
    require(!stream_mismatch->Negotiate(&mismatch_error),
            "stream mismatch HELLO was accepted");
    require(stream_mismatch->fatal_latched(),
            "stream mismatch HELLO was not fatal");
}

void test_ack_is_not_release_and_peer_exit_clears()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    ScriptedTransport transport;
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_ack(descriptor, true),
         {}});
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kTimeout, {}, "deadline"});
    auto handoff = make_handoff(&transport, descriptor);
    const SpatialRoiIpcHandoffResult result = submit_prepared(handoff, descriptor);
    require(result.fatal(), "missing RELEASE was not fatal");
    require(handoff->ownership_indeterminate(),
            "timed-out RELEASE returned ownership prematurely");
    require(handoff->outstanding_count() == 1,
            "fatal path did not retain full export in outstanding table");
    require(transport.writes.size() == 2, "fatal path retransmitted HELLO/FRAME");
    require(handoff->counters().ack_accepted == 1,
            "ACK was not recorded as admission");
    require(handoff->counters().releases == 0,
            "ACK incorrectly counted as RELEASE");
    require(handoff->ConfirmPeerExited(), "peer-exit confirmation was rejected");
    require(!handoff->ownership_indeterminate() && handoff->outstanding_count() == 0,
            "peer-exit confirmation did not clear retained ownership");
    require(handoff->fatal_latched(), "peer-exit confirmation cleared fatal latch");
}

void test_owner_lifetime_and_destructor_quarantine()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    ScriptedTransport null_owner_transport;
    auto null_owner_handoff =
        make_handoff(&null_owner_transport, descriptor);
    const std::size_t null_owner_writes = null_owner_transport.writes.size();
    require(null_owner_handoff->SubmitPreparedForTest(
                {make_frame(descriptor), {}}, {})
                .status == SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
            "null prepared owner was accepted");
    require(null_owner_transport.writes.size() == null_owner_writes,
            "null prepared owner wrote a FRAME");

    const std::uint64_t quarantines_before =
        SpatialRoiIpcHandoff::quarantined_destructor_count();
    std::weak_ptr<void> weak_owner;
    std::shared_ptr<void> owner;
    {
        ScriptedTransport transport;
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kLine,
             serialize_ack(descriptor, true), {}});
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kEof, {}, "peer died"});
        auto handoff = make_handoff(&transport, descriptor);
        owner = std::make_shared<int>(19);
        weak_owner = owner;
        require(submit_prepared(handoff, descriptor, owner).fatal(),
                "owner lifetime setup did not become fatal");
        owner.reset();
        require(!weak_owner.expired(),
                "fatal handoff released retained test owner too early");
        require(handoff->ConfirmPeerExited(),
                "owner lifetime peer-exit confirmation failed");
        require(weak_owner.expired(),
                "peer-exit confirmation did not release retained test owner");
    }
    require(SpatialRoiIpcHandoff::quarantined_destructor_count() ==
                quarantines_before,
            "confirmed peer exit unnecessarily quarantined the table");

    std::weak_ptr<void> quarantined_owner;
    {
        ScriptedTransport transport;
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kLine,
             serialize_ack(descriptor, true), {}});
        transport.reads.push_back(
            {SpatialRoiIpcTransportReadStatus::kTimeout, {}, "deadline"});
        auto handoff = make_handoff(&transport, descriptor);
        std::shared_ptr<void> owner_to_quarantine = std::make_shared<int>(23);
        quarantined_owner = owner_to_quarantine;
        require(submit_prepared(handoff, descriptor, owner_to_quarantine).fatal(),
                "destructor quarantine setup did not become fatal");
        owner_to_quarantine.reset();
        require(!quarantined_owner.expired(),
                "handoff did not retain owner before destructor");
    }
    require(!quarantined_owner.expired(),
            "destructor dropped indeterminate owner instead of quarantining");
    require(SpatialRoiIpcHandoff::quarantined_destructor_count() ==
                quarantines_before + 1,
            "destructor quarantine counter did not advance exactly once");
}

void test_fatal_protocol_cases()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    const std::string release = serialize_release(descriptor);
    const std::string ack = serialize_ack(descriptor, true);

    struct Case {
        std::string name;
        std::vector<SpatialRoiIpcTransportReadResult> reads;
    };

    SpatialRoiFrameDescriptor other = make_descriptor(2, 2);
    std::vector<Case> cases;
    cases.push_back({"malformed", {{SpatialRoiIpcTransportReadStatus::kLine,
                                     "not-json", {}}}});
    cases.push_back({"release_before_ack",
                     {{SpatialRoiIpcTransportReadStatus::kLine, release, {}}}});
    cases.push_back({"mismatched_ack",
                     {{SpatialRoiIpcTransportReadStatus::kLine,
                       serialize_ack(other, true), {}}}});
    cases.push_back({"eof", {{SpatialRoiIpcTransportReadStatus::kEof, {}, {}}}});
    cases.push_back({"timeout",
                     {{SpatialRoiIpcTransportReadStatus::kTimeout, {}, {}}}});
    cases.push_back({"oversized",
                     {{SpatialRoiIpcTransportReadStatus::kTooLarge, {}, {}}}});

    for (const Case& test_case : cases) {
        ScriptedTransport transport;
        transport.reads = test_case.reads;
        auto handoff = make_handoff(&transport, descriptor);
        const SpatialRoiIpcHandoffResult result =
            submit_prepared(handoff, descriptor);
        require(result.fatal(), test_case.name + " was not fatal");
        require(handoff->outstanding_count() == 1,
                test_case.name + " discarded indeterminate export");
        require(transport.writes.size() == 2,
                test_case.name + " caused a retransmit");
        require(handoff->ConfirmPeerExited(),
                test_case.name + " could not confirm peer exit");
    }

    // A valid ACK followed by an identical ACK is a duplicate, not an
    // implicit release.  It must leave the export retained.
    ScriptedTransport duplicate_transport;
    duplicate_transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine, ack, {}});
    duplicate_transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine, ack, {}});
    auto duplicate = make_handoff(&duplicate_transport, descriptor);
    require(submit_prepared(duplicate, descriptor).fatal(),
            "duplicate ACK was not fatal");
    require(duplicate->counters().duplicate_messages == 1,
            "duplicate ACK counter mismatch");
    require(duplicate->outstanding_count() == 1,
            "duplicate ACK released the export");
}

void test_local_duplicate_and_write_failure()
{
    const SpatialRoiFrameDescriptor descriptor = make_descriptor();
    ScriptedTransport transport;
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_ack(descriptor, true), {}});
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_release(descriptor), {}});
    auto handoff = make_handoff(&transport, descriptor);
    require(submit_prepared(handoff, descriptor).completed(),
            "initial frame did not complete");
    require(submit_prepared(handoff, descriptor).fatal(),
            "local duplicate FRAME was not fatal");
    require(transport.writes.size() == 2, "local duplicate was retransmitted");

    ScriptedTransport failed_transport;
    failed_transport.fail_on_write_number = 2;
    auto failed = make_handoff(&failed_transport, descriptor);
    require(submit_prepared(failed, descriptor).fatal(),
            "transport write failure was not fatal");
    require(failed->outstanding_count() == 1,
            "write failure discarded retained export");
    require(failed_transport.read_timeouts.size() == 1,
            "write failure incorrectly waited for an ACK/RELEASE response");
}

void test_strictly_increasing_roi_index()
{
    const SpatialRoiFrameDescriptor first = make_descriptor(1, 1);
    const SpatialRoiFrameDescriptor second = make_descriptor(2, 2);
    ScriptedTransport transport;
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_ack(first, true), {}});
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_release(first), {}});
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_ack(second, true), {}});
    transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_release(second), {}});
    auto handoff = make_handoff(&transport, first);
    require(submit_prepared(handoff, first).completed(),
            "first monotonically indexed frame did not complete");
    require(submit_prepared(handoff, second).completed(),
            "second monotonically indexed frame did not complete");
    const std::size_t writes_before_old = transport.writes.size();
    require(submit_prepared(handoff, first).fatal(),
            "old ROI stream index beyond history was not fatal");
    require(transport.writes.size() == writes_before_old,
            "old ROI stream index was retransmitted");
    require(handoff->outstanding_count() == 0,
            "local old-index rejection created ownership without FRAME");
}

void test_dense_roi_index_start_and_gap()
{
    // The first index is part of the wire contract, not merely a positive
    // value.  A stream that starts at two must fail before any FRAME byte.
    const SpatialRoiFrameDescriptor starts_at_two = make_descriptor(1, 2);
    ScriptedTransport start_transport;
    auto start_handoff = make_handoff(&start_transport, starts_at_two);
    const std::size_t start_writes = start_transport.writes.size();
    require(submit_prepared(start_handoff, starts_at_two).fatal(),
            "ROI stream accepted a first index other than one");
    require(start_transport.writes.size() == start_writes,
            "invalid first ROI index wrote a FRAME");
    require(start_handoff->outstanding_count() == 0,
            "invalid first ROI index created retained ownership");

    // Once one is accepted, a gap is equally unsafe: the receiver cannot
    // distinguish a dropped frame from a producer restart.
    const SpatialRoiFrameDescriptor first = make_descriptor(1, 1);
    const SpatialRoiFrameDescriptor gap = make_descriptor(2, 3);
    ScriptedTransport gap_transport;
    gap_transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_ack(first, true), {}});
    gap_transport.reads.push_back(
        {SpatialRoiIpcTransportReadStatus::kLine,
         serialize_release(first), {}});
    auto gap_handoff = make_handoff(&gap_transport, first);
    require(submit_prepared(gap_handoff, first).completed(),
            "first dense ROI index did not complete");
    const std::size_t gap_writes = gap_transport.writes.size();
    require(submit_prepared(gap_handoff, gap).fatal(),
            "ROI stream accepted an index gap");
    require(gap_transport.writes.size() == gap_writes,
            "ROI index gap wrote a FRAME");
    require(gap_handoff->outstanding_count() == 0,
            "ROI index gap created retained ownership");
}

}  // namespace

int main()
{
    try {
        test_completed_and_rejected();
        test_negotiation_is_required_and_exact();
        test_ack_is_not_release_and_peer_exit_clears();
        test_owner_lifetime_and_destructor_quarantine();
        test_fatal_protocol_cases();
        test_local_duplicate_and_write_failure();
        test_strictly_increasing_roi_index();
        test_dense_roi_index_start_and_gap();
        std::cout << "spatial_roi_ipc_handoff_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_ipc_handoff_tests: FAIL: " << exception.what()
                  << '\n';
        return 1;
    }
}
