#pragma once

#include "camera.h"
#include "gui/startup_timing.h"
#include "json.hpp"
#include "recording_validation.h"
#include "video_capture.h"

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct AppStorageConfig;
struct CameraEmergent;
struct CameraParams;
struct CameraEachSelect;
struct CameraControl;
struct CameraResources;
struct COpenGLDisplay;
struct CropAndEncodeWorker;
struct CropPreviewWorker;
struct CropProducerWorker;
struct EncoderConfig;
struct FrameIPCManager;
struct GL_Texture;
struct INDIGOSignalBuilder;
struct ImageWriterWorker;
struct PoseWorker;
struct PTPParams;
struct SpatialSnapshotWorker;
struct YoloWorker;

namespace orange::session {
struct RecordingSessionState;
}

namespace orange::gui {

enum class GuiCameraStartupPhase {
    kIdle,
    kOpeningCameras,
    kPreparingStreamResources,
    kWaitingForGuiTextures,
    kConstructingStreamRuntime,
    kWaitingForFirstFrames,
    kCanceling,
    kFailed,
};

const char* gui_camera_startup_phase_name(GuiCameraStartupPhase phase) noexcept;

struct GuiSelectedCameraDevice {
    GigEVisionDeviceInfo device{};
    int device_index = -1;
};

struct GuiCameraOpenRequest {
    std::vector<GuiSelectedCameraDevice> selected_devices;
    std::vector<std::string> camera_config_files;
    int stream_downsample = 1;
    int display_preview_max_fps = 0;
    int fallback_crop_size_px = 0;
    int fallback_crop_preview_max_fps = 0;
    std::filesystem::path timing_artifact_directory;
    nlohmann::json timing_context = nlohmann::json::object();
    uint64_t request_started_ns = 0;
    uint64_t selection_finished_ns = 0;
    GuiStartupTimingRecorder* timing = nullptr;
};

struct GuiCameraOpenProduct {
    std::unique_ptr<CameraParams[]> camera_params;
    std::unique_ptr<CameraEachSelect[]> camera_selection;
    std::unique_ptr<CameraEmergent[]> cameras;
    int camera_count = 0;
    bool ptp_stream_sync = false;
    bool mixed_ptp_modes = false;
    int crop_size_px = 0;
    bool mixed_crop_sizes = false;
    int crop_preview_max_fps = 0;
    bool mixed_crop_preview_max_fps = false;
};

// Existing GUI runtime storage remains owned by main, but all slow startup
// construction is performed by this module. Pointer-to-pointer fields are
// populated atomically on the GUI thread only after the background product is
// complete, so the UI never observes a partially constructed pipeline.
struct GuiStreamStartupBindings {
    CameraEmergent* cameras = nullptr;
    CameraParams* camera_params = nullptr;
    CameraEachSelect* camera_selection = nullptr;
    int camera_count = 0;
    CameraControl* camera_control = nullptr;
    PTPParams* ptp_params = nullptr;
    int evt_buffer_size = 0;
    bool ptp_stream_sync = false;
    int crop_size_px = 0;
    int display_cuda_device_id = 0;
    bool show_crop_preview_windows = false;
    const std::string* yolo_model = nullptr;
    EncoderConfig* encoder_config = nullptr;
    std::string input_folder;
    INDIGOSignalBuilder* indigo_signal_builder = nullptr;
    ImageWriterWorker* image_writer = nullptr;
    orange::session::RecordingSessionState* recording_session = nullptr;
    const AppStorageConfig* app_storage_config = nullptr;
    GuiStartupTimingRecorder* timing = nullptr;
    std::filesystem::path timing_artifact_directory;

    std::vector<CameraResources>* camera_resources = nullptr;
    std::vector<std::unique_ptr<FrameIPCManager>>* frame_ipc_managers = nullptr;
    std::vector<std::string>* frame_ipc_init_errors = nullptr;
    COpenGLDisplay*** display_workers = nullptr;
    CropProducerWorker*** crop_producer_workers = nullptr;
    CropAndEncodeWorker*** crop_encode_workers = nullptr;
    CropPreviewWorker*** crop_preview_workers = nullptr;
    SpatialSnapshotWorker*** spatial_snapshot_workers = nullptr;
    PoseWorker*** pose_workers = nullptr;
    GL_Texture** display_textures = nullptr;
    GL_Texture** crop_textures = nullptr;
    std::vector<YoloWorker*>* yolo_workers = nullptr;
    std::vector<std::thread>* acquisition_threads = nullptr;
};

enum class GuiCameraStartupEventKind {
    kNone,
    kCameraOpenSucceeded,
    kCameraOpenFailed,
    kStreamActivated,
    kStreamReady,
    kStreamFailed,
    kCanceled,
};

struct GuiCameraStartupEvent {
    GuiCameraStartupEventKind kind = GuiCameraStartupEventKind::kNone;
    std::string message;
    std::vector<std::string> errors;
    std::unique_ptr<GuiCameraOpenProduct> camera_open_product;
};

struct GuiCameraStartupStatus {
    GuiCameraStartupPhase phase = GuiCameraStartupPhase::kIdle;
    bool busy = false;
    bool stream_runtime_installed = false;
    std::string operation;
    std::string message;
};

class GuiCameraStartupController {
public:
    GuiCameraStartupController();
    ~GuiCameraStartupController();

    GuiCameraStartupController(const GuiCameraStartupController&) = delete;
    GuiCameraStartupController& operator=(const GuiCameraStartupController&) = delete;

    bool StartCameraOpen(GuiCameraOpenRequest request, std::string* error_out = nullptr);
    bool StartStream(GuiStreamStartupBindings bindings, std::string* error_out = nullptr);
    GuiCameraStartupEvent PollGuiThread();
    void NotifyCameraOpenInstalled() noexcept;
    void RequestCancel(std::string reason = "operator_requested") noexcept;
    void NotifyInstalledStreamStopped() noexcept;
    void ShutdownGuiThread() noexcept;

    GuiCameraStartupStatus status() const;
    bool busy() const;
    bool stream_runtime_installed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

RecordingPreflightResult run_gui_recording_preflight(
    const CameraParams* camera_params,
    const CameraEachSelect* camera_selection,
    int camera_count,
    const std::string& selected_yolo_model,
    int crop_size_px);

int resolve_gui_crop_size_from_camera_configs(
    const CameraParams* camera_params,
    int camera_count,
    int fallback_crop_size,
    bool* mixed_values_out);
int resolve_gui_crop_preview_max_fps_from_camera_configs(
    const CameraParams* camera_params,
    int camera_count,
    int fallback_preview_max_fps,
    bool* mixed_values_out);
void apply_gui_crop_size_to_camera_configs(
    CameraParams* camera_params,
    int camera_count,
    int crop_size_px);
void apply_gui_crop_preview_max_fps_to_camera_configs(
    CameraParams* camera_params,
    int camera_count,
    int preview_max_fps);

}  // namespace orange::gui
