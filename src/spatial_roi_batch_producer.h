#pragma once

#include <cuda_runtime_api.h>
#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orange::spatial_roi {

// Camera-native and encoded-raster rectangles use half-open pixel bounds:
// [x, x + width) x [y, y + height).
struct SpatialRoiRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct SpatialRoiRaster {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The producer's allocation contract is an ordered list of the exact raster
// capacities admitted by the recording plan.  A capacity is expressed as a
// tightly-packed Mono8 raster; its byte count is checked (and derived) by the
// producer rather than rounded up to a common maximum.
using SpatialRoiOutputCapacity = SpatialRoiRaster;

// The v1 producer accepts only a single-byte camera-native source. Unknown is
// deliberately the default so a caller must state the pixel contract instead
// of silently treating an arbitrary device image as Mono8.
enum class SpatialRoiSourcePixelFormat {
    kUnknown = 0,
    kMono8,
};

// Source identity is repeated on every work item so an independently routed
// ROI remains self-identifying after it leaves the batch. All work items in a
// batch must carry exactly the same source identity.
struct SpatialRoiFrameIdentity {
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

struct SpatialRoiOutputGeometry {
    // Exact Mono8 pixels copied from the camera-native source.
    SpatialRoiRect content_rect;

    // Encoded output allocation. Pixels outside encoded_content_rect are
    // explicitly zero-filled by SpatialRoiBatchProducer.
    SpatialRoiRaster encoded_raster;
    SpatialRoiRect encoded_content_rect;
};

struct SpatialRoiWorkItem {
    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    std::string arena_id;
    std::string logical_stream_id;
    std::string spatial_roi_plan_sha256;
    SpatialRoiFrameIdentity source;
    SpatialRoiOutputGeometry geometry;
};

namespace detail {
struct SpatialRoiVerifiedPlanCapability;
}

// The ordered, immutable ROI binding copied from a verified recording plan.
// It is kept separate from SpatialRoiWorkItem so callers cannot accidentally
// make a producer's admission contract depend on a mutable work item.
struct SpatialRoiPlanRoiBinding {
    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    std::string arena_id;
    std::string logical_stream_id;
    SpatialRoiRect source_rect;
    SpatialRoiRaster encoded_raster;
    SpatialRoiRect encoded_content_rect;
    std::size_t output_bytes = 0;
};

// The acquisition integration resolves this view from WORKER_ENTRY's
// delayed_consumer_image()/delayed_consumer_event() and supplies a source_lease
// owning that allocation/event pair. This primitive never examines detections
// and has no YOLO dependency.
struct SpatialRoiSourceView {
    const unsigned char* device_data = nullptr;
    std::size_t pitch_bytes = 0;
    // Known size of the pitched device allocation in bytes. The producer
    // requires at least pitch_bytes * height so every source row is covered;
    // a larger backing allocation is permitted.
    std::size_t allocation_bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int gpu_id = -1;
    // Only kMono8 is supported in schema v1. The default is deliberately
    // rejected so an arbitrary device image cannot be treated as Mono8.
    SpatialRoiSourcePixelFormat pixel_format =
        SpatialRoiSourcePixelFormat::kUnknown;
    // Opaque ownership of the allocation/event lease. The producer never
    // dereferences this token; it retains the token until the source read is
    // fenced, and quarantines its entire pool if cleanup cannot prove that
    // fence. A raw pointer plus a ready event is not sufficient for
    // asynchronous input.
    std::shared_ptr<void> source_lease;
    // Mandatory fence for the producer's asynchronous source read. The
    // producer always waits on this event before issuing a native-pixel copy;
    // The source_lease owns the event through the batch completion fence (or
    // the producer's process-lifetime quarantine after failed cleanup).
    cudaEvent_t ready_event = nullptr;
    SpatialRoiFrameIdentity identity;
};

struct SpatialRoiBatchLimits {
    int gpu_id = -1;
    std::size_t batch_slot_count = 0;
    std::size_t max_rois_per_batch = 0;

    // A verified plan's pool depth is the producer's batch-slot count.  The
    // byte fields are the exact per-camera Mono8 pool admission, not a
    // maximum supplied by a caller.
    std::uint32_t pool_frames_per_stream = 0;
    std::uint64_t admission_pool_bytes = 0;
    // Compatibility spelling for clients that called this value an expected
    // pool size.  Verified-plan construction populates both and validation
    // requires every populated spelling to agree with the allocation size.
    std::uint64_t expected_pool_bytes = 0;

    std::string expected_recording_id;
    std::string expected_recording_identity_token;
    std::string expected_producer_generation;

    // These values are authoritative bindings from the verified recording
    // plan.  They are deliberately required so a producer cannot be reused
    // for another camera, raster, recording, or plan by accident.
    int expected_camera_id = -1;
    std::string expected_camera_serial;
    SpatialRoiRaster expected_native_raster;
    std::string expected_spatial_roi_plan_sha256;

    // Exact descriptors in the plan's resolved ROI order.
    std::vector<SpatialRoiPlanRoiBinding> expected_roi_descriptors;

    // One exact output capacity per admitted ROI, in the only accepted work
    // item order.  Each work item's encoded_raster must equal the capacity at
    // the same index.  One allocation of width*height bytes is made for every
    // capacity in every pool slot.
    std::vector<SpatialRoiOutputCapacity> output_capacities;

private:
    // Only spatial_roi_batch_limits_from_verified_plan() can mint this
    // capability. The public limits remain available to pure validation tests,
    // but arbitrary caller-provided limits cannot construct a CUDA producer.
    std::shared_ptr<const detail::SpatialRoiVerifiedPlanCapability>
        verified_plan_capability_;
    friend bool spatial_roi_batch_limits_from_verified_plan(
        const nlohmann::json& verified_plan,
        const std::string& camera_serial,
        int gpu_id,
        SpatialRoiBatchLimits* limits_out,
        std::string* error_out);
    friend class SpatialRoiBatchProducer;
};

// Verify a generated plan and materialize the exact limits for one camera.
// The camera serial and GPU are the only runtime selectors; all identities,
// geometry, pool depth, and admission bytes come from the verified plan.
bool spatial_roi_batch_limits_from_verified_plan(
    const nlohmann::json& verified_plan,
    const std::string& camera_serial,
    int gpu_id,
    SpatialRoiBatchLimits* limits_out,
    std::string* error_out = nullptr);

// Descriptive alias for callers that prefer the builder naming convention.
inline bool build_spatial_roi_batch_limits(
    const nlohmann::json& verified_plan,
    const std::string& camera_serial,
    int gpu_id,
    SpatialRoiBatchLimits* limits_out,
    std::string* error_out = nullptr)
{
    return spatial_roi_batch_limits_from_verified_plan(
        verified_plan, camera_serial, gpu_id, limits_out, error_out);
}

inline bool make_spatial_roi_batch_limits(
    const nlohmann::json& verified_plan,
    const std::string& camera_serial,
    int gpu_id,
    SpatialRoiBatchLimits* limits_out,
    std::string* error_out = nullptr)
{
    return spatial_roi_batch_limits_from_verified_plan(
        verified_plan, camera_serial, gpu_id, limits_out, error_out);
}

enum class SpatialRoiBatchStatus {
    kAccepted,
    kInvalidArgument,
    kPoolExhausted,
    // A CUDA operation needed to admit/reclaim a batch failed. This is an
    // internal fault and is intentionally distinct from kStopped.
    kCudaError,
    // Async work may have been queued, but cleanup synchronization failed.
    // The producer retains the entire pool state; the caller must not release
    // the source based on this result because no completion fence is available.
    kSourceQuarantined,
    // StopAccepting was linearized before this submission acquired admission.
    kStopped,
};

const char* spatial_roi_batch_status_name(SpatialRoiBatchStatus status) noexcept;

// Pure validation used both before CUDA admission and by host-side tests.
// Equality between content_rect and encoded_content_rect dimensions is the
// no-resize guarantee. Placement within encoded_raster is padding only.
bool validate_spatial_roi_batch(
    const SpatialRoiSourceView& source,
    const std::vector<SpatialRoiWorkItem>& work_items,
    const SpatialRoiBatchLimits& limits,
    std::string* error_out = nullptr);

struct SpatialRoiOutputView {
    SpatialRoiWorkItem work_item;
    unsigned char* device_data = nullptr;
    std::size_t pitch_bytes = 0;
};

namespace detail {
class SpatialRoiBatchPoolState;
}

// Move-only ownership of one preallocated batch slot. Every output pointer is
// valid until Reset() or destruction. The caller must retain the result until
// all output consumers are finished. For an accepted result,
// completion_event() is recorded once after every zero-fill and native-pixel
// copy in the batch; it is therefore also the fence after which the one source
// WORKER_ENTRY lease may be released. If an asynchronous CUDA operation fails
// after work has been queued, TryProduce synchronously drains its producer
// stream before returning. If that drain fails, the result is
// kSourceQuarantined and the source must not be released: the producer keeps
// the entire CUDA pool (including its source lease) alive for the process
// lifetime because no completion fence can prove that queued work finished.
class SpatialRoiBatchResult {
public:
    SpatialRoiBatchResult() = default;
    ~SpatialRoiBatchResult();

    SpatialRoiBatchResult(const SpatialRoiBatchResult&) = delete;
    SpatialRoiBatchResult& operator=(const SpatialRoiBatchResult&) = delete;
    SpatialRoiBatchResult(SpatialRoiBatchResult&& other) noexcept;
    SpatialRoiBatchResult& operator=(SpatialRoiBatchResult&& other) noexcept;

    SpatialRoiBatchStatus status() const noexcept { return status_; }
    bool accepted() const noexcept { return status_ == SpatialRoiBatchStatus::kAccepted; }
    // False for accepted work because the caller must first wait on
    // completion_event, and permanently false for kSourceQuarantined. For all
    // other results the producer has either not queued source work or has
    // synchronously drained its stream before returning.
    bool source_release_safe() const noexcept { return source_release_safe_; }
    bool source_quarantined() const noexcept
    {
        return status_ == SpatialRoiBatchStatus::kSourceQuarantined;
    }
    const std::string& error() const noexcept { return error_; }
    std::uint64_t batch_sequence() const noexcept { return batch_sequence_; }
    cudaEvent_t completion_event() const noexcept { return completion_event_; }
    const std::vector<SpatialRoiOutputView>& outputs() const noexcept { return outputs_; }

    // Relinquish the output slot after every downstream consumer is complete.
    // Recycling is asynchronous: a slot whose extraction event is still in
    // flight remains pending and is not offered to a later batch.
    void Reset() noexcept;

private:
    friend class SpatialRoiBatchProducer;

    SpatialRoiBatchStatus status_ = SpatialRoiBatchStatus::kStopped;
    bool source_release_safe_ = true;
    std::string error_;
    std::uint64_t batch_sequence_ = 0;
    cudaEvent_t completion_event_ = nullptr;
    std::vector<SpatialRoiOutputView> outputs_;
    // Retained through Reset as an explicit result-level lease. The pool slot
    // retains its own copy until the completion event is observed, so a caller
    // cannot accidentally release the source before its asynchronous read is
    // complete by calling Reset early.
    std::shared_ptr<void> source_lease_;
    std::shared_ptr<detail::SpatialRoiBatchPoolState> state_;
    std::size_t slot_index_ = 0;
    std::uint64_t slot_generation_ = 0;
};

// One producer is intended per camera. It owns one nonblocking CUDA stream and
// a fixed-depth pool. All device allocations and CUDA event creation occur at
// construction; TryProduce performs no CUDA allocation and never waits for a
// pool slot.
class SpatialRoiBatchProducer {
public:
    explicit SpatialRoiBatchProducer(const SpatialRoiBatchLimits& limits);
    ~SpatialRoiBatchProducer();

    SpatialRoiBatchProducer(const SpatialRoiBatchProducer&) = delete;
    SpatialRoiBatchProducer& operator=(const SpatialRoiBatchProducer&) = delete;
    SpatialRoiBatchProducer(SpatialRoiBatchProducer&&) = delete;
    SpatialRoiBatchProducer& operator=(SpatialRoiBatchProducer&&) = delete;

    SpatialRoiBatchResult TryProduce(
        const SpatialRoiSourceView& source,
        const std::vector<SpatialRoiWorkItem>& work_items);

    // Linearization point for shutdown. StopAccepting waits for any submission
    // that has entered admission, including its CUDA enqueue sequence, and no
    // new submission can enqueue after this call returns.
    void StopAccepting() noexcept;
    // Fail-stop after a downstream CUDA completion check can no longer prove
    // source/output safety. The pool and outstanding source leases are then
    // retained for the process lifetime rather than reused or released.
    void Quarantine() noexcept;
    std::size_t slot_capacity() const noexcept;
    std::size_t available_slot_count() const noexcept;
    std::size_t pending_slot_count() const noexcept;
    const SpatialRoiBatchLimits& limits() const noexcept { return limits_; }

private:
    SpatialRoiBatchLimits limits_;
    std::shared_ptr<detail::SpatialRoiBatchPoolState> state_;
};

}  // namespace orange::spatial_roi
