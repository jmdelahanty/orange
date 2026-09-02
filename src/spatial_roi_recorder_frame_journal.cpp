#include "spatial_roi_recorder_frame_journal.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

constexpr std::size_t kDiagnosticBytes = 1024;

constexpr const char* kDetachStatuses[] = {
    "detached", "invalid_argument", "wrong_device", "busy",
    "pool_exhausted", "cuda_error", "source_quarantined", "stopped"};

bool bounded_printable(const std::string& value,
                       const std::size_t maximum,
                       const bool allow_empty = true)
{
    if ((!allow_empty && value.empty()) || value.size() > maximum) {
        return false;
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

void set_error_noexcept(std::string* error_out, const std::string& value) noexcept
{
    if (!error_out) {
        return;
    }
    try {
        *error_out = value;
    } catch (...) {
        // Error reporting is advisory for the callback-facing API.
    }
}

std::string bounded_diagnostic(const std::string& value) noexcept
{
    try {
        std::string result;
        result.reserve(std::min(value.size(), kDiagnosticBytes));
        for (std::size_t index = 0;
             index < value.size() && result.size() < kDiagnosticBytes;
             ++index) {
            const unsigned char byte = static_cast<unsigned char>(value[index]);
            result.push_back((byte < 0x20U || byte == 0x7fU)
                                 ? '?'
                                 : static_cast<char>(byte));
        }
        if (result.empty()) {
            result = "spatial ROI frame journal failure";
        }
        return result;
    } catch (...) {
        return "spatial ROI frame journal failure";
    }
}

void increment(std::uint64_t* value) noexcept
{
    if (value && *value != std::numeric_limits<std::uint64_t>::max()) {
        ++*value;
    }
}

bool same_stream(const ipc::SpatialRoiIpcStreamIdentity& lhs,
                 const ipc::SpatialRoiIpcStreamIdentity& rhs) noexcept
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial && lhs.roi_id == rhs.roi_id &&
           lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256;
}

bool same_raster(const SpatialRoiFrameRaster& lhs,
                 const SpatialRoiFrameRaster& rhs) noexcept
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_rect(const SpatialRoiFrameRect& lhs,
               const SpatialRoiFrameRect& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width &&
           lhs.height == rhs.height;
}

bool same_padding(const SpatialRoiFramePadding& lhs,
                  const SpatialRoiFramePadding& rhs) noexcept
{
    return lhs.left == rhs.left && lhs.top == rhs.top &&
           lhs.right == rhs.right && lhs.bottom == rhs.bottom &&
           lhs.value_mono8 == rhs.value_mono8;
}

bool same_correlation(const ipc::SpatialRoiIpcCorrelation& lhs,
                      const ipc::SpatialRoiIpcCorrelation& rhs) noexcept
{
    return same_stream(lhs.stream, rhs.stream) &&
           lhs.local_frame_id == rhs.local_frame_id &&
           lhs.camera_frame_id == rhs.camera_frame_id &&
           lhs.recording_frame_id == rhs.recording_frame_id &&
           lhs.roi_stream_frame_index == rhs.roi_stream_frame_index;
}

bool same_descriptor(const SpatialRoiFrameDescriptor& lhs,
                     const SpatialRoiFrameDescriptor& rhs) noexcept
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.local_frame_id == rhs.local_frame_id &&
           lhs.camera_frame_id == rhs.camera_frame_id &&
           lhs.recording_frame_id == rhs.recording_frame_id &&
           lhs.roi_stream_frame_index == rhs.roi_stream_frame_index &&
           lhs.camera_timestamp_ns == rhs.camera_timestamp_ns &&
           lhs.timestamp_sys_ns == rhs.timestamp_sys_ns &&
           lhs.roi_id == rhs.roi_id && lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256 &&
           same_raster(lhs.native_raster, rhs.native_raster) &&
           same_rect(lhs.content_rect, rhs.content_rect) &&
           same_raster(lhs.encoded_raster, rhs.encoded_raster) &&
           same_rect(lhs.encoded_content_rect, rhs.encoded_content_rect) &&
           same_padding(lhs.padding, rhs.padding) &&
           lhs.source_pixel_format == rhs.source_pixel_format &&
           lhs.bytes == rhs.bytes && lhs.source_gpu_id == rhs.source_gpu_id &&
           lhs.assigned_gpu_id == rhs.assigned_gpu_id &&
           lhs.assigned_shard_id == rhs.assigned_shard_id &&
           lhs.routing_policy == rhs.routing_policy;
}

bool same_geometry(const ipc::SpatialRoiRecorderCudaDetachGeometry& lhs,
                   const SpatialRoiFrameDescriptor& rhs) noexcept
{
    return same_raster(lhs.native_raster, rhs.native_raster) &&
           same_rect(lhs.content_rect, rhs.content_rect) &&
           same_raster(lhs.encoded_raster, rhs.encoded_raster) &&
           same_rect(lhs.encoded_content_rect, rhs.encoded_content_rect) &&
           same_padding(lhs.padding, rhs.padding) &&
           lhs.routing_policy == rhs.routing_policy;
}

}  // namespace

bool derive_spatial_roi_frame_journal_pending_bound(
    const std::size_t max_frames_per_stream,
    const std::size_t encoder_waiting_queue_capacity,
    std::size_t* bound_out,
    std::string* error_out) noexcept
{
    constexpr std::size_t kOwnerAndTerminalRejectionHeadroom = 2;
    if (!bound_out) {
        set_error_noexcept(error_out,
                           "frame journal pending-bound output is required");
        return false;
    }
    if (max_frames_per_stream == 0 || encoder_waiting_queue_capacity == 0) {
        set_error_noexcept(
            error_out,
            "frame journal pending-bound inputs must be positive");
        return false;
    }
    if (encoder_waiting_queue_capacity >
        std::numeric_limits<std::size_t>::max() -
            kOwnerAndTerminalRejectionHeadroom) {
        set_error_noexcept(error_out,
                           "frame journal pending-bound addition overflowed");
        return false;
    }
    *bound_out = std::min(
        max_frames_per_stream,
        encoder_waiting_queue_capacity + kOwnerAndTerminalRejectionHeadroom);
    if (error_out) {
        try {
            error_out->clear();
        } catch (...) {
        }
    }
    return true;
}

SpatialRoiRecorderFrameJournal::SpatialRoiRecorderFrameJournal(
    SpatialRoiRecorderFrameJournalConfig config)
    : shared_writer_(std::move(config.shared_writer)),
      writer_(config.writer ? config.writer : shared_writer_.get()),
      expected_stream_(std::move(config.expected_stream)),
      max_frames_(config.max_frames),
      max_pending_entries_(config.max_pending_entries)
{
    std::string error;
    try {
        if (config.writer && shared_writer_) {
            latch_failure_locked("exactly one evidence writer form is required",
                                 nullptr);
            return;
        }
        if (!writer_) {
            latch_failure_locked("evidence writer is required", nullptr);
            return;
        }
        if (max_frames_ == 0) {
            latch_failure_locked("max_frames must be positive", nullptr);
            return;
        }
        if (max_pending_entries_ == 0) {
            latch_failure_locked("max_pending_entries must be positive", nullptr);
            return;
        }
        if (!ipc::validate_spatial_roi_ipc_stream_identity(expected_stream_,
                                                           &error)) {
            latch_failure_locked(
                error.empty() ? "expected stream identity is invalid" : error,
                nullptr);
            return;
        }
        valid_ = true;
    } catch (const std::exception& exception) {
        latch_failure_locked(exception.what(), nullptr);
    } catch (...) {
        latch_failure_locked("frame journal construction failed", nullptr);
    }
}

SpatialRoiRecorderFrameJournal::SpatialRoiRecorderFrameJournal(
    SpatialRoiRecorderEvidenceWriter& writer,
    ipc::SpatialRoiIpcStreamIdentity expected_stream,
    const std::size_t max_frames,
    const std::size_t max_pending_entries)
    : SpatialRoiRecorderFrameJournal(
          SpatialRoiRecorderFrameJournalConfig{std::move(expected_stream),
                                               &writer,
                                               nullptr,
                                               max_frames,
                                               max_pending_entries})
{
}

SpatialRoiRecorderFrameJournal::SpatialRoiRecorderFrameJournal(
    std::shared_ptr<SpatialRoiRecorderEvidenceWriter> writer,
    ipc::SpatialRoiIpcStreamIdentity expected_stream,
    const std::size_t max_frames,
    const std::size_t max_pending_entries)
    : SpatialRoiRecorderFrameJournal(
          SpatialRoiRecorderFrameJournalConfig{std::move(expected_stream),
                                               nullptr,
                                               std::move(writer),
                                               max_frames,
                                               max_pending_entries})
{
}

bool SpatialRoiRecorderFrameJournal::valid() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_ && !fatal_;
}

bool SpatialRoiRecorderFrameJournal::fatal_latched() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fatal_;
}

bool SpatialRoiRecorderFrameJournal::AcceptTransportOutcome(
    const SpatialRoiRecorderFrameTransportOutcome& outcome,
    std::string* error_out) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_ || fatal_) {
            set_error_noexcept(error_out, first_failure_reason_);
            return false;
        }
        try {
            return accept_transport_locked(outcome, error_out);
        } catch (const std::exception& exception) {
            return latch_failure_locked(exception.what(), error_out);
        } catch (...) {
            return latch_failure_locked("transport outcome handling failed",
                                        error_out);
        }
    } catch (...) {
        set_error_noexcept(error_out, "frame journal mutex failure");
        return false;
    }
}

bool SpatialRoiRecorderFrameJournal::AcceptEncoderResult(
    const encoder::SpatialRoiLosslessFrameResult& result,
    std::string* error_out) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_ || fatal_) {
            set_error_noexcept(error_out, first_failure_reason_);
            return false;
        }
        try {
            return accept_encoder_locked(result, error_out);
        } catch (const std::exception& exception) {
            return latch_failure_locked(exception.what(), error_out);
        } catch (...) {
            return latch_failure_locked("encoder result handling failed",
                                        error_out);
        }
    } catch (...) {
        set_error_noexcept(error_out, "frame journal mutex failure");
        return false;
    }
}

bool SpatialRoiRecorderFrameJournal::final_ready() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return valid_ && !fatal_ && pending_.empty() &&
               transport_count_ == appended_count_ &&
               (transport_count_ == 0 ||
                next_flush_index_ ==
                    static_cast<std::uint64_t>(transport_count_) + 1U);
    } catch (...) {
        return false;
    }
}

std::size_t SpatialRoiRecorderFrameJournal::pending_entries() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::size_t SpatialRoiRecorderFrameJournal::max_frames() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return max_frames_;
}

std::size_t SpatialRoiRecorderFrameJournal::max_pending_entries() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return max_pending_entries_;
}

std::size_t SpatialRoiRecorderFrameJournal::transport_count() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_count_;
}

std::size_t SpatialRoiRecorderFrameJournal::appended_count() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return appended_count_;
}

SpatialRoiRecorderFrameJournalCounters
SpatialRoiRecorderFrameJournal::counters() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

SpatialRoiRecorderFrameJournalOperationalSnapshot
SpatialRoiRecorderFrameJournal::operational_snapshot() const noexcept
{
    SpatialRoiRecorderFrameJournalOperationalSnapshot snapshot;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.valid = valid_ && !fatal_;
        snapshot.fatal_latched = fatal_;
        snapshot.max_frames = max_frames_;
        snapshot.pending_entries_limit = max_pending_entries_;
        snapshot.pending_entries_current = pending_.size();
        snapshot.transport_count = transport_count_;
        snapshot.appended_count = appended_count_;
        snapshot.next_flush_index = next_flush_index_;
        snapshot.counters = counters_;
        snapshot.final_ready =
            snapshot.valid && pending_.empty() &&
            transport_count_ == appended_count_ &&
            (transport_count_ == 0 ||
             next_flush_index_ ==
                 static_cast<std::uint64_t>(transport_count_) + 1U);
        snapshot.observation_succeeded = true;
    } catch (...) {
        // All fields retain their conservative defaults.  In particular, a
        // failed observation can never be mistaken for valid/final-ready
        // evidence by a lifecycle serializer.
    }
    return snapshot;
}

std::string SpatialRoiRecorderFrameJournal::first_failure_reason() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_failure_reason_;
    } catch (...) {
        return "spatial ROI frame journal failure";
    }
}

bool SpatialRoiRecorderFrameJournal::Drain(const std::size_t max_entries,
                                           std::string* error_out) noexcept
{
    try {
        std::lock_guard<std::mutex> drain_lock(drain_mutex_);
        return drain_ready(max_entries, error_out);
    } catch (const std::exception& exception) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return latch_failure_locked(exception.what(), error_out);
        } catch (...) {
            set_error_noexcept(error_out, "evidence drain failed");
            return false;
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return latch_failure_locked("evidence drain failed", error_out);
        } catch (...) {
            set_error_noexcept(error_out, "evidence drain failed");
            return false;
        }
    }
}

bool SpatialRoiRecorderFrameJournal::latch_failure_locked(
    const std::string& reason,
    std::string* error_out) noexcept
{
    if (!fatal_) {
        fatal_ = true;
        valid_ = false;
        try {
            first_failure_reason_ = bounded_diagnostic(reason);
        } catch (...) {
            try {
                first_failure_reason_ = "spatial ROI frame journal failure";
            } catch (...) {
                // A no-throw callback must remain no-throw even under an
                // exhausted allocator. The fatal bit is authoritative.
            }
        }
    }
    set_error_noexcept(error_out, first_failure_reason_);
    return false;
}

bool SpatialRoiRecorderFrameJournal::reject_locked(
    const std::string& reason,
    std::string* error_out) noexcept
{
    increment(&counters_.invalid_input_rejections);
    return latch_failure_locked(reason, error_out);
}

bool SpatialRoiRecorderFrameJournal::validate_transport(
    const SpatialRoiRecorderFrameTransportOutcome& outcome,
    std::string* error_out) const
{
    const std::string& detach_status = outcome.detach_status;
    const bool source_release_safe = outcome.source_release_safe;
    if (!validate_spatial_roi_frame_descriptor(outcome.descriptor, error_out)) {
        return false;
    }
    if (!same_stream(
            ipc::spatial_roi_ipc_stream_identity_from_descriptor(
                outcome.descriptor),
            expected_stream_)) {
        set_error_noexcept(error_out,
                           "transport frame stream identity does not match");
        return false;
    }
    const std::uint64_t index = outcome.descriptor.roi_stream_frame_index;
    if (index == 0 || index > static_cast<std::uint64_t>(max_frames_)) {
        set_error_noexcept(error_out,
                           "transport ROI frame index exceeds journal bounds");
        return false;
    }
    if (!bounded_printable(detach_status, 64, false) ||
        !bounded_printable(outcome.dispatch_reason,
                           ipc::kSpatialRoiIpcMaxTextBytes) ||
        !bounded_printable(outcome.ack_reason,
                           ipc::kSpatialRoiIpcMaxTextBytes) ||
        !bounded_printable(outcome.ack_error,
                           ipc::kSpatialRoiIpcMaxTextBytes) ||
        !bounded_printable(outcome.release_reason,
                           ipc::kSpatialRoiIpcMaxTextBytes) ||
        !bounded_printable(outcome.release_error,
                           ipc::kSpatialRoiIpcMaxTextBytes)) {
        set_error_noexcept(error_out, "transport lifecycle text is invalid");
        return false;
    }
    if (std::find(std::begin(kDetachStatuses), std::end(kDetachStatuses),
                  detach_status) == std::end(kDetachStatuses)) {
        set_error_noexcept(error_out, "detach status is not recognized");
        return false;
    }
    const bool admitted = outcome.dispatch_admitted;
    if (admitted) {
        if (!outcome.dispatch_reason.empty()) {
            set_error_noexcept(error_out,
                               "accepted dispatch must not carry a reason");
            return false;
        }
        if (!source_release_safe) {
            set_error_noexcept(error_out,
                               "accepted dispatch requires source-safe detach");
            return false;
        }
    } else if (outcome.dispatch_reason.empty() &&
               detach_status == "detached") {
        set_error_noexcept(error_out,
                           "rejected dispatch requires a bounded reason");
        return false;
    }
    if (source_release_safe && !outcome.ack_attempted) {
        set_error_noexcept(error_out,
                           "source-safe dispatch requires an ACK attempt");
        return false;
    }
    if (!outcome.ack_attempted && (outcome.ack_sent || outcome.ack_accepted ||
                           !outcome.ack_reason.empty() ||
                           !outcome.ack_error.empty())) {
        set_error_noexcept(error_out, "ACK evidence is attempted inconsistently");
        return false;
    }
    if (outcome.ack_sent && !outcome.ack_attempted) {
        set_error_noexcept(error_out, "sent ACK was not attempted");
        return false;
    }
    if (outcome.ack_accepted != admitted) {
        set_error_noexcept(error_out,
                           "ACK payload accepted bit must equal dispatch admission");
        return false;
    }
    if (outcome.ack_accepted && !outcome.ack_reason.empty()) {
        set_error_noexcept(error_out, "accepted ACK must not carry a reason");
        return false;
    }
    if (outcome.ack_attempted && !outcome.ack_accepted &&
        outcome.ack_reason.empty()) {
        set_error_noexcept(error_out, "rejected ACK requires a wire reason");
        return false;
    }
    if (!outcome.ack_sent && outcome.ack_attempted &&
        outcome.ack_error.empty()) {
        set_error_noexcept(error_out, "failed ACK write requires an error");
        return false;
    }
    if (outcome.ack_sent && !outcome.ack_error.empty()) {
        set_error_noexcept(error_out, "sent ACK must not carry a write error");
        return false;
    }
    if (source_release_safe && outcome.ack_sent &&
        !outcome.release_attempted) {
        set_error_noexcept(
            error_out,
            "source-safe dispatch with sent ACK requires a RELEASE attempt");
        return false;
    }
    if (!outcome.release_attempted && (outcome.release_sent ||
                               !outcome.release_reason.empty() ||
                               !outcome.release_error.empty())) {
        set_error_noexcept(error_out,
                           "RELEASE evidence is attempted inconsistently");
        return false;
    }
    if (outcome.release_sent && !outcome.release_attempted) {
        set_error_noexcept(error_out, "sent RELEASE was not attempted");
        return false;
    }
    if (outcome.release_attempted && !outcome.ack_sent) {
        set_error_noexcept(error_out,
                           "RELEASE cannot be attempted after ACK write failure");
        return false;
    }
    const std::string expected_release_reason =
        admitted ? "source_detached" : "source_rejected";
    if (outcome.release_attempted &&
        outcome.release_reason != expected_release_reason) {
        set_error_noexcept(error_out,
                           "RELEASE reason does not match dispatch outcome");
        return false;
    }
    if (outcome.release_sent && !outcome.release_error.empty()) {
        set_error_noexcept(error_out, "sent RELEASE must not carry a write error");
        return false;
    }
    if (!outcome.release_sent && outcome.release_attempted &&
        outcome.release_error.empty()) {
        set_error_noexcept(error_out, "failed RELEASE write requires an error");
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::validate_encoder(
    const encoder::SpatialRoiLosslessFrameResult& result,
    std::string* error_out) const
{
    if (!ipc::validate_spatial_roi_ipc_correlation(result.correlation,
                                                   error_out)) {
        return false;
    }
    if (!same_stream(result.correlation.stream, expected_stream_)) {
        set_error_noexcept(error_out,
                           "encoder result stream identity does not match");
        return false;
    }
    const std::uint64_t index = result.correlation.roi_stream_frame_index;
    if (index == 0 || index > static_cast<std::uint64_t>(max_frames_)) {
        set_error_noexcept(error_out,
                           "encoder ROI frame index exceeds journal bounds");
        return false;
    }
    if (!bounded_printable(result.failure_reason,
                           ipc::kSpatialRoiIpcMaxTextBytes)) {
        set_error_noexcept(error_out, "encoder failure reason is invalid");
        return false;
    }
    if (result.status == encoder::SpatialRoiLosslessFrameResultStatus::Encoded) {
        // GOP cadence is validated by the encoder/evidence seam, where the
        // selected immutable profile is available.  The journal only joins
        // truthful per-frame evidence, so an interior GOP packet correctly
        // reporting keyframe=false remains valid here.
        if (!result.failure_reason.empty() || !result.nvenc_pts_assigned ||
            result.output_frame_index == 0 ||
            result.output_frame_index != index ||
            result.nvenc_pts == std::numeric_limits<std::uint64_t>::max() ||
            result.output_frame_index != result.nvenc_pts + 1U ||
            result.packet_count != 1 || result.encoded_bytes == 0) {
            set_error_noexcept(error_out, "encoded result evidence is invalid");
            return false;
        }
    } else if (result.status == encoder::SpatialRoiLosslessFrameResultStatus::Failed) {
        if (result.failure_reason.empty() || result.output_frame_index != 0 ||
            result.packet_count != 0 || result.encoded_bytes != 0 ||
            result.keyframe || (!result.nvenc_pts_assigned && result.nvenc_pts != 0)) {
            set_error_noexcept(error_out, "failed result evidence is invalid");
            return false;
        }
    } else {
        set_error_noexcept(error_out, "encoder result status is not recognized");
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::accept_transport_locked(
    const SpatialRoiRecorderFrameTransportOutcome& outcome,
    std::string* error_out)
{
    const auto& descriptor = outcome.descriptor;
    const std::string& detach_status = outcome.detach_status;
    const bool source_release_safe = outcome.source_release_safe;
    if (!same_stream(
            ipc::spatial_roi_ipc_stream_identity_from_descriptor(descriptor),
            expected_stream_)) {
        increment(&counters_.cross_stream_rejections);
    }
    std::string validation_error;
    if (!validate_transport(outcome, &validation_error)) {
        return reject_locked(validation_error.empty() ? "invalid transport outcome"
                                                       : validation_error,
                             error_out);
    }
    const std::uint64_t index = descriptor.roi_stream_frame_index;
    auto found = pending_.find(index);
    if (found != pending_.end() && found->second.transport) {
        TransportRecord candidate;
        candidate.descriptor = descriptor;
        candidate.detach_status = detach_status;
        candidate.source_release_safe = source_release_safe;
        candidate.dispatch_admitted = outcome.dispatch_admitted;
        candidate.dispatch_reason = outcome.dispatch_reason;
        candidate.ack_attempted = outcome.ack_attempted;
        candidate.ack_sent = outcome.ack_sent;
        candidate.ack_accepted = outcome.ack_accepted;
        candidate.ack_reason = outcome.ack_reason;
        candidate.ack_error = outcome.ack_error;
        candidate.release_attempted = outcome.release_attempted;
        candidate.release_sent = outcome.release_sent;
        candidate.release_reason = outcome.release_reason;
        candidate.release_error = outcome.release_error;
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked(
            same_transport(*found->second.transport, candidate)
                ? "duplicate transport outcome"
                : "conflicting transport outcome for frame index " +
                      std::to_string(index),
            error_out);
    }
    if (index < next_flush_index_) {
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked("duplicate transport outcome after flush",
                                    error_out);
    }
    if (!outcome.dispatch_admitted &&
        found != pending_.end() &&
        found->second.encoder_result) {
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked(
            "rejected dispatch unexpectedly has an encoder result", error_out);
    }
    if (found == pending_.end()) {
        if (pending_.size() >= max_pending_entries_) {
            increment(&counters_.pending_overflow_rejections);
            return latch_failure_locked(
                "pending journal entry bound exceeded while accepting transport "
                "outcome: index=" + std::to_string(index) +
                    " pending=" + std::to_string(pending_.size()) +
                    " limit=" + std::to_string(max_pending_entries_) +
                    " next_flush_index=" + std::to_string(next_flush_index_),
                error_out);
        }
        found = pending_.emplace(index, Entry{}).first;
        counters_.pending_entries_high_water = std::max<std::uint64_t>(
            counters_.pending_entries_high_water,
            static_cast<std::uint64_t>(pending_.size()));
    }
    TransportRecord record;
    record.descriptor = descriptor;
    record.detach_status = detach_status;
    record.source_release_safe = source_release_safe;
    record.dispatch_admitted = outcome.dispatch_admitted;
    record.dispatch_reason = outcome.dispatch_reason;
    record.ack_attempted = outcome.ack_attempted;
    record.ack_sent = outcome.ack_sent;
    record.ack_accepted = outcome.ack_accepted;
    record.ack_reason = outcome.ack_reason;
    record.ack_error = outcome.ack_error;
    record.release_attempted = outcome.release_attempted;
    record.release_sent = outcome.release_sent;
    record.release_reason = outcome.release_reason;
    record.release_error = outcome.release_error;
    found->second.transport = std::move(record);
    const TransportRecord& stored = *found->second.transport;
    increment(&counters_.transport_outcomes);
    if (stored.dispatch_admitted) {
        increment(&counters_.dispatch_admitted);
    } else {
        increment(&counters_.dispatch_rejected);
    }
    if (stored.ack_attempted) increment(&counters_.ack_attempted);
    if (stored.ack_sent) increment(&counters_.ack_sent);
    if (stored.ack_accepted) increment(&counters_.ack_accepted);
    if (stored.ack_attempted && !stored.ack_sent && !stored.ack_error.empty()) {
        increment(&counters_.ack_write_failures);
    }
    if (stored.release_attempted) increment(&counters_.release_attempted);
    if (stored.release_sent) increment(&counters_.release_sent);
    if (stored.release_attempted && !stored.release_sent &&
        !stored.release_error.empty()) {
        increment(&counters_.release_write_failures);
    }
    ++transport_count_;
    if (!stage_ready_locked(error_out)) {
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::accept_encoder_locked(
    const encoder::SpatialRoiLosslessFrameResult& result,
    std::string* error_out)
{
    if (!same_stream(result.correlation.stream, expected_stream_)) {
        increment(&counters_.cross_stream_rejections);
    }
    std::string validation_error;
    if (!validate_encoder(result, &validation_error)) {
        return reject_locked(validation_error.empty() ? "invalid encoder result"
                                                       : validation_error,
                             error_out);
    }
    const std::uint64_t index = result.correlation.roi_stream_frame_index;
    auto found = pending_.find(index);
    if (found != pending_.end() && found->second.encoder_result) {
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked(
            same_encoder_result(*found->second.encoder_result, result)
                ? "duplicate encoder result"
                : "conflicting encoder result for frame index " +
                      std::to_string(index),
            error_out);
    }
    if (index < next_flush_index_) {
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked("duplicate encoder result after flush",
                                    error_out);
    }
    if (found != pending_.end() && found->second.transport &&
        !found->second.transport->dispatch_admitted) {
        increment(&counters_.duplicate_or_conflict_rejections);
        return latch_failure_locked(
            "rejected dispatch unexpectedly has an encoder result", error_out);
    }
    if (found == pending_.end()) {
        if (pending_.size() >= max_pending_entries_) {
            increment(&counters_.pending_overflow_rejections);
            return latch_failure_locked(
                "pending journal entry bound exceeded while accepting encoder "
                "result: index=" + std::to_string(index) +
                    " pending=" + std::to_string(pending_.size()) +
                    " limit=" + std::to_string(max_pending_entries_) +
                    " next_flush_index=" + std::to_string(next_flush_index_),
                error_out);
        }
        found = pending_.emplace(index, Entry{}).first;
        counters_.pending_entries_high_water = std::max<std::uint64_t>(
            counters_.pending_entries_high_water,
            static_cast<std::uint64_t>(pending_.size()));
    }
    found->second.encoder_result = result;
    increment(&counters_.encoder_results);
    if (!stage_ready_locked(error_out)) {
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::append_entry(const Entry& entry,
                                                  std::string* error_out)
{
    if (!entry.transport || !writer_) {
        set_error_noexcept(error_out, "journal entry is not appendable");
        return false;
    }
    const TransportRecord& transport = *entry.transport;
    SpatialRoiRecorderFrameEvidence evidence;
    evidence.frame = transport.descriptor;
    evidence.detach_status = transport.detach_status;
    evidence.source_release_safe = transport.source_release_safe;
    evidence.dispatch_admitted = transport.dispatch_admitted;
    evidence.dispatch_reason = transport.dispatch_reason;
    evidence.ack_attempted = transport.ack_attempted;
    evidence.ack_sent = transport.ack_sent;
    evidence.ack_accepted = transport.ack_accepted;
    evidence.ack_reason = transport.ack_reason;
    evidence.ack_error = transport.ack_error;
    evidence.release_attempted = transport.release_attempted;
    evidence.release_sent = transport.release_sent;
    evidence.release_reason = transport.release_reason;
    evidence.release_error = transport.release_error;
    if (transport.dispatch_admitted && !entry.encoder_result) {
        set_error_noexcept(error_out,
                           "admitted journal entry is missing encoder result");
        return false;
    }
    if (entry.encoder_result) {
        const auto& result = *entry.encoder_result;
        evidence.encode_status =
            result.status == encoder::SpatialRoiLosslessFrameResultStatus::Encoded
                ? "encoded"
                : "failed";
        evidence.output_frame_index = result.output_frame_index;
        evidence.packet_count = result.packet_count;
        evidence.encoded_bytes = result.encoded_bytes;
        evidence.keyframe = result.keyframe;
    }
    std::string append_error;
    bool appended = false;
    try {
        appended = writer_->AppendFrame(evidence, &append_error);
    } catch (const std::exception& exception) {
        append_error = exception.what();
    } catch (...) {
        append_error = "evidence writer AppendFrame threw";
    }
    if (!appended) {
        set_error_noexcept(error_out,
                           append_error.empty() ? "evidence writer rejected frame"
                                                : append_error);
        return false;
    }
    if (error_out) error_out->clear();
    return true;
}

bool SpatialRoiRecorderFrameJournal::stage_ready_locked(std::string* error_out)
{
    if (next_flush_index_ == 0) {
        return true;
    }
    auto found = pending_.find(next_flush_index_);
    if (found == pending_.end() || !found->second.transport) {
        return true;
    }
    if (!found->second.transport->dispatch_admitted &&
        found->second.encoder_result) {
        return latch_failure_locked(
            "rejected dispatch unexpectedly has an encoder result",
            error_out);
    }
    if (found->second.transport->dispatch_admitted &&
        !found->second.encoder_result) {
        return true;
    }
    if (found->second.encoder_result) {
        const auto& transport = *found->second.transport;
        const auto& result = *found->second.encoder_result;
        std::string pair_error;
        if (!ipc::spatial_roi_ipc_correlation_matches_descriptor(
                result.correlation, transport.descriptor, &pair_error) ||
            !same_geometry(result.geometry, transport.descriptor) ||
            result.camera_timestamp_ns != transport.descriptor.camera_timestamp_ns ||
            result.timestamp_sys_ns != transport.descriptor.timestamp_sys_ns ||
            result.source_gpu_id != transport.descriptor.source_gpu_id ||
            result.assigned_gpu_id != transport.descriptor.assigned_gpu_id ||
            result.assigned_shard_id != transport.descriptor.assigned_shard_id) {
            return latch_failure_locked(
                pair_error.empty()
                    ? "encoder result geometry/timestamp/GPU conflicts with transport"
                    : pair_error,
                error_out);
        }
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::drain_ready(
    const std::size_t max_entries,
    std::string* error_out)
{
    std::size_t drained = 0;
    while (max_entries == 0 || drained < max_entries) {
        Entry entry;
        std::uint64_t index = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!valid_ || fatal_) {
                set_error_noexcept(error_out, first_failure_reason_);
                return false;
            }
            if (next_flush_index_ == 0) break;
            index = next_flush_index_;
            auto found = pending_.find(index);
            if (found == pending_.end() || !found->second.transport) break;
            if (!stage_ready_locked(error_out)) return false;
            if (found->second.transport->dispatch_admitted &&
                !found->second.encoder_result) break;
            entry = found->second;
        }

        std::string append_error;
        if (!append_entry(entry, &append_error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            increment(&counters_.append_failures);
            return latch_failure_locked(
                append_error.empty() ? "evidence writer rejected frame"
                                     : append_error,
                error_out);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto found = pending_.find(index);
        if (found == pending_.end()) {
            return latch_failure_locked("drained journal entry disappeared",
                                        error_out);
        }
        pending_.erase(found);
        increment(&counters_.frames_appended);
        ++appended_count_;
        if (next_flush_index_ == std::numeric_limits<std::uint64_t>::max()) {
            next_flush_index_ = 0;
        } else {
            ++next_flush_index_;
        }
        ++drained;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiRecorderFrameJournal::same_transport(
    const TransportRecord& lhs,
    const TransportRecord& rhs) noexcept
{
    return same_descriptor(lhs.descriptor, rhs.descriptor) &&
           lhs.detach_status == rhs.detach_status &&
           lhs.source_release_safe == rhs.source_release_safe &&
           lhs.dispatch_admitted == rhs.dispatch_admitted &&
           lhs.dispatch_reason == rhs.dispatch_reason &&
           lhs.ack_attempted == rhs.ack_attempted &&
           lhs.ack_sent == rhs.ack_sent &&
           lhs.ack_accepted == rhs.ack_accepted &&
           lhs.ack_reason == rhs.ack_reason && lhs.ack_error == rhs.ack_error &&
           lhs.release_attempted == rhs.release_attempted &&
           lhs.release_sent == rhs.release_sent &&
           lhs.release_reason == rhs.release_reason &&
           lhs.release_error == rhs.release_error;
}

bool SpatialRoiRecorderFrameJournal::same_encoder_result(
    const encoder::SpatialRoiLosslessFrameResult& lhs,
    const encoder::SpatialRoiLosslessFrameResult& rhs) noexcept
{
    return lhs.status == rhs.status &&
           same_correlation(lhs.correlation, rhs.correlation) &&
           lhs.geometry.native_raster.width == rhs.geometry.native_raster.width &&
           lhs.geometry.native_raster.height == rhs.geometry.native_raster.height &&
           same_rect(lhs.geometry.content_rect, rhs.geometry.content_rect) &&
           lhs.geometry.encoded_raster.width == rhs.geometry.encoded_raster.width &&
           lhs.geometry.encoded_raster.height == rhs.geometry.encoded_raster.height &&
           same_rect(lhs.geometry.encoded_content_rect,
                     rhs.geometry.encoded_content_rect) &&
           same_padding(lhs.geometry.padding, rhs.geometry.padding) &&
           lhs.geometry.routing_policy == rhs.geometry.routing_policy &&
           lhs.camera_timestamp_ns == rhs.camera_timestamp_ns &&
           lhs.timestamp_sys_ns == rhs.timestamp_sys_ns &&
           lhs.source_gpu_id == rhs.source_gpu_id &&
           lhs.assigned_gpu_id == rhs.assigned_gpu_id &&
           lhs.assigned_shard_id == rhs.assigned_shard_id &&
           lhs.output_frame_index == rhs.output_frame_index &&
           lhs.nvenc_pts_assigned == rhs.nvenc_pts_assigned &&
           lhs.nvenc_pts == rhs.nvenc_pts &&
           lhs.packet_count == rhs.packet_count &&
           lhs.encoded_bytes == rhs.encoded_bytes &&
           lhs.keyframe == rhs.keyframe &&
           lhs.failure_reason == rhs.failure_reason;
}
}  // namespace orange::spatial_roi::recording
