#pragma once

#include "spatial_roi_ipc_protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace orange::spatial_roi::ipc {

inline constexpr std::uint64_t kSpatialRoiRecorderCudaDetachMaxPoolBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kSpatialRoiRecorderCudaDetachMaxTimeoutMs =
    5U * 60U * 1000U;

// Immutable fixed-region geometry copied from the independently verified
// recorder contract. The plan digest in expected_stream names the authority;
// these repeated fields prevent a self-consistent but plan-inconsistent FRAME
// from selecting different source pixels under that identity.
struct SpatialRoiRecorderCudaDetachGeometry {
    SpatialRoiFrameRaster native_raster;
    SpatialRoiFrameRect content_rect;
    SpatialRoiFrameRaster encoded_raster;
    SpatialRoiFrameRect encoded_content_rect;
    SpatialRoiFramePadding padding;
    std::string routing_policy;
};

// The recorder owns one fixed-size pool for one declared ROI stream.  The
// dimensions are taken from the verified plan by the caller; a FRAME with a
// different encoded raster is rejected before any CUDA IPC handle is opened.
struct SpatialRoiRecorderCudaDetachConfig {
    // The pool is bound to one complete logical stream.  Dimensions and GPU
    // checks alone are insufficient: this identity prevents a valid frame
    // from another recording, generation, camera, or ROI from opening its
    // producer handles in this recorder.
    SpatialRoiIpcStreamIdentity expected_stream;
    int recorder_gpu_id = -1;
    int expected_source_gpu_id = -1;
    int expected_assigned_shard_id = -1;
    SpatialRoiRecorderCudaDetachGeometry expected_geometry;
    std::size_t slot_count = 0;
    // This independently authenticated budget must come from recorder
    // admission. The pool rejects both a zero/oversized budget and any plan
    // whose recorder-owned Mono8+NV12 slots exceed it.
    std::uint64_t max_pool_bytes = 0;
    // One absolute deadline covers the imported source event and all queued
    // detach/transform work. A timeout quarantines the pool and is terminal
    // for this recorder process.
    std::uint32_t operation_timeout_ms = 0;
};

enum class SpatialRoiRecorderDetachStatus {
    kDetached,
    kInvalidArgument,
    kWrongDevice,
    kBusy,
    kPoolExhausted,
    kCudaError,
    // A CUDA operation or import cleanup failed after the recorder could no
    // longer prove source completion.  The pool is intentionally quarantined
    // for process lifetime and no source RELEASE should be inferred from it.
    kSourceQuarantined,
    kStopped,
};

const char* spatial_roi_recorder_detach_status_name(
    SpatialRoiRecorderDetachStatus status) noexcept;

struct SpatialRoiRecorderCudaDetachCounters {
    std::uint64_t detach_attempted = 0;
    std::uint64_t detached = 0;
    std::uint64_t invalid_arguments = 0;
    std::uint64_t wrong_device = 0;
    std::uint64_t busy = 0;
    std::uint64_t pool_exhausted = 0;
    std::uint64_t stopped = 0;
    std::uint64_t cuda_errors = 0;
    std::uint64_t memory_imports = 0;
    std::uint64_t event_imports = 0;
    std::uint64_t source_waits = 0;
    std::uint64_t mono8_bytes_copied = 0;
    std::uint64_t nv12_conversions = 0;
    std::uint64_t slot_releases = 0;
    std::uint64_t source_quarantines = 0;
    std::uint64_t cleanup_failures = 0;
    std::uint64_t generation_exhausted = 0;
};

namespace detail {
class SpatialRoiRecorderCudaDetachState;
}

// A successful result owns one recorder-side slot.  Its destructor returns
// that slot to the bounded pool; callers may also release it explicitly after
// the encoder has finished using both device views.  No source CUDA IPC
// handle is retained by this object: imports are closed before kDetached is
// returned, after the copy stream has completed.
class SpatialRoiRecorderDetachedFrame final {
public:
    SpatialRoiRecorderDetachedFrame() = default;
    ~SpatialRoiRecorderDetachedFrame();

    SpatialRoiRecorderDetachedFrame(const SpatialRoiRecorderDetachedFrame&) =
        delete;
    SpatialRoiRecorderDetachedFrame& operator=(
        const SpatialRoiRecorderDetachedFrame&) = delete;
    SpatialRoiRecorderDetachedFrame(SpatialRoiRecorderDetachedFrame&& other)
        noexcept;
    SpatialRoiRecorderDetachedFrame& operator=(
        SpatialRoiRecorderDetachedFrame&& other) noexcept;

    bool valid() const noexcept { return state_ != nullptr; }
    bool released() const noexcept { return state_ == nullptr; }

    const SpatialRoiIpcCorrelation& correlation() const noexcept
    {
        return correlation_;
    }
    const unsigned char* device_mono8() const noexcept
    {
        return device_mono8_;
    }
    const unsigned char* device_nv12() const noexcept
    {
        return device_nv12_;
    }
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::size_t mono8_bytes() const noexcept { return mono8_bytes_; }
    std::size_t nv12_bytes() const noexcept { return nv12_bytes_; }
    std::size_t row_pitch_bytes() const noexcept
    {
        return static_cast<std::size_t>(width_);
    }

    // Idempotent and noexcept.  A generation mismatch is ignored and counted
    // by the state rather than allowing a stale result to free a reused slot.
    void Release() noexcept;

private:
    friend class SpatialRoiRecorderCudaDetachPool;

    SpatialRoiRecorderDetachedFrame(
        std::shared_ptr<detail::SpatialRoiRecorderCudaDetachState> state,
        std::size_t slot_index,
        std::uint64_t slot_generation,
        SpatialRoiIpcCorrelation correlation,
        unsigned char* device_mono8,
        unsigned char* device_nv12,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t mono8_bytes,
        std::size_t nv12_bytes) noexcept;

    std::shared_ptr<detail::SpatialRoiRecorderCudaDetachState> state_;
    std::size_t slot_index_ = 0;
    std::uint64_t slot_generation_ = 0;
    SpatialRoiIpcCorrelation correlation_;
    unsigned char* device_mono8_ = nullptr;
    unsigned char* device_nv12_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t mono8_bytes_ = 0;
    std::size_t nv12_bytes_ = 0;
};

struct SpatialRoiRecorderCudaDetachResult {
    SpatialRoiRecorderDetachStatus status =
        SpatialRoiRecorderDetachStatus::kCudaError;
    std::string error;
    SpatialRoiRecorderDetachedFrame frame;
    bool source_safe = true;

    bool detached() const noexcept
    {
        return status == SpatialRoiRecorderDetachStatus::kDetached &&
               frame.valid();
    }

    // True means the caller may complete the rejected ACK + exact RELEASE
    // path without relying on an unresolved CUDA import. It is false only for
    // a quarantined operation whose source completion could not be proven.
    bool source_release_safe() const noexcept
    {
        return source_safe &&
               status != SpatialRoiRecorderDetachStatus::kSourceQuarantined;
    }
};

// Recorder-side synchronous detach foundation.  It performs no dynamic CUDA
// allocation on the hot path and never waits for a free slot.  A caller that
// retains every returned frame eventually receives kPoolExhausted until one
// frame is released.
class SpatialRoiRecorderCudaDetachPool final {
public:
    explicit SpatialRoiRecorderCudaDetachPool(
        SpatialRoiRecorderCudaDetachConfig config) noexcept;
    ~SpatialRoiRecorderCudaDetachPool();

    SpatialRoiRecorderCudaDetachPool(const SpatialRoiRecorderCudaDetachPool&) =
        delete;
    SpatialRoiRecorderCudaDetachPool& operator=(
        const SpatialRoiRecorderCudaDetachPool&) = delete;
    SpatialRoiRecorderCudaDetachPool(SpatialRoiRecorderCudaDetachPool&&) =
        delete;
    SpatialRoiRecorderCudaDetachPool& operator=(
        SpatialRoiRecorderCudaDetachPool&&) = delete;

    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }
    const SpatialRoiRecorderCudaDetachConfig& config() const noexcept
    {
        return config_;
    }

    std::size_t slot_capacity() const noexcept;
    std::size_t available_slot_count() const noexcept;
    SpatialRoiRecorderCudaDetachCounters counters() const noexcept;

    // The input is the validated wire-level FRAME descriptor.  On success,
    // this function has synchronously waited for the producer completion event,
    // copied the full packed Mono8 raster, generated NV12 (Y exact, UV=128),
    // and closed both imported handles.  Only then is the result RELEASE-ready.
    // A failure after source import returns kSourceQuarantined and preserves
    // the imported resources in a process-lifetime quarantine.
    // One recorder thread owns a pool/stream at a time. A concurrent caller is
    // rejected immediately with kBusy; it can never enqueue into the shared
    // CUDA stream. A future multi-threaded recorder should fan work into one
    // owner queue rather than retrying this call.
    //
    // kSourceQuarantined is terminal for the recorder process. The supervisor
    // must stop sending work, prove this process has exited, and only then let
    // the producer release an unresolved exported allocation. There is no
    // in-process quarantine recovery API by design.
    //
    // This is not a generic untrusted CUDA-handle importer. The caller must
    // first complete the dedicated single-producer HELLO/handoff state
    // machine, which binds one atomic FRAME (correlation + memory/event
    // handles) to the exact spawned peer and enforces dense frame sequencing.
    // This pool deliberately does not duplicate transport authentication or
    // replay detection.
    SpatialRoiRecorderCudaDetachResult TryDetach(
        const SpatialRoiIpcFrame& frame) noexcept;

    // Stop accepting new frames while allowing already returned detached
    // frames to retain the shared pool state until their Release/destruction.
    void Stop() noexcept;

private:
    SpatialRoiRecorderCudaDetachResult TryDetachImpl(
        const SpatialRoiIpcFrame& frame,
        bool* source_import_unresolved);

    std::shared_ptr<detail::SpatialRoiRecorderCudaDetachState> state_;
    SpatialRoiRecorderCudaDetachConfig config_;
    std::atomic_flag operation_in_progress_ = ATOMIC_FLAG_INIT;
    bool valid_ = false;
    std::string error_;
};

}  // namespace orange::spatial_roi::ipc
