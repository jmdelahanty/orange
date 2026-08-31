#include "spatial_roi_acquisition_bridge.h"

#include "worker_entry_release.h"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace orange::spatial_roi {

namespace {

void set_error(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
}

const char* context_part(const char* value)
{
    return value ? value : "";
}

bool checked_packed_byte_count(
    const WORKER_ENTRY& entry,
    std::uint64_t* expected_bytes)
{
    if (entry.width <= 0 || entry.height <= 0) {
        return false;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(entry.width);
    const std::uint64_t height = static_cast<std::uint64_t>(entry.height);
    if (height != 0 && width > std::numeric_limits<std::uint64_t>::max() / height) {
        return false;
    }
    *expected_bytes = width * height;
    return true;
}

bool has_complete_identity(const SpatialRoiAcquisitionIdentity& identity)
{
    return !identity.recording_id.empty() &&
           !identity.recording_identity_token.empty() &&
           !identity.producer_generation.empty() &&
           identity.camera_id >= 0 &&
           !identity.camera_serial.empty() &&
           identity.local_frame_id != 0 &&
           identity.camera_frame_id != 0 &&
           identity.recording_frame_id != 0 &&
           identity.camera_timestamp_ns != 0 &&
           identity.timestamp_sys_ns != 0;
}

SpatialRoiFrameIdentity make_frame_identity(
    const SpatialRoiAcquisitionIdentity& identity)
{
    SpatialRoiFrameIdentity frame_identity;
    frame_identity.recording_id = identity.recording_id;
    frame_identity.recording_identity_token = identity.recording_identity_token;
    frame_identity.producer_generation = identity.producer_generation;
    frame_identity.camera_id = identity.camera_id;
    frame_identity.camera_serial = identity.camera_serial;
    frame_identity.local_frame_id = identity.local_frame_id;
    frame_identity.camera_frame_id = identity.camera_frame_id;
    frame_identity.recording_frame_id = identity.recording_frame_id;
    frame_identity.camera_timestamp_ns = identity.camera_timestamp_ns;
    frame_identity.timestamp_sys_ns = identity.timestamp_sys_ns;
    return frame_identity;
}

class WorkerEntrySourceLeaseHolder final {
public:
    WorkerEntrySourceLeaseHolder(
        SafeQueue<WORKER_ENTRY*>* recycle_queue,
        WORKER_ENTRY* entry,
        WorkerEntryReleaseContext context)
        : camera_serial_(context_part(context.camera_serial)),
          worker_name_(context_part(context.worker_name)),
          lease_(
              recycle_queue,
              entry,
              WorkerEntryReleaseContext{
                  camera_serial_.empty() ? nullptr : camera_serial_.c_str(),
                  worker_name_.empty() ? nullptr : worker_name_.c_str()})
    {
    }

    WorkerEntrySourceLeaseHolder(const WorkerEntrySourceLeaseHolder&) = delete;
    WorkerEntrySourceLeaseHolder& operator=(
        const WorkerEntrySourceLeaseHolder&) = delete;

    bool active() const noexcept { return lease_.active(); }

private:
    // WorkerEntryLease stores context pointers.  Keep owned copies before
    // constructing it so an acquisition caller may pass stack-backed context
    // strings without creating a dangling pointer in an accepted envelope.
    std::string camera_serial_;
    std::string worker_name_;
    WorkerEntryLease lease_;
};

}  // namespace

const char* spatial_roi_acquisition_bridge_status_name(
    SpatialRoiAcquisitionBridgeStatus status) noexcept
{
    switch (status) {
    case SpatialRoiAcquisitionBridgeStatus::kSubmitted:
        return "submitted";
    case SpatialRoiAcquisitionBridgeStatus::kRuntimeIncomplete:
        return "runtime_incomplete";
    case SpatialRoiAcquisitionBridgeStatus::kMissingEntry:
        return "missing_entry";
    case SpatialRoiAcquisitionBridgeStatus::kMissingRecycleQueue:
        return "missing_recycle_queue";
    case SpatialRoiAcquisitionBridgeStatus::kMissingRuntime:
        return "missing_runtime";
    case SpatialRoiAcquisitionBridgeStatus::kNotRecording:
        return "not_recording";
    case SpatialRoiAcquisitionBridgeStatus::kGpuDirect:
        return "gpu_direct";
    case SpatialRoiAcquisitionBridgeStatus::kUnowned:
        return "unowned";
    case SpatialRoiAcquisitionBridgeStatus::kNotMono8:
        return "not_mono8";
    case SpatialRoiAcquisitionBridgeStatus::kInvalidDimensions:
        return "invalid_dimensions";
    case SpatialRoiAcquisitionBridgeStatus::kStrideUnknown:
        return "stride_unknown";
    case SpatialRoiAcquisitionBridgeStatus::kMissingSource:
        return "missing_source";
    case SpatialRoiAcquisitionBridgeStatus::kSourceGpuUnknown:
        return "source_gpu_unknown";
    case SpatialRoiAcquisitionBridgeStatus::kZeroIdentity:
        return "zero_identity";
    case SpatialRoiAcquisitionBridgeStatus::kIdentityMismatch:
        return "identity_mismatch";
    case SpatialRoiAcquisitionBridgeStatus::kLeaseUnavailable:
        return "lease_unavailable";
    case SpatialRoiAcquisitionBridgeStatus::kRuntimeRejected:
        return "runtime_rejected";
    case SpatialRoiAcquisitionBridgeStatus::kRuntimeException:
        return "runtime_exception";
    }
    return "unknown";
}

SpatialRoiAcquisitionBridgeStatus validate_spatial_roi_acquisition_input(
    const WORKER_ENTRY* entry,
    const SafeQueue<WORKER_ENTRY*>* recycle_queue,
    const SpatialRoiRecordingRuntime* runtime,
    const SpatialRoiAcquisitionIdentity& identity,
    std::string* error_out)
{
    if (!entry) {
        set_error(error_out, "WORKER_ENTRY is null");
        return SpatialRoiAcquisitionBridgeStatus::kMissingEntry;
    }
    if (!recycle_queue) {
        set_error(error_out, "WORKER_ENTRY recycle queue is null");
        return SpatialRoiAcquisitionBridgeStatus::kMissingRecycleQueue;
    }
    if (!runtime) {
        set_error(error_out, "spatial ROI recording runtime is null");
        return SpatialRoiAcquisitionBridgeStatus::kMissingRuntime;
    }
    if (!identity.recording_active) {
        set_error(error_out, "recording is not active for this WORKER_ENTRY");
        return SpatialRoiAcquisitionBridgeStatus::kNotRecording;
    }
    if (entry->gpu_direct_mode) {
        set_error(error_out, "GPU-direct WORKER_ENTRY is not an owned ROI source");
        return SpatialRoiAcquisitionBridgeStatus::kGpuDirect;
    }
    if (!entry->owns_memory) {
        set_error(error_out, "WORKER_ENTRY does not own its source memory");
        return SpatialRoiAcquisitionBridgeStatus::kUnowned;
    }
    if (entry->pixelFormat != GVSP_PIX_MONO8) {
        set_error(error_out, "WORKER_ENTRY pixel format is not packed Mono8");
        return SpatialRoiAcquisitionBridgeStatus::kNotMono8;
    }

    std::uint64_t expected_bytes = 0;
    if (!checked_packed_byte_count(*entry, &expected_bytes)) {
        set_error(error_out, "WORKER_ENTRY dimensions are not positive");
        return SpatialRoiAcquisitionBridgeStatus::kInvalidDimensions;
    }
    // A packed source is the only source for which pitch=width is proven by
    // this bridge.  Any other byte count means the stride/layout is unknown;
    // do not guess a pitch or let the producer read rows speculatively.
    if (entry->source_buffer_bytes != expected_bytes) {
        set_error(
            error_out,
            "WORKER_ENTRY source_buffer_bytes does not prove packed width pitch");
        return SpatialRoiAcquisitionBridgeStatus::kStrideUnknown;
    }
    if (!entry->delayed_consumer_image() ||
        !entry->delayed_consumer_event() ||
        !*entry->delayed_consumer_event()) {
        set_error(
            error_out,
            "WORKER_ENTRY delayed consumer image/event is incomplete");
        return SpatialRoiAcquisitionBridgeStatus::kMissingSource;
    }
    if (entry->image_gpu_id < 0) {
        set_error(error_out, "WORKER_ENTRY source GPU is unknown");
        return SpatialRoiAcquisitionBridgeStatus::kSourceGpuUnknown;
    }
    if (!has_complete_identity(identity)) {
        set_error(
            error_out,
            "recording identity is incomplete or contains a zero field");
        return SpatialRoiAcquisitionBridgeStatus::kZeroIdentity;
    }

    // These fields are authoritative on the packed entry itself.  The
    // recording/session strings are supplied separately, but frame counters
    // and timestamps must not be substituted from another frame.
    if (identity.local_frame_id != entry->frame_id ||
        identity.camera_frame_id != entry->camera_frame_id ||
        identity.recording_frame_id != entry->recording_frame_id ||
        identity.camera_timestamp_ns != entry->timestamp ||
        identity.timestamp_sys_ns != entry->timestamp_sys) {
        set_error(
            error_out,
            "recording identity does not match WORKER_ENTRY frame fields");
        return SpatialRoiAcquisitionBridgeStatus::kIdentityMismatch;
    }
    return SpatialRoiAcquisitionBridgeStatus::kSubmitted;
}

SpatialRoiAcquisitionBridgeResult submit_spatial_roi_acquisition(
    WORKER_ENTRY* entry,
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WorkerEntryReleaseContext context,
    SpatialRoiRecordingRuntime* runtime,
    const SpatialRoiAcquisitionIdentity& identity,
    SpatialRoiAcquisitionBridgePreSubmitHook pre_submit_hook)
{
    SpatialRoiAcquisitionBridgeResult bridge_result;
    // These three pointer checks must precede lease construction. The caller
    // still owns the acquisition/base reference at this boundary, so a
    // non-null entry remains valid while the bridge retains its own lease.
    if (!entry || !recycle_queue || !runtime) {
        bridge_result.bridge_status = validate_spatial_roi_acquisition_input(
            entry,
            recycle_queue,
            runtime,
            identity,
            &bridge_result.error);
        return bridge_result;
    }

    try {
        // Retain before validating frame fields so the entry cannot recycle
        // between validation and source-view construction. The holder is the
        // sole owner of the lease object. The source view only carries its
        // type-erased shared ownership token into the producer.
        auto lease_holder = std::make_shared<WorkerEntrySourceLeaseHolder>(
            recycle_queue,
            entry,
            context);
        if (!lease_holder->active()) {
            bridge_result.bridge_status =
                SpatialRoiAcquisitionBridgeStatus::kLeaseUnavailable;
            bridge_result.error =
                "WORKER_ENTRY lease could not be retained";
            return bridge_result;
        }

        bridge_result.bridge_status = validate_spatial_roi_acquisition_input(
            entry,
            recycle_queue,
            runtime,
            identity,
            &bridge_result.error);
        if (bridge_result.bridge_status !=
            SpatialRoiAcquisitionBridgeStatus::kSubmitted) {
            return bridge_result;
        }

        SpatialRoiSourceView source;
        source.device_data = entry->delayed_consumer_image();
        source.pitch_bytes = static_cast<std::size_t>(entry->width);
        source.allocation_bytes =
            static_cast<std::size_t>(entry->source_buffer_bytes);
        source.width = static_cast<std::uint32_t>(entry->width);
        source.height = static_cast<std::uint32_t>(entry->height);
        source.gpu_id = entry->image_gpu_id;
        source.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
        source.ready_event = *entry->delayed_consumer_event();
        source.source_lease = std::move(lease_holder);
        source.identity = make_frame_identity(identity);
        if (pre_submit_hook) {
            pre_submit_hook();
        }
        bridge_result.runtime_submission = runtime->TrySubmit(source);
    } catch (const std::exception& exception) {
        bridge_result.bridge_status =
            SpatialRoiAcquisitionBridgeStatus::kRuntimeException;
        bridge_result.error = exception.what();
        return bridge_result;
    } catch (...) {
        bridge_result.bridge_status =
            SpatialRoiAcquisitionBridgeStatus::kRuntimeException;
        bridge_result.error = "spatial ROI runtime TrySubmit threw";
        return bridge_result;
    }

    if (bridge_result.runtime_submission.status ==
        SpatialRoiRuntimeSubmitStatus::kAccepted) {
        bridge_result.bridge_status =
            SpatialRoiAcquisitionBridgeStatus::kSubmitted;
        return bridge_result;
    }
    if (bridge_result.runtime_submission.status ==
        SpatialRoiRuntimeSubmitStatus::kIncomplete) {
        bridge_result.bridge_status =
            SpatialRoiAcquisitionBridgeStatus::kRuntimeIncomplete;
        return bridge_result;
    }

    bridge_result.bridge_status =
        SpatialRoiAcquisitionBridgeStatus::kRuntimeRejected;
    bridge_result.error =
        "spatial ROI runtime rejected the source: " +
        std::string(spatial_roi_runtime_submit_status_name(
            bridge_result.runtime_submission.status));
    return bridge_result;
}

}  // namespace orange::spatial_roi
