// tools/nv12_prefill_validation.cpp
//
// Camera-free validation harness for the mono NV12 chroma pre-fill change
// (perf/nv12-chroma-prefill). Modeled on tools/nvenc_stress_load.cpp.
//
// It reproduces the modern full-frame mono recording data path exactly at the
// CUDA/NVENC level:
//
//   synthetic Mono8 device frame
//     -> per-frame Y-plane cudaMemcpy2DAsync into a persistent "prepared"
//        NV12 buffer (EncoderPreprocessWorker mono branch)
//     -> NvEncoderCuda::CopyToDeviceFrame into the NVENC input ring
//        (EncoderHwWorker non-direct branch)
//     -> EncodeFrame
//
// Two modes:
//   --mode legacy   pre-change behavior: per-frame UV plane copy into the
//                   prepared buffer plus a full Y+UV CopyToDeviceFrame.
//   --mode prefill  post-change behavior: chroma pre-filled once via
//                   NvEncoderCuda::FillInputFrameChromaPlanes(0x80) (and once
//                   in the prepared buffer), per-frame copies are Y-only.
//
// The per-frame "prepare" phase (everything between GetNextInputFrame and
// EncodeFrame) is timed with host clocks around device synchronization so the
// two modes are directly comparable. The bitstream can be dumped for offline
// ffmpeg decode verification that every decoded frame's U and V planes are
// uniform neutral gray.

#include "NvEncoder/NvEncoderCuda.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    int gpu_id = 0;
    uint32_t width = 4512;
    uint32_t height = 4512;
    uint32_t frames = 300;
    uint32_t gop = 60;
    std::string codec = "hevc";
    std::string mode = "prefill";  // legacy | prefill
    bool monochrome_encoding = true;
    std::string bitstream_out_path;
    std::string csv_path;
};

[[noreturn]] void usage(const char* argv0, int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out << "Usage:\n"
        << "  " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --gpu-id <int>            CUDA device id. Default 0.\n"
        << "  --width <int>             Encode width. Default 4512.\n"
        << "  --height <int>            Encode height. Default 4512.\n"
        << "  --frames <int>            Frames to encode. Default 300.\n"
        << "  --gop <int>               GOP length. Default 60.\n"
        << "  --codec <hevc|h264>       Default hevc.\n"
        << "  --mode <legacy|prefill>   Chroma handling mode. Default prefill.\n"
        << "  --no-monochrome-encoding  Disable NVENC monoChromeEncoding\n"
        << "                            (production mono profile enables it).\n"
        << "  --bitstream-out <path>    Raw elementary stream output.\n"
        << "  --csv <path>              Per-frame prepare/encode timing CSV.\n"
        << "  --help\n";
    std::exit(exit_code);
}

uint32_t parse_u32(const std::string& value, const char* name)
{
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("Invalid ") + name + " value: " + value);
    }
    return static_cast<uint32_t>(parsed);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto consume = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage(argv[0], 0);
        } else if (arg == "--gpu-id") {
            options.gpu_id = static_cast<int>(parse_u32(consume("--gpu-id"), "--gpu-id"));
        } else if (arg == "--width") {
            options.width = parse_u32(consume("--width"), "--width");
        } else if (arg == "--height") {
            options.height = parse_u32(consume("--height"), "--height");
        } else if (arg == "--frames") {
            options.frames = parse_u32(consume("--frames"), "--frames");
        } else if (arg == "--gop") {
            options.gop = parse_u32(consume("--gop"), "--gop");
        } else if (arg == "--codec") {
            options.codec = consume("--codec");
        } else if (arg == "--mode") {
            options.mode = consume("--mode");
        } else if (arg == "--no-monochrome-encoding") {
            options.monochrome_encoding = false;
        } else if (arg == "--bitstream-out") {
            options.bitstream_out_path = consume("--bitstream-out");
        } else if (arg == "--csv") {
            options.csv_path = consume("--csv");
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (options.width == 0 || options.height == 0 || options.frames == 0) {
        throw std::runtime_error("--width, --height, and --frames must be positive");
    }
    if ((options.width % 2) != 0 || (options.height % 2) != 0) {
        throw std::runtime_error("--width and --height must be even for NV12");
    }
    if (options.mode != "legacy" && options.mode != "prefill") {
        throw std::runtime_error("--mode must be legacy or prefill");
    }
    if (options.codec != "hevc" && options.codec != "h264") {
        throw std::runtime_error("--codec must be hevc or h264");
    }
    return options;
}

void check_cuda(cudaError_t status, const char* call)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(call) + " failed: " + cudaGetErrorString(status));
    }
}

void check_cu(CUresult status, const char* call)
{
    if (status != CUDA_SUCCESS) {
        const char* name = nullptr;
        cuGetErrorName(status, &name);
        throw std::runtime_error(std::string(call) + " failed: " + (name ? name : "unknown"));
    }
}

// Synthetic mono content: a diagonal gradient plus a bright square that moves
// with the frame index, so consecutive frames differ (exercises inter frames).
std::vector<uint8_t> make_mono_frame(uint32_t width, uint32_t height, uint32_t frame_index)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = frame.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    const uint32_t box = std::min<uint32_t>(256, std::min(width, height) / 4);
    const uint32_t max_x = width > box ? width - box : 0;
    const uint32_t max_y = height > box ? height - box : 0;
    const uint32_t bx = max_x > 0 ? (frame_index * 97) % max_x : 0;
    const uint32_t by = max_y > 0 ? (frame_index * 61) % max_y : 0;
    for (uint32_t y = by; y < by + box && y < height; ++y) {
        std::memset(frame.data() + static_cast<size_t>(y) * width + bx, 235,
                    std::min<size_t>(box, width - bx));
    }
    return frame;
}

double mean(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(std::ceil(values.size() * p / 100.0)) - 1);
    return values[index];
}

double stddev(const std::vector<double>& values)
{
    if (values.size() < 2) {
        return 0.0;
    }
    const double m = mean(values);
    double accum = 0.0;
    for (double v : values) {
        accum += (v - m) * (v - m);
    }
    return std::sqrt(accum / static_cast<double>(values.size() - 1));
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        const bool legacy = options.mode == "legacy";

        check_cu(cuInit(0), "cuInit");
        check_cuda(cudaSetDevice(options.gpu_id), "cudaSetDevice");
        check_cuda(cudaFree(nullptr), "cudaFree(0)");
        CUcontext cu_context = nullptr;
        check_cu(cuCtxGetCurrent(&cu_context), "cuCtxGetCurrent");
        if (!cu_context) {
            throw std::runtime_error("No current CUDA context");
        }

        cudaStream_t stream = nullptr;
        check_cuda(cudaStreamCreate(&stream), "cudaStreamCreate");

        NvEncoderCuda encoder(cu_context, options.width, options.height,
                              NV_ENC_BUFFER_FORMAT_NV12, 3);
        NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        initialize_params.encodeConfig = &encode_config;
        encoder.CreateDefaultEncoderParams(
            &initialize_params,
            options.codec == "hevc" ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID,
            NV_ENC_PRESET_P1_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY);
        initialize_params.frameRateNum = 60;
        initialize_params.frameRateDen = 1;
        encode_config.gopLength = options.gop;
        encode_config.frameIntervalP = 1;
        encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encode_config.rcParams.averageBitRate = 150000000;
        encode_config.rcParams.maxBitRate = 150000000;
        encode_config.rcParams.vbvBufferSize = 150000000;
        if (options.monochrome_encoding) {
            encode_config.monoChromeEncoding = 1;
        }
        encoder.CreateEncoder(&initialize_params);
        encoder.SetIOCudaStreams(
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream),
            reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream));

        const NvEncInputFrame* pitch_probe = encoder.GetNextInputFrame();
        if (!pitch_probe) {
            throw std::runtime_error("No NVENC input frame during pitch probe");
        }
        const uint32_t pitch = pitch_probe->pitch;
        const size_t luma_bytes = static_cast<size_t>(pitch) * options.height;
        const size_t chroma_bytes = luma_bytes / 2;

        // Persistent "prepared" NV12 buffer (mirrors the preprocess entry pool).
        unsigned char* d_prepared = nullptr;
        check_cuda(cudaMalloc(&d_prepared, luma_bytes + chroma_bytes), "cudaMalloc(prepared)");
        check_cuda(cudaMemset(d_prepared, 0, luma_bytes + chroma_bytes), "cudaMemset(prepared)");

        // Legacy mode keeps a default UV plane and copies it per frame; prefill
        // mode fills chroma once (prepared buffer and the NVENC ring).
        unsigned char* d_uv_default = nullptr;
        if (legacy) {
            check_cuda(cudaMalloc(&d_uv_default, chroma_bytes), "cudaMalloc(uv_default)");
            check_cuda(cudaMemset(d_uv_default, 0x80, chroma_bytes), "cudaMemset(uv_default)");
        } else {
            check_cuda(cudaMemset(d_prepared + luma_bytes, 0x80, chroma_bytes),
                       "cudaMemset(prepared uv)");
            encoder.FillInputFrameChromaPlanes(0x80);
        }
        check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(init)");

        // Synthetic mono source frames, uploaded once (mirrors camera d_image).
        constexpr uint32_t kSourceFrames = 8;
        std::vector<unsigned char*> d_sources(kSourceFrames, nullptr);
        for (uint32_t i = 0; i < kSourceFrames; ++i) {
            const std::vector<uint8_t> host_frame =
                make_mono_frame(options.width, options.height, i);
            check_cuda(cudaMalloc(&d_sources[i], host_frame.size()), "cudaMalloc(source)");
            check_cuda(cudaMemcpy(d_sources[i], host_frame.data(), host_frame.size(),
                                  cudaMemcpyHostToDevice),
                       "cudaMemcpy(source)");
        }

        std::ofstream bitstream_out;
        if (!options.bitstream_out_path.empty()) {
            bitstream_out.open(options.bitstream_out_path, std::ios::binary | std::ios::trunc);
            if (!bitstream_out) {
                throw std::runtime_error("Failed to open bitstream output: " +
                                         options.bitstream_out_path);
            }
        }
        std::ofstream csv;
        if (!options.csv_path.empty()) {
            csv.open(options.csv_path);
            if (!csv) {
                throw std::runtime_error("Failed to open CSV output: " + options.csv_path);
            }
            csv << "frame_index,prepare_ms,encode_ms\n";
        }

        const size_t per_frame_prepare_payload_bytes =
            static_cast<size_t>(options.width) * options.height     // preprocess Y copy
            + static_cast<size_t>(options.width) * options.height   // ring Y copy
            + (legacy
                   ? chroma_bytes                                    // prepared UV copy
                         + static_cast<size_t>(options.width) * options.height / 2  // ring UV copy
                   : 0);

        std::cout << "nv12_prefill_validation started"
                  << " gpu_id=" << options.gpu_id
                  << " resolution=" << options.width << "x" << options.height
                  << " frames=" << options.frames
                  << " mode=" << options.mode
                  << " codec=" << options.codec
                  << " monochrome_encoding=" << (options.monochrome_encoding ? 1 : 0)
                  << " ring_pitch=" << pitch
                  << " prepare_payload_bytes_per_frame=" << per_frame_prepare_payload_bytes
                  << std::endl;

        std::vector<double> prepare_ms_samples;
        std::vector<double> encode_ms_samples;
        prepare_ms_samples.reserve(options.frames);
        encode_ms_samples.reserve(options.frames);
        std::vector<std::vector<uint8_t>> packets;
        uint64_t total_bytes = 0;

        const auto run_start = std::chrono::steady_clock::now();
        for (uint32_t frame_index = 0; frame_index < options.frames; ++frame_index) {
            const NvEncInputFrame* input_frame = encoder.GetNextInputFrame();
            if (!input_frame || !input_frame->inputPtr) {
                throw std::runtime_error("NvEncoder returned no input frame");
            }
            const unsigned char* d_source = d_sources[frame_index % kSourceFrames];

            check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(pre-prepare)");
            const auto prepare_start = std::chrono::steady_clock::now();

            // --- Preprocess stage (EncoderPreprocessWorker mono branch) ---
            check_cuda(cudaMemcpy2DAsync(d_prepared, pitch, d_source, options.width,
                                         options.width, options.height,
                                         cudaMemcpyDeviceToDevice, stream),
                       "cudaMemcpy2DAsync(Y)");
            if (legacy) {
                check_cuda(cudaMemcpyAsync(d_prepared + luma_bytes, d_uv_default,
                                           chroma_bytes, cudaMemcpyDeviceToDevice, stream),
                           "cudaMemcpyAsync(UV default)");
            }

            // --- HW worker stage (EncoderHwWorker non-direct branch) ---
            NvEncoderCuda::CopyToDeviceFrame(
                cu_context,
                d_prepared,
                pitch,
                reinterpret_cast<CUdeviceptr>(input_frame->inputPtr),
                input_frame->pitch,
                static_cast<int>(options.width),
                static_cast<int>(options.height),
                CU_MEMORYTYPE_DEVICE,
                input_frame->bufferFormat,
                input_frame->chromaOffsets,
                legacy ? input_frame->numChromaPlanes : 0u);

            check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(post-prepare)");
            const auto prepare_end = std::chrono::steady_clock::now();
            const double prepare_ms =
                std::chrono::duration<double, std::milli>(prepare_end - prepare_start).count();
            prepare_ms_samples.push_back(prepare_ms);

            NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
            pic_params.frameIdx = frame_index;
            pic_params.inputTimeStamp = frame_index;
            pic_params.inputDuration = 1;
            const auto encode_start = std::chrono::steady_clock::now();
            encoder.EncodeFrame(packets, &pic_params);
            const double encode_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - encode_start).count();
            encode_ms_samples.push_back(encode_ms);

            for (const auto& packet : packets) {
                total_bytes += packet.size();
                if (bitstream_out && !packet.empty()) {
                    bitstream_out.write(reinterpret_cast<const char*>(packet.data()),
                                        static_cast<std::streamsize>(packet.size()));
                }
            }
            if (csv) {
                csv << frame_index << ","
                    << std::fixed << std::setprecision(6)
                    << prepare_ms << "," << encode_ms << "\n";
            }
        }

        encoder.EndEncode(packets);
        for (const auto& packet : packets) {
            total_bytes += packet.size();
            if (bitstream_out && !packet.empty()) {
                bitstream_out.write(reinterpret_cast<const char*>(packet.data()),
                                    static_cast<std::streamsize>(packet.size()));
            }
        }
        if (bitstream_out) {
            bitstream_out.flush();
            if (!bitstream_out) {
                throw std::runtime_error("Failed while writing bitstream output");
            }
        }
        encoder.DestroyEncoder();

        const double total_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_start).count();
        std::cout << "nv12_prefill_validation summary\n"
                  << "  mode=" << options.mode << "\n"
                  << "  frames=" << options.frames << "\n"
                  << "  bitstream_bytes=" << total_bytes << "\n"
                  << "  sustained_fps=" << std::fixed << std::setprecision(3)
                  << (total_s > 0.0 ? options.frames / total_s : 0.0) << "\n"
                  << "  prepare_ms_mean=" << mean(prepare_ms_samples)
                  << " p95=" << percentile(prepare_ms_samples, 95.0)
                  << " max=" << (prepare_ms_samples.empty() ? 0.0 :
                                 *std::max_element(prepare_ms_samples.begin(),
                                                   prepare_ms_samples.end()))
                  << " stddev=" << stddev(prepare_ms_samples) << "\n"
                  << "  encode_ms_mean=" << mean(encode_ms_samples)
                  << " p95=" << percentile(encode_ms_samples, 95.0) << "\n"
                  << "  prepare_payload_bytes_per_frame=" << per_frame_prepare_payload_bytes
                  << std::endl;

        for (unsigned char* d_source : d_sources) {
            cudaFree(d_source);
        }
        if (d_uv_default) {
            cudaFree(d_uv_default);
        }
        cudaFree(d_prepared);
        check_cuda(cudaStreamDestroy(stream), "cudaStreamDestroy");
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "nv12_prefill_validation failed: " << ex.what() << std::endl;
        return 1;
    }
}
