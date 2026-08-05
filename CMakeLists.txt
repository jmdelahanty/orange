cmake_minimum_required(VERSION 3.19)
project(orange LANGUAGES CXX CUDA)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(ORANGE_ENABLE_NVTX "Enable NVTX profiling markers" OFF)
option(ORANGE_ENABLE_YOLO_PROFILE "Enable YOLO timing profile logging" OFF)
option(ORANGE_ENABLE_CUDA_DEBUG "Enable CUDA context debug logging" OFF)
option(ORANGE_USE_FAST_MATH "Enable aggressive host fast-math options outside Debug" ON)
option(ORANGE_ENABLE_TSAN "Build host concurrency tests with ThreadSanitizer" OFF)

if(ORANGE_ENABLE_TSAN)
    # TSan's shadow-memory layout conflicts with high-entropy ASLR on Linux
    # kernels >= 6.5 ("FATAL: ThreadSanitizer: unexpected memory mapping").
    # setarch -R disables ASLR for the test process only.
    set(ORANGE_TSAN_TEST_LAUNCHER setarch ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
else()
    set(ORANGE_TSAN_TEST_LAUNCHER "")
endif()

set(ORANGE_CUDA_ARCHITECTURES "86" CACHE STRING "CUDA architectures to compile for")
set(CMAKE_CUDA_ARCHITECTURES "${ORANGE_CUDA_ARCHITECTURES}" CACHE STRING "CUDA architectures" FORCE)

function(orange_pick_default_path out_var)
    foreach(candidate IN LISTS ARGN)
        if(candidate AND EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

set(ORANGE_ROOT "/opt/orange" CACHE PATH "Orange dependency root")

set(ORANGE_EVT_ROOT "" CACHE PATH "EVT SDK root")
if(NOT ORANGE_EVT_ROOT)
    orange_pick_default_path(_evt_default "/opt/EVT/eSDK")
    set(ORANGE_EVT_ROOT "${_evt_default}" CACHE PATH "EVT SDK root" FORCE)
endif()

set(ORANGE_CUDA_ROOT "" CACHE PATH "CUDA toolkit root")
if(NOT ORANGE_CUDA_ROOT)
    orange_pick_default_path(_cuda_default "/usr/local/cuda")
    set(ORANGE_CUDA_ROOT "${_cuda_default}" CACHE PATH "CUDA toolkit root" FORCE)
endif()

set(ORANGE_FFMPEG_ROOT "" CACHE PATH "FFmpeg root containing include/ and lib/")
if(NOT ORANGE_FFMPEG_ROOT)
    orange_pick_default_path(
        _ffmpeg_default
        "${ORANGE_ROOT}/lib/ffmpeg-nvidia"
        "$ENV{HOME}/nvidia/ffmpeg/build"
        "$ENV{HOME}/nvidia/ffmpeg")
    set(ORANGE_FFMPEG_ROOT "${_ffmpeg_default}" CACHE PATH "FFmpeg root containing include/ and lib/" FORCE)
endif()

set(ORANGE_TENSORRT_ROOT "" CACHE PATH "TensorRT root containing include/ and lib/")
if(NOT ORANGE_TENSORRT_ROOT)
    orange_pick_default_path(
        _tensorrt_default
        "/usr/local/TensorRT-10.0.1.6"
        "$ENV{HOME}/nvidia/TensorRT")
    set(ORANGE_TENSORRT_ROOT "${_tensorrt_default}" CACHE PATH "TensorRT root containing include/ and lib/" FORCE)
endif()

set(ORANGE_OPENCV_ROOT "" CACHE PATH "Optional OpenCV install root")
if(NOT ORANGE_OPENCV_ROOT)
    orange_pick_default_path(
        _opencv_default
        "${ORANGE_ROOT}/lib/opencv"
        "/usr/local")
    set(ORANGE_OPENCV_ROOT "${_opencv_default}" CACHE PATH "Optional OpenCV install root" FORCE)
endif()

foreach(required_dir
        ORANGE_EVT_ROOT
        ORANGE_CUDA_ROOT
        ORANGE_FFMPEG_ROOT
        ORANGE_TENSORRT_ROOT)
    if(NOT ${required_dir})
        message(FATAL_ERROR "${required_dir} is not set. Configure it with -D${required_dir}=...")
    endif()
endforeach()

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(DIR_IMGUI "${THIRD_PARTY_DIR}/imgui")
set(DIR_IMGUI_BACKEND "${THIRD_PARTY_DIR}/imgui/backends")
set(DIR_IMPLOT "${THIRD_PARTY_DIR}/implot")
set(DIR_FILEBROWSER "${THIRD_PARTY_DIR}/ImGuiFileDialog")
set(DIR_ICONFONT "${THIRD_PARTY_DIR}/IconFontCppHeaders")
set(DIR_FLATBUFFERS "${THIRD_PARTY_DIR}/flatbuffers")
set(DIR_NVENCODER "${SRC_DIR}/NvEncoder")
set(DIR_NVENC_API "${CMAKE_SOURCE_DIR}/nvenc_api")

if(NOT EXISTS "${DIR_FLATBUFFERS}/include/flatbuffers/flatbuffers.h")
    message(FATAL_ERROR
        "Missing third_party/flatbuffers. Run `git submodule update --init --recursive` "
        "from the repo root, or use ./build.sh so the dependency is bootstrapped automatically.")
endif()

set(OPENGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GLFW3 REQUIRED glfw3)

if(EXISTS "${ORANGE_OPENCV_ROOT}/lib/cmake/opencv4")
    set(OpenCV_DIR "${ORANGE_OPENCV_ROOT}/lib/cmake/opencv4")
elseif(EXISTS "${ORANGE_OPENCV_ROOT}/lib64/cmake/opencv4")
    set(OpenCV_DIR "${ORANGE_OPENCV_ROOT}/lib64/cmake/opencv4")
endif()
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio highgui video sfm)

find_path(ENET_INCLUDE_DIR enet/enet.h PATHS /usr/local/include /usr/include REQUIRED)
find_library(ENET_LIBRARY enet PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu REQUIRED)

find_path(GLEW_INCLUDE_DIR GL/glew.h PATHS /usr/local/include /usr/include REQUIRED)
find_library(GLEW_LIBRARY GLEW PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu REQUIRED)

find_path(EVT_INCLUDE_DIR EvtParamAttribute.h PATHS "${ORANGE_EVT_ROOT}/include" REQUIRED)
find_library(EVT_CAMERA_LIBRARY EmergentCamera PATHS "${ORANGE_EVT_ROOT}/lib" REQUIRED)
find_library(EVT_GENICAM_LIBRARY EmergentGenICam PATHS "${ORANGE_EVT_ROOT}/lib" REQUIRED)
find_library(EVT_GIGE_LIBRARY EmergentGigEVision PATHS "${ORANGE_EVT_ROOT}/lib" REQUIRED)

find_path(CUDA_INCLUDE_DIR cuda.h PATHS "${ORANGE_CUDA_ROOT}/include" REQUIRED)
find_library(CUDA_CUDART_LIBRARY cudart PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_DRIVER_LIBRARY cuda PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_NPPICC_LIBRARY nppicc PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_NPPIDEI_LIBRARY nppidei PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_NPPC_LIBRARY nppc PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_NPPIG_LIBRARY nppig PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(CUDA_NPPIAL_LIBRARY nppial PATHS "${ORANGE_CUDA_ROOT}/lib64" REQUIRED)
find_library(NVIDIA_ENCODE_LIBRARY nvidia-encode PATHS "${ORANGE_CUDA_ROOT}/lib64" /usr/lib /usr/lib/x86_64-linux-gnu REQUIRED)

if(ORANGE_ENABLE_NVTX)
    find_library(NVTX_LIBRARY nvToolsExt PATHS "${ORANGE_CUDA_ROOT}/lib64" /usr/lib /usr/lib/x86_64-linux-gnu)
    if(NOT NVTX_LIBRARY)
        message(FATAL_ERROR "ORANGE_ENABLE_NVTX=ON but nvToolsExt was not found. Install Nsight/CUDA NVTX libraries or disable ORANGE_ENABLE_NVTX.")
    endif()
endif()

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS
        "${ORANGE_FFMPEG_ROOT}/include"
        "${ORANGE_FFMPEG_ROOT}/build/include"
    REQUIRED)
find_library(AVFORMAT_LIBRARY avformat PATHS "${ORANGE_FFMPEG_ROOT}/lib" "${ORANGE_FFMPEG_ROOT}/build/lib" REQUIRED)
find_library(SWSCALE_LIBRARY swscale PATHS "${ORANGE_FFMPEG_ROOT}/lib" "${ORANGE_FFMPEG_ROOT}/build/lib" REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample PATHS "${ORANGE_FFMPEG_ROOT}/lib" "${ORANGE_FFMPEG_ROOT}/build/lib" REQUIRED)
find_library(AVUTIL_LIBRARY avutil PATHS "${ORANGE_FFMPEG_ROOT}/lib" "${ORANGE_FFMPEG_ROOT}/build/lib" REQUIRED)
find_library(AVCODEC_LIBRARY avcodec PATHS "${ORANGE_FFMPEG_ROOT}/lib" "${ORANGE_FFMPEG_ROOT}/build/lib" REQUIRED)

find_path(TENSORRT_INCLUDE_DIR NvInfer.h PATHS "${ORANGE_TENSORRT_ROOT}/include" REQUIRED)
find_library(TENSORRT_LIBRARY nvinfer PATHS "${ORANGE_TENSORRT_ROOT}/lib" REQUIRED)
find_library(TENSORRT_PLUGIN_LIBRARY nvinfer_plugin PATHS "${ORANGE_TENSORRT_ROOT}/lib" REQUIRED)

get_filename_component(EVT_LIBRARY_DIR "${EVT_CAMERA_LIBRARY}" DIRECTORY)
get_filename_component(CUDA_LIBRARY_DIR "${CUDA_CUDART_LIBRARY}" DIRECTORY)
get_filename_component(FFMPEG_LIBRARY_DIR "${AVFORMAT_LIBRARY}" DIRECTORY)
get_filename_component(TENSORRT_LIBRARY_DIR "${TENSORRT_LIBRARY}" DIRECTORY)

# Third-party / SDK include directories. These are attached to every project
# target as SYSTEM includes (via the orange_project_warnings interface library
# below), so -Wall/-Wextra do not report warnings originating in vendored or SDK
# headers. Only ${SRC_DIR} (project code) stays on the normal -I search path.
set(ORANGE_THIRD_PARTY_INCLUDE_DIRS
    "${DIR_IMGUI}"
    "${DIR_IMGUI_BACKEND}"
    "${DIR_IMPLOT}"
    "${DIR_FILEBROWSER}"
    "${DIR_ICONFONT}"
    "${DIR_NVENCODER}"
    "${DIR_NVENC_API}/include"
    "${DIR_FLATBUFFERS}/include"
    "${ENET_INCLUDE_DIR}"
    "${GLEW_INCLUDE_DIR}"
    "${EVT_INCLUDE_DIR}"
    "${CUDA_INCLUDE_DIR}"
    "${FFMPEG_INCLUDE_DIR}"
    "${TENSORRT_INCLUDE_DIR}"
    ${OpenCV_INCLUDE_DIRS}
    ${GLFW3_INCLUDE_DIRS}
)

set(ORANGE_COMMON_INCLUDE_DIRS
    "${SRC_DIR}"
)

file(GLOB NVENCODER_SOURCES "${DIR_NVENCODER}/*.cpp")

set(IMGUI_SOURCES
    "${DIR_IMGUI}/imgui.cpp"
    "${DIR_IMGUI}/imgui_demo.cpp"
    "${DIR_IMGUI}/imgui_draw.cpp"
    "${DIR_IMGUI}/imgui_tables.cpp"
    "${DIR_IMGUI}/imgui_widgets.cpp"
    "${DIR_IMGUI}/misc/cpp/imgui_stdlib.cpp"
    "${DIR_IMGUI_BACKEND}/imgui_impl_glfw.cpp"
    "${DIR_IMGUI_BACKEND}/imgui_impl_opengl3.cpp"
)

# Keep the vendored backend source untouched, but compile its main-window
# size queries through Orange's callback-updated cache.
set_property(
    SOURCE "${DIR_IMGUI_BACKEND}/imgui_impl_glfw.cpp"
    APPEND PROPERTY COMPILE_OPTIONS
    "-include" "${SRC_DIR}/imgui_glfw_size_cache_override.h"
)

set(IMPLOT_SOURCES
    "${DIR_IMPLOT}/implot.cpp"
    "${DIR_IMPLOT}/implot_items.cpp"
    "${DIR_IMPLOT}/implot_demo.cpp"
)

set(FILEDIALOG_SOURCES
    "${DIR_FILEBROWSER}/ImGuiFileDialog.cpp"
)

# Vendored third-party sources (imgui, implot, ImGuiFileDialog, NvEncoder) are
# compiled into project targets that carry -Wall/-Wextra. Silence warnings for
# these specific sources at the source level: source-scoped COMPILE_OPTIONS are
# emitted last on the command line, so -w overrides the target's warning flags
# for exactly these files while project code stays fully warned.
set(ORANGE_VENDORED_SOURCES
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${FILEDIALOG_SOURCES}
    ${NVENCODER_SOURCES}
)
set_property(
    SOURCE ${ORANGE_VENDORED_SOURCES}
    APPEND PROPERTY COMPILE_OPTIONS "-w")

set(ORANGE_CUDA_SOURCES
    "${SRC_DIR}/kernel.cu"
)
if(EXISTS "${SRC_DIR}/optimized_yolo_preprocess.cu")
    list(APPEND ORANGE_CUDA_SOURCES "${SRC_DIR}/optimized_yolo_preprocess.cu")
endif()

set(ORANGE_SOURCES
    "${SRC_DIR}/orange.cpp"
    "${SRC_DIR}/network_base.cpp"
    "${SRC_DIR}/video_container_finalization.cpp"
    "${SRC_DIR}/FFmpegWriter.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
    "${SRC_DIR}/calibration_transaction.cpp"
    "${SRC_DIR}/camera_preview_utils.cpp"
    "${SRC_DIR}/video_capture.cpp"
    "${SRC_DIR}/acquire_frames.cpp"
    "${SRC_DIR}/gui/async_startup_worker.cpp"
    "${SRC_DIR}/gui/bounded_startup_tasks.cpp"
    "${SRC_DIR}/gui/camera_startup_controller.cpp"
    "${SRC_DIR}/gui/per_camera_stream_runtime.cpp"
    "${SRC_DIR}/gui/texture_resources.cpp"
    "${SRC_DIR}/gui/startup_timing.cpp"
    "${SRC_DIR}/offthreadmachine.cpp"
    "${SRC_DIR}/opengldisplay.cpp"
    "${SRC_DIR}/imgui_glfw_size_cache.cpp"
    "${SRC_DIR}/yolov8_det.cpp"
    "${SRC_DIR}/yolo_worker.cpp"
    "${SRC_DIR}/yolo_event_log.cpp"
    "${SRC_DIR}/enet_thread.cpp"
    "${SRC_DIR}/project.cpp"
    "${SRC_DIR}/recording_validation.cpp"
    "${SRC_DIR}/recording_output_descriptor.cpp"
    "${SRC_DIR}/video_encode_profile.cpp"
    "${SRC_DIR}/recording_output_utils.cpp"
    "${SRC_DIR}/orange_local_control.cpp"
    "${SRC_DIR}/projected_center_preflight.cpp"
    "${SRC_DIR}/global.cpp"
    "${SRC_DIR}/image_writer_worker.cpp"
    "${SRC_DIR}/citrus_recording_geometry.cpp"
    "${SRC_DIR}/gui/autorun.cpp"
    "${SRC_DIR}/gui/env_util.cpp"
    "${SRC_DIR}/gui/guided_capture_autorun.cpp"
    "${SRC_DIR}/gui/guided_capture_autorun_config.cpp"
    "${SRC_DIR}/gui/arena_centering_analysis.cpp"
    "${SRC_DIR}/gui/daily_registration_geometry.cpp"
    "${SRC_DIR}/gui/projected_surface_scale_analysis.cpp"
    "${SRC_DIR}/gui/projected_surface_scale_artifact.cpp"
    "${SRC_DIR}/gui/arena_centering_autorun.cpp"
    "${SRC_DIR}/gui/arena_centering_autorun_config.cpp"
    "${SRC_DIR}/gui/camera_properties_panel.cpp"
    "${SRC_DIR}/gui/frame_ipc_panel.cpp"
    "${SRC_DIR}/gui/host_ptp_panel.cpp"
    "${SRC_DIR}/gui/incremental_clip_shadow.cpp"
    "${SRC_DIR}/gui/recording_finalizer.cpp"
    "${SRC_DIR}/gui/recording_panel.cpp"
    "${SRC_DIR}/gui/recording_snapshots.cpp"
    "${SRC_DIR}/gui/session_status.cpp"
    "${SRC_DIR}/gui/session_status_render.cpp"
    "${SRC_DIR}/gui/spatial_layout/calibration_metadata.cpp"
    "${SRC_DIR}/gui/spatial_layout/calibration_transaction_bridge.cpp"
    "${SRC_DIR}/gui/spatial_layout/canvas_edit.cpp"
    "${SRC_DIR}/gui/spatial_layout/capture_panel.cpp"
    "${SRC_DIR}/gui/spatial_layout/citrus_import.cpp"
    "${SRC_DIR}/gui/spatial_layout/citrus_template_workflow.cpp"
    "${SRC_DIR}/gui/spatial_layout/calibration_workflow.cpp"
    "${SRC_DIR}/gui/spatial_layout/daily_registration_preview.cpp"
    "${SRC_DIR}/gui/spatial_layout/daily_registration_runtime_selection.cpp"
    "${SRC_DIR}/gui/spatial_layout/daily_registration_workflow.cpp"
    "${SRC_DIR}/gui/spatial_layout/geometry.cpp"
    "${SRC_DIR}/gui/spatial_layout/group_capture_controller.cpp"
    "${SRC_DIR}/gui/spatial_layout/hough_panel.cpp"
    "${SRC_DIR}/gui/spatial_layout/layout_artifact_persistence.cpp"
    "${SRC_DIR}/gui/spatial_layout/layout_editor.cpp"
    "${SRC_DIR}/gui/spatial_layout/linked_top_rim_layout.cpp"
    "${SRC_DIR}/gui/spatial_layout/layout_state.cpp"
    "${SRC_DIR}/gui/spatial_layout/metadata_panel.cpp"
    "${SRC_DIR}/gui/spatial_layout/persistence_panel.cpp"
    "${SRC_DIR}/gui/spatial_layout/physical_target_bundle.cpp"
    "${SRC_DIR}/gui/spatial_layout/preflight.cpp"
    "${SRC_DIR}/gui/spatial_layout/preview_capture.cpp"
    "${SRC_DIR}/gui/spatial_layout/preview_overlay.cpp"
    "${SRC_DIR}/gui/spatial_layout/projection_snapshot_client.cpp"
    "${SRC_DIR}/gui/spatial_layout/runtime_builder.cpp"
    "${SRC_DIR}/gui/spatial_layout/save_job_preparation.cpp"
    "${SRC_DIR}/gui/spatial_layout/save_jobs.cpp"
    "${SRC_DIR}/gui/spatial_layout/session_capture_matrix.cpp"
    "${SRC_DIR}/gui/spatial_layout/session_review.cpp"
    "${SRC_DIR}/gui/spatial_layout/session_io.cpp"
    "${SRC_DIR}/aperture_characterization.cpp"
    "${SRC_DIR}/calibration_image_set.cpp"
    "${SRC_DIR}/dish_top_rim_observation.cpp"
    "${SRC_DIR}/ruler_alignment.cpp"
    "${SRC_DIR}/spatial_calibration_snapshot.cpp"
    "${SRC_DIR}/spatial_layout_schema.cpp"
    "${SRC_DIR}/spatial_snapshot_worker.cpp"
    "${SRC_DIR}/spatial_layout_ui.cpp"
    "${SRC_DIR}/usaf_resolution_calibration.cpp"
    "${SRC_DIR}/usaf_resolution_ui.cpp"
    "${SRC_DIR}/crop_and_encode_worker.cpp"
    "${SRC_DIR}/crop_preview_worker.cpp"
    "${SRC_DIR}/crop_producer.cpp"
    "${SRC_DIR}/crop_producer_worker.cpp"
    "${SRC_DIR}/pose_event_log.cpp"
    "${SRC_DIR}/pose_worker.cpp"
    "${SRC_DIR}/recording_ingress.cpp"
    "${SRC_DIR}/external_recorder_contract_utils.cpp"
    "${SRC_DIR}/external_recorder_lifecycle.cpp"
    "${SRC_DIR}/external_recorder_supervisor.cpp"
    "${SRC_DIR}/session/external_crop_recorder_config.cpp"
    "${SRC_DIR}/session/crop_rolling_sidecars.cpp"
    "${SRC_DIR}/session/recording_session.cpp"
    "${SRC_DIR}/shared_recording_output.cpp"
    "${SRC_DIR}/encoder_preprocess_worker.cpp"
    "${SRC_DIR}/encoder_hw_worker.cpp"
    "${SRC_DIR}/encoder_qp_map.cpp"
    "${SRC_DIR}/modern_recording_pipeline.cpp"
    "${SRC_DIR}/pre_encoder_reference_writer.cpp"
    ${NVENCODER_SOURCES}
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${FILEDIALOG_SOURCES}
    ${ORANGE_CUDA_SOURCES}
)

set(ORANGE_CLIENT_SOURCES
    "${SRC_DIR}/orange_headless_client.cpp"
    "${SRC_DIR}/network_base.cpp"
    "${SRC_DIR}/video_container_finalization.cpp"
    "${SRC_DIR}/FFmpegWriter.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
    "${SRC_DIR}/video_capture.cpp"
    "${SRC_DIR}/acquire_frames.cpp"
    "${SRC_DIR}/gui/startup_timing.cpp"
    "${SRC_DIR}/offthreadmachine.cpp"
    "${SRC_DIR}/recording_ingress.cpp"
    "${SRC_DIR}/external_recorder_contract_utils.cpp"
    "${SRC_DIR}/external_recorder_lifecycle.cpp"
    "${SRC_DIR}/external_recorder_supervisor.cpp"
    "${SRC_DIR}/session/external_crop_recorder_config.cpp"
    "${SRC_DIR}/session/crop_rolling_sidecars.cpp"
    "${SRC_DIR}/session/recording_session.cpp"
    "${SRC_DIR}/recording_output_descriptor.cpp"
    "${SRC_DIR}/video_encode_profile.cpp"
    "${SRC_DIR}/recording_output_utils.cpp"
    "${SRC_DIR}/shared_recording_output.cpp"
    "${SRC_DIR}/encoder_preprocess_worker.cpp"
    "${SRC_DIR}/encoder_hw_worker.cpp"
    "${SRC_DIR}/encoder_qp_map.cpp"
    "${SRC_DIR}/modern_recording_pipeline.cpp"
    "${SRC_DIR}/pre_encoder_reference_writer.cpp"
    "${SRC_DIR}/global.cpp"
    "${SRC_DIR}/project.cpp"
    "${SRC_DIR}/citrus_recording_geometry.cpp"
    "${SRC_DIR}/spatial_calibration_snapshot.cpp"
    "${SRC_DIR}/spatial_layout_schema.cpp"
    "${SRC_DIR}/spatial_snapshot_worker.cpp"
    "${SRC_DIR}/yolov8_det.cpp"
    "${SRC_DIR}/yolo_worker.cpp"
    "${SRC_DIR}/yolo_event_log.cpp"
    "${SRC_DIR}/yolo_event_log_validation.cpp"
    "${SRC_DIR}/pose_event_log_validation.cpp"
    "${SRC_DIR}/recording_validation.cpp"
    "${SRC_DIR}/crop_and_encode_worker.cpp"
    "${SRC_DIR}/crop_preview_worker.cpp"
    "${SRC_DIR}/crop_producer.cpp"
    "${SRC_DIR}/crop_producer_worker.cpp"
    "${SRC_DIR}/pose_event_log.cpp"
    "${SRC_DIR}/pose_worker.cpp"
    ${NVENCODER_SOURCES}
    ${ORANGE_CUDA_SOURCES}
)

set(YOLO_OFFLINE_SOURCES
    "benchmark/yolo_offline.cpp"
    "${SRC_DIR}/yolov8_det.cpp"
    ${ORANGE_CUDA_SOURCES}
)

set(EVT_LENS_PROBE_SOURCES
    "tools/evt_lens_probe.cpp"
    "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
)

set(EVT_APERTURE_CHARACTERIZE_SOURCES
    "tools/evt_aperture_characterize.cpp"
    "${SRC_DIR}/aperture_characterization.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
    "${SRC_DIR}/project.cpp"
    "${SRC_DIR}/spatial_calibration_snapshot.cpp"
    "${SRC_DIR}/spatial_layout_schema.cpp"
)

set(EVT_STREAM_SMOKE_SOURCES
    "tools/evt_stream_smoke.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
    "${SRC_DIR}/project.cpp"
    "${SRC_DIR}/spatial_calibration_snapshot.cpp"
    "${SRC_DIR}/spatial_layout_schema.cpp"
)

set(NVENC_STRESS_LOAD_SOURCES
    "tools/nvenc_stress_load.cpp"
    ${NVENCODER_SOURCES}
)

set(NV12_PREFILL_VALIDATION_SOURCES
    "tools/nv12_prefill_validation.cpp"
    ${NVENCODER_SOURCES}
)

set(EXTERNAL_RECORDER_IPC_PROBE_SOURCES
    "tools/external_recorder_ipc_probe.cpp"
    "${SRC_DIR}/video_container_finalization.cpp"
    "${SRC_DIR}/FFmpegWriter.cpp"
    "${SRC_DIR}/encoder_qp_map.cpp"
    "${SRC_DIR}/video_encode_profile.cpp"
    ${NVENCODER_SOURCES}
)

set(EXTERNAL_RECORDER_SUPERVISOR_PLAN_SOURCES
    "tools/external_recorder_supervisor_plan.cpp"
    "${SRC_DIR}/encoder_qp_map.cpp"
    "${SRC_DIR}/external_recorder_supervisor.cpp"
)

# Project-code warning flags. Scoped to C++ compilation only ($<COMPILE_LANGUAGE:CXX>)
# so nvcc device compilation of .cu sources is untouched. Attached to every project
# target below via link_libraries(); vendored sources opt out at the source level
# (see ORANGE_VENDORED_SOURCES above). -Werror is intentionally NOT enabled here.
add_library(orange_project_warnings INTERFACE)
target_compile_options(orange_project_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    # -Wmissing-field-initializers is suppressed intentionally: all ~278 hits are
    # the canonical NVENC idiom `NV_ENC_* params = { NV_ENC_*_VER };` (set the
    # version tag, zero-initialize the rest) plus a couple of designated-init
    # aggregates that deliberately leave trailing members zero. These are all
    # well-defined value-initialization, purely stylistic, and dominated the
    # warning output (100+ hits), so the whole class is disabled here rather than
    # littering every SDK struct with a full field list.
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>
)
target_include_directories(orange_project_warnings SYSTEM INTERFACE
    ${ORANGE_THIRD_PARTY_INCLUDE_DIRS}
)
link_libraries(orange_project_warnings)

add_executable(orange ${ORANGE_SOURCES})
add_executable(orange_client EXCLUDE_FROM_ALL ${ORANGE_CLIENT_SOURCES})
add_executable(yolo_offline EXCLUDE_FROM_ALL ${YOLO_OFFLINE_SOURCES})
add_executable(evt_lens_probe ${EVT_LENS_PROBE_SOURCES})
add_executable(evt_aperture_characterize ${EVT_APERTURE_CHARACTERIZE_SOURCES})
add_executable(evt_stream_smoke EXCLUDE_FROM_ALL ${EVT_STREAM_SMOKE_SOURCES})
add_executable(nvenc_stress_load EXCLUDE_FROM_ALL ${NVENC_STRESS_LOAD_SOURCES})
add_executable(nv12_prefill_validation EXCLUDE_FROM_ALL ${NV12_PREFILL_VALIDATION_SOURCES})
add_executable(external_recorder_ipc_probe EXCLUDE_FROM_ALL ${EXTERNAL_RECORDER_IPC_PROBE_SOURCES})
add_executable(external_recorder_supervisor_plan EXCLUDE_FROM_ALL ${EXTERNAL_RECORDER_SUPERVISOR_PLAN_SOURCES})

if(BUILD_TESTING)
    add_executable(recording_validation_tests
        "tools/recording_validation_tests.cpp"
        "${SRC_DIR}/recording_validation.cpp"
    )
    target_include_directories(recording_validation_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_validation_tests PRIVATE cxx_std_17)
    add_test(NAME recording_validation_tests COMMAND recording_validation_tests)

    add_executable(recording_output_descriptor_tests
        "tools/recording_output_descriptor_tests.cpp"
        "${SRC_DIR}/recording_output_descriptor.cpp"
    )
    target_include_directories(recording_output_descriptor_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_output_descriptor_tests PRIVATE cxx_std_17)
    add_test(NAME recording_output_descriptor_tests COMMAND recording_output_descriptor_tests)

    add_executable(crop_rolling_sidecars_tests
        "tools/crop_rolling_sidecars_tests.cpp"
        "${SRC_DIR}/session/crop_rolling_sidecars.cpp"
    )
    target_include_directories(crop_rolling_sidecars_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(crop_rolling_sidecars_tests PRIVATE cxx_std_17)
    add_test(NAME crop_rolling_sidecars_tests COMMAND crop_rolling_sidecars_tests)

    # Shadow-mode incremental clip splitter (stage 4). The worker core, the
    # announcement intake, the JSONL partial index, the finalize cross-check
    # comparator, and an end-to-end run of the real worker thread against
    # synthetic root CSVs. The lifecycle-facing wrappers only read plain
    # struct fields, so no external recorder sources are linked.
    add_executable(incremental_clip_shadow_tests
        "tools/incremental_clip_shadow_tests.cpp"
        "${SRC_DIR}/gui/incremental_clip_shadow.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
        "${SRC_DIR}/session/crop_rolling_sidecars.cpp"
    )
    target_include_directories(incremental_clip_shadow_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(incremental_clip_shadow_tests PRIVATE cxx_std_17)
    target_link_libraries(incremental_clip_shadow_tests PRIVATE Threads::Threads)
    add_test(NAME incremental_clip_shadow_tests COMMAND incremental_clip_shadow_tests)

    set(RECORDING_SESSION_MANIFEST_TEST_SOURCES ${ORANGE_CLIENT_SOURCES})
    list(REMOVE_ITEM
         RECORDING_SESSION_MANIFEST_TEST_SOURCES
         "${SRC_DIR}/orange_headless_client.cpp")
    add_executable(recording_session_manifest_tests EXCLUDE_FROM_ALL
        "tools/recording_session_manifest_tests.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(recording_session_manifest_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_session_manifest_tests PRIVATE cxx_std_17)
    add_test(
        NAME recording_session_manifest_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target recording_session_manifest_tests)
    add_test(NAME recording_session_manifest_tests COMMAND recording_session_manifest_tests)
    set_tests_properties(recording_session_manifest_tests
        PROPERTIES DEPENDS recording_session_manifest_tests_build)

    add_executable(recording_session_drain_tests EXCLUDE_FROM_ALL
        "tools/recording_session_drain_tests.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(recording_session_drain_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_session_drain_tests PRIVATE cxx_std_17)
    add_test(
        NAME recording_session_drain_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target recording_session_drain_tests)
    add_test(NAME recording_session_drain_tests COMMAND recording_session_drain_tests)
    set_tests_properties(recording_session_drain_tests
        PROPERTIES DEPENDS recording_session_drain_tests_build)

    # Locks down the phased recording-run start API (prepare / supervisor
    # start / complete / abort) used by the GUI async start path plus the
    # synchronous begin_recording_run wrapper; reuses the closed source set
    # already proven to link for the recording session manifest tests.
    add_executable(recording_session_phased_start_tests EXCLUDE_FROM_ALL
        "tools/recording_session_phased_start_tests.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(recording_session_phased_start_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_session_phased_start_tests PRIVATE cxx_std_17)
    add_test(
        NAME recording_session_phased_start_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target recording_session_phased_start_tests)
    add_test(NAME recording_session_phased_start_tests COMMAND recording_session_phased_start_tests)
    set_tests_properties(recording_session_phased_start_tests
        PROPERTIES DEPENDS recording_session_phased_start_tests_build)

    add_executable(recording_folder_reuse_regression_tests EXCLUDE_FROM_ALL
        "tools/recording_folder_reuse_regression_tests.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(recording_folder_reuse_regression_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_folder_reuse_regression_tests PRIVATE cxx_std_17)
    add_test(
        NAME recording_folder_reuse_regression_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target recording_folder_reuse_regression_tests)
    add_test(NAME recording_folder_reuse_regression_tests COMMAND recording_folder_reuse_regression_tests)
    set_tests_properties(recording_folder_reuse_regression_tests
        PROPERTIES DEPENDS recording_folder_reuse_regression_tests_build)

    # gui/recording_finalizer.cpp calls into the recording session, external
    # recorder, ingress, and crop encoder stacks, so reuse the closed source
    # set already proven to link for the recording session manifest tests.
    add_executable(recording_finalizer_tests EXCLUDE_FROM_ALL
        "tools/recording_finalizer_tests.cpp"
        "${SRC_DIR}/gui/recording_finalizer.cpp"
        "${SRC_DIR}/gui/incremental_clip_shadow.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(recording_finalizer_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_finalizer_tests PRIVATE cxx_std_17)
    add_test(
        NAME recording_finalizer_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target recording_finalizer_tests)
    add_test(NAME recording_finalizer_tests COMMAND recording_finalizer_tests)
    set_tests_properties(recording_finalizer_tests
        PROPERTIES DEPENDS recording_finalizer_tests_build)

    # gui/session_status.cpp needs has_gui_timepoint (gui/recording_finalizer.cpp),
    # the recording session drain path, and the external recorder lifecycle
    # stack, so reuse the closed source set already proven to link for the
    # recording session manifest tests. The ImGui renderers live in
    # gui/session_status_render.cpp (orange target only) so this test links
    # the pure builders without ImGui or display/worker objects.
    add_executable(session_status_tests EXCLUDE_FROM_ALL
        "tools/session_status_tests.cpp"
        "${SRC_DIR}/gui/session_status.cpp"
        "${SRC_DIR}/gui/recording_finalizer.cpp"
        "${SRC_DIR}/gui/incremental_clip_shadow.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
        ${RECORDING_SESSION_MANIFEST_TEST_SOURCES}
    )
    target_include_directories(session_status_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(session_status_tests PRIVATE cxx_std_17)
    add_test(
        NAME session_status_tests_build
        COMMAND ${CMAKE_COMMAND}
                --build "${CMAKE_BINARY_DIR}"
                --target session_status_tests)
    add_test(NAME session_status_tests COMMAND session_status_tests)
    set_tests_properties(session_status_tests
        PROPERTIES DEPENDS session_status_tests_build)

    add_executable(video_encode_profile_tests
        "tools/video_encode_profile_tests.cpp"
        "${SRC_DIR}/video_encode_profile.cpp"
    )
    target_include_directories(video_encode_profile_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(video_encode_profile_tests PRIVATE cxx_std_17)
    add_test(NAME video_encode_profile_tests COMMAND video_encode_profile_tests)

    add_executable(crop_preview_cadence_tests
        "tools/crop_preview_cadence_tests.cpp"
    )
    target_include_directories(crop_preview_cadence_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(crop_preview_cadence_tests PRIVATE cxx_std_17)
    add_test(NAME crop_preview_cadence_tests COMMAND crop_preview_cadence_tests)

    add_executable(gui_display_frame_rate_tests
        "tools/gui_display_frame_rate_tests.cpp"
    )
    target_include_directories(gui_display_frame_rate_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_display_frame_rate_tests PRIVATE cxx_std_17)
    add_test(NAME gui_display_frame_rate_tests COMMAND gui_display_frame_rate_tests)

    add_executable(threadworker_tests
        "tools/threadworker_tests.cpp"
        "${SRC_DIR}/offthreadmachine.cpp"
    )
    target_include_directories(threadworker_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(threadworker_tests PRIVATE cxx_std_17)
    target_link_libraries(threadworker_tests PRIVATE Threads::Threads)
    add_test(NAME threadworker_tests
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:threadworker_tests>)

    add_executable(threadworker_tsan_stress
        "tools/threadworker_tsan_stress.cpp"
        "${SRC_DIR}/offthreadmachine.cpp"
    )
    target_include_directories(threadworker_tsan_stress PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(threadworker_tsan_stress PRIVATE cxx_std_17)
    target_link_libraries(threadworker_tsan_stress PRIVATE Threads::Threads)
    add_test(NAME threadworker_tsan_stress
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:threadworker_tsan_stress>)

    add_executable(synthetic_pipeline_tests
        "tools/synthetic_pipeline_tests.cpp"
        "${SRC_DIR}/offthreadmachine.cpp"
    )
    target_include_directories(synthetic_pipeline_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(synthetic_pipeline_tests PRIVATE cxx_std_17)
    target_link_libraries(synthetic_pipeline_tests PRIVATE Threads::Threads)
    add_test(NAME synthetic_pipeline_tests
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:synthetic_pipeline_tests>)

    add_executable(worker_entry_ownership_core_host_tests
        "tools/worker_entry_ownership_core_host_tests.cpp"
    )
    target_include_directories(worker_entry_ownership_core_host_tests PRIVATE "${SRC_DIR}")
    target_compile_features(worker_entry_ownership_core_host_tests PRIVATE cxx_std_17)
    add_test(NAME worker_entry_ownership_core_host_tests
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:worker_entry_ownership_core_host_tests>)

    add_executable(worker_entry_ownership_tsan_hammer
        "tools/worker_entry_ownership_tsan_hammer.cpp"
    )
    target_include_directories(worker_entry_ownership_tsan_hammer PRIVATE "${SRC_DIR}")
    target_compile_features(worker_entry_ownership_tsan_hammer PRIVATE cxx_std_17)
    target_link_libraries(worker_entry_ownership_tsan_hammer PRIVATE Threads::Threads)
    add_test(NAME worker_entry_ownership_tsan_hammer
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:worker_entry_ownership_tsan_hammer>)

    add_executable(worker_entry_handoff_tsan_hammer
        "tools/worker_entry_handoff_tsan_hammer.cpp"
        "${SRC_DIR}/offthreadmachine.cpp"
    )
    target_include_directories(worker_entry_handoff_tsan_hammer PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(worker_entry_handoff_tsan_hammer PRIVATE cxx_std_17)
    target_link_libraries(worker_entry_handoff_tsan_hammer PRIVATE Threads::Threads)
    add_test(NAME worker_entry_handoff_tsan_hammer
        COMMAND ${ORANGE_TSAN_TEST_LAUNCHER} $<TARGET_FILE:worker_entry_handoff_tsan_hammer>)

    if(ORANGE_ENABLE_TSAN)
        # Host-only targets that exercise cross-thread code paths. CUDA and
        # SDK-linked targets are deliberately excluded: TSan cannot instrument
        # device code and uninstrumented libraries produce noise.
        foreach(_tsan_target
                threadworker_tests
                threadworker_tsan_stress
                synthetic_pipeline_tests
                worker_entry_ownership_core_host_tests
                worker_entry_ownership_tsan_hammer
                worker_entry_handoff_tsan_hammer)
            target_compile_options(${_tsan_target} PRIVATE -fsanitize=thread -g -O1)
            target_link_options(${_tsan_target} PRIVATE -fsanitize=thread)
        endforeach()
    endif()

    add_executable(worker_entry_release_tests
        "tools/worker_entry_release_tests.cpp"
    )
    target_include_directories(worker_entry_release_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(worker_entry_release_tests PRIVATE cxx_std_17)
    target_link_libraries(worker_entry_release_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY})
    add_test(NAME worker_entry_release_tests COMMAND worker_entry_release_tests)

    add_executable(external_crop_recorder_config_tests
        "tools/external_crop_recorder_config_tests.cpp"
        "${SRC_DIR}/session/external_crop_recorder_config.cpp"
    )
    target_include_directories(external_crop_recorder_config_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(external_crop_recorder_config_tests PRIVATE cxx_std_17)
    add_test(NAME external_crop_recorder_config_tests COMMAND external_crop_recorder_config_tests)

    add_executable(ffmpeg_writer_failure_tests
        "tools/ffmpeg_writer_failure_tests.cpp"
        "${SRC_DIR}/video_container_finalization.cpp"
        "${SRC_DIR}/FFmpegWriter.cpp"
    )
    target_include_directories(ffmpeg_writer_failure_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(ffmpeg_writer_failure_tests PRIVATE cxx_std_17)
    target_link_libraries(ffmpeg_writer_failure_tests PRIVATE
        ${AVFORMAT_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        pthread
    )
    set_target_properties(ffmpeg_writer_failure_tests PROPERTIES
        BUILD_RPATH "${FFMPEG_LIBRARY_DIR}")
    add_test(NAME ffmpeg_writer_failure_tests COMMAND ffmpeg_writer_failure_tests)

    add_test(
        NAME validate_gui_crop_preview_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/validate_gui_crop_preview_tests.py")

    add_test(
        NAME imgui_glfw_size_cache_wiring_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/imgui_glfw_size_cache_wiring_tests.py")

    add_test(
        NAME orange_gui_local_control_wiring_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/orange_gui_local_control_wiring_tests.py")

    add_test(
        NAME compare_gui_crop_preview_validation_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/compare_gui_crop_preview_validation_tests.py")

    add_test(
        NAME summarize_gui_validation_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/summarize_gui_validation_tests.py")

    add_test(
        NAME run_gui_aq_off_validation_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/run_gui_aq_off_validation_tests.py")

    add_test(
        NAME run_gui_fourcam_external_ipc_validation_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/run_gui_fourcam_external_ipc_validation_tests.py")

    add_test(
        NAME validate_gui_citrus_completion_recording_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/validate_gui_citrus_completion_recording_tests.py")

    add_test(
        NAME check_gui_citrus_completion_ready_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/check_gui_citrus_completion_ready_tests.py")

    add_test(
        NAME orange_citrus_orchestrator_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/orange_citrus_orchestrator_tests.py")

    add_test(
        NAME orange_citrus_fourcam_orchestrator_profile_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/orange_citrus_fourcam_orchestrator_profile_tests.py")

    add_test(
        NAME update_app_config_display_profile_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/update_app_config_display_profile_tests.py")

    add_test(
        NAME verify_external_recorder_session_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/verify_external_recorder_session_tests.py")

    add_test(
        NAME external_recorder_smoke_runner_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/external_recorder_smoke_runner_tests.py")

    add_test(
        NAME evt_stream_smoke_wrapper_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/evt_stream_smoke_wrapper_tests.py")

    add_executable(yolo_event_log_validation_tests
        "tools/yolo_event_log_validation_tests.cpp"
        "${SRC_DIR}/yolo_event_log_validation.cpp"
    )
    target_include_directories(yolo_event_log_validation_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(yolo_event_log_validation_tests PRIVATE cxx_std_17)
    add_test(NAME yolo_event_log_validation_tests COMMAND yolo_event_log_validation_tests)

    add_executable(yolo_spatial_mask_tests
        "tools/yolo_spatial_mask_tests.cpp"
    )
    target_include_directories(yolo_spatial_mask_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(yolo_spatial_mask_tests PRIVATE cxx_std_17)
    add_test(NAME yolo_spatial_mask_tests COMMAND yolo_spatial_mask_tests)

    add_executable(calibration_transaction_tests
        "tools/calibration_transaction_tests.cpp"
        "${SRC_DIR}/calibration_transaction.cpp"
        "${SRC_DIR}/gui/spatial_layout/calibration_transaction_bridge.cpp"
    )
    target_include_directories(calibration_transaction_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(calibration_transaction_tests PRIVATE cxx_std_17)
    add_test(NAME calibration_transaction_tests COMMAND calibration_transaction_tests)

    add_executable(camera_config_validation_tests
        "tools/camera_config_validation_tests.cpp"
    )
    target_include_directories(camera_config_validation_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(camera_config_validation_tests PRIVATE cxx_std_17)
    add_test(NAME camera_config_validation_tests COMMAND camera_config_validation_tests)

    add_executable(camera_sensor_pipeline_state_tests
        "tools/camera_sensor_pipeline_state_tests.cpp"
        "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
    )
    target_include_directories(camera_sensor_pipeline_state_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(camera_sensor_pipeline_state_tests PRIVATE cxx_std_17)
    target_link_libraries(camera_sensor_pipeline_state_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
    )
    add_test(NAME camera_sensor_pipeline_state_tests COMMAND camera_sensor_pipeline_state_tests)

    add_test(
        NAME sensor_baseline_characterization_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/sensor_baseline_characterization_tests.py")

    add_test(
        NAME make_guided_calibration_timing_config_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/make_guided_calibration_timing_config_tests.py")

    add_executable(app_storage_config_tests
        "tools/app_storage_config_tests.cpp"
        "${SRC_DIR}/project.cpp"
        "${SRC_DIR}/camera.cpp"
        "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
        "${SRC_DIR}/spatial_calibration_snapshot.cpp"
        "${SRC_DIR}/spatial_layout_schema.cpp"
    )
    target_include_directories(app_storage_config_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(app_storage_config_tests PRIVATE cxx_std_17)
    target_link_libraries(app_storage_config_tests PRIVATE
        ${ENET_LIBRARY}
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
    )
    add_test(NAME app_storage_config_tests COMMAND app_storage_config_tests)

    add_executable(recording_snapshots_tests
        "tools/recording_snapshots_tests.cpp"
        "${SRC_DIR}/citrus_recording_geometry.cpp"
        "${SRC_DIR}/gui/recording_snapshots.cpp"
        "${SRC_DIR}/gui/spatial_layout/projection_snapshot_client.cpp"
        "${SRC_DIR}/project.cpp"
        "${SRC_DIR}/camera.cpp"
        "${SRC_DIR}/camera_sensor_pipeline_state.cpp"
        "${SRC_DIR}/spatial_calibration_snapshot.cpp"
        "${SRC_DIR}/spatial_layout_schema.cpp"
    )
    target_include_directories(recording_snapshots_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(recording_snapshots_tests PRIVATE cxx_std_17)
    target_link_libraries(recording_snapshots_tests PRIVATE
        ${ENET_LIBRARY}
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
    )
    add_test(NAME recording_snapshots_tests COMMAND recording_snapshots_tests)

    add_test(
        NAME validate_recording_geometry_assets_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/validate_recording_geometry_assets_tests.py")

    add_test(
        NAME vbr_cq_quality_matrix_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/vbr_cq_quality_matrix_tests.py")

    add_test(
        NAME calibration_package_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/calibration_package_tests.py")

    add_executable(canvas_projection_geometry_identity_tests
        "tools/canvas_projection_geometry_identity_tests.cpp"
    )
    target_include_directories(canvas_projection_geometry_identity_tests PRIVATE
        ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(canvas_projection_geometry_identity_tests PRIVATE cxx_std_17)
    add_test(
        NAME canvas_projection_geometry_identity_tests
        COMMAND canvas_projection_geometry_identity_tests)

    add_executable(citrus_recording_geometry_tests
        "tools/citrus_recording_geometry_tests.cpp"
        "${SRC_DIR}/citrus_recording_geometry.cpp"
    )
    target_include_directories(citrus_recording_geometry_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(citrus_recording_geometry_tests PRIVATE cxx_std_17)
    add_test(NAME citrus_recording_geometry_tests COMMAND citrus_recording_geometry_tests)

    add_executable(gui_autorun_tests
        "tools/gui_autorun_tests.cpp"
        "${SRC_DIR}/gui/autorun.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
    )
    target_include_directories(gui_autorun_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_autorun_tests PRIVATE cxx_std_17)
    target_link_libraries(gui_autorun_tests PRIVATE Threads::Threads)
    add_test(NAME gui_autorun_tests COMMAND gui_autorun_tests)

    add_executable(gui_startup_timing_tests
        "tools/gui_startup_timing_tests.cpp"
        "${SRC_DIR}/gui/startup_timing.cpp"
    )
    target_include_directories(gui_startup_timing_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_startup_timing_tests PRIVATE cxx_std_17)
    target_link_libraries(gui_startup_timing_tests PRIVATE Threads::Threads)
    add_test(NAME gui_startup_timing_tests COMMAND gui_startup_timing_tests)

    add_executable(gui_async_startup_worker_tests
        "tools/gui_async_startup_worker_tests.cpp"
        "${SRC_DIR}/gui/async_startup_worker.cpp"
    )
    target_include_directories(gui_async_startup_worker_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_async_startup_worker_tests PRIVATE cxx_std_17)
    target_link_libraries(gui_async_startup_worker_tests PRIVATE Threads::Threads)
    add_test(NAME gui_async_startup_worker_tests COMMAND gui_async_startup_worker_tests)

    add_executable(gui_bounded_startup_tasks_tests
        "tools/gui_bounded_startup_tasks_tests.cpp"
        "${SRC_DIR}/gui/async_startup_worker.cpp"
        "${SRC_DIR}/gui/bounded_startup_tasks.cpp"
    )
    target_include_directories(gui_bounded_startup_tasks_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_bounded_startup_tasks_tests PRIVATE cxx_std_17)
    target_link_libraries(gui_bounded_startup_tasks_tests PRIVATE Threads::Threads)
    add_test(NAME gui_bounded_startup_tasks_tests COMMAND gui_bounded_startup_tasks_tests)

    add_executable(gui_startup_ownership_transaction_tests
        "tools/gui_startup_ownership_transaction_tests.cpp"
        "${SRC_DIR}/gui/async_startup_worker.cpp"
        "${SRC_DIR}/gui/bounded_startup_tasks.cpp"
    )
    target_include_directories(gui_startup_ownership_transaction_tests PRIVATE
        ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(gui_startup_ownership_transaction_tests PRIVATE cxx_std_17)
    target_link_libraries(gui_startup_ownership_transaction_tests PRIVATE
        Threads::Threads)
    add_test(
        NAME gui_startup_ownership_transaction_tests
        COMMAND gui_startup_ownership_transaction_tests)

    add_executable(per_camera_stream_runtime_contract_tests
        "tools/per_camera_stream_runtime_contract_tests.cpp"
    )
    target_include_directories(per_camera_stream_runtime_contract_tests PRIVATE
        ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(per_camera_stream_runtime_contract_tests PRIVATE cxx_std_17)
    add_test(
        NAME per_camera_stream_runtime_contract_tests
        COMMAND per_camera_stream_runtime_contract_tests)

    add_executable(guided_capture_autorun_config_tests
        "tools/guided_capture_autorun_config_tests.cpp"
        "${SRC_DIR}/gui/guided_capture_autorun_config.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
    )
    target_include_directories(guided_capture_autorun_config_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(guided_capture_autorun_config_tests PRIVATE cxx_std_17)
    add_test(NAME guided_capture_autorun_config_tests COMMAND guided_capture_autorun_config_tests)

    add_executable(arena_centering_analysis_tests
        "tools/arena_centering_analysis_tests.cpp"
        "${SRC_DIR}/gui/arena_centering_analysis.cpp"
    )
    target_include_directories(arena_centering_analysis_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(arena_centering_analysis_tests PRIVATE cxx_std_17)
    target_link_libraries(arena_centering_analysis_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    add_test(NAME arena_centering_analysis_tests COMMAND arena_centering_analysis_tests)

    add_executable(daily_registration_geometry_tests
        "tools/daily_registration_geometry_tests.cpp"
        "${SRC_DIR}/gui/daily_registration_geometry.cpp"
    )
    target_include_directories(daily_registration_geometry_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(daily_registration_geometry_tests PRIVATE cxx_std_17)
    add_test(NAME daily_registration_geometry_tests COMMAND daily_registration_geometry_tests)

    add_executable(daily_registration_preview_tests
        "tools/daily_registration_preview_tests.cpp"
        "${SRC_DIR}/calibration_transaction.cpp"
        "${SRC_DIR}/gui/daily_registration_geometry.cpp"
        "${SRC_DIR}/gui/spatial_layout/daily_registration_preview.cpp"
    )
    target_include_directories(daily_registration_preview_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(daily_registration_preview_tests PRIVATE cxx_std_17)
    add_test(NAME daily_registration_preview_tests COMMAND daily_registration_preview_tests)

    add_executable(daily_registration_runtime_selection_tests
        "tools/daily_registration_runtime_selection_tests.cpp"
        "${SRC_DIR}/gui/spatial_layout/daily_registration_runtime_selection.cpp"
    )
    target_include_directories(daily_registration_runtime_selection_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(daily_registration_runtime_selection_tests PRIVATE cxx_std_17)
    add_test(
        NAME daily_registration_runtime_selection_tests
        COMMAND daily_registration_runtime_selection_tests)

    add_executable(projected_surface_scale_analysis_tests
        "tools/projected_surface_scale_analysis_tests.cpp"
        "${SRC_DIR}/gui/projected_surface_scale_analysis.cpp"
    )
    target_include_directories(projected_surface_scale_analysis_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(projected_surface_scale_analysis_tests PRIVATE cxx_std_17)
    target_link_libraries(projected_surface_scale_analysis_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME projected_surface_scale_analysis_tests COMMAND projected_surface_scale_analysis_tests)

    add_executable(arena_centering_detect_images
        "tools/arena_centering_detect_images.cpp"
        "${SRC_DIR}/gui/arena_centering_analysis.cpp"
    )
    target_include_directories(arena_centering_detect_images PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(arena_centering_detect_images PRIVATE cxx_std_17)
    target_link_libraries(arena_centering_detect_images PRIVATE ${OpenCV_LIBS} Threads::Threads)

    add_executable(arena_centering_autorun_config_tests
        "tools/arena_centering_autorun_config_tests.cpp"
        "${SRC_DIR}/gui/arena_centering_autorun_config.cpp"
        "${SRC_DIR}/gui/env_util.cpp"
    )
    target_include_directories(arena_centering_autorun_config_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(arena_centering_autorun_config_tests PRIVATE cxx_std_17)
    add_test(NAME arena_centering_autorun_config_tests COMMAND arena_centering_autorun_config_tests)
    add_test(
        NAME validate_gui_guided_capture_result_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/validate_gui_guided_capture_result_tests.py"
    )

    add_executable(spatial_calibration_snapshot_tests
        "tools/spatial_calibration_snapshot_tests.cpp"
        "${SRC_DIR}/spatial_calibration_snapshot.cpp"
        "${SRC_DIR}/spatial_layout_schema.cpp"
    )
    target_include_directories(spatial_calibration_snapshot_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(spatial_calibration_snapshot_tests PRIVATE cxx_std_17)
    add_test(NAME spatial_calibration_snapshot_tests COMMAND spatial_calibration_snapshot_tests)

    add_executable(dish_top_rim_observation_tests
        "tools/dish_top_rim_observation_tests.cpp"
        "${SRC_DIR}/calibration_image_set.cpp"
        "${SRC_DIR}/dish_top_rim_observation.cpp"
        "${SRC_DIR}/spatial_layout_schema.cpp"
    )
    target_include_directories(dish_top_rim_observation_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(dish_top_rim_observation_tests PRIVATE cxx_std_17)
    target_link_libraries(dish_top_rim_observation_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME dish_top_rim_observation_tests COMMAND dish_top_rim_observation_tests)

    add_executable(ruler_alignment_tests
        "tools/ruler_alignment_tests.cpp"
        "${SRC_DIR}/ruler_alignment.cpp"
    )
    target_include_directories(ruler_alignment_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(ruler_alignment_tests PRIVATE cxx_std_17)
    target_link_libraries(ruler_alignment_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME ruler_alignment_tests COMMAND ruler_alignment_tests)

    add_executable(calibration_image_set_tests
        "tools/calibration_image_set_tests.cpp"
        "${SRC_DIR}/calibration_image_set.cpp"
    )
    target_include_directories(calibration_image_set_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(calibration_image_set_tests PRIVATE cxx_std_17)
    add_test(NAME calibration_image_set_tests COMMAND calibration_image_set_tests)

    add_executable(fnv1a64_fingerprint_tests
        "tools/fnv1a64_fingerprint_tests.cpp"
    )
    target_include_directories(fnv1a64_fingerprint_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(fnv1a64_fingerprint_tests PRIVATE cxx_std_17)
    add_test(NAME fnv1a64_fingerprint_tests COMMAND fnv1a64_fingerprint_tests)

    add_executable(pose_event_log_tests
        "tools/pose_event_log_tests.cpp"
        "${SRC_DIR}/pose_event_log.cpp"
    )
    target_include_directories(pose_event_log_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(pose_event_log_tests PRIVATE cxx_std_17)
    add_test(NAME pose_event_log_tests COMMAND pose_event_log_tests)

    add_executable(pose_event_log_validation_tests
        "tools/pose_event_log_validation_tests.cpp"
        "${SRC_DIR}/pose_event_log_validation.cpp"
        "${SRC_DIR}/yolo_event_log_validation.cpp"
    )
    target_include_directories(pose_event_log_validation_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(pose_event_log_validation_tests PRIVATE cxx_std_17)
    add_test(NAME pose_event_log_validation_tests COMMAND pose_event_log_validation_tests)

    add_executable(shaman_v2_tests
        "tools/shaman_v2_tests.cpp"
    )
    target_include_directories(shaman_v2_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(shaman_v2_tests PRIVATE cxx_std_17)
    add_test(NAME shaman_v2_tests COMMAND shaman_v2_tests)

    add_executable(encoder_qp_map_tests
        "tools/encoder_qp_map_tests.cpp"
        "${SRC_DIR}/encoder_qp_map.cpp"
    )
    target_include_directories(encoder_qp_map_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(encoder_qp_map_tests PRIVATE cxx_std_17)
    add_test(NAME encoder_qp_map_tests COMMAND encoder_qp_map_tests)

    add_executable(external_recorder_supervisor_tests
        "tools/external_recorder_supervisor_tests.cpp"
        "${SRC_DIR}/encoder_qp_map.cpp"
        "${SRC_DIR}/external_recorder_contract_utils.cpp"
        "${SRC_DIR}/external_recorder_lifecycle.cpp"
        "${SRC_DIR}/external_recorder_supervisor.cpp"
    )
    target_include_directories(external_recorder_supervisor_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(external_recorder_supervisor_tests PRIVATE cxx_std_17)
    add_test(NAME external_recorder_supervisor_tests COMMAND external_recorder_supervisor_tests)

    add_executable(external_recorder_contract_utils_tests
        "tools/external_recorder_contract_utils_tests.cpp"
        "${SRC_DIR}/encoder_qp_map.cpp"
        "${SRC_DIR}/external_recorder_contract_utils.cpp"
        "${SRC_DIR}/external_recorder_supervisor.cpp"
    )
    target_include_directories(external_recorder_contract_utils_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(external_recorder_contract_utils_tests PRIVATE cxx_std_17)
    add_test(NAME external_recorder_contract_utils_tests COMMAND external_recorder_contract_utils_tests)

    add_executable(external_recorder_supervisor_socket_stub
        "tools/external_recorder_supervisor_socket_stub.cpp"
    )
    target_include_directories(external_recorder_supervisor_socket_stub PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(external_recorder_supervisor_socket_stub PRIVATE cxx_std_17)
    add_dependencies(external_recorder_supervisor_tests external_recorder_supervisor_socket_stub)
    if(TARGET recording_session_phased_start_tests)
        add_dependencies(recording_session_phased_start_tests external_recorder_supervisor_socket_stub)
    endif()

    add_executable(orange_local_control_tests
        "tools/orange_local_control_tests.cpp"
        "${SRC_DIR}/orange_local_control.cpp"
        "${SRC_DIR}/projected_center_preflight.cpp"
    )
    target_include_directories(orange_local_control_tests PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(orange_local_control_tests PRIVATE cxx_std_17)
    target_link_libraries(orange_local_control_tests PRIVATE Threads::Threads)
    add_test(NAME orange_local_control_tests COMMAND orange_local_control_tests)

    add_test(
        NAME orange_local_control_client_tests
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/orange_local_control_client_tests.py")
endif()

function(orange_configure_target target_name)
    target_include_directories(${target_name} PRIVATE ${ORANGE_COMMON_INCLUDE_DIRS})
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>,$<BOOL:${ORANGE_USE_FAST_MATH}>>:-Ofast;-ffast-math>
        $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:-G;-g;-O0>
    )
    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:DEBUG>
        $<$<BOOL:${ORANGE_ENABLE_NVTX}>:ENABLE_NVTX_PROFILING>
        $<$<BOOL:${ORANGE_ENABLE_YOLO_PROFILE}>:YOLO_PROFILE=1>
        $<$<BOOL:${ORANGE_ENABLE_CUDA_DEBUG}>:DEBUG_CUDA_CONTEXT>
        $<$<BOOL:${ORANGE_ENABLE_CUDA_DEBUG}>:ENABLE_CUDA_DEBUG_LOGGING>
    )
    set_target_properties(${target_name} PROPERTIES
        BUILD_RPATH "${EVT_LIBRARY_DIR};${CUDA_LIBRARY_DIR};${FFMPEG_LIBRARY_DIR};${TENSORRT_LIBRARY_DIR}"
    )
endfunction()

orange_configure_target(orange)
orange_configure_target(orange_client)
orange_configure_target(yolo_offline)
orange_configure_target(evt_lens_probe)
orange_configure_target(evt_aperture_characterize)
orange_configure_target(evt_stream_smoke)
orange_configure_target(nvenc_stress_load)
orange_configure_target(nv12_prefill_validation)
orange_configure_target(external_recorder_ipc_probe)
orange_configure_target(external_recorder_supervisor_plan)
if(TARGET recording_session_manifest_tests)
    orange_configure_target(recording_session_manifest_tests)
endif()
if(TARGET recording_finalizer_tests)
    orange_configure_target(recording_finalizer_tests)
endif()
if(TARGET session_status_tests)
    orange_configure_target(session_status_tests)
endif()

target_link_libraries(orange PRIVATE
    ${ENET_LIBRARY}
    ${GLEW_LIBRARY}
    OpenGL::GL
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${CUDA_NPPICC_LIBRARY}
    ${CUDA_NPPIDEI_LIBRARY}
    ${CUDA_NPPC_LIBRARY}
    ${CUDA_NPPIG_LIBRARY}
    ${CUDA_NPPIAL_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    ${EVT_CAMERA_LIBRARY}
    ${EVT_GENICAM_LIBRARY}
    ${EVT_GIGE_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${OpenCV_LIBS}
    ${TENSORRT_LIBRARY}
    ${TENSORRT_PLUGIN_LIBRARY}
    ${GLFW3_LIBRARIES}
    m
    pthread
)
if(ORANGE_ENABLE_NVTX)
    target_link_libraries(orange PRIVATE ${NVTX_LIBRARY})
endif()

target_link_libraries(orange_client PRIVATE
    ${EVT_CAMERA_LIBRARY}
    ${EVT_GENICAM_LIBRARY}
    ${EVT_GIGE_LIBRARY}
    ${ENET_LIBRARY}
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${CUDA_NPPICC_LIBRARY}
    ${CUDA_NPPIDEI_LIBRARY}
    ${CUDA_NPPC_LIBRARY}
    ${CUDA_NPPIG_LIBRARY}
    ${CUDA_NPPIAL_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${OpenCV_LIBS}
    ${TENSORRT_LIBRARY}
    ${TENSORRT_PLUGIN_LIBRARY}
    m
    pthread
)
if(ORANGE_ENABLE_NVTX)
    target_link_libraries(orange_client PRIVATE ${NVTX_LIBRARY})
endif()

if(TARGET recording_session_manifest_tests)
    target_link_libraries(recording_session_manifest_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(recording_session_manifest_tests PRIVATE ${NVTX_LIBRARY})
    endif()
endif()

if(TARGET recording_finalizer_tests)
    target_link_libraries(recording_finalizer_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(recording_finalizer_tests PRIVATE ${NVTX_LIBRARY})
    endif()
endif()

if(TARGET session_status_tests)
    target_link_libraries(session_status_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(session_status_tests PRIVATE ${NVTX_LIBRARY})
    endif()
endif()

if(TARGET recording_session_drain_tests)
    target_link_libraries(recording_session_drain_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(recording_session_drain_tests PRIVATE ${NVTX_LIBRARY})
    endif()
endif()

if(TARGET recording_session_phased_start_tests)
    target_link_libraries(recording_session_phased_start_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(recording_session_phased_start_tests PRIVATE ${NVTX_LIBRARY})
    endif()

    target_link_libraries(recording_folder_reuse_regression_tests PRIVATE
        ${EVT_CAMERA_LIBRARY}
        ${EVT_GENICAM_LIBRARY}
        ${EVT_GIGE_LIBRARY}
        ${ENET_LIBRARY}
        ${CUDA_CUDART_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
        ${CUDA_NPPICC_LIBRARY}
        ${CUDA_NPPIDEI_LIBRARY}
        ${CUDA_NPPC_LIBRARY}
        ${CUDA_NPPIG_LIBRARY}
        ${CUDA_NPPIAL_LIBRARY}
        ${NVIDIA_ENCODE_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${OpenCV_LIBS}
        ${TENSORRT_LIBRARY}
        ${TENSORRT_PLUGIN_LIBRARY}
        m
        pthread
    )
    if(ORANGE_ENABLE_NVTX)
        target_link_libraries(recording_folder_reuse_regression_tests PRIVATE ${NVTX_LIBRARY})
    endif()
endif()

target_link_libraries(yolo_offline PRIVATE
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${CUDA_NPPICC_LIBRARY}
    ${CUDA_NPPIDEI_LIBRARY}
    ${CUDA_NPPC_LIBRARY}
    ${CUDA_NPPIG_LIBRARY}
    ${CUDA_NPPIAL_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    ${OpenCV_LIBS}
    ${TENSORRT_LIBRARY}
    ${TENSORRT_PLUGIN_LIBRARY}
)
if(ORANGE_ENABLE_NVTX)
    target_link_libraries(yolo_offline PRIVATE ${NVTX_LIBRARY})
endif()

target_link_libraries(evt_lens_probe PRIVATE
    ${EVT_CAMERA_LIBRARY}
    ${EVT_GENICAM_LIBRARY}
    ${EVT_GIGE_LIBRARY}
    m
    pthread
)

target_link_libraries(evt_aperture_characterize PRIVATE
    ${EVT_CAMERA_LIBRARY}
    ${EVT_GENICAM_LIBRARY}
    ${EVT_GIGE_LIBRARY}
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${ENET_LIBRARY}
    m
    pthread
)

target_link_libraries(evt_stream_smoke PRIVATE
    ${EVT_CAMERA_LIBRARY}
    ${EVT_GENICAM_LIBRARY}
    ${EVT_GIGE_LIBRARY}
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${ENET_LIBRARY}
    m
    pthread
)

target_link_libraries(nvenc_stress_load PRIVATE
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    m
    pthread
)

target_link_libraries(nv12_prefill_validation PRIVATE
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    m
    pthread
)

target_link_libraries(external_recorder_ipc_probe PRIVATE
    ${CUDA_CUDART_LIBRARY}
    ${CUDA_DRIVER_LIBRARY}
    ${NVIDIA_ENCODE_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${AVCODEC_LIBRARY}
    m
    pthread
)

message(STATUS "orange build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "orange CUDA arch: ${CMAKE_CUDA_ARCHITECTURES}")
message(STATUS "orange NVTX: ${ORANGE_ENABLE_NVTX}")
message(STATUS "orange YOLO profile: ${ORANGE_ENABLE_YOLO_PROFILE}")
message(STATUS "orange CUDA debug: ${ORANGE_ENABLE_CUDA_DEBUG}")
message(STATUS "orange fast math: ${ORANGE_USE_FAST_MATH}")
message(STATUS "orange EVT root: ${ORANGE_EVT_ROOT}")
message(STATUS "orange CUDA root: ${ORANGE_CUDA_ROOT}")
message(STATUS "orange FFmpeg root: ${ORANGE_FFMPEG_ROOT}")
message(STATUS "orange TensorRT root: ${ORANGE_TENSORRT_ROOT}")
message(STATUS "orange OpenCV root: ${ORANGE_OPENCV_ROOT}")
