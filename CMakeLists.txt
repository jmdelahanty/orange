cmake_minimum_required(VERSION 3.19)
project(orange LANGUAGES CXX CUDA)

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
set(DIR_NVENCODER "${SRC_DIR}/NvEncoder")
set(DIR_NVENC_API "${CMAKE_SOURCE_DIR}/nvenc_api")

set(OPENGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
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

set(ORANGE_COMMON_INCLUDE_DIRS
    "${SRC_DIR}"
    "${DIR_IMGUI}"
    "${DIR_IMGUI_BACKEND}"
    "${DIR_IMPLOT}"
    "${DIR_FILEBROWSER}"
    "${DIR_ICONFONT}"
    "${DIR_NVENCODER}"
    "${DIR_NVENC_API}/include"
    "${THIRD_PARTY_DIR}/flatbuffers/include"
    "${ENET_INCLUDE_DIR}"
    "${GLEW_INCLUDE_DIR}"
    "${EVT_INCLUDE_DIR}"
    "${CUDA_INCLUDE_DIR}"
    "${FFMPEG_INCLUDE_DIR}"
    "${TENSORRT_INCLUDE_DIR}"
    ${OpenCV_INCLUDE_DIRS}
    ${GLFW3_INCLUDE_DIRS}
)

file(GLOB NVENCODER_SOURCES "${DIR_NVENCODER}/*.cpp")

set(IMGUI_SOURCES
    "${DIR_IMGUI}/imgui.cpp"
    "${DIR_IMGUI}/imgui_demo.cpp"
    "${DIR_IMGUI}/imgui_draw.cpp"
    "${DIR_IMGUI}/imgui_tables.cpp"
    "${DIR_IMGUI}/imgui_widgets.cpp"
    "${DIR_IMGUI_BACKEND}/imgui_impl_glfw.cpp"
    "${DIR_IMGUI_BACKEND}/imgui_impl_opengl3.cpp"
)

set(IMPLOT_SOURCES
    "${DIR_IMPLOT}/implot.cpp"
    "${DIR_IMPLOT}/implot_items.cpp"
    "${DIR_IMPLOT}/implot_demo.cpp"
)

set(FILEDIALOG_SOURCES
    "${DIR_FILEBROWSER}/ImGuiFileDialog.cpp"
)

set(ORANGE_CUDA_SOURCES
    "${SRC_DIR}/kernel.cu"
)
if(EXISTS "${SRC_DIR}/optimized_yolo_preprocess.cu")
    list(APPEND ORANGE_CUDA_SOURCES "${SRC_DIR}/optimized_yolo_preprocess.cu")
endif()

set(ORANGE_SOURCES
    "${SRC_DIR}/orange.cpp"
    "${SRC_DIR}/network_base.cpp"
    "${SRC_DIR}/FFmpegWriter.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/video_capture.cpp"
    "${SRC_DIR}/acquire_frames.cpp"
    "${SRC_DIR}/offthreadmachine.cpp"
    "${SRC_DIR}/opengldisplay.cpp"
    "${SRC_DIR}/gpu_video_encoder.cpp"
    "${SRC_DIR}/yolov8_det.cpp"
    "${SRC_DIR}/yolo_worker.cpp"
    "${SRC_DIR}/enet_thread.cpp"
    "${SRC_DIR}/project.cpp"
    "${SRC_DIR}/global.cpp"
    "${SRC_DIR}/image_writer_worker.cpp"
    "${SRC_DIR}/aperture_characterization.cpp"
    "${SRC_DIR}/crop_and_encode_worker.cpp"
    "${SRC_DIR}/encoder_preprocess_worker.cpp"
    "${SRC_DIR}/encoder_hw_worker.cpp"
    ${NVENCODER_SOURCES}
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${FILEDIALOG_SOURCES}
    ${ORANGE_CUDA_SOURCES}
)

set(ORANGE_CLIENT_SOURCES
    "${SRC_DIR}/orange_headless_client.cpp"
    "${SRC_DIR}/network_base.cpp"
    "${SRC_DIR}/FFmpegWriter.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/video_capture.cpp"
    "${SRC_DIR}/offthreadmachine.cpp"
    "${SRC_DIR}/acquire_frames_headless.cpp"
    "${SRC_DIR}/gpu_video_encoder.cpp"
    "${SRC_DIR}/global.cpp"
    "${SRC_DIR}/project.cpp"
    ${NVENCODER_SOURCES}
    "${SRC_DIR}/kernel.cu"
)

set(YOLO_OFFLINE_SOURCES
    "benchmark/yolo_offline.cpp"
    "${SRC_DIR}/yolov8_det.cpp"
    ${ORANGE_CUDA_SOURCES}
)

set(EVT_LENS_PROBE_SOURCES
    "tools/evt_lens_probe.cpp"
)

set(EVT_APERTURE_CHARACTERIZE_SOURCES
    "tools/evt_aperture_characterize.cpp"
    "${SRC_DIR}/aperture_characterization.cpp"
    "${SRC_DIR}/camera.cpp"
    "${SRC_DIR}/project.cpp"
)

add_executable(orange ${ORANGE_SOURCES})
add_executable(orange_client EXCLUDE_FROM_ALL ${ORANGE_CLIENT_SOURCES})
add_executable(yolo_offline EXCLUDE_FROM_ALL ${YOLO_OFFLINE_SOURCES})
add_executable(evt_lens_probe ${EVT_LENS_PROBE_SOURCES})
add_executable(evt_aperture_characterize ${EVT_APERTURE_CHARACTERIZE_SOURCES})

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
    ${NVIDIA_ENCODE_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${AVCODEC_LIBRARY}
    m
    pthread
)
if(ORANGE_ENABLE_NVTX)
    target_link_libraries(orange_client PRIVATE ${NVTX_LIBRARY})
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
