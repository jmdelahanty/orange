cmake_minimum_required(VERSION 3.20)
project(nvenc_native_nv12_probe LANGUAGES CXX)

set(ORANGE_SOURCE_ROOT "" CACHE PATH "Orange checkout containing src/NvEncoder")
set(CUDA_13_ROOT "" CACHE PATH "Isolated CUDA 13.1+ toolkit root")
set(NVENC_13_INTERFACE_DIR "" CACHE PATH "Official Video Codec Interface 13.1 Interface directory")

foreach(required ORANGE_SOURCE_ROOT CUDA_13_ROOT NVENC_13_INTERFACE_DIR)
    if(NOT IS_DIRECTORY "${${required}}")
        message(FATAL_ERROR "${required} must name an existing directory")
    endif()
endforeach()

if(NOT EXISTS "${CUDA_13_ROOT}/include/cuda.h")
    message(FATAL_ERROR "CUDA_13_ROOT does not contain include/cuda.h")
endif()
if(NOT EXISTS "${NVENC_13_INTERFACE_DIR}/nvEncodeAPI.h")
    message(FATAL_ERROR "NVENC_13_INTERFACE_DIR does not contain nvEncodeAPI.h")
endif()

find_library(CUDA_DRIVER_LIBRARY
    NAMES cuda libcuda.so.1
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib64
    REQUIRED)
find_library(NVIDIA_ENCODE_LIBRARY
    NAMES nvidia-encode libnvidia-encode.so.1
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib64
    REQUIRED)

# Private compatibility transformations are pinned to the reviewed source shape.
# Fail configuration if a future wrapper change makes a migration a no-op.
function(checked_replace old_text new_text variable source_text)
    string(FIND "${source_text}" "${old_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "API 13 adaptation no longer matches ${variable}: ${old_text}")
    endif()
    string(REPLACE "${old_text}" "${new_text}" replaced "${source_text}")
    set(${variable} "${replaced}" PARENT_SCOPE)
endfunction()

# The checkout's wrapper predates API 13.1. Generate a private build-tree copy
# with API-13.1 field migrations and mapped-format/pitch propagation. The tracked wrapper
# and all production targets remain byte-for-byte untouched.
file(READ "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoder.cpp" NVENCODER13_SOURCE)
checked_replace(
    "pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? 2 : 0;"
    "pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;\n        pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.outputBitDepth =\n            pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth;"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
checked_replace(
    "pEncoderParams->encodeConfig->encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 != 2"
    "pEncoderParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth != NV_ENC_BIT_DEPTH_10"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
checked_replace(
    "registerResource.pOutputFencePoint = pOutputFencePoint;"
    "(void)pOutputFencePoint;"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
checked_replace(
    "NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams));"
    "{ NVENCSTATUS initStatus = m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams); if (initStatus != NV_ENC_SUCCESS) { const char* detail = m_nvenc.nvEncGetLastErrorString ? m_nvenc.nvEncGetLastErrorString(m_hEncoder) : nullptr; NVENC_THROW_ERROR(std::string(\"nvEncInitializeEncoder returned \" ) + std::to_string(initStatus) + (detail ? std::string(\": \") + detail : std::string()), initStatus); } }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
checked_replace(
    "NVENC_API_CALL(m_nvenc.nvEncLockBitstream(m_hEncoder, &lockBitstreamData));"
    "{ NVENCSTATUS lockStatus = m_nvenc.nvEncLockBitstream(m_hEncoder, &lockBitstreamData); if (lockStatus != NV_ENC_SUCCESS) { const char* detail = m_nvenc.nvEncGetLastErrorString ? m_nvenc.nvEncGetLastErrorString(m_hEncoder) : nullptr; NVENC_THROW_ERROR(std::string(\"nvEncLockBitstream returned \" ) + std::to_string(lockStatus) + (detail ? std::string(\": \") + detail : std::string()), lockStatus); } }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
# API 13.1 returns the format of the mapped resource.  Carry it through to
# NV_ENC_PIC_PARAMS, matching FFmpeg's current CUARRAY path instead of assuming
# that the constructor's requested format describes every mapped resource.
checked_replace(
    "m_vMappedInputBuffers[bfrIdx] = mapInputResource.mappedResource;"
    "m_vMappedInputBuffers[bfrIdx] = mapInputResource.mappedResource;\n    m_eBufferFormat = mapInputResource.mappedBufferFmt;\n    if (m_iToSend == 0) { std::cout << \"nvenc_mapped_buffer_format=\" << static_cast<uint32_t>(mapInputResource.mappedBufferFmt) << std::endl; }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
# Supply a fallback only for callers without an explicit pitch. Native callers
# set the validated 4608-byte pitch; this is not an inferred opaque array stride.
checked_replace(
    "picParams.inputHeight = GetEncodeHeight();"
    "picParams.inputHeight = GetEncodeHeight();\n    picParams.inputPitch = picParams.inputPitch ? picParams.inputPitch : GetEncodeWidth();"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/NvEncoder13.cpp" "${NVENCODER13_SOURCE}")

add_executable(nvenc_native_nv12_probe
    nvenc_native_nv12_probe.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/NvEncoder13.cpp"
    "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoderCuda.cpp")

target_compile_features(nvenc_native_nv12_probe PRIVATE cxx_std_17)
target_compile_options(nvenc_native_nv12_probe PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_include_directories(nvenc_native_nv12_probe PRIVATE
    "${ORANGE_SOURCE_ROOT}/src"
    "${ORANGE_SOURCE_ROOT}/src/NvEncoder")
# Order matters: compile NvEncoder wrappers against the pinned 13.1 interface,
# and compile all CUDA calls against the isolated 13.1 toolkit headers.
target_include_directories(nvenc_native_nv12_probe SYSTEM BEFORE PRIVATE
    "${NVENC_13_INTERFACE_DIR}"
    "${CUDA_13_ROOT}/include")
target_link_libraries(nvenc_native_nv12_probe PRIVATE
    "${CUDA_DRIVER_LIBRARY}"
    "${NVIDIA_ENCODE_LIBRARY}"
    m pthread)


# Build only the recorder and its small dependency set against API/CUDA 13.1.
# This directory is independent of Orange's top-level CUDA 12.2 build.
option(BUILD_NATIVE_RECORDER "Build the opt-in native-input external recorder" OFF)
if(BUILD_NATIVE_RECORDER)
    set(FFMPEG_ROOT "/opt/orange/lib/ffmpeg-nvidia" CACHE PATH "Existing FFmpeg installation")
    set(EVT_INCLUDE_DIR "/opt/EVT/eSDK/include" CACHE PATH "Existing EVT headers needed by profile types")
    find_library(CUDA_PROBE_RUNTIME NAMES cudart PATHS "${CUDA_13_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    foreach(component avformat avcodec avutil swscale swresample)
        find_library(PROBE_${component} NAMES ${component} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        list(APPEND PROBE_FFMPEG_LIBRARIES "${PROBE_${component}}")
    endforeach()
    file(READ "${ORANGE_SOURCE_ROOT}/src/video_encode_profile.cpp" PROFILE13_SOURCE)
    checked_replace(
        "hevc_config.pixelBitDepthMinus8 = 0;"
        "hevc_config.inputBitDepth = NV_ENC_BIT_DEPTH_8;\n        hevc_config.outputBitDepth = NV_ENC_BIT_DEPTH_8;\n        hevc_config.chromaFormatIDC = 1;"
        PROFILE13_SOURCE "${PROFILE13_SOURCE}")
    checked_replace("vui.videoFormat = kVuiVideoFormatUnspecified;"
        "vui.videoFormat = static_cast<decltype(vui.videoFormat)>(kVuiVideoFormatUnspecified);"
        PROFILE13_SOURCE "${PROFILE13_SOURCE}")
    checked_replace("encode_config->rcParams.enableMinQP = 0;"
        "encode_config->frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;\n    encode_config->rcParams.enableMinQP = 0;"
        PROFILE13_SOURCE "${PROFILE13_SOURCE}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/video_encode_profile13.cpp" "${PROFILE13_SOURCE}")
    add_executable(external_recorder_ipc_probe_native
        "${ORANGE_SOURCE_ROOT}/tools/external_recorder_ipc_probe.cpp"
        "${ORANGE_SOURCE_ROOT}/src/external_recorder_frame_metadata.cpp"
        "${ORANGE_SOURCE_ROOT}/src/video_container_finalization.cpp"
        "${ORANGE_SOURCE_ROOT}/src/FFmpegWriter.cpp"
        "${ORANGE_SOURCE_ROOT}/src/encoder_qp_map.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/video_encode_profile13.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/NvEncoder13.cpp"
        "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoderCuda.cpp")
    target_compile_features(external_recorder_ipc_probe_native PRIVATE cxx_std_17)
    target_compile_definitions(external_recorder_ipc_probe_native PRIVATE ORANGE_NVENC_NATIVE_ARRAY_INPUT=1)
    target_include_directories(external_recorder_ipc_probe_native PRIVATE
        "${ORANGE_SOURCE_ROOT}/src" "${ORANGE_SOURCE_ROOT}/src/NvEncoder")
    target_include_directories(external_recorder_ipc_probe_native SYSTEM BEFORE PRIVATE
        "${NVENC_13_INTERFACE_DIR}" "${CUDA_13_ROOT}/include"
        "${FFMPEG_ROOT}/include" "${EVT_INCLUDE_DIR}")
    target_link_libraries(external_recorder_ipc_probe_native PRIVATE
        "${CUDA_DRIVER_LIBRARY}" "${CUDA_PROBE_RUNTIME}" "${NVIDIA_ENCODE_LIBRARY}"
        ${PROBE_FFMPEG_LIBRARIES} m pthread)
    set_target_properties(external_recorder_ipc_probe_native PROPERTIES
        BUILD_RPATH "${CUDA_13_ROOT}/lib;${FFMPEG_ROOT}/lib")
endif()

# Keep inference and IPC producer on the application's CUDA 12.2 runtime.
option(BUILD_CONTENTION_TOOLS "Build camera-free CUDA 12 inference and IPC probes" OFF)
if(BUILD_CONTENTION_TOOLS)
    set(CUDA_12_ROOT "/usr/local/cuda-12.2" CACHE PATH "Production CUDA toolkit")
    set(TENSORRT_ROOT "/usr/local/TensorRT-10.0.1.6" CACHE PATH "Production TensorRT installation")
    find_library(CUDA12_RUNTIME NAMES cudart PATHS "${CUDA_12_ROOT}/lib64" NO_DEFAULT_PATH REQUIRED)
    find_library(TRT_INFER NAMES nvinfer PATHS "${TENSORRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(TRT_PLUGIN NAMES nvinfer_plugin PATHS "${TENSORRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    add_executable(trt_contention_probe trt_contention_probe.cpp)
    target_compile_features(trt_contention_probe PRIVATE cxx_std_17)
    target_compile_options(trt_contention_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(trt_contention_probe SYSTEM BEFORE PRIVATE
        "${CUDA_12_ROOT}/include" "${TENSORRT_ROOT}/include")
    target_link_libraries(trt_contention_probe PRIVATE "${CUDA12_RUNTIME}" "${TRT_INFER}" "${TRT_PLUGIN}" pthread)
    set_target_properties(trt_contention_probe PROPERTIES BUILD_RPATH "${CUDA_12_ROOT}/lib64;${TENSORRT_ROOT}/lib")
    add_executable(ipc_replay_probe ipc_replay_probe.cpp)
    target_compile_features(ipc_replay_probe PRIVATE cxx_std_17)
    target_compile_options(ipc_replay_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(ipc_replay_probe SYSTEM BEFORE PRIVATE "${CUDA_12_ROOT}/include")
    target_include_directories(ipc_replay_probe PRIVATE "${ORANGE_SOURCE_ROOT}/src")
    target_link_libraries(ipc_replay_probe PRIVATE "${CUDA12_RUNTIME}" pthread)
    set_target_properties(ipc_replay_probe PROPERTIES BUILD_RPATH "${CUDA_12_ROOT}/lib64")
endif()

option(BUILD_LEGACY_RECORDER_CONTROL "Build the same recorder source against the existing API/CUDA headers" OFF)
if(BUILD_LEGACY_RECORDER_CONTROL)
    if(NOT BUILD_NATIVE_RECORDER OR NOT BUILD_CONTENTION_TOOLS)
        message(FATAL_ERROR "Legacy control needs BUILD_NATIVE_RECORDER and BUILD_CONTENTION_TOOLS")
    endif()
    add_executable(external_recorder_ipc_probe_legacy
        "${ORANGE_SOURCE_ROOT}/tools/external_recorder_ipc_probe.cpp"
        "${ORANGE_SOURCE_ROOT}/src/external_recorder_frame_metadata.cpp"
        "${ORANGE_SOURCE_ROOT}/src/video_container_finalization.cpp"
        "${ORANGE_SOURCE_ROOT}/src/FFmpegWriter.cpp"
        "${ORANGE_SOURCE_ROOT}/src/encoder_qp_map.cpp"
        "${ORANGE_SOURCE_ROOT}/src/video_encode_profile.cpp"
        "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoder.cpp"
        "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoderCuda.cpp")
    target_compile_features(external_recorder_ipc_probe_legacy PRIVATE cxx_std_17)
    target_include_directories(external_recorder_ipc_probe_legacy PRIVATE
        "${ORANGE_SOURCE_ROOT}/src" "${ORANGE_SOURCE_ROOT}/src/NvEncoder")
    target_include_directories(external_recorder_ipc_probe_legacy SYSTEM BEFORE PRIVATE
        "${ORANGE_SOURCE_ROOT}/nvenc_api/include" "${CUDA_12_ROOT}/include"
        "${FFMPEG_ROOT}/include" "${EVT_INCLUDE_DIR}")
    target_link_libraries(external_recorder_ipc_probe_legacy PRIVATE
        "${CUDA_DRIVER_LIBRARY}" "${CUDA12_RUNTIME}" "${NVIDIA_ENCODE_LIBRARY}"
        ${PROBE_FFMPEG_LIBRARIES} m pthread)
    set_target_properties(external_recorder_ipc_probe_legacy PROPERTIES BUILD_RPATH "${CUDA_12_ROOT}/lib64;${FFMPEG_ROOT}/lib")
endif()

# Optional diagnostic surface-write kernel. Only its PTX is compiled by the
# isolated compiler; the host tools remain CXX/Driver-API programs.
option(BUILD_NATIVE_KERNEL "Build the experimental native Y surface-write PTX" OFF)
if(BUILD_NATIVE_KERNEL)
    if(NOT EXISTS "${CUDA_13_ROOT}/bin/nvcc")
        message(FATAL_ERROR "BUILD_NATIVE_KERNEL requires the isolated nvcc")
    endif()
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_nv12_write.ptx"
        COMMAND "${CUDA_13_ROOT}/bin/nvcc" --ptx -O3 -std=c++17
            --gpu-architecture=compute_86 --generate-line-info
            --output-file "${CMAKE_CURRENT_BINARY_DIR}/native_nv12_write.ptx"
            "${CMAKE_CURRENT_SOURCE_DIR}/native_nv12_write.cu"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native_nv12_write.cu"
        VERBATIM)
    add_custom_target(native_nv12_write_ptx ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/native_nv12_write.ptx")
    add_dependencies(nvenc_native_nv12_probe native_nv12_write_ptx)
    if(BUILD_NATIVE_RECORDER)
        add_dependencies(external_recorder_ipc_probe_native native_nv12_write_ptx)
    endif()
endif()
