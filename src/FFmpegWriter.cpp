// src/FFmpegWriter.cpp

#include "FFmpegWriter.h"
#include "fsuid_guard.h"
#include "nvtx_profiling.h"
#include "video_container_finalization.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <pthread.h>
#endif

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {
bool is_start_code(const uint8_t* data, size_t size, size_t* start_code_len) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        *start_code_len = 4;
        return true;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        *start_code_len = 3;
        return true;
    }
    return false;
}

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8)  |
           static_cast<uint32_t>(data[3]);
}

void update_peak(std::atomic<size_t>& peak, size_t value) {
    size_t observed = peak.load(std::memory_order_relaxed);
    while (value > observed &&
           !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

std::string av_error_string(int error_code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(error_code, buffer.data(), buffer.size()) < 0) {
        return "ffmpeg_error_" + std::to_string(error_code);
    }
    return buffer.data();
}

int validated_frame_rate(int fps)
{
    if (fps <= 0) {
        throw std::invalid_argument(
            "FFmpegWriter frame rate must be a positive integer");
    }
    return fps;
}

bool is_fixed_gop_policy(const FFmpegWriterKeyframePolicy& policy)
{
    return policy.name == kFFmpegWriterFixedGopIdrPolicyName ||
        (policy.gop_length >= 2 &&
         policy.name == "fixed_gop_" + std::to_string(policy.gop_length) +
                            "_idr");
}

void validate_keyframe_policy(const FFmpegWriterKeyframePolicy& policy)
{
    if (policy.name == kFFmpegWriterAllFramesIdrPolicyName) {
        if (policy.gop_length != 1) {
            throw std::invalid_argument(
                "FFmpegWriter all_frames_idr policy requires gop_length=1");
        }
        return;
    }
    if (policy.name == kFFmpegWriterFixedGopIdrPolicyName) {
        if (policy.gop_length < 2) {
            throw std::invalid_argument(
                "FFmpegWriter fixed_gop_idr policy requires gop_length>=2");
        }
        return;
    }
    if (is_fixed_gop_policy(policy)) {
        return;
    }
    throw std::invalid_argument(
        "FFmpegWriter keyframe policy name is not supported");
}

std::string keyframe_policy_summary_name(
    const FFmpegWriterKeyframePolicy& policy)
{
    if (is_fixed_gop_policy(policy)) {
        return "fixed_gop_" + std::to_string(policy.gop_length) + "_idr";
    }
    return policy.name;
}

FFmpegWriterKeyframePolicy validated_keyframe_policy(
    FFmpegWriterKeyframePolicy policy)
{
    validate_keyframe_policy(policy);
    return policy;
}

bool frame_index_has_representable_pts(int64_t frame_index, int fps)
{
    if (frame_index < 0 || frame_index == std::numeric_limits<int64_t>::max() ||
        fps <= 0) {
        return false;
    }
    // FFmpegWriter's stream clock is fixed at 90 kHz. Check both this frame
    // and the following frame before forming frame_index + 1 or asking FFmpeg
    // to rescale it. __int128 keeps this proof independent of multiplication
    // overflow in int64_t.
    const __int128 next_scaled =
        static_cast<__int128>(frame_index + 1) * 90000;
    // av_rescale_q uses AV_ROUND_NEAR_INF for finite values. Reproduce its
    // positive-input rounding here rather than accepting a value whose floor
    // fits but whose rounded result overflows to AV_NOPTS_VALUE/INT64_MIN.
    const __int128 rounded_next_scaled = next_scaled + fps / 2;
    return rounded_next_scaled / fps <= std::numeric_limits<int64_t>::max();
}

bool is_contract_relative_label(const std::string& label)
{
    if (label.empty()) {
        return false;
    }
    const std::filesystem::path path(label);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

int duplicate_fd_cloexec(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
#ifdef F_DUPFD_CLOEXEC
    return ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    const int duplicate = ::dup(fd);
    if (duplicate < 0) {
        return -1;
    }
    if (::fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        ::close(duplicate);
        errno = saved_errno;
        return -1;
    }
    return duplicate;
#endif
}

bool descriptor_is_writable_regular_file(int fd, bool require_readable)
{
    const int flags = ::fcntl(fd, F_GETFL);
    const int access_mode = flags < 0 ? O_RDONLY : (flags & O_ACCMODE);
    if (flags < 0 || access_mode == O_RDONLY ||
        (require_readable &&
         (access_mode != O_RDWR || (flags & O_APPEND) != 0))) {
        return false;
    }
    struct stat descriptor_stat {};
    return ::fstat(fd, &descriptor_stat) == 0 &&
           S_ISREG(descriptor_stat.st_mode);
}

bool descriptors_name_distinct_inodes(int first_fd, int second_fd)
{
    struct stat first {};
    struct stat second {};
    return ::fstat(first_fd, &first) == 0 &&
           ::fstat(second_fd, &second) == 0 &&
           !(first.st_dev == second.st_dev && first.st_ino == second.st_ino);
}

bool write_all_fd(int fd, const std::string& bytes)
{
    size_t completed = 0;
    while (completed < bytes.size()) {
        const ssize_t count = ::write(
            fd, bytes.data() + completed, bytes.size() - completed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        completed += static_cast<size_t>(count);
    }
    return true;
}

bool persist_finalization_status(
    bool descriptor_output,
    int video_fd,
    int finalization_fd,
    const std::string& output_path,
    const std::string& finalization_path,
    int fps,
    OrangeVideoContainerFinalization::Status status,
    const OrangeVideoContainerFinalization::Outcome& outcome,
    bool log_success)
{
    std::string error;
    std::filesystem::path sidecar_path;
    const bool persisted = descriptor_output
        ? OrangeVideoContainerFinalization::Persist(
              video_fd, output_path, finalization_fd, finalization_path,
              fps, status, outcome, &error)
        : OrangeVideoContainerFinalization::Persist(
              output_path, fps, status, outcome, &sidecar_path, &error);
    if (!persisted) {
        std::cerr << "FFMPEG: could not persist container finalization status for "
                  << output_path << ": " << error << std::endl;
        return false;
    }
    if (log_success) {
        // stdout is a machine-readable JSONL lifecycle channel for the
        // camera-level spatial ROI recorder child. Keep diagnostics on
        // stderr so successful writer teardown cannot corrupt that protocol.
        std::cerr << "FFMPEG: container finalization status="
                  << OrangeVideoContainerFinalization::StatusName(status)
                  << " sidecar="
                  << (descriptor_output ? finalization_path
                                        : sidecar_path.string())
                  << std::endl;
    }
    return true;
}
} // namespace

FFmpegWriterDescriptorOutputConfig::FFmpegWriterDescriptorOutputConfig(
    int video_fd,
    int keyframe_sidecar_fd,
    int finalization_sidecar_fd,
    uint64_t max_video_bytes,
    std::string video_display_label,
    std::string keyframe_sidecar_display_label,
    std::string finalization_sidecar_display_label)
    : max_video_bytes_(max_video_bytes),
      video_display_label_(std::move(video_display_label)),
      keyframe_sidecar_display_label_(
          std::move(keyframe_sidecar_display_label)),
      finalization_sidecar_display_label_(
          std::move(finalization_sidecar_display_label))
{
    if (max_video_bytes_ == 0 ||
        max_video_bytes_ > static_cast<uint64_t>(
                               std::numeric_limits<off_t>::max())) {
        throw std::invalid_argument(
            "FFmpegWriter descriptor max_video_bytes must be nonzero and representable by off_t");
    }
    if (!is_contract_relative_label(video_display_label_) ||
        !is_contract_relative_label(keyframe_sidecar_display_label_) ||
        !is_contract_relative_label(finalization_sidecar_display_label_)) {
        throw std::invalid_argument(
            "FFmpegWriter descriptor labels must be non-empty contract-relative paths");
    }
    if (!descriptor_is_writable_regular_file(video_fd, true) ||
        !descriptor_is_writable_regular_file(keyframe_sidecar_fd, false) ||
        !descriptor_is_writable_regular_file(finalization_sidecar_fd, false)) {
        throw std::invalid_argument(
            "FFmpegWriter video descriptor must be read-write and all descriptor outputs must be writable regular files");
    }
    if (!descriptors_name_distinct_inodes(video_fd, keyframe_sidecar_fd) ||
        !descriptors_name_distinct_inodes(video_fd, finalization_sidecar_fd) ||
        !descriptors_name_distinct_inodes(keyframe_sidecar_fd,
                                         finalization_sidecar_fd)) {
        throw std::invalid_argument(
            "FFmpegWriter descriptor outputs must name three distinct inodes");
    }

    video_fd_ = duplicate_fd_cloexec(video_fd);
    if (video_fd_ < 0) {
        throw std::runtime_error(
            "FFmpegWriter could not duplicate video descriptor: " +
            std::to_string(errno));
    }
    keyframe_sidecar_fd_ = duplicate_fd_cloexec(keyframe_sidecar_fd);
    if (keyframe_sidecar_fd_ < 0) {
        const int saved_errno = errno;
        close_owned_fds();
        throw std::runtime_error(
            "FFmpegWriter could not duplicate keyframe descriptor: " +
            std::to_string(saved_errno));
    }
    finalization_sidecar_fd_ = duplicate_fd_cloexec(finalization_sidecar_fd);
    if (finalization_sidecar_fd_ < 0) {
        const int saved_errno = errno;
        close_owned_fds();
        throw std::runtime_error(
            "FFmpegWriter could not duplicate finalization descriptor: " +
            std::to_string(saved_errno));
    }
}

FFmpegWriterDescriptorOutputConfig::~FFmpegWriterDescriptorOutputConfig()
{
    close_owned_fds();
}

FFmpegWriterDescriptorOutputConfig::FFmpegWriterDescriptorOutputConfig(
    FFmpegWriterDescriptorOutputConfig&& other) noexcept
    : video_fd_(other.release_video_fd()),
      keyframe_sidecar_fd_(other.release_keyframe_sidecar_fd()),
      finalization_sidecar_fd_(other.release_finalization_sidecar_fd()),
      max_video_bytes_(other.max_video_bytes_),
      video_display_label_(std::move(other.video_display_label_)),
      keyframe_sidecar_display_label_(
          std::move(other.keyframe_sidecar_display_label_)),
      finalization_sidecar_display_label_(
          std::move(other.finalization_sidecar_display_label_))
{
}

FFmpegWriterDescriptorOutputConfig&
FFmpegWriterDescriptorOutputConfig::operator=(
    FFmpegWriterDescriptorOutputConfig&& other) noexcept
{
    if (this != &other) {
        close_owned_fds();
        video_fd_ = other.release_video_fd();
        keyframe_sidecar_fd_ = other.release_keyframe_sidecar_fd();
        finalization_sidecar_fd_ = other.release_finalization_sidecar_fd();
        max_video_bytes_ = other.max_video_bytes_;
        video_display_label_ = std::move(other.video_display_label_);
        keyframe_sidecar_display_label_ =
            std::move(other.keyframe_sidecar_display_label_);
        finalization_sidecar_display_label_ =
            std::move(other.finalization_sidecar_display_label_);
    }
    return *this;
}

int FFmpegWriterDescriptorOutputConfig::release_video_fd() noexcept
{
    const int fd = video_fd_;
    video_fd_ = -1;
    return fd;
}

int FFmpegWriterDescriptorOutputConfig::release_keyframe_sidecar_fd() noexcept
{
    const int fd = keyframe_sidecar_fd_;
    keyframe_sidecar_fd_ = -1;
    return fd;
}

int FFmpegWriterDescriptorOutputConfig::release_finalization_sidecar_fd() noexcept
{
    const int fd = finalization_sidecar_fd_;
    finalization_sidecar_fd_ = -1;
    return fd;
}

void FFmpegWriterDescriptorOutputConfig::close_owned_fds() noexcept
{
    if (video_fd_ >= 0) {
        ::close(video_fd_);
        video_fd_ = -1;
    }
    if (keyframe_sidecar_fd_ >= 0) {
        ::close(keyframe_sidecar_fd_);
        keyframe_sidecar_fd_ = -1;
    }
    if (finalization_sidecar_fd_ >= 0) {
        ::close(finalization_sidecar_fd_);
        finalization_sidecar_fd_ = -1;
    }
}

FFmpegWriterLatencyStats FFmpegWriter::latency_stats() const
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    return latency_stats_;
}

FFmpegWriterFailureStats FFmpegWriter::failure_stats() const
{
    FFmpegWriterFailureStats result;
    result.packet_allocation_failures =
        packet_allocation_failures_.load(std::memory_order_relaxed);
    result.packet_enqueue_failures =
        packet_enqueue_failures_.load(std::memory_order_relaxed);
    result.packet_write_failures =
        packet_write_failures_.load(std::memory_order_relaxed);
    result.muxer_flush_failures =
        muxer_flush_failures_.load(std::memory_order_relaxed);
    result.sidecar_write_failures =
        sidecar_write_failures_.load(std::memory_order_relaxed);
    result.video_size_limit_failures =
        video_size_limit_failures_.load(std::memory_order_relaxed);
    result.thread_failures = thread_failures_.load(std::memory_order_relaxed);
    result.total_failures = total_failures_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        result.failed = writer_error_.load(std::memory_order_acquire);
        result.last_error_code = last_error_code_;
        result.last_error = last_error_;
    }
    return result;
}

void FFmpegWriter::latch_failure(
    FailureKind kind,
    int error_code,
    const char* operation) noexcept
{
    switch (kind) {
        case FailureKind::PacketAllocation:
            packet_allocation_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::PacketEnqueue:
            packet_enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::PacketWrite:
            packet_write_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::MuxerFlush:
            muxer_flush_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::SidecarWrite:
            sidecar_write_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::VideoSizeLimit:
            video_size_limit_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FailureKind::Thread:
            thread_failures_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    total_failures_.fetch_add(1, std::memory_order_relaxed);

    // Record the first failure as the stable reason. The counters continue to
    // count subsequent failures, but callers should not see the root cause
    // replaced by a later cascading FFmpeg error.
    try {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        if (!writer_error_.load(std::memory_order_relaxed)) {
            writer_error_.store(true, std::memory_order_release);
            last_error_code_ = error_code;
            try {
                last_error_ = operation ? operation : "FFmpegWriter failure";
            } catch (...) {
                last_error_.clear();
            }
        } else {
            writer_error_.store(true, std::memory_order_release);
        }
    } catch (...) {
        // The failure latch must survive even if diagnostics cannot allocate.
        writer_error_.store(true, std::memory_order_release);
    }
}

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    const char *szOutFilePath,
    const char *metadata_file,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags,
    FFmpegWriterQueueConfig queue_config)
    : nFps(validated_frame_rate(nFps)),
      output_path_(szOutFilePath ? szOutFilePath : ""),
      queue_config_(queue_config)
{
    if (szOutFilePath) {
        output_label_ = std::filesystem::path(szOutFilePath).stem().string();
    }
    codec_id_ = eCodecId;
    if (metadata_file) {
        keyframe_file_ = metadata_file;
    }
    initialize_container(eCodecId, nWidth, nHeight, metadata_tags);
}

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    FFmpegWriterDescriptorOutputConfig output,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags,
    FFmpegWriterQueueConfig queue_config,
    FFmpegWriterKeyframePolicy keyframe_policy)
    : nFps(validated_frame_rate(nFps)),
      descriptor_output_(true),
      output_path_(std::move(output.video_display_label_)),
      keyframe_file_(std::move(output.keyframe_sidecar_display_label_)),
      finalization_file_(
          std::move(output.finalization_sidecar_display_label_)),
      max_video_bytes_(output.max_video_bytes_),
      keyframe_policy_(validated_keyframe_policy(std::move(keyframe_policy))),
      queue_config_(queue_config)
{
    // Validate the policy before transferring any descriptor ownership. If
    // construction rejects an unknown policy, output's destructor still owns
    // and closes the caller-supplied duplicates.
    video_fd_ = output.release_video_fd();
    keyframe_fd_ = output.release_keyframe_sidecar_fd();
    finalization_fd_ = output.release_finalization_sidecar_fd();
    output_label_ = std::filesystem::path(output_path_).stem().string();
    codec_id_ = eCodecId;
    initialize_container(eCodecId, nWidth, nHeight, metadata_tags);
}

void FFmpegWriter::initialize_container(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags)
{
    // Ensure legacy pathname outputs are created as the invoking user even
    // when running under sudo. Descriptor mode never performs a pathname open.
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    // Construction-time failures throw (docs/error_handling_convention.md):
    // a writer that failed to open must be impossible to ignore. Callers run
    // either on worker threads with catch/latch/drain boundaries or inside
    // init-path try/catch blocks. Partial state is released before throwing
    // because a throwing constructor does not run the destructor.
    try {
        oc = avformat_alloc_context();
        if (!oc) {
            throw std::runtime_error(
                "FFmpegWriter: avformat_alloc_context failed");
        }
        AVOutputFormat *fmt =
            (AVOutputFormat *)av_guess_format("mp4", NULL, NULL);
        if (!fmt) {
            throw std::runtime_error(
                "FFmpegWriter: av_guess_format(mp4) failed");
        }
        oc->oformat = fmt;

        for (const auto& tag : metadata_tags) {
            if (!tag.first.empty() && !tag.second.empty()) {
                av_dict_set(&oc->metadata, tag.first.c_str(),
                            tag.second.c_str(), 0);
            }
        }
        if (av_dict_set(&oc->metadata,
                        OrangeVideoContainerFinalization::
                            kFullFrameRatePlaybackIntentKey,
                        "1", 0) < 0) {
            throw std::runtime_error(
                "FFmpegWriter: could not set full-frame-rate playback intent for " +
                output_path_);
        }

        vs = avformat_new_stream(oc, NULL);
        if (!vs) {
            throw std::runtime_error(
                "FFmpegWriter: could not alloc video stream");
        }
        vs->id = 0;
        vs->time_base = AVRational{1, 90000};
        vs->r_frame_rate = AVRational{nFps, 1};
        vs->avg_frame_rate = AVRational{nFps, 1};

        AVCodecParameters *vpar = vs->codecpar;
        vpar->codec_id = eCodecId;
        vpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vpar->width = nWidth;
        vpar->height = nHeight;
        vpar->format = AV_PIX_FMT_YUV420P;
        vpar->color_range = AVCOL_RANGE_JPEG;

        if (vpar->codec_id == AV_CODEC_ID_H264) {
            vpar->codec_tag = MKTAG('a', 'v', 'c', '1');
        } else if (vpar->codec_id == AV_CODEC_ID_HEVC) {
            vpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }

        if (descriptor_output_) {
            video_io_fd_ = duplicate_fd_cloexec(video_fd_);
            if (video_io_fd_ < 0) {
                throw std::runtime_error(
                    "FFmpegWriter: could not duplicate descriptor AVIO fd for " +
                    output_path_ + ": " + std::to_string(errno));
            }
            if (::ftruncate(video_io_fd_, 0) != 0 ||
                ::lseek(video_io_fd_, 0, SEEK_SET) < 0) {
                throw std::runtime_error(
                    "FFmpegWriter: could not truncate/rewind descriptor output " +
                    output_path_ + ": " + std::to_string(errno));
            }
            constexpr int kAvioBufferSize = 64 * 1024;
            auto* avio_buffer = static_cast<unsigned char*>(
                av_malloc(kAvioBufferSize));
            if (!avio_buffer) {
                throw std::runtime_error(
                    "FFmpegWriter: could not allocate descriptor AVIO buffer");
            }
            custom_avio_ = avio_alloc_context(
                avio_buffer,
                kAvioBufferSize,
                1,
                this,
                nullptr,
                &FFmpegWriter::descriptor_write_packet,
                &FFmpegWriter::descriptor_seek);
            if (!custom_avio_) {
                av_free(avio_buffer);
                throw std::runtime_error(
                    "FFmpegWriter: could not allocate descriptor AVIO context");
            }
            custom_avio_->seekable = AVIO_SEEKABLE_NORMAL;
            oc->pb = custom_avio_;
            oc->flags |= AVFMT_FLAG_CUSTOM_IO;
        } else {
            if (avio_open(&oc->pb, output_path_.c_str(), AVIO_FLAG_WRITE) < 0) {
                throw std::runtime_error(
                    "FFmpegWriter: could not open output file " + output_path_);
            }
        }

        AVDictionary* muxer_options = nullptr;
        if (av_dict_set(&muxer_options, "movflags", "use_metadata_tags", 0) <
            0) {
            av_dict_free(&muxer_options);
            throw std::runtime_error(
                "FFmpegWriter: could not configure QuickTime mdta metadata for " +
                output_path_);
        }
        const int header_result = avformat_write_header(oc, &muxer_options);
        const bool movflags_unconsumed =
            av_dict_get(muxer_options, "movflags", nullptr, 0) != nullptr;
        av_dict_free(&muxer_options);
        if (header_result < 0 || movflags_unconsumed) {
            const std::string failure_detail = header_result < 0
                ? av_error_string(header_result)
                : "movflags=use_metadata_tags was not consumed by the MP4 muxer";
            throw std::runtime_error(
                "FFmpegWriter: avformat_write_header or mdta option failed for " +
                (output_path_.empty() ? std::string("(null)") : output_path_) +
                ": " + failure_detail);
        }
        open_ = true;

        OrangeVideoContainerFinalization::Outcome recording_outcome;
        recording_outcome.header_written = true;
        const bool lifecycle_persisted = persist_finalization_status(
            descriptor_output_, video_fd_, finalization_fd_, output_path_,
            finalization_file_, nFps,
            OrangeVideoContainerFinalization::Status::RecordingOpen,
            recording_outcome, false);
        if (descriptor_output_ && !lifecycle_persisted) {
            throw std::runtime_error(
                "FFmpegWriter: could not persist descriptor recording-open status for " +
                output_path_);
        }
    } catch (...) {
        cleanup_failed_construction();
        throw;
    }
}

int FFmpegWriter::descriptor_write_packet(void* opaque,
                                          uint8_t* buffer,
                                          int buffer_size)
{
    auto* writer = static_cast<FFmpegWriter*>(opaque);
    if (!writer || writer->video_io_fd_ < 0 || !buffer || buffer_size <= 0) {
        return AVERROR(EINVAL);
    }
    const off_t current_position =
        ::lseek(writer->video_io_fd_, 0, SEEK_CUR);
    if (current_position < 0) {
        const int write_error = AVERROR(errno == 0 ? EIO : errno);
        writer->latch_failure(
            FailureKind::PacketWrite,
            write_error,
            "FFmpegWriter descriptor AVIO position query failed");
        return write_error;
    }
    const uint64_t current = static_cast<uint64_t>(current_position);
    const uint64_t requested = static_cast<uint64_t>(buffer_size);
    if (current > writer->max_video_bytes_ ||
        requested > writer->max_video_bytes_ - current) {
        if (!writer->video_size_limit_exceeded()) {
            writer->latch_failure(
                FailureKind::VideoSizeLimit,
                AVERROR(ENOSPC),
                "FFmpegWriter descriptor max_video_bytes exceeded");
        }
        return AVERROR(ENOSPC);
    }
    int completed = 0;
    while (completed < buffer_size) {
        const ssize_t count = ::write(
            writer->video_io_fd_, buffer + completed,
            static_cast<size_t>(buffer_size - completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const int write_error = AVERROR(errno == 0 ? EIO : errno);
            writer->latch_failure(
                FailureKind::PacketWrite,
                write_error,
                "FFmpegWriter descriptor AVIO write failed");
            return completed > 0 ? completed : write_error;
        }
        if (count == 0) {
            writer->latch_failure(
                FailureKind::PacketWrite,
                AVERROR(EIO),
                "FFmpegWriter descriptor AVIO zero-byte write");
            return completed > 0 ? completed : AVERROR(EIO);
        }
        completed += static_cast<int>(count);
    }
    return completed;
}

int64_t FFmpegWriter::descriptor_seek(void* opaque,
                                      int64_t offset,
                                      int whence)
{
    auto* writer = static_cast<FFmpegWriter*>(opaque);
    if (!writer || writer->video_io_fd_ < 0) {
        return AVERROR(EINVAL);
    }
    const int seek_whence = whence & ~AVSEEK_FORCE;
    if (seek_whence == AVSEEK_SIZE) {
        struct stat descriptor_stat {};
        if (::fstat(writer->video_io_fd_, &descriptor_stat) != 0 ||
            descriptor_stat.st_size < 0) {
            return AVERROR(errno == 0 ? EIO : errno);
        }
        return static_cast<int64_t>(descriptor_stat.st_size);
    }
    if (seek_whence != SEEK_SET && seek_whence != SEEK_CUR &&
        seek_whence != SEEK_END) {
        return AVERROR(EINVAL);
    }
    if (offset < static_cast<int64_t>(std::numeric_limits<off_t>::min()) ||
        offset > static_cast<int64_t>(std::numeric_limits<off_t>::max())) {
        return AVERROR(EOVERFLOW);
    }
    const off_t position = ::lseek(
        writer->video_io_fd_, static_cast<off_t>(offset), seek_whence);
    if (position < 0) {
        return AVERROR(errno);
    }
    return static_cast<int64_t>(position);
}

void FFmpegWriter::release_custom_avio() noexcept
{
    if (!custom_avio_) {
        return;
    }
    if (oc && oc->pb == custom_avio_) {
        oc->pb = nullptr;
    }
    av_freep(&custom_avio_->buffer);
    avio_context_free(&custom_avio_);
}

void FFmpegWriter::close_descriptor_fds() noexcept
{
    if (video_io_fd_ >= 0) {
        ::close(video_io_fd_);
        video_io_fd_ = -1;
    }
    if (video_fd_ >= 0) {
        ::close(video_fd_);
        video_fd_ = -1;
    }
    if (keyframe_fd_ >= 0) {
        ::close(keyframe_fd_);
        keyframe_fd_ = -1;
    }
    if (finalization_fd_ >= 0) {
        ::close(finalization_fd_);
        finalization_fd_ = -1;
    }
}

void FFmpegWriter::cleanup_failed_construction() noexcept
{
    if (descriptor_output_) {
        release_custom_avio();
    } else if (oc && oc->pb) {
        avio_closep(&oc->pb);
    }
    if (oc) {
        avformat_free_context(oc);
        oc = nullptr;
    }
    vs = nullptr;
    open_ = false;
    close_descriptor_fds();
}

#ifdef ORANGE_FFMPEG_WRITER_TESTING
void FFmpegWriter::test_invalidate_descriptor_video_io_for_finalize() noexcept
{
    if (video_io_fd_ >= 0) {
        (void)::close(video_io_fd_);
        video_io_fd_ = -1;
    }
}

void FFmpegWriter::test_invalidate_descriptor_keyframe_for_finalize() noexcept
{
    if (keyframe_fd_ >= 0) {
        (void)::close(keyframe_fd_);
        keyframe_fd_ = -1;
    }
}

void FFmpegWriter::test_invalidate_descriptor_finalization_for_finalize() noexcept
{
    if (finalization_fd_ >= 0) {
        (void)::close(finalization_fd_);
        finalization_fd_ = -1;
    }
}
#endif

FFmpegWriter::~FFmpegWriter()
{
    (void)finalize();
}

bool FFmpegWriter::finalize() noexcept
{
    std::lock_guard<std::mutex> finalization_lock(finalization_mutex_);
    if (finalization_complete_.load(std::memory_order_acquire)) {
        return finalization_succeeded_;
    }
    // Linearize close-of-admission against push_packet's entire admission and
    // descriptor-counter update. Release this mutex before joining so the
    // writer thread remains independent of the producer-side boundary.
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        finalization_started_.store(true, std::memory_order_release);
        quit_requested_.store(true, std::memory_order_release);
    }

    // Joining is part of this API, rather than a caller convention. This makes
    // the returned failure snapshot terminal.
    try {
        join_thread();
        // A caller may use FFmpegWriter synchronously without create_thread().
        // Drain any already-admitted packets before touching the mux trailer.
        if (queued_packets_.load(std::memory_order_relaxed) > 0) {
            write_thread_loop();
        }
    } catch (...) {
        writer_thread_error_.store(true, std::memory_order_release);
        latch_failure(
            FailureKind::Thread,
            AVERROR(EIO),
            "FFmpegWriter terminal queue drain failed");
    }

    try {
        OrangeVideoContainerFinalization::Outcome outcome;
        outcome.header_written = open_;
        if (!persist_finalization_status(
                descriptor_output_, video_fd_, finalization_fd_, output_path_,
                finalization_file_, nFps,
                OrangeVideoContainerFinalization::Status::Finalizing,
                outcome, false) && descriptor_output_) {
            latch_failure(
                FailureKind::SidecarWrite,
                AVERROR(EIO),
                "FFmpegWriter finalizing-sidecar write failed");
        }

        if (oc) {
            if (open_) {
                if (descriptor_output_ &&
                    is_fixed_gop_policy(keyframe_policy_) &&
                    (!descriptor_zero_based_contiguous_ ||
                     !descriptor_keyframe_policy_satisfied_)) {
                    latch_failure(
                        FailureKind::PacketEnqueue,
                        AVERROR(EPROTO),
                        "FFmpegWriter descriptor fixed-GOP frame sequence is not zero-based contiguous");
                }
                write_keyframe_sidecar();
                // Send a NULL packet to flush any muxer-internal frames.
                const int flush_result = av_interleaved_write_frame(oc, NULL);
                if (flush_result < 0) {
                    latch_failure(
                        FailureKind::MuxerFlush,
                        flush_result,
                        "av_interleaved_write_frame(NULL) failed");
                    std::cerr << "FFMPEG: muxer flush failed for " << output_path_
                              << ": " << av_error_string(flush_result)
                              << std::endl;
                }
                outcome.trailer_attempted = true;
                const int trailer_result = av_write_trailer(oc);
                outcome.trailer_written = trailer_result >= 0;
                if (trailer_result < 0) {
                    outcome.trailer_error_code = trailer_result;
                    outcome.trailer_error = av_error_string(trailer_result);
                    latch_failure(
                        FailureKind::MuxerFlush,
                        trailer_result,
                        "av_write_trailer failed");
                    std::cerr << "FFMPEG: av_write_trailer failed for "
                              << output_path_ << ": " << outcome.trailer_error
                              << std::endl;
                }

                // Any earlier packet/mux/keyframe-evidence failure makes the
                // strict terminal artifact fail closed even if trailer write
                // itself happened to return success.
                if (failed()) {
                    outcome.trailer_written = false;
                    if (outcome.trailer_error.empty()) {
                        try {
                            const FFmpegWriterFailureStats failures =
                                failure_stats();
                            outcome.trailer_error_code =
                                failures.last_error_code;
                            outcome.trailer_error = failures.last_error.empty()
                                ? "FFmpegWriter packet, muxer, or sidecar failure"
                                : failures.last_error;
                        } catch (...) {
                            outcome.trailer_error =
                                "FFmpegWriter packet, muxer, or sidecar failure";
                        }
                    }
                }
            } else {
                outcome.trailer_error = "header_not_written";
            }
            if (descriptor_output_) {
                outcome.output_close_attempted = true;
                int io_error = 0;
                if (custom_avio_) {
                    avio_flush(custom_avio_);
                    io_error = custom_avio_->error;
                    release_custom_avio();
                } else {
                    io_error = AVERROR(EIO);
                    outcome.output_close_error =
                        "descriptor_output_io_context_unavailable";
                }
                int sync_error = 0;
                if (video_io_fd_ < 0) {
                    sync_error = AVERROR(EBADF);
                } else if (::fsync(video_io_fd_) != 0) {
                    sync_error = AVERROR(errno == 0 ? EIO : errno);
                }
                int close_error = 0;
                if (video_io_fd_ >= 0 && ::close(video_io_fd_) != 0) {
                    close_error = AVERROR(errno == 0 ? EIO : errno);
                }
                video_io_fd_ = -1;
                const int descriptor_close_error =
                    io_error < 0 ? io_error
                                 : (sync_error < 0 ? sync_error : close_error);
                outcome.output_closed = descriptor_close_error >= 0;
                if (descriptor_close_error < 0) {
                    outcome.output_close_error_code = descriptor_close_error;
                    if (outcome.output_close_error.empty()) {
                        outcome.output_close_error =
                            av_error_string(descriptor_close_error);
                    }
                    latch_failure(
                        FailureKind::MuxerFlush,
                        descriptor_close_error,
                        "FFmpegWriter descriptor AVIO close/fsync failed");
                    std::cerr << "FFMPEG: descriptor AVIO close failed for "
                              << output_path_ << ": "
                              << outcome.output_close_error << std::endl;
                }
            } else if (oc->pb) {
                outcome.output_close_attempted = true;
                const int close_result = avio_closep(&oc->pb);
                outcome.output_closed = close_result >= 0;
                if (close_result < 0) {
                    outcome.output_close_error_code = close_result;
                    outcome.output_close_error = av_error_string(close_result);
                    latch_failure(
                        FailureKind::MuxerFlush,
                        close_result,
                        "FFmpegWriter output close failed");
                    std::cerr << "FFMPEG: avio_closep failed for " << output_path_
                              << ": " << outcome.output_close_error << std::endl;
                }
            } else {
                outcome.output_close_error =
                    "output_io_context_unavailable";
                latch_failure(
                    FailureKind::MuxerFlush,
                    AVERROR(EIO),
                    "FFmpegWriter output I/O context unavailable at finalize");
            }
            avformat_free_context(oc);
            oc = nullptr;
            vs = nullptr;
            open_ = false;
        } else {
            outcome.trailer_error = "format_context_unavailable";
            outcome.output_close_error = "format_context_unavailable";
            latch_failure(
                FailureKind::MuxerFlush,
                AVERROR(EIO),
                "FFmpegWriter format context unavailable at finalize");
        }

        if (outcome.trailer_written && outcome.output_closed) {
            outcome.playback_intent_patch_attempted = true;
            const bool patch_applied = descriptor_output_
                ? OrangeVideoContainerFinalization::
                      PatchFullFrameRatePlaybackIntent(
                          video_fd_, output_path_,
                          &outcome.playback_intent_patch_error)
                : OrangeVideoContainerFinalization::
                      PatchFullFrameRatePlaybackIntent(
                          output_path_, &outcome.playback_intent_patch_error);
            if (!patch_applied) {
                if (descriptor_output_) {
                    latch_failure(
                        FailureKind::MuxerFlush,
                        AVERROR(EIO),
                        "FFmpegWriter playback-intent patch failed");
                }
                std::cerr << "FFMPEG: " << outcome.playback_intent_patch_error
                          << " for " << output_path_ << std::endl;
            } else {
                outcome.playback_intent_patch_applied = true;
            }
        }

        const auto terminal_status =
            OrangeVideoContainerFinalization::ClassifyTerminalStatus(outcome);
        const bool terminal_persisted = persist_finalization_status(
            descriptor_output_, video_fd_, finalization_fd_, output_path_,
            finalization_file_, nFps, terminal_status, outcome, true);
        if (!terminal_persisted && descriptor_output_) {
            latch_failure(
                FailureKind::SidecarWrite,
                AVERROR(EIO),
                "FFmpegWriter terminal-sidecar write failed");
        }
        release_custom_avio();
        close_descriptor_fds();
        const bool terminal_evidence_ok =
            descriptor_output_ ? terminal_persisted : true;
        finalization_succeeded_ = terminal_evidence_ok &&
            terminal_status == OrangeVideoContainerFinalization::Status::Complete &&
            !failed();
    } catch (...) {
        latch_failure(
            FailureKind::MuxerFlush,
            AVERROR(EIO),
            "FFmpegWriter unexpected terminal finalization exception");
        if (descriptor_output_) {
            release_custom_avio();
        } else if (oc && oc->pb) {
            (void)avio_closep(&oc->pb);
        }
        if (oc) {
            avformat_free_context(oc);
            oc = nullptr;
        }
        vs = nullptr;
        open_ = false;
        close_descriptor_fds();
        finalization_succeeded_ = false;
    }
    finalization_complete_.store(true, std::memory_order_release);
    return finalization_succeeded_;
}

bool FFmpegWriter::push_packet(uint8_t* pData,
                               int nBytes,
                               int64_t nPts,
                               uint64_t gop_index,
                               bool is_last_packet_in_gop,
                               uint64_t gop_release_started_ns)
{
    return push_packet_impl(pData, nBytes, nPts, gop_index,
                             is_last_packet_in_gop,
                             gop_release_started_ns, std::nullopt);
}

bool FFmpegWriter::push_packet_with_keyframe(uint8_t* pData,
                                              int nBytes,
                                              int64_t nPts,
                                              bool is_keyframe,
                                              uint64_t gop_index,
                                              bool is_last_packet_in_gop,
                                              uint64_t gop_release_started_ns)
{
    return push_packet_impl(pData, nBytes, nPts, gop_index,
                             is_last_packet_in_gop,
                             gop_release_started_ns, is_keyframe);
}

bool FFmpegWriter::push_packet_impl(uint8_t* pData,
                                    int nBytes,
                                    int64_t nPts,
                                    uint64_t gop_index,
                                    bool is_last_packet_in_gop,
                                    uint64_t gop_release_started_ns,
                                    std::optional<bool> actual_keyframe)
{
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (finalization_started_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!open_ || !vs) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EPIPE),
            "FFmpegWriter is not open for packet enqueue");
        return false;
    }
    NVTX_ENCODE_DYNAMIC([&]() {
        return "FFmpegWriter push_packet label=" + output_label_ +
               " pts=" + std::to_string(nPts) +
               " gop=" + std::to_string(gop_index) +
               " bytes=" + std::to_string(nBytes);
    }());
    const uint64_t push_start_ns = steady_clock_now_ns();
    if (nBytes <= 0) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EINVAL),
            "FFmpegWriter packet size is not positive");
        return false;
    }
    if (quit_requested_.load(std::memory_order_acquire)) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EPIPE),
            "FFmpegWriter packet enqueue attempted after quit");
        return false;
    }
    if (failed()) {
        return false;
    }
    if (!pData) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EINVAL),
            "FFmpegWriter packet data is null");
        return false;
    }

    const bool has_explicit_pts = nPts >= 0;
    const int64_t frame_index =
        has_explicit_pts ? nPts : sequential_frame_counter_;
    if (!frame_index_has_representable_pts(frame_index, nFps)) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EOVERFLOW),
            "FFmpegWriter frame index cannot be represented in the 90 kHz timeline");
        return false;
    }
    if (descriptor_output_ &&
        descriptor_total_frames_ == std::numeric_limits<uint64_t>::max()) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EOVERFLOW),
            "FFmpegWriter descriptor frame counter overflow");
        return false;
    }

    const size_t queued_packets = queued_packets_.load(std::memory_order_relaxed);
    const size_t queued_bytes = queued_bytes_.load(std::memory_order_relaxed);
    const bool packets_limited = queue_config_.max_queued_packets > 0;
    const bool bytes_limited = queue_config_.max_queued_bytes > 0;
    const bool exceeds_packet_limit =
        packets_limited &&
        (queued_packets >= queue_config_.max_queued_packets ||
         queue_config_.max_queued_packets - queued_packets < 1);
    const bool exceeds_byte_limit =
        bytes_limited &&
        (queued_bytes > queue_config_.max_queued_bytes ||
         static_cast<size_t>(nBytes) >
             queue_config_.max_queued_bytes - queued_bytes);
    if (exceeds_packet_limit || exceeds_byte_limit) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(ENOBUFS),
            "FFmpegWriter packet queue overflow");
        const bool first_overflow = !queue_overflowed_.exchange(true, std::memory_order_relaxed);
        queue_overflow_events_.fetch_add(1, std::memory_order_relaxed);
        if (first_overflow) {
            std::cerr << "FFMPEG: packet queue overflow"
                      << " packets=" << queued_packets
                      << " bytes=" << queued_bytes
                      << " limit_packets=" << queue_config_.max_queued_packets
                      << " limit_bytes=" << queue_config_.max_queued_bytes
                      << std::endl;
        }
        return false;
    }

    const uint64_t alloc_copy_start_ns = steady_clock_now_ns();
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        latch_failure(
            FailureKind::PacketAllocation,
            AVERROR(ENOMEM),
            "av_packet_alloc failed");
        std::cerr << "FFMPEG: av_packet_alloc failed for " << output_path_
                  << std::endl;
        return false;
    }
    const int packet_alloc_result = av_new_packet(pkt, nBytes);
    if (packet_alloc_result < 0) {
        latch_failure(
            FailureKind::PacketAllocation,
            packet_alloc_result,
            "av_new_packet failed");
        std::cerr << "FFMPEG: av_new_packet failed for " << output_path_
                  << ": " << av_error_string(packet_alloc_result) << std::endl;
        av_packet_free(&pkt);
        return false;
    }
    memcpy(pkt->data, pData, nBytes);
    const uint64_t alloc_copy_end_ns = steady_clock_now_ns();

    pkt->pts = av_rescale_q(frame_index, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    const int64_t next_pts = av_rescale_q(
        frame_index + 1, AVRational{1, nFps}, vs->time_base);
    // Keep a defensive check at the subtraction boundary as well. For
    // non-negative frame indices a negative or regressing result is an
    // overflow/sentinel, never a valid timestamp.
    if (pkt->pts < 0 || next_pts < pkt->pts) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EOVERFLOW),
            "FFmpegWriter rescaled frame timestamp overflow");
        av_packet_free(&pkt);
        return false;
    }
    pkt->duration = std::max<int64_t>(1, next_pts - pkt->pts);
    if (has_explicit_pts) {
        sequential_frame_counter_ = std::max<int64_t>(sequential_frame_counter_, frame_index + 1);
    } else {
        sequential_frame_counter_++;
    }

    const bool packet_idr = packet_has_idr(pData, static_cast<size_t>(nBytes));
    if (actual_keyframe && *actual_keyframe != packet_idr) {
        if (descriptor_output_) {
            descriptor_keyframe_policy_satisfied_ = false;
        }
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(EPROTO),
            "FFmpegWriter explicit keyframe flag disagrees with packet IDR");
        av_packet_free(&pkt);
        return false;
    }
    const bool is_idr = actual_keyframe.value_or(packet_idr);
    if (descriptor_output_) {
        const bool expected_idr =
            (frame_index % static_cast<int64_t>(keyframe_policy_.gop_length)) == 0;
        if (is_idr != expected_idr) {
            descriptor_keyframe_policy_satisfied_ = false;
            latch_failure(
                FailureKind::PacketEnqueue,
                AVERROR(EPROTO),
                expected_idr
                    ? "FFmpegWriter descriptor output requires an IDR at every GOP boundary"
                    : "FFmpegWriter descriptor output received an unexpected interior IDR");
            av_packet_free(&pkt);
            return false;
        }
    }
    try {
        if (is_idr) {
            pkt->flags |= AV_PKT_FLAG_KEY;
            if (!descriptor_output_) {
                keyframe_frames_.push_back(frame_index);
            }
        }
    } catch (const std::exception& exception) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(ENOMEM),
            "FFmpegWriter keyframe bookkeeping failed");
        std::cerr << "FFMPEG: packet bookkeeping failed for " << output_path_
                  << ": " << exception.what() << std::endl;
        av_packet_free(&pkt);
        return false;
    } catch (...) {
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(ENOMEM),
            "FFmpegWriter keyframe bookkeeping failed");
        std::cerr << "FFMPEG: packet bookkeeping failed for " << output_path_
                  << ": non-std exception" << std::endl;
        av_packet_free(&pkt);
        return false;
    }
    const size_t new_packet_count = queued_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t new_byte_count =
        queued_bytes_.fetch_add(static_cast<size_t>(pkt->size), std::memory_order_relaxed) +
        static_cast<size_t>(pkt->size);
    update_peak(peak_queued_packets_, new_packet_count);
    update_peak(peak_queued_bytes_, new_byte_count);
    QueuedPacket queued_packet;
    queued_packet.packet = pkt;
    queued_packet.enqueued_at_ns = steady_clock_now_ns();
    queued_packet.gop_index = gop_index;
    queued_packet.is_last_packet_in_gop = is_last_packet_in_gop;
    queued_packet.gop_release_started_ns = gop_release_started_ns;
    const uint64_t queue_push_start_ns = steady_clock_now_ns();
    try {
        m_queue.push(queued_packet);
    } catch (const std::exception& exception) {
        queued_packets_.fetch_sub(1, std::memory_order_relaxed);
        queued_bytes_.fetch_sub(
            static_cast<size_t>(pkt->size), std::memory_order_relaxed);
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(ENOMEM),
            "FFmpegWriter packet queue push failed");
        std::cerr << "FFMPEG: packet queue push failed for " << output_path_
                  << ": " << exception.what() << std::endl;
        av_packet_free(&pkt);
        return false;
    } catch (...) {
        queued_packets_.fetch_sub(1, std::memory_order_relaxed);
        queued_bytes_.fetch_sub(
            static_cast<size_t>(pkt->size), std::memory_order_relaxed);
        latch_failure(
            FailureKind::PacketEnqueue,
            AVERROR(ENOMEM),
            "FFmpegWriter packet queue push failed");
        std::cerr << "FFMPEG: packet queue push failed for " << output_path_
                  << ": non-std exception" << std::endl;
        av_packet_free(&pkt);
        return false;
    }
    if (descriptor_output_) {
        if (!descriptor_has_frame_index_) {
            descriptor_has_frame_index_ = true;
            descriptor_first_frame_index_ = frame_index;
            descriptor_last_frame_index_ = frame_index;
            descriptor_zero_based_contiguous_ = frame_index == 0;
        } else {
            const bool next_is_contiguous =
                descriptor_last_frame_index_ !=
                    std::numeric_limits<int64_t>::max() &&
                frame_index == descriptor_last_frame_index_ + 1;
            descriptor_zero_based_contiguous_ =
                descriptor_zero_based_contiguous_ && next_is_contiguous;
            descriptor_last_frame_index_ = frame_index;
        }
        ++descriptor_total_frames_;
        if (is_idr) {
            ++descriptor_keyframe_frames_;
        } else {
            ++descriptor_non_keyframe_frames_;
        }
    }
    const uint64_t push_end_ns = steady_clock_now_ns();
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        if (alloc_copy_end_ns >= alloc_copy_start_ns) {
            observe_latency_ns(
                &latency_stats_.packet_alloc_copy,
                alloc_copy_end_ns - alloc_copy_start_ns);
        }
        if (push_end_ns >= queue_push_start_ns) {
            observe_latency_ns(
                &latency_stats_.queue_push,
                push_end_ns - queue_push_start_ns);
        }
        if (push_end_ns >= push_start_ns) {
            observe_latency_ns(
                &latency_stats_.push_packet_total,
                push_end_ns - push_start_ns);
        }
    }
    return true;
}

void FFmpegWriter::create_thread()
{
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (finalization_started_.load(std::memory_order_acquire)) {
        return;
    }
    if (!open_) {
        latch_failure(
            FailureKind::Thread,
            AVERROR(EPIPE),
            "FFmpegWriter thread creation attempted while closed");
        return;
    }
    if (m_thread.joinable()) {
        latch_failure(
            FailureKind::Thread,
            AVERROR(EALREADY),
            "FFmpegWriter thread is already running");
        return;
    }
    quit_requested_.store(false, std::memory_order_release);
    try {
        m_thread = std::thread(&FFmpegWriter::write_thread, this);
    } catch (...) {
        latch_failure(
            FailureKind::Thread,
            AVERROR(EAGAIN),
            "FFmpegWriter thread creation failed");
        throw;
    }
#ifdef __linux__
    std::string thread_name = output_label_.empty()
        ? "FFmpegWriter"
        : "FFm_" + output_label_;
    if (thread_name.size() > 15) {
        thread_name.resize(15);
    }
    (void)pthread_setname_np(m_thread.native_handle(), thread_name.c_str());
#endif
}

void FFmpegWriter::quit_thread()
{
    // The queue is polled. An atomic stop bit avoids allocating a sentinel at
    // finalization, where allocation failure could otherwise strand join().
    // The loop drains all packets admitted before it observes an empty queue.
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    quit_requested_.store(true, std::memory_order_release);
}

void FFmpegWriter::join_thread()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FFmpegWriter::write_one_pkt(AVPacket* pkt)
{
    if (!open_ || !oc) {
        return;
    }
    NVTX_ENCODE_DYNAMIC(std::string("FFmpegWriter av_interleaved_write_frame"));
    const int ret = av_interleaved_write_frame(oc, pkt);
    if (ret < 0) {
        latch_failure(
            FailureKind::PacketWrite,
            ret,
            "av_interleaved_write_frame(packet) failed");
        std::cerr << "FFMPEG: Error while writing video frame for "
                  << output_path_ << ": " << av_error_string(ret) << std::endl;
    }
}

bool FFmpegWriter::write_packet(uint8_t* pData, int nBytes, int64_t nPts)
{
    return push_packet(pData, nBytes, nPts);
}

void FFmpegWriter::write_thread()
{
    // An exception escaping this thread would call std::terminate and kill
    // the whole process (stranding every encoder shard's in-flight GOPs).
    // Log it, latch the error flag and exit the thread cleanly instead so the
    // owner can still finalize the container.
    try {
        write_thread_loop();
    } catch (const std::exception& e) {
        writer_thread_error_.store(true, std::memory_order_release);
        latch_failure(
            FailureKind::Thread,
            AVERROR(EIO),
            "FFmpegWriter writer thread exception");
        std::cerr << "FFMPEG: writer thread [" << output_label_
                  << "] exception: " << e.what() << std::endl;
    } catch (...) {
        writer_thread_error_.store(true, std::memory_order_release);
        latch_failure(
            FailureKind::Thread,
            AVERROR(EIO),
            "FFmpegWriter writer thread exception");
        std::cerr << "FFMPEG: writer thread [" << output_label_
                  << "] non-std exception" << std::endl;
    }
}

void FFmpegWriter::write_thread_loop()
{
    while (true) {
        QueuedPacket queued_packet;
        if (m_queue.pop(queued_packet)) {
            if (queued_packet.packet) {
                const uint64_t dequeue_started_ns = steady_clock_now_ns();
                queued_packets_.fetch_sub(1, std::memory_order_relaxed);
                queued_bytes_.fetch_sub(
                    static_cast<size_t>(queued_packet.packet->size),
                    std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(latency_mutex_);
                    if (dequeue_started_ns >= queued_packet.enqueued_at_ns) {
                        observe_latency_ns(
                            &latency_stats_.queue_wait,
                            dequeue_started_ns - queued_packet.enqueued_at_ns);
                    }
                }

                const uint64_t write_started_ns = steady_clock_now_ns();
                NVTX_ENCODE_DYNAMIC([&]() {
                    return "FFmpegWriter write_packet label=" + output_label_ +
                           " gop=" + std::to_string(queued_packet.gop_index) +
                           " bytes=" + std::to_string(queued_packet.packet->size);
                }());
                write_one_pkt(queued_packet.packet);
                const uint64_t write_finished_ns = steady_clock_now_ns();
                {
                    std::lock_guard<std::mutex> lock(latency_mutex_);
                    observe_latency_ns(
                        &latency_stats_.packet_write,
                        write_finished_ns - write_started_ns);
                    if (queued_packet.is_last_packet_in_gop &&
                        queued_packet.gop_release_started_ns > 0 &&
                        write_finished_ns >= queued_packet.gop_release_started_ns) {
                        observe_latency_ns(
                            &latency_stats_.gop_release_to_last_write,
                            write_finished_ns - queued_packet.gop_release_started_ns);
                    }
                }
                av_packet_free(&queued_packet.packet);
            } else {
                break;
            }
        }
        else {
            if (quit_requested_.load(std::memory_order_acquire)) {
                break;
            }
            usleep(100);
        }
    }
}

std::string FFmpegWriter::keyframe_sidecar_path() const
{
    if (keyframe_file_.empty()) {
        return {};
    }
    std::filesystem::path p(keyframe_file_);
    if (p.extension() == ".csv") {
        p.replace_extension(".json");
    } else if (p.extension().empty()) {
        p += ".json";
    }
    return p.string();
}

void FFmpegWriter::write_keyframe_sidecar()
{
    const std::string out_path = descriptor_output_
        ? keyframe_file_
        : keyframe_sidecar_path();
    if (out_path.empty()) {
        return;
    }

    const char* codec_name = "unknown";
    if (codec_id_ == AV_CODEC_ID_H264) {
        codec_name = "h264";
    } else if (codec_id_ == AV_CODEC_ID_HEVC) {
        codec_name = "hevc";
    }

    std::ostringstream document;
    document << "{\n";
    if (descriptor_output_) {
        // Closed, constant-size descriptor schema. A pre-terminal crash leaves
        // the pre-created inode empty, recording_open/finalizing, or invalid
        // after a partial ftruncate/write; none can be mistaken for this
        // terminal=true document by the strict evidence validator.
        document << "  \"schema_id\": "
                    "\"orange.spatial_roi_keyframe_summary\",\n";
        document << "  \"schema_version\": 1,\n";
        document << "  \"terminal\": true,\n";
        document << "  \"codec\": \"" << codec_name << "\",\n";
        document << "  \"fps\": " << nFps << ",\n";
        document << "  \"total_frames\": "
                 << descriptor_total_frames_ << ",\n";
        document << "  \"frame_index_sequence\": {\n";
        document << "    \"first\": ";
        if (descriptor_has_frame_index_) {
            document << descriptor_first_frame_index_;
        } else {
            document << "null";
        }
        document << ",\n";
        document << "    \"last\": ";
        if (descriptor_has_frame_index_) {
            document << descriptor_last_frame_index_;
        } else {
            document << "null";
        }
        document << ",\n";
        document << "    \"zero_based_contiguous\": "
                 << (descriptor_zero_based_contiguous_ ? "true" : "false")
                 << "\n";
        document << "  },\n";
        document << "  \"keyframe_policy\": {\n";
        document << "    \"name\": \""
                 << keyframe_policy_summary_name(keyframe_policy_)
                 << "\",\n";
        document << "    \"keyframe_frames\": "
                 << descriptor_keyframe_frames_ << ",\n";
        document << "    \"non_keyframe_frames\": "
                 << descriptor_non_keyframe_frames_ << ",\n";
        const bool all_idr_satisfied =
            descriptor_keyframe_frames_ == descriptor_total_frames_ &&
            descriptor_non_keyframe_frames_ == 0;
        const bool fixed_gop_satisfied =
            descriptor_zero_based_contiguous_ &&
            descriptor_keyframe_policy_satisfied_;
        document << "    \"satisfied\": "
                 << ((keyframe_policy_.name == kFFmpegWriterAllFramesIdrPolicyName
                          ? all_idr_satisfied
                          : fixed_gop_satisfied)
                         ? "true"
                         : "false")
                 << "\n";
        document << "  }\n";
    } else {
        // Preserve the legacy pathname schema exactly.
        document << "  \"codec\": \"" << codec_name << "\",\n";
        document << "  \"fps\": " << nFps << ",\n";
        document << "  \"total_frames\": "
                 << static_cast<int64_t>(sequential_frame_counter_) << ",\n";
        document << "  \"keyframe_frames\": [";
        for (size_t i = 0; i < keyframe_frames_.size(); ++i) {
            if (i) {
                document << ", ";
            }
            document << keyframe_frames_[i];
        }
        document << "]\n";
    }
    document << "}\n";

    if (descriptor_output_) {
        const std::string bytes = document.str();
        const bool wrote = keyframe_fd_ >= 0 &&
            ::ftruncate(keyframe_fd_, 0) == 0 &&
            ::lseek(keyframe_fd_, 0, SEEK_SET) >= 0 &&
            write_all_fd(keyframe_fd_, bytes) &&
            ::fsync(keyframe_fd_) == 0;
        if (!wrote) {
            const int write_error = errno == 0 ? EIO : errno;
            latch_failure(
                FailureKind::SidecarWrite,
                AVERROR(write_error),
                "FFmpegWriter descriptor keyframe sidecar write failed");
            std::cerr << "FFMPEG: Failed while writing descriptor keyframe sidecar "
                      << out_path << std::endl;
        }
        return;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(out_path, std::ios::trunc);
    if (!out.is_open()) {
        const int open_error = errno == 0 ? EIO : errno;
        latch_failure(
            FailureKind::SidecarWrite,
            AVERROR(open_error),
            "FFmpegWriter keyframe sidecar open failed");
        std::cerr << "FFMPEG: Failed to write keyframe sidecar " << out_path << std::endl;
        return;
    }
    out << document.str();
    out.flush();
    if (!out) {
        latch_failure(
            FailureKind::SidecarWrite,
            AVERROR(EIO),
            "FFmpegWriter keyframe sidecar write failed");
        std::cerr << "FFMPEG: Failed while writing keyframe sidecar "
                  << out_path << std::endl;
    }
}

bool FFmpegWriter::packet_has_idr(const uint8_t* data, size_t size) const
{
    if (!data || size == 0) {
        return false;
    }
    if (codec_id_ == AV_CODEC_ID_H264) {
        return packet_has_idr_h264(data, size);
    }
    if (codec_id_ == AV_CODEC_ID_HEVC) {
        return packet_has_idr_hevc(data, size);
    }
    return false;
}

bool FFmpegWriter::packet_has_idr_h264(const uint8_t* data, size_t size) const
{
    bool found_start_code = false;
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t start_len = 0;
        if (is_start_code(data + i, size - i, &start_len)) {
            found_start_code = true;
            size_t nal_start = i + start_len;
            if (nal_start >= size) {
                break;
            }
            uint8_t nal_type = data[nal_start] & 0x1F;
            if (nal_type == 5) {
                return true;
            }
            i = nal_start;
        }
    }
    if (found_start_code) {
        return false;
    }

    size_t offset = 0;
    while (offset + 4 <= size) {
        uint32_t nal_len = read_be32(data + offset);
        offset += 4;
        if (nal_len == 0 || offset + nal_len > size) {
            break;
        }
        uint8_t nal_type = data[offset] & 0x1F;
        if (nal_type == 5) {
            return true;
        }
        offset += nal_len;
    }
    return false;
}

bool FFmpegWriter::packet_has_idr_hevc(const uint8_t* data, size_t size) const
{
    bool found_start_code = false;
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t start_len = 0;
        if (is_start_code(data + i, size - i, &start_len)) {
            found_start_code = true;
            size_t nal_start = i + start_len;
            if (nal_start >= size) {
                break;
            }
            uint8_t nal_type = (data[nal_start] >> 1) & 0x3F;
            if (nal_type == 19 || nal_type == 20) {
                return true;
            }
            i = nal_start;
        }
    }
    if (found_start_code) {
        return false;
    }

    size_t offset = 0;
    while (offset + 4 <= size) {
        uint32_t nal_len = read_be32(data + offset);
        offset += 4;
        if (nal_len == 0 || offset + nal_len > size) {
            break;
        }
        uint8_t nal_type = (data[offset] >> 1) & 0x3F;
        if (nal_type == 19 || nal_type == 20) {
            return true;
        }
        offset += nal_len;
    }
    return false;
}
