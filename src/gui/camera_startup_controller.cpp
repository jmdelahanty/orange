#include "gui/camera_startup_controller.h"

#include "crop_and_encode_worker.h"
#include "crop_preview_worker.h"
#include "crop_producer_worker.h"
#include "frame_ipc_manager.h"
#include "acquire_frames.h"
#include "gui/async_startup_worker.h"
#include "gui/recording_snapshots.h"
#include "gui/texture_resources.h"
#include "image_writer_worker.h"
#include "network_base.h"
#include "opengldisplay.h"
#include "pose_worker.h"
#include "project.h"
#include "recording_config_state.h"
#include "recording_ingress.h"
#include "session/recording_session.h"
#include "spatial_snapshot_worker.h"
#include "yolo_worker.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace orange::gui {
namespace {

constexpr double kFirstFrameTimeoutSeconds = 45.0;

uint64_t now_ns() noexcept
{
    return GuiStartupTimingRecorder::NowNs();
}

std::vector<RecordingValidationCameraInput> build_validation_inputs(
    const CameraParams* camera_params,
    const CameraEachSelect* camera_selection,
    const int camera_count)
{
    std::vector<RecordingValidationCameraInput> inputs;
    if (!camera_params || !camera_selection || camera_count <= 0) {
        return inputs;
    }
    inputs.reserve(static_cast<std::size_t>(camera_count));
    for (int i = 0; i < camera_count; ++i) {
        RecordingValidationCameraInput input;
        input.camera_index = i;
        input.camera_serial = camera_params[i].camera_serial;
        input.record_enabled = camera_selection[i].record;
        input.source_gpu_id = camera_params[i].gpu_id;
        input.strategy = camera_params[i].recording.strategy;
        input.constraints = camera_params[i].recording.constraints;
        inputs.push_back(std::move(input));
    }
    return inputs;
}

std::string exception_message()
{
    try {
        throw;
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "non-standard exception";
    }
}

void close_open_product_cameras(GuiCameraOpenProduct* product) noexcept
{
    if (!product || !product->cameras || !product->camera_params) {
        return;
    }
    for (int i = product->camera_count - 1; i >= 0; --i) {
        try {
            close_camera(
                &product->cameras[i].camera,
                &product->camera_params[i]);
        } catch (const std::exception& error) {
            std::cerr << "[GUI][camera_startup] camera close rollback failed"
                      << " camera=" << product->camera_params[i].camera_serial
                      << " error=" << error.what() << std::endl;
        } catch (...) {
            std::cerr << "[GUI][camera_startup] camera close rollback failed"
                      << " camera=" << product->camera_params[i].camera_serial
                      << " error=non_std_exception" << std::endl;
        }
    }
}

struct StreamStartupProduct {
    explicit StreamStartupProduct(const int count)
        : camera_count(count),
          resource_initialized(static_cast<std::size_t>(count), false),
          display_texture_initialized(static_cast<std::size_t>(count), false),
          crop_texture_initialized(static_cast<std::size_t>(count), false),
          stream_opened(static_cast<std::size_t>(count), false),
          frame_buffers_allocated(static_cast<std::size_t>(count), false),
          camera_resources(static_cast<std::size_t>(count)),
          frame_ipc_managers(static_cast<std::size_t>(count)),
          frame_ipc_init_errors(static_cast<std::size_t>(count)),
          display_workers(static_cast<std::size_t>(count)),
          crop_producer_workers(static_cast<std::size_t>(count)),
          crop_encode_workers(static_cast<std::size_t>(count)),
          crop_preview_workers(static_cast<std::size_t>(count)),
          spatial_snapshot_workers(static_cast<std::size_t>(count)),
          pose_workers(static_cast<std::size_t>(count)),
          yolo_workers(static_cast<std::size_t>(count)),
          display_textures(std::make_unique<GL_Texture[]>(count)),
          crop_textures(std::make_unique<GL_Texture[]>(count))
    {
    }

    int camera_count = 0;
    std::vector<bool> resource_initialized;
    std::vector<bool> display_texture_initialized;
    std::vector<bool> crop_texture_initialized;
    std::vector<bool> stream_opened;
    std::vector<bool> frame_buffers_allocated;
    std::vector<CameraResources> camera_resources;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    std::vector<std::string> frame_ipc_init_errors;
    std::vector<std::unique_ptr<COpenGLDisplay>> display_workers;
    std::vector<std::unique_ptr<CropProducerWorker>> crop_producer_workers;
    std::vector<std::unique_ptr<CropAndEncodeWorker>> crop_encode_workers;
    std::vector<std::unique_ptr<CropPreviewWorker>> crop_preview_workers;
    std::vector<std::unique_ptr<SpatialSnapshotWorker>> spatial_snapshot_workers;
    std::vector<std::unique_ptr<PoseWorker>> pose_workers;
    std::vector<std::unique_ptr<YoloWorker>> yolo_workers;
    std::unique_ptr<GL_Texture[]> display_textures;
    std::unique_ptr<GL_Texture[]> crop_textures;
    bool background_resources_cleaned = false;
};

void cleanup_background_stream_product(
    StreamStartupProduct* product,
    const GuiStreamStartupBindings& bindings) noexcept
{
    if (!product || product->background_resources_cleaned) {
        return;
    }

    // No worker thread is started before activation. Destruction is therefore
    // bounded and cannot race live queues.
    for (int i = product->camera_count - 1; i >= 0; --i) {
        product->pose_workers[static_cast<std::size_t>(i)].reset();
        product->spatial_snapshot_workers[static_cast<std::size_t>(i)].reset();
        product->crop_preview_workers[static_cast<std::size_t>(i)].reset();
        product->crop_encode_workers[static_cast<std::size_t>(i)].reset();
        product->crop_producer_workers[static_cast<std::size_t>(i)].reset();
        product->yolo_workers[static_cast<std::size_t>(i)].reset();
        product->display_workers[static_cast<std::size_t>(i)].reset();
    }

    if (bindings.cameras && bindings.camera_params) {
        for (int i = product->camera_count - 1; i >= 0; --i) {
            const std::size_t index = static_cast<std::size_t>(i);
            if (product->frame_buffers_allocated[index]) {
                try {
                    destroy_frame_buffer(
                        &bindings.cameras[i].camera,
                        bindings.cameras[i].evt_frame,
                        bindings.evt_buffer_size,
                        &bindings.camera_params[i]);
                } catch (const std::exception& error) {
                    std::cerr << "[GUI][camera_startup] frame-buffer rollback failed"
                              << " camera=" << bindings.camera_params[i].camera_serial
                              << " error=" << error.what() << std::endl;
                } catch (...) {
                }
                delete[] bindings.cameras[i].evt_frame;
                bindings.cameras[i].evt_frame = nullptr;
                bindings.cameras[i].evt_frame_count = 0;
                product->frame_buffers_allocated[index] = false;
            }
            if (product->stream_opened[index]) {
                const EVT_ERROR error =
                    EVT_CameraCloseStream(&bindings.cameras[i].camera);
                if (error != EVT_SUCCESS) {
                    std::cerr << "[GUI][camera_startup] stream-close rollback failed"
                              << " camera=" << bindings.camera_params[i].camera_serial
                              << " error=" << get_evt_error_string(error) << std::endl;
                }
                product->stream_opened[index] = false;
            }
        }
    }

    product->frame_ipc_managers.clear();
    for (int i = product->camera_count - 1; i >= 0; --i) {
        const std::size_t index = static_cast<std::size_t>(i);
        if (!product->resource_initialized[index]) {
            continue;
        }
        try {
            product->camera_resources[index].cleanup();
        } catch (...) {
        }
        product->resource_initialized[index] = false;
    }
    product->background_resources_cleaned = true;
}

void cleanup_gui_stream_textures(
    StreamStartupProduct* product,
    const GuiStreamStartupBindings& bindings) noexcept
{
    if (!product || !bindings.camera_params || !bindings.camera_selection) {
        return;
    }
    for (int i = product->camera_count - 1; i >= 0; --i) {
        const std::size_t index = static_cast<std::size_t>(i);
        try {
            if (product->crop_texture_initialized[index]) {
                clear_upload_and_cleanup(
                    product->crop_textures[i],
                    bindings.crop_size_px,
                    bindings.crop_size_px);
                product->crop_texture_initialized[index] = false;
            }
            if (product->display_texture_initialized[index]) {
                const int width = std::max(
                    1,
                    static_cast<int>(bindings.camera_params[i].width) /
                        std::max(1, bindings.camera_selection[i].downsample));
                const int height = std::max(
                    1,
                    static_cast<int>(bindings.camera_params[i].height) /
                        std::max(1, bindings.camera_selection[i].downsample));
                clear_upload_and_cleanup(
                    product->display_textures[i], width, height);
                product->display_texture_initialized[index] = false;
            }
        } catch (const std::exception& error) {
            std::cerr << "[GUI][camera_startup] texture rollback failed"
                      << " camera=" << bindings.camera_params[i].camera_serial
                      << " error=" << error.what() << std::endl;
        } catch (...) {
        }
    }
}

bool validate_stream_bindings(
    const GuiStreamStartupBindings& bindings,
    std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!bindings.cameras || !bindings.camera_params ||
        !bindings.camera_selection || bindings.camera_count <= 0 ||
        !bindings.camera_control || !bindings.ptp_params ||
        !bindings.encoder_config || !bindings.yolo_model ||
        !bindings.indigo_signal_builder || !bindings.image_writer ||
        !bindings.recording_session || !bindings.app_storage_config ||
        !bindings.timing || !bindings.camera_resources ||
        !bindings.frame_ipc_managers || !bindings.frame_ipc_init_errors ||
        !bindings.display_workers || !bindings.crop_producer_workers ||
        !bindings.crop_encode_workers || !bindings.crop_preview_workers ||
        !bindings.spatial_snapshot_workers || !bindings.pose_workers ||
        !bindings.display_textures || !bindings.crop_textures ||
        !bindings.yolo_workers || !bindings.acquisition_threads) {
        if (error_out) {
            *error_out = "stream startup bindings are incomplete";
        }
        return false;
    }
    if (*bindings.display_workers || *bindings.crop_producer_workers ||
        *bindings.crop_encode_workers || *bindings.crop_preview_workers ||
        *bindings.spatial_snapshot_workers || *bindings.pose_workers ||
        *bindings.display_textures || *bindings.crop_textures ||
        !bindings.camera_resources->empty() ||
        !bindings.acquisition_threads->empty()) {
        if (error_out) {
            *error_out = "stream runtime storage is not empty";
        }
        return false;
    }
    return true;
}

}  // namespace

const char* gui_camera_startup_phase_name(
    const GuiCameraStartupPhase phase) noexcept
{
    switch (phase) {
        case GuiCameraStartupPhase::kIdle: return "idle";
        case GuiCameraStartupPhase::kOpeningCameras: return "opening_cameras";
        case GuiCameraStartupPhase::kPreparingStreamResources:
            return "preparing_stream_resources";
        case GuiCameraStartupPhase::kWaitingForGuiTextures:
            return "waiting_for_gui_textures";
        case GuiCameraStartupPhase::kConstructingStreamRuntime:
            return "constructing_stream_runtime";
        case GuiCameraStartupPhase::kWaitingForFirstFrames:
            return "waiting_for_first_frames";
        case GuiCameraStartupPhase::kCanceling: return "canceling";
        case GuiCameraStartupPhase::kFailed: return "failed";
    }
    return "unknown";
}

RecordingPreflightResult run_gui_recording_preflight(
    const CameraParams* camera_params,
    const CameraEachSelect* camera_selection,
    const int camera_count,
    const std::string& selected_yolo_model,
    const int crop_size_px)
{
    RecordingPreflightResult result = run_recording_preflight(
        build_validation_inputs(camera_params, camera_selection, camera_count),
        [](const int source_gpu_id, const int helper_gpu_id) {
            return build_recording_validation_gpu_path_info(
                source_gpu_id, helper_gpu_id);
        });
    const int resolved_crop_size =
        CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
    if (!camera_params || !camera_selection || camera_count <= 0) {
        return result;
    }

    for (int i = 0; i < camera_count; ++i) {
        const std::string serial = camera_params[i].camera_serial.empty()
            ? ("camera_index_" + std::to_string(i))
            : camera_params[i].camera_serial;
        auto append_camera_error = [&](const std::string& message) {
            result.errors.push_back(serial + ": " + message);
            result.ok = false;
        };

        if (camera_selection[i].yolo) {
            std::string engine_path = selected_yolo_model;
            if (camera_selection[i].yolo_model &&
                camera_selection[i].yolo_model[0] != '\0') {
                engine_path = camera_selection[i].yolo_model;
            }
            if (engine_path.empty()) {
                append_camera_error(
                    "YOLO enabled but no detect engine is configured or selected.");
            } else if (!std::filesystem::exists(engine_path)) {
                append_camera_error(
                    "YOLO detect engine does not exist: " + engine_path);
            }
        }

        if (!camera_selection[i].crop_and_encode) {
            if (camera_selection[i].pose) {
                append_camera_error("Pose currently requires Crop+Encode enabled.");
            }
            continue;
        }
        if (!camera_selection[i].record) {
            append_camera_error(
                "Crop+Encode currently requires full-frame Record enabled.");
        }
        if (!camera_selection[i].yolo) {
            append_camera_error("Crop+Encode requires YOLO enabled.");
        }
        if (camera_selection[i].pose && !camera_selection[i].yolo) {
            append_camera_error("Pose currently requires YOLO enabled.");
        }
        if (static_cast<int>(camera_params[i].width) < resolved_crop_size ||
            static_cast<int>(camera_params[i].height) < resolved_crop_size) {
            std::ostringstream error;
            error << "Crop+Encode requires source frames at least "
                  << resolved_crop_size << "x" << resolved_crop_size
                  << "; configured frame is " << camera_params[i].width
                  << "x" << camera_params[i].height << ".";
            append_camera_error(error.str());
        }
    }
    result.ok = result.errors.empty();
    return result;
}

int resolve_gui_crop_size_from_camera_configs(
    const CameraParams* camera_params,
    const int camera_count,
    const int fallback_crop_size,
    bool* mixed_values_out)
{
    if (mixed_values_out) *mixed_values_out = false;
    if (!camera_params || camera_count <= 0) {
        return CropAndEncodeWorker::SanitizeCropSize(fallback_crop_size);
    }
    const int resolved = CropAndEncodeWorker::SanitizeCropSize(
        camera_params[0].crop_pipeline.crop_size_px);
    bool mixed = false;
    for (int i = 1; i < camera_count; ++i) {
        if (CropAndEncodeWorker::SanitizeCropSize(
                camera_params[i].crop_pipeline.crop_size_px) != resolved) {
            mixed = true;
            break;
        }
    }
    if (mixed_values_out) *mixed_values_out = mixed;
    return resolved;
}

int resolve_gui_crop_preview_max_fps_from_camera_configs(
    const CameraParams* camera_params,
    const int camera_count,
    const int fallback_preview_max_fps,
    bool* mixed_values_out)
{
    if (mixed_values_out) *mixed_values_out = false;
    if (!camera_params || camera_count <= 0) {
        return sanitize_camera_crop_preview_max_fps(fallback_preview_max_fps);
    }
    const int resolved = sanitize_camera_crop_preview_max_fps(
        camera_params[0].crop_pipeline.preview_max_fps);
    bool mixed = false;
    for (int i = 1; i < camera_count; ++i) {
        if (sanitize_camera_crop_preview_max_fps(
                camera_params[i].crop_pipeline.preview_max_fps) != resolved) {
            mixed = true;
            break;
        }
    }
    if (mixed_values_out) *mixed_values_out = mixed;
    return resolved;
}

void apply_gui_crop_size_to_camera_configs(
    CameraParams* camera_params,
    const int camera_count,
    const int crop_size_px)
{
    if (!camera_params || camera_count <= 0) return;
    const int resolved = CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
    for (int i = 0; i < camera_count; ++i) {
        camera_params[i].crop_pipeline.crop_size_px = resolved;
    }
}

void apply_gui_crop_preview_max_fps_to_camera_configs(
    CameraParams* camera_params,
    const int camera_count,
    const int preview_max_fps)
{
    if (!camera_params || camera_count <= 0) return;
    const int resolved = sanitize_camera_crop_preview_max_fps(preview_max_fps);
    for (int i = 0; i < camera_count; ++i) {
        camera_params[i].crop_pipeline.preview_max_fps = resolved;
    }
}

struct GuiCameraStartupController::Impl {
    mutable std::mutex mutex;
    GuiAsyncStartupWorker worker;
    GuiCameraStartupPhase phase = GuiCameraStartupPhase::kIdle;
    std::string operation;
    std::string message;
    bool cancel_pending = false;
    std::string cancel_reason;
    bool stream_runtime_installed = false;
    uint64_t stream_request_started_ns = 0;
    GuiStartupTimingRecorder* open_timing = nullptr;
    uint64_t open_background_finished_ns = 0;
    std::string selected_yolo_model;
    std::unique_ptr<GuiCameraOpenProduct> open_product;
    std::unique_ptr<StreamStartupProduct> stream_product;
    GuiStreamStartupBindings stream_bindings;
    std::vector<std::string> pending_errors;

    void set_phase(
        const GuiCameraStartupPhase new_phase,
        std::string new_operation,
        std::string new_message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        phase = new_phase;
        operation = std::move(new_operation);
        message = std::move(new_message);
    }

    std::string current_cancel_reason() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return cancel_reason.empty() ? "startup_canceled" : cancel_reason;
    }

    GuiAsyncStartupWorkResult prepare_stream_resources(
        const std::atomic<bool>& cancel_requested)
    {
        GuiStreamStartupBindings bindings;
        {
            std::lock_guard<std::mutex> lock(mutex);
            bindings = stream_bindings;
        }
        auto product =
            std::make_unique<StreamStartupProduct>(bindings.camera_count);
        try {
            const uint64_t preflight_started_ns = now_ns();
            const RecordingPreflightResult preflight =
                run_gui_recording_preflight(
                    bindings.camera_params,
                    bindings.camera_selection,
                    bindings.camera_count,
                    *bindings.yolo_model,
                    bindings.crop_size_px);
            bindings.timing->RecordGlobalInterval(
                "recording_preflight",
                preflight_started_ns,
                now_ns());
            if (!preflight.ok) {
                std::lock_guard<std::mutex> lock(mutex);
                pending_errors = preflight.errors;
                return GuiAsyncStartupWorkResult::Failed(
                    "recording_preflight_failed");
            }
            if (cancel_requested.load(std::memory_order_acquire)) {
                return GuiAsyncStartupWorkResult::Canceled(
                    current_cancel_reason());
            }

            const uint64_t allocation_started_ns = now_ns();
            std::size_t maximum_frame_bytes = 0;
            for (int i = 0; i < bindings.camera_count; ++i) {
                maximum_frame_bytes = std::max(
                    maximum_frame_bytes,
                    static_cast<std::size_t>(bindings.camera_params[i].width) *
                        static_cast<std::size_t>(bindings.camera_params[i].height));
            }
            bindings.timing->RecordGlobalInterval(
                "pipeline_session_allocation",
                allocation_started_ns,
                now_ns());

            for (int i = 0; i < bindings.camera_count; ++i) {
                if (!gui_camera_has_acquisition_work(
                        bindings.camera_selection[i])) {
                    continue;
                }
                if (cancel_requested.load(std::memory_order_acquire)) {
                    cleanup_background_stream_product(product.get(), bindings);
                    return GuiAsyncStartupWorkResult::Canceled(
                        current_cancel_reason());
                }
                GuiStartupTimingScope scope(
                    bindings.timing,
                    "camera_resource_and_ipc_initialization",
                    bindings.camera_params[i].camera_serial);
                std::cout << "Initializing resources for camera " << i
                          << " on GPU " << bindings.camera_params[i].gpu_id
                          << std::endl;
                product->camera_resources[static_cast<std::size_t>(i)].initialize(
                    bindings.camera_params[i].gpu_id,
                    maximum_frame_bytes,
                    bindings.camera_selection[i].yolo,
                    bindings.camera_params[i].recording.resources
                        .acquire_work_entries);
                product->resource_initialized[static_cast<std::size_t>(i)] = true;
                if (bindings.camera_selection[i].send_frame_ipc) {
                    auto manager =
                        std::make_unique<FrameIPCManager>(
                            &bindings.camera_params[i]);
                    if (!manager->isEnabled()) {
                        product->frame_ipc_init_errors[static_cast<std::size_t>(i)] =
                            manager->getInitError();
                    } else {
                        product->frame_ipc_managers[static_cast<std::size_t>(i)] =
                            std::move(manager);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                stream_product = std::move(product);
            }
            return GuiAsyncStartupWorkResult::Succeeded();
        } catch (...) {
            cleanup_background_stream_product(product.get(), bindings);
            return GuiAsyncStartupWorkResult::Failed(
                "stream resource preparation failed: " + exception_message());
        }
    }

    bool setup_gui_textures(std::string* error_out)
    {
        if (error_out) error_out->clear();
        try {
            // CUDA/OpenGL interop registration is device-specific. Keep all
            // GUI textures on the configured display device, matching the
            // pre-async lifecycle.
            const cudaError_t set_device_status =
                cudaSetDevice(stream_bindings.display_cuda_device_id);
            if (set_device_status != cudaSuccess) {
                throw std::runtime_error(
                    std::string("cudaSetDevice(display) failed: ") +
                    cudaGetErrorString(set_device_status));
            }
            for (int i = 0; i < stream_bindings.camera_count; ++i) {
                const std::size_t index = static_cast<std::size_t>(i);
                if (stream_bindings.camera_selection[i].stream_on) {
                    GuiStartupTimingScope scope(
                        stream_bindings.timing,
                        "display_texture_setup",
                        stream_bindings.camera_params[i].camera_serial);
                    const int width = std::max(
                        1,
                        static_cast<int>(stream_bindings.camera_params[i].width) /
                            std::max(
                                1,
                                stream_bindings.camera_selection[i].downsample));
                    const int height = std::max(
                        1,
                        static_cast<int>(stream_bindings.camera_params[i].height) /
                            std::max(
                                1,
                                stream_bindings.camera_selection[i].downsample));
                    setup_texture(
                        stream_product->display_textures[i], width, height);
                    stream_product->display_texture_initialized[index] = true;
                }
                if (stream_bindings.camera_selection[i].crop_and_encode) {
                    GuiStartupTimingScope scope(
                        stream_bindings.timing,
                        "crop_texture_setup",
                        stream_bindings.camera_params[i].camera_serial);
                    setup_texture(
                        stream_product->crop_textures[i],
                        stream_bindings.crop_size_px,
                        stream_bindings.crop_size_px);
                    stream_product->crop_texture_initialized[index] = true;
                }
            }
            return true;
        } catch (const std::exception& error) {
            if (error_out) *error_out = error.what();
        } catch (...) {
            if (error_out) *error_out = "non-standard texture setup failure";
        }
        return false;
    }

    GuiAsyncStartupWorkResult construct_stream_runtime(
        const std::atomic<bool>& cancel_requested)
    {
        try {
            const uint64_t storage_started_ns = now_ns();
            stream_bindings.timing->RecordGlobalInterval(
                "worker_storage_allocation",
                storage_started_ns,
                now_ns());

            for (int i = 0; i < stream_bindings.camera_count; ++i) {
                if (cancel_requested.load(std::memory_order_acquire)) {
                    cleanup_background_stream_product(
                        stream_product.get(), stream_bindings);
                    return GuiAsyncStartupWorkResult::Canceled(
                        current_cancel_reason());
                }
                GuiStartupTimingScope scope(
                    stream_bindings.timing,
                    "worker_construction",
                    stream_bindings.camera_params[i].camera_serial);
                CameraResources& resources =
                    stream_product->camera_resources[static_cast<std::size_t>(i)];
                if (stream_bindings.camera_selection[i].stream_on) {
                    const std::string name =
                        "OpenGLDisplay_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    stream_product->display_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<COpenGLDisplay>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            &stream_bindings.camera_selection[i],
                            stream_product->display_textures[i].cuda_buffer,
                            stream_bindings.indigo_signal_builder,
                            *resources.recycle_queue);
                }
                if (stream_bindings.camera_selection[i].yolo) {
                    const std::string name =
                        "YoloWorker_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    stream_product->yolo_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<YoloWorker>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            &stream_bindings.camera_selection[i],
                            stream_bindings.camera_control,
                            *resources.recycle_queue);
                    if (stream_product->display_workers[static_cast<std::size_t>(i)]) {
                        stream_product->yolo_workers[static_cast<std::size_t>(i)]
                            ->SetDisplayWorker(
                                stream_product
                                    ->display_workers[static_cast<std::size_t>(i)]
                                    .get());
                    }
                }
                if (stream_bindings.camera_selection[i].crop_and_encode ||
                    stream_bindings.camera_selection[i].pose) {
                    const std::string name =
                        "CropProducer_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    stream_product
                        ->crop_producer_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<CropProducerWorker>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            *resources.recycle_queue,
                            stream_bindings.camera_control,
                            stream_bindings.crop_size_px);
                    if (stream_product->yolo_workers[static_cast<std::size_t>(i)]) {
                        stream_product->yolo_workers[static_cast<std::size_t>(i)]
                            ->SetCropProducerWorker(
                                stream_product
                                    ->crop_producer_workers[static_cast<std::size_t>(i)]
                                    .get());
                    }
                }
                if (stream_bindings.camera_selection[i].crop_and_encode) {
                    const std::string name =
                        "CropEncode_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    stream_product->crop_encode_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<CropAndEncodeWorker>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            stream_bindings.encoder_config->folder_name,
                            *resources.recycle_queue,
                            stream_product->crop_textures[i].cuda_buffer,
                            stream_bindings.camera_control,
                            stream_bindings.crop_size_px);
                    auto* producer = stream_product
                        ->crop_producer_workers[static_cast<std::size_t>(i)]
                        .get();
                    if (producer) {
                        const std::string preview_name =
                            "CropPreview_Cam_" +
                            stream_bindings.camera_params[i].camera_serial;
                        stream_product
                            ->crop_preview_workers[static_cast<std::size_t>(i)] =
                            std::make_unique<CropPreviewWorker>(
                                preview_name.c_str(),
                                &stream_bindings.camera_params[i],
                                stream_product->crop_textures[i].cuda_buffer,
                                producer->GetCropProducer(),
                                stream_bindings.crop_size_px);
                        auto* crop_preview = stream_product
                            ->crop_preview_workers[static_cast<std::size_t>(i)]
                            .get();
                        auto* crop_encode = stream_product
                            ->crop_encode_workers[static_cast<std::size_t>(i)]
                            .get();
                        crop_preview->SetPreviewDisplayEnabled(
                            stream_bindings.show_crop_preview_windows);
                        crop_encode->SetCropProducer(producer->GetCropProducer());
                        crop_encode->SetCropProducerWorker(producer);
                        crop_encode->SetCropPreviewWorker(crop_preview);
                        producer->SetCropPreviewWorker(crop_preview);
                        producer->SetCropAndEncodeWorker(crop_encode);
                    }
                }
                if (stream_bindings.camera_selection[i].pose &&
                    stream_product
                        ->crop_producer_workers[static_cast<std::size_t>(i)]) {
                    const std::string name =
                        "PoseWorker_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    auto* producer = stream_product
                        ->crop_producer_workers[static_cast<std::size_t>(i)]
                        .get();
                    stream_product->pose_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<PoseWorker>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            producer->GetCropProducer(),
                            stream_product
                                ->frame_ipc_managers[static_cast<std::size_t>(i)]
                                .get());
                    producer->SetPoseWorker(
                        stream_product->pose_workers[static_cast<std::size_t>(i)]
                            .get());
                }
                if (gui_camera_has_acquisition_work(
                        stream_bindings.camera_selection[i])) {
                    const std::string name =
                        "SpatialSnapshot_Cam_" +
                        stream_bindings.camera_params[i].camera_serial;
                    stream_product
                        ->spatial_snapshot_workers[static_cast<std::size_t>(i)] =
                        std::make_unique<SpatialSnapshotWorker>(
                            name.c_str(),
                            &stream_bindings.camera_params[i],
                            *resources.recycle_queue);
                }
            }

            for (int i = 0; i < stream_bindings.camera_count; ++i) {
                if (!gui_camera_has_acquisition_work(
                        stream_bindings.camera_selection[i])) {
                    continue;
                }
                if (cancel_requested.load(std::memory_order_acquire)) {
                    cleanup_background_stream_product(
                        stream_product.get(), stream_bindings);
                    return GuiAsyncStartupWorkResult::Canceled(
                        current_cancel_reason());
                }
                {
                    GuiStartupTimingScope scope(
                        stream_bindings.timing,
                        "stream_open",
                        stream_bindings.camera_params[i].camera_serial);
                    camera_open_stream(
                        &stream_bindings.cameras[i].camera,
                        &stream_bindings.camera_params[i],
                        "gui_async_start_streaming");
                    stream_product->stream_opened[static_cast<std::size_t>(i)] =
                        true;
                }
                {
                    GuiStartupTimingScope scope(
                        stream_bindings.timing,
                        "frame_buffer_allocate_and_queue",
                        stream_bindings.camera_params[i].camera_serial);
                    stream_bindings.cameras[i].evt_frame =
                        new Emergent::CEmergentFrame[stream_bindings.evt_buffer_size];
                    stream_bindings.cameras[i].evt_frame_count =
                        stream_bindings.evt_buffer_size;
                    allocate_frame_buffer(
                        &stream_bindings.cameras[i].camera,
                        stream_bindings.cameras[i].evt_frame,
                        &stream_bindings.camera_params[i],
                        stream_bindings.evt_buffer_size);
                    stream_product
                        ->frame_buffers_allocated[static_cast<std::size_t>(i)] =
                        true;
                }
            }
            if (stream_bindings.ptp_stream_sync) {
                for (int i = 0; i < stream_bindings.camera_count; ++i) {
                    if (!gui_camera_has_acquisition_work(
                            stream_bindings.camera_selection[i])) {
                        continue;
                    }
                    GuiStartupTimingScope scope(
                        stream_bindings.timing,
                        "ptp_mode_configuration",
                        stream_bindings.camera_params[i].camera_serial);
                    ptp_camera_sync(
                        &stream_bindings.cameras[i].camera,
                        &stream_bindings.camera_params[i]);
                }
            }
            return GuiAsyncStartupWorkResult::Succeeded();
        } catch (...) {
            const std::string error = exception_message();
            cleanup_background_stream_product(
                stream_product.get(), stream_bindings);
            return GuiAsyncStartupWorkResult::Failed(
                "stream runtime construction failed: " + error);
        }
    }

    GuiCameraStartupEvent fail_stream_before_activation(
        const GuiAsyncStartupWorkResult& result)
    {
        GuiCameraStartupEvent event;
        event.kind = result.canceled
            ? GuiCameraStartupEventKind::kCanceled
            : GuiCameraStartupEventKind::kStreamFailed;
        event.message = result.error.empty()
            ? (result.canceled ? "stream startup canceled" : "stream startup failed")
            : result.error;
        {
            std::lock_guard<std::mutex> lock(mutex);
            event.errors = pending_errors;
            if (event.errors.empty() && !event.message.empty()) {
                event.errors.push_back(event.message);
            }
        }
        if (stream_product) {
            cleanup_background_stream_product(
                stream_product.get(), stream_bindings);
            cleanup_gui_stream_textures(
                stream_product.get(), stream_bindings);
            stream_product.reset();
        }
        stream_bindings.camera_control->subscribe = false;
        if (result.canceled) {
            stream_bindings.timing->MarkStopped(event.message);
        } else {
            stream_bindings.timing->MarkFailed(event.message);
        }
        stream_bindings.timing->MarkHandlerComplete();
        stream_bindings.timing->FlushPending();
        set_phase(
            result.canceled
                ? GuiCameraStartupPhase::kIdle
                : GuiCameraStartupPhase::kFailed,
            {},
            event.message);
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel_pending = false;
            cancel_reason.clear();
        }
        return event;
    }

    GuiCameraStartupEvent activate_stream_runtime()
    {
        GuiCameraStartupEvent event;
        event.kind = GuiCameraStartupEventKind::kStreamActivated;
        event.message = "stream runtime activated; awaiting first frames";
        try {
            const int count = stream_bindings.camera_count;
            // Allocate every main-owned slot under local RAII first. The GUI
            // must see either an entirely empty runtime or a complete one;
            // allocation failure cannot leave partially installed arrays.
            auto display_worker_slots =
                std::make_unique<COpenGLDisplay*[]>(count);
            auto crop_producer_slots =
                std::make_unique<CropProducerWorker*[]>(count);
            auto crop_encode_slots =
                std::make_unique<CropAndEncodeWorker*[]>(count);
            auto crop_preview_slots =
                std::make_unique<CropPreviewWorker*[]>(count);
            auto spatial_snapshot_slots =
                std::make_unique<SpatialSnapshotWorker*[]>(count);
            auto pose_slots = std::make_unique<PoseWorker*[]>(count);
            std::vector<YoloWorker*> yolo_slots(
                static_cast<std::size_t>(count), nullptr);

            for (int i = 0; i < count; ++i) {
                const std::size_t index = static_cast<std::size_t>(i);
                display_worker_slots[i] =
                    stream_product->display_workers[index].release();
                crop_producer_slots[i] =
                    stream_product->crop_producer_workers[index].release();
                crop_encode_slots[i] =
                    stream_product->crop_encode_workers[index].release();
                crop_preview_slots[i] =
                    stream_product->crop_preview_workers[index].release();
                spatial_snapshot_slots[i] =
                    stream_product->spatial_snapshot_workers[index].release();
                pose_slots[i] =
                    stream_product->pose_workers[index].release();
                yolo_slots[index] =
                    stream_product->yolo_workers[index].release();
            }

            *stream_bindings.display_workers = display_worker_slots.release();
            *stream_bindings.crop_producer_workers =
                crop_producer_slots.release();
            *stream_bindings.crop_encode_workers = crop_encode_slots.release();
            *stream_bindings.crop_preview_workers = crop_preview_slots.release();
            *stream_bindings.spatial_snapshot_workers =
                spatial_snapshot_slots.release();
            *stream_bindings.pose_workers = pose_slots.release();
            *stream_bindings.yolo_workers = std::move(yolo_slots);
            *stream_bindings.display_textures =
                stream_product->display_textures.release();
            *stream_bindings.crop_textures =
                stream_product->crop_textures.release();
            *stream_bindings.camera_resources =
                std::move(stream_product->camera_resources);
            *stream_bindings.frame_ipc_managers =
                std::move(stream_product->frame_ipc_managers);
            *stream_bindings.frame_ipc_init_errors =
                std::move(stream_product->frame_ipc_init_errors);
            {
                std::lock_guard<std::mutex> lock(mutex);
                stream_runtime_installed = true;
            }

            // subscribe means the runtime arrays are installed and safe for
            // the rest of the GUI frame to consume. Background preparation
            // uses the controller's busy state as its mutation lock instead.
            stream_bindings.camera_control->subscribe = true;
            stream_bindings.timing->MarkGlobalInstant("subscribe_enabled");
            // Camera streams, frame buffers, workers, textures, and CUDA
            // resources now belong to main's ordinary teardown path. Drop the
            // staging shell immediately so an activation failure cannot later
            // attempt a second rollback of transferred resources.
            stream_product.reset();

            {
                GuiStartupTimingScope scope(
                    stream_bindings.timing,
                    "recording_pipeline_construction");
                orange::session::create_recording_pipelines_for_stream(
                    stream_bindings.recording_session,
                    stream_bindings.camera_params,
                    stream_bindings.camera_selection,
                    count,
                    *stream_bindings.encoder_config,
                    stream_bindings.camera_resources->data(),
                    stream_bindings.camera_control,
                    stream_bindings.app_storage_config);
            }

            for (int i = 0; i < count; ++i) {
                GuiStartupTimingScope scope(
                    stream_bindings.timing,
                    "worker_thread_start",
                    stream_bindings.camera_params[i].camera_serial);
                if ((*stream_bindings.display_workers)[i]) {
                    (*stream_bindings.display_workers)[i]->SetMaxQueueSize(240);
                    (*stream_bindings.display_workers)[i]->StartThread();
                }
                if ((*stream_bindings.crop_encode_workers)[i]) {
                    (*stream_bindings.crop_encode_workers)[i]->SetMaxQueueSize(240);
                    (*stream_bindings.crop_encode_workers)[i]->StartThread();
                }
                if ((*stream_bindings.crop_preview_workers)[i]) {
                    (*stream_bindings.crop_preview_workers)[i]->SetMaxQueueSize(
                        CropPreviewWorker::kDefaultQueueSize);
                    (*stream_bindings.crop_preview_workers)[i]->StartThread();
                }
                if ((*stream_bindings.pose_workers)[i]) {
                    (*stream_bindings.pose_workers)[i]->SetMaxQueueSize(32);
                    (*stream_bindings.pose_workers)[i]->StartThread();
                }
                if ((*stream_bindings.spatial_snapshot_workers)[i]) {
                    (*stream_bindings.spatial_snapshot_workers)[i]->SetMaxQueueSize(2);
                    (*stream_bindings.spatial_snapshot_workers)[i]->StartThread();
                }
                if ((*stream_bindings.crop_producer_workers)[i]) {
                    (*stream_bindings.crop_producer_workers)[i]->SetMaxQueueSize(240);
                    (*stream_bindings.crop_producer_workers)[i]->StartThread();
                }
                if ((*stream_bindings.yolo_workers)[static_cast<std::size_t>(i)]) {
                    (*stream_bindings.yolo_workers)[static_cast<std::size_t>(i)]
                        ->SetMaxQueueSize(240);
                    (*stream_bindings.yolo_workers)[static_cast<std::size_t>(i)]
                        ->StartThread();
                }
                orange::session::start_recording_pipeline_for_camera(
                    stream_bindings.recording_session, i);
            }

            if (stream_bindings.ptp_stream_sync) {
                stream_bindings.camera_control->sync_camera = true;
            }

            for (int i = 0; i < count; ++i) {
                if (!gui_camera_has_acquisition_work(
                        stream_bindings.camera_selection[i])) {
                    continue;
                }
                CameraEmergent* acquire_camera = &stream_bindings.cameras[i];
                CameraParams* acquire_params = &stream_bindings.camera_params[i];
                CameraEachSelect* acquire_selection =
                    &stream_bindings.camera_selection[i];
                COpenGLDisplay* acquire_display =
                    (*stream_bindings.display_workers)[i];
                RecordingIngress* acquire_ingress =
                    orange::session::recording_ingress_for_camera(
                        *stream_bindings.recording_session, i);
                YoloWorker* acquire_yolo =
                    (*stream_bindings.yolo_workers)[static_cast<std::size_t>(i)];
                CameraResources* acquire_resources =
                    &(*stream_bindings.camera_resources)[static_cast<std::size_t>(i)];
                FrameIPCManager* acquire_ipc =
                    (*stream_bindings.frame_ipc_managers)[static_cast<std::size_t>(i)]
                        .get();
                SpatialSnapshotWorker* acquire_snapshot =
                    (*stream_bindings.spatial_snapshot_workers)[i];
                GuiStartupTimingRecorder* timing = stream_bindings.timing;
                GuiStartupTimingScope scope(
                    timing,
                    "acquisition_thread_launch",
                    acquire_params->camera_serial);
                stream_bindings.acquisition_threads->emplace_back(
                    [=]() {
                        try {
                            acquire_frames(
                                acquire_camera,
                                acquire_params,
                                acquire_selection,
                                stream_bindings.camera_control,
                                stream_bindings.ptp_params,
                                stream_bindings.indigo_signal_builder,
                                acquire_display,
                                acquire_ingress,
                                acquire_yolo,
                                stream_bindings.image_writer,
                                acquire_resources,
                                acquire_ipc,
                                nullptr,
                                acquire_snapshot,
                                timing);
                        } catch (const std::exception& error) {
                            timing->MarkCameraInstant(
                                acquire_params->camera_serial,
                                "acquisition_thread_failed",
                                0,
                                {{"error", error.what()}});
                            std::cerr << "[FATAL] acquisition thread for camera "
                                      << acquire_params->camera_serial
                                      << " failed: " << error.what() << std::endl;
                        } catch (...) {
                            timing->MarkCameraInstant(
                                acquire_params->camera_serial,
                                "acquisition_thread_failed",
                                0,
                                {{"error", "non_std_exception"}});
                            std::cerr << "[FATAL] acquisition thread for camera "
                                      << acquire_params->camera_serial
                                      << " failed with a non-std exception"
                                      << std::endl;
                        }
                    });
            }
            stream_bindings.timing->MarkHandlerComplete();
            set_phase(
                GuiCameraStartupPhase::kWaitingForFirstFrames,
                "stream_start",
                event.message);
            return event;
        } catch (...) {
            event.kind = GuiCameraStartupEventKind::kStreamFailed;
            event.message =
                "stream activation failed: " + exception_message();
            event.errors = {event.message};
            stream_bindings.timing->MarkHandlerComplete();
            stream_bindings.timing->MarkFailed(event.message);
            set_phase(
                GuiCameraStartupPhase::kFailed,
                "stream_start",
                event.message);
            return event;
        }
    }
};

GuiCameraStartupController::GuiCameraStartupController()
    : impl_(std::make_unique<Impl>())
{
}

GuiCameraStartupController::~GuiCameraStartupController()
{
    ShutdownGuiThread();
}

bool GuiCameraStartupController::StartCameraOpen(
    GuiCameraOpenRequest request,
    std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!impl_ || request.selected_devices.empty() || !request.timing) {
        if (error_out) *error_out = "camera-open request is incomplete";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if ((impl_->phase != GuiCameraStartupPhase::kIdle &&
             impl_->phase != GuiCameraStartupPhase::kFailed) ||
            impl_->stream_runtime_installed) {
            if (error_out) *error_out = "another camera lifecycle is active";
            return false;
        }
        impl_->phase = GuiCameraStartupPhase::kOpeningCameras;
        impl_->operation = "camera_open";
        impl_->message = "Opening and configuring selected cameras...";
        impl_->cancel_pending = false;
        impl_->cancel_reason.clear();
        impl_->open_timing = request.timing;
        impl_->open_background_finished_ns = 0;
    }

    std::vector<GuiStartupTimingCamera> timing_cameras;
    timing_cameras.reserve(request.selected_devices.size());
    for (std::size_t i = 0; i < request.selected_devices.size(); ++i) {
        GuiStartupTimingCamera camera;
        camera.serial = request.selected_devices[i].device.serialNumber;
        camera.camera_index = static_cast<int>(i);
        camera.context = {{"device_index", request.selected_devices[i].device_index}};
        timing_cameras.push_back(std::move(camera));
    }
    request.timing->Begin(
        "camera_open",
        request.timing_artifact_directory,
        timing_cameras,
        request.timing_context,
        request.request_started_ns);
    request.timing->RecordGlobalInterval(
        "config_discovery_and_camera_selection",
        request.request_started_ns,
        request.selection_finished_ns > 0
            ? request.selection_finished_ns
            : now_ns());

    const bool started = impl_->worker.Start(
        "camera_open",
        [impl = impl_.get(), request = std::move(request)](
            const std::atomic<bool>& cancel_requested) mutable {
            auto product = std::make_unique<GuiCameraOpenProduct>();
            product->camera_count =
                static_cast<int>(request.selected_devices.size());
            product->camera_params =
                std::make_unique<CameraParams[]>(product->camera_count);
            product->camera_selection =
                std::make_unique<CameraEachSelect[]>(product->camera_count);
            product->cameras =
                std::make_unique<CameraEmergent[]>(product->camera_count);
            std::vector<bool> skip_setting_params(
                static_cast<std::size_t>(product->camera_count), false);
            int opened_camera_count = 0;
            try {
                for (int i = 0; i < product->camera_count; ++i) {
                    if (cancel_requested.load(std::memory_order_acquire)) {
                        product->camera_count = opened_camera_count;
                        close_open_product_cameras(product.get());
                        request.timing->MarkHandlerComplete();
                        const std::string reason =
                            impl->current_cancel_reason();
                        request.timing->MarkStopped(reason);
                        return GuiAsyncStartupWorkResult::Canceled(
                            reason);
                    }
                    const auto& selected =
                        request.selected_devices[static_cast<std::size_t>(i)];
                    GuiStartupTimingScope scope(
                        request.timing,
                        "load_camera_config",
                        selected.device.serialNumber);
                    GigEVisionDeviceInfo device = selected.device;
                    if (!set_camera_params(
                            &product->camera_params[i],
                            &device,
                            request.camera_config_files,
                            selected.device_index,
                            product->camera_count)) {
                        skip_setting_params[static_cast<std::size_t>(i)] = true;
                        product->camera_params[i].camera_id = selected.device_index;
                        product->camera_params[i].num_cameras =
                            product->camera_count;
                    }
                    product->camera_selection[i].downsample =
                        request.stream_downsample;
                    product->camera_selection[i].display_preview_max_fps =
                        request.display_preview_max_fps;
                    if (product->camera_params[i].camera_name ==
                        "ceiling_center") {
                        product->camera_selection[i].stream_on = true;
                        product->camera_selection[i].yolo = false;
                    }
                    if (product->camera_params[i].camera_name == "shelter") {
                        product->camera_selection[i].stream_on = true;
                    }
                }

                for (int i = 0; i < product->camera_count; ++i) {
                    if (cancel_requested.load(std::memory_order_acquire)) {
                        product->camera_count = opened_camera_count;
                        close_open_product_cameras(product.get());
                        request.timing->MarkHandlerComplete();
                        const std::string reason =
                            impl->current_cancel_reason();
                        request.timing->MarkStopped(reason);
                        return GuiAsyncStartupWorkResult::Canceled(
                            reason);
                    }
                    const auto& selected =
                        request.selected_devices[static_cast<std::size_t>(i)];
                    GigEVisionDeviceInfo device = selected.device;
                    GuiStartupTimingScope scope(
                        request.timing,
                        "open_and_configure_camera",
                        product->camera_params[i].camera_serial);
                    if (!skip_setting_params[static_cast<std::size_t>(i)]) {
                        open_camera_with_params(
                            &product->cameras[i].camera,
                            &device,
                            &product->camera_params[i],
                            "gui_async_open_selected_cameras");
                    } else {
                        update_camera_params(
                            &product->cameras[i].camera,
                            &device,
                            &product->camera_params[i]);
                    }
                    opened_camera_count = i + 1;
                }

                int ptp_count = 0;
                for (int i = 0; i < product->camera_count; ++i) {
                    if (camera_sync_mode_uses_ptp(&product->camera_params[i])) {
                        ++ptp_count;
                    }
                }
                product->ptp_stream_sync =
                    product->camera_count > 0 &&
                    ptp_count == product->camera_count;
                product->mixed_ptp_modes =
                    ptp_count > 0 && ptp_count < product->camera_count;
                product->crop_size_px = resolve_gui_crop_size_from_camera_configs(
                    product->camera_params.get(),
                    product->camera_count,
                    request.fallback_crop_size_px,
                    &product->mixed_crop_sizes);
                product->crop_preview_max_fps =
                    resolve_gui_crop_preview_max_fps_from_camera_configs(
                        product->camera_params.get(),
                        product->camera_count,
                        request.fallback_crop_preview_max_fps,
                        &product->mixed_crop_preview_max_fps);
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->open_product = std::move(product);
                    impl->open_background_finished_ns = now_ns();
                }
                return GuiAsyncStartupWorkResult::Succeeded();
            } catch (...) {
                product->camera_count = opened_camera_count;
                close_open_product_cameras(product.get());
                const std::string error = exception_message();
                request.timing->MarkHandlerComplete();
                request.timing->MarkFailed(
                    "camera_open_failed: " + error);
                return GuiAsyncStartupWorkResult::Failed(error);
            }
        },
        error_out);
    if (!started) {
        impl_->set_phase(GuiCameraStartupPhase::kFailed, {},
                         error_out ? *error_out : "could not start camera worker");
    }
    return started;
}

bool GuiCameraStartupController::StartStream(
    GuiStreamStartupBindings bindings,
    std::string* error_out)
{
    if (!impl_ || !validate_stream_bindings(bindings, error_out)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if ((impl_->phase != GuiCameraStartupPhase::kIdle &&
             impl_->phase != GuiCameraStartupPhase::kFailed) ||
            impl_->stream_runtime_installed) {
            if (error_out) *error_out = "another camera lifecycle is active";
            return false;
        }
        impl_->selected_yolo_model = *bindings.yolo_model;
        bindings.yolo_model = &impl_->selected_yolo_model;
        impl_->stream_bindings = bindings;
        impl_->phase = GuiCameraStartupPhase::kPreparingStreamResources;
        impl_->operation = "stream_start";
        impl_->message = "Preparing stream resources...";
        impl_->cancel_pending = false;
        impl_->cancel_reason.clear();
        impl_->pending_errors.clear();
        impl_->stream_request_started_ns = now_ns();
    }

    // These pointers are consumed by background worker constructors. Freeze
    // the selected engine identity before launching the worker so editing the
    // GUI field cannot invalidate a c_str() concurrently.
    for (int i = 0; i < bindings.camera_count; ++i) {
        if (bindings.camera_selection[i].yolo) {
            bindings.camera_selection[i].yolo_model =
                impl_->selected_yolo_model.c_str();
        }
    }

    std::vector<GuiStartupTimingCamera> timing_cameras;
    for (int i = 0; i < bindings.camera_count; ++i) {
        if (!gui_camera_has_acquisition_work(bindings.camera_selection[i])) {
            continue;
        }
        GuiStartupTimingCamera camera;
        camera.serial = bindings.camera_params[i].camera_serial;
        camera.camera_index = i;
        camera.gpu_id = bindings.camera_params[i].gpu_id;
        camera.context = {
            {"width", bindings.camera_params[i].width},
            {"height", bindings.camera_params[i].height},
            {"stream_on", bindings.camera_selection[i].stream_on},
            {"record", bindings.camera_selection[i].record},
            {"yolo", bindings.camera_selection[i].yolo},
            {"crop_and_encode", bindings.camera_selection[i].crop_and_encode},
            {"pose", bindings.camera_selection[i].pose},
            {"send_frame_ipc", bindings.camera_selection[i].send_frame_ipc},
        };
        timing_cameras.push_back(std::move(camera));
    }
    bindings.timing->Begin(
        "stream_start",
        bindings.timing_artifact_directory,
        timing_cameras,
        {
            {"ptp_stream_sync", bindings.ptp_stream_sync},
            {"evt_buffer_size", bindings.evt_buffer_size},
            {"selected_camera_count", timing_cameras.size()},
            {"yolo_model", *bindings.yolo_model},
            {"execution_model", "async_controller_two_phase"},
        },
        impl_->stream_request_started_ns);

    if (std::any_of(
            bindings.camera_selection,
            bindings.camera_selection + bindings.camera_count,
            [](const CameraEachSelect& selection) {
                return selection.record || selection.crop_and_encode;
            })) {
        bindings.encoder_config->folder_name = bindings.input_folder;
    }
    const bool started = impl_->worker.Start(
        "stream_resources",
        [impl = impl_.get()](const std::atomic<bool>& cancel_requested) {
            return impl->prepare_stream_resources(cancel_requested);
        },
        error_out);
    if (!started) {
        bindings.timing->MarkHandlerComplete();
        bindings.timing->MarkFailed(
            error_out ? *error_out : "stream resource worker did not start");
        impl_->set_phase(GuiCameraStartupPhase::kFailed, {},
                         error_out ? *error_out : "stream worker did not start");
    }
    return started;
}

GuiCameraStartupEvent GuiCameraStartupController::PollGuiThread()
{
    GuiCameraStartupEvent event;
    if (!impl_) return event;

    const auto completion = impl_->worker.Poll();
    if (completion) {
        if (completion->operation == "camera_open") {
            if (completion->result.success) {
                if (impl_->worker.cancel_requested()) {
                    std::unique_ptr<GuiCameraOpenProduct> canceled_product;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        canceled_product = std::move(impl_->open_product);
                    }
                    close_open_product_cameras(canceled_product.get());
                    const std::string reason =
                        impl_->current_cancel_reason();
                    if (impl_->open_timing) {
                        impl_->open_timing->MarkHandlerComplete();
                        impl_->open_timing->MarkStopped(reason);
                    }
                    event.kind = GuiCameraStartupEventKind::kCanceled;
                    event.message = reason;
                    impl_->set_phase(
                        GuiCameraStartupPhase::kIdle, {}, event.message);
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        impl_->cancel_pending = false;
                        impl_->cancel_reason.clear();
                    }
                    return event;
                }
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    event.camera_open_product = std::move(impl_->open_product);
                    impl_->phase = GuiCameraStartupPhase::kIdle;
                    impl_->operation.clear();
                    impl_->message = "Selected cameras opened successfully.";
                }
                event.kind = GuiCameraStartupEventKind::kCameraOpenSucceeded;
                event.message = "Selected cameras opened successfully.";
            } else {
                event.kind = completion->result.canceled
                    ? GuiCameraStartupEventKind::kCanceled
                    : GuiCameraStartupEventKind::kCameraOpenFailed;
                event.message = completion->result.error.empty()
                    ? (completion->result.canceled
                        ? "camera open canceled"
                        : "camera open failed")
                    : completion->result.error;
                impl_->set_phase(
                    completion->result.canceled
                        ? GuiCameraStartupPhase::kIdle
                        : GuiCameraStartupPhase::kFailed,
                    {},
                    event.message);
            }
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->cancel_pending = false;
                impl_->cancel_reason.clear();
            }
            return event;
        }

        if (completion->operation == "stream_resources") {
            if (!completion->result.success) {
                return impl_->fail_stream_before_activation(completion->result);
            }
            if (impl_->worker.cancel_requested()) {
                return impl_->fail_stream_before_activation(
                    GuiAsyncStartupWorkResult::Canceled(
                        impl_->current_cancel_reason()));
            }
            impl_->set_phase(
                GuiCameraStartupPhase::kWaitingForGuiTextures,
                "stream_start",
                "Creating GUI-owned OpenGL textures...");
            std::string texture_error;
            if (!impl_->setup_gui_textures(&texture_error)) {
                return impl_->fail_stream_before_activation(
                    GuiAsyncStartupWorkResult::Failed(
                        "GUI texture setup failed: " + texture_error));
            }
            impl_->set_phase(
                GuiCameraStartupPhase::kConstructingStreamRuntime,
                "stream_start",
                "Constructing workers and opening camera streams...");
            std::string start_error;
            if (!impl_->worker.Start(
                    "stream_runtime",
                    [impl = impl_.get()](
                        const std::atomic<bool>& cancel_requested) {
                        return impl->construct_stream_runtime(cancel_requested);
                    },
                    &start_error)) {
                return impl_->fail_stream_before_activation(
                    GuiAsyncStartupWorkResult::Failed(start_error));
            }
            return event;
        }

        if (completion->operation == "stream_runtime") {
            if (!completion->result.success) {
                return impl_->fail_stream_before_activation(completion->result);
            }
            if (impl_->worker.cancel_requested()) {
                return impl_->fail_stream_before_activation(
                    GuiAsyncStartupWorkResult::Canceled(
                        impl_->current_cancel_reason()));
            }
            return impl_->activate_stream_runtime();
        }
    }

    GuiCameraStartupPhase phase;
    bool cancel_pending = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        phase = impl_->phase;
        cancel_pending = impl_->cancel_pending;
    }
    if (cancel_pending &&
        phase == GuiCameraStartupPhase::kWaitingForGuiTextures &&
        !impl_->worker.running()) {
        return impl_->fail_stream_before_activation(
            GuiAsyncStartupWorkResult::Canceled(
                impl_->current_cancel_reason()));
    }

    if (phase == GuiCameraStartupPhase::kWaitingForFirstFrames) {
        const GuiStartupTimingStatus timing_status =
            impl_->stream_bindings.timing->Status();
        const bool all_first_frames =
            timing_status.expected_first_frame_camera_count == 0 ||
            timing_status.observed_first_frame_camera_count >=
                timing_status.expected_first_frame_camera_count;
        if (timing_status.status == "complete" && all_first_frames) {
            event.kind = GuiCameraStartupEventKind::kStreamReady;
            event.message = "All selected cameras delivered their first frame.";
            impl_->set_phase(
                GuiCameraStartupPhase::kIdle,
                {},
                event.message);
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->cancel_pending = false;
                impl_->cancel_reason.clear();
            }
            return event;
        }
        if (timing_status.status == "failed") {
            event.kind = GuiCameraStartupEventKind::kStreamFailed;
            event.message = timing_status.failure_reason.empty()
                ? "stream startup timing failed"
                : timing_status.failure_reason;
            event.errors = {event.message};
            impl_->set_phase(
                GuiCameraStartupPhase::kFailed,
                "stream_start",
                event.message);
            return event;
        }
        if (timing_status.status == "stopped" || cancel_pending) {
            event.kind = GuiCameraStartupEventKind::kCanceled;
            event.message = timing_status.stop_reason.empty()
                ? impl_->current_cancel_reason()
                : timing_status.stop_reason;
            return event;
        }
        const uint64_t elapsed_ns =
            now_ns() > impl_->stream_request_started_ns
                ? now_ns() - impl_->stream_request_started_ns
                : 0;
        if (static_cast<double>(elapsed_ns) / 1.0e9 >
            kFirstFrameTimeoutSeconds) {
            event.kind = GuiCameraStartupEventKind::kStreamFailed;
            event.message = "timed out waiting for all first camera frames";
            event.errors = {event.message};
            impl_->stream_bindings.timing->MarkFailed(event.message);
            impl_->set_phase(
                GuiCameraStartupPhase::kFailed,
                "stream_start",
                event.message);
            return event;
        }
    }
    return event;
}

void GuiCameraStartupController::NotifyCameraOpenInstalled() noexcept
{
    if (!impl_) return;
    GuiStartupTimingRecorder* timing = nullptr;
    uint64_t background_finished_ns = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        timing = impl_->open_timing;
        background_finished_ns = impl_->open_background_finished_ns;
        impl_->open_timing = nullptr;
        impl_->open_background_finished_ns = 0;
    }
    if (!timing) return;
    const uint64_t finished_ns = now_ns();
    if (background_finished_ns != 0) {
        timing->RecordGlobalInterval(
            "post_open_gui_configuration",
            background_finished_ns,
            finished_ns);
    }
    timing->MarkHandlerComplete(finished_ns);
    timing->MarkOperationComplete(finished_ns);
}

void GuiCameraStartupController::RequestCancel(std::string reason) noexcept
{
    if (!impl_) return;
    bool runtime_installed = false;
    GuiStartupTimingRecorder* timing = nullptr;
    std::string resolved_reason;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->phase == GuiCameraStartupPhase::kIdle) return;
        if (impl_->cancel_pending) return;
        impl_->cancel_pending = true;
        impl_->cancel_reason = reason.empty() ? "operator_requested" : reason;
        impl_->message = "Canceling camera startup...";
        runtime_installed = impl_->stream_runtime_installed;
        timing = impl_->stream_bindings.timing;
        resolved_reason = impl_->cancel_reason;
    }
    impl_->worker.RequestCancel();
    if (runtime_installed && timing) {
        timing->MarkStopped(resolved_reason);
    }
}

void GuiCameraStartupController::NotifyInstalledStreamStopped() noexcept
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stream_runtime_installed = false;
    impl_->cancel_pending = false;
    impl_->cancel_reason.clear();
    impl_->phase = GuiCameraStartupPhase::kIdle;
    impl_->operation.clear();
    impl_->message = "Stream stopped.";
}

void GuiCameraStartupController::ShutdownGuiThread() noexcept
{
    if (!impl_) return;
    RequestCancel("gui_shutdown");
    impl_->worker.Shutdown();
    if (impl_->open_product) {
        close_open_product_cameras(impl_->open_product.get());
        impl_->open_product.reset();
    }
    if (impl_->stream_product && !impl_->stream_runtime_installed) {
        cleanup_background_stream_product(
            impl_->stream_product.get(), impl_->stream_bindings);
        cleanup_gui_stream_textures(
            impl_->stream_product.get(), impl_->stream_bindings);
        impl_->stream_product.reset();
        if (impl_->stream_bindings.camera_control) {
            impl_->stream_bindings.camera_control->subscribe = false;
        }
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->stream_runtime_installed) {
        impl_->phase = GuiCameraStartupPhase::kIdle;
        impl_->operation.clear();
    }
    impl_->cancel_pending = false;
}

GuiCameraStartupStatus GuiCameraStartupController::status() const
{
    GuiCameraStartupStatus status;
    if (!impl_) return status;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    status.phase = impl_->phase;
    status.busy = impl_->phase != GuiCameraStartupPhase::kIdle &&
        impl_->phase != GuiCameraStartupPhase::kFailed;
    status.stream_runtime_installed = impl_->stream_runtime_installed;
    status.operation = impl_->operation;
    status.message = impl_->message;
    return status;
}

bool GuiCameraStartupController::busy() const
{
    return status().busy;
}

bool GuiCameraStartupController::stream_runtime_installed() const
{
    return status().stream_runtime_installed;
}

}  // namespace orange::gui
