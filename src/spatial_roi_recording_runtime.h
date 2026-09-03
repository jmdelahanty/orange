#pragma once

#include "json.hpp"
#include "spatial_roi_batch_producer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orange::spatial_roi {

// A sink is called on the lane's worker thread after extraction has completed.
// It must not return kCompleted until the downstream handoff has reached a
// source-safe boundary (for example, a detached copy or an explicit recorder
// RELEASE). This is a handoff terminal, not proof that session finalization
// succeeded. Retaining the shared envelope retains the producer slot and
// source lease. A sink that cannot accept this ROI must return kRejected,
// while an operational or handoff failure must return kFailed. A missing sink
// and a callback exception both fail closed as kFailed.
enum class SpatialRoiLaneSinkResult {
    kCompleted,
    kRejected,
    kFailed,
};

const char* spatial_roi_lane_sink_result_name(
    SpatialRoiLaneSinkResult result) noexcept;

enum class SpatialRoiLaneTerminalReason {
    kPending,
    kCompleted,
    kSinkRejected,
    kSinkFailed,
    kSourceQuarantined,
    kQueueFull,
    kQueueAdmissionFailed,
    kStopped,
};

const char* spatial_roi_lane_terminal_reason_name(
    SpatialRoiLaneTerminalReason reason) noexcept;

enum class SpatialRoiRuntimeSubmitStatus {
    // The producer admitted the batch and every ROI lane accepted its item.
    // Individual sinks can still fail asynchronously; inspect the envelope's
    // completion status after drain.
    kAccepted,
    // The producer admitted the batch, but one or more required lanes could
    // not be admitted. This is the default strict-mode result.
    kIncomplete,
    kStopped,
    kInvalidArgument,
    kPoolExhausted,
    kCudaError,
    kSourceQuarantined,
    // The recording frame identity did not advance beyond the last batch
    // admitted by this runtime instance.
    kDuplicateOrOutOfOrder,
    // Another caller currently owns the single-camera admission path. The
    // caller must drop this source or retry only if its ownership contract
    // permits doing so; TrySubmit never waits for that caller.
    kBusy,
};

const char* spatial_roi_runtime_submit_status_name(
    SpatialRoiRuntimeSubmitStatus status) noexcept;

enum class SpatialRoiBatchCompletionStatus {
    kPending,
    kComplete,
    kIncomplete,
};

const char* spatial_roi_batch_completion_status_name(
    SpatialRoiBatchCompletionStatus status) noexcept;

struct SpatialRoiBatchTerminalSnapshot {
    std::size_t required_lane_count = 0;
    std::size_t terminal_lane_count = 0;
    SpatialRoiBatchCompletionStatus status =
        SpatialRoiBatchCompletionStatus::kPending;
    std::vector<SpatialRoiLaneTerminalReason> lane_reasons;
};

// Move-only ownership of one extracted batch. The runtime creates exactly one
// envelope per accepted producer result and gives a shared reference to each
// admitted ROI lane. The contained result must not be reset by a sink; its
// lifetime is the envelope's lifetime.
class SpatialRoiBatchEnvelope final {
public:
    SpatialRoiBatchEnvelope() = delete;
    ~SpatialRoiBatchEnvelope();

    SpatialRoiBatchEnvelope(const SpatialRoiBatchEnvelope&) = delete;
    SpatialRoiBatchEnvelope& operator=(const SpatialRoiBatchEnvelope&) = delete;
    SpatialRoiBatchEnvelope(SpatialRoiBatchEnvelope&& other) noexcept;
    SpatialRoiBatchEnvelope& operator=(SpatialRoiBatchEnvelope&& other) noexcept;

    const SpatialRoiBatchResult& result() const noexcept { return result_; }
    const std::vector<SpatialRoiWorkItem>& work_items() const noexcept
    {
        return work_items_;
    }
    std::uint64_t batch_sequence() const noexcept { return batch_sequence_; }
    std::size_t required_lane_count() const noexcept
    {
        return required_lane_count_;
    }

    // This snapshot is race-free and reflects terminal outcomes observed so
    // far. kComplete/kIncomplete are published only once every required lane
    // has reached a terminal reason.
    SpatialRoiBatchTerminalSnapshot terminal_snapshot() const;
    bool strict_complete() const;

private:
    friend class SpatialRoiRecordingRuntime;

    SpatialRoiBatchEnvelope(
        SpatialRoiBatchResult&& result,
        std::vector<SpatialRoiWorkItem>&& work_items,
        std::uint64_t batch_sequence);

    // Returns true exactly when this call reaches the final required lane.
    bool mark_lane_terminal(
        std::size_t lane_index,
        SpatialRoiLaneTerminalReason reason) noexcept;

    SpatialRoiBatchResult result_;
    std::vector<SpatialRoiWorkItem> work_items_;
    std::uint64_t batch_sequence_ = 0;
    std::size_t required_lane_count_ = 0;

    mutable std::mutex terminal_mutex_;
    std::vector<SpatialRoiLaneTerminalReason> lane_reasons_;
    std::size_t terminal_lane_count_ = 0;
    SpatialRoiBatchCompletionStatus completion_status_ =
        SpatialRoiBatchCompletionStatus::kPending;
};

// Immutable delivery identity assigned by the individual ROI lane when its
// queue admits an item.  The index is deliberately not stored in the shared
// batch envelope: one source batch fans out to independent ROI streams, each
// of which has its own dense one-based sequence.  The constructor is private
// so a sink cannot manufacture a new lane/index/envelope combination, and the
// fields are const so an accepted identity cannot be changed before it reaches
// an exporter.  A sink may retain this value together with envelope for an
// asynchronous IPC or metadata handoff.
struct SpatialRoiLaneDelivery final {
    const std::size_t lane_index;
    const std::uint64_t roi_stream_frame_index;
    const std::shared_ptr<const SpatialRoiBatchEnvelope> envelope;

private:
    friend class SpatialRoiRecordingRuntime;

    SpatialRoiLaneDelivery(
        std::size_t lane_index,
        std::uint64_t roi_stream_frame_index,
        std::shared_ptr<const SpatialRoiBatchEnvelope> envelope)
        : lane_index(lane_index),
          roi_stream_frame_index(roi_stream_frame_index),
          envelope(std::move(envelope))
    {
    }
};

// The preferred sink API.  The delivery value is immutable from the sink's
// perspective and contains all identity needed by a future exporter.
using SpatialRoiLaneSink = std::function<SpatialRoiLaneSinkResult(
    const SpatialRoiLaneDelivery& delivery)>;

// Source compatibility for clients that have not yet migrated to the
// delivery-value callback.  Runtime internals always invoke SpatialRoiLaneSink
// and adapt this form at construction.  New code should use SpatialRoiLaneSink
// so the per-lane stream index cannot be accidentally omitted.
using SpatialRoiLegacyLaneSink = std::function<SpatialRoiLaneSinkResult(
    std::size_t lane_index,
    std::shared_ptr<const SpatialRoiBatchEnvelope> envelope)>;

struct SpatialRoiRecordingRuntimeCounters {
    std::uint64_t submit_attempted = 0;
    std::uint64_t producer_accepted = 0;
    std::uint64_t producer_invalid_argument = 0;
    std::uint64_t producer_pool_exhausted = 0;
    std::uint64_t producer_cuda_error = 0;
    std::uint64_t producer_source_quarantined = 0;
    std::uint64_t producer_stopped = 0;
    std::uint64_t admission_busy = 0;
    std::uint64_t duplicate_or_out_of_order = 0;

    std::uint64_t lane_admitted = 0;
    std::uint64_t lane_queue_full = 0;
    std::uint64_t lane_queue_admission_failed = 0;
    std::uint64_t lane_stopped = 0;
    std::uint64_t lane_completed = 0;
    std::uint64_t lane_sink_rejected = 0;
    std::uint64_t lane_sink_failed = 0;
    std::uint64_t lane_source_quarantined = 0;

    // A batch is counted exactly once, when every required lane has reached a
    // terminal reason. Schema v1 is always strict, so
    // strict_incomplete_batches advances with batches_incomplete.
    std::uint64_t batches_complete = 0;
    std::uint64_t batches_incomplete = 0;
    std::uint64_t strict_incomplete_batches = 0;
};

struct SpatialRoiBatchSubmission {
    SpatialRoiRuntimeSubmitStatus status =
        SpatialRoiRuntimeSubmitStatus::kStopped;
    SpatialRoiBatchStatus producer_status = SpatialRoiBatchStatus::kStopped;
    std::uint64_t batch_sequence = 0;
    std::size_t lane_count = 0;
    std::size_t admitted_lane_count = 0;
    std::vector<SpatialRoiLaneTerminalReason> lane_reasons;
    // Present for an admitted producer result, including a strict-incomplete
    // fanout. Keeping this reference is optional; queued lanes own their own
    // references and source/result lifetime does not depend on the caller.
    std::shared_ptr<const SpatialRoiBatchEnvelope> envelope;

    bool producer_accepted() const noexcept
    {
        return producer_status == SpatialRoiBatchStatus::kAccepted;
    }
};

// Runtime owner for one verified-plan camera. Construction accepts only the
// immutable verified plan JSON, camera serial, and GPU; it materializes exact
// producer limits from that plan and creates one bounded lane per plan ROI.
// No constructor accepts caller-supplied work-item geometry or producer
// limits.
class SpatialRoiRecordingRuntime final {
public:
    SpatialRoiRecordingRuntime(
        const nlohmann::json& verified_plan,
        std::string camera_serial,
        int gpu_id,
        SpatialRoiLaneSink sink = {});
    SpatialRoiRecordingRuntime(
        const nlohmann::json& verified_plan,
        std::string camera_serial,
        int gpu_id,
        SpatialRoiLegacyLaneSink sink);
    ~SpatialRoiRecordingRuntime();

    SpatialRoiRecordingRuntime(const SpatialRoiRecordingRuntime&) = delete;
    SpatialRoiRecordingRuntime& operator=(const SpatialRoiRecordingRuntime&) = delete;
    SpatialRoiRecordingRuntime(SpatialRoiRecordingRuntime&&) = delete;
    SpatialRoiRecordingRuntime& operator=(SpatialRoiRecordingRuntime&&) = delete;

    // This path never waits for another submitter or a lane queue. It may
    // reject synchronously when admission is busy, the producer pool is
    // exhausted, or a lane queue is full. CUDA extraction remains
    // asynchronous; lane threads wait on the batch completion event. Rare
    // CUDA enqueue-error cleanup may synchronize to prove source safety.
    SpatialRoiBatchSubmission TrySubmit(const SpatialRoiSourceView& source);

    // Linearize shutdown against TrySubmit. No submission can enqueue after
    // this returns. Drain() then joins every lane after FIFO processing of all
    // already-admitted work. A sink may request stop/drain re-entrantly; that
    // call performs stop-only to avoid self-join, and the owner must later
    // drain from a non-lane thread. Destroying the runtime from a sink callback
    // is forbidden.
    void StopAccepting() noexcept;
    void Drain() noexcept;
    // Returns false when any admitted lane terminates incompletely (including
    // sink rejection/failure, source quarantine, or a queue admission drop),
    // or when invoked re-entrantly from a lane callback where joining would
    // self-deadlock. fully_drained_out distinguishes a completed join with a
    // reported lane failure from the latter stop-only boundary. The optional
    // error is bounded by the implementation and is best-effort because this
    // boundary is noexcept.
    bool StopAcceptingAndDrain(
        std::string* error_out = nullptr,
        bool* fully_drained_out = nullptr) noexcept;

    bool accepting() const noexcept;
    bool failed() const noexcept;
    std::string failure_reason() const;
    bool strict_mode() const noexcept { return true; }
    std::size_t lane_count() const noexcept { return lanes_.size(); }
    std::size_t lane_queue_capacity() const noexcept
    {
        return lane_queue_capacity_;
    }
    const SpatialRoiBatchLimits& producer_limits() const noexcept
    {
        return producer_->limits();
    }
    SpatialRoiRecordingRuntimeCounters counters() const noexcept;

private:
    class Lane;

    void mark_lane_terminal(
        const std::shared_ptr<SpatialRoiBatchEnvelope>& envelope,
        std::size_t lane_index,
        SpatialRoiLaneTerminalReason reason) noexcept;
    void fail_fast_after_sink_failure(
        std::size_t failed_lane_index,
        SpatialRoiLaneTerminalReason reason) noexcept;
    void latch_failure(const char* reason) noexcept;
    void QuarantineAfterCudaCompletionFailure() noexcept;
    static SpatialRoiRuntimeSubmitStatus map_producer_status(
        SpatialRoiBatchStatus status) noexcept;
    static SpatialRoiBatchStatus map_runtime_terminal_status(
        SpatialRoiRuntimeSubmitStatus status) noexcept;
    static SpatialRoiWorkItem make_work_item(
        const SpatialRoiPlanRoiBinding& descriptor,
        const SpatialRoiFrameIdentity& identity,
        const std::string& plan_sha256);

    std::unique_ptr<SpatialRoiBatchProducer> producer_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::size_t lane_queue_capacity_ = 0;
    mutable std::mutex admission_mutex_;
    mutable std::mutex drain_mutex_;
    mutable std::mutex failure_mutex_;
    bool accepting_ = false;
    SpatialRoiRuntimeSubmitStatus terminal_status_ =
        SpatialRoiRuntimeSubmitStatus::kStopped;
    std::uint64_t next_batch_sequence_ = 1;
    std::uint64_t last_admitted_recording_frame_id_ = 0;
    // A sink terminal is asynchronous to TrySubmit. Keep its fail-fast stop
    // request distinct from failed_: failed_ also records strict queue-full
    // batches, which do not invalidate later dense lane admission.
    std::atomic<bool> sink_failure_stop_requested_{false};
    bool failed_ = false;
    std::string first_failure_;

    struct AtomicCounters;
    std::unique_ptr<AtomicCounters> counters_;
};

}  // namespace orange::spatial_roi
