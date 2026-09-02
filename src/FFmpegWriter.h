// FFmpegWriter.h
#pragma once
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
};
#include <iostream>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <utility>
#include <vector>
#include "latency_stats.h"
#include "thread.h"

struct FFmpegWriterQueueConfig {
    size_t max_queued_packets = 0;
    size_t max_queued_bytes = 0;
};

// The descriptor writer normally enforces all-IDR packets for compatibility
// with the original lossless ROI stream.  A fixed-GOP policy is an explicit
// opt-in: frame zero and every subsequent zero-based GOP boundary must be an
// IDR, while packets inside the GOP must not be IDR.  The writer retains only
// counters and a small amount of sequence state, so this policy remains O(1)
// for arbitrarily long recordings.
inline constexpr const char* kFFmpegWriterAllFramesIdrPolicyName =
    "all_frames_idr";
inline constexpr const char* kFFmpegWriterFixedGopIdrPolicyName =
    "fixed_gop_idr";

struct FFmpegWriterKeyframePolicy {
    std::string name = kFFmpegWriterAllFramesIdrPolicyName;
    std::uint32_t gop_length = 1;
};

struct FFmpegWriterLatencyStats {
    LatencyAggregateStats push_packet_total;
    LatencyAggregateStats packet_alloc_copy;
    LatencyAggregateStats queue_push;
    LatencyAggregateStats queue_wait;
    LatencyAggregateStats packet_write;
    LatencyAggregateStats gop_release_to_last_write;
};

// Failure state is intentionally a value type. A snapshot taken before
// finalize() is provisional; after finalize() returns it includes all terminal
// mux, close/fsync, playback-patch, and sidecar outcomes. The counters count
// occurrences; failed is a monotonic latch for the writer's lifetime.
struct FFmpegWriterFailureStats {
    bool failed = false;
    uint64_t packet_allocation_failures = 0;
    uint64_t packet_enqueue_failures = 0;
    uint64_t packet_write_failures = 0;
    // Includes NULL-packet flush, trailer, output close/fsync, and required
    // playback-intent patch failures; the established field name is retained
    // for snapshot/wire compatibility.
    uint64_t muxer_flush_failures = 0;
    uint64_t sidecar_write_failures = 0;
    uint64_t video_size_limit_failures = 0;
    uint64_t thread_failures = 0;
    uint64_t total_failures = 0;
    int last_error_code = 0;
    std::string last_error;
};

// Descriptor-backed output authority for artifact-root-owned recording files.
//
// The constructor treats its integer arguments as borrowed descriptors and
// duplicates each one with close-on-exec. This object owns those duplicates,
// is deliberately move-only, and transfers them to FFmpegWriter. The caller's
// original descriptors remain valid for independent inode verification.
// The video descriptor must be read-write because finalization verifies and
// patches the held media inode; sidecars must be writable. max_video_bytes is
// a required nonzero hard ceiling enforced before each custom-AVIO write.
// Labels must be non-empty contract-relative paths and are never reopened as
// authority.
class FFmpegWriterDescriptorOutputConfig {
public:
    explicit FFmpegWriterDescriptorOutputConfig(
        int video_fd,
        int keyframe_sidecar_fd,
        int finalization_sidecar_fd,
        uint64_t max_video_bytes,
        std::string video_display_label,
        std::string keyframe_sidecar_display_label,
        std::string finalization_sidecar_display_label);
    ~FFmpegWriterDescriptorOutputConfig();

    FFmpegWriterDescriptorOutputConfig(
        const FFmpegWriterDescriptorOutputConfig&) = delete;
    FFmpegWriterDescriptorOutputConfig& operator=(
        const FFmpegWriterDescriptorOutputConfig&) = delete;
    FFmpegWriterDescriptorOutputConfig(
        FFmpegWriterDescriptorOutputConfig&& other) noexcept;
    FFmpegWriterDescriptorOutputConfig& operator=(
        FFmpegWriterDescriptorOutputConfig&& other) noexcept;

private:
    friend class FFmpegWriter;

    int release_video_fd() noexcept;
    int release_keyframe_sidecar_fd() noexcept;
    int release_finalization_sidecar_fd() noexcept;
    void close_owned_fds() noexcept;

    int video_fd_ = -1;
    int keyframe_sidecar_fd_ = -1;
    int finalization_sidecar_fd_ = -1;
    uint64_t max_video_bytes_ = 0;
    std::string video_display_label_;
    std::string keyframe_sidecar_display_label_;
    std::string finalization_sidecar_display_label_;
};

class FFmpegWriter
{
public:
    // Throws std::runtime_error if the output container cannot be opened
    // (allocation, avio_open or header write failure). A successfully
    // constructed writer is always open; a recording can therefore never
    // silently "record to nowhere" (docs/error_handling_convention.md).
    FFmpegWriter(AVCodecID eCodecId, int nWidth, int nHeight, int nFps, const char *szOutFilePath, const char *metadata_file,
                 const std::vector<std::pair<std::string, std::string>>& metadata_tags = {},
                 FFmpegWriterQueueConfig queue_config = {});
    // Additive descriptor-authoritative constructor. output is move-only so
    // ownership transfer is explicit; legacy pathname construction above is
    // unchanged.
    FFmpegWriter(AVCodecID eCodecId,
                 int nWidth,
                 int nHeight,
                 int nFps,
                 FFmpegWriterDescriptorOutputConfig output,
                 const std::vector<std::pair<std::string, std::string>>& metadata_tags = {},
                 FFmpegWriterQueueConfig queue_config = {},
                 FFmpegWriterKeyframePolicy keyframe_policy = {});
    ~FFmpegWriter();
    // Synchronously closes the packet-admission boundary, drains and joins
    // the writer thread, flushes the muxer, writes the trailer, closes/fsyncs
    // output I/O, applies the playback-intent patch, and persists keyframe and
    // terminal-finalization evidence. The result and failure_stats() are
    // stable when this returns. Repeated calls are idempotent. The destructor
    // invokes this only as a compatibility fallback for legacy callers.
    bool finalize() noexcept;
    bool finalized() const {
        return finalization_complete_.load(std::memory_order_acquire);
    }
    bool write_packet(uint8_t *pData, int nBytes, int64_t nPts);
    // Returns true only after the packet has entered the bounded writer queue.
    // Invalid packets and capacity/encoding failures leave the monotonic
    // failure latch set. Once explicit finalization has closed admission, a
    // concurrent or later call returns false without changing terminal state.
    bool push_packet(uint8_t* pData,
                     int nBytes,
                     int64_t nPts,
                     uint64_t gop_index = 0,
                     bool is_last_packet_in_gop = false,
                     uint64_t gop_release_started_ns = 0);
    // Explicit-keyframe variant used by configured encoders. The boolean is
    // the actual packet flag observed from the encoder; the writer cross-checks
    // it against the packet's IDR NAL and then applies the selected policy.
    bool push_packet_with_keyframe(uint8_t* pData,
                                   int nBytes,
                                   int64_t nPts,
                                   bool is_keyframe,
                                   uint64_t gop_index = 0,
                                   bool is_last_packet_in_gop = false,
                                   uint64_t gop_release_started_ns = 0);
    void create_thread();
    void quit_thread();
    void join_thread();
    void write_one_pkt(AVPacket* pkt);
    bool is_open() const { return open_; }
    // True once the writer thread exited on an exception. The thread never
    // calls exit(); it logs, latches this flag and stops so the owner can
    // still finalize the container.
    bool writer_thread_failed() const { return writer_thread_error_.load(std::memory_order_acquire); }
    // True after any packet allocation/enqueue/write, terminal
    // muxer/close/patch, sidecar, or writer-thread failure. This latch is
    // never cleared.
    bool failed() const { return writer_error_.load(std::memory_order_acquire); }
    bool has_error() const { return failed(); }
    FFmpegWriterFailureStats failure_stats() const;
    bool has_queue_overflowed() const { return queue_overflowed_.load(std::memory_order_relaxed); }
    uint64_t queue_overflow_events() const { return queue_overflow_events_.load(std::memory_order_relaxed); }
    size_t queued_packets() const { return queued_packets_.load(std::memory_order_relaxed); }
    size_t queued_bytes() const { return queued_bytes_.load(std::memory_order_relaxed); }
    size_t peak_queued_packets() const { return peak_queued_packets_.load(std::memory_order_relaxed); }
    size_t peak_queued_bytes() const { return peak_queued_bytes_.load(std::memory_order_relaxed); }
    uint64_t max_video_bytes() const { return max_video_bytes_; }
    bool video_size_limit_exceeded() const {
        return video_size_limit_failures_.load(std::memory_order_relaxed) > 0;
    }
    const FFmpegWriterQueueConfig& queue_config() const { return queue_config_; }
    FFmpegWriterLatencyStats latency_stats() const;
#ifdef ORANGE_FFMPEG_WRITER_TESTING
    // Deterministic terminal-I/O fault injection. These hooks are compiled
    // only into the focused host tests and never into production targets.
    void test_invalidate_descriptor_video_io_for_finalize() noexcept;
    void test_invalidate_descriptor_keyframe_for_finalize() noexcept;
    void test_invalidate_descriptor_finalization_for_finalize() noexcept;
#endif
private:
    enum class FailureKind {
        PacketAllocation,
        PacketEnqueue,
        PacketWrite,
        MuxerFlush,
        SidecarWrite,
        VideoSizeLimit,
        Thread,
    };

    struct QueuedPacket {
        AVPacket* packet = nullptr;
        uint64_t enqueued_at_ns = 0;
        uint64_t gop_index = 0;
        bool is_last_packet_in_gop = false;
        uint64_t gop_release_started_ns = 0;
    };

    AVFormatContext *oc = NULL;
    AVStream *vs = NULL;
    bool open_ = false;
    int nFps = 0;
    int nPts = 0;
    std::ofstream *metadata;
    SafeQueue<QueuedPacket> m_queue; // Queue for packets to be written
    std::thread m_thread;
    int64_t sequential_frame_counter_ = 0; // Counter for sequential frame numbers
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    bool descriptor_output_ = false;
    std::string output_label_;
    std::string output_path_;
    std::string keyframe_file_;
    std::string finalization_file_;
    int video_fd_ = -1;
    int video_io_fd_ = -1;
    int keyframe_fd_ = -1;
    int finalization_fd_ = -1;
    AVIOContext* custom_avio_ = nullptr;
    uint64_t max_video_bytes_ = 0;
    // Legacy pathname output preserves its historical explicit frame list.
    // Descriptor output records an O(1), closed summary under either the
    // default all-IDR policy or an explicit fixed-GOP policy, so a
    // multi-million-frame recording does not consume O(frames) memory/sidecar
    // bytes.
    std::vector<int64_t> keyframe_frames_;
    uint64_t descriptor_total_frames_ = 0;
    uint64_t descriptor_keyframe_frames_ = 0;
    uint64_t descriptor_non_keyframe_frames_ = 0;
    int64_t descriptor_first_frame_index_ = 0;
    int64_t descriptor_last_frame_index_ = 0;
    bool descriptor_has_frame_index_ = false;
    bool descriptor_zero_based_contiguous_ = true;
    FFmpegWriterKeyframePolicy keyframe_policy_;
    bool descriptor_keyframe_policy_satisfied_ = true;
    FFmpegWriterQueueConfig queue_config_;
    std::atomic<size_t> queued_packets_{0};
    std::atomic<size_t> queued_bytes_{0};
    std::atomic<size_t> peak_queued_packets_{0};
    std::atomic<size_t> peak_queued_bytes_{0};
    std::atomic<bool> queue_overflowed_{false};
    std::atomic<uint64_t> queue_overflow_events_{0};
    std::atomic<bool> writer_thread_error_{false};
    std::atomic<bool> writer_error_{false};
    std::atomic<uint64_t> packet_allocation_failures_{0};
    std::atomic<uint64_t> packet_enqueue_failures_{0};
    std::atomic<uint64_t> packet_write_failures_{0};
    std::atomic<uint64_t> muxer_flush_failures_{0};
    std::atomic<uint64_t> sidecar_write_failures_{0};
    std::atomic<uint64_t> video_size_limit_failures_{0};
    std::atomic<uint64_t> thread_failures_{0};
    std::atomic<uint64_t> total_failures_{0};
    std::atomic<bool> quit_requested_{false};
    std::atomic<bool> finalization_started_{false};
    std::atomic<bool> finalization_complete_{false};
    bool finalization_succeeded_ = false;
    mutable std::mutex finalization_mutex_;
    mutable std::mutex failure_mutex_;
    int last_error_code_ = 0;
    std::string last_error_;
    // Packet admission is single-owner in normal operation, but serializing
    // it here makes queue byte/count bounds and keyframe/PTS bookkeeping exact
    // even if a caller accidentally supplies more than one producer.
    mutable std::mutex admission_mutex_;
    mutable std::mutex latency_mutex_;
    FFmpegWriterLatencyStats latency_stats_;
    void initialize_container(
        AVCodecID codec_id,
        int width,
        int height,
        const std::vector<std::pair<std::string, std::string>>& metadata_tags);
    void cleanup_failed_construction() noexcept;
    void release_custom_avio() noexcept;
    void close_descriptor_fds() noexcept;
    static int descriptor_write_packet(void* opaque,
                                       uint8_t* buffer,
                                       int buffer_size);
    static int64_t descriptor_seek(void* opaque,
                                   int64_t offset,
                                   int whence);
    void latch_failure(FailureKind kind, int error_code, const char* operation) noexcept;
    void write_thread();
    void write_thread_loop();
    void write_keyframe_sidecar();
    bool push_packet_impl(uint8_t* pData,
                          int nBytes,
                          int64_t nPts,
                          uint64_t gop_index,
                          bool is_last_packet_in_gop,
                          uint64_t gop_release_started_ns,
                          std::optional<bool> actual_keyframe);
    bool packet_has_idr(const uint8_t* data, size_t size) const;
    bool packet_has_idr_h264(const uint8_t* data, size_t size) const;
    bool packet_has_idr_hevc(const uint8_t* data, size_t size) const;
    std::string keyframe_sidecar_path() const;
};
