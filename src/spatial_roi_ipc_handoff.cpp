#include "spatial_roi_ipc_handoff.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>

namespace orange::spatial_roi::ipc {
namespace {

constexpr std::size_t kMaxRetainedErrorBytes = kSpatialRoiIpcMaxTextBytes;

std::atomic<std::uint64_t> g_quarantined_destructors{0};

void increment_atomic_saturating(std::atomic<std::uint64_t>* value) noexcept
{
    if (!value) {
        return;
    }
    std::uint64_t current = value->load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max() &&
           !value->compare_exchange_weak(current,
                                          current + 1,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

bool same_stream(const SpatialRoiIpcStreamIdentity& lhs,
                 const SpatialRoiIpcStreamIdentity& rhs) noexcept
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.roi_id == rhs.roi_id && lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256;
}

bool same_correlation(const SpatialRoiIpcCorrelation& lhs,
                      const SpatialRoiIpcCorrelation& rhs) noexcept
{
    return same_stream(lhs.stream, rhs.stream) &&
           lhs.local_frame_id == rhs.local_frame_id &&
           lhs.camera_frame_id == rhs.camera_frame_id &&
           lhs.recording_frame_id == rhs.recording_frame_id &&
           lhs.roi_stream_frame_index == rhs.roi_stream_frame_index;
}

std::string_view nonempty_or(const std::string& value,
                             const char* fallback) noexcept
{
    return value.empty() ? std::string_view(fallback) : std::string_view(value);
}

SpatialRoiIpcHandoffResult make_result(
    const SpatialRoiIpcHandoffResultStatus status,
    const SpatialRoiIpcCorrelation* correlation,
    const std::string_view error) noexcept
{
    SpatialRoiIpcHandoffResult result;
    result.status = status;
    try {
        if (correlation) {
            result.correlation = *correlation;
        }
        const std::size_t length =
            std::min(error.size(), kMaxRetainedErrorBytes);
        if (length == 0) {
            result.error.clear();
        } else {
            result.error.assign(error.data(), length);
        }
    } catch (...) {
        // Status is the authoritative result. A diagnostic copy must never
        // turn an exact RELEASE into an escaping exception.
        result.correlation.reset();
        result.error.clear();
    }
    return result;
}

std::string read_status_error(const SpatialRoiIpcTransportReadResult& read)
{
    if (!read.error.empty()) {
        return read.error;
    }
    switch (read.status) {
    case SpatialRoiIpcTransportReadStatus::kTimeout:
        return "spatial ROI IPC response timed out";
    case SpatialRoiIpcTransportReadStatus::kEof:
        return "spatial ROI IPC peer reached EOF";
    case SpatialRoiIpcTransportReadStatus::kError:
        return "spatial ROI IPC transport read failed";
    case SpatialRoiIpcTransportReadStatus::kTooLarge:
        return "spatial ROI IPC response exceeds maximum wire size";
    case SpatialRoiIpcTransportReadStatus::kLine:
        return "";
    }
    return "spatial ROI IPC transport returned an unknown read status";
}

}  // namespace

const char* spatial_roi_ipc_handoff_result_status_name(
    const SpatialRoiIpcHandoffResultStatus status) noexcept
{
    switch (status) {
    case SpatialRoiIpcHandoffResultStatus::kCompleted:
        return "completed";
    case SpatialRoiIpcHandoffResultStatus::kRejected:
        return "rejected";
    case SpatialRoiIpcHandoffResultStatus::kInvalidArgument:
        return "invalid_argument";
    case SpatialRoiIpcHandoffResultStatus::kBuildFailed:
        return "build_failed";
    case SpatialRoiIpcHandoffResultStatus::kOutstandingFull:
        return "outstanding_full";
    case SpatialRoiIpcHandoffResultStatus::kNotNegotiated:
        return "not_negotiated";
    case SpatialRoiIpcHandoffResultStatus::kFatal:
        return "fatal";
    }
    return "unknown";
}

SpatialRoiIpcHandoff::SpatialRoiIpcHandoff(
    SpatialRoiIpcFrameExporter& exporter,
    SpatialRoiIpcLineTransport& transport,
    SpatialRoiIpcHandoffConfig config)
    : exporter_(&exporter), transport_(&transport), config_(std::move(config))
{
    try {
        if (!exporter_->valid()) {
            error_ = "spatial ROI IPC handoff received an invalid exporter: " +
                     exporter_->error();
            return;
        }
        if (!validate_spatial_roi_ipc_stream_identity(
                config_.expected_stream, &error_)) {
            if (error_.empty()) {
                error_ = "invalid expected spatial ROI IPC stream identity";
            }
            return;
        }
        if (config_.max_outstanding_frames == 0 ||
            config_.max_outstanding_frames > kSpatialRoiIpcMaxQueueFrames) {
            error_ = "max_outstanding_frames is outside the bounded protocol range";
            return;
        }
        if (config_.response_timeout.count() <= 0) {
            error_ = "response_timeout must be positive";
            return;
        }

        // Reserve both bounded tables before the first submission.  Failure
        // to reserve makes this handoff unusable rather than allowing a
        // partially admitted export to escape through an allocation throw.
        outstanding_ = std::make_unique<OutstandingTable>();
        outstanding_->reserve(config_.max_outstanding_frames);
        valid_ = true;
    } catch (const std::exception& exception) {
        error_ = "spatial ROI IPC handoff construction failed: ";
        error_ += exception.what();
    } catch (...) {
        error_ = "spatial ROI IPC handoff construction failed";
    }
}

SpatialRoiIpcHandoff::~SpatialRoiIpcHandoff()
{
    // The table contains CUDA IPC exports whose producer-side allocations may
    // still be owned by a live peer.  Destruction cannot infer peer exit and
    // must not release those envelopes merely because the owner object is
    // going away.  Releasing the unique_ptr intentionally quarantines the
    // table and all retained exports with no allocation or cleanup work.
    if (outstanding_ && !outstanding_->empty() &&
        !peer_exited_confirmed_) {
        increment_atomic_saturating(&g_quarantined_destructors);
        (void)outstanding_.release();
    }
}

#ifdef ORANGE_SPATIAL_ROI_HANDOFF_TESTING
std::uint64_t SpatialRoiIpcHandoff::quarantined_destructor_count() noexcept
{
    return g_quarantined_destructors.load(std::memory_order_relaxed);
}
#endif

void SpatialRoiIpcHandoff::increment(std::uint64_t* value) noexcept
{
    if (value && *value != std::numeric_limits<std::uint64_t>::max()) {
        ++*value;
    }
}

std::string SpatialRoiIpcHandoff::bounded_error(const std::string& message)
{
    if (message.size() <= kMaxRetainedErrorBytes) {
        return message;
    }
    return message.substr(0, kMaxRetainedErrorBytes);
}

void SpatialRoiIpcHandoff::latch_fatal(
    const char* message,
    std::uint64_t* reason_counter) noexcept
{
    try {
        if (!fatal_latched_) {
            fatal_latched_ = true;
            increment(&counters_.fatal_latches);
            increment(reason_counter);
        }
        last_error_ = bounded_error(message ? std::string(message) : std::string());
    } catch (...) {
        fatal_latched_ = true;
    }
}

void SpatialRoiIpcHandoff::latch_fatal(
    const std::string& message,
    std::uint64_t* reason_counter) noexcept
{
    try {
        if (!fatal_latched_) {
            fatal_latched_ = true;
            increment(&counters_.fatal_latches);
            increment(reason_counter);
        }
        last_error_ = bounded_error(message);
    } catch (...) {
        // The latch itself is set before diagnostic string handling.  A
        // diagnostic allocation failure must not make a fatal path look
        // recoverable.
        fatal_latched_ = true;
    }
}

SpatialRoiIpcHandoffResult SpatialRoiIpcHandoff::make_fatal_result(
    const SpatialRoiIpcCorrelation* correlation,
    const std::string_view message) noexcept
{
    SpatialRoiIpcHandoffResult result;
    result.status = SpatialRoiIpcHandoffResultStatus::kFatal;
    try {
        if (correlation) {
            result.correlation = *correlation;
        }
        const std::size_t length =
            std::min(message.size(), kMaxRetainedErrorBytes);
        if (length == 0) {
            result.error.clear();
        } else {
            result.error.assign(message.data(), length);
        }
    } catch (...) {
        // Keep the status and ownership state authoritative even if an
        // optional diagnostic copy cannot allocate.
        result.correlation.reset();
        result.error.clear();
    }
    return result;
}

bool SpatialRoiIpcHandoff::expected_stream_matches(
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out) const
{
    const SpatialRoiIpcStreamIdentity actual =
        spatial_roi_ipc_stream_identity_from_descriptor(descriptor);
    if (!same_stream(actual, config_.expected_stream)) {
        if (error_out) {
            *error_out =
                "FRAME stream identity does not match this logical-stream handoff";
        }
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcHandoff::response_correlation_matches(
    const SpatialRoiIpcCorrelation& actual,
    const SpatialRoiIpcCorrelation& expected) const noexcept
{
    if (!same_correlation(actual, expected)) {
        return false;
    }
    return true;
}

SpatialRoiIpcTransportReadResult SpatialRoiIpcHandoff::read_response(
    std::uint64_t* counter,
    std::string* error_out) noexcept
{
    SpatialRoiIpcTransportReadResult read;
    try {
        if (!transport_) {
            read.status = SpatialRoiIpcTransportReadStatus::kError;
            read.error = "spatial ROI IPC transport is null";
        } else {
            read = transport_->ReadLine(config_.response_timeout,
                                        kSpatialRoiIpcMaxWireMessageBytes);
            // Fail closed if an adapter violates the max-wire contract and
            // materializes an oversized line anyway.
            if (read.status == SpatialRoiIpcTransportReadStatus::kLine &&
                read.line.size() > kSpatialRoiIpcMaxWireMessageBytes) {
                read.line.clear();
                read.status = SpatialRoiIpcTransportReadStatus::kTooLarge;
            }
        }
        if (read.status == SpatialRoiIpcTransportReadStatus::kTimeout) {
            increment(&counters_.timeouts);
        } else if (read.status == SpatialRoiIpcTransportReadStatus::kEof) {
            increment(&counters_.eof);
        } else if (read.status == SpatialRoiIpcTransportReadStatus::kTooLarge) {
            increment(&counters_.oversized_messages);
            increment(&counters_.malformed_messages);
        } else if (read.status == SpatialRoiIpcTransportReadStatus::kError) {
            increment(&counters_.transport_errors);
        }
        increment(counter);
        if (error_out) {
            *error_out = bounded_error(read_status_error(read));
        }
    } catch (const std::exception& exception) {
        read = {};
        read.status = SpatialRoiIpcTransportReadStatus::kError;
        try {
            read.error = bounded_error(exception.what());
        } catch (...) {
        }
        increment(&counters_.transport_errors);
        increment(counter);
        if (error_out) {
            try {
                *error_out = bounded_error(read.error);
            } catch (...) {
                error_out->clear();
            }
        }
    } catch (...) {
        read = {};
        read.status = SpatialRoiIpcTransportReadStatus::kError;
        increment(&counters_.transport_errors);
        increment(counter);
        if (error_out) {
            try {
                *error_out = "spatial ROI IPC transport read threw";
            } catch (...) {
                error_out->clear();
            }
        }
    }
    return read;
}

bool SpatialRoiIpcHandoff::validate_hello(
    const SpatialRoiIpcHello& hello,
    std::string* error_out) const
{
    if (!same_stream(hello.stream, config_.expected_stream)) {
        if (error_out) {
            *error_out = "recorder HELLO stream identity does not match producer";
        }
        return false;
    }
    if (hello.role != kSpatialRoiIpcRecorderRole) {
        if (error_out) {
            *error_out = "recorder HELLO role must be recorder";
        }
        return false;
    }
    if (hello.queue_capacity_frames != config_.max_outstanding_frames) {
        if (error_out) {
            *error_out =
                "recorder HELLO queue capacity does not equal configured handoff bound";
        }
        return false;
    }
    if (hello.features != spatial_roi_ipc_required_features()) {
        if (error_out) {
            *error_out = "recorder HELLO feature set is not exact";
        }
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcHandoff::Negotiate(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!valid_) {
        if (error_out) {
            *error_out = error_;
        }
        return false;
    }
    if (fatal_latched_) {
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (negotiated_) {
        return true;
    }

    auto fail_negotiation = [&](const std::string& message,
                                std::uint64_t* reason_counter) {
        increment(&counters_.negotiation_failures);
        latch_fatal(message, reason_counter);
        if (error_out) {
            try {
                *error_out = bounded_error(message);
            } catch (...) {
                error_out->clear();
            }
        }
        return false;
    };

    SpatialRoiIpcHello local_hello;
    local_hello.stream = config_.expected_stream;
    local_hello.role = kSpatialRoiIpcProducerRole;
    local_hello.queue_capacity_frames =
        static_cast<std::uint32_t>(config_.max_outstanding_frames);
    local_hello.features = spatial_roi_ipc_required_features();

    std::string validation_error;
    std::string wire;
    try {
        wire = serialize_spatial_roi_ipc_message(local_hello, &validation_error);
        if (wire.empty()) {
            return fail_negotiation(
                validation_error.empty() ? "producer HELLO serialization failed"
                                          : validation_error,
                &counters_.malformed_messages);
        }
        std::string write_error;
        if (!transport_ || !transport_->WriteLine(wire, &write_error)) {
            increment(&counters_.transport_errors);
            return fail_negotiation(
                write_error.empty() ? "producer HELLO write failed" : write_error,
                nullptr);
        }
        increment(&counters_.hello_sent);
    } catch (const std::exception& exception) {
        increment(&counters_.transport_errors);
        (void)exception;
        return fail_negotiation("producer HELLO write threw", nullptr);
    } catch (...) {
        increment(&counters_.transport_errors);
        return fail_negotiation("producer HELLO write threw", nullptr);
    }

    try {
        std::string read_error;
        const SpatialRoiIpcTransportReadResult read =
            read_response(nullptr, &read_error);
        if (read.status != SpatialRoiIpcTransportReadStatus::kLine) {
            return fail_negotiation(read_error, nullptr);
        }

        SpatialRoiIpcMessage peer_message;
        try {
            if (!parse_spatial_roi_ipc_message(read.line,
                                               &peer_message,
                                               &validation_error)) {
                increment(&counters_.malformed_messages);
                return fail_negotiation(
                    validation_error.empty() ? "malformed recorder HELLO" : validation_error,
                    nullptr);
            }
        } catch (const std::exception& exception) {
            increment(&counters_.malformed_messages);
            (void)exception;
            return fail_negotiation("malformed recorder HELLO", nullptr);
        } catch (...) {
            increment(&counters_.malformed_messages);
            return fail_negotiation("malformed recorder HELLO", nullptr);
        }
        if (!std::holds_alternative<SpatialRoiIpcHello>(peer_message)) {
            increment(&counters_.unexpected_messages);
            return fail_negotiation(
                "producer HELLO expected recorder HELLO response", nullptr);
        }
        const SpatialRoiIpcHello& peer_hello =
            std::get<SpatialRoiIpcHello>(peer_message);
        if (!validate_hello(peer_hello, &validation_error)) {
            increment(&counters_.mismatched_messages);
            return fail_negotiation(validation_error, nullptr);
        }
        increment(&counters_.hello_received);
        negotiated_ = true;
        return true;
    } catch (...) {
        // HELLO has already been written. No exception may leave the endpoint
        // apparently negotiable, because retrying could pair a new peer with
        // an old producer identity.
        increment(&counters_.transport_errors);
        return fail_negotiation("exception after producer HELLO write", nullptr);
    }
}

SpatialRoiIpcHandoffResult SpatialRoiIpcHandoff::Submit(
    const SpatialRoiLaneDelivery& delivery,
    const int assigned_recorder_gpu_id,
    const int assigned_shard_id)
{
    increment(&counters_.submit_attempted);
    if (!valid_) {
        increment(&counters_.build_failures);
        return make_result(SpatialRoiIpcHandoffResultStatus::kBuildFailed,
                           nullptr,
                           nonempty_or(error_,
                                       "spatial ROI IPC handoff is invalid"));
    }
    if (fatal_latched_) {
        return make_fatal_result(nullptr, last_error_);
    }
    if (!negotiated_) {
        return make_result(
            SpatialRoiIpcHandoffResultStatus::kNotNegotiated,
            nullptr,
            "spatial ROI IPC Submit requires successful HELLO negotiation");
    }

    SpatialRoiIpcExport export_value;
    std::string build_error;
    try {
        if (!exporter_ ||
            !exporter_->Build(delivery,
                              assigned_recorder_gpu_id,
                              assigned_shard_id,
                              &export_value,
                              &build_error)) {
            increment(&counters_.build_failures);
            if (build_error.empty()) {
                build_error = "spatial ROI IPC FRAME export build failed";
            }
            return make_result(SpatialRoiIpcHandoffResultStatus::kBuildFailed,
                               nullptr,
                               build_error);
        }
    } catch (const std::exception& exception) {
        increment(&counters_.build_failures);
        return make_result(SpatialRoiIpcHandoffResultStatus::kBuildFailed,
                           nullptr,
                           exception.what());
    } catch (...) {
        increment(&counters_.build_failures);
        return make_result(SpatialRoiIpcHandoffResultStatus::kBuildFailed,
                           nullptr,
                           "spatial ROI IPC FRAME export build threw");
    }

    // submit_export_impl is deliberately shared with the host-test seam, but
    // does not increment submit_attempted a second time.
    return submit_export_impl(std::move(export_value), {}, false);
}

#ifdef ORANGE_SPATIAL_ROI_HANDOFF_TESTING
SpatialRoiIpcHandoffResult SpatialRoiIpcHandoff::SubmitPreparedForTest(
    SpatialRoiIpcExport export_value,
    std::shared_ptr<void> retained_test_owner)
{
    increment(&counters_.submit_attempted);
    return submit_export_impl(std::move(export_value),
                              std::move(retained_test_owner),
                              true);
}
#endif

SpatialRoiIpcHandoffResult SpatialRoiIpcHandoff::submit_export_impl(
    SpatialRoiIpcExport export_value,
    std::shared_ptr<void> retained_test_owner,
    const bool test_prepared)
{
    if (!valid_) {
        increment(&counters_.invalid_arguments);
        return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                           nullptr,
                           nonempty_or(error_,
                                       "spatial ROI IPC handoff is invalid"));
    }
    if (fatal_latched_) {
        return make_fatal_result(nullptr, last_error_);
    }
    if (!negotiated_) {
        return make_result(
            SpatialRoiIpcHandoffResultStatus::kNotNegotiated,
            nullptr,
            "spatial ROI IPC Submit requires successful HELLO negotiation");
    }

    if (test_prepared && !retained_test_owner) {
        increment(&counters_.invalid_arguments);
        return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                           nullptr,
                           "test prepared FRAME requires a non-null retained owner");
    }
    if (!test_prepared && !export_value.envelope) {
        // The exporter is the only production construction path and always
        // retains its batch envelope. A missing envelope would make ACK/
        // RELEASE ownership impossible to prove.
        increment(&counters_.invalid_arguments);
        return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                           nullptr,
                           "exporter FRAME is missing its retained envelope");
    }

    std::string validation_error;
    try {
        if (!validate_spatial_roi_ipc_frame(export_value.frame,
                                            &validation_error)) {
            increment(&counters_.invalid_arguments);
            return make_result(
                SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                nullptr,
                nonempty_or(validation_error,
                            "invalid spatial ROI IPC FRAME"));
        }
        if (!expected_stream_matches(export_value.frame.descriptor,
                                     &validation_error)) {
            increment(&counters_.invalid_arguments);
            return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                               nullptr,
                               validation_error);
        }
    } catch (const std::exception& exception) {
        increment(&counters_.invalid_arguments);
        return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                           nullptr,
                           exception.what());
    } catch (...) {
        increment(&counters_.invalid_arguments);
        return make_result(SpatialRoiIpcHandoffResultStatus::kInvalidArgument,
                           nullptr,
                           "spatial ROI IPC FRAME validation threw");
    }

    const SpatialRoiIpcCorrelation correlation =
        spatial_roi_ipc_correlation_from_descriptor(export_value.frame.descriptor);
    const SpatialRoiIpcCorrelationKey key = correlation.key();
    if (last_submitted_roi_stream_frame_index_ ==
            std::numeric_limits<std::uint64_t>::max() ||
        correlation.roi_stream_frame_index !=
            last_submitted_roi_stream_frame_index_ + 1) {
        latch_fatal(
            "spatial ROI IPC roi_stream_frame_index must be exactly the next index",
            nullptr);
        return make_fatal_result(&correlation, last_error_);
    }
    if (outstanding_->find(key) != outstanding_->end()) {
        latch_fatal("duplicate outstanding spatial ROI IPC FRAME correlation",
                    nullptr);
        return make_fatal_result(&correlation, last_error_);
    }
    if (outstanding_->size() >= config_.max_outstanding_frames) {
        increment(&counters_.outstanding_full);
        return make_result(SpatialRoiIpcHandoffResultStatus::kOutstandingFull,
                           &correlation,
                           "spatial ROI IPC outstanding table is full");
    }

    try {
        // This emplace is the ownership barrier: the complete export, including
        // its envelope, is in the table before serialization or WriteLine.
        const auto inserted = outstanding_->emplace(
            key,
            Outstanding{std::move(export_value),
                        std::move(retained_test_owner),
                        false,
                        false});
        if (!inserted.second) {
            latch_fatal("duplicate spatial ROI IPC FRAME correlation", nullptr);
        return make_fatal_result(&correlation, last_error_);
        }
        increment(&counters_.frames_inserted);
        last_submitted_roi_stream_frame_index_ = correlation.roi_stream_frame_index;
        counters_.outstanding_high_water = std::max<std::uint64_t>(
            counters_.outstanding_high_water,
            static_cast<std::uint64_t>(outstanding_->size()));
    } catch (const std::exception& exception) {
        (void)exception;
        latch_fatal("failed to retain spatial ROI IPC export", nullptr);
        return make_fatal_result(&correlation, last_error_);
    } catch (...) {
        latch_fatal("failed to retain spatial ROI IPC export", nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    auto outstanding_it = outstanding_->find(key);
    if (outstanding_it == outstanding_->end()) {
        // This cannot occur in a single-threaded handoff.  Keep a defensive
        // fatal path in case a future implementation changes the table.
        latch_fatal("spatial ROI IPC outstanding entry disappeared", nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    std::string wire;
    try {
        wire = serialize_spatial_roi_ipc_message(
            outstanding_it->second.export_value.frame,
                                                 &validation_error);
        if (wire.empty()) {
            latch_fatal(validation_error.empty()
                            ? "spatial ROI IPC FRAME serialization failed"
                            : validation_error,
                        nullptr);
        return make_fatal_result(&correlation, last_error_);
        }
        std::string transport_error;
        if (!transport_ || !transport_->WriteLine(wire, &transport_error)) {
            increment(&counters_.transport_errors);
            latch_fatal(transport_error.empty()
                            ? "spatial ROI IPC FRAME write failed"
                            : transport_error,
                        nullptr);
        return make_fatal_result(&correlation, last_error_);
        }
        increment(&counters_.frames_written);
    } catch (const std::exception& exception) {
        increment(&counters_.transport_errors);
        (void)exception;
        latch_fatal("spatial ROI IPC FRAME write threw", nullptr);
        return make_fatal_result(&correlation, last_error_);
    } catch (...) {
        increment(&counters_.transport_errors);
        latch_fatal("spatial ROI IPC FRAME write threw", nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    std::string read_error;
    const SpatialRoiIpcTransportReadResult ack_read =
        read_response(nullptr, &read_error);
    if (ack_read.status != SpatialRoiIpcTransportReadStatus::kLine) {
        latch_fatal(read_error, nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    SpatialRoiIpcMessage ack_message;
    try {
        if (!parse_spatial_roi_ipc_message(ack_read.line,
                                           &ack_message,
                                           &validation_error)) {
            increment(&counters_.malformed_messages);
            latch_fatal(validation_error.empty()
                            ? "malformed spatial ROI IPC ACK response"
                            : validation_error,
                        nullptr);
        return make_fatal_result(&correlation, last_error_);
        }
    } catch (const std::exception& exception) {
        increment(&counters_.malformed_messages);
        (void)exception;
        latch_fatal("malformed spatial ROI IPC ACK response", nullptr);
        return make_fatal_result(&correlation, last_error_);
    } catch (...) {
        increment(&counters_.malformed_messages);
        latch_fatal("malformed spatial ROI IPC ACK response", nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    if (!std::holds_alternative<SpatialRoiIpcAck>(ack_message)) {
        if (std::holds_alternative<SpatialRoiIpcRelease>(ack_message)) {
            increment(&counters_.release_before_ack);
            latch_fatal("spatial ROI IPC RELEASE arrived before ACK", nullptr);
        } else {
            increment(&counters_.unexpected_messages);
            latch_fatal("spatial ROI IPC expected ACK as first response", nullptr);
        }
        return make_fatal_result(&correlation, last_error_);
    }

    const SpatialRoiIpcAck& ack = std::get<SpatialRoiIpcAck>(ack_message);
    if (!response_correlation_matches(ack.correlation, correlation)) {
        increment(&counters_.mismatched_messages);
        latch_fatal("peer ACK correlation does not exactly match outstanding FRAME",
                    nullptr);
        return make_fatal_result(&correlation, last_error_);
    }
    outstanding_it->second.ack_seen = true;
    outstanding_it->second.ack_accepted = ack.accepted;
    if (ack.accepted) {
        increment(&counters_.ack_accepted);
    } else {
        increment(&counters_.ack_rejected);
    }

    const SpatialRoiIpcTransportReadResult release_read =
        read_response(nullptr, &read_error);
    if (release_read.status != SpatialRoiIpcTransportReadStatus::kLine) {
        latch_fatal(read_error, nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    SpatialRoiIpcMessage release_message;
    try {
        if (!parse_spatial_roi_ipc_message(release_read.line,
                                           &release_message,
                                           &validation_error)) {
            increment(&counters_.malformed_messages);
            latch_fatal(validation_error.empty()
                            ? "malformed spatial ROI IPC RELEASE response"
                            : validation_error,
                        nullptr);
        return make_fatal_result(&correlation, last_error_);
        }
    } catch (const std::exception& exception) {
        increment(&counters_.malformed_messages);
        (void)exception;
        latch_fatal("malformed spatial ROI IPC RELEASE response", nullptr);
        return make_fatal_result(&correlation, last_error_);
    } catch (...) {
        increment(&counters_.malformed_messages);
        latch_fatal("malformed spatial ROI IPC RELEASE response", nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    if (!std::holds_alternative<SpatialRoiIpcRelease>(release_message)) {
        if (std::holds_alternative<SpatialRoiIpcAck>(release_message)) {
            const SpatialRoiIpcCorrelation& duplicate_or_mismatch =
                std::get<SpatialRoiIpcAck>(release_message).correlation;
            if (same_correlation(duplicate_or_mismatch, correlation)) {
                increment(&counters_.duplicate_messages);
                latch_fatal("duplicate spatial ROI IPC ACK", nullptr);
            } else {
                increment(&counters_.mismatched_messages);
                latch_fatal("spatial ROI IPC ACK correlation changed before RELEASE",
                            nullptr);
            }
        } else {
            increment(&counters_.unexpected_messages);
            latch_fatal("spatial ROI IPC expected RELEASE after ACK", nullptr);
        }
        return make_fatal_result(&correlation, last_error_);
    }

    const SpatialRoiIpcRelease& release =
        std::get<SpatialRoiIpcRelease>(release_message);
    if (!response_correlation_matches(release.correlation, correlation)) {
        increment(&counters_.mismatched_messages);
        latch_fatal(
            "peer RELEASE correlation does not exactly match outstanding FRAME",
            nullptr);
        return make_fatal_result(&correlation, last_error_);
    }

    const bool accepted = outstanding_it->second.ack_accepted;
    outstanding_->erase(outstanding_it);
    increment(&counters_.releases);
    if (accepted) {
        increment(&counters_.completed_frames);
        return make_result(SpatialRoiIpcHandoffResultStatus::kCompleted,
                           &correlation,
                           "");
    }
    increment(&counters_.rejected_frames);
    return make_result(SpatialRoiIpcHandoffResultStatus::kRejected,
                       &correlation,
                       "peer rejected spatial ROI FRAME admission");
}

bool SpatialRoiIpcHandoff::ConfirmPeerExited() noexcept
{
    if (!fatal_latched_) {
        return false;
    }
    try {
        if (outstanding_) {
            outstanding_->clear();
        }
        peer_exited_confirmed_ = true;
        increment(&counters_.peer_exit_confirmations);
        return true;
    } catch (...) {
        // unordered_map::clear and export destructors are expected to be
        // non-throwing.  Keep the latch and indeterminate ownership visible if
        // an unforeseen destructor violates that contract.
        return false;
    }
}

}  // namespace orange::spatial_roi::ipc
