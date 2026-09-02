#pragma once

#include "spatial_roi_ipc_handoff.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace orange::spatial_roi::ipc {

// The recorder session is the bounded, host-side protocol seam between one
// producer stream and one recorder stream.  CUDA import, conversion, and
// encoder admission stay behind the callback so this class can be tested
// without a CUDA context.
struct SpatialRoiRecorderIpcSessionConfig {
    SpatialRoiIpcStreamIdentity expected_stream;
    // This is the recorder's bounded admission capacity and is echoed in its
    // HELLO.  The session itself processes at most one source frame at a time.
    std::uint32_t queue_capacity_frames = 1;
    std::chrono::milliseconds response_timeout{1000};
};

enum class SpatialRoiRecorderIpcDispatchStatus {
    // Detach completed and recorder-side work was admitted.
    kEnqueued,
    // The source was safely detached or rejected before import; RELEASE is
    // safe after the corresponding ACK.
    kRejected,
    // The callback cannot prove that the producer's CUDA source is no longer
    // owned by it.  The session must not ACK or RELEASE this frame.
    kSourceOwnershipUncertain,
};

struct SpatialRoiRecorderIpcDispatchResult {
    SpatialRoiRecorderIpcDispatchStatus status =
        SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain;
    // Exact detach result produced before encoder admission is attempted.
    // kEnqueued requires "detached"; safe and uncertain failures retain their
    // concrete bounded result instead of collapsing it into the ACK status.
    std::string detach_status;
    bool source_release_safe = false;
    // Required for kRejected and kSourceOwnershipUncertain. It is bounded by
    // the IPC protocol validator before being placed on the wire or in
    // evidence.
    std::string reason;

    bool can_release_source() const noexcept
    {
        return source_release_safe;
    }

    bool accepted() const noexcept
    {
        return status == SpatialRoiRecorderIpcDispatchStatus::kEnqueued;
    }
};

using SpatialRoiRecorderIpcDispatch = std::function<
    SpatialRoiRecorderIpcDispatchResult(const SpatialRoiIpcFrame& frame)>;

// Exact recorder-side transport outcome for one FRAME. The observer is invoked
// once after the attempted ACK/RELEASE sequence, including either write
// failure. A source-ownership-uncertain result is reported before the session
// latches fatal, with no invented ACK or RELEASE attempt. ACK acceptance is the
// intended payload bit and remains distinct from whether that payload reached
// the wire.
struct SpatialRoiRecorderIpcFrameOutcome {
    SpatialRoiFrameDescriptor descriptor;
    SpatialRoiRecorderIpcDispatchResult dispatch;
    bool ack_attempted = false;
    bool ack_sent = false;
    bool ack_accepted = false;
    std::string ack_reason;
    std::string ack_error;
    bool release_attempted = false;
    bool release_sent = false;
    std::string release_reason;
    std::string release_error;
};

using SpatialRoiRecorderIpcFrameOutcomeObserver = std::function<bool(
    const SpatialRoiRecorderIpcFrameOutcome& outcome,
    std::string* error_out)>;

enum class SpatialRoiRecorderIpcSessionStatus {
    // One producer EOF was received without an in-flight ownership ambiguity.
    kCleanEof,
    // A bounded protocol/transport/ownership failure latched TERMINAL_ERROR.
    kFatal,
    kNotNegotiated,
    kInvalidArgument,
};

const char* spatial_roi_recorder_ipc_session_status_name(
    SpatialRoiRecorderIpcSessionStatus status) noexcept;

struct SpatialRoiRecorderIpcSessionResult {
    SpatialRoiRecorderIpcSessionStatus status =
        SpatialRoiRecorderIpcSessionStatus::kFatal;
    std::optional<SpatialRoiIpcCorrelation> correlation;
    std::string error;

    bool clean_eof() const noexcept
    {
        return status == SpatialRoiRecorderIpcSessionStatus::kCleanEof;
    }
    bool fatal() const noexcept
    {
        return status == SpatialRoiRecorderIpcSessionStatus::kFatal;
    }
};

struct SpatialRoiRecorderIpcSessionCounters {
    std::uint64_t hello_sent = 0;
    std::uint64_t hello_received = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t frames_enqueued = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t acks_sent = 0;
    std::uint64_t releases_sent = 0;
    std::uint64_t terminal_errors_sent = 0;

    std::uint64_t malformed_messages = 0;
    std::uint64_t unexpected_messages = 0;
    std::uint64_t mismatched_messages = 0;
    std::uint64_t duplicate_frames = 0;
    std::uint64_t frame_gaps = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t transport_errors = 0;
    std::uint64_t eof = 0;
    std::uint64_t dispatch_failures = 0;
    std::uint64_t ownership_uncertain = 0;
    std::uint64_t frame_outcomes_reported = 0;
    std::uint64_t frame_outcome_failures = 0;
    std::uint64_t fatal_latches = 0;
};

// One recorder-side session for exactly one authenticated logical stream.
// It owns neither the transport nor recorder resources.  It is intentionally
// synchronous and single-owner: a future process listener may put one of
// these objects behind each connected socket, but this class does not create
// listeners, reconnect, or negotiate drain/finalize.
class SpatialRoiRecorderIpcSession final {
public:
    SpatialRoiRecorderIpcSession(
        SpatialRoiIpcLineTransport& transport,
        SpatialRoiRecorderIpcSessionConfig config,
        SpatialRoiRecorderIpcDispatch dispatch,
        SpatialRoiRecorderIpcFrameOutcomeObserver outcome_observer = {});
    ~SpatialRoiRecorderIpcSession() = default;

    SpatialRoiRecorderIpcSession(const SpatialRoiRecorderIpcSession&) = delete;
    SpatialRoiRecorderIpcSession& operator=(
        const SpatialRoiRecorderIpcSession&) = delete;
    SpatialRoiRecorderIpcSession(SpatialRoiRecorderIpcSession&&) = delete;
    SpatialRoiRecorderIpcSession& operator=(SpatialRoiRecorderIpcSession&&) = delete;

    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }
    const SpatialRoiRecorderIpcSessionConfig& config() const noexcept
    {
        return config_;
    }
    bool negotiated() const noexcept { return negotiated_; }
    bool fatal_latched() const noexcept { return fatal_latched_; }
    bool clean_eof() const noexcept { return clean_eof_; }
    const std::string& last_error() const noexcept { return last_error_; }
    SpatialRoiRecorderIpcSessionCounters counters() const noexcept
    {
        return counters_;
    }

    // Receive and validate the producer HELLO, then send the exact recorder
    // HELLO. It is successful only when stream identity, role, queue bound,
    // and feature vector are all exact.
    bool Negotiate(std::string* error_out = nullptr);

    // Drain FRAME records until clean producer EOF or a fatal protocol,
    // transport, timeout, callback, or ownership error. A clean EOF is
    // terminal for this session; calling Run again does not reconnect.
    SpatialRoiRecorderIpcSessionResult Run();

private:
    static void increment(std::uint64_t* value) noexcept;
    static std::string bounded_error(const std::string& message);

    bool validate_producer_hello(const SpatialRoiIpcHello& hello,
                                 std::string* error_out) const;
    bool same_stream(const SpatialRoiIpcStreamIdentity& lhs,
                     const SpatialRoiIpcStreamIdentity& rhs) const noexcept;
    bool send_message(const SpatialRoiIpcMessage& message,
                      std::uint64_t* success_counter,
                      std::string* error_out) noexcept;
    bool report_frame_outcome(
        const SpatialRoiRecorderIpcFrameOutcome& outcome,
        std::string* error_out) noexcept;
    void latch_fatal(const char* error_code,
                     const std::string& message,
                     const std::optional<SpatialRoiIpcCorrelation>& correlation)
        noexcept;
    SpatialRoiRecorderIpcSessionResult make_result(
        SpatialRoiRecorderIpcSessionStatus status,
        const std::optional<SpatialRoiIpcCorrelation>& correlation,
        const std::string& error) const noexcept;
    void send_terminal_error(
        const char* error_code,
        const std::string& message,
        const std::optional<SpatialRoiIpcCorrelation>& correlation) noexcept;

    SpatialRoiIpcLineTransport* transport_ = nullptr;
    SpatialRoiRecorderIpcSessionConfig config_;
    SpatialRoiRecorderIpcDispatch dispatch_;
    SpatialRoiRecorderIpcFrameOutcomeObserver outcome_observer_;
    bool valid_ = false;
    bool negotiated_ = false;
    bool fatal_latched_ = false;
    bool clean_eof_ = false;
    bool terminal_error_sent_ = false;
    std::string error_;
    std::string last_error_;
    // Only roi_stream_frame_index is required to be dense. Camera/recording
    // frame identities may legitimately skip when acquisition drops frames.
    std::uint64_t last_roi_stream_frame_index_ = 0;
    SpatialRoiRecorderIpcSessionCounters counters_;
};

}  // namespace orange::spatial_roi::ipc
