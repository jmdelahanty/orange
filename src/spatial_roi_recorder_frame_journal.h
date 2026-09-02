#pragma once

#include "spatial_roi_ipc_protocol.h"
#include "spatial_roi_lossless_encoder.h"
#include "spatial_roi_recorder_evidence.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace orange::spatial_roi::recording {

// The complete recorder-side result of one FRAME dispatch.  The CUDA IPC
// handles are deliberately not retained by the journal: ownership has
// already been settled by the transport/detach path, while the descriptor and
// every lifecycle bit are retained until their evidence row is appended.
struct SpatialRoiRecorderFrameTransportOutcome {
    // The session already validated the complete wire FRAME before dispatch.
    // Retain only its scientific/geometry descriptor here; CUDA IPC handle
    // strings are capability material and must not outlive the detach seam.
    SpatialRoiFrameDescriptor descriptor;

    // Empty until the detach seam records an explicit outcome.  A default
    // construction must never imply successful ownership transfer.
    std::string detach_status;
    bool source_release_safe = false;

    // Dispatch admission is internal recorder truth and remains separate from
    // what the ACK writer managed to put on the wire.
    bool dispatch_admitted = false;
    std::string dispatch_reason;

    // ACK accepted is the accepted bit in the ACK payload. It is retained even
    // when the attempted write fails, so admission and transport delivery stay
    // independently observable.
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

// Short aliases make the type convenient at the transport seam without
// introducing a second representation of the outcome.
using SpatialRoiRecorderTransportOutcome =
    SpatialRoiRecorderFrameTransportOutcome;

// Derive the smallest pending-entry bound that can represent every frame
// legally owned by a bounded encoder plus the first terminal admission
// rejection. The encoder queue counts waiting work only: one additional frame
// may be active on its owner thread, and the following queue-full FRAME still
// needs one journal entry so its rejection can be proven. The result is
// clamped to the authenticated recording-length ceiling.
bool derive_spatial_roi_frame_journal_pending_bound(
    std::size_t max_frames_per_stream,
    std::size_t encoder_waiting_queue_capacity,
    std::size_t* bound_out,
    std::string* error_out = nullptr) noexcept;

struct SpatialRoiRecorderFrameJournalConfig {
    ipc::SpatialRoiIpcStreamIdentity expected_stream;

    // Exactly one writer form is required. A raw pointer is non-owning; the
    // shared pointer keeps the writer alive for the journal's lifetime.
    SpatialRoiRecorderEvidenceWriter* writer = nullptr;
    std::shared_ptr<SpatialRoiRecorderEvidenceWriter> shared_writer;

    std::size_t max_frames = 0;
    std::size_t max_pending_entries = 0;
};

struct SpatialRoiRecorderFrameJournalCounters {
    std::uint64_t transport_outcomes = 0;
    std::uint64_t dispatch_admitted = 0;
    std::uint64_t dispatch_rejected = 0;
    std::uint64_t encoder_results = 0;
    std::uint64_t frames_appended = 0;

    std::uint64_t ack_attempted = 0;
    std::uint64_t ack_sent = 0;
    std::uint64_t ack_accepted = 0;
    std::uint64_t ack_write_failures = 0;
    std::uint64_t release_attempted = 0;
    std::uint64_t release_sent = 0;
    std::uint64_t release_write_failures = 0;

    std::uint64_t duplicate_or_conflict_rejections = 0;
    std::uint64_t cross_stream_rejections = 0;
    std::uint64_t invalid_input_rejections = 0;
    std::uint64_t pending_overflow_rejections = 0;
    std::uint64_t pending_entries_high_water = 0;
    std::uint64_t append_failures = 0;
};

// One coherent, allocation-free operational observation.  Every field is
// copied while holding the journal state mutex exactly once, so lifecycle
// telemetry cannot combine a pre-drain pending count with post-drain
// counters.  A mutex/runtime failure is represented by
// observation_succeeded=false rather than escaping this no-throw boundary.
// The snapshot performs no evidence I/O and is safe to sample from the
// recorder control thread while transport/encoder callbacks continue to
// stage bounded records.
struct SpatialRoiRecorderFrameJournalOperationalSnapshot final {
    bool observation_succeeded = false;
    bool valid = false;
    bool fatal_latched = false;
    bool final_ready = false;

    std::size_t max_frames = 0;
    std::size_t pending_entries_limit = 0;
    std::size_t pending_entries_current = 0;
    std::size_t transport_count = 0;
    std::size_t appended_count = 0;
    std::uint64_t next_flush_index = 0;

    // Includes pending_entries_high_water, transport/encoder/appended
    // cardinalities, ACK/RELEASE lifecycle truth, rejection classes,
    // pending-overflow rejections, and append failures.
    SpatialRoiRecorderFrameJournalCounters counters;
};

// Joins one transport outcome with its asynchronous lossless encoder result.
// Only incomplete entries are retained. Completed entries are appended in
// one-based ROI index order and immediately erased, so memory is bounded by
// max_pending_entries rather than by recording length.
class SpatialRoiRecorderFrameJournal final {
public:
    explicit SpatialRoiRecorderFrameJournal(
        SpatialRoiRecorderFrameJournalConfig config);

    SpatialRoiRecorderFrameJournal(
        SpatialRoiRecorderEvidenceWriter& writer,
        ipc::SpatialRoiIpcStreamIdentity expected_stream,
        std::size_t max_frames,
        std::size_t max_pending_entries);

    SpatialRoiRecorderFrameJournal(
        std::shared_ptr<SpatialRoiRecorderEvidenceWriter> writer,
        ipc::SpatialRoiIpcStreamIdentity expected_stream,
        std::size_t max_frames,
        std::size_t max_pending_entries);

    ~SpatialRoiRecorderFrameJournal() = default;

    SpatialRoiRecorderFrameJournal(const SpatialRoiRecorderFrameJournal&) =
        delete;
    SpatialRoiRecorderFrameJournal& operator=(
        const SpatialRoiRecorderFrameJournal&) = delete;

    bool valid() const noexcept;
    bool fatal_latched() const noexcept;
    bool failed() const noexcept { return fatal_latched(); }

    // Both entry points are noexcept. In particular, this is safe to install
    // directly as SpatialRoiLosslessFrameResultCallback.
    bool AcceptTransportOutcome(
        const SpatialRoiRecorderFrameTransportOutcome& outcome,
        std::string* error_out = nullptr) noexcept;
    // Ingestion only records a bounded in-memory result. It never calls the
    // evidence writer and is therefore safe to install as the encoder's
    // synchronous result callback. Call Drain/Flush from the recorder-owned
    // non-callback context to perform blocking evidence I/O.
    bool AcceptEncoderResult(
        const encoder::SpatialRoiLosslessFrameResult& result,
        std::string* error_out = nullptr) noexcept;

    bool Drain(std::size_t max_entries = 0,
               std::string* error_out = nullptr) noexcept;
    bool Flush(std::string* error_out = nullptr) noexcept
    {
        return Drain(0, error_out);
    }
    bool DrainReady(std::size_t max_entries = 0,
                    std::string* error_out = nullptr) noexcept
    {
        return Drain(max_entries, error_out);
    }
    bool FlushReady(std::string* error_out = nullptr) noexcept
    {
        return Drain(0, error_out);
    }

    bool OnTransportOutcome(
        const SpatialRoiRecorderFrameTransportOutcome& outcome,
        std::string* error_out = nullptr) noexcept
    {
        return AcceptTransportOutcome(outcome, error_out);
    }
    bool OnEncoderResult(
        const encoder::SpatialRoiLosslessFrameResult& result,
        std::string* error_out = nullptr) noexcept
    {
        return AcceptEncoderResult(result, error_out);
    }

    bool operator()(const encoder::SpatialRoiLosslessFrameResult& result,
                    std::string* error_out = nullptr) noexcept
    {
        return AcceptEncoderResult(result, error_out);
    }

    // Final readiness does not finalize the evidence writer. The caller must
    // still provide the writer's terminal encoder snapshot and finalize it.
    bool final_ready() const noexcept;
    bool ready() const noexcept { return final_ready(); }
    bool ready_to_finalize() const noexcept { return final_ready(); }

    std::size_t pending_entries() const noexcept;
    std::size_t max_frames() const noexcept;
    std::size_t max_pending_entries() const noexcept;
    std::size_t transport_count() const noexcept;
    std::size_t appended_count() const noexcept;

    SpatialRoiRecorderFrameJournalCounters counters() const noexcept;
    SpatialRoiRecorderFrameJournalOperationalSnapshot operational_snapshot()
        const noexcept;
    std::string first_failure_reason() const noexcept;
    std::string error() const noexcept { return first_failure_reason(); }

private:
    struct TransportRecord {
        SpatialRoiFrameDescriptor descriptor;
        std::string detach_status;
        bool source_release_safe = false;
        bool dispatch_admitted = false;
        std::string dispatch_reason;
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

    struct Entry {
        std::optional<TransportRecord> transport;
        std::optional<encoder::SpatialRoiLosslessFrameResult> encoder_result;
    };

    bool accept_transport_locked(
        const SpatialRoiRecorderFrameTransportOutcome& outcome,
        std::string* error_out);
    bool accept_encoder_locked(
        const encoder::SpatialRoiLosslessFrameResult& result,
        std::string* error_out);
    bool stage_ready_locked(std::string* error_out);
    bool drain_ready(std::size_t max_entries, std::string* error_out);
    bool append_entry(const Entry& entry, std::string* error_out);
    bool latch_failure_locked(const std::string& reason,
                              std::string* error_out) noexcept;
    bool reject_locked(const std::string& reason,
                       std::string* error_out) noexcept;
    bool validate_transport(const SpatialRoiRecorderFrameTransportOutcome& outcome,
                            std::string* error_out) const;
    bool validate_encoder(
        const encoder::SpatialRoiLosslessFrameResult& result,
        std::string* error_out) const;
    static bool same_transport(const TransportRecord& lhs,
                               const TransportRecord& rhs) noexcept;
    static bool same_encoder_result(
        const encoder::SpatialRoiLosslessFrameResult& lhs,
        const encoder::SpatialRoiLosslessFrameResult& rhs) noexcept;

    mutable std::mutex mutex_;
    // Serializes blocking writer I/O without holding mutex_, which is shared
    // by the encoder callback-facing ingestion methods.
    mutable std::mutex drain_mutex_;
    std::shared_ptr<SpatialRoiRecorderEvidenceWriter> shared_writer_;
    SpatialRoiRecorderEvidenceWriter* writer_ = nullptr;
    ipc::SpatialRoiIpcStreamIdentity expected_stream_;
    std::size_t max_frames_ = 0;
    std::size_t max_pending_entries_ = 0;

    std::map<std::uint64_t, Entry> pending_;
    std::size_t transport_count_ = 0;
    std::size_t appended_count_ = 0;
    std::uint64_t next_flush_index_ = 1;
    bool valid_ = false;
    bool fatal_ = false;
    std::string first_failure_reason_;
    SpatialRoiRecorderFrameJournalCounters counters_;
};

}  // namespace orange::spatial_roi::recording
