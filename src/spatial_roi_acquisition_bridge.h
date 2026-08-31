#pragma once

#include "spatial_roi_recording_runtime.h"
#include "video_capture.h"

#include <cstdint>
#include <string>

struct WorkerEntryReleaseContext;

namespace orange::spatial_roi {

// This adapter is intentionally narrower than WORKER_ENTRY.  The acquisition
// loop owns the recording identity that is not present on WORKER_ENTRY and
// supplies it here after the recording/session state has been resolved.
struct SpatialRoiAcquisitionIdentity {
    bool recording_active = false;
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    int camera_id = -1;
    std::string camera_serial;
    std::uint64_t local_frame_id = 0;
    std::uint64_t camera_frame_id = 0;
    std::uint64_t recording_frame_id = 0;
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;
};

enum class SpatialRoiAcquisitionBridgeStatus {
    // The runtime accepted the producer result and all required lanes were
    // admitted.  The returned runtime submission owns the source lease.
    kSubmitted,
    // The producer result was admitted, but strict runtime fanout was
    // incomplete.  The returned envelope still owns the source lease.
    kRuntimeIncomplete,

    // Synchronous, pre-retain validation failures.
    kMissingEntry,
    kMissingRecycleQueue,
    kMissingRuntime,
    kNotRecording,
    kGpuDirect,
    kUnowned,
    kNotMono8,
    kInvalidDimensions,
    kStrideUnknown,
    kMissingSource,
    kSourceGpuUnknown,
    kZeroIdentity,
    kIdentityMismatch,
    kLeaseUnavailable,

    // TrySubmit returned a terminal runtime status or threw.  Inspect the
    // nested submission for the exact runtime status when it returned one.
    kRuntimeRejected,
    kRuntimeException,
};

const char* spatial_roi_acquisition_bridge_status_name(
    SpatialRoiAcquisitionBridgeStatus status) noexcept;

// Pure, non-retaining validation seam.  Every check here runs before
// TryRetainWorkerEntryLease, so rejection cannot alter WORKER_ENTRY's ref
// count or enqueue it for recycling.  The runtime pointer is only checked for
// presence; its verified-plan binding remains authoritative in TrySubmit.
SpatialRoiAcquisitionBridgeStatus validate_spatial_roi_acquisition_input(
    const WORKER_ENTRY* entry,
    const SafeQueue<WORKER_ENTRY*>* recycle_queue,
    const SpatialRoiRecordingRuntime* runtime,
    const SpatialRoiAcquisitionIdentity& identity,
    std::string* error_out = nullptr);

struct SpatialRoiAcquisitionBridgeResult {
    SpatialRoiAcquisitionBridgeStatus bridge_status =
        SpatialRoiAcquisitionBridgeStatus::kRuntimeRejected;
    SpatialRoiBatchSubmission runtime_submission;
    std::string error;

    bool submitted() const noexcept
    {
        return bridge_status == SpatialRoiAcquisitionBridgeStatus::kSubmitted;
    }

    bool runtime_envelope_retained() const noexcept
    {
        return static_cast<bool>(runtime_submission.envelope);
    }
};

// Test-only seam: when non-null, this hook is invoked after the source lease
// has been retained and the source view assembled, immediately before the
// runtime call.  Production callers leave it null.  The bridge catches an
// exception from the hook just like an allocation/constructor exception.
using SpatialRoiAcquisitionBridgePreSubmitHook = void (*)();

// Resolve one recording-active, packed Mono8 WORKER_ENTRY into a source view
// and submit it to the already-constructed verified-plan runtime. The caller
// must own the acquisition/base reference while entering this function; the
// bridge retains its own lease before inspecting mutable frame fields. The
// adapter never waits for a CUDA event, worker lane, or queue. Exactly one
// WorkerEntryLease is created, in a shared holder copied into source_lease;
// the producer/result/envelope then keep that holder alive until all admitted
// ROI lanes have released their references.
SpatialRoiAcquisitionBridgeResult submit_spatial_roi_acquisition(
    WORKER_ENTRY* entry,
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WorkerEntryReleaseContext context,
    SpatialRoiRecordingRuntime* runtime,
    const SpatialRoiAcquisitionIdentity& identity,
    SpatialRoiAcquisitionBridgePreSubmitHook pre_submit_hook = nullptr);

}  // namespace orange::spatial_roi
