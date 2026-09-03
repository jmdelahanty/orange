#include "spatial_roi_recorder_video_sanity.h"

#include "gui/spatial_layout/sha256.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace orange::spatial_roi::recording {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using orange::gui::spatial_layout::checksum::StreamingSha256;

constexpr std::size_t kMaxErrorBytes = 512;
constexpr std::size_t kIoBufferBytes = 64U * 1024U;
constexpr std::size_t kHashBufferBytes = 1024U * 1024U;
constexpr std::uint64_t kMaxDecodedFrames = 100ULL * 1000ULL * 1000ULL;
constexpr std::uint32_t kMaxDimension = 32768U;
constexpr std::uint64_t kMaxPixels = 268ULL * 1000ULL * 1000ULL;
// The bundled FFmpeg decoder aligns its buffer width to a 64-byte SIMD stride,
// and HEVC may additionally expose a coded picture larger than the visible
// conformance-window raster.  Reserve one conservative 64x64 envelope while
// continuing to require exact visible dimensions from both the MP4 and every
// decoded AVFrame.  NVENC exercises this for valid 2256-pixel ROI rasters.
constexpr std::uint32_t kHevcDecoderAllocationAlignment = 64U;
constexpr double kMinFrameRate = 0.001;
constexpr double kMaxFrameRate = 10000.0;
constexpr auto kMaxTimeout = std::chrono::hours(1);

// FFmpeg does not expose a packet-allocation limit on AVFormatContext.  The
// media-size bound is therefore the strongest packet bound available to this
// probe before demux allocation; the packet is checked again immediately
// after av_read_frame.  The codec-side max_pixels guard is set before opening
// the decoder.  Do not replace the authenticated media bound with a fixed
// convenience cap here.
constexpr std::uint64_t kMaxPacketBytesForAvio =
    static_cast<std::uint64_t>(std::numeric_limits<int>::max());

bool fail(std::string* error_out, std::string message)
{
    if (message.size() > kMaxErrorBytes) {
        message.resize(kMaxErrorBytes);
    }
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

void clear_error(std::string* error_out)
{
    if (error_out != nullptr) {
        error_out->clear();
    }
}

std::string av_error(const std::string& operation, const int value)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    if (av_strerror(value, text.data(), text.size()) != 0) {
        text[0] = '\0';
    }
    std::string message = operation;
    if (text[0] != '\0') {
        message += ": ";
        message += text.data();
    }
    return message;
}

bool expired(const Deadline deadline) noexcept
{
    return Clock::now() >= deadline;
}

bool valid_dimensions(const std::uint32_t width,
                     const std::uint32_t height,
                     std::uint64_t* pixels_out,
                     std::string* error_out)
{
    if (pixels_out == nullptr || width == 0 || height == 0 ||
        width > kMaxDimension || height > kMaxDimension) {
        return fail(error_out, "video sanity dimensions are outside the bound");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels == 0 || pixels > kMaxPixels) {
        return fail(error_out, "video sanity pixel count is outside the bound");
    }
    *pixels_out = pixels;
    return true;
}

bool hevc_decoder_pixel_bound(const std::uint32_t visible_width,
                              const std::uint32_t visible_height,
                              std::uint64_t* pixels_out,
                              std::string* error_out)
{
    if (pixels_out == nullptr) {
        return fail(error_out, "video sanity decoder pixel output is null");
    }
    const auto align_for_decoder = [](const std::uint32_t value) {
        return (static_cast<std::uint64_t>(value) +
                kHevcDecoderAllocationAlignment - 1U) /
               kHevcDecoderAllocationAlignment *
               kHevcDecoderAllocationAlignment;
    };
    const std::uint64_t coded_width_bound = align_for_decoder(visible_width);
    const std::uint64_t coded_height_bound = align_for_decoder(visible_height);
    if (coded_width_bound == 0 || coded_height_bound == 0 ||
        coded_width_bound > kMaxDimension ||
        coded_height_bound > kMaxDimension ||
        coded_width_bound >
            std::numeric_limits<std::uint64_t>::max() / coded_height_bound) {
        return fail(error_out,
                    "video sanity HEVC coded raster is outside the bound");
    }
    const std::uint64_t pixels = coded_width_bound * coded_height_bound;
    if (pixels == 0 || pixels > kMaxPixels) {
        return fail(error_out,
                    "video sanity HEVC coded pixel count is outside the bound");
    }
    *pixels_out = pixels;
    return true;
}

bool valid_path(const std::string& path, std::string* error_out)
{
    if (path.empty() || path.size() > kSpatialRoiRecorderArtifactMaxPathBytes ||
        path.find('\0') != std::string::npos || path.front() == '/') {
        return fail(error_out, "video sanity path is not a bounded relative path");
    }
    return true;
}

struct IoState final {
    int fd = -1;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
    Deadline deadline{};
    bool timed_out = false;
    bool io_failed = false;
};

int read_packet(void* opaque, std::uint8_t* buffer, const int buffer_size)
{
    auto* state = static_cast<IoState*>(opaque);
    if (state == nullptr || buffer == nullptr || buffer_size <= 0 ||
        state->fd < 0) {
        return AVERROR(EINVAL);
    }
    if (expired(state->deadline)) {
        state->timed_out = true;
        return AVERROR_EXIT;
    }
    if (state->offset >= state->size) {
        return AVERROR_EOF;
    }

    const std::uint64_t available = state->size - state->offset;
    const std::size_t requested = static_cast<std::size_t>(std::min<
        std::uint64_t>(available, static_cast<std::uint64_t>(buffer_size)));
    ssize_t count = -1;
    do {
        count = ::pread(state->fd,
                        buffer,
                        requested,
                        static_cast<off_t>(state->offset));
    } while (count < 0 && errno == EINTR && !expired(state->deadline));
    if (count < 0) {
        if (expired(state->deadline)) {
            state->timed_out = true;
            return AVERROR_EXIT;
        }
        state->io_failed = true;
        return AVERROR(errno);
    }
    if (count == 0) {
        state->io_failed = true;
        return AVERROR(EIO);
    }
    state->offset += static_cast<std::uint64_t>(count);
    return static_cast<int>(count);
}

int64_t seek_packet(void* opaque, const int64_t offset, const int whence)
{
    auto* state = static_cast<IoState*>(opaque);
    if (state == nullptr || state->fd < 0) {
        return AVERROR(EINVAL);
    }
    if (expired(state->deadline)) {
        state->timed_out = true;
        return AVERROR_EXIT;
    }
    if ((whence & AVSEEK_SIZE) != 0) {
        return static_cast<int64_t>(state->size);
    }

    const int origin = whence & 0xFFFF;
    std::int64_t base = 0;
    switch (origin) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            if (state->offset >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return AVERROR(EOVERFLOW);
            }
            base = static_cast<std::int64_t>(state->offset);
            break;
        case SEEK_END:
            if (state->size >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return AVERROR(EOVERFLOW);
            }
            base = static_cast<std::int64_t>(state->size);
            break;
        default:
            return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)) {
        return AVERROR(EOVERFLOW);
    }
    const std::int64_t destination = base + offset;
    if (destination < 0 ||
        static_cast<std::uint64_t>(destination) > state->size) {
        return AVERROR(EINVAL);
    }
    state->offset = static_cast<std::uint64_t>(destination);
    return destination;
}

int interrupt_callback(void* opaque)
{
    auto* state = static_cast<IoState*>(opaque);
    if (state == nullptr) {
        return 1;
    }
    if (expired(state->deadline)) {
        state->timed_out = true;
        return 1;
    }
    return 0;
}

class FormatResources final {
public:
    FormatResources() = default;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;

    ~FormatResources()
    {
        if (format != nullptr) {
            // The AVIO is retained separately and is always freed below.
            // Clearing pb also makes cleanup safe on every avformat_open_input
            // error path, including versions that retain the format object.
            format->pb = nullptr;
            avformat_close_input(&format);
        }
        if (io != nullptr) {
            avio_context_free(&io);
        }
    }

    FormatResources(const FormatResources&) = delete;
    FormatResources& operator=(const FormatResources&) = delete;
};

class CodecResources final {
public:
    CodecResources() = default;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    ~CodecResources()
    {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
    }

    CodecResources(const CodecResources&) = delete;
    CodecResources& operator=(const CodecResources&) = delete;
};

struct ProbeData final {
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root;
    std::shared_ptr<SpatialRoiRecorderArtifactFile> video_file;
    SpatialRoiRecorderArtifactIdentity artifact_root_identity{};
    SpatialRoiRecorderArtifactIdentity video_identity{};
    std::string video_relative_path;
    std::uint64_t size_bytes = 0;
    std::string sha256;
    std::string duration_seconds;
    std::string frame_rate;
    std::string time_base;
    bool has_decoded_pts = false;
    std::int64_t first_decoded_pts = AV_NOPTS_VALUE;
    std::int64_t last_decoded_pts = AV_NOPTS_VALUE;
    std::string container;
    std::string codec;
    std::string decoder;
    std::string pixel_format;
    std::string color_range;
    std::uint32_t bit_depth = 0;
    std::string chroma_subsampling;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frame_count = 0;
    std::vector<SpatialRoiRecorderVideoSanitySample> samples;
};

struct DecodedTimeline final {
    bool saw_pts = false;
    bool saw_missing_pts = false;
    std::int64_t first_pts = AV_NOPTS_VALUE;
    std::int64_t previous_pts = AV_NOPTS_VALUE;
};

bool identity_matches(const SpatialRoiRecorderArtifactIdentity& left,
                      const SpatialRoiRecorderArtifactIdentity& right) noexcept
{
    return left.device == right.device && left.inode == right.inode &&
           left.inode != 0;
}

bool check_retained_file(
    const SpatialRoiRecorderArtifactFile& file,
    const SpatialRoiRecorderArtifactIdentity& artifact_root_identity,
    const std::uint64_t expected_size,
    SpatialRoiRecorderArtifactIdentity* identity_out,
    std::string* error_out)
{
    if (!file.valid() || file.access() != SpatialRoiRecorderArtifactFileAccess::kReadOnly ||
        file.relative_path().empty() ||
        !identity_matches(file.artifact_root_identity(), artifact_root_identity)) {
        return fail(error_out, "video sanity retained file is not read-only and root-bound");
    }
    std::string binding_error;
    if (!file.VerifyCurrentBinding(&binding_error)) {
        return fail(error_out, "video sanity retained file binding check failed");
    }
    struct stat stat_value {};
    if (::fstat(file.borrowed_fd(), &stat_value) != 0) {
        return fail(error_out, "video sanity could not stat the retained descriptor");
    }
    if (!S_ISREG(stat_value.st_mode) || stat_value.st_size <= 0) {
        return fail(error_out, "video sanity media is not a nonempty regular file");
    }
    if (static_cast<std::uint64_t>(stat_value.st_size) != expected_size) {
        return fail(error_out, "video sanity media size changed during the probe");
    }
    const SpatialRoiRecorderArtifactIdentity identity{
        static_cast<std::uint64_t>(stat_value.st_dev),
        static_cast<std::uint64_t>(stat_value.st_ino)};
    if (!identity_matches(identity, file.identity()) || identity_out == nullptr) {
        return fail(error_out, "video sanity media inode identity changed");
    }
    if (!file.VerifyCurrentBinding(&binding_error)) {
        return fail(error_out, "video sanity retained file binding changed during stat");
    }
    *identity_out = identity;
    return true;
}

bool hash_retained_file(const SpatialRoiRecorderArtifactFile& file,
                        const std::uint64_t size,
                        const Deadline deadline,
                        std::string* digest_out,
                        std::string* error_out)
{
    if (digest_out == nullptr) {
        return fail(error_out, "video sanity digest output is null");
    }
    StreamingSha256 hasher;
    std::array<std::uint8_t, kHashBufferBytes> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        if (expired(deadline)) {
            return fail(error_out, "video sanity deadline expired while hashing");
        }
        const std::size_t requested = static_cast<std::size_t>(std::min<
            std::uint64_t>(size - offset, buffer.size()));
        ssize_t count = -1;
        do {
            count = ::pread(file.borrowed_fd(),
                            buffer.data(),
                            requested,
                            static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR && !expired(deadline));
        if (count < 0) {
            if (expired(deadline)) {
                return fail(error_out,
                            "video sanity deadline expired while hashing");
            }
            return fail(error_out, "video sanity could not hash the retained descriptor");
        }
        if (count == 0) {
            return fail(error_out, "video sanity hash read made no progress");
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    *digest_out = "sha256:" + hasher.final_hex();
    return true;
}

bool is_mov_family(const AVInputFormat* format) noexcept
{
    if (format == nullptr || format->name == nullptr) {
        return false;
    }
    const std::string names(format->name);
    std::size_t begin = 0;
    while (begin <= names.size()) {
        const std::size_t comma = names.find(',', begin);
        const std::size_t end = comma == std::string::npos ? names.size() : comma;
        const std::string token = names.substr(begin, end - begin);
        if (token == "mov" || token == "mp4" || token == "m4a" ||
            token == "3gp" || token == "3g2" || token == "mj2") {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return false;
}

struct PixelSemantics final {
    const AVPixFmtDescriptor* descriptor = nullptr;
    const char* canonical_name = nullptr;
    std::uint32_t bit_depth = 0;
    const char* chroma_subsampling = nullptr;
};

bool validate_profile_pixel_format(const int pixel_format,
                                   PixelSemantics* semantics_out,
                                   std::string* error_out)
{
    if (semantics_out == nullptr || pixel_format < 0) {
        return fail(error_out,
                    "video sanity stream does not declare a pixel format");
    }
    const auto* descriptor = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(pixel_format));
    if (descriptor == nullptr || descriptor->name == nullptr ||
        (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL |
                              AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                              AV_PIX_FMT_FLAG_FLOAT)) != 0 ||
        descriptor->nb_components != 3 || descriptor->log2_chroma_w != 1 ||
        descriptor->log2_chroma_h != 1 || descriptor->comp[0].depth != 8 ||
        descriptor->comp[1].depth != 8 || descriptor->comp[2].depth != 8 ||
        descriptor->comp[0].plane != 0 || descriptor->comp[0].step <= 0 ||
        (std::strcmp(descriptor->name, "yuv420p") != 0 &&
         std::strcmp(descriptor->name, "yuvj420p") != 0 &&
         std::strcmp(descriptor->name, "nv12") != 0)) {
        return fail(error_out,
                    "video sanity pixel format is not supported full-range 8-bit 4:2:0");
    }
    *semantics_out = {descriptor, descriptor->name, 8U, "4:2:0"};
    return true;
}

bool compatible_profile_pixel_formats(const PixelSemantics& container,
                                      const PixelSemantics& decoded) noexcept
{
    // yuv420p/yuvj420p/NV12 are distinct storage layouts but represent the
    // same 8-bit 4:2:0 luma/chroma contract. Full-range is checked separately
    // on both the container parameters and each decoded frame.
    return container.bit_depth == decoded.bit_depth &&
           std::strcmp(container.chroma_subsampling,
                       decoded.chroma_subsampling) == 0;
}

bool validate_luma_plane(const AVFrame& frame,
                         const AVPixFmtDescriptor& descriptor,
                         const std::uint32_t width,
                         const std::uint32_t height,
                         std::string* error_out)
{
    if (frame.data[0] == nullptr || frame.linesize[0] <= 0 ||
        descriptor.comp[0].plane != 0 || descriptor.comp[0].step <= 0 ||
        descriptor.comp[0].offset < 0 || descriptor.comp[0].depth != 8 ||
        descriptor.comp[0].shift < 0 || descriptor.comp[0].shift >= 16 ||
        (descriptor.flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL |
                             AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                             AV_PIX_FMT_FLAG_FLOAT)) != 0) {
        return fail(error_out, "video sanity decoded frame has no usable luma plane");
    }

    if (height == 0 || frame.buf[0] == nullptr ||
        frame.buf[0]->data == nullptr || frame.buf[0]->size <= 0) {
        return fail(error_out,
                    "video sanity decoded frame has no bounded luma buffer");
    }
    const std::uint64_t sample_bytes = descriptor.comp[0].depth <= 8 ? 1U : 2U;
    const std::uint64_t row_end =
        static_cast<std::uint64_t>(width - 1U) *
            static_cast<std::uint64_t>(descriptor.comp[0].step) +
        static_cast<std::uint64_t>(descriptor.comp[0].offset) + sample_bytes;
    if (row_end > static_cast<std::uint64_t>(frame.linesize[0])) {
        return fail(error_out, "video sanity luma linesize is shorter than the raster");
    }
    const std::uint64_t plane_end =
        static_cast<std::uint64_t>(height - 1U) *
            static_cast<std::uint64_t>(frame.linesize[0]) + row_end;
    if (plane_end > static_cast<std::uint64_t>(frame.buf[0]->size)) {
        return fail(error_out,
                    "video sanity decoded luma plane exceeds its buffer");
    }
    const std::uintptr_t buffer_begin =
        reinterpret_cast<std::uintptr_t>(frame.buf[0]->data);
    const std::uintptr_t data_begin =
        reinterpret_cast<std::uintptr_t>(frame.data[0]);
    if (data_begin < buffer_begin ||
        data_begin - buffer_begin >
            static_cast<std::uintptr_t>(frame.buf[0]->size) ||
        plane_end > static_cast<std::uint64_t>(
                         static_cast<std::uintptr_t>(frame.buf[0]->size) -
                         (data_begin - buffer_begin))) {
        return fail(error_out,
                    "video sanity decoded luma pointer is outside its buffer");
    }
    return true;
}

bool read_luma_sample(const AVFrame& frame,
                      const AVPixFmtDescriptor& descriptor,
                      const std::uint32_t width,
                      const std::uint32_t height,
                      const Deadline deadline,
                      std::vector<SpatialRoiRecorderVideoSanitySample>* samples,
                      const std::uint64_t frame_index,
                      std::string* error_out)
{
    if (samples == nullptr ||
        !validate_luma_plane(frame, descriptor, width, height, error_out)) {
        return false;
    }

    const std::size_t sample_bytes = descriptor.comp[0].depth <= 8 ? 1U : 2U;
    const std::uint32_t depth = static_cast<std::uint32_t>(descriptor.comp[0].depth);
    const std::uint64_t source_max = (1ULL << depth) - 1ULL;
    long double sum = 0.0L;
    long double sum_squares = 0.0L;
    std::uint64_t black = 0;
    std::uint64_t white = 0;
    std::uint32_t minimum = 255;
    std::uint32_t maximum = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        if (expired(deadline)) {
            return fail(error_out,
                        "video sanity deadline expired while computing luma statistics");
        }
        const auto* row = frame.data[0] +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.linesize[0]);
        for (std::uint32_t x = 0; x < width; ++x) {
            if ((x & 0x3ffU) == 0U && expired(deadline)) {
                return fail(error_out,
                            "video sanity deadline expired while computing luma statistics");
            }
            const auto* value_bytes = row +
                static_cast<std::size_t>(x) *
                    static_cast<std::size_t>(descriptor.comp[0].step) +
                static_cast<std::size_t>(descriptor.comp[0].offset);
            std::uint64_t source_value = 0;
            if (sample_bytes == 1U) {
                source_value = value_bytes[0];
            } else {
                std::uint16_t raw = 0;
                std::memcpy(&raw, value_bytes, sizeof(raw));
                if ((descriptor.flags & AV_PIX_FMT_FLAG_BE) != 0) {
                    raw = static_cast<std::uint16_t>((raw >> 8U) |
                                                     (raw << 8U));
                }
                source_value = static_cast<std::uint64_t>(raw) >>
                    static_cast<unsigned int>(descriptor.comp[0].shift);
                source_value &= source_max;
            }
            const std::uint32_t value = static_cast<std::uint32_t>(
                (source_value * 255ULL + source_max / 2ULL) / source_max);
            sum += static_cast<long double>(value);
            sum_squares += static_cast<long double>(value) * value;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            if (value < 8U) {
                ++black;
            }
            if (value > 247U) {
                ++white;
            }
        }
    }

    const long double count = static_cast<long double>(width) * height;
    const long double mean = sum / count;
    const long double variance = std::max(0.0L, sum_squares / count - mean * mean);
    const double mean_double = static_cast<double>(mean);
    const double stddev_double = static_cast<double>(std::sqrt(variance));
    if (!std::isfinite(mean_double) || !std::isfinite(stddev_double)) {
        return fail(error_out, "video sanity luma statistics are not finite");
    }
    samples->push_back({frame_index,
                        mean_double,
                        stddev_double,
                        minimum,
                        maximum,
                        static_cast<double>(black) / static_cast<double>(count),
                        static_cast<double>(white) / static_cast<double>(count),
                        static_cast<std::uint64_t>(width) * height});
    return true;
}

bool rational_seconds(const AVRational rational, double* seconds_out) noexcept
{
    if (seconds_out == nullptr || rational.num <= 0 || rational.den <= 0) {
        return false;
    }
    const double seconds = av_q2d(rational);
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return false;
    }
    *seconds_out = seconds;
    return true;
}

std::string canonical_rational(const AVRational rational)
{
    if (rational.num <= 0 || rational.den <= 0) {
        return {};
    }
    const std::int64_t numerator = rational.num;
    const std::int64_t denominator = rational.den;
    const std::int64_t divisor = std::gcd(numerator, denominator);
    if (divisor <= 0) {
        return {};
    }
    std::ostringstream result;
    result << numerator / divisor << '/' << denominator / divisor;
    return result.str();
}

bool validate_stream_cadence(const AVStream& stream,
                             const double expected_frame_rate,
                             AVRational* frame_rate_out,
                             double* time_base_seconds_out,
                             std::int64_t* ticks_per_frame_out,
                             std::string* error_out)
{
    if (frame_rate_out == nullptr || time_base_seconds_out == nullptr ||
        ticks_per_frame_out == nullptr) {
        return fail(error_out, "video sanity cadence output is null");
    }
    double time_base_seconds = 0.0;
    if (!rational_seconds(stream.time_base, &time_base_seconds)) {
        return fail(error_out, "video sanity stream has no valid time base");
    }
    const AVRational candidates[] = {stream.avg_frame_rate, stream.r_frame_rate};
    double selected_rate = 0.0;
    AVRational selected_rational{0, 1};
    bool have_rate = false;
    // Stream rate metadata is an exact rational in FFmpeg for the Orange
    // profile. Allow only a small conversion/rounding error; the larger
    // one-frame tolerance is reserved for aggregate duration validation.
    const double rate_tolerance =
        std::max(0.000001, expected_frame_rate * 0.0001);
    for (const AVRational candidate : candidates) {
        double rate = 0.0;
        if (!rational_seconds(candidate, &rate)) {
            continue;
        }
        if (!have_rate) {
            selected_rate = rate;
            selected_rational = candidate;
            have_rate = true;
        }
        if (std::fabs(rate - expected_frame_rate) > rate_tolerance ||
            std::fabs(rate - selected_rate) > rate_tolerance) {
            return fail(error_out,
                        "video sanity stream cadence does not match the contract");
        }
    }
    if (!have_rate) {
        return fail(error_out, "video sanity stream has no valid frame cadence");
    }
    const long double expected_period =
        1.0L / static_cast<long double>(expected_frame_rate);
    const long double ticks_per_frame =
        expected_period / static_cast<long double>(time_base_seconds);
    const long double integral_ticks = std::round(ticks_per_frame);
    if (!std::isfinite(ticks_per_frame) ||
        static_cast<long double>(time_base_seconds) > expected_period / 4.0L ||
        integral_ticks < 4.0L ||
        integral_ticks >
            static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        std::fabs(ticks_per_frame - integral_ticks) > 1e-9L) {
        return fail(error_out,
                    "video sanity time base cannot represent the contract cadence exactly");
    }
    if (canonical_rational(selected_rational).empty()) {
        return fail(error_out, "video sanity stream cadence is not representable");
    }
    *frame_rate_out = selected_rational;
    *time_base_seconds_out = time_base_seconds;
    *ticks_per_frame_out = static_cast<std::int64_t>(integral_ticks);
    return true;
}

bool duration_string(const AVFormatContext& format,
                     const AVStream& stream,
                     const double expected_frame_rate,
                     const std::uint64_t expected_frame_count,
                     const double time_base_seconds,
                     std::string* duration_out,
                     double* duration_seconds_out,
                     std::string* error_out)
{
    if (duration_out == nullptr || duration_seconds_out == nullptr) {
        return fail(error_out, "video sanity duration output is null");
    }
    double format_seconds = 0.0;
    double stream_seconds = 0.0;
    if (format.duration != AV_NOPTS_VALUE && format.duration > 0) {
        format_seconds = static_cast<double>(format.duration) / AV_TIME_BASE;
        if (!std::isfinite(format_seconds) || format_seconds <= 0.0) {
            return fail(error_out,
                        "video sanity container duration is not finite and positive");
        }
    } else {
        return fail(error_out,
                    "video sanity container duration is missing, zero, or negative");
    }
    if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0) {
        if (stream.time_base.num <= 0 || stream.time_base.den <= 0) {
            return fail(error_out, "video sanity stream duration has no valid time base");
        }
        stream_seconds = static_cast<double>(stream.duration) *
                         av_q2d(stream.time_base);
        if (!std::isfinite(stream_seconds) || stream_seconds <= 0.0) {
            return fail(error_out,
                        "video sanity stream duration is not finite and positive");
        }
    } else {
        return fail(error_out,
                    "video sanity stream duration is missing, zero, or negative");
    }
    double seconds = format_seconds > 0.0 ? format_seconds : stream_seconds;
    const double duration_tolerance =
        std::max(time_base_seconds, 1.0 / static_cast<double>(AV_TIME_BASE)) /
            2.0 +
        1e-9;
    if (format_seconds > 0.0 && stream_seconds > 0.0) {
        if (std::fabs(format_seconds - stream_seconds) > duration_tolerance) {
            return fail(error_out,
                        "video sanity container and stream durations disagree");
        }
    }
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return fail(error_out,
                    "video sanity media does not report a truthful positive duration");
    }
    const double expected_duration =
        static_cast<double>(expected_frame_count) / expected_frame_rate;
    if (!std::isfinite(expected_duration) ||
        std::fabs(seconds - expected_duration) > duration_tolerance) {
        return fail(error_out,
                    "video sanity duration does not match frame count and cadence");
    }
    std::ostringstream output;
    output.precision(17);
    output << seconds;
    std::string result = output.str();
    if (result.empty() || result.size() > 64U) {
        return fail(error_out,
                    "video sanity media duration could not be represented");
    }
    *duration_out = std::move(result);
    *duration_seconds_out = seconds;
    return true;
}

bool note_decoded_pts(const AVFrame& frame,
                      const std::int64_t expected_ticks_per_frame,
                      DecodedTimeline* timeline,
                      std::string* error_out)
{
    if (timeline == nullptr || expected_ticks_per_frame <= 0) {
        return fail(error_out, "video sanity decoded timeline state is invalid");
    }
    std::int64_t pts = frame.best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame.pts;
    }
    if (pts == AV_NOPTS_VALUE) {
        timeline->saw_missing_pts = true;
        if (timeline->saw_pts) {
            return fail(error_out,
                        "video sanity decoded timeline has missing timestamps");
        }
        return true;
    }
    if (timeline->saw_missing_pts) {
        return fail(error_out,
                    "video sanity decoded timeline has sparse timestamps");
    }
    if (!timeline->saw_pts) {
        timeline->first_pts = pts;
        timeline->previous_pts = pts;
        timeline->saw_pts = true;
        return true;
    }
    if (pts <= timeline->previous_pts) {
        return fail(error_out,
                    "video sanity decoded timestamps are not strictly monotonic");
    }
    const long double delta_ticks =
        static_cast<long double>(pts) -
        static_cast<long double>(timeline->previous_pts);
    if (!std::isfinite(delta_ticks) ||
        std::fabs(delta_ticks -
                  static_cast<long double>(expected_ticks_per_frame)) > 0.5L) {
        return fail(error_out,
                    "video sanity decoded timestamps are not dense at contract cadence");
    }
    timeline->previous_pts = pts;
    return true;
}

bool validate_decoded_timeline(const DecodedTimeline& timeline,
                               const std::uint64_t frame_count,
                               const std::int64_t expected_ticks_per_frame,
                               std::string* error_out)
{
    if (!timeline.saw_pts) {
        return fail(error_out,
                    "video sanity fixed MP4 profile lacks decoded timestamps");
    }
    if (timeline.saw_missing_pts || frame_count == 0 ||
        timeline.first_pts == AV_NOPTS_VALUE ||
        timeline.previous_pts == AV_NOPTS_VALUE) {
        return fail(error_out, "video sanity decoded timeline is incomplete");
    }
    const long double span_ticks =
        static_cast<long double>(timeline.previous_pts) -
        static_cast<long double>(timeline.first_pts);
    const long double expected_span_ticks =
        static_cast<long double>(frame_count - 1U) *
        static_cast<long double>(expected_ticks_per_frame);
    if (!std::isfinite(span_ticks) ||
        std::fabs(span_ticks - expected_span_ticks) > 0.5L) {
        return fail(error_out,
                    "video sanity decoded timeline span does not match cadence");
    }
    return true;
}

bool check_probe_result(const std::vector<SpatialRoiRecorderVideoSanitySample>& samples,
                        const std::uint64_t expected_pixels,
                        const Deadline deadline,
                        std::string* error_out)
{
    if (samples.empty()) {
        return fail(error_out, "video sanity did not produce deterministic samples");
    }
    double maximum_black = 0.0;
    double maximum_stddev = 0.0;
    for (const auto& sample : samples) {
        if (expired(deadline)) {
            return fail(error_out,
                        "video sanity deadline expired while checking luma statistics");
        }
        if (!std::isfinite(sample.mean) || !std::isfinite(sample.stddev) ||
            !std::isfinite(sample.black_fraction_lt8) ||
            !std::isfinite(sample.white_fraction_gt247) || sample.mean < 0.0 ||
            sample.mean > 255.0 || sample.stddev < 0.0 || sample.min > sample.max ||
            sample.max > 255 || sample.black_fraction_lt8 < 0.0 ||
            sample.black_fraction_lt8 > 1.0 || sample.white_fraction_gt247 < 0.0 ||
            sample.white_fraction_gt247 > 1.0 ||
            sample.decoded_bytes != expected_pixels) {
            return fail(error_out, "video sanity produced invalid luma statistics");
        }
        maximum_black = std::max(maximum_black, sample.black_fraction_lt8);
        maximum_stddev = std::max(maximum_stddev, sample.stddev);
    }
    if (maximum_black >= 0.98 || maximum_stddev < 5.0) {
        return fail(error_out, "video sanity luma statistics fail fixed thresholds");
    }
    return true;
}

bool run_probe(const SpatialRoiRecorderVideoSanityRequest& request,
               ProbeData* data_out,
               std::string* error_out)
{
    if (data_out == nullptr) {
        return fail(error_out, "video sanity probe data output is null");
    }
    if (!request.artifact_root || !request.artifact_root->valid()) {
        return fail(error_out, "video sanity requires a valid artifact root");
    }
    if (!valid_path(request.video_relative_path, error_out) ||
        !request.artifact_root->IsAllowed(request.video_relative_path)) {
        return fail(error_out, "video sanity video path is not contract-authorized");
    }
    std::uint64_t expected_pixels = 0;
    std::uint64_t decoder_pixel_bound = 0;
    if (!valid_dimensions(request.encoded_width,
                          request.encoded_height,
                          &expected_pixels,
                          error_out) ||
        !hevc_decoder_pixel_bound(request.encoded_width,
                                  request.encoded_height,
                                  &decoder_pixel_bound,
                                  error_out) ||
        request.expected_frame_count == 0 ||
        request.expected_frame_count > kMaxDecodedFrames ||
        !std::isfinite(request.expected_frame_rate) ||
        request.expected_frame_rate < kMinFrameRate ||
        request.expected_frame_rate > kMaxFrameRate ||
        request.timeout.count() <= 0 || request.timeout > kMaxTimeout) {
        return fail(error_out, "video sanity request is outside its authenticated bounds");
    }
    const auto deadline = Clock::now() + request.timeout;
    const SpatialRoiRecorderArtifactIdentity root_identity =
        request.artifact_root->artifact_root_identity();
    const std::uint64_t authenticated_media_bytes = request.max_media_bytes;
    if (authenticated_media_bytes == 0 ||
        authenticated_media_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return fail(error_out, "video sanity request has no single authenticated media bound");
    }

    std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
    if (!request.artifact_root->OpenExistingFile(
            request.video_relative_path,
            SpatialRoiRecorderArtifactFileAccess::kReadOnly,
            &file,
            error_out) ||
        !file || file->relative_path() != request.video_relative_path) {
        return fail(error_out, "video sanity could not open the exact contract video");
    }
    struct stat initial_stat {};
    if (::fstat(file->borrowed_fd(), &initial_stat) != 0 ||
        !S_ISREG(initial_stat.st_mode) || initial_stat.st_size <= 0) {
        return fail(error_out, "video sanity video is not a nonempty regular file");
    }
    const std::uint64_t media_size = static_cast<std::uint64_t>(initial_stat.st_size);
    if (media_size > authenticated_media_bytes) {
        return fail(error_out, "video sanity media exceeds the authenticated byte bound");
    }
    SpatialRoiRecorderArtifactIdentity video_identity;
    if (!check_retained_file(*file,
                             root_identity,
                             media_size,
                             &video_identity,
                             error_out)) {
        return false;
    }

    std::string initial_digest;
    if (!hash_retained_file(*file, media_size, deadline, &initial_digest, error_out)) {
        return false;
    }
    // Hashing uses the retained descriptor, but a writer can still change the
    // regular file while those reads are in flight. Recheck the descriptor's
    // identity, size, and descriptor-relative directory binding after the
    // digest and before handing the same descriptor to FFmpeg.
    if (!check_retained_file(*file,
                             root_identity,
                             media_size,
                             &video_identity,
                             error_out)) {
        return false;
    }
    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired before demux setup");
    }

    IoState io_state{file->borrowed_fd(), media_size, 0, deadline, false, false};
    auto* io_buffer = static_cast<unsigned char*>(av_malloc(kIoBufferBytes));
    if (io_buffer == nullptr) {
        return fail(error_out, "video sanity could not allocate bounded AVIO buffer");
    }
    FormatResources format_resources;
    format_resources.io = avio_alloc_context(io_buffer,
                                             static_cast<int>(kIoBufferBytes),
                                             0,
                                             &io_state,
                                             read_packet,
                                             nullptr,
                                             seek_packet);
    if (format_resources.io == nullptr) {
        av_free(io_buffer);
        return fail(error_out, "video sanity could not allocate custom AVIO");
    }
    format_resources.format = avformat_alloc_context();
    if (format_resources.format == nullptr) {
        return fail(error_out, "video sanity could not allocate demux context");
    }
    format_resources.format->pb = format_resources.io;
    format_resources.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format_resources.format->interrupt_callback.callback = interrupt_callback;
    format_resources.format->interrupt_callback.opaque = &io_state;
    // Bound the two format-level accumulation pools using authenticated
    // dimensions/cardinality.  These fields are the public libavformat
    // controls available before demuxing; packet payload allocation remains
    // demuxer-specific and is checked immediately after each read.
    format_resources.format->max_streams = 1;
    format_resources.format->max_probe_packets = static_cast<int>(std::min<
        std::uint64_t>(request.expected_frame_count + 1U,
                       static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    const std::uint64_t index_budget =
        static_cast<std::uint64_t>(request.expected_frame_count) *
        sizeof(AVIndexEntry);
    format_resources.format->max_index_size = static_cast<unsigned int>(
        std::min<std::uint64_t>(index_budget,
                                std::numeric_limits<unsigned int>::max()));
    const std::uint64_t picture_budget = expected_pixels * 4ULL;
    format_resources.format->max_picture_buffer = static_cast<unsigned int>(
        std::min<std::uint64_t>(picture_budget,
                                std::numeric_limits<unsigned int>::max()));
    format_resources.format->probesize = static_cast<std::int64_t>(media_size);
    const long double analyze_seconds =
        (static_cast<long double>(request.expected_frame_count) + 1.0L) /
        static_cast<long double>(request.expected_frame_rate);
    const long double analyze_microseconds =
        analyze_seconds * static_cast<long double>(AV_TIME_BASE);
    format_resources.format->max_analyze_duration =
        static_cast<std::int64_t>(std::min<long double>(
            analyze_microseconds,
            static_cast<long double>(std::numeric_limits<std::int64_t>::max())));
    // AVIO exposes only an int-sized packet hint.  Keep it tied to the
    // authenticated media bound and retain the stronger post-read check
    // below; demuxers are otherwise free to choose their packet allocations.
    format_resources.io->max_packet_size = static_cast<int>(std::min<
        std::uint64_t>(media_size, kMaxPacketBytesForAvio));
    int ffmpeg_result = avformat_open_input(
        &format_resources.format, nullptr, nullptr, nullptr);
    if (ffmpeg_result < 0) {
        if (io_state.timed_out) {
            return fail(error_out, "video sanity deadline expired opening media");
        }
        return fail(error_out, av_error("video sanity could not demux media", ffmpeg_result));
    }
    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired opening media");
    }
    // Stream metadata such as duration and codec pixel format is not
    // authoritative until stream-info probing has completed. The interrupt
    // callback above covers demux I/O, but timeout enforcement remains
    // cooperative because an individual FFmpeg codec call may be
    // non-interruptible. An outer supervised process deadline is mandatory
    // for untrusted or adversarial media.
    ffmpeg_result = avformat_find_stream_info(format_resources.format, nullptr);
    if (ffmpeg_result < 0) {
        if (io_state.timed_out) {
            return fail(error_out,
                        "video sanity deadline expired finding stream information");
        }
        return fail(error_out,
                    av_error("video sanity could not find stream information",
                             ffmpeg_result));
    }
    if (expired(deadline)) {
        return fail(error_out,
                    "video sanity deadline expired finding stream information");
    }
    if (format_resources.format == nullptr ||
        !is_mov_family(format_resources.format->iformat)) {
        return fail(error_out, "video sanity requires an MP4/MOV-family container");
    }
    if (format_resources.format->nb_streams != 1U ||
        format_resources.format->streams == nullptr ||
        format_resources.format->streams[0] == nullptr ||
        format_resources.format->streams[0]->codecpar == nullptr ||
        format_resources.format->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        return fail(error_out, "video sanity requires exactly one video stream");
    }
    AVStream* stream = format_resources.format->streams[0];
    if (stream->codecpar->codec_id != AV_CODEC_ID_HEVC) {
        return fail(error_out, "video sanity requires HEVC video");
    }
    if (stream->codecpar->width != static_cast<int>(request.encoded_width) ||
        stream->codecpar->height != static_cast<int>(request.encoded_height)) {
        return fail(error_out, "video sanity container dimensions do not match the contract");
    }
    PixelSemantics container_pixel;
    if (!validate_profile_pixel_format(stream->codecpar->format,
                                       &container_pixel,
                                       error_out)) {
        return false;
    }
    if (stream->codecpar->color_range != AVCOL_RANGE_JPEG) {
        return fail(error_out,
                    "video sanity container pixel range is not full range");
    }
    AVRational stream_frame_rate{0, 1};
    double time_base_seconds = 0.0;
    std::int64_t expected_ticks_per_frame = 0;
    if (!validate_stream_cadence(*stream,
                                 request.expected_frame_rate,
                                 &stream_frame_rate,
                                 &time_base_seconds,
                                 &expected_ticks_per_frame,
                                 error_out)) {
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr || decoder->name == nullptr) {
        return fail(error_out, "video sanity HEVC decoder is unavailable");
    }
    CodecResources codec_resources;
    codec_resources.codec = avcodec_alloc_context3(decoder);
    codec_resources.packet = av_packet_alloc();
    codec_resources.frame = av_frame_alloc();
    if (codec_resources.codec == nullptr || codec_resources.packet == nullptr ||
        codec_resources.frame == nullptr) {
        return fail(error_out, "video sanity could not allocate decoder state");
    }
    if (avcodec_parameters_to_context(codec_resources.codec, stream->codecpar) < 0) {
        return fail(error_out, "video sanity could not copy HEVC decoder parameters");
    }
    if (codec_resources.codec->color_range != AVCOL_RANGE_JPEG) {
        return fail(error_out,
                    "video sanity decoder pixel range is not full range");
    }
    // These are decoder-side allocation guards and must be set before
    // avcodec_open2.  max_samples is mostly relevant to audio codecs, but
    // setting it here keeps a future multi-media demux path bounded too.
    // max_pixels applies to decoder allocation before conformance-window
    // cropping and includes FFmpeg's internal stride alignment. Bound it to a
    // conservative 64x64 envelope around the authenticated visible raster.
    // The container and every decoded AVFrame are still required below to
    // equal the exact contract dimensions, so this does not permit a
    // different visible raster.
    codec_resources.codec->max_pixels =
        static_cast<std::int64_t>(decoder_pixel_bound);
    codec_resources.codec->max_samples = static_cast<std::int64_t>(expected_pixels);
    codec_resources.codec->thread_count = 1;
    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired before opening decoder");
    }
    ffmpeg_result = avcodec_open2(codec_resources.codec, decoder, nullptr);
    if (ffmpeg_result < 0) {
        return fail(error_out, av_error("video sanity could not open HEVC decoder", ffmpeg_result));
    }
    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired opening decoder");
    }

    std::vector<std::uint64_t> sample_indices;
    sample_indices.reserve(3U);
    sample_indices.push_back(0);
    if (request.expected_frame_count > 2U) {
        sample_indices.push_back(request.expected_frame_count / 2U);
    }
    if (request.expected_frame_count > 1U) {
        sample_indices.push_back(request.expected_frame_count - 1U);
    }
    std::vector<SpatialRoiRecorderVideoSanitySample> samples;
    samples.reserve(sample_indices.size());
    std::uint64_t decoded_frames = 0;
    DecodedTimeline decoded_timeline;
    PixelSemantics decoded_pixel_semantics;
    bool have_decoded_pixel_semantics = false;
    std::string decode_error;
    const auto receive_available = [&]() -> bool {
        while (true) {
            if (expired(deadline)) {
                decode_error = "video sanity deadline expired while decoding";
                return false;
            }
            const int receive_result =
                avcodec_receive_frame(codec_resources.codec, codec_resources.frame);
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return true;
            }
            if (receive_result < 0) {
                decode_error = av_error("video sanity decoder failed", receive_result);
                return false;
            }
            if (decoded_frames >= request.expected_frame_count) {
                decode_error = "video sanity decoded more frames than expected";
                return false;
            }
            if (codec_resources.frame->width != static_cast<int>(request.encoded_width) ||
                codec_resources.frame->height != static_cast<int>(request.encoded_height)) {
                decode_error = "video sanity decoded frame dimensions do not match";
                return false;
            }
            PixelSemantics decoded_pixel;
            if (!validate_profile_pixel_format(codec_resources.frame->format,
                                               &decoded_pixel,
                                               &decode_error) ||
                !compatible_profile_pixel_formats(container_pixel,
                                                  decoded_pixel)) {
                if (decode_error.empty()) {
                    decode_error =
                        "video sanity decoded pixel format lacks container provenance";
                }
                return false;
            }
            if (!have_decoded_pixel_semantics) {
                decoded_pixel_semantics = decoded_pixel;
                have_decoded_pixel_semantics = true;
            } else if (std::strcmp(decoded_pixel_semantics.canonical_name,
                                   decoded_pixel.canonical_name) != 0) {
                decode_error =
                    "video sanity decoded pixel format changed within stream";
                return false;
            }
            if (codec_resources.frame->color_range != AVCOL_RANGE_JPEG) {
                decode_error = "video sanity decoded pixel range is not full range";
                return false;
            }
            if (!validate_luma_plane(*codec_resources.frame,
                                     *decoded_pixel.descriptor,
                                     request.encoded_width,
                                     request.encoded_height,
                                     &decode_error)) {
                return false;
            }
            if (!note_decoded_pts(*codec_resources.frame,
                                  expected_ticks_per_frame,
                                  &decoded_timeline,
                                  &decode_error)) {
                return false;
            }
            if (std::find(sample_indices.begin(),
                          sample_indices.end(),
                          decoded_frames) != sample_indices.end() &&
                !read_luma_sample(*codec_resources.frame,
                                  *decoded_pixel.descriptor,
                                  request.encoded_width,
                                  request.encoded_height,
                                  deadline,
                                  &samples,
                                  decoded_frames,
                                  &decode_error)) {
                return false;
            }
            ++decoded_frames;
            av_frame_unref(codec_resources.frame);
        }
    };

    bool input_eof = false;
    while (!input_eof) {
        if (expired(deadline)) {
            return fail(error_out, "video sanity deadline expired while reading media");
        }
        ffmpeg_result = av_read_frame(format_resources.format, codec_resources.packet);
        if (ffmpeg_result == AVERROR_EOF) {
            input_eof = true;
            break;
        }
        if (ffmpeg_result < 0) {
            if (io_state.timed_out) {
                return fail(error_out, "video sanity deadline expired while reading media");
            }
            return fail(error_out, av_error("video sanity could not read media", ffmpeg_result));
        }
        if (codec_resources.packet->stream_index != 0) {
            return fail(error_out, "video sanity encountered an unexpected stream");
        }
        if (codec_resources.packet->data == nullptr ||
            codec_resources.packet->size <= 0 ||
            static_cast<std::uint64_t>(codec_resources.packet->size) > media_size) {
            return fail(error_out,
                        "video sanity packet exceeds the authenticated media bound");
        }
        while (true) {
            if (expired(deadline)) {
                return fail(error_out,
                            "video sanity deadline expired while submitting packet");
            }
            ffmpeg_result = avcodec_send_packet(codec_resources.codec,
                                                codec_resources.packet);
            if (ffmpeg_result == AVERROR(EAGAIN)) {
                if (!receive_available()) {
                    return fail(error_out, decode_error);
                }
                continue;
            }
            if (ffmpeg_result < 0) {
                return fail(error_out, av_error("video sanity decoder rejected packet",
                                                ffmpeg_result));
            }
            if (!receive_available()) {
                return fail(error_out, decode_error);
            }
            break;
        }
        av_packet_unref(codec_resources.packet);
    }

    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired before decoder flush");
    }
    ffmpeg_result = avcodec_send_packet(codec_resources.codec, nullptr);
    if (ffmpeg_result < 0 && ffmpeg_result != AVERROR_EOF) {
        return fail(error_out, av_error("video sanity decoder flush failed", ffmpeg_result));
    }
    if (!receive_available()) {
        return fail(error_out, decode_error);
    }
    if (decoded_frames != request.expected_frame_count ||
        samples.size() != sample_indices.size()) {
        return fail(error_out, "video sanity decoded frame count does not match exactly");
    }
    if (!validate_decoded_timeline(decoded_timeline,
                                   decoded_frames,
                                   expected_ticks_per_frame,
                                   error_out) ||
        !check_probe_result(samples, expected_pixels, deadline, error_out)) {
        return false;
    }

    std::string final_digest;
    if (!check_retained_file(*file,
                             root_identity,
                             media_size,
                             &video_identity,
                             error_out) ||
        !hash_retained_file(*file, media_size, deadline, &final_digest, error_out) ||
        final_digest != initial_digest ||
        !check_retained_file(*file,
                             root_identity,
                             media_size,
                             &video_identity,
                             error_out)) {
        if (final_digest != initial_digest && !final_digest.empty()) {
            return fail(error_out, "video sanity media changed while it was decoded");
        }
        return false;
    }
    const std::string container = format_resources.format->iformat->name != nullptr
                                      ? format_resources.format->iformat->name
                                      : "mov";
    const std::string codec = avcodec_get_name(stream->codecpar->codec_id);
    const std::string decoder_provenance =
        std::string(decoder->name) + "@" + av_version_info();
    std::string duration;
    double duration_seconds = 0.0;
    if (!duration_string(*format_resources.format,
                         *stream,
                         request.expected_frame_rate,
                         request.expected_frame_count,
                         time_base_seconds,
                         &duration,
                         &duration_seconds,
                         error_out)) {
        return false;
    }
    if (expired(deadline)) {
        return fail(error_out, "video sanity deadline expired before publication");
    }
    // Convert the retained unique owner only after every check has passed.
    // The resulting object keeps both descriptor-relative authorities alive
    // for consumers of the non-forgeable result.
    data_out->artifact_root = request.artifact_root;
    data_out->video_file = std::shared_ptr<SpatialRoiRecorderArtifactFile>(
        std::move(file));
    data_out->artifact_root_identity = root_identity;
    data_out->video_identity = video_identity;
    data_out->video_relative_path = request.video_relative_path;
    data_out->size_bytes = media_size;
    data_out->sha256 = std::move(final_digest);
    data_out->duration_seconds = std::move(duration);
    data_out->frame_rate = canonical_rational(stream_frame_rate);
    data_out->time_base = canonical_rational(stream->time_base);
    data_out->has_decoded_pts = decoded_timeline.saw_pts;
    data_out->first_decoded_pts = decoded_timeline.saw_pts
                                      ? decoded_timeline.first_pts
                                      : AV_NOPTS_VALUE;
    data_out->last_decoded_pts = decoded_timeline.saw_pts
                                     ? decoded_timeline.previous_pts
                                     : AV_NOPTS_VALUE;
    data_out->container = container;
    data_out->codec = codec;
    data_out->decoder = decoder_provenance;
    data_out->pixel_format = decoded_pixel_semantics.canonical_name;
    // FFmpeg's canonical name for AVCOL_RANGE_JPEG is "pc"; retain that
    // spelling in the non-forgeable result and metadata contract.
    data_out->color_range = "pc";
    data_out->bit_depth = decoded_pixel_semantics.bit_depth;
    data_out->chroma_subsampling =
        decoded_pixel_semantics.chroma_subsampling;
    data_out->width = request.encoded_width;
    data_out->height = request.encoded_height;
    data_out->frame_count = decoded_frames;
    data_out->samples = std::move(samples);
    return true;
}

}  // namespace

bool SpatialRoiRecorderVideoSanityProbe::Run(
    const SpatialRoiRecorderVideoSanityRequest& request,
    std::unique_ptr<SpatialRoiRecorderVideoSanityResult>* result_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (result_out == nullptr) {
        return fail(error_out, "video sanity result output is null");
    }
    result_out->reset();
    try {
        ProbeData data;
        if (!run_probe(request, &data, error_out)) {
            return false;
        }
        *result_out = std::unique_ptr<SpatialRoiRecorderVideoSanityResult>(
            new SpatialRoiRecorderVideoSanityResult(
                std::move(data.artifact_root),
                std::move(data.video_file),
                data.artifact_root_identity,
                data.video_identity,
                std::move(data.video_relative_path),
                data.size_bytes,
                std::move(data.sha256),
                std::move(data.duration_seconds),
                std::move(data.frame_rate),
                std::move(data.time_base),
                data.has_decoded_pts,
                data.first_decoded_pts,
                data.last_decoded_pts,
                std::move(data.container),
                std::move(data.codec),
                std::move(data.decoder),
                std::move(data.pixel_format),
                std::move(data.color_range),
                data.bit_depth,
                std::move(data.chroma_subsampling),
                data.width,
                data.height,
                data.frame_count,
                std::move(data.samples)));
        return true;
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("video sanity probe failed: ") + exception.what());
    } catch (...) {
        return fail(error_out, "video sanity probe failed unexpectedly");
    }
}

bool probe_spatial_roi_recorder_video_sanity(
    const SpatialRoiRecorderVideoSanityRequest& request,
    std::unique_ptr<SpatialRoiRecorderVideoSanityResult>* result_out,
    std::string* error_out)
{
    return SpatialRoiRecorderVideoSanityProbe::Run(request, result_out, error_out);
}

}  // namespace orange::spatial_roi::recording
