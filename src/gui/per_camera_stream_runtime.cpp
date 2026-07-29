#include "gui/per_camera_stream_runtime.h"

#include "crop_and_encode_worker.h"
#include "crop_preview_worker.h"
#include "crop_producer_worker.h"
#include "frame_ipc_manager.h"
#include "opengldisplay.h"
#include "pose_worker.h"
#include "spatial_snapshot_worker.h"
#include "yolo_worker.h"

#include <cuda_runtime.h>

#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orange::gui {
namespace {

template <typename Worker>
void stop_worker(Worker* worker) noexcept
{
    if (!worker) return;
    try {
        worker->StopThread();
    } catch (...) {
    }
}

void log_cleanup_error(
    const std::string& serial,
    const char* operation,
    const std::string& error) noexcept
{
    std::cerr << "[GUI][stream_runtime] " << operation
              << " failed camera="
              << (serial.empty() ? "unknown" : serial)
              << " error=" << error << std::endl;
}

}  // namespace

static_assert(!std::is_copy_constructible_v<PerCameraStreamRuntime>);
static_assert(!std::is_copy_assignable_v<PerCameraStreamRuntime>);
static_assert(std::is_nothrow_destructible_v<PerCameraStreamRuntime>);
static_assert(std::is_nothrow_move_constructible_v<PerCameraStreamRuntime>);
static_assert(std::is_nothrow_move_assignable_v<PerCameraStreamRuntime>);

PerCameraStreamRuntime::~PerCameraStreamRuntime() noexcept
{
    Reset();
}

PerCameraStreamRuntime::PerCameraStreamRuntime(
    PerCameraStreamRuntime&& other) noexcept
{
    MoveFrom(std::move(other));
}

PerCameraStreamRuntime& PerCameraStreamRuntime::operator=(
    PerCameraStreamRuntime&& other) noexcept
{
    if (this != &other) {
        Reset();
        MoveFrom(std::move(other));
    }
    return *this;
}

void PerCameraStreamRuntime::Bind(
    CameraEmergent* camera,
    CameraParams* params)
{
    if (!camera || !params) {
        throw std::invalid_argument(
            "PerCameraStreamRuntime requires camera and parameter bindings");
    }
    if (!empty()) {
        throw std::logic_error(
            "PerCameraStreamRuntime cannot be rebound while it owns resources");
    }
    camera_ = camera;
    params_ = params;
    gpu_id_ = params->gpu_id;
    camera_serial_ = params->camera_serial;
    ClearBorrowedFrameView();
}

bool PerCameraStreamRuntime::empty() const noexcept
{
    return !camera_resources_initialized_ && !stream_opened_ &&
        !frame_array_ && allocated_frame_buffer_count_ == 0 &&
        !frame_ipc_manager_ && !display_worker_ && !crop_producer_worker_ &&
        !crop_encode_worker_ && !crop_preview_worker_ &&
        !spatial_snapshot_worker_ && !pose_worker_ && !yolo_worker_;
}

void PerCameraStreamRuntime::InitializeCameraResources(
    const std::size_t maximum_frame_bytes,
    const bool enable_yolo_events,
    const int acquire_work_entries)
{
    if (!bound()) {
        throw std::logic_error(
            "camera resources cannot initialize before runtime binding");
    }
    if (camera_resources_initialized_) {
        throw std::logic_error("camera resources are already initialized");
    }
    // Publish ownership before entering a throwing initializer so Reset() can
    // clean any CUDA allocations completed before the exception.
    camera_resources_initialized_ = true;
    camera_resources_.initialize(
        gpu_id_,
        maximum_frame_bytes,
        enable_yolo_events,
        acquire_work_entries);
}

void PerCameraStreamRuntime::SetFrameIpcManager(
    std::unique_ptr<FrameIPCManager> manager) noexcept
{
    frame_ipc_manager_ = std::move(manager);
    frame_ipc_init_error_.clear();
}

FrameIPCManager* PerCameraStreamRuntime::frame_ipc_manager() const noexcept
{
    return frame_ipc_manager_.get();
}

void PerCameraStreamRuntime::SetFrameIpcInitError(std::string error)
{
    frame_ipc_init_error_ = std::move(error);
}

void PerCameraStreamRuntime::OpenCameraStream(const char* context)
{
    if (!bound()) {
        throw std::logic_error("camera stream cannot open before runtime binding");
    }
    if (stream_opened_) {
        throw std::logic_error("camera stream is already open");
    }
    camera_open_stream(&camera_->camera, params_, context);
    stream_opened_ = true;
}

void PerCameraStreamRuntime::AllocateFrameArray(const int frame_count)
{
    if (!bound() || !stream_opened_) {
        throw std::logic_error(
            "frame array requires a bound runtime with an open stream");
    }
    if (frame_count <= 0) {
        throw std::invalid_argument("frame array count must be positive");
    }
    if (frame_array_) {
        throw std::logic_error("frame array is already allocated");
    }
    frame_array_ =
        std::make_unique<Emergent::CEmergentFrame[]>(frame_count);
    allocated_frame_buffer_count_ = 0;
    camera_->evt_frame = frame_array_.get();
    camera_->evt_frame_count = 0;
}

void PerCameraStreamRuntime::AllocateAndQueueFrameBuffer(
    const int frame_index)
{
    if (!bound() || !frame_array_) {
        throw std::logic_error(
            "frame buffer requires a bound runtime with an allocated array");
    }
    if (frame_index != allocated_frame_buffer_count_) {
        throw std::logic_error(
            "frame buffers must be allocated sequentially");
    }
    Emergent::CEmergentFrame* frame = &frame_array_[frame_index];
    set_frame_buffer(frame, params_);
    check_camera_errors(
        EVT_AllocateFrameBuffer(
            &camera_->camera,
            frame,
            EVT_FRAME_BUFFER_ZERO_COPY),
        camera_serial_.c_str());
    // Record ownership before queueing: a queue failure still leaves an SDK
    // allocation that Reset() must release.
    ++allocated_frame_buffer_count_;
    camera_->evt_frame_count = allocated_frame_buffer_count_;
    check_camera_errors(
        EVT_CameraQueueFrame(&camera_->camera, frame),
        camera_serial_.c_str());
}

void PerCameraStreamRuntime::SetDisplayWorker(
    std::unique_ptr<COpenGLDisplay> worker) noexcept
{
    display_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetCropProducerWorker(
    std::unique_ptr<CropProducerWorker> worker) noexcept
{
    crop_producer_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetCropEncodeWorker(
    std::unique_ptr<CropAndEncodeWorker> worker) noexcept
{
    crop_encode_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetCropPreviewWorker(
    std::unique_ptr<CropPreviewWorker> worker) noexcept
{
    crop_preview_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetSpatialSnapshotWorker(
    std::unique_ptr<SpatialSnapshotWorker> worker) noexcept
{
    spatial_snapshot_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetPoseWorker(
    std::unique_ptr<PoseWorker> worker) noexcept
{
    pose_worker_ = std::move(worker);
}

void PerCameraStreamRuntime::SetYoloWorker(
    std::unique_ptr<YoloWorker> worker) noexcept
{
    yolo_worker_ = std::move(worker);
}

COpenGLDisplay* PerCameraStreamRuntime::display_worker() const noexcept
{
    return display_worker_.get();
}

CropProducerWorker*
PerCameraStreamRuntime::crop_producer_worker() const noexcept
{
    return crop_producer_worker_.get();
}

CropAndEncodeWorker*
PerCameraStreamRuntime::crop_encode_worker() const noexcept
{
    return crop_encode_worker_.get();
}

CropPreviewWorker*
PerCameraStreamRuntime::crop_preview_worker() const noexcept
{
    return crop_preview_worker_.get();
}

SpatialSnapshotWorker*
PerCameraStreamRuntime::spatial_snapshot_worker() const noexcept
{
    return spatial_snapshot_worker_.get();
}

PoseWorker* PerCameraStreamRuntime::pose_worker() const noexcept
{
    return pose_worker_.get();
}

YoloWorker* PerCameraStreamRuntime::yolo_worker() const noexcept
{
    return yolo_worker_.get();
}

void PerCameraStreamRuntime::StopWorkers() noexcept
{
    stop_worker(yolo_worker_.get());
    stop_worker(display_worker_.get());
    stop_worker(crop_producer_worker_.get());
    stop_worker(crop_preview_worker_.get());
    stop_worker(crop_encode_worker_.get());
    stop_worker(pose_worker_.get());
    stop_worker(spatial_snapshot_worker_.get());
}

void PerCameraStreamRuntime::FinalizeAndDestroyWorkers() noexcept
{
    // Endpoints first, then upstream producers, matching the established GUI
    // drain order. StopWorkers() is idempotent and protects destructor-only
    // rollback paths where a caller did not explicitly stop first.
    StopWorkers();
    display_worker_.reset();

    if (crop_encode_worker_) {
        try {
            crop_encode_worker_->finalize_recording();
        } catch (const std::exception& error) {
            log_cleanup_error(
                camera_serial_, "crop recording finalization", error.what());
        } catch (...) {
            log_cleanup_error(
                camera_serial_,
                "crop recording finalization",
                "non-standard exception");
        }
        crop_encode_worker_.reset();
    }
    crop_preview_worker_.reset();

    if (pose_worker_) {
        try {
            pose_worker_->CloseRecording();
        } catch (...) {
            log_cleanup_error(
                camera_serial_, "pose recording close", "exception");
        }
        pose_worker_.reset();
    }
    spatial_snapshot_worker_.reset();

    if (crop_producer_worker_) {
        try {
            crop_producer_worker_->CloseRecording();
        } catch (...) {
            log_cleanup_error(
                camera_serial_, "crop producer close", "exception");
        }
        crop_producer_worker_.reset();
    }
    yolo_worker_.reset();
}

void PerCameraStreamRuntime::ReleaseStreamAndIpc() noexcept
{
    frame_ipc_manager_.reset();
    frame_ipc_init_error_.clear();

    if (frame_array_ || stream_opened_) {
        SelectCameraDeviceForCleanup();
    }
    if (camera_ && frame_array_) {
        for (int frame_index = 0;
             frame_index < allocated_frame_buffer_count_;
             ++frame_index) {
            const EVT_ERROR error = EVT_ReleaseFrameBuffer(
                &camera_->camera,
                &frame_array_[frame_index]);
            if (error != EVT_SUCCESS) {
                log_cleanup_error(
                    camera_serial_,
                    "frame-buffer release",
                    "frame_index=" + std::to_string(frame_index) + " " +
                        get_evt_error_string(error));
            }
        }
    }
    allocated_frame_buffer_count_ = 0;
    ClearBorrowedFrameView();
    frame_array_.reset();

    if (camera_ && stream_opened_) {
        const EVT_ERROR error = EVT_CameraCloseStream(&camera_->camera);
        if (error != EVT_SUCCESS) {
            log_cleanup_error(
                camera_serial_,
                "stream close",
                get_evt_error_string(error));
        }
    }
    stream_opened_ = false;
}

void PerCameraStreamRuntime::ReleaseCameraResources() noexcept
{
    if (!camera_resources_initialized_) return;
    SelectCameraDeviceForCleanup();
    try {
        camera_resources_.cleanup();
    } catch (...) {
        log_cleanup_error(
            camera_serial_, "camera CUDA resource cleanup", "exception");
    }
    camera_resources_initialized_ = false;
}

void PerCameraStreamRuntime::Reset() noexcept
{
    FinalizeAndDestroyWorkers();
    ReleaseStreamAndIpc();
    ReleaseCameraResources();
    camera_ = nullptr;
    params_ = nullptr;
    gpu_id_ = -1;
    camera_serial_.clear();
}

void PerCameraStreamRuntime::MoveFrom(
    PerCameraStreamRuntime&& other) noexcept
{
    camera_ = other.camera_;
    params_ = other.params_;
    gpu_id_ = other.gpu_id_;
    camera_serial_ = std::move(other.camera_serial_);
    camera_resources_initialized_ = other.camera_resources_initialized_;
    stream_opened_ = other.stream_opened_;
    allocated_frame_buffer_count_ = other.allocated_frame_buffer_count_;
    frame_array_ = std::move(other.frame_array_);
    camera_resources_ = std::move(other.camera_resources_);
    frame_ipc_manager_ = std::move(other.frame_ipc_manager_);
    frame_ipc_init_error_ = std::move(other.frame_ipc_init_error_);
    display_worker_ = std::move(other.display_worker_);
    crop_producer_worker_ = std::move(other.crop_producer_worker_);
    crop_encode_worker_ = std::move(other.crop_encode_worker_);
    crop_preview_worker_ = std::move(other.crop_preview_worker_);
    spatial_snapshot_worker_ = std::move(other.spatial_snapshot_worker_);
    pose_worker_ = std::move(other.pose_worker_);
    yolo_worker_ = std::move(other.yolo_worker_);

    if (camera_) {
        camera_->evt_frame = frame_array_.get();
        camera_->evt_frame_count = allocated_frame_buffer_count_;
    }

    other.camera_ = nullptr;
    other.params_ = nullptr;
    other.gpu_id_ = -1;
    other.camera_resources_initialized_ = false;
    other.stream_opened_ = false;
    other.allocated_frame_buffer_count_ = 0;
}

void PerCameraStreamRuntime::SelectCameraDeviceForCleanup() const noexcept
{
    if (gpu_id_ < 0) return;
    const cudaError_t status = cudaSetDevice(gpu_id_);
    if (status != cudaSuccess) {
        log_cleanup_error(
            camera_serial_,
            "CUDA device selection",
            cudaGetErrorString(status));
    }
}

void PerCameraStreamRuntime::ClearBorrowedFrameView() noexcept
{
    if (!camera_) return;
    camera_->evt_frame = nullptr;
    camera_->evt_frame_count = 0;
}

}  // namespace orange::gui
