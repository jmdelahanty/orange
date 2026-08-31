#pragma once

#include "spatial_roi_ipc_protocol.h"
#include "spatial_roi_recording_runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace orange::spatial_roi::ipc {

// A successful export keeps the batch envelope alive.  The envelope owns the
// producer result and its source lease; callers must retain this object until
// the recorder has completed its CUDA-IPC use and sent RELEASE.  The exporter
// itself does not send a message and does not claim recorder queue capacity.
struct SpatialRoiIpcExport {
    SpatialRoiIpcFrame frame;
    std::shared_ptr<const SpatialRoiBatchEnvelope> envelope;
};

// Converts one completed producer lane into the CUDA-IPC FRAME payload.  The
// verified plan is parsed and materialized at construction.  No caller can
// supply replacement ROI geometry or identity through this adapter.
class SpatialRoiIpcFrameExporter final {
public:
    // Construction is fail-closed and records invalid-plan errors in
    // valid()/error(); it does not construct a CUDA producer or allocate
    // device memory.  The only selectors are the verified plan, camera serial,
    // and source/producer GPU.
    SpatialRoiIpcFrameExporter(const nlohmann::json& verified_plan,
                               std::string camera_serial,
                               int producer_gpu_id) noexcept;

    SpatialRoiIpcFrameExporter(const SpatialRoiIpcFrameExporter&) = delete;
    SpatialRoiIpcFrameExporter& operator=(const SpatialRoiIpcFrameExporter&) =
        delete;
    SpatialRoiIpcFrameExporter(SpatialRoiIpcFrameExporter&&) = delete;
    SpatialRoiIpcFrameExporter& operator=(SpatialRoiIpcFrameExporter&&) =
        delete;

    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }
    int producer_gpu_id() const noexcept { return producer_gpu_id_; }
    const SpatialRoiBatchLimits& limits() const noexcept { return limits_; }

    // Build one FRAME from the immutable delivery assigned by a successfully
    // admitted runtime lane. In particular, the dense positive
    // roi_stream_frame_index cannot be substituted independently from the
    // envelope/lane pair. This function never increments or otherwise
    // reserves a stream counter. A failed build has no queue/counter side
    // effect.
    //
    // The output retains envelope, so the CUDA allocation and completion event
    // remain valid until the recorder's eventual RELEASE path drops it.  The
    // function is noexcept: malformed state, CUDA failures, and allocation
    // failures are returned as false with a diagnostic when possible.
    bool Build(
        const SpatialRoiLaneDelivery& delivery,
        int assigned_recorder_gpu_id,
        int assigned_shard_id,
        SpatialRoiIpcExport* export_out,
        std::string* error_out = nullptr) const noexcept;

private:
    SpatialRoiBatchLimits limits_;
    std::string camera_serial_;
    int producer_gpu_id_ = -1;
    bool valid_ = false;
    std::string error_;
};

}  // namespace orange::spatial_roi::ipc
