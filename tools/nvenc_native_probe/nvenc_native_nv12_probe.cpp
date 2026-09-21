#include "NvEncoder/NvEncoderCuda.h"

#include <cuda.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(CUDA_VERSION) || CUDA_VERSION < 13010
#error "nvenc_native_nv12_probe requires CUDA Toolkit 13.1 or newer headers"
#endif

#ifndef CUDA_ARRAY3D_VIDEO_ENCODE_DECODE
#error "CUDA 13.1 CUDA_ARRAY3D_VIDEO_ENCODE_DECODE is required"
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop_requested{false};

enum class InputKind {
    Linear,
    NativeArray,
};

enum class UpdateMode {
    Prefilled,
    PerFrame,
    RegisteredSource,
};

enum class PacingMode {
    Continuous,
    SplitGop,
};

enum class SourceLayout {
    Pitched,
    Packed,
};

enum class NativeUpdate {
    Copy,
    Kernel,
};

struct Options {
    int gpu_id = 0;
    uint32_t width = 4512;
    uint32_t height = 4512;
    uint32_t native_storage_width = 0;
    uint32_t native_register_pitch = 0;
    uint32_t fps = 100;
    uint64_t start_at_monotonic_ns = 0;
    PacingMode pacing = PacingMode::Continuous;
    uint32_t split_gop_idle_frames = 0;
    uint64_t frames = 300;
    uint64_t warmup_frames = 50;
    uint32_t source_frames = 4;
    SourceLayout source_layout = SourceLayout::Pitched;
    uint32_t extra_output_delay = 0;
    uint32_t gop = 25;
    uint32_t bitrate_bps = 150000000;
    uint32_t max_bitrate_bps = 150000000;
    uint32_t vbv_buffer_size = 150000000;
    bool pace = true;
    bool monochrome = true;
    InputKind input = InputKind::NativeArray;
    UpdateMode update = UpdateMode::Prefilled;
    NativeUpdate native_update = NativeUpdate::Copy;
    std::string native_kernel_ptx;
    std::string raw_file;
    uint32_t raw_pitch = 0;
    uint64_t raw_frame_bytes = 0;
    std::string reference_prefix;
    std::string readback_prefix;
    std::string bitstream_path;
    std::string csv_path;
};

struct DeviceSource {
    CUdeviceptr pointer = 0;
    size_t pitch = 0;
    NV_ENC_REGISTERED_PTR registered = nullptr;
};

struct InputSurface {
    CUdeviceptr linear = 0;
    size_t pitch = 0;
    size_t register_pitch = 0;
    CUarray array = nullptr;
    CUarray y_plane = nullptr;
    CUarray uv_plane = nullptr;
    CUsurfObject y_surface = 0;
    NV_ENC_REGISTERED_PTR registered = nullptr;
};

struct FrameSample {
    uint64_t frame_index = 0;
    uint32_t slot = 0;
    uint32_t source = 0;
    uint32_t source_pitch = 0;
    uint32_t input_pitch = 0;
    bool measured = false;
    double update_wall_ms = 0.0;
    double update_gpu_ms = 0.0;
    double encode_wall_ms = 0.0;
    double map_ms = 0.0;
    double encode_picture_ms = 0.0;
    double completion_wait_ms = 0.0;
    double lock_bitstream_ms = 0.0;
    double bitstream_copy_ms = 0.0;
    double unlock_bitstream_ms = 0.0;
    double unmap_ms = 0.0;
    uint32_t returned_packets = 0;
    uint64_t returned_bytes = 0;
    uint64_t timeline_frame_index = 0;
    uint64_t burst_index = 0;
    uint32_t frame_in_burst = 0;
    uint32_t idle_periods_before = 0;
    uint64_t scheduled_steady_ns = 0;
    uint64_t loop_start_steady_ns = 0;
    uint64_t input_ready_steady_ns = 0;
    uint64_t update_start_steady_ns = 0;
    uint64_t update_end_steady_ns = 0;
    uint64_t encode_start_steady_ns = 0;
    uint64_t encode_end_steady_ns = 0;
    uint64_t frame_done_steady_ns = 0;
    double schedule_lateness_ms = 0.0;
};

void signal_handler(int)
{
    g_stop_requested.store(true, std::memory_order_release);
}

double ns_to_ms(uint64_t value)
{
    return static_cast<double>(value) / 1000000.0;
}

double elapsed_ms(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

const char* input_name(InputKind input)
{
    return input == InputKind::NativeArray ? "native-array" : "linear";
}

const char* update_name(UpdateMode update)
{
    if (update == UpdateMode::PerFrame) {
        return "per-frame";
    }
    if (update == UpdateMode::RegisteredSource) {
        return "registered-source";
    }
    return "prefilled";
}

const char* pacing_name(PacingMode pacing)
{
    return pacing == PacingMode::SplitGop ? "split-gop" : "continuous";
}

const char* source_layout_name(SourceLayout layout)
{
    return layout == SourceLayout::Packed ? "packed" : "pitched";
}

const char* native_update_name(NativeUpdate update)
{
    return update == NativeUpdate::Kernel ? "kernel" : "copy";
}

uint64_t steady_ns(Clock::time_point time)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

uint64_t scheduled_timeline_frame(const Options& options, uint64_t submitted_frame)
{
    if (options.pacing != PacingMode::SplitGop) {
        return submitted_frame;
    }
    const uint64_t burst = submitted_frame / options.gop;
    const uint64_t in_burst = submitted_frame % options.gop;
    return burst * (static_cast<uint64_t>(options.gop) + options.split_gop_idle_frames) + in_burst;
}

void check_cu(CUresult status, const char* call)
{
    if (status == CUDA_SUCCESS) {
        return;
    }
    const char* name = nullptr;
    const char* description = nullptr;
    cuGetErrorName(status, &name);
    cuGetErrorString(status, &description);
    throw std::runtime_error(
        std::string(call) + " failed: " + (name ? name : "unknown") +
        (description ? std::string(" (") + description + ")" : std::string()));
}

uint64_t parse_u64(const std::string& value, const char* option)
{
    if (value.empty()) {
        throw std::runtime_error(std::string("Missing value for ") + option);
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::runtime_error(std::string("Invalid value for ") + option + ": " + value);
    }
    return static_cast<uint64_t>(parsed);
}

uint32_t parse_u32(const std::string& value, const char* option)
{
    const uint64_t parsed = parse_u64(value, option);
    if (parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string("Value is too large for ") + option);
    }
    return static_cast<uint32_t>(parsed);
}

int parse_nonnegative_int(const std::string& value, const char* option)
{
    const uint64_t parsed = parse_u64(value, option);
    if (parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("Value is too large for ") + option);
    }
    return static_cast<int>(parsed);
}

[[noreturn]] void usage(const char* argv0, int exit_code)
{
    std::ostream& out = exit_code == 0 ? std::cout : std::cerr;
    out
        << "Usage: " << argv0 << " --input <linear|native-array> --update <prefilled|per-frame|registered-source> [options]\n\n"
        << "Matched comparison defaults: HEVC, P1, low latency, VBR 150 Mbps, GOP 25,\n"
        << "AQ off, temporal AQ off, lookahead off, 4512x4512 at 100 fps.\n\n"
        << "  --gpu-id <n>                 CUDA device ordinal (default 0)\n"
        << "  --input <kind>               linear or native-array (default native-array)\n"
        << "  --update <mode>              prefilled, per-frame, or registered-source (default prefilled)\n"
        << "  --width <n> --height <n>     Frame dimensions (default 4512x4512)\n"
        << "  --native-storage-width <n>  Native array allocation/row pitch (default width aligned to 128)\n"
        << "  --native-register-pitch <n> NVENC CUDAARRAY registration pitch (default storage width)\n"
        << "  --fps <n>                    Encode and pacing rate (default 100)\n"
        << "  --start-at-monotonic-ns <n> First frame CLOCK_MONOTONIC deadline (default 0=current behavior)\n"
        << "  --pacing <mode>              continuous or split-gop (default continuous)\n"
        << "  --split-gop-idle-frames <n> Missing frame periods after each GOP (default gop length)\n"
        << "  --frames <n>                 Total submitted frames (default 300)\n"
        << "  --warmup-frames <n>          Rows excluded from summary (default 50)\n"
        << "  --source-frames <n>          Cached patterned/raw Mono8 frames (default 4)\n"
        << "  --source-layout <layout>    Cached NV12 allocation: pitched or packed (default pitched)\n"
        << "  --native-update <mode>      Native per-frame Y update: copy or kernel (default copy)\n"
        << "  --native-kernel-ptx <path>  PTX containing orange_native_nv12_write_luma (kernel mode)\n"
        << "  --extra-output-delay <n>     NvEncoder output delay (default 0)\n"
        << "  --raw-file <path>            Optional cached Mono8/raw source sequence\n"
        << "  --raw-pitch <n>              Raw Y row pitch (default width)\n"
        << "  --raw-frame-bytes <n>        Raw record stride (default raw_pitch*height)\n"
        << "  --reference-prefix <path>    Write each cached source as <path>_sourceNNNN.y8\n"
        << "  --readback-prefix <path>     Read back slot 0 Y/UV before encode and verify bytes\n"
        << "  --bitstream-out <path>       Save raw HEVC Annex-B output\n"
        << "  --csv <path>                 Save per-frame timings\n"
        << "  --no-pace                    Submit as fast as possible\n"
        << "  --no-monochrome              Disable NVENC monochrome flag\n"
        << "  --help\n";
    std::exit(exit_code);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];
        std::string inline_value;
        const size_t equals = argument.find('=');
        if (argument.rfind("--", 0) == 0 && equals != std::string::npos) {
            inline_value = argument.substr(equals + 1);
            argument = argument.substr(0, equals);
        }
        auto consume = [&](const char* option) {
            if (!inline_value.empty()) {
                return inline_value;
            }
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + option);
            }
            return std::string(argv[++index]);
        };

        if (argument == "--help" || argument == "-h") {
            usage(argv[0], 0);
        } else if (argument == "--gpu-id") {
            options.gpu_id = parse_nonnegative_int(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--input") {
            const std::string value = consume(argument.c_str());
            if (value == "linear") {
                options.input = InputKind::Linear;
            } else if (value == "native-array") {
                options.input = InputKind::NativeArray;
            } else {
                throw std::runtime_error("--input must be linear or native-array");
            }
        } else if (argument == "--update") {
            const std::string value = consume(argument.c_str());
            if (value == "prefilled") {
                options.update = UpdateMode::Prefilled;
            } else if (value == "per-frame") {
                options.update = UpdateMode::PerFrame;
            } else if (value == "registered-source") {
                options.update = UpdateMode::RegisteredSource;
            } else {
                throw std::runtime_error("--update must be prefilled, per-frame, or registered-source");
            }
        } else if (argument == "--width") {
            options.width = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--height") {
            options.height = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--native-storage-width") {
            options.native_storage_width = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--native-register-pitch") {
            options.native_register_pitch = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--fps") {
            options.fps = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--start-at-monotonic-ns") {
            options.start_at_monotonic_ns = parse_u64(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--pacing") {
            const std::string value = consume(argument.c_str());
            if (value == "continuous") {
                options.pacing = PacingMode::Continuous;
            } else if (value == "split-gop") {
                options.pacing = PacingMode::SplitGop;
            } else {
                throw std::runtime_error("--pacing must be continuous or split-gop");
            }
        } else if (argument == "--split-gop-idle-frames") {
            options.split_gop_idle_frames = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--frames") {
            options.frames = parse_u64(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--warmup-frames") {
            options.warmup_frames = parse_u64(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--source-frames") {
            options.source_frames = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--source-layout") {
            const std::string value = consume(argument.c_str());
            if (value == "pitched") {
                options.source_layout = SourceLayout::Pitched;
            } else if (value == "packed") {
                options.source_layout = SourceLayout::Packed;
            } else {
                throw std::runtime_error("--source-layout must be pitched or packed");
            }
        } else if (argument == "--native-update") {
            const std::string value = consume(argument.c_str());
            if (value == "copy") {
                options.native_update = NativeUpdate::Copy;
            } else if (value == "kernel") {
                options.native_update = NativeUpdate::Kernel;
            } else {
                throw std::runtime_error("--native-update must be copy or kernel");
            }
        } else if (argument == "--native-kernel-ptx") {
            options.native_kernel_ptx = consume(argument.c_str());
        } else if (argument == "--extra-output-delay") {
            options.extra_output_delay = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--raw-file") {
            options.raw_file = consume(argument.c_str());
        } else if (argument == "--raw-pitch") {
            options.raw_pitch = parse_u32(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--raw-frame-bytes") {
            options.raw_frame_bytes = parse_u64(consume(argument.c_str()), argument.c_str());
        } else if (argument == "--reference-prefix") {
            options.reference_prefix = consume(argument.c_str());
        } else if (argument == "--readback-prefix") {
            options.readback_prefix = consume(argument.c_str());
        } else if (argument == "--bitstream-out") {
            options.bitstream_path = consume(argument.c_str());
        } else if (argument == "--csv") {
            options.csv_path = consume(argument.c_str());
        } else if (argument == "--no-pace") {
            options.pace = false;
        } else if (argument == "--no-monochrome") {
            options.monochrome = false;
        } else {
            throw std::runtime_error("Unknown option: " + argument);
        }
    }

    if (options.width == 0 || options.height == 0 ||
        (options.width & 1U) != 0 || (options.height & 1U) != 0) {
        throw std::runtime_error("NV12 width and height must be positive and even");
    }
    if (options.width % 4 != 0) {
        throw std::runtime_error("Linear NVENC registration requires byte pitch divisible by four");
    }
    if (options.native_storage_width == 0) {
        options.native_storage_width = (options.width + 127U) & ~127U;
    }
    if (options.native_storage_width < options.width ||
        (options.native_storage_width & 1U) != 0) {
        throw std::runtime_error("--native-storage-width must be even and >= width");
    }
    if (options.native_register_pitch == 0) {
        options.native_register_pitch = options.native_storage_width;
    }
    if (options.native_register_pitch < options.width ||
        (options.native_register_pitch % 4U) != 0) {
        throw std::runtime_error("--native-register-pitch must be >= width and divisible by four");
    }
    if (options.frames == 0 || options.source_frames == 0) {
        throw std::runtime_error("--frames and --source-frames must be positive");
    }
    if (options.pace && options.fps == 0) {
        throw std::runtime_error("--fps must be positive unless --no-pace is used");
    }
    if (options.start_at_monotonic_ns >
        static_cast<uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max())) {
        throw std::runtime_error("--start-at-monotonic-ns exceeds the steady-clock range");
    }
    if (options.gop == 0) {
        throw std::runtime_error("GOP length must be positive");
    }
    if (options.split_gop_idle_frames == 0) {
        options.split_gop_idle_frames = options.gop;
    }
    if (options.update == UpdateMode::RegisteredSource && options.input != InputKind::Linear) {
        throw std::runtime_error("--update registered-source requires --input linear");
    }
    if (options.native_update == NativeUpdate::Kernel) {
        if (options.input != InputKind::NativeArray || options.update != UpdateMode::PerFrame) {
            throw std::runtime_error(
                "--native-update kernel requires --input native-array --update per-frame");
        }
        if (options.native_kernel_ptx.empty()) {
            throw std::runtime_error("--native-update kernel requires --native-kernel-ptx");
        }
    }
    if (options.extra_output_delay > 64) {
        throw std::runtime_error("--extra-output-delay must be <= 64");
    }
    if (options.raw_pitch == 0) {
        options.raw_pitch = options.width;
    }
    if (options.raw_pitch < options.width) {
        throw std::runtime_error("--raw-pitch must be >= width");
    }
    if (options.raw_frame_bytes == 0) {
        options.raw_frame_bytes = static_cast<uint64_t>(options.raw_pitch) * options.height;
    }
    if (options.raw_frame_bytes < static_cast<uint64_t>(options.raw_pitch) * options.height) {
        throw std::runtime_error("--raw-frame-bytes is smaller than raw_pitch*height");
    }
    return options;
}

std::vector<uint8_t> make_pattern(uint32_t width, uint32_t height, uint32_t frame_index)
{
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = frame.data() + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t macro = ((x / 64U) * 17U + (y / 64U) * 31U + frame_index * 47U) & 0xffU;
            const bool ruler = (x % 509U) < 3U || (y % 487U) < 3U;
            const bool diagonal = ((x + y + frame_index * 13U) % 997U) < 4U;
            row[x] = static_cast<uint8_t>(ruler ? 235U : (diagonal ? 16U : macro));
        }
    }
    return frame;
}

std::vector<std::vector<uint8_t>> load_raw_luma(const Options& options)
{
    std::ifstream input(options.raw_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open raw source: " + options.raw_file);
    }
    if (options.raw_frame_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("--raw-frame-bytes exceeds addressable size");
    }
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> record(static_cast<size_t>(options.raw_frame_bytes));
    for (uint32_t frame_index = 0; frame_index < options.source_frames; ++frame_index) {
        input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()));
        if (input.gcount() == 0 && input.eof()) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(record.size())) {
            throw std::runtime_error("Raw source ended with a partial record");
        }
        std::vector<uint8_t> luma(static_cast<size_t>(options.width) * options.height);
        for (uint32_t y = 0; y < options.height; ++y) {
            std::memcpy(
                luma.data() + static_cast<size_t>(y) * options.width,
                record.data() + static_cast<size_t>(y) * options.raw_pitch,
                options.width);
        }
        frames.push_back(std::move(luma));
    }
    if (frames.empty()) {
        throw std::runtime_error("Raw source contains no complete frames");
    }
    return frames;
}

std::vector<DeviceSource> make_sources(const Options& options)
{
    std::vector<std::vector<uint8_t>> host_frames;
    if (!options.raw_file.empty()) {
        host_frames = load_raw_luma(options);
    }

    const uint32_t count = host_frames.empty()
        ? options.source_frames
        : static_cast<uint32_t>(host_frames.size());
    std::vector<DeviceSource> sources;
    sources.reserve(count);
    for (uint32_t frame_index = 0; frame_index < count; ++frame_index) {
        std::vector<uint8_t> generated;
        const std::vector<uint8_t>* host = nullptr;
        if (host_frames.empty()) {
            generated = make_pattern(options.width, options.height, frame_index);
            host = &generated;
        } else {
            host = &host_frames[frame_index];
        }

        if (!options.reference_prefix.empty()) {
            std::ostringstream path;
            path << options.reference_prefix << "_source"
                 << std::setw(4) << std::setfill('0') << frame_index << ".y8";
            std::ofstream reference(path.str(), std::ios::binary | std::ios::trunc);
            if (!reference) {
                throw std::runtime_error("Failed to open reference Y output: " + path.str());
            }
            reference.write(
                reinterpret_cast<const char*>(host->data()),
                static_cast<std::streamsize>(host->size()));
            if (!reference) {
                throw std::runtime_error("Failed to write reference Y output: " + path.str());
            }
        }

        DeviceSource source;
        if (options.source_layout == SourceLayout::Pitched) {
            check_cu(
                cuMemAllocPitch(
                    &source.pointer,
                    &source.pitch,
                    options.width,
                    options.height * 3 / 2,
                    16),
                "cuMemAllocPitch(source NV12)");
        } else {
            source.pitch = options.width;
            check_cu(
                cuMemAlloc(
                    &source.pointer,
                    source.pitch * options.height * 3 / 2),
                "cuMemAlloc(packed source NV12)");
        }
        CUDA_MEMCPY2D copy = {};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = host->data();
        copy.srcPitch = options.width;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = source.pointer;
        copy.dstPitch = source.pitch;
        copy.WidthInBytes = options.width;
        copy.Height = options.height;
        check_cu(cuMemcpy2D(&copy), "cuMemcpy2D(source Y upload)");
        check_cu(
            cuMemsetD2D8(
                source.pointer + source.pitch * options.height,
                source.pitch,
                128,
                options.width,
                options.height / 2),
            "cuMemsetD2D8(source UV)");
        sources.push_back(source);
    }
    return sources;
}

void initialize_neutral_uv(const Options& options, InputSurface& surface)
{
    if (options.input == InputKind::Linear) {
        check_cu(
            cuMemsetD2D8(
                surface.linear + surface.pitch * options.height,
                surface.pitch,
                128,
                options.width,
                options.height / 2),
            "cuMemsetD2D8(linear UV)");
        return;
    }

    std::vector<uint8_t> neutral(surface.pitch * (options.height / 2), 128);
    CUDA_MEMCPY2D copy = {};
    copy.srcMemoryType = CU_MEMORYTYPE_HOST;
    copy.srcHost = neutral.data();
    copy.srcPitch = surface.pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = surface.uv_plane;
    copy.WidthInBytes = surface.pitch;
    copy.Height = options.height / 2;
    check_cu(cuMemcpy2D(&copy), "cuMemcpy2D(native UV prefill)");
}

void create_surface(const Options& options, InputSurface& surface)
{
    if (options.input == InputKind::Linear) {
        check_cu(
            cuMemAllocPitch(
                &surface.linear,
                &surface.pitch,
                options.width,
                options.height * 3 / 2,
                16),
            "cuMemAllocPitch(linear NV12)");
        surface.register_pitch = surface.pitch;
        initialize_neutral_uv(options, surface);
        return;
    }

    CUDA_ARRAY3D_DESCRIPTOR descriptor = {};
    descriptor.Width = options.native_storage_width;
    descriptor.Height = options.height;
    descriptor.Depth = 0;
    descriptor.Format = CU_AD_FORMAT_NV12;
    descriptor.NumChannels = 3;
    descriptor.Flags = CUDA_ARRAY3D_VIDEO_ENCODE_DECODE | CUDA_ARRAY3D_SURFACE_LDST;
    check_cu(cuArray3DCreate(&surface.array, &descriptor), "cuArray3DCreate(native NV12)");
    check_cu(cuArrayGetPlane(&surface.y_plane, surface.array, 0), "cuArrayGetPlane(Y)");
    check_cu(cuArrayGetPlane(&surface.uv_plane, surface.array, 1), "cuArrayGetPlane(UV)");
    if (options.native_update == NativeUpdate::Kernel) {
        CUDA_RESOURCE_DESC resource = {};
        resource.resType = CU_RESOURCE_TYPE_ARRAY;
        resource.res.array.hArray = surface.y_plane;
        check_cu(
            cuSurfObjectCreate(&surface.y_surface, &resource),
            "cuSurfObjectCreate(native Y)");
    }
    // Current FFmpeg CUARRAY registration uses the first plane's byte width,
    // not parent Width * NumChannels, for multi-planar video arrays.
    surface.pitch = options.native_storage_width;
    surface.register_pitch = options.native_register_pitch;
    if (surface.pitch > options.width) {
        std::vector<uint8_t> black(surface.pitch * options.height, 0);
        CUDA_MEMCPY2D clear = {};
        clear.srcMemoryType = CU_MEMORYTYPE_HOST;
        clear.srcHost = black.data();
        clear.srcPitch = surface.pitch;
        clear.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        clear.dstArray = surface.y_plane;
        clear.WidthInBytes = surface.pitch;
        clear.Height = options.height;
        check_cu(cuMemcpy2D(&clear), "cuMemcpy2D(native Y initialize)");
    }
    initialize_neutral_uv(options, surface);
}

void describe_native_surface(const InputSurface& surface)
{
    CUDA_ARRAY3D_DESCRIPTOR parent = {};
    CUDA_ARRAY_DESCRIPTOR y = {};
    CUDA_ARRAY_DESCRIPTOR uv = {};
    check_cu(cuArray3DGetDescriptor(&parent, surface.array), "cuArray3DGetDescriptor(parent)");
    check_cu(cuArrayGetDescriptor(&y, surface.y_plane), "cuArrayGetDescriptor(Y)");
    check_cu(cuArrayGetDescriptor(&uv, surface.uv_plane), "cuArrayGetDescriptor(UV)");
    std::cout
        << "native_array parent=" << parent.Width << "x" << parent.Height
        << " format=" << static_cast<unsigned>(parent.Format)
        << " channels=" << parent.NumChannels
        << " flags=0x" << std::hex << parent.Flags << std::dec << "\n"
        << "native_array y_plane=" << y.Width << "x" << y.Height
        << " format=" << static_cast<unsigned>(y.Format)
        << " channels=" << y.NumChannels << "\n"
        << "native_array uv_plane=" << uv.Width << "x" << uv.Height
        << " format=" << static_cast<unsigned>(uv.Format)
        << " channels=" << uv.NumChannels << " row_bytes=" << (uv.Width * uv.NumChannels)
        << " nvenc_register_pitch=" << surface.register_pitch
        << std::endl;
}

CUfunction load_native_update_kernel(const Options& options, CUmodule& module)
{
    if (options.native_update != NativeUpdate::Kernel) {
        return nullptr;
    }
    check_cu(
        cuModuleLoad(&module, options.native_kernel_ptx.c_str()),
        "cuModuleLoad(native update PTX)");
    CUfunction function = nullptr;
    check_cu(
        cuModuleGetFunction(&function, module, "orange_native_nv12_write_luma"),
        "cuModuleGetFunction(orange_native_nv12_write_luma)");
    return function;
}

std::pair<double, double> update_luma_timed(
    const Options& options,
    const DeviceSource& source,
    InputSurface& destination,
    CUfunction native_kernel,
    CUstream stream,
    CUevent start_event,
    CUevent stop_event)
{
    const auto wall_start = Clock::now();
    check_cu(cuEventRecord(start_event, stream), "cuEventRecord(update start)");

    if (options.native_update == NativeUpdate::Kernel) {
        if (!native_kernel || destination.y_surface == 0) {
            throw std::runtime_error("Native update kernel or Y surface object is unavailable");
        }
        CUsurfObject output = destination.y_surface;
        CUdeviceptr input = source.pointer;
        size_t source_pitch = source.pitch;
        uint32_t width = options.width;
        uint32_t height = options.height;
        void* arguments[] = {&output, &input, &source_pitch, &width, &height};
        constexpr uint32_t kBytesPerThread = 16;
        constexpr uint32_t kBlockX = 32;
        constexpr uint32_t kBlockY = 8;
        const uint32_t grid_x =
            (width + kBytesPerThread * kBlockX - 1) / (kBytesPerThread * kBlockX);
        const uint32_t grid_y = (height + kBlockY - 1) / kBlockY;
        check_cu(
            cuLaunchKernel(
                native_kernel,
                grid_x,
                grid_y,
                1,
                kBlockX,
                kBlockY,
                1,
                0,
                stream,
                arguments,
                nullptr),
            "cuLaunchKernel(native Y update)");
    } else {
        CUDA_MEMCPY2D copy = {};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source.pointer;
        copy.srcPitch = source.pitch;
        if (options.input == InputKind::NativeArray) {
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = destination.y_plane;
        } else {
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstDevice = destination.linear;
            copy.dstPitch = destination.pitch;
        }
        copy.WidthInBytes = options.width;
        copy.Height = options.height;
        check_cu(cuMemcpy2DAsync(&copy, stream), "cuMemcpy2DAsync(Y update)");
    }
    check_cu(cuEventRecord(stop_event, stream), "cuEventRecord(update stop)");
    check_cu(cuEventSynchronize(stop_event), "cuEventSynchronize(update stop)");

    float gpu_ms = 0.0f;
    check_cu(cuEventElapsedTime(&gpu_ms, start_event, stop_event), "cuEventElapsedTime(Y update)");
    return {elapsed_ms(wall_start), static_cast<double>(gpu_ms)};
}

uint64_t fnv1a64(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void verify_input_readback(
    const Options& options,
    const DeviceSource& source,
    const InputSurface& surface)
{
    const size_t y_bytes = static_cast<size_t>(options.width) * options.height;
    const size_t uv_bytes = y_bytes / 2;
    std::vector<uint8_t> expected_y(y_bytes);
    std::vector<uint8_t> actual_y(y_bytes);
    std::vector<uint8_t> actual_uv(uv_bytes);

    CUDA_MEMCPY2D source_copy = {};
    source_copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    source_copy.srcDevice = source.pointer;
    source_copy.srcPitch = source.pitch;
    source_copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    source_copy.dstHost = expected_y.data();
    source_copy.dstPitch = options.width;
    source_copy.WidthInBytes = options.width;
    source_copy.Height = options.height;
    check_cu(cuMemcpy2D(&source_copy), "cuMemcpy2D(source Y readback)");

    CUDA_MEMCPY2D y_copy = {};
    if (options.input == InputKind::NativeArray) {
        y_copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        y_copy.srcArray = surface.y_plane;
    } else {
        y_copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        y_copy.srcDevice = surface.linear;
        y_copy.srcPitch = surface.pitch;
    }
    y_copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    y_copy.dstHost = actual_y.data();
    y_copy.dstPitch = options.width;
    y_copy.WidthInBytes = options.width;
    y_copy.Height = options.height;
    check_cu(cuMemcpy2D(&y_copy), "cuMemcpy2D(input Y readback)");

    CUDA_MEMCPY2D uv_copy = {};
    if (options.input == InputKind::NativeArray) {
        uv_copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        uv_copy.srcArray = surface.uv_plane;
    } else {
        uv_copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        uv_copy.srcDevice = surface.linear + surface.pitch * options.height;
        uv_copy.srcPitch = surface.pitch;
    }
    uv_copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    uv_copy.dstHost = actual_uv.data();
    uv_copy.dstPitch = options.width;
    uv_copy.WidthInBytes = options.width;
    uv_copy.Height = options.height / 2;
    check_cu(cuMemcpy2D(&uv_copy), "cuMemcpy2D(input UV readback)");

    uint64_t y_mismatches = 0;
    uint32_t y_max_abs_delta = 0;
    for (size_t index = 0; index < y_bytes; ++index) {
        const uint32_t delta = actual_y[index] > expected_y[index]
            ? actual_y[index] - expected_y[index]
            : expected_y[index] - actual_y[index];
        y_mismatches += delta != 0;
        y_max_abs_delta = std::max(y_max_abs_delta, delta);
    }
    uint64_t uv_mismatches = 0;
    uint8_t uv_min = 255;
    uint8_t uv_max = 0;
    for (uint8_t value : actual_uv) {
        uv_mismatches += value != 128;
        uv_min = std::min(uv_min, value);
        uv_max = std::max(uv_max, value);
    }

    auto write_bytes = [&](const std::string& suffix, const std::vector<uint8_t>& bytes) {
        std::ofstream output(options.readback_prefix + suffix, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to open input readback output");
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("Failed to write input readback output");
        }
    };
    write_bytes("_expected_y.y8", expected_y);
    write_bytes("_actual_y.y8", actual_y);
    write_bytes("_actual_uv.nv12uv", actual_uv);

    std::cout << "input_readback"
              << " expected_y_fnv1a64=0x" << std::hex << fnv1a64(expected_y)
              << " actual_y_fnv1a64=0x" << fnv1a64(actual_y)
              << " actual_uv_fnv1a64=0x" << fnv1a64(actual_uv) << std::dec
              << " y_mismatches=" << y_mismatches
              << " y_max_abs_delta=" << y_max_abs_delta
              << " uv_mismatches=" << uv_mismatches
              << " uv_min=" << static_cast<unsigned>(uv_min)
              << " uv_max=" << static_cast<unsigned>(uv_max)
              << std::endl;
}

void configure_encoder(
    const Options& options,
    NvEncoderCuda& encoder,
    NV_ENC_INITIALIZE_PARAMS& initialize,
    NV_ENC_CONFIG& config)
{
    initialize = {NV_ENC_INITIALIZE_PARAMS_VER};
    config = {NV_ENC_CONFIG_VER};
    initialize.encodeConfig = &config;
    encoder.CreateDefaultEncoderParams(
        &initialize,
        NV_ENC_CODEC_HEVC_GUID,
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY);

    initialize.encodeWidth = options.width;
    initialize.encodeHeight = options.height;
    initialize.darWidth = options.width;
    initialize.darHeight = options.height;
    initialize.frameRateNum = std::max<uint32_t>(1, options.fps);
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;

    config.gopLength = options.gop;
    config.frameIntervalP = 1;
    config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = options.bitrate_bps;
    config.rcParams.maxBitRate = options.max_bitrate_bps;
    config.rcParams.vbvBufferSize = options.vbv_buffer_size;
    config.rcParams.enableAQ = 0;
    config.rcParams.enableTemporalAQ = 0;
    config.rcParams.enableLookahead = 0;
    config.rcParams.lookaheadDepth = 0;
    config.rcParams.lowDelayKeyFrameScale = 1;
    config.encodeCodecConfig.hevcConfig.idrPeriod = options.gop;
    config.encodeCodecConfig.hevcConfig.chromaFormatIDC = 1;
    config.encodeCodecConfig.hevcConfig.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    config.encodeCodecConfig.hevcConfig.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    config.monoChromeEncoding = options.monochrome ? 1 : 0;
}

void append_packets(
    std::ofstream& output,
    const std::vector<std::vector<uint8_t>>& packets,
    uint64_t& packet_count,
    uint64_t& byte_count)
{
    for (const auto& packet : packets) {
        ++packet_count;
        byte_count += packet.size();
        if (output && !packet.empty()) {
            output.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
        }
    }
}

double percentile(std::vector<double> values, double percentile_value)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(std::ceil(percentile_value * values.size() / 100.0)) - 1);
    return values[index];
}

double mean(const std::vector<double>& values)
{
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

void print_metric(const char* name, const std::vector<double>& values)
{
    const double maximum = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    std::cout << "  " << name
              << " mean=" << mean(values)
              << " p50=" << percentile(values, 50.0)
              << " p95=" << percentile(values, 95.0)
              << " max=" << maximum << " ms\n";
}

void write_csv(const Options& options, const std::vector<FrameSample>& samples)
{
    if (options.csv_path.empty()) {
        return;
    }
    std::ofstream csv(options.csv_path, std::ios::trunc);
    if (!csv) {
        throw std::runtime_error("Failed to open CSV: " + options.csv_path);
    }
    csv << "frame_index,phase,slot,source,input,update,copy_wall_ms,copy_gpu_ms,"
           "encode_wall_ms,map_ms,encode_picture_ms,completion_wait_ms,lock_bitstream_ms,"
           "bitstream_copy_ms,unlock_bitstream_ms,unmap_ms,returned_packets,returned_bytes,"
           "source_layout,source_pitch,input_pitch,pacing,timeline_frame_index,burst_index,frame_in_burst,"
           "idle_periods_before,scheduled_steady_ns,loop_start_steady_ns,input_ready_steady_ns,"
           "copy_start_steady_ns,copy_end_steady_ns,encode_start_steady_ns,encode_end_steady_ns,"
           "frame_done_steady_ns,schedule_lateness_ms,native_update,update_wall_ms,update_gpu_ms,"
           "update_start_steady_ns,update_end_steady_ns\n";
    for (const FrameSample& sample : samples) {
        csv << sample.frame_index << ',' << (sample.measured ? "measure" : "warmup") << ','
            << sample.slot << ',' << sample.source << ',' << input_name(options.input) << ','
            << update_name(options.update) << ',' << std::fixed << std::setprecision(6)
            << sample.update_wall_ms << ',' << sample.update_gpu_ms << ',' << sample.encode_wall_ms << ','
            << sample.map_ms << ',' << sample.encode_picture_ms << ',' << sample.completion_wait_ms << ','
            << sample.lock_bitstream_ms << ',' << sample.bitstream_copy_ms << ','
            << sample.unlock_bitstream_ms << ',' << sample.unmap_ms << ','
            << sample.returned_packets << ',' << sample.returned_bytes << ','
            << source_layout_name(options.source_layout) << ','
            << sample.source_pitch << ',' << sample.input_pitch << ','
            << (options.pace ? pacing_name(options.pacing) : "disabled") << ','
            << sample.timeline_frame_index << ',' << sample.burst_index << ','
            << sample.frame_in_burst << ',' << sample.idle_periods_before << ','
            << sample.scheduled_steady_ns << ',' << sample.loop_start_steady_ns << ','
            << sample.input_ready_steady_ns << ',' << sample.update_start_steady_ns << ','
            << sample.update_end_steady_ns << ',' << sample.encode_start_steady_ns << ','
            << sample.encode_end_steady_ns << ',' << sample.frame_done_steady_ns << ','
            << sample.schedule_lateness_ms << ',' << native_update_name(options.native_update) << ','
            << sample.update_wall_ms << ',' << sample.update_gpu_ms << ','
            << sample.update_start_steady_ns << ',' << sample.update_end_steady_ns << '\n';
    }
}

void free_surfaces(std::vector<InputSurface>& surfaces)
{
    for (InputSurface& surface : surfaces) {
        if (surface.y_surface) {
            cuSurfObjectDestroy(surface.y_surface);
        }
        if (surface.array) {
            cuArrayDestroy(surface.array);
        }
        if (surface.linear) {
            cuMemFree(surface.linear);
        }
        surface = {};
    }
}

void free_sources(std::vector<DeviceSource>& sources)
{
    for (DeviceSource& source : sources) {
        if (source.pointer) {
            cuMemFree(source.pointer);
        }
        source = {};
    }
}

}  // namespace

int main(int argc, char** argv)
{
    CUdevice device = 0;
    CUcontext context = nullptr;
    CUstream stream = nullptr;
    CUevent update_start_event = nullptr;
    CUevent update_stop_event = nullptr;
    CUmodule native_update_module = nullptr;
    CUfunction native_update_kernel = nullptr;
    std::vector<DeviceSource> sources;
    std::vector<InputSurface> surfaces;

    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        check_cu(cuInit(0), "cuInit");
        int driver_version = 0;
        check_cu(cuDriverGetVersion(&driver_version), "cuDriverGetVersion");
        check_cu(cuDeviceGet(&device, options.gpu_id), "cuDeviceGet");
        check_cu(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
        check_cu(cuCtxSetCurrent(context), "cuCtxSetCurrent");
        check_cu(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
        check_cu(cuEventCreate(&update_start_event, CU_EVENT_DEFAULT), "cuEventCreate(start)");
        check_cu(cuEventCreate(&update_stop_event, CU_EVENT_DEFAULT), "cuEventCreate(stop)");
        native_update_kernel = load_native_update_kernel(options, native_update_module);

        char device_name[256] = {};
        check_cu(cuDeviceGetName(device_name, sizeof(device_name), device), "cuDeviceGetName");
        std::cout
            << "NVENC native-NV12 probe\n"
            << "  gpu=" << options.gpu_id << " name=" << device_name << "\n"
            << "  cuda_driver_api=" << driver_version << " cuda_header=" << CUDA_VERSION << "\n"
            << "  nvenc_header=" << NVENCAPI_MAJOR_VERSION << '.' << NVENCAPI_MINOR_VERSION << "\n"
            << "  input=" << input_name(options.input) << " update=" << update_name(options.update)
            << " native_update=" << native_update_name(options.native_update) << "\n"
            << "  resolution=" << options.width << 'x' << options.height
            << " fps=" << options.fps << " frames=" << options.frames
            << " warmup=" << options.warmup_frames << "\n"
            << "  start_at_monotonic_ns=" << options.start_at_monotonic_ns << "\n"
            << "  pacing=" << (options.pace ? pacing_name(options.pacing) : "disabled")
            << " split_gop_on_frames=" << options.gop
            << " split_gop_idle_frames=" << options.split_gop_idle_frames << "\n"
            << "  codec=hevc preset=p1 tuning=low_latency rc=vbr bitrate=" << options.bitrate_bps
            << " aq=0 temporal_aq=0 lookahead=0 gop=" << options.gop << "\n"
            << "  raw_source=" << (options.raw_file.empty() ? "generated-structured" : options.raw_file)
            << " source_layout=" << source_layout_name(options.source_layout)
            << " native_kernel_ptx="
            << (options.native_kernel_ptx.empty() ? "none" : options.native_kernel_ptx)
            << std::endl;

        const auto setup_start = Clock::now();
        sources = make_sources(options);
        std::cout << "source_nv12 count=" << sources.size()
                  << " layout=" << source_layout_name(options.source_layout)
                  << " pitch=" << sources.front().pitch
                  << " bytes_each=" << (sources.front().pitch * options.height * 3 / 2)
                  << std::endl;

        double prefill_copy_wall_ms = 0.0;
        double prefill_copy_gpu_ms = 0.0;
        uint64_t submitted_frames = 0;
        uint64_t packet_count = 0;
        uint64_t byte_count = 0;
        double setup_wall_ms = 0.0;
        double submission_seconds = 0.0;
        std::vector<FrameSample> samples;
        samples.reserve(static_cast<size_t>(options.frames));

        std::ofstream bitstream;
        if (!options.bitstream_path.empty()) {
            bitstream.open(options.bitstream_path, std::ios::binary | std::ios::trunc);
            if (!bitstream) {
                throw std::runtime_error("Failed to open bitstream output: " + options.bitstream_path);
            }
        }

        {
            NvEncoderCuda encoder(
                context,
                options.width,
                options.height,
                NV_ENC_BUFFER_FORMAT_NV12,
                options.extra_output_delay);
            NV_ENC_INITIALIZE_PARAMS initialize = {NV_ENC_INITIALIZE_PARAMS_VER};
            NV_ENC_CONFIG config = {NV_ENC_CONFIG_VER};
            configure_encoder(options, encoder, initialize, config);
            encoder.SetExternalInputBufferMode(true);
            encoder.CreateEncoder(&initialize);
            encoder.PrepareExternalRegisteredSlots();

            const uint32_t surface_count = encoder.GetEncoderBufferCount();
            const bool registered_source = options.update == UpdateMode::RegisteredSource;
            if (registered_source) {
                if (sources.size() < surface_count) {
                    throw std::runtime_error(
                        "registered-source requires at least one cached source per encoder buffer (" +
                        std::to_string(surface_count) + ")");
                }
                for (DeviceSource& source : sources) {
                    source.registered = encoder.RegisterExternalResource(
                        reinterpret_cast<void*>(source.pointer),
                        NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
                        static_cast<int>(source.pitch));
                }
                std::cout << "registered_source_nv12 count=" << sources.size()
                          << " pitch=" << sources.front().pitch
                          << " bytes_each=" << (sources.front().pitch * options.height * 3 / 2)
                          << std::endl;
            } else {
                surfaces.resize(surface_count);
                for (uint32_t slot = 0; slot < surface_count; ++slot) {
                    create_surface(options, surfaces[slot]);
                    void* resource = options.input == InputKind::NativeArray
                        ? reinterpret_cast<void*>(surfaces[slot].array)
                        : reinterpret_cast<void*>(surfaces[slot].linear);
                    const NV_ENC_INPUT_RESOURCE_TYPE resource_type = options.input == InputKind::NativeArray
                        ? NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY
                        : NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
                    surfaces[slot].registered = encoder.RegisterExternalResource(
                        resource, resource_type, static_cast<int>(surfaces[slot].register_pitch));
                }
                if (options.input == InputKind::NativeArray && !surfaces.empty()) {
                    describe_native_surface(surfaces.front());
                } else if (!surfaces.empty()) {
                    std::cout << "linear_surface pitch=" << surfaces.front().pitch
                              << " bytes=" << (surfaces.front().pitch * options.height * 3 / 2)
                              << std::endl;
                }
            }

            if (options.update == UpdateMode::Prefilled) {
                for (uint32_t slot = 0; slot < surface_count; ++slot) {
                    const auto timing = update_luma_timed(
                        options,
                        sources[slot % sources.size()],
                        surfaces[slot],
                        native_update_kernel,
                        stream,
                        update_start_event,
                        update_stop_event);
                    prefill_copy_wall_ms += timing.first;
                    prefill_copy_gpu_ms += timing.second;
                }
            }
            if (!options.readback_prefix.empty()) {
                if (registered_source) {
                    InputSurface source_view;
                    source_view.linear = sources.front().pointer;
                    source_view.pitch = sources.front().pitch;
                    verify_input_readback(options, sources.front(), source_view);
                } else if (options.update == UpdateMode::PerFrame) {
                    update_luma_timed(
                        options,
                        sources.front(),
                        surfaces.front(),
                        native_update_kernel,
                        stream,
                        update_start_event,
                        update_stop_event);
                    verify_input_readback(options, sources.front(), surfaces.front());
                } else {
                    verify_input_readback(options, sources.front(), surfaces.front());
                }
            }

            encoder.SetIOCudaStreams(
                reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream),
                reinterpret_cast<NV_ENC_CUSTREAM_PTR>(&stream));

            const auto frame_period = options.fps > 0
                ? std::chrono::nanoseconds(1000000000LL / options.fps)
                : std::chrono::nanoseconds(0);
            std::vector<std::vector<uint8_t>> packets;
            setup_wall_ms = elapsed_ms(setup_start);
            const auto setup_done = Clock::now();
            auto run_start = setup_done;
            if (options.start_at_monotonic_ns != 0) {
                run_start = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                    std::chrono::nanoseconds(options.start_at_monotonic_ns)));
                if (run_start <= setup_done) {
                    throw std::runtime_error(
                        "--start-at-monotonic-ns expired before encoder setup completed");
                }
            }

            for (uint64_t frame_index = 0;
                 frame_index < options.frames && !g_stop_requested.load(std::memory_order_acquire);
                 ++frame_index) {
                const auto loop_start = Clock::now();
                while (!encoder.WaitForNextInputFrameAvailable(100)) {
                    if (g_stop_requested.load(std::memory_order_acquire)) {
                        break;
                    }
                }
                if (g_stop_requested.load(std::memory_order_acquire)) {
                    break;
                }

                FrameSample sample;
                sample.frame_index = frame_index;
                sample.slot = encoder.GetNextInputFrameIndex();
                sample.timeline_frame_index = scheduled_timeline_frame(options, frame_index);
                sample.burst_index = frame_index / options.gop;
                sample.frame_in_burst = static_cast<uint32_t>(frame_index % options.gop);
                sample.idle_periods_before = options.pacing == PacingMode::SplitGop &&
                        sample.frame_in_burst == 0 && frame_index != 0
                    ? options.split_gop_idle_frames
                    : 0;
                const auto scheduled_time = run_start +
                    frame_period * static_cast<int64_t>(sample.timeline_frame_index);
                const auto input_ready = Clock::now();
                if ((options.pace && frame_period.count() > 0) ||
                    (frame_index == 0 && options.start_at_monotonic_ns != 0)) {
                    std::this_thread::sleep_until(scheduled_time);
                }
                const auto frame_start = Clock::now();
                sample.scheduled_steady_ns = steady_ns(scheduled_time);
                sample.loop_start_steady_ns = steady_ns(loop_start);
                sample.input_ready_steady_ns = steady_ns(input_ready);
                sample.schedule_lateness_ms =
                    std::chrono::duration<double, std::milli>(frame_start - scheduled_time).count();
                sample.source = options.update == UpdateMode::PerFrame || registered_source
                    ? static_cast<uint32_t>(frame_index % sources.size())
                    : static_cast<uint32_t>(sample.slot % sources.size());
                sample.source_pitch = static_cast<uint32_t>(sources[sample.source].pitch);
                sample.measured = frame_index >= options.warmup_frames;

                if (options.update == UpdateMode::PerFrame) {
                    sample.update_start_steady_ns = steady_ns(Clock::now());
                    const auto timing = update_luma_timed(
                        options,
                        sources[sample.source],
                        surfaces[sample.slot],
                        native_update_kernel,
                        stream,
                        update_start_event,
                        update_stop_event);
                    sample.update_wall_ms = timing.first;
                    sample.update_gpu_ms = timing.second;
                    sample.update_end_steady_ns = steady_ns(Clock::now());
                }

                const NV_ENC_REGISTERED_PTR registered = registered_source
                    ? sources[sample.source].registered
                    : surfaces[sample.slot].registered;
                const size_t input_pitch = registered_source
                    ? sources[sample.source].pitch
                    : surfaces[sample.slot].pitch;
                sample.input_pitch = static_cast<uint32_t>(input_pitch);
                encoder.SetNextInputRegisteredResource(registered);
                NV_ENC_PIC_PARAMS picture = {NV_ENC_PIC_PARAMS_VER};
                picture.frameIdx = static_cast<uint32_t>(frame_index);
                picture.inputTimeStamp = sample.timeline_frame_index;
                picture.inputDuration = 1;
                picture.inputPitch = static_cast<uint32_t>(input_pitch);

                NvEncoderEncodeFrameTiming encode_timing;
                const auto encode_start = Clock::now();
                sample.encode_start_steady_ns = steady_ns(encode_start);
                encoder.EncodeFrame(packets, &picture, nullptr, nullptr, nullptr, &encode_timing);
                const auto encode_end = Clock::now();
                sample.encode_end_steady_ns = steady_ns(encode_end);
                sample.encode_wall_ms = elapsed_ms(encode_start);
                sample.map_ms = ns_to_ms(encode_timing.map_input_resource_ns);
                sample.encode_picture_ms = ns_to_ms(encode_timing.encode_picture_ns);
                sample.completion_wait_ms = ns_to_ms(encode_timing.completion_wait_ns);
                sample.lock_bitstream_ms = ns_to_ms(encode_timing.lock_bitstream_ns);
                sample.bitstream_copy_ms = ns_to_ms(encode_timing.bitstream_copy_ns);
                sample.unlock_bitstream_ms = ns_to_ms(encode_timing.unlock_bitstream_ns);
                sample.unmap_ms = ns_to_ms(encode_timing.unmap_input_resource_ns);
                sample.returned_packets = static_cast<uint32_t>(packets.size());
                for (const auto& packet : packets) {
                    sample.returned_bytes += packet.size();
                }
                append_packets(bitstream, packets, packet_count, byte_count);
                sample.frame_done_steady_ns = steady_ns(Clock::now());
                samples.push_back(sample);
                ++submitted_frames;

                if (options.pace && frame_period.count() > 0) {
                    const uint64_t next_timeline_frame = scheduled_timeline_frame(options, frame_index + 1);
                    std::this_thread::sleep_until(
                        run_start + frame_period * static_cast<int64_t>(next_timeline_frame));
                }
            }
            submission_seconds = std::chrono::duration<double>(Clock::now() - run_start).count();

            std::vector<std::vector<uint8_t>> final_packets;
            encoder.EndEncode(final_packets);
            append_packets(bitstream, final_packets, packet_count, byte_count);
        }

        write_csv(options, samples);

        std::vector<double> update_wall;
        std::vector<double> update_gpu;
        std::vector<double> encode_wall;
        std::vector<double> map;
        std::vector<double> encode_picture;
        std::vector<double> lock;
        std::vector<double> schedule_lateness;
        for (const FrameSample& sample : samples) {
            if (!sample.measured) {
                continue;
            }
            update_wall.push_back(sample.update_wall_ms);
            update_gpu.push_back(sample.update_gpu_ms);
            encode_wall.push_back(sample.encode_wall_ms);
            map.push_back(sample.map_ms);
            encode_picture.push_back(sample.encode_picture_ms);
            lock.push_back(sample.lock_bitstream_ms);
            schedule_lateness.push_back(sample.schedule_lateness_ms);
        }

        const uint64_t timeline_periods = scheduled_timeline_frame(options, submitted_frames);
        const double scheduled_seconds = options.fps > 0
            ? static_cast<double>(timeline_periods) / options.fps
            : 0.0;

        std::cout << std::fixed << std::setprecision(6)
                  << "summary\n"
                  << "  submitted_frames=" << submitted_frames
                  << " measured_frames=" << update_wall.size()
                  << " packets=" << packet_count << " bytes=" << byte_count << "\n"
                  << "  setup_wall_ms=" << setup_wall_ms << "\n"
                  << "  achieved_fps=" << (submission_seconds > 0.0 ? submitted_frames / submission_seconds : 0.0) << "\n"
                  << "  timeline_periods=" << timeline_periods
                  << " scheduled_seconds=" << scheduled_seconds
                  << " scheduled_average_fps="
                  << (scheduled_seconds > 0.0 ? submitted_frames / scheduled_seconds : 0.0) << "\n"
                  << "  prefill_copy_wall_total_ms=" << prefill_copy_wall_ms
                  << " prefill_copy_gpu_total_ms=" << prefill_copy_gpu_ms << "\n";
        print_metric("update_wall_ms", update_wall);
        print_metric("update_gpu_ms", update_gpu);
        print_metric("encode_wall_ms", encode_wall);
        print_metric("map_ms", map);
        print_metric("encode_picture_ms", encode_picture);
        print_metric("lock_bitstream_ms", lock);
        print_metric("schedule_lateness_ms", schedule_lateness);
        if (!options.bitstream_path.empty()) {
            std::cout << "  bitstream=" << options.bitstream_path << '\n';
        }
        if (!options.csv_path.empty()) {
            std::cout << "  csv=" << options.csv_path << '\n';
        }

        check_cu(cuStreamSynchronize(stream), "cuStreamSynchronize(before surface destroy)");
        free_surfaces(surfaces);
        free_sources(sources);
        if (native_update_module) {
            check_cu(cuModuleUnload(native_update_module), "cuModuleUnload(native update PTX)");
            native_update_module = nullptr;
            native_update_kernel = nullptr;
        }
        check_cu(cuEventDestroy(update_stop_event), "cuEventDestroy(stop)");
        update_stop_event = nullptr;
        check_cu(cuEventDestroy(update_start_event), "cuEventDestroy(start)");
        update_start_event = nullptr;
        check_cu(cuStreamDestroy(stream), "cuStreamDestroy");
        stream = nullptr;
        check_cu(cuDevicePrimaryCtxRelease(device), "cuDevicePrimaryCtxRelease");
        context = nullptr;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nvenc_native_nv12_probe: " << error.what() << std::endl;
        if (context) {
            cuCtxSetCurrent(context);
            if (stream) {
                (void)cuStreamSynchronize(stream);
            }
            free_surfaces(surfaces);
            free_sources(sources);
            if (native_update_module) {
                cuModuleUnload(native_update_module);
            }
            if (update_stop_event) {
                cuEventDestroy(update_stop_event);
            }
            if (update_start_event) {
                cuEventDestroy(update_start_event);
            }
            if (stream) {
                cuStreamDestroy(stream);
            }
            cuDevicePrimaryCtxRelease(device);
        }
        return 1;
    }
}
