#pragma once

#include "spatial_roi_ipc_exporter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orange::spatial_roi::ipc {

// The handoff deliberately owns no socket or thread.  A production adapter
// can implement this interface over a Unix socket, while deterministic tests
// can feed preconstructed JSON lines.  ReadLine must return kTimeout when the
// supplied bounded wait expires; it must not silently turn a timeout into EOF.
enum class SpatialRoiIpcTransportReadStatus {
    kLine,
    kTimeout,
    kEof,
    kError,
    // The adapter rejected an oversized line before materializing it.
    kTooLarge,
};

struct SpatialRoiIpcTransportReadResult {
    SpatialRoiIpcTransportReadStatus status =
        SpatialRoiIpcTransportReadStatus::kError;
    std::string line;
    std::string error;
};

class SpatialRoiIpcLineTransport {
public:
    virtual ~SpatialRoiIpcLineTransport() = default;

    // The line supplied here is the exact serialized FRAME record, including
    // its single trailing newline.  A false return means no retransmit will be
    // attempted; the handoff latches fatal and retains the export.
    virtual bool WriteLine(const std::string& line,
                           std::string* error_out) = 0;

    // Read exactly one complete message line, waiting no longer than timeout.
    // The adapter must enforce max_wire_bytes before buffering/materializing a
    // line. It should return kTooLarge with an empty line when that bound is
    // exceeded. Implementations may throw; the handoff catches exceptions and
    // treats them as fatal transport failures.
    virtual SpatialRoiIpcTransportReadResult ReadLine(
        std::chrono::milliseconds timeout,
        std::size_t max_wire_bytes) = 0;
};

struct SpatialRoiIpcHandoffConfig {
    // This object is intended to be one logical stream.  The identity is
    // checked in full against every FRAME and every ACK/RELEASE.
    SpatialRoiIpcStreamIdentity expected_stream;

    // The synchronous implementation normally has one outstanding export,
    // but the bounded table is explicit so a future caller cannot accidentally
    // turn it into an unbounded ownership queue.
    std::size_t max_outstanding_frames = 1;
    std::chrono::milliseconds response_timeout{1000};
};

enum class SpatialRoiIpcHandoffResultStatus {
    // The peer admitted the frame and sent the exact RELEASE.
    kCompleted,
    // The peer rejected admission and sent the exact RELEASE.  ACK alone is
    // never sufficient to return this status.
    kRejected,
    // The export could not be built or was invalid before any FRAME byte was
    // written.  This is not a protocol-fatal state.
    kInvalidArgument,
    kBuildFailed,
    kOutstandingFull,
    // Submit was attempted before the producer/recorder HELLO exchange.
    kNotNegotiated,
    // A protocol, transport, timeout, or ownership failure latched the
    // handoff.  Any export already in the table remains indeterminate until
    // ConfirmPeerExited() is called.
    kFatal,
};

const char* spatial_roi_ipc_handoff_result_status_name(
    SpatialRoiIpcHandoffResultStatus status) noexcept;

struct SpatialRoiIpcHandoffResult {
    SpatialRoiIpcHandoffResultStatus status =
        SpatialRoiIpcHandoffResultStatus::kFatal;
    std::optional<SpatialRoiIpcCorrelation> correlation;
    std::string error;

    bool completed() const noexcept
    {
        return status == SpatialRoiIpcHandoffResultStatus::kCompleted;
    }

    bool rejected() const noexcept
    {
        return status == SpatialRoiIpcHandoffResultStatus::kRejected;
    }

    bool fatal() const noexcept
    {
        return status == SpatialRoiIpcHandoffResultStatus::kFatal;
    }
};

// Counters are monotonically incremented with saturation.  They are
// diagnostics only; no counter value is used to decide whether an export is
// still owned.  In particular, ACKs do not advance released_frames.
struct SpatialRoiIpcHandoffCounters {
    std::uint64_t submit_attempted = 0;
    std::uint64_t frames_inserted = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t ack_accepted = 0;
    std::uint64_t ack_rejected = 0;
    std::uint64_t releases = 0;
    std::uint64_t completed_frames = 0;
    std::uint64_t rejected_frames = 0;
    std::uint64_t outstanding_full = 0;
    std::uint64_t build_failures = 0;
    std::uint64_t invalid_arguments = 0;

    std::uint64_t malformed_messages = 0;
    std::uint64_t unexpected_messages = 0;
    std::uint64_t mismatched_messages = 0;
    std::uint64_t duplicate_messages = 0;
    std::uint64_t release_before_ack = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t eof = 0;
    std::uint64_t transport_errors = 0;
    std::uint64_t fatal_latches = 0;
    std::uint64_t peer_exit_confirmations = 0;
    std::uint64_t hello_sent = 0;
    std::uint64_t hello_received = 0;
    std::uint64_t negotiation_failures = 0;
    std::uint64_t oversized_messages = 0;

    std::uint64_t outstanding_high_water = 0;
};

// Synchronous one-logical-stream FRAME -> ACK -> RELEASE handoff. The
// handoff does not own the exporter or transport; both must outlive it. It is
// not thread-safe and must be called by one stream owner at a time.
//
// Supervisor precondition: exactly one instance may ever be constructed for
// (recording_identity_token, producer_generation, logical_stream_id). A new
// producer process/restart must mint a new producer_generation and construct a
// fresh endpoint. This class intentionally has no process-global registry; the
// production supervisor must enforce this endpoint-lifecycle gate.
class SpatialRoiIpcHandoff final {
public:
    SpatialRoiIpcHandoff(SpatialRoiIpcFrameExporter& exporter,
                        SpatialRoiIpcLineTransport& transport,
                        SpatialRoiIpcHandoffConfig config);
    ~SpatialRoiIpcHandoff();

    SpatialRoiIpcHandoff(const SpatialRoiIpcHandoff&) = delete;
    SpatialRoiIpcHandoff& operator=(const SpatialRoiIpcHandoff&) = delete;
    SpatialRoiIpcHandoff(SpatialRoiIpcHandoff&&) = delete;
    SpatialRoiIpcHandoff& operator=(SpatialRoiIpcHandoff&&) = delete;

    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }
    const SpatialRoiIpcHandoffConfig& config() const noexcept { return config_; }

    bool fatal_latched() const noexcept { return fatal_latched_; }
    bool negotiated() const noexcept { return negotiated_; }
    bool peer_exited_confirmed() const noexcept
    {
        return peer_exited_confirmed_;
    }
    // True when at least one full export/envelope still cannot be safely
    // returned to its producer because the peer did not complete RELEASE.
    bool ownership_indeterminate() const noexcept
    {
        return outstanding_ && !outstanding_->empty();
    }
    std::size_t outstanding_count() const noexcept
    {
        return outstanding_ ? outstanding_->size() : 0;
    }
    SpatialRoiIpcHandoffCounters counters() const noexcept { return counters_; }
    const std::string& last_error() const noexcept { return last_error_; }

    // Send the producer HELLO and synchronously receive the recorder HELLO.
    // Both messages must contain the full expected stream identity, the
    // opposite role, a bounded queue capacity, and exactly the required
    // feature set. This is idempotent only after successful negotiation.
    bool Negotiate(std::string* error_out = nullptr);

    // Build through the verified exporter, then perform the synchronous
    // handoff.  Build failure happens before table admission and no FRAME is
    // written.  Once a FRAME is written, any non-success result is fatal and
    // leaves the export in outstanding_ until peer exit is confirmed.
    SpatialRoiIpcHandoffResult Submit(
        const SpatialRoiLaneDelivery& delivery,
        int assigned_recorder_gpu_id,
        int assigned_shard_id);

#ifdef ORANGE_SPATIAL_ROI_HANDOFF_TESTING
    // Test-only prepared-frame seam. Production builds do not expose a way to
    // bypass SpatialRoiIpcFrameExporter::Build(). The owner is deliberately a
    // separate non-null token so host tests can prove retained ownership
    // without manufacturing a private batch envelope.
    SpatialRoiIpcHandoffResult SubmitPreparedForTest(
        SpatialRoiIpcExport export_value,
        std::shared_ptr<void> retained_test_owner);

    static std::uint64_t quarantined_destructor_count() noexcept;
#endif

    // A fatal transport/protocol path cannot infer whether the recorder still
    // owns the CUDA allocation.  This explicit external fact is the only
    // operation that clears retained indeterminate exports.  It does not
    // clear fatal_latched_; callers must construct a fresh handoff for a new
    // producer epoch and must not retransmit the old FRAME.
    bool ConfirmPeerExited() noexcept;

private:
    struct Outstanding {
        SpatialRoiIpcExport export_value;
        std::shared_ptr<void> retained_test_owner;
        bool ack_seen = false;
        bool ack_accepted = false;
    };

    using OutstandingTable = std::unordered_map<
        SpatialRoiIpcCorrelationKey,
        Outstanding,
        SpatialRoiIpcCorrelationKeyHash>;

    static void increment(std::uint64_t* value) noexcept;
    static std::string bounded_error(const std::string& message);
    void latch_fatal(const char* message,
                     std::uint64_t* reason_counter) noexcept;
    void latch_fatal(const std::string& message,
                     std::uint64_t* reason_counter) noexcept;
    SpatialRoiIpcHandoffResult make_fatal_result(
        const SpatialRoiIpcCorrelation* correlation,
        std::string_view message) noexcept;
    SpatialRoiIpcHandoffResult submit_export_impl(
        SpatialRoiIpcExport export_value,
        std::shared_ptr<void> retained_test_owner,
        bool test_prepared);
    bool expected_stream_matches(
        const SpatialRoiFrameDescriptor& descriptor,
        std::string* error_out) const;
    bool response_correlation_matches(
        const SpatialRoiIpcCorrelation& actual,
        const SpatialRoiIpcCorrelation& expected) const noexcept;
    SpatialRoiIpcTransportReadResult read_response(
        std::uint64_t* counter,
        std::string* error_out) noexcept;
    bool validate_hello(const SpatialRoiIpcHello& hello,
                        std::string* error_out) const;

    SpatialRoiIpcFrameExporter* exporter_ = nullptr;
    SpatialRoiIpcLineTransport* transport_ = nullptr;
    SpatialRoiIpcHandoffConfig config_;
    bool valid_ = false;
    bool fatal_latched_ = false;
    bool negotiated_ = false;
    bool peer_exited_confirmed_ = false;
    std::string error_;
    std::string last_error_;
    std::unique_ptr<OutstandingTable> outstanding_;
    std::uint64_t last_submitted_roi_stream_frame_index_ = 0;
    SpatialRoiIpcHandoffCounters counters_;
};

}  // namespace orange::spatial_roi::ipc
