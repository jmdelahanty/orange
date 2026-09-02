#include "spatial_roi_recorder_ipc_session.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace orange::spatial_roi::ipc {
namespace {

std::string sanitise_error(const std::string& value)
{
    const std::size_t length =
        std::min(value.size(), kSpatialRoiIpcMaxTextBytes);
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(value[index]);
        result.push_back((byte < 0x20 || byte == 0x7f)
                             ? '?'
                             : static_cast<char>(byte));
    }
    if (result.empty()) {
        result = "spatial ROI IPC recorder session failure";
    }
    return result;
}

std::string read_failure_text(const SpatialRoiIpcTransportReadResult& read)
{
    if (!read.error.empty()) {
        return sanitise_error(read.error);
    }
    switch (read.status) {
    case SpatialRoiIpcTransportReadStatus::kTimeout:
        return "spatial ROI IPC recorder read timed out";
    case SpatialRoiIpcTransportReadStatus::kEof:
        return "spatial ROI IPC producer reached EOF";
    case SpatialRoiIpcTransportReadStatus::kError:
        return "spatial ROI IPC recorder transport read failed";
    case SpatialRoiIpcTransportReadStatus::kTooLarge:
        return "spatial ROI IPC message exceeds maximum wire size";
    case SpatialRoiIpcTransportReadStatus::kLine:
        return "";
    }
    return "spatial ROI IPC recorder transport returned an unknown status";
}

bool is_valid_dispatch_status(
    const SpatialRoiRecorderIpcDispatchStatus status) noexcept
{
    return status == SpatialRoiRecorderIpcDispatchStatus::kEnqueued ||
           status == SpatialRoiRecorderIpcDispatchStatus::kRejected ||
           status ==
               SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain;
}

bool bounded_lifecycle_text(const std::string& value,
                            const bool allow_empty = false) noexcept
{
    if (value.empty()) {
        return allow_empty;
    }
    if (value.size() > kSpatialRoiIpcMaxTextBytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte < 0x20 || byte == 0x7f;
    });
}

bool bounded_detach_status(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
               byte == '_';
    });
}

bool is_known_detach_status(const std::string& value) noexcept
{
    // Keep this set in lockstep with SpatialRoiRecorderDetachStatus.  The
    // detach status is an ownership claim, not an arbitrary diagnostic tag:
    // accepting a new spelling here would let a callback bypass the
    // source-safety matrix below.
    constexpr const char* kDetachStatuses[] = {
        "detached", "invalid_argument", "wrong_device", "busy",
        "pool_exhausted", "cuda_error", "source_quarantined", "stopped"};
    for (const char* known : kDetachStatuses) {
        if (value == known) {
            return true;
        }
    }
    return false;
}

bool validate_dispatch_result(const SpatialRoiRecorderIpcDispatchResult& result,
                              std::string* error_out)
{
    const auto reject = [&](const char* message) {
        if (error_out) {
            *error_out = message;
        }
        return false;
    };
    if (!is_valid_dispatch_status(result.status)) {
        return reject("recorder dispatch returned an unknown status");
    }
    if (!bounded_detach_status(result.detach_status) ||
        !bounded_lifecycle_text(result.reason, true)) {
        return reject("recorder dispatch returned invalid lifecycle text");
    }
    if (!is_known_detach_status(result.detach_status)) {
        return reject("recorder dispatch returned unknown detach status");
    }
    switch (result.status) {
    case SpatialRoiRecorderIpcDispatchStatus::kEnqueued:
        if (result.detach_status != "detached" ||
            !result.source_release_safe || !result.reason.empty()) {
            return reject(
                "enqueued dispatch requires a source-safe detach and no reason");
        }
        break;
    case SpatialRoiRecorderIpcDispatchStatus::kRejected:
        if (!result.source_release_safe ||
            result.detach_status == "source_quarantined" ||
            result.reason.empty()) {
            return reject(
                "rejected dispatch requires source safety and a reason");
        }
        break;
    case SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain:
        if (result.source_release_safe ||
            result.detach_status != "source_quarantined" ||
            result.reason.empty()) {
            return reject(
                "uncertain dispatch requires unsafe source ownership and a reason");
        }
        break;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

}  // namespace

const char* spatial_roi_recorder_ipc_session_status_name(
    const SpatialRoiRecorderIpcSessionStatus status) noexcept
{
    switch (status) {
    case SpatialRoiRecorderIpcSessionStatus::kCleanEof:
        return "clean_eof";
    case SpatialRoiRecorderIpcSessionStatus::kFatal:
        return "fatal";
    case SpatialRoiRecorderIpcSessionStatus::kNotNegotiated:
        return "not_negotiated";
    case SpatialRoiRecorderIpcSessionStatus::kInvalidArgument:
        return "invalid_argument";
    }
    return "unknown";
}

SpatialRoiRecorderIpcSession::SpatialRoiRecorderIpcSession(
    SpatialRoiIpcLineTransport& transport,
    SpatialRoiRecorderIpcSessionConfig config,
    SpatialRoiRecorderIpcDispatch dispatch,
    SpatialRoiRecorderIpcFrameOutcomeObserver outcome_observer)
    : transport_(&transport),
      config_(std::move(config)),
      dispatch_(std::move(dispatch)),
      outcome_observer_(std::move(outcome_observer))
{
    try {
        if (!validate_spatial_roi_ipc_stream_identity(
                config_.expected_stream, &error_)) {
            if (error_.empty()) {
                error_ = "invalid expected recorder IPC stream identity";
            }
            return;
        }
        if (config_.queue_capacity_frames == 0 ||
            config_.queue_capacity_frames > kSpatialRoiIpcMaxQueueFrames) {
            error_ =
                "recorder IPC queue_capacity_frames is outside the bounded range";
            return;
        }
        if (config_.response_timeout.count() <= 0) {
            error_ = "recorder IPC response_timeout must be positive";
            return;
        }
        if (!dispatch_) {
            error_ = "recorder IPC dispatch callback is required";
            return;
        }
        valid_ = true;
    } catch (const std::exception& exception) {
        error_ = sanitise_error(exception.what());
    } catch (...) {
        error_ = "recorder IPC session construction failed";
    }
}

void SpatialRoiRecorderIpcSession::increment(std::uint64_t* value) noexcept
{
    if (value && *value != std::numeric_limits<std::uint64_t>::max()) {
        ++*value;
    }
}

std::string SpatialRoiRecorderIpcSession::bounded_error(
    const std::string& message)
{
    try {
        return sanitise_error(message);
    } catch (...) {
        return "spatial ROI IPC recorder session failure";
    }
}

bool SpatialRoiRecorderIpcSession::same_stream(
    const SpatialRoiIpcStreamIdentity& lhs,
    const SpatialRoiIpcStreamIdentity& rhs) const noexcept
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

bool SpatialRoiRecorderIpcSession::validate_producer_hello(
    const SpatialRoiIpcHello& hello,
    std::string* error_out) const
{
    if (!same_stream(hello.stream, config_.expected_stream)) {
        if (error_out) {
            *error_out =
                "producer HELLO stream identity does not match recorder admission";
        }
        return false;
    }
    if (hello.role != kSpatialRoiIpcProducerRole) {
        if (error_out) {
            *error_out = "producer HELLO role must be producer";
        }
        return false;
    }
    // Both peers are configured from the same authenticated per-stream
    // bound. Directional capacities would require distinct wire fields;
    // accepting two meanings under this one field would make the handshake
    // asymmetric with the producer-side validator.
    if (hello.queue_capacity_frames != config_.queue_capacity_frames) {
        if (error_out) {
            *error_out =
                "producer HELLO queue capacity does not equal recorder admission";
        }
        return false;
    }
    if (hello.features != spatial_roi_ipc_required_features()) {
        if (error_out) {
            *error_out = "producer HELLO feature set is not exact";
        }
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderIpcSession::send_message(
    const SpatialRoiIpcMessage& message,
    std::uint64_t* success_counter,
    std::string* error_out) noexcept
{
    try {
        if (!transport_) {
            if (error_out) {
                *error_out = "recorder IPC transport is null";
            }
            return false;
        }
        std::string validation_error;
        const std::string wire = serialize_spatial_roi_ipc_message(
            message, &validation_error);
        if (wire.empty()) {
            if (error_out) {
                *error_out = bounded_error(
                    validation_error.empty()
                        ? "recorder IPC message serialization failed"
                        : validation_error);
            }
            return false;
        }
        std::string transport_error;
        if (!transport_->WriteLine(wire, &transport_error)) {
            if (error_out) {
                *error_out = bounded_error(
                    transport_error.empty()
                        ? "recorder IPC transport write failed"
                        : transport_error);
            }
            return false;
        }
        increment(success_counter);
        if (error_out) {
            error_out->clear();
        }
        return true;
    } catch (const std::exception& exception) {
        if (error_out) {
            *error_out = bounded_error(exception.what());
        }
        return false;
    } catch (...) {
        if (error_out) {
            *error_out = "recorder IPC transport write threw";
        }
        return false;
    }
}

bool SpatialRoiRecorderIpcSession::report_frame_outcome(
    const SpatialRoiRecorderIpcFrameOutcome& outcome,
    std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    if (!outcome_observer_) {
        return true;
    }
    try {
        std::string observer_error;
        if (!outcome_observer_(outcome, &observer_error)) {
            increment(&counters_.frame_outcome_failures);
            if (error_out) {
                *error_out = bounded_error(
                    observer_error.empty()
                        ? "recorder frame-outcome observer rejected the outcome"
                        : observer_error);
            }
            return false;
        }
        increment(&counters_.frame_outcomes_reported);
        return true;
    } catch (const std::exception& exception) {
        increment(&counters_.frame_outcome_failures);
        if (error_out) {
            *error_out = bounded_error(exception.what());
        }
        return false;
    } catch (...) {
        increment(&counters_.frame_outcome_failures);
        if (error_out) {
            *error_out = "recorder frame-outcome observer threw";
        }
        return false;
    }
}

void SpatialRoiRecorderIpcSession::send_terminal_error(
    const char* error_code,
    const std::string& message,
    const std::optional<SpatialRoiIpcCorrelation>& correlation) noexcept
{
    if (terminal_error_sent_) {
        return;
    }
    // Set this before constructing/serializing: a failing terminal write must
    // never result in a second attempt or a reconnect-like retry.
    terminal_error_sent_ = true;
    try {
        SpatialRoiIpcTerminalError terminal;
        terminal.stream = config_.expected_stream;
        terminal.error_code =
            (error_code && *error_code != '\0') ? error_code : "fatal";
        terminal.message = bounded_error(message);
        if (correlation && same_stream(correlation->stream,
                                       config_.expected_stream)) {
            terminal.correlation = *correlation;
        }
        std::string write_error;
        if (!send_message(SpatialRoiIpcMessage{std::move(terminal)},
                          &counters_.terminal_errors_sent,
                          &write_error)) {
            increment(&counters_.transport_errors);
        }
    } catch (...) {
        increment(&counters_.transport_errors);
    }
}

void SpatialRoiRecorderIpcSession::latch_fatal(
    const char* error_code,
    const std::string& message,
    const std::optional<SpatialRoiIpcCorrelation>& correlation) noexcept
{
    if (fatal_latched_) {
        return;
    }
    fatal_latched_ = true;
    increment(&counters_.fatal_latches);
    try {
        last_error_ = bounded_error(message);
    } catch (...) {
        last_error_.clear();
    }
    send_terminal_error(error_code, last_error_, correlation);
}

SpatialRoiRecorderIpcSessionResult SpatialRoiRecorderIpcSession::make_result(
    const SpatialRoiRecorderIpcSessionStatus status,
    const std::optional<SpatialRoiIpcCorrelation>& correlation,
    const std::string& error) const noexcept
{
    SpatialRoiRecorderIpcSessionResult result;
    result.status = status;
    try {
        result.correlation = correlation;
        result.error = bounded_error(error);
    } catch (...) {
        result.correlation.reset();
        result.error.clear();
    }
    return result;
}

bool SpatialRoiRecorderIpcSession::Negotiate(std::string* error_out)
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
    if (fatal_latched_ || clean_eof_) {
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (negotiated_) {
        return true;
    }

    SpatialRoiIpcTransportReadResult read;
    try {
        read = transport_->ReadLine(config_.response_timeout,
                                     kSpatialRoiIpcMaxWireMessageBytes);
    } catch (const std::exception& exception) {
        increment(&counters_.transport_errors);
        latch_fatal("transport_read_exception", exception.what(), std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    } catch (...) {
        increment(&counters_.transport_errors);
        latch_fatal("transport_read_exception",
                    "producer HELLO read threw",
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (read.status == SpatialRoiIpcTransportReadStatus::kEof) {
        increment(&counters_.eof);
        latch_fatal("eof_before_hello", "producer EOF before HELLO", std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (read.status != SpatialRoiIpcTransportReadStatus::kLine) {
        if (read.status == SpatialRoiIpcTransportReadStatus::kTimeout) {
            increment(&counters_.timeouts);
        } else {
            increment(&counters_.transport_errors);
        }
        const std::string message = read_failure_text(read);
        latch_fatal(read.status == SpatialRoiIpcTransportReadStatus::kTooLarge
                        ? "message_oversized"
                        : "transport_read_failed",
                    message,
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (read.line.size() > kSpatialRoiIpcMaxWireMessageBytes) {
        increment(&counters_.malformed_messages);
        latch_fatal("message_oversized",
                    "producer HELLO exceeds maximum wire size",
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }

    SpatialRoiIpcMessage message;
    std::string parse_error;
    try {
        if (!parse_spatial_roi_ipc_message(read.line, &message, &parse_error)) {
            increment(&counters_.malformed_messages);
            latch_fatal("malformed_hello",
                        parse_error.empty() ? "malformed producer HELLO"
                                            : parse_error,
                        std::nullopt);
            if (error_out) {
                *error_out = last_error_;
            }
            return false;
        }
    } catch (const std::exception& exception) {
        increment(&counters_.malformed_messages);
        latch_fatal("malformed_hello", exception.what(), std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    } catch (...) {
        increment(&counters_.malformed_messages);
        latch_fatal("malformed_hello", "producer HELLO parse threw", std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    if (!std::holds_alternative<SpatialRoiIpcHello>(message)) {
        increment(&counters_.unexpected_messages);
        latch_fatal("unexpected_hello_message",
                    "recorder expected producer HELLO",
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    const SpatialRoiIpcHello& producer_hello =
        std::get<SpatialRoiIpcHello>(message);
    if (!validate_producer_hello(producer_hello, &parse_error)) {
        increment(&counters_.mismatched_messages);
        latch_fatal("hello_mismatch",
                    parse_error.empty() ? "producer HELLO mismatch" : parse_error,
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    increment(&counters_.hello_received);

    SpatialRoiIpcHello recorder_hello;
    recorder_hello.stream = config_.expected_stream;
    recorder_hello.role = kSpatialRoiIpcRecorderRole;
    recorder_hello.queue_capacity_frames = config_.queue_capacity_frames;
    recorder_hello.features = spatial_roi_ipc_required_features();
    std::string write_error;
    if (!send_message(SpatialRoiIpcMessage{std::move(recorder_hello)},
                      &counters_.hello_sent,
                      &write_error)) {
        increment(&counters_.transport_errors);
        latch_fatal("hello_write_failed",
                    write_error.empty() ? "recorder HELLO write failed"
                                        : write_error,
                    std::nullopt);
        if (error_out) {
            *error_out = last_error_;
        }
        return false;
    }
    negotiated_ = true;
    return true;
}

SpatialRoiRecorderIpcSessionResult SpatialRoiRecorderIpcSession::Run()
{
    if (!valid_) {
        return make_result(SpatialRoiRecorderIpcSessionStatus::kInvalidArgument,
                           std::nullopt,
                           error_);
    }
    if (fatal_latched_) {
        return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                           std::nullopt,
                           last_error_);
    }
    if (clean_eof_) {
        return make_result(SpatialRoiRecorderIpcSessionStatus::kCleanEof,
                           std::nullopt,
                           "producer EOF");
    }
    if (!negotiated_) {
        return make_result(
            SpatialRoiRecorderIpcSessionStatus::kNotNegotiated,
            std::nullopt,
            "recorder IPC Run requires successful HELLO negotiation");
    }

    for (;;) {
        SpatialRoiIpcTransportReadResult read;
        try {
            read = transport_->ReadLine(config_.response_timeout,
                                         kSpatialRoiIpcMaxWireMessageBytes);
        } catch (const std::exception& exception) {
            increment(&counters_.transport_errors);
            latch_fatal("transport_read_exception", exception.what(), std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        } catch (...) {
            increment(&counters_.transport_errors);
            latch_fatal("transport_read_exception",
                        "recorder frame read threw",
                        std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }

        if (read.status == SpatialRoiIpcTransportReadStatus::kEof) {
            increment(&counters_.eof);
            clean_eof_ = true;
            return make_result(SpatialRoiRecorderIpcSessionStatus::kCleanEof,
                               std::nullopt,
                               "producer EOF");
        }
        if (read.status != SpatialRoiIpcTransportReadStatus::kLine) {
            if (read.status == SpatialRoiIpcTransportReadStatus::kTimeout) {
                increment(&counters_.timeouts);
            } else {
                increment(&counters_.transport_errors);
            }
            const std::string failure = read_failure_text(read);
            latch_fatal(read.status == SpatialRoiIpcTransportReadStatus::kTooLarge
                            ? "message_oversized"
                            : "transport_read_failed",
                        failure,
                        std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }
        if (read.line.size() > kSpatialRoiIpcMaxWireMessageBytes) {
            increment(&counters_.malformed_messages);
            latch_fatal("message_oversized",
                        "producer FRAME exceeds maximum wire size",
                        std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }

        SpatialRoiIpcMessage message;
        std::string parse_error;
        try {
            if (!parse_spatial_roi_ipc_message(read.line, &message, &parse_error)) {
                increment(&counters_.malformed_messages);
                latch_fatal("malformed_frame",
                            parse_error.empty() ? "malformed producer FRAME"
                                                : parse_error,
                            std::nullopt);
                return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                                   std::nullopt,
                                   last_error_);
            }
        } catch (const std::exception& exception) {
            increment(&counters_.malformed_messages);
            latch_fatal("malformed_frame", exception.what(), std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        } catch (...) {
            increment(&counters_.malformed_messages);
            latch_fatal("malformed_frame", "producer FRAME parse threw", std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }
        if (!std::holds_alternative<SpatialRoiIpcFrame>(message)) {
            increment(&counters_.unexpected_messages);
            latch_fatal("unexpected_message",
                        "recorder expected producer FRAME",
                        std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }

        const SpatialRoiIpcFrame& frame = std::get<SpatialRoiIpcFrame>(message);
        increment(&counters_.frames_received);
        const SpatialRoiIpcCorrelation correlation =
            spatial_roi_ipc_correlation_from_descriptor(frame.descriptor);
        if (!same_stream(
                spatial_roi_ipc_stream_identity_from_descriptor(frame.descriptor),
                config_.expected_stream)) {
            increment(&counters_.mismatched_messages);
            latch_fatal("frame_stream_mismatch",
                        "producer FRAME stream identity does not match recorder admission",
                        std::nullopt);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               std::nullopt,
                               last_error_);
        }

        const std::uint64_t expected_index =
            (last_roi_stream_frame_index_ ==
             std::numeric_limits<std::uint64_t>::max())
                ? 0
                : last_roi_stream_frame_index_ + 1;
        if (expected_index == 0 ||
            correlation.roi_stream_frame_index != expected_index) {
            if (correlation.roi_stream_frame_index <=
                last_roi_stream_frame_index_) {
                increment(&counters_.duplicate_frames);
            } else {
                increment(&counters_.frame_gaps);
            }
            latch_fatal("frame_identity_not_dense",
                        "producer FRAME roi_stream_frame_index is not dense",
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }

        SpatialRoiRecorderIpcDispatchResult dispatch_result;
        try {
            dispatch_result = dispatch_(frame);
        } catch (const std::exception& exception) {
            increment(&counters_.dispatch_failures);
            latch_fatal("dispatch_exception", exception.what(), correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        } catch (...) {
            increment(&counters_.dispatch_failures);
            latch_fatal("dispatch_exception",
                        "recorder FRAME dispatch threw",
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }
        std::string dispatch_validation_error;
        if (!validate_dispatch_result(dispatch_result,
                                      &dispatch_validation_error)) {
            increment(&counters_.dispatch_failures);
            latch_fatal("invalid_dispatch_result",
                        dispatch_validation_error,
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }

        SpatialRoiRecorderIpcFrameOutcome outcome;
        outcome.descriptor = frame.descriptor;
        outcome.dispatch = dispatch_result;
        if (!dispatch_result.can_release_source()) {
            std::string ignored_observer_error;
            (void)report_frame_outcome(outcome, &ignored_observer_error);
            increment(&counters_.ownership_uncertain);
            latch_fatal("source_ownership_uncertain",
                        dispatch_result.reason,
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }

        const bool accepted = dispatch_result.accepted();
        if (accepted) {
            increment(&counters_.frames_enqueued);
        } else {
            increment(&counters_.frames_rejected);
        }
        SpatialRoiIpcAck ack;
        ack.correlation = correlation;
        ack.accepted = accepted;
        ack.reason = accepted ? "" : dispatch_result.reason;
        std::string write_error;
        outcome.ack_attempted = true;
        outcome.ack_accepted = ack.accepted;
        outcome.ack_reason = ack.reason;
        if (!send_message(SpatialRoiIpcMessage{ack},
                          &counters_.acks_sent,
                          &write_error)) {
            outcome.ack_error = write_error.empty()
                ? "recorder ACK write failed" : write_error;
            std::string ignored_observer_error;
            (void)report_frame_outcome(outcome, &ignored_observer_error);
            increment(&counters_.transport_errors);
            latch_fatal("ack_write_failed",
                        write_error.empty() ? "recorder ACK write failed"
                                            : write_error,
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }
        outcome.ack_sent = true;

        SpatialRoiIpcRelease release;
        release.correlation = correlation;
        release.reason = accepted ? "source_detached" : "source_rejected";
        outcome.release_attempted = true;
        outcome.release_reason = release.reason;
        if (!send_message(SpatialRoiIpcMessage{release},
                          &counters_.releases_sent,
                          &write_error)) {
            outcome.release_error = write_error.empty()
                ? "recorder RELEASE write failed" : write_error;
            std::string ignored_observer_error;
            (void)report_frame_outcome(outcome, &ignored_observer_error);
            increment(&counters_.transport_errors);
            latch_fatal("release_write_failed",
                        write_error.empty() ? "recorder RELEASE write failed"
                                            : write_error,
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }
        outcome.release_sent = true;
        std::string observer_error;
        if (!report_frame_outcome(outcome, &observer_error)) {
            latch_fatal("frame_outcome_observer_failed",
                        observer_error.empty()
                            ? "recorder frame-outcome observer failed"
                            : observer_error,
                        correlation);
            return make_result(SpatialRoiRecorderIpcSessionStatus::kFatal,
                               correlation,
                               last_error_);
        }
        last_roi_stream_frame_index_ = correlation.roi_stream_frame_index;
    }
}

}  // namespace orange::spatial_roi::ipc
