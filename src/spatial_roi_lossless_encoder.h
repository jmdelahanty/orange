#pragma once

#include "spatial_roi_recorder_artifact_root.h"
#include "spatial_roi_recorder_cuda_detach.h"
#include "latency_stats.h"
#include "video_encode_profile.h"
#include "FFmpegWriter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace orange::spatial_roi::encoder {

inline constexpr const char* kSpatialRoiLegacyLosslessProfileId =
    "hevc_p7_lossless_cqp0_gop1_v1";
inline constexpr const char* kSpatialRoiP1LowLatencyProfileId =
    "hevc_p1_low_latency_vbr_q20_gop1_v1";
inline constexpr const char* kSpatialRoiP1LowLatencyGop25ProfileId =
    "hevc_p1_low_latency_vbr_q20_gop25_v1";

// The encoder has no pathname authority.  The recorder contract owner opens
// exactly these four files from one descriptor-authorized artifact root and
// transfers the retained handles as a move-only bundle.  The shared root is
// kept alive so every retained file's namespace binding remains meaningful
// until terminal evidence has been sealed.
struct SpatialRoiLosslessEncoderArtifactBundle final {
    std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot> artifact_root;
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> video;
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> metadata_csv;
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> keyframes_json;
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> finalization_json;

    SpatialRoiLosslessEncoderArtifactBundle() = default;
    ~SpatialRoiLosslessEncoderArtifactBundle() = default;
    SpatialRoiLosslessEncoderArtifactBundle(
        const SpatialRoiLosslessEncoderArtifactBundle&) = delete;
    SpatialRoiLosslessEncoderArtifactBundle& operator=(
        const SpatialRoiLosslessEncoderArtifactBundle&) = delete;
    SpatialRoiLosslessEncoderArtifactBundle(
        SpatialRoiLosslessEncoderArtifactBundle&&) noexcept = default;
    SpatialRoiLosslessEncoderArtifactBundle& operator=(
        SpatialRoiLosslessEncoderArtifactBundle&&) noexcept = default;
};

enum class SpatialRoiLosslessFrameResultStatus {
    Encoded,
    Failed,
};

// The immutable terminal encode result for one successfully admitted input.
// Results are delivered in exact admission order.  Sparse source recording
// IDs remain provenance; output_frame_index is the external, dense, one-based
// identity and nvenc_pts is the corresponding internal, zero-based identity.
// A failed/nonencoded result never invents packet evidence or an external
// output index, although nvenc_pts_assigned may retain an attempted submission.
struct SpatialRoiLosslessFrameResult {
    SpatialRoiLosslessFrameResultStatus status =
        SpatialRoiLosslessFrameResultStatus::Failed;
    ipc::SpatialRoiIpcCorrelation correlation;
    ipc::SpatialRoiRecorderCudaDetachGeometry geometry;
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = -1;

    std::uint64_t output_frame_index = 0;
    bool nvenc_pts_assigned = false;
    std::uint64_t nvenc_pts = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t encoded_bytes = 0;
    bool keyframe = false;
    std::string failure_reason;
};

// Return true only after the recorder has durably accepted this result into
// its own bounded evidence path. Returning false (with an optional reason) or
// throwing is a terminal encoder failure. The callback runs synchronously on
// the encoder owner thread and must not block. Reentrant Enqueue/Finalize calls
// are rejected, and no encoder mutex is held while the callback runs.
using SpatialRoiLosslessFrameResultCallback = std::function<bool(
    const SpatialRoiLosslessFrameResult& result,
    std::string* error_out)>;

#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
// Deterministic allocation/exception seams compiled only into the focused
// encoder test binary. Production recorder builds do not expose or carry a
// fault-injection callback in their encoder contract.
enum class SpatialRoiLosslessEncoderTestFaultPoint {
    BeforeDetachedWorkItemAllocation,
    BeforeDeviceViewWorkItemAllocation,
    BeforeDeviceViewAckAllocation,
    BeforeQueueInsertion,
    BeforeMetadataMappingWrite,
    AfterOwnerCleanup,
};
using SpatialRoiLosslessEncoderTestFaultInjector =
    std::function<void(SpatialRoiLosslessEncoderTestFaultPoint)>;
#endif

struct SpatialRoiLosslessWriterTerminalSnapshot {
    // Numeric counters and close truth are captured after FFmpegWriter's
    // explicit, synchronous finalization has drained its thread, flushed the
    // muxer, closed the output, and published both terminal sidecars.
    bool observed = false;
    bool failure_latched = false;
    bool packet_write_error_latched = false;
    bool writer_thread_failure_latched = false;
    bool queue_overflow_latched = false;
    bool close_finalization_validated = false;
    bool close_finalization_failure_latched = false;
    std::uint64_t packet_allocation_failures = 0;
    std::uint64_t packet_enqueue_failures = 0;
    std::uint64_t packet_write_failures = 0;
    std::uint64_t muxer_flush_failures = 0;
    std::uint64_t sidecar_write_failures = 0;
    std::uint64_t video_size_limit_failures = 0;
    std::uint64_t thread_failures = 0;
    std::uint64_t total_failures = 0;
    std::uint64_t queue_overflow_events = 0;
    int last_error_code = 0;
    std::string first_failure_reason;
    std::string close_finalization_failure_reason;
};

// A coherent, non-terminal operational view of one recorder-owned encoder.
// The recorder control thread may sample this while the owner and FFmpegWriter
// threads are active. It contains no handles or references and is therefore
// safe to retain after the sampling call returns. The input queue limits are
// the exact bounds enforced by this encoder; the writer limits are copied from
// FFmpegWriter's queue configuration.
struct SpatialRoiLosslessWriterOperationalSnapshot {
    bool observed = false;
    std::size_t queue_max_packets = 0;
    std::size_t queue_max_bytes = 0;
    std::size_t queue_current_packets = 0;
    std::size_t queue_current_bytes = 0;
    std::size_t queue_peak_packets = 0;
    std::size_t queue_peak_bytes = 0;
    bool queue_overflowed = false;
    std::uint64_t queue_overflow_events = 0;
    FFmpegWriterFailureStats failure_stats;
    FFmpegWriterLatencyStats latency_stats;
};

// This snapshot is intentionally separate from the existing terminal stats:
// it exposes queue pressure and latency while a stream is running without
// changing the established terminal/evidence shape. Sampling takes the
// encoder mutex and uses FFmpegWriter's own atomic/mutex-protected accessors;
// it performs no I/O and does not allocate on the encode path.
struct SpatialRoiLosslessEncoderOperationalSnapshot {
    bool initialized = false;
    bool accepting = false;
    bool failed = false;
    bool finalized = false;

    std::size_t input_queue_max_frames = 0;
    std::uint64_t input_queue_max_bytes = 0;
    std::size_t input_queue_current_frames = 0;
    std::uint64_t input_queue_current_bytes = 0;
    std::size_t input_queue_peak_frames = 0;
    std::uint64_t input_queue_peak_bytes = 0;

    std::uint64_t enqueue_attempted = 0;
    std::uint64_t enqueue_admitted = 0;
    std::uint64_t dequeued = 0;
    std::uint64_t rejected = 0;
    std::uint64_t queue_overflows = 0;

    LatencyAggregateStats queue_wait_latency;
    LatencyAggregateStats source_copy_latency;
    LatencyAggregateStats encode_call_latency;
    LatencyAggregateStats admission_to_result_latency;

    SpatialRoiLosslessWriterOperationalSnapshot writer;
};

// This is the recorder's already-verified, one-stream encode contract.  It is
// intentionally a value type rather than the full-frame external-recorder parser
// result: callers must provide the strict fields that were authenticated by
// the spatial ROI recorder contract before constructing this core.
struct SpatialRoiLosslessEncoderConfig {
    ipc::SpatialRoiIpcStreamIdentity stream;
    ipc::SpatialRoiRecorderCudaDetachGeometry geometry;
    int source_gpu_id = -1;
    int recorder_gpu_id = -1;
    int assigned_shard_id = -1;

    std::string profile_id = kSpatialRoiLegacyLosslessProfileId;
    std::string codec = "hevc";
    std::string preset = "p7";
    std::string tuning = "lossless";
    bool lossless = true;
    std::string rate_control_mode = "cqp";
    std::uint32_t quality_value = 0;
    std::uint32_t gop_length = 1;
    bool aq = false;
    bool temporal_aq = false;
    bool lookahead = false;
    std::uint32_t lookahead_depth = 0;
    std::uint32_t fps = 0;
    std::string input_format = "mono8";
    std::string encoded_format = "nv12";
    bool no_resize = true;
    bool luma_preserved_exactly = true;
    std::uint32_t neutral_chroma_value = 128;

    // Both queues are bounded. queue_capacity is the maximum number of
    // detached frames waiting for the one owner thread; max_queue_bytes is an
    // independent byte budget for their eventual queued NV12 views. Raw
    // detached Mono8 is prepared only after dequeue by that owner.
    std::size_t queue_capacity = 0;
    std::uint64_t max_queue_bytes = 0;
    std::size_t writer_queue_max_packets = 0;
    std::size_t writer_queue_max_bytes = 0;

    // Each source copy gets one absolute deadline beginning immediately before
    // CopyToDeviceFrame and covering its stream-completion query; caller
    // acknowledgement waits use the same bound. A CUDA driver call that never
    // returns cannot be interrupted here, and Finalize still joins the owner
    // and writer threads, so a process supervisor must apply its own outer
    // termination bound in that case.
    std::uint32_t operation_timeout_ms = 2000;

    // The stream's four output artifacts are already opened and authorized by
    // the recorder contract owner. No output pathname is accepted here.
    SpatialRoiLosslessEncoderArtifactBundle artifacts;

    // Hard long-run limits minted by the verified recorder contract. Frame
    // admission checks max_frames_per_stream before consuming a dense index;
    // max_media_bytes_per_stream is enforced by FFmpegWriter's descriptor AVIO.
    std::uint64_t max_frames_per_stream = 0;
    std::uint64_t max_media_bytes_per_stream = 0;

    // Required. Exactly one terminal result is attempted for every frame for
    // which admission succeeded. Callback failure is explicit and terminal;
    // a result is never dropped, replaced, or silently overwritten.
    SpatialRoiLosslessFrameResultCallback frame_result_callback;

#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
    SpatialRoiLosslessEncoderTestFaultInjector test_fault_injector;
#endif
};

// Identity and timing supplied with one immutable recorder-owned NV12 view.
// The correlation stream must exactly match config.stream. GPU fields are
// repeated so a view cannot silently come from a different recorder binding.
struct SpatialRoiLosslessFrameMetadata {
    ipc::SpatialRoiIpcCorrelation correlation;
    std::uint64_t camera_timestamp_ns = 0;
    std::uint64_t timestamp_sys_ns = 0;
    int source_gpu_id = -1;
    int assigned_gpu_id = -1;
    int assigned_shard_id = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t byte_length = 0;
    std::uint64_t row_pitch_bytes = 0;
};

// device_nv12 is immutable until Enqueue returns. lifetime must retain the
// allocation for the asynchronous queue path; the encoder drops it only after
// the copy into NvEncoderCuda's private input surface has synchronized.
struct SpatialRoiLosslessDeviceView {
    const unsigned char* device_nv12 = nullptr;
    std::shared_ptr<const void> lifetime;
    SpatialRoiLosslessFrameMetadata metadata;
};

struct SpatialRoiLosslessEncoderStats {
    std::uint64_t enqueue_attempted = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t dequeued = 0;
    std::uint64_t rejected = 0;
    std::uint64_t queue_overflows = 0;
    std::uint64_t copy_completed = 0;
    // Number of detached Mono8 inputs copied directly into the luma plane of
    // a neutral-chroma NVENC ring surface by the encoder owner.
    std::uint64_t mono8_input_copies = 0;
    std::uint64_t source_releases = 0;
    std::uint64_t encoded_frames = 0;
    std::uint64_t encoded_packets = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t copy_failures = 0;
    std::uint64_t source_quarantines = 0;
    std::uint64_t destination_quarantines = 0;
    std::uint64_t encode_failures = 0;
    std::uint64_t writer_failures = 0;
    std::uint64_t writer_queue_overflows = 0;
    std::uint64_t frame_results_emitted = 0;
    std::uint64_t encoded_results = 0;
    std::uint64_t failed_results = 0;
    std::uint64_t result_callback_failures = 0;
    std::uint64_t peak_queue_depth = 0;
    std::uint64_t peak_queue_bytes = 0;
    std::uint64_t queue_depth = 0;
    std::uint64_t queue_bytes = 0;
    LatencyAggregateStats queue_wait_latency;
    LatencyAggregateStats source_copy_latency;
    LatencyAggregateStats encode_call_latency;
    LatencyAggregateStats admission_to_result_latency;
    std::uint64_t finalize_calls = 0;
    // This is the aggregate terminal-success bit: it requires the writer's
    // exact complete/patch-applied sidecar and a successful metadata flush.
    bool finalized = false;
    bool failed = false;
    bool source_release_safe = true;
    bool metadata_flushed = false;
    bool media_finalization_validated = false;
    bool artifacts_sealed = false;
};

// Constructed exactly once after the owner and writer threads have terminated.
// Callers receive shared ownership of a const object, so later API misuse or
// statistics reads cannot rewrite the evidence used for finalization.
struct SpatialRoiLosslessEncoderTerminalSnapshot {
    ipc::SpatialRoiIpcStreamIdentity stream;
    bool terminal = false;
    bool successful = false;
    bool drain_completed = false;
    bool metadata_flushed = false;
    bool media_finalization_validated = false;
    bool artifacts_sealed = false;
    bool all_enqueue_attempts_accounted = false;
    bool nonempty_stream = false;
    bool all_admitted_results_emitted = false;
    bool source_release_safe = false;
    bool source_quarantined = false;
    bool destination_quarantined = false;
    std::string terminal_reason;
    SpatialRoiLosslessEncoderStats counts;
    SpatialRoiLosslessWriterTerminalSnapshot writer;
    SpatialRoiLosslessWriterOperationalSnapshot writer_operational;
};

// Host-only validation and profile construction seams. They reject any
// non-strict profile field instead of normalizing it into a different stream.
bool validate_spatial_roi_lossless_encoder_config(
    const SpatialRoiLosslessEncoderConfig& config,
    std::string* error_out = nullptr);

// Generic validator. It accepts the legacy lossless profile, the original
// P1/low-latency/VBR-Q20/GOP1 profile, and the explicit GOP-25 variant. All
// variants preserve the exact one-input/one-packet/dense-PTS evidence contract;
// only the keyframe cadence changes for the GOP-25 profile.
bool validate_spatial_roi_encoder_config(
    const SpatialRoiLosslessEncoderConfig& config,
    std::string* error_out = nullptr);

VideoEncodeProfile build_spatial_roi_lossless_encoder_profile(
    const SpatialRoiLosslessEncoderConfig& config);

VideoEncodeProfile build_spatial_roi_encoder_profile(
    const SpatialRoiLosslessEncoderConfig& config);

const char* spatial_roi_lossless_frame_result_status_name(
    SpatialRoiLosslessFrameResultStatus status) noexcept;

bool validate_spatial_roi_lossless_frame_result(
    const SpatialRoiLosslessFrameResult& result,
    std::string* error_out = nullptr);

class SpatialRoiLosslessEncoder final {
public:
    explicit SpatialRoiLosslessEncoder(SpatialRoiLosslessEncoderConfig config);
    // Normal destruction synchronously finalizes. Destruction from inside the
    // owner-thread result callback requests stop without self-joining; the
    // detached worker retains the implementation until drain and cleanup are
    // complete. This makes callback-triggered release of the last wrapper
    // owner safe from std::terminate and implementation use-after-free.
    ~SpatialRoiLosslessEncoder();

    SpatialRoiLosslessEncoder(const SpatialRoiLosslessEncoder&) = delete;
    SpatialRoiLosslessEncoder& operator=(const SpatialRoiLosslessEncoder&) = delete;

    bool valid() const noexcept;
    bool failed() const noexcept;
    std::string error() const;
    const VideoEncodeProfile& profile() const noexcept;
    SpatialRoiLosslessEncoderStats stats() const noexcept;

    // Returns a coherent operational sample suitable for control-thread
    // telemetry. It never performs I/O and is safe to call concurrently with
    // Enqueue, encoding, writer draining, or Finalize.
    SpatialRoiLosslessEncoderOperationalSnapshot
    operational_snapshot() const noexcept;

    // Null before Finalize completes. Every successful/repeated Finalize call
    // returns the same immutable snapshot object.
    std::shared_ptr<const SpatialRoiLosslessEncoderTerminalSnapshot>
    terminal_snapshot() const noexcept;

    // Takes ownership of the detached result. Its Release is performed by the
    // owner thread after the synchronized device-to-NVENC input copy. A false
    // return from this detached-frame overload is strictly pre-admission: the
    // detached allocation has been returned safely and no result callback will
    // later be emitted for that frame. The diagnostic overload preserves the
    // bounded rejection reason needed by the recorder ACK/evidence path. The
    // first admitted frame fixes the input mode for this encoder instance;
    // mixing this packed-Mono8 path with generic NV12 views is rejected.
    bool Enqueue(ipc::SpatialRoiRecorderDetachedFrame&& detached,
                 const SpatialRoiFrameDescriptor& descriptor) noexcept;
    bool Enqueue(ipc::SpatialRoiRecorderDetachedFrame&& detached,
                 const SpatialRoiFrameDescriptor& descriptor,
                 std::string* error_out) noexcept;

    // Enqueues an immutable view and waits only until its source copy has
    // completed (bounded by operation_timeout_ms). The owner thread then
    // continues encode/mux work while the caller may safely let lifetime
    // expire. A timeout is terminal; the source is retained in quarantine.
    // If admission occurred before that timeout, its terminal callback is
    // still delivered even though this bounded wait returns false. This
    // generic NV12 path likewise cannot be mixed with detached Mono8 frames in
    // one encoder instance.
    bool Enqueue(const SpatialRoiLosslessDeviceView& view) noexcept;

    // Stops admission, drains the bounded queue, calls NvEncoderCuda::EndEncode
    // at most once, requires that the zero-delay profile left no delayed frame
    // packets, and finalizes the writer. Repeated calls return the original result. If CUDA copy
    // completion is uncertain, the destination and source are quarantined,
    // EndEncode/destruction are skipped, and this returns false.
    bool Finalize() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace orange::spatial_roi::encoder
