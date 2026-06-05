// src/recording_writer_types.h

#ifndef ORANGE_RECORDING_WRITER_TYPES_H
#define ORANGE_RECORDING_WRITER_TYPES_H

#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "NvEncoder/NvEncoderCLIOptions.h"
#include <cuda.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct Writer
{
    std::string video_file;
    std::string keyframe_file;
    std::string metadata_file;
    FFmpegWriter *video = nullptr;
    std::ofstream* metadata = nullptr;
};

struct EncoderContext
{
    NV_ENC_BUFFER_FORMAT eFormat;
    NvEncoderInitParam encodeCLIOptions;
    CUcontext cuContext;
    unsigned long long num_frame_encode = 0;
    std::vector<std::vector<uint8_t>> vPacket;
    NvEncoderCuda *pEnc;
};

#endif // ORANGE_RECORDING_WRITER_TYPES_H
