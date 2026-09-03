#include "spatial_roi_recording_runtime.h"

#include "session/spatial_roi_recording_config.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace orange::spatial_roi {

namespace {

const char* sink_result_name(SpatialRoiLaneSinkResult result) noexcept
{
    switch (result) {
    case SpatialRoiLaneSinkResult::kCompleted:
        return "completed";
    case SpatialRoiLaneSinkResult::kRejected:
        return "rejected";
    case SpatialRoiLaneSinkResult::kFailed:
        return "failed";
    }
    return "unknown";
}

const char* terminal_reason_name(SpatialRoiLaneTerminalReason reason) noexcept
{
    switch (reason) {
    case SpatialRoiLaneTerminalReason::kPending:
        return "pending";
    case SpatialRoiLaneTerminalReason::kCompleted:
        return "completed";
    case SpatialRoiLaneTerminalReason::kSinkRejected:
        return "sink_rejected";
    case SpatialRoiLaneTerminalReason::kSinkFailed:
        return "sink_failed";
    case SpatialRoiLaneTerminalReason::kSourceQuarantined:
        return "source_quarantined";
    case SpatialRoiLaneTerminalReason::kQueueFull:
        return "queue_full";
    case SpatialRoiLaneTerminalReason::kQueueAdmissionFailed:
        return "queue_admission_failed";
    case SpatialRoiLaneTerminalReason::kStopped:
        return "stopped";
    }
    return "unknown";
}

const char* runtime_status_name(SpatialRoiRuntimeSubmitStatus status) noexcept
{
    switch (status) {
    case SpatialRoiRuntimeSubmitStatus::kAccepted:
        return "accepted";
    case SpatialRoiRuntimeSubmitStatus::kIncomplete:
        return "incomplete";
    case SpatialRoiRuntimeSubmitStatus::kStopped:
        return "stopped";
    case SpatialRoiRuntimeSubmitStatus::kInvalidArgument:
        return "invalid_argument";
    case SpatialRoiRuntimeSubmitStatus::kPoolExhausted:
        return "pool_exhausted";
    case SpatialRoiRuntimeSubmitStatus::kCudaError:
        return "cuda_error";
    case SpatialRoiRuntimeSubmitStatus::kSourceQuarantined:
        return "source_quarantined";
    case SpatialRoiRuntimeSubmitStatus::kDuplicateOrOutOfOrder:
        return "duplicate_or_out_of_order";
    case SpatialRoiRuntimeSubmitStatus::kBusy:
        return "busy";
    }
    return "unknown";
}

const char* completion_status_name(
    SpatialRoiBatchCompletionStatus status) noexcept
{
    switch (status) {
    case SpatialRoiBatchCompletionStatus::kPending:
        return "pending";
    case SpatialRoiBatchCompletionStatus::kComplete:
        return "complete";
    case SpatialRoiBatchCompletionStatus::kIncomplete:
        return "incomplete";
    }
    return "unknown";
}

}  // namespace

const char* spatial_roi_lane_sink_result_name(
    SpatialRoiLaneSinkResult result) noexcept
{
    return sink_result_name(result);
}

const char* spatial_roi_lane_terminal_reason_name(
    SpatialRoiLaneTerminalReason reason) noexcept
{
    return terminal_reason_name(reason);
}

const char* spatial_roi_runtime_submit_status_name(
    SpatialRoiRuntimeSubmitStatus status) noexcept
{
    return runtime_status_name(status);
}

const char* spatial_roi_batch_completion_status_name(
    SpatialRoiBatchCompletionStatus status) noexcept
{
    return completion_status_name(status);
}

SpatialRoiBatchEnvelope::SpatialRoiBatchEnvelope(
    SpatialRoiBatchResult&& result,
    std::vector<SpatialRoiWorkItem>&& work_items,
    std::uint64_t batch_sequence)
    : result_(std::move(result)),
      work_items_(std::move(work_items)),
      batch_sequence_(batch_sequence),
      required_lane_count_(work_items_.size()),
      lane_reasons_(required_lane_count_,
                    SpatialRoiLaneTerminalReason::kPending)
{
}

SpatialRoiBatchEnvelope::~SpatialRoiBatchEnvelope() = default;

SpatialRoiBatchEnvelope::SpatialRoiBatchEnvelope(
    SpatialRoiBatchEnvelope&& other) noexcept
{
    std::lock_guard<std::mutex> lock(other.terminal_mutex_);
    result_ = std::move(other.result_);
    work_items_ = std::move(other.work_items_);
    batch_sequence_ = other.batch_sequence_;
    required_lane_count_ = other.required_lane_count_;
    lane_reasons_ = std::move(other.lane_reasons_);
    terminal_lane_count_ = other.terminal_lane_count_;
    completion_status_ = other.completion_status_;

    other.batch_sequence_ = 0;
    other.required_lane_count_ = 0;
    other.terminal_lane_count_ = 0;
    other.completion_status_ = SpatialRoiBatchCompletionStatus::kPending;
}

SpatialRoiBatchEnvelope& SpatialRoiBatchEnvelope::operator=(
    SpatialRoiBatchEnvelope&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(terminal_mutex_, other.terminal_mutex_);
    result_ = std::move(other.result_);
    work_items_ = std::move(other.work_items_);
    batch_sequence_ = other.batch_sequence_;
    required_lane_count_ = other.required_lane_count_;
    lane_reasons_ = std::move(other.lane_reasons_);
    terminal_lane_count_ = other.terminal_lane_count_;
    completion_status_ = other.completion_status_;

    other.batch_sequence_ = 0;
    other.required_lane_count_ = 0;
    other.terminal_lane_count_ = 0;
    other.completion_status_ = SpatialRoiBatchCompletionStatus::kPending;
    return *this;
}

bool SpatialRoiBatchEnvelope::mark_lane_terminal(
    std::size_t lane_index,
    SpatialRoiLaneTerminalReason reason) noexcept
{
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    if (lane_index >= lane_reasons_.size() ||
        lane_reasons_[lane_index] != SpatialRoiLaneTerminalReason::kPending) {
        return false;
    }
    lane_reasons_[lane_index] = reason;
    ++terminal_lane_count_;
    if (terminal_lane_count_ == required_lane_count_) {
        completion_status_ = SpatialRoiBatchCompletionStatus::kComplete;
        for (const SpatialRoiLaneTerminalReason terminal : lane_reasons_) {
            if (terminal != SpatialRoiLaneTerminalReason::kCompleted) {
                completion_status_ =
                    SpatialRoiBatchCompletionStatus::kIncomplete;
                break;
            }
        }
        return true;
    }
    return false;
}

SpatialRoiBatchTerminalSnapshot SpatialRoiBatchEnvelope::terminal_snapshot() const
{
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    SpatialRoiBatchTerminalSnapshot snapshot;
    snapshot.required_lane_count = required_lane_count_;
    snapshot.terminal_lane_count = terminal_lane_count_;
    snapshot.status = completion_status_;
    snapshot.lane_reasons = lane_reasons_;
    return snapshot;
}

bool SpatialRoiBatchEnvelope::strict_complete() const
{
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    return completion_status_ == SpatialRoiBatchCompletionStatus::kComplete;
}

struct SpatialRoiRecordingRuntime::AtomicCounters {
    std::atomic<std::uint64_t> submit_attempted{0};
    std::atomic<std::uint64_t> producer_accepted{0};
    std::atomic<std::uint64_t> producer_invalid_argument{0};
    std::atomic<std::uint64_t> producer_pool_exhausted{0};
    std::atomic<std::uint64_t> producer_cuda_error{0};
    std::atomic<std::uint64_t> producer_source_quarantined{0};
    std::atomic<std::uint64_t> producer_stopped{0};
    std::atomic<std::uint64_t> admission_busy{0};
    std::atomic<std::uint64_t> duplicate_or_out_of_order{0};
    std::atomic<std::uint64_t> lane_admitted{0};
    std::atomic<std::uint64_t> lane_queue_full{0};
    std::atomic<std::uint64_t> lane_queue_admission_failed{0};
    std::atomic<std::uint64_t> lane_stopped{0};
    std::atomic<std::uint64_t> lane_completed{0};
    std::atomic<std::uint64_t> lane_sink_rejected{0};
    std::atomic<std::uint64_t> lane_sink_failed{0};
    std::atomic<std::uint64_t> lane_source_quarantined{0};
    std::atomic<std::uint64_t> batches_complete{0};
    std::atomic<std::uint64_t> batches_incomplete{0};
    std::atomic<std::uint64_t> strict_incomplete_batches{0};
};

class SpatialRoiRecordingRuntime::Lane final {
public:
    enum class Admission {
        kAccepted,
        kQueueFull,
        kFailed,
        kStopped,
    };

    struct AdmissionResult {
        Admission status = Admission::kFailed;
        std::uint64_t roi_stream_frame_index = 0;
    };

    struct QueuedDelivery {
        std::shared_ptr<SpatialRoiBatchEnvelope> envelope;
        std::uint64_t roi_stream_frame_index = 0;
    };

    Lane(SpatialRoiRecordingRuntime* owner,
         std::size_t lane_index,
         int gpu_id,
         std::size_t queue_capacity,
         SpatialRoiLaneSink sink)
        : owner_(owner),
          lane_index_(lane_index),
          gpu_id_(gpu_id),
          queue_capacity_(queue_capacity),
          sink_(std::move(sink)),
          accepting_(true),
          worker_(&Lane::run, this),
          worker_id_(worker_.get_id())
    {
    }

    ~Lane()
    {
        StopAccepting();
        Join();
    }

    Lane(const Lane&) = delete;
    Lane& operator=(const Lane&) = delete;

    AdmissionResult TryEnqueue(
        const std::shared_ptr<SpatialRoiBatchEnvelope>& envelope)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return {Admission::kStopped, 0};
        }
        if (outstanding_ >= queue_capacity_) {
            return {Admission::kQueueFull, 0};
        }
        // A wrapped counter would violate the dense positive-index contract.
        // Treat the impossible terminal value as an admission failure without
        // changing the counter or queue state.
        if (next_roi_stream_frame_index_ == std::numeric_limits<std::uint64_t>::max()) {
            accepting_ = false;
            condition_.notify_all();
            return {Admission::kFailed, 0};
        }
        try {
            queue_.push_back(
                {envelope, next_roi_stream_frame_index_});
        } catch (...) {
            // A lane whose bounded queue cannot materialize its next node is
            // failed closed. Already admitted FIFO work still drains.
            accepting_ = false;
            condition_.notify_all();
            return {Admission::kFailed, 0};
        }
        const std::uint64_t admitted_index = next_roi_stream_frame_index_++;
        ++outstanding_;
        condition_.notify_one();
        return {Admission::kAccepted, admitted_index};
    }

    void StopAccepting() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
        }
        condition_.notify_all();
    }

    // StopAccepting is deliberately separate from cancellation: normal
    // shutdown preserves FIFO work that was already admitted.  After a sink
    // failure, however, the failed lane cannot safely invoke its sink again.
    // Move its queued deliveries out under the lane lock, decrementing the
    // outstanding count at the same linearization point.  The runtime marks
    // those envelopes terminal after this method returns, so no sink callback
    // runs while the lane lock is held.
    void StopAcceptingAndTakeQueued(
        std::deque<QueuedDelivery>* canceled) noexcept
    {
        if (!canceled) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            canceled->swap(queue_);
            const std::size_t canceled_count = canceled->size();
            if (canceled_count >= outstanding_) {
                outstanding_ = 0;
            } else {
                outstanding_ -= canceled_count;
            }
        }
        condition_.notify_all();
    }

    void Join() noexcept
    {
        if (worker_id_ == std::this_thread::get_id()) {
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool IsCurrentThread() const noexcept
    {
        return worker_id_ == std::this_thread::get_id();
    }

private:
    void run() noexcept
    {
        for (;;) {
            QueuedDelivery queued_delivery;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return !accepting_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    // StopAccepting preserves FIFO work already admitted. The
                    // worker exits only after that work has been handled.
                    if (!accepting_) {
                        return;
                    }
                    continue;
                }
                queued_delivery = std::move(queue_.front());
                queue_.pop_front();
            }

            std::shared_ptr<SpatialRoiBatchEnvelope>& envelope =
                queued_delivery.envelope;

            SpatialRoiLaneTerminalReason terminal =
                SpatialRoiLaneTerminalReason::kSinkFailed;
            try {
                if (cudaSetDevice(gpu_id_) != cudaSuccess ||
                    cudaEventSynchronize(
                        envelope->result().completion_event()) != cudaSuccess) {
                    owner_->QuarantineAfterCudaCompletionFailure();
                    terminal =
                        SpatialRoiLaneTerminalReason::kSourceQuarantined;
                } else {
                    const SpatialRoiLaneSinkResult result =
                        sink_ ? sink_(SpatialRoiLaneDelivery{
                                         lane_index_,
                                         queued_delivery.roi_stream_frame_index,
                                         envelope})
                              : SpatialRoiLaneSinkResult::kFailed;
                    switch (result) {
                    case SpatialRoiLaneSinkResult::kCompleted:
                        terminal = SpatialRoiLaneTerminalReason::kCompleted;
                        break;
                    case SpatialRoiLaneSinkResult::kRejected:
                        terminal = SpatialRoiLaneTerminalReason::kSinkRejected;
                        break;
                    case SpatialRoiLaneSinkResult::kFailed:
                        terminal = SpatialRoiLaneTerminalReason::kSinkFailed;
                        break;
                    default:
                        terminal = SpatialRoiLaneTerminalReason::kSinkFailed;
                        break;
                    }
                }
            } catch (...) {
                terminal = SpatialRoiLaneTerminalReason::kSinkFailed;
            }
            if (terminal == SpatialRoiLaneTerminalReason::kSinkRejected ||
                terminal == SpatialRoiLaneTerminalReason::kSinkFailed) {
                // This stops admission before the next source frame can be
                // accepted, then cancels only this lane's queued work. The
                // fail-fast request is published before the stop lock, and
                // the active sink reason is latched before canceled entries
                // can attempt to record their stopped terminal state.
                owner_->fail_fast_after_sink_failure(lane_index_, terminal);
            }
            owner_->mark_lane_terminal(envelope, lane_index_, terminal);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (outstanding_ > 0) {
                    --outstanding_;
                }
            }
        }
    }

    SpatialRoiRecordingRuntime* owner_ = nullptr;
    std::size_t lane_index_ = 0;
    int gpu_id_ = -1;
    std::size_t queue_capacity_ = 0;
    SpatialRoiLaneSink sink_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedDelivery> queue_;
    std::size_t outstanding_ = 0;
    std::uint64_t next_roi_stream_frame_index_ = 1;
    bool accepting_ = false;
    std::thread worker_;
    const std::thread::id worker_id_;
};

SpatialRoiRecordingRuntime::SpatialRoiRecordingRuntime(
    const nlohmann::json& verified_plan,
    std::string camera_serial,
    int gpu_id,
    SpatialRoiLaneSink sink)
    : counters_(std::make_unique<AtomicCounters>())
{
    session::spatial_roi::SpatialRoiRecordingPlan parsed_plan;
    std::string error;
    if (!session::spatial_roi::parse_verified_plan(
            verified_plan, &parsed_plan, &error)) {
        throw std::invalid_argument(
            "Cannot construct spatial ROI runtime from unverified plan: " +
            error);
    }

    const auto camera_it = parsed_plan.cameras.find(camera_serial);
    if (camera_it == parsed_plan.cameras.end()) {
        throw std::invalid_argument(
            "Verified spatial ROI plan has no camera " + camera_serial);
    }
    if (camera_it->second.rois.empty()) {
        throw std::invalid_argument(
            "Verified spatial ROI plan camera has no ROI lanes: " +
            camera_serial);
    }

    // queue_frames_per_stream is part of the verified, normalized plan
    // configuration but is intentionally not copied into the producer limits.
    // Read it only after parse_verified_plan has authenticated the envelope.
    try {
        const nlohmann::json& buffering =
            verified_plan.at("plan").at("configuration").at("buffering");
        const std::uint64_t queue_capacity =
            buffering.at("queue_frames_per_stream").get<std::uint64_t>();
        if (queue_capacity == 0 ||
            queue_capacity > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument(
                "verified spatial ROI queue_frames_per_stream is invalid");
        }
        lane_queue_capacity_ = static_cast<std::size_t>(queue_capacity);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& exception) {
        throw std::invalid_argument(
            std::string("verified spatial ROI queue configuration is invalid: ") +
            exception.what());
    }

    SpatialRoiBatchLimits limits;
    if (!spatial_roi_batch_limits_from_verified_plan(
            verified_plan, camera_serial, gpu_id, &limits, &error)) {
        throw std::invalid_argument(
            "Cannot materialize spatial ROI producer limits: " + error);
    }
    if (limits.expected_roi_descriptors.size() != camera_it->second.rois.size()) {
        throw std::invalid_argument(
            "Spatial ROI producer limits do not match verified camera ROI count");
    }

    producer_ = std::make_unique<SpatialRoiBatchProducer>(limits);
    lanes_.reserve(camera_it->second.rois.size());
    for (std::size_t lane_index = 0;
         lane_index < camera_it->second.rois.size();
         ++lane_index) {
        lanes_.push_back(std::make_unique<Lane>(
            this, lane_index, gpu_id, lane_queue_capacity_, sink));
    }
    accepting_ = true;
}

SpatialRoiRecordingRuntime::SpatialRoiRecordingRuntime(
    const nlohmann::json& verified_plan,
    std::string camera_serial,
    int gpu_id,
    SpatialRoiLegacyLaneSink sink)
    : SpatialRoiRecordingRuntime(
          verified_plan,
          std::move(camera_serial),
          gpu_id,
          SpatialRoiLaneSink(
              [legacy_sink = std::move(sink)](
                  const SpatialRoiLaneDelivery& delivery) {
                  return legacy_sink
                             ? legacy_sink(delivery.lane_index,
                                           delivery.envelope)
                             : SpatialRoiLaneSinkResult::kFailed;
              }))
{
}

SpatialRoiRecordingRuntime::~SpatialRoiRecordingRuntime()
{
    StopAcceptingAndDrain();
    // Keep this explicit: all lane callbacks and queued envelopes are gone
    // before the producer's CUDA stream/pool owner is released. A sink may
    // still retain an envelope; its result-level shared state keeps the CUDA
    // pool alive until that external reference is released.
    producer_.reset();
}

SpatialRoiWorkItem SpatialRoiRecordingRuntime::make_work_item(
    const SpatialRoiPlanRoiBinding& descriptor,
    const SpatialRoiFrameIdentity& identity,
    const std::string& plan_sha256)
{
    SpatialRoiWorkItem item;
    item.roi_id = descriptor.roi_id;
    item.region_id = descriptor.region_id;
    item.arena_group_id = descriptor.arena_group_id;
    item.arena_id = descriptor.arena_id;
    item.logical_stream_id = descriptor.logical_stream_id;
    item.spatial_roi_plan_sha256 = plan_sha256;
    item.source = identity;
    item.geometry.content_rect = {
        descriptor.source_rect.x,
        descriptor.source_rect.y,
        descriptor.source_rect.width,
        descriptor.source_rect.height,
    };
    item.geometry.encoded_raster = {
        descriptor.encoded_raster.width,
        descriptor.encoded_raster.height,
    };
    item.geometry.encoded_content_rect = {
        descriptor.encoded_content_rect.x,
        descriptor.encoded_content_rect.y,
        descriptor.encoded_content_rect.width,
        descriptor.encoded_content_rect.height,
    };
    return item;
}

SpatialRoiRuntimeSubmitStatus SpatialRoiRecordingRuntime::map_producer_status(
    SpatialRoiBatchStatus status) noexcept
{
    switch (status) {
    case SpatialRoiBatchStatus::kAccepted:
        return SpatialRoiRuntimeSubmitStatus::kAccepted;
    case SpatialRoiBatchStatus::kInvalidArgument:
        return SpatialRoiRuntimeSubmitStatus::kInvalidArgument;
    case SpatialRoiBatchStatus::kPoolExhausted:
        return SpatialRoiRuntimeSubmitStatus::kPoolExhausted;
    case SpatialRoiBatchStatus::kCudaError:
        return SpatialRoiRuntimeSubmitStatus::kCudaError;
    case SpatialRoiBatchStatus::kSourceQuarantined:
        return SpatialRoiRuntimeSubmitStatus::kSourceQuarantined;
    case SpatialRoiBatchStatus::kStopped:
        return SpatialRoiRuntimeSubmitStatus::kStopped;
    }
    return SpatialRoiRuntimeSubmitStatus::kCudaError;
}

SpatialRoiBatchStatus SpatialRoiRecordingRuntime::map_runtime_terminal_status(
    SpatialRoiRuntimeSubmitStatus status) noexcept
{
    switch (status) {
    case SpatialRoiRuntimeSubmitStatus::kCudaError:
        return SpatialRoiBatchStatus::kCudaError;
    case SpatialRoiRuntimeSubmitStatus::kSourceQuarantined:
        return SpatialRoiBatchStatus::kSourceQuarantined;
    case SpatialRoiRuntimeSubmitStatus::kStopped:
    case SpatialRoiRuntimeSubmitStatus::kAccepted:
    case SpatialRoiRuntimeSubmitStatus::kIncomplete:
    case SpatialRoiRuntimeSubmitStatus::kInvalidArgument:
    case SpatialRoiRuntimeSubmitStatus::kPoolExhausted:
    case SpatialRoiRuntimeSubmitStatus::kDuplicateOrOutOfOrder:
    case SpatialRoiRuntimeSubmitStatus::kBusy:
        return SpatialRoiBatchStatus::kStopped;
    }
    return SpatialRoiBatchStatus::kStopped;
}

SpatialRoiBatchSubmission SpatialRoiRecordingRuntime::TrySubmit(
    const SpatialRoiSourceView& source)
{
    SpatialRoiBatchSubmission submission;
    counters_->submit_attempted.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> admission_lock(
        admission_mutex_, std::try_to_lock);
    if (!admission_lock.owns_lock()) {
        submission.status = SpatialRoiRuntimeSubmitStatus::kBusy;
        submission.producer_status = SpatialRoiBatchStatus::kStopped;
        counters_->admission_busy.fetch_add(1, std::memory_order_relaxed);
        return submission;
    }

    // A lane publishes its sink-failure stop request before it acquires
    // admission_mutex_. Observe that request here as well, closing the race
    // in which a submitter could otherwise pass accepting_ before the failing
    // lane gets to stop the producer and all lane queues.
    if (sink_failure_stop_requested_.load(std::memory_order_acquire) &&
        accepting_) {
        accepting_ = false;
        terminal_status_ = SpatialRoiRuntimeSubmitStatus::kStopped;
        if (producer_) {
            producer_->StopAccepting();
        }
        for (const auto& lane : lanes_) {
            lane->StopAccepting();
        }
    }

    if (!accepting_) {
        submission.status = terminal_status_;
        submission.producer_status =
            map_runtime_terminal_status(terminal_status_);
        if (terminal_status_ == SpatialRoiRuntimeSubmitStatus::kCudaError) {
            counters_->producer_cuda_error.fetch_add(
                1, std::memory_order_relaxed);
        } else if (terminal_status_ ==
                   SpatialRoiRuntimeSubmitStatus::kSourceQuarantined) {
            counters_->producer_source_quarantined.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            counters_->producer_stopped.fetch_add(
                1, std::memory_order_relaxed);
        }
        return submission;
    }

    if (source.identity.recording_frame_id != 0 &&
        last_admitted_recording_frame_id_ != 0 &&
        source.identity.recording_frame_id <=
            last_admitted_recording_frame_id_) {
        submission.status =
            SpatialRoiRuntimeSubmitStatus::kDuplicateOrOutOfOrder;
        submission.producer_status = SpatialRoiBatchStatus::kInvalidArgument;
        counters_->duplicate_or_out_of_order.fetch_add(
            1, std::memory_order_relaxed);
        return submission;
    }

    // Materialize all caller-visible variable-size state before producer or
    // lane admission. Once the first lane accepts this frame, the remainder
    // of TrySubmit must not throw and make the acquisition bridge report an
    // exception for work that is already in flight.
    submission.lane_count = lanes_.size();
    submission.lane_reasons.assign(
        submission.lane_count, SpatialRoiLaneTerminalReason::kPending);

    const SpatialRoiBatchLimits& limits = producer_->limits();
    std::vector<SpatialRoiWorkItem> work_items;
    work_items.reserve(limits.expected_roi_descriptors.size());
    for (const SpatialRoiPlanRoiBinding& descriptor :
         limits.expected_roi_descriptors) {
        work_items.push_back(make_work_item(
            descriptor, source.identity, limits.expected_spatial_roi_plan_sha256));
    }

    SpatialRoiBatchResult result = producer_->TryProduce(source, work_items);
    submission.producer_status = result.status();
    if (!result.accepted()) {
        submission.status = map_producer_status(result.status());
        if (result.status() == SpatialRoiBatchStatus::kCudaError ||
            result.status() == SpatialRoiBatchStatus::kSourceQuarantined ||
            result.status() == SpatialRoiBatchStatus::kStopped) {
            accepting_ = false;
            terminal_status_ = submission.status;
        }
        switch (result.status()) {
        case SpatialRoiBatchStatus::kInvalidArgument:
            counters_->producer_invalid_argument.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case SpatialRoiBatchStatus::kPoolExhausted:
            counters_->producer_pool_exhausted.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case SpatialRoiBatchStatus::kCudaError:
            counters_->producer_cuda_error.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case SpatialRoiBatchStatus::kSourceQuarantined:
            counters_->producer_source_quarantined.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case SpatialRoiBatchStatus::kStopped:
            counters_->producer_stopped.fetch_add(1, std::memory_order_relaxed);
            break;
        case SpatialRoiBatchStatus::kAccepted:
            break;
        }
        return submission;
    }

    counters_->producer_accepted.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t batch_sequence = next_batch_sequence_++;
    // The envelope constructor is intentionally private so no caller can
    // manufacture an envelope without an accepted producer result. Use a
    // direct allocation here (inside the owning runtime) instead of
    // make_shared, whose allocator is not a friend of the envelope class.
    std::shared_ptr<SpatialRoiBatchEnvelope> envelope(new SpatialRoiBatchEnvelope(
        std::move(result), std::move(work_items), batch_sequence));
    submission.batch_sequence = batch_sequence;
    submission.envelope = envelope;
    last_admitted_recording_frame_id_ = source.identity.recording_frame_id;

    bool fatal_lane_admission_failure = false;
    for (std::size_t lane_index = 0; lane_index < lanes_.size(); ++lane_index) {
        Lane::AdmissionResult admission;
        try {
            admission = lanes_[lane_index]->TryEnqueue(envelope);
        } catch (...) {
            admission = {Lane::Admission::kFailed, 0};
        }
        if (admission.status == Lane::Admission::kAccepted) {
            ++submission.admitted_lane_count;
            counters_->lane_admitted.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        SpatialRoiLaneTerminalReason reason =
            SpatialRoiLaneTerminalReason::kStopped;
        if (admission.status == Lane::Admission::kQueueFull) {
            reason = SpatialRoiLaneTerminalReason::kQueueFull;
            counters_->lane_queue_full.fetch_add(1, std::memory_order_relaxed);
        } else if (admission.status == Lane::Admission::kFailed) {
            reason = SpatialRoiLaneTerminalReason::kQueueAdmissionFailed;
            counters_->lane_queue_admission_failed.fetch_add(
                1, std::memory_order_relaxed);
            fatal_lane_admission_failure = true;
        } else {
            counters_->lane_stopped.fetch_add(1, std::memory_order_relaxed);
            fatal_lane_admission_failure = true;
        }
        submission.lane_reasons[lane_index] = reason;
        mark_lane_terminal(envelope, lane_index, reason);
    }

    if (fatal_lane_admission_failure) {
        accepting_ = false;
        terminal_status_ = SpatialRoiRuntimeSubmitStatus::kStopped;
        producer_->StopAccepting();
        for (const auto& lane : lanes_) {
            lane->StopAccepting();
        }
    }

    const bool all_lanes_admitted =
        submission.admitted_lane_count == submission.lane_count;
    submission.status = all_lanes_admitted
                            ? SpatialRoiRuntimeSubmitStatus::kAccepted
                            : SpatialRoiRuntimeSubmitStatus::kIncomplete;
    return submission;
}

void SpatialRoiRecordingRuntime::mark_lane_terminal(
    const std::shared_ptr<SpatialRoiBatchEnvelope>& envelope,
    std::size_t lane_index,
    SpatialRoiLaneTerminalReason reason) noexcept
{
    if (reason != SpatialRoiLaneTerminalReason::kCompleted &&
        reason != SpatialRoiLaneTerminalReason::kPending) {
        latch_failure(terminal_reason_name(reason));
    }
    switch (reason) {
    case SpatialRoiLaneTerminalReason::kCompleted:
        counters_->lane_completed.fetch_add(1, std::memory_order_relaxed);
        break;
    case SpatialRoiLaneTerminalReason::kSinkRejected:
        counters_->lane_sink_rejected.fetch_add(1, std::memory_order_relaxed);
        break;
    case SpatialRoiLaneTerminalReason::kSinkFailed:
        counters_->lane_sink_failed.fetch_add(1, std::memory_order_relaxed);
        break;
    case SpatialRoiLaneTerminalReason::kSourceQuarantined:
        counters_->lane_source_quarantined.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case SpatialRoiLaneTerminalReason::kQueueFull:
    case SpatialRoiLaneTerminalReason::kQueueAdmissionFailed:
    case SpatialRoiLaneTerminalReason::kPending:
        break;
    case SpatialRoiLaneTerminalReason::kStopped:
        counters_->lane_stopped.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    if (envelope->mark_lane_terminal(lane_index, reason)) {
        if (envelope->strict_complete()) {
            counters_->batches_complete.fetch_add(1, std::memory_order_relaxed);
        } else {
            counters_->batches_incomplete.fetch_add(
                1, std::memory_order_relaxed);
            counters_->strict_incomplete_batches.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void SpatialRoiRecordingRuntime::fail_fast_after_sink_failure(
    std::size_t failed_lane_index,
    SpatialRoiLaneTerminalReason reason) noexcept
{
    // This store is the asynchronous stop linearization point. A TrySubmit
    // already inside admission is concurrent with the failing sink; every
    // later caller observes the request after taking admission_mutex_.
    sink_failure_stop_requested_.store(true, std::memory_order_release);
    latch_failure(terminal_reason_name(reason));
    // Linearize the stop against TrySubmit first.  A sink callback runs on a
    // lane thread and never owns admission_mutex_, so this is safe even when
    // the sink re-enters StopAcceptingAndDrain().
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        const bool was_accepting = accepting_;
        accepting_ = false;
        if (was_accepting) {
            terminal_status_ = SpatialRoiRuntimeSubmitStatus::kStopped;
        }
        if (producer_) {
            producer_->StopAccepting();
        }
        for (const auto& lane : lanes_) {
            lane->StopAccepting();
        }
    }

    if (failed_lane_index >= lanes_.size()) {
        return;
    }

    // Healthy lanes retain their admitted queues and therefore continue to
    // drain FIFO.  Only deliveries that have not yet been dequeued from the
    // failed lane are terminalized as stopped and released here.
    std::deque<Lane::QueuedDelivery> canceled;
    lanes_[failed_lane_index]->StopAcceptingAndTakeQueued(&canceled);
    for (auto& delivery : canceled) {
        mark_lane_terminal(
            delivery.envelope,
            failed_lane_index,
            SpatialRoiLaneTerminalReason::kStopped);
    }
}

void SpatialRoiRecordingRuntime::latch_failure(const char* reason) noexcept
{
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (failed_) {
        return;
    }
    failed_ = true;
    try {
        first_failure_ = (reason && *reason) ? reason : "spatial ROI lane failed";
    } catch (...) {
        // The boolean remains authoritative if explanatory allocation fails.
    }
}

void SpatialRoiRecordingRuntime::QuarantineAfterCudaCompletionFailure() noexcept
{
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    accepting_ = false;
    terminal_status_ = SpatialRoiRuntimeSubmitStatus::kSourceQuarantined;
    if (producer_) {
        producer_->Quarantine();
    }
}

void SpatialRoiRecordingRuntime::StopAccepting() noexcept
{
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (accepting_) {
        terminal_status_ = SpatialRoiRuntimeSubmitStatus::kStopped;
    }
    accepting_ = false;
    if (producer_) {
        producer_->StopAccepting();
    }
    for (const auto& lane : lanes_) {
        lane->StopAccepting();
    }
}

void SpatialRoiRecordingRuntime::Drain() noexcept
{
    // A recorder sink runs on a lane thread. Re-entrant drain from that
    // callback must not self-join (or deadlock another callback doing the
    // same). It is safely reduced to stop-only; the owner must perform the
    // final drain from a non-lane thread after the callback returns.
    for (const auto& lane : lanes_) {
        if (lane->IsCurrentThread()) {
            StopAccepting();
            return;
        }
    }
    std::lock_guard<std::mutex> drain_lock(drain_mutex_);
    StopAccepting();
    for (const auto& lane : lanes_) {
        lane->Join();
    }
}

bool SpatialRoiRecordingRuntime::StopAcceptingAndDrain(
    std::string* error_out,
    bool* fully_drained_out) noexcept
{
    if (fully_drained_out) {
        *fully_drained_out = false;
    }
    for (const auto& lane : lanes_) {
        if (lane->IsCurrentThread()) {
            StopAccepting();
            if (error_out) {
                try {
                    *error_out =
                        "runtime drain was requested from a lane callback; "
                        "only stop admission completed";
                } catch (...) {
                    error_out->clear();
                }
            }
            return false;
        }
    }
    Drain();
    if (fully_drained_out) {
        *fully_drained_out = true;
    }
    const bool success = !failed();
    if (!success && error_out) {
        try {
            *error_out = failure_reason();
        } catch (...) {
            error_out->clear();
        }
    } else if (error_out) {
        try {
            error_out->clear();
        } catch (...) {
        }
    }
    return success;
}

bool SpatialRoiRecordingRuntime::accepting() const noexcept
{
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    return accepting_;
}

bool SpatialRoiRecordingRuntime::failed() const noexcept
{
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return failed_;
}

std::string SpatialRoiRecordingRuntime::failure_reason() const
{
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return first_failure_;
}

SpatialRoiRecordingRuntimeCounters SpatialRoiRecordingRuntime::counters() const
    noexcept
{
    SpatialRoiRecordingRuntimeCounters snapshot;
    snapshot.submit_attempted =
        counters_->submit_attempted.load(std::memory_order_relaxed);
    snapshot.producer_accepted =
        counters_->producer_accepted.load(std::memory_order_relaxed);
    snapshot.producer_invalid_argument =
        counters_->producer_invalid_argument.load(std::memory_order_relaxed);
    snapshot.producer_pool_exhausted =
        counters_->producer_pool_exhausted.load(std::memory_order_relaxed);
    snapshot.producer_cuda_error =
        counters_->producer_cuda_error.load(std::memory_order_relaxed);
    snapshot.producer_source_quarantined =
        counters_->producer_source_quarantined.load(std::memory_order_relaxed);
    snapshot.producer_stopped =
        counters_->producer_stopped.load(std::memory_order_relaxed);
    snapshot.admission_busy =
        counters_->admission_busy.load(std::memory_order_relaxed);
    snapshot.duplicate_or_out_of_order =
        counters_->duplicate_or_out_of_order.load(std::memory_order_relaxed);
    snapshot.lane_admitted =
        counters_->lane_admitted.load(std::memory_order_relaxed);
    snapshot.lane_queue_full =
        counters_->lane_queue_full.load(std::memory_order_relaxed);
    snapshot.lane_queue_admission_failed =
        counters_->lane_queue_admission_failed.load(
            std::memory_order_relaxed);
    snapshot.lane_stopped =
        counters_->lane_stopped.load(std::memory_order_relaxed);
    snapshot.lane_completed =
        counters_->lane_completed.load(std::memory_order_relaxed);
    snapshot.lane_sink_rejected =
        counters_->lane_sink_rejected.load(std::memory_order_relaxed);
    snapshot.lane_sink_failed =
        counters_->lane_sink_failed.load(std::memory_order_relaxed);
    snapshot.lane_source_quarantined =
        counters_->lane_source_quarantined.load(std::memory_order_relaxed);
    snapshot.batches_complete =
        counters_->batches_complete.load(std::memory_order_relaxed);
    snapshot.batches_incomplete =
        counters_->batches_incomplete.load(std::memory_order_relaxed);
    snapshot.strict_incomplete_batches =
        counters_->strict_incomplete_batches.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace orange::spatial_roi
