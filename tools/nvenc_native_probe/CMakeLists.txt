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

# The checkout's wrapper predates API 13.1. Generate a private build-tree copy
# with API-13.1 field migrations and mapped-format/pitch propagation. The tracked wrapper
# and all production targets remain byte-for-byte untouched.
file(READ "${ORANGE_SOURCE_ROOT}/src/NvEncoder/NvEncoder.cpp" NVENCODER13_SOURCE)
string(REPLACE
    "pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? 2 : 0;"
    "pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;\n        pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.outputBitDepth =\n            pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth;"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
string(REPLACE
    "pEncoderParams->encodeConfig->encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 != 2"
    "pEncoderParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth != NV_ENC_BIT_DEPTH_10"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
string(REPLACE
    "registerResource.pOutputFencePoint = pOutputFencePoint;"
    "(void)pOutputFencePoint;"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
string(REPLACE
    "NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams));"
    "{ NVENCSTATUS initStatus = m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams); if (initStatus != NV_ENC_SUCCESS) { const char* detail = m_nvenc.nvEncGetLastErrorString ? m_nvenc.nvEncGetLastErrorString(m_hEncoder) : nullptr; NVENC_THROW_ERROR(std::string(\"nvEncInitializeEncoder returned \" ) + std::to_string(initStatus) + (detail ? std::string(\": \") + detail : std::string()), initStatus); } }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
string(REPLACE
    "NVENC_API_CALL(m_nvenc.nvEncLockBitstream(m_hEncoder, &lockBitstreamData));"
    "{ NVENCSTATUS lockStatus = m_nvenc.nvEncLockBitstream(m_hEncoder, &lockBitstreamData); if (lockStatus != NV_ENC_SUCCESS) { const char* detail = m_nvenc.nvEncGetLastErrorString ? m_nvenc.nvEncGetLastErrorString(m_hEncoder) : nullptr; NVENC_THROW_ERROR(std::string(\"nvEncLockBitstream returned \" ) + std::to_string(lockStatus) + (detail ? std::string(\": \") + detail : std::string()), lockStatus); } }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
# API 13.1 returns the format of the mapped resource.  Carry it through to
# NV_ENC_PIC_PARAMS, matching FFmpeg's current CUARRAY path instead of assuming
# that the constructor's requested format describes every mapped resource.
string(REPLACE
    "m_vMappedInputBuffers[bfrIdx] = mapInputResource.mappedResource;"
    "m_vMappedInputBuffers[bfrIdx] = mapInputResource.mappedResource;\n    m_eBufferFormat = mapInputResource.mappedBufferFmt;\n    if (m_iToSend == 0) { std::cout << \"nvenc_mapped_buffer_format=\" << static_cast<uint32_t>(mapInputResource.mappedBufferFmt) << std::endl; }"
    NVENCODER13_SOURCE "${NVENCODER13_SOURCE}")
# CUDAARRAY registrations have no linear allocation pitch to recover later,
# so pass the visible NV12 row width explicitly, also matching FFmpeg.
string(REPLACE
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
