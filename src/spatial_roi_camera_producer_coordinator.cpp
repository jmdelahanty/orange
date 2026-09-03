#include "spatial_roi_camera_producer_coordinator.h"

#include "spatial_roi_ipc_exporter.h"
#include "spatial_roi_ipc_handoff.h"
#include "spatial_roi_unix_socket_connector.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace orange::spatial_roi {
namespace {

constexpr std::size_t kRequiredStreamCount = 4;
constexpr auto kMaximumWait = std::chrono::minutes(5);
constexpr std::size_t kMaximumReasonBytes = 1024;

bool set_error(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

std::string bounded_reason(const std::string& value, const char* fallback)
{
    std::string result = value;
    if (result.empty()) {
        result = fallback ? fallback : "operation failed";
    }
    if (result.size() > kMaximumReasonBytes) {
        result.resize(kMaximumReasonBytes);
    }
    return result;
}

std::string exception_reason(const char* operation,
                             const std::exception& exception)
{
    return bounded_reason(std::string(operation ? operation : "operation") +
                              " threw: " + exception.what(),
                          "operation threw");
}

std::string unknown_exception_reason(const char* operation)
{
    return std::string(operation ? operation : "operation") +
           " threw an unknown exception";
}

ipc::SpatialRoiIpcStreamIdentity stream_identity(
    const session::spatial_roi::SpatialRoiRecorderStreamView& stream)
{
    ipc::SpatialRoiIpcStreamIdentity identity;
    identity.recording_id = stream.recording_id;
    identity.recording_identity_token = stream.recording_identity_token;
    identity.producer_generation = stream.producer_generation;
    identity.camera_id = stream.camera_id;
    identity.camera_serial = stream.camera_serial;
    identity.roi_id = stream.roi_id;
    identity.region_id = stream.region_id;
    identity.arena_group_id = stream.arena_group_id;
    identity.arena_id = stream.has_arena_id ? stream.arena_id : "";
    identity.logical_stream_id = stream.logical_stream_id;
    identity.spatial_roi_plan_sha256 = stream.spatial_roi_plan_sha256;
    return identity;
}

class ConcreteProducerStream final : public SpatialRoiCameraProducerStream {
public:
    ConcreteProducerStream(
        const nlohmann::json& verified_plan,
        session::spatial_roi::SpatialRoiRecorderStreamView stream,
        int producer_gpu_id,
        pid_t expected_recorder_pid,
        uid_t expected_recorder_uid,
        std::chrono::milliseconds connect_timeout,
        std::chrono::milliseconds write_timeout,
        std::chrono::milliseconds response_timeout)
        : verified_plan_(verified_plan),
          stream_(std::move(stream)),
          producer_gpu_id_(producer_gpu_id),
          expected_recorder_pid_(expected_recorder_pid),
          expected_recorder_uid_(expected_recorder_uid),
          connect_timeout_(connect_timeout),
          write_timeout_(write_timeout),
          response_timeout_(response_timeout),
          exporter_(verified_plan_, stream_.camera_serial, producer_gpu_id_)
    {
    }

    bool Start(std::string* error_out) override
    {
        if (error_out) {
            error_out->clear();
        }
        if (started_) {
            return true;
        }
        if (failed_) {
            return set_error(error_out,
                             bounded_reason(first_failure_,
                                            "producer stream is failed"));
        }
        if (!exporter_.valid()) {
            return fail(error_out,
                        "spatial ROI producer exporter is invalid: " +
                            exporter_.error());
        }
        if (stream_.expected_shard_gpu_ids.size() != 1 ||
            stream_.expected_shard_gpu_ids.front() != stream_.recorder_gpu_id) {
            return fail(error_out,
                        "producer stream has no exact single-shard GPU binding");
        }

        ipc::SpatialRoiUnixSocketConnectorConfig connector_config;
        connector_config.socket_path = stream_.socket_path;
        connector_config.connect_timeout = connect_timeout_;
        connector_config.transport_config.write_timeout = write_timeout_;
        connector_config.transport_config.expected_peer_pid =
            expected_recorder_pid_;
        connector_config.transport_config.expected_peer_uid =
            expected_recorder_uid_;

        std::string error;
        transport_ = ipc::SpatialRoiUnixSocketConnector::Connect(
            connector_config, &error);
        if (!transport_) {
            return fail(error_out,
                        "producer stream connector failed: " +
                            bounded_reason(error, "connector failed"));
        }

        ipc::SpatialRoiIpcHandoffConfig handoff_config;
        handoff_config.expected_stream = stream_identity(stream_);
        handoff_config.max_outstanding_frames = stream_.encode_queue_depth;
        handoff_config.response_timeout = response_timeout_;
        handoff_ = std::make_unique<ipc::SpatialRoiIpcHandoff>(
            exporter_, *transport_, std::move(handoff_config));
        if (!handoff_->valid()) {
            return fail(error_out,
                        "producer stream handoff is invalid: " +
                            bounded_reason(handoff_->error(),
                                           "handoff construction failed"));
        }
        if (!handoff_->Negotiate(&error)) {
            failed_ = true;
            first_failure_ = bounded_reason(
                error, "producer stream HELLO negotiation failed");
            return fail(error_out, first_failure_);
        }
        started_ = true;
        return true;
    }

    SpatialRoiLaneSinkResult Submit(
        const SpatialRoiLaneDelivery& delivery) override
    {
        if (!started_ || !handoff_) {
            return SpatialRoiLaneSinkResult::kFailed;
        }
        try {
            const ipc::SpatialRoiIpcHandoffResult result = handoff_->Submit(
                delivery, stream_.recorder_gpu_id, 0);
            if (result.completed()) {
                return SpatialRoiLaneSinkResult::kCompleted;
            }
            if (result.rejected()) {
                return SpatialRoiLaneSinkResult::kRejected;
            }
        } catch (...) {
            // The runtime turns callback exceptions into a terminal lane
            // failure. No reconnect or retransmit is permitted here.  A
            // transport exception may have happened after FRAME admission,
            // so retain ownership until the recorder is proven gone.
        }
        return SpatialRoiLaneSinkResult::kFailed;
    }

    void Stop() noexcept override
    {
        started_ = false;
        // Closing the transport is required for recorder EOF and must happen
        // before the recorder process is waited on.  The handoff, exporter,
        // and closed transport object remain alive: a fatal handoff may still
        // retain producer CUDA ownership until the child is definitively
        // reaped.  ReleaseProducerCudaResourcesAfterRecorderReaped() performs
        // the later confirmation and object teardown.
        if (transport_) {
            transport_->Close();
        }
    }

    bool ReleaseProducerCudaResourcesAfterRecorderReaped(
        std::string* error_out) noexcept override
    {
        if (error_out) {
            try {
                error_out->clear();
            } catch (...) {
            }
        }
        if (producer_resources_released_) {
            return true;
        }

        // Be robust to a caller that reaches this boundary without first
        // stopping admission.  Closing an already-closed transport is
        // idempotent, and no further Submit is permitted after this method.
        Stop();

        try {
            if (handoff_) {
                // ConfirmPeerExited() is intentionally not called on healthy
                // handoffs: it requires a fatal latch.  A fatal handoff is the
                // only path that can retain an export after synchronous
                // Submit returns, and confirmation is the sole operation
                // allowed to clear that ownership.
                if (handoff_->fatal_latched() &&
                    !handoff_->peer_exited_confirmed() &&
                    !handoff_->ConfirmPeerExited()) {
                    if (error_out) {
                        *error_out =
                            "producer handoff could not confirm recorder reap";
                    }
                    return false;
                }
                if (handoff_->ownership_indeterminate()) {
                    if (error_out) {
                        *error_out =
                            "producer handoff still retains indeterminate CUDA ownership";
                    }
                    return false;
                }
            }

            // The handoff stores non-owning pointers to exporter_/transport_.
            // Destroy it before either owner, even though transport was
            // already closed above.
            handoff_.reset();
            transport_.reset();
            producer_resources_released_ = true;
            return true;
        } catch (const std::exception& exception) {
            if (error_out) {
                try {
                    *error_out = bounded_reason(
                        std::string("producer CUDA resource release threw: ") +
                            exception.what(),
                        "producer CUDA resource release threw");
                } catch (...) {
                    error_out->clear();
                }
            }
        } catch (...) {
            if (error_out) {
                try {
                    *error_out =
                        "producer CUDA resource release threw an unknown exception";
                } catch (...) {
                    error_out->clear();
                }
            }
        }
        return false;
    }

private:
    bool fail(std::string* error_out, std::string message)
    {
        failed_ = true;
        first_failure_ = bounded_reason(message, "producer stream failed");
        return set_error(error_out, std::move(message));
    }

    nlohmann::json verified_plan_;
    session::spatial_roi::SpatialRoiRecorderStreamView stream_;
    int producer_gpu_id_ = -1;
    pid_t expected_recorder_pid_ = -1;
    uid_t expected_recorder_uid_ = static_cast<uid_t>(-1);
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds write_timeout_;
    std::chrono::milliseconds response_timeout_;
    ipc::SpatialRoiIpcFrameExporter exporter_;
    std::unique_ptr<ipc::SpatialRoiUnixSocketLineTransport> transport_;
    std::unique_ptr<ipc::SpatialRoiIpcHandoff> handoff_;
    bool started_ = false;
    bool failed_ = false;
    bool producer_resources_released_ = false;
    std::string first_failure_;
};

// Adapter for the existing four-lane SpatialRoiRecordingRuntime. It is kept
// private because callers should use the coordinator's one-source Submit API.
class ConcreteProducerRuntime final : public SpatialRoiCameraProducerRuntime {
public:
    explicit ConcreteProducerRuntime(
        std::shared_ptr<SpatialRoiRecordingRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    ConcreteProducerRuntime(
        const nlohmann::json& verified_plan,
        const std::string& camera_serial,
        int producer_gpu_id,
        SpatialRoiLaneSink sink)
        : runtime_(std::make_shared<SpatialRoiRecordingRuntime>(
              verified_plan, camera_serial, producer_gpu_id, std::move(sink)))
    {
    }

    SpatialRoiBatchSubmission TrySubmit(
        const SpatialRoiSourceView& source) override
    {
        return runtime_->TrySubmit(source);
    }

    void StopAccepting() noexcept override { runtime_->StopAccepting(); }

    bool StopAcceptingAndDrain(std::string* error_out,
                               bool* fully_drained_out) noexcept override
    {
        return runtime_->StopAcceptingAndDrain(error_out, fully_drained_out);
    }

private:
    std::shared_ptr<SpatialRoiRecordingRuntime> runtime_;
};

bool validate_stream_contract(
    const session::spatial_roi::SpatialRoiRecorderStreamView& stream,
    const session::spatial_roi::SpatialRoiRecorderCameraContractView& view,
    int producer_gpu_id,
    std::string* error_out)
{
    if (stream.camera_id != view.camera_id ||
        stream.camera_serial != view.camera_serial ||
        stream.source_gpu_id != producer_gpu_id ||
        stream.analytics_gpu_id != view.analytics_gpu_id ||
        stream.assigned_gpu_id != stream.recorder_gpu_id ||
        stream.encode_queue_depth == 0 ||
        stream.encode_queue_depth > ipc::kSpatialRoiIpcMaxQueueFrames ||
        stream.expected_shard_gpu_ids.size() != 1 ||
        stream.expected_shard_gpu_ids.front() != stream.recorder_gpu_id) {
        return set_error(
            error_out,
            "camera producer stream does not match the authenticated source/GPU/queue binding");
    }
    if (stream.stream_kind != "spatial_roi" ||
        stream.output_kind != "spatial_roi" ||
        stream.logical_stream_id.empty() || stream.roi_id.empty() ||
        stream.region_id.empty() || stream.socket_path.empty()) {
        return set_error(error_out,
                         "camera producer stream is not a complete fixed-region lane");
    }
    return true;
}

}  // namespace

const char* spatial_roi_camera_producer_state_name(
    SpatialRoiCameraProducerState state) noexcept
{
    switch (state) {
    case SpatialRoiCameraProducerState::kConstructed:
        return "constructed";
    case SpatialRoiCameraProducerState::kStarting:
        return "starting";
    case SpatialRoiCameraProducerState::kReady:
        return "ready";
    case SpatialRoiCameraProducerState::kStopped:
        return "stopped";
    case SpatialRoiCameraProducerState::kFailed:
        return "failed";
    }
    return "unknown";
}

const char* spatial_roi_camera_producer_submit_status_name(
    SpatialRoiCameraProducerSubmitStatus status) noexcept
{
    switch (status) {
    case SpatialRoiCameraProducerSubmitStatus::kSubmitted:
        return "submitted";
    case SpatialRoiCameraProducerSubmitStatus::kRuntimeIncomplete:
        return "runtime_incomplete";
    case SpatialRoiCameraProducerSubmitStatus::kNotReady:
        return "not_ready";
    case SpatialRoiCameraProducerSubmitStatus::kStopped:
        return "stopped";
    case SpatialRoiCameraProducerSubmitStatus::kBusy:
        return "busy";
    case SpatialRoiCameraProducerSubmitStatus::kInvalidArgument:
        return "invalid_argument";
    case SpatialRoiCameraProducerSubmitStatus::kPoolExhausted:
        return "pool_exhausted";
    case SpatialRoiCameraProducerSubmitStatus::kCudaError:
        return "cuda_error";
    case SpatialRoiCameraProducerSubmitStatus::kSourceQuarantined:
        return "source_quarantined";
    case SpatialRoiCameraProducerSubmitStatus::kDuplicateOrOutOfOrder:
        return "duplicate_or_out_of_order";
    case SpatialRoiCameraProducerSubmitStatus::kFailed:
        return "failed";
    }
    return "unknown";
}

SpatialRoiCameraProducerCoordinator::SpatialRoiCameraProducerCoordinator(
    SpatialRoiCameraProducerConfig config,
    session::spatial_roi::SpatialRoiRecorderCameraContractView contract,
    std::vector<StreamSlot> streams)
    : config_(std::move(config)),
      contract_(std::move(contract)),
      streams_(std::make_shared<std::vector<StreamSlot>>(std::move(streams))),
      runtime_factory_(config_.runtime_factory)
{
}

SpatialRoiCameraProducerCoordinator::~SpatialRoiCameraProducerCoordinator()
{
    StopAndDrain();
    if (!producer_resources_released_ && runtime_) {
        // The recorder may still retain session-cached CUDA IPC mappings even
        // though every individual FRAME received RELEASE. Without definitive
        // child-reap proof, destroying the concrete runtime could free its
        // batch-pool allocations/events underneath those mappings. Leak the
        // stopped runtime deliberately; this is the same fail-closed policy as
        // an indeterminate handoff export and is bounded to an abnormal
        // unreaped-recorder process lifetime.
        (void)runtime_.release();
        recording_runtime_.reset();
    }
}

std::string SpatialRoiCameraProducerCoordinator::bounded_reason(
    const std::string& value,
    const char* fallback)
{
    return ::orange::spatial_roi::bounded_reason(value, fallback);
}

bool SpatialRoiCameraProducerCoordinator::fail(std::string* error_out,
                                                std::string message)
{
    return set_error(error_out, std::move(message));
}

bool SpatialRoiCameraProducerCoordinator::validate_config(
    const SpatialRoiCameraProducerConfig& config,
    std::string* error_out)
{
    if (config.producer_gpu_id < 0 || config.expected_recorder_pid <= 0 ||
        config.expected_recorder_uid == static_cast<uid_t>(-1)) {
        return fail(error_out,
                    "camera producer requires nonnegative producer GPU and recorder credentials");
    }
    const auto valid_wait = [](const std::chrono::milliseconds value) {
        return value.count() > 0 && value <= kMaximumWait;
    };
    if (!valid_wait(config.connect_timeout) ||
        !valid_wait(config.write_timeout) ||
        !valid_wait(config.ipc_response_timeout)) {
        return fail(error_out,
                    "camera producer waits must be positive and at most five minutes");
    }
    if (config.expected_recording_root.empty()) {
        return fail(error_out, "camera producer recording root is empty");
    }
    return true;
}

bool SpatialRoiCameraProducerCoordinator::validate_contract(
    const session::spatial_roi::SpatialRoiRecorderCameraContractView& view,
    int producer_gpu_id,
    std::string* error_out)
{
    if (view.product_kind !=
            session::spatial_roi::kSpatialRoiRecorderCameraProductKind ||
        view.stream_count != kRequiredStreamCount ||
        view.stream_order.size() != kRequiredStreamCount ||
        view.streams.size() != kRequiredStreamCount ||
        view.analytics_gpu_id != producer_gpu_id) {
        return fail(error_out,
                    "camera producer accepts exactly four fixed-region streams on the producer GPU");
    }
    std::set<std::string> ids;
    for (std::size_t index = 0; index < view.streams.size(); ++index) {
        const auto& stream = view.streams[index];
        if (view.stream_order[index] != stream.logical_stream_id ||
            !ids.insert(stream.logical_stream_id).second ||
            !validate_stream_contract(stream, view, producer_gpu_id, error_out)) {
            return false;
        }
    }
    return ids.size() == kRequiredStreamCount;
}

std::unique_ptr<SpatialRoiCameraProducerCoordinator>
SpatialRoiCameraProducerCoordinator::Create(
    SpatialRoiCameraProducerConfig config,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!validate_config(config, error_out)) {
        return nullptr;
    }

    try {
        session::spatial_roi::SpatialRoiRecorderCameraContractView contract;
        std::string error;
        if (!session::spatial_roi::parse_spatial_roi_recorder_camera_contract(
                config.candidate_contract,
                config.independently_verified_plan,
                config.expected_recording_root,
                config.expected_gpu_mapping,
                &contract,
                &error)) {
            fail(error_out,
                 "camera producer contract authentication failed: " +
                     bounded_reason(error, "invalid camera contract"));
            return nullptr;
        }
        if (!validate_contract(contract, config.producer_gpu_id, &error)) {
            fail(error_out,
                 "camera producer contract validation failed: " +
                     bounded_reason(error, "invalid camera contract"));
            return nullptr;
        }

        SpatialRoiCameraProducerStreamFactory stream_factory =
            config.stream_factory;
        if (!stream_factory) {
            const nlohmann::json verified_plan =
                config.independently_verified_plan;
            const int producer_gpu_id = config.producer_gpu_id;
            const pid_t recorder_pid = config.expected_recorder_pid;
            const uid_t recorder_uid = config.expected_recorder_uid;
            const auto connect_timeout = config.connect_timeout;
            const auto write_timeout = config.write_timeout;
            const auto response_timeout = config.ipc_response_timeout;
            stream_factory =
                [verified_plan,
                 producer_gpu_id,
                 recorder_pid,
                 recorder_uid,
                 connect_timeout,
                 write_timeout,
                 response_timeout](
                    const session::spatial_roi::SpatialRoiRecorderStreamView& stream,
                    std::size_t,
                    std::string*) {
                    return std::unique_ptr<SpatialRoiCameraProducerStream>(
                        new ConcreteProducerStream(
                            verified_plan,
                            stream,
                            producer_gpu_id,
                            recorder_pid,
                            recorder_uid,
                            connect_timeout,
                            write_timeout,
                            response_timeout));
                };
        }

        std::vector<StreamSlot> streams;
        streams.reserve(kRequiredStreamCount);
        for (std::size_t index = 0; index < contract.streams.size(); ++index) {
            const auto& stream_contract = contract.streams[index];
            std::string factory_error;
            std::unique_ptr<SpatialRoiCameraProducerStream> stream;
            try {
                stream = stream_factory(stream_contract, index, &factory_error);
            } catch (const std::exception& exception) {
                factory_error = exception_reason("producer stream factory",
                                                 exception);
            } catch (...) {
                factory_error = unknown_exception_reason(
                    "producer stream factory");
            }
            if (!stream) {
                fail(error_out,
                     "camera producer could not construct stream " +
                         stream_contract.logical_stream_id + ": " +
                         bounded_reason(factory_error, "null stream"));
                return nullptr;
            }
            StreamSlot slot;
            slot.contract = stream_contract;
            slot.stream = std::move(stream);
            streams.push_back(std::move(slot));
        }
        if (streams.size() != kRequiredStreamCount) {
            fail(error_out, "camera producer did not construct four streams");
            return nullptr;
        }
        return std::unique_ptr<SpatialRoiCameraProducerCoordinator>(
            new SpatialRoiCameraProducerCoordinator(std::move(config),
                                                     std::move(contract),
                                                     std::move(streams)));
    } catch (const std::exception& exception) {
        fail(error_out,
             exception_reason("camera producer creation", exception));
        return nullptr;
    } catch (...) {
        fail(error_out,
             unknown_exception_reason("camera producer creation"));
        return nullptr;
    }
}

SpatialRoiCameraProducerSubmitStatus
SpatialRoiCameraProducerCoordinator::map_runtime_status(
    SpatialRoiRuntimeSubmitStatus status) noexcept
{
    switch (status) {
    case SpatialRoiRuntimeSubmitStatus::kAccepted:
        return SpatialRoiCameraProducerSubmitStatus::kSubmitted;
    case SpatialRoiRuntimeSubmitStatus::kIncomplete:
        return SpatialRoiCameraProducerSubmitStatus::kRuntimeIncomplete;
    case SpatialRoiRuntimeSubmitStatus::kBusy:
        return SpatialRoiCameraProducerSubmitStatus::kBusy;
    case SpatialRoiRuntimeSubmitStatus::kInvalidArgument:
        return SpatialRoiCameraProducerSubmitStatus::kInvalidArgument;
    case SpatialRoiRuntimeSubmitStatus::kPoolExhausted:
        return SpatialRoiCameraProducerSubmitStatus::kPoolExhausted;
    case SpatialRoiRuntimeSubmitStatus::kCudaError:
        return SpatialRoiCameraProducerSubmitStatus::kCudaError;
    case SpatialRoiRuntimeSubmitStatus::kSourceQuarantined:
        return SpatialRoiCameraProducerSubmitStatus::kSourceQuarantined;
    case SpatialRoiRuntimeSubmitStatus::kDuplicateOrOutOfOrder:
        return SpatialRoiCameraProducerSubmitStatus::kDuplicateOrOutOfOrder;
    case SpatialRoiRuntimeSubmitStatus::kStopped:
        return SpatialRoiCameraProducerSubmitStatus::kStopped;
    }
    return SpatialRoiCameraProducerSubmitStatus::kFailed;
}

void SpatialRoiCameraProducerCoordinator::latch_failure(
    std::string reason) noexcept
{
    state_ = SpatialRoiCameraProducerState::kFailed;
    if (first_failure_.empty()) {
        try {
            first_failure_ = bounded_reason(reason, "camera producer failed");
        } catch (...) {
        }
    }
}

void SpatialRoiCameraProducerCoordinator::stop_streams_best_effort() noexcept
{
    for (auto& slot : *streams_) {
        if (!slot.start_attempted || slot.stopped || !slot.stream) {
            continue;
        }
        slot.stopped = true;
        try {
            slot.stream->Stop();
        } catch (...) {
            latch_failure("producer stream stop threw");
        }
    }
}

bool SpatialRoiCameraProducerCoordinator::release_streams_after_recorder_reaped(
    std::string* error_out) noexcept
{
    bool success = true;
    std::string first_error;
    for (auto& slot : *streams_) {
        if (!slot.start_attempted || !slot.stream) {
            continue;
        }
        try {
            std::string stream_error;
            if (!slot.stream->ReleaseProducerCudaResourcesAfterRecorderReaped(
                    &stream_error)) {
                success = false;
                if (first_error.empty()) {
                    first_error = bounded_reason(
                        stream_error,
                        "producer stream retained CUDA ownership after recorder reap");
                }
            }
        } catch (const std::exception& exception) {
            success = false;
            if (first_error.empty()) {
                first_error = exception_reason(
                    "producer stream CUDA resource release", exception);
            }
        } catch (...) {
            success = false;
            if (first_error.empty()) {
                first_error = unknown_exception_reason(
                    "producer stream CUDA resource release");
            }
        }
    }
    if (!success) {
        latch_failure(first_error.empty()
                          ? "producer stream retained CUDA ownership after recorder reap"
                          : first_error);
        if (error_out) {
            try {
                *error_out = first_failure_.empty()
                                 ? "producer CUDA resource release failed"
                                 : first_failure_;
            } catch (...) {
            }
        }
    }
    return success;
}

bool SpatialRoiCameraProducerCoordinator::Start(std::string* error_out)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiCameraProducerState::kReady) {
        return true;
    }
    if (state_ != SpatialRoiCameraProducerState::kConstructed) {
        return fail(error_out,
                    "camera producer Start is invalid from state " +
                        std::string(spatial_roi_camera_producer_state_name(
                            state_)));
    }
    state_ = SpatialRoiCameraProducerState::kStarting;
    for (auto& slot : *streams_) {
        slot.start_attempted = true;
        std::string error;
        try {
            if (!slot.stream->Start(&error)) {
                latch_failure("producer stream start failed: " +
                              bounded_reason(error, "stream start failed"));
                stop_streams_best_effort();
                return fail(error_out, first_failure_);
            }
            slot.started = true;
        } catch (const std::exception& exception) {
            latch_failure(exception_reason("producer stream start", exception));
            stop_streams_best_effort();
            return fail(error_out, first_failure_);
        } catch (...) {
            latch_failure(unknown_exception_reason("producer stream start"));
            stop_streams_best_effort();
            return fail(error_out, first_failure_);
        }
    }

    const auto stream_slots = streams_;
    SpatialRoiLaneSink sink =
        [stream_slots](const SpatialRoiLaneDelivery& delivery) {
            if (delivery.lane_index >= stream_slots->size() ||
                !(*stream_slots)[delivery.lane_index].stream) {
                return SpatialRoiLaneSinkResult::kFailed;
            }
            try {
                return (*stream_slots)[delivery.lane_index].stream->Submit(
                    delivery);
            } catch (...) {
                return SpatialRoiLaneSinkResult::kFailed;
            }
        };

    try {
        std::string runtime_error;
        if (runtime_factory_) {
            runtime_ = runtime_factory_(
                config_.independently_verified_plan,
                contract_.camera_serial,
                config_.producer_gpu_id,
                std::move(sink),
                &runtime_error);
        } else {
            recording_runtime_ = std::make_shared<SpatialRoiRecordingRuntime>(
                config_.independently_verified_plan,
                contract_.camera_serial,
                config_.producer_gpu_id,
                std::move(sink));
            runtime_ = std::make_unique<ConcreteProducerRuntime>(
                recording_runtime_);
        }
        if (!runtime_) {
            latch_failure("camera producer runtime factory failed: " +
                          bounded_reason(runtime_error, "null runtime"));
            stop_streams_best_effort();
            return fail(error_out, first_failure_);
        }
    } catch (const std::exception& exception) {
        latch_failure(exception_reason("camera producer runtime", exception));
        stop_streams_best_effort();
        return fail(error_out, first_failure_);
    } catch (...) {
        latch_failure(unknown_exception_reason("camera producer runtime"));
        stop_streams_best_effort();
        return fail(error_out, first_failure_);
    }
    state_ = SpatialRoiCameraProducerState::kReady;
    return true;
}

SpatialRoiCameraProducerSubmitResult
SpatialRoiCameraProducerCoordinator::Submit(
    const SpatialRoiSourceView& source) noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    SpatialRoiCameraProducerSubmitResult result;
    try {
        ++submit_attempted_;
        if (state_ != SpatialRoiCameraProducerState::kReady || !runtime_) {
            result.status = state_ == SpatialRoiCameraProducerState::kFailed
                                ? SpatialRoiCameraProducerSubmitStatus::kFailed
                                : (state_ == SpatialRoiCameraProducerState::kStopped
                                       ? SpatialRoiCameraProducerSubmitStatus::kStopped
                                       : SpatialRoiCameraProducerSubmitStatus::kNotReady);
            result.error = "camera producer is not ready";
            return result;
        }
        result.runtime_submission = runtime_->TrySubmit(source);
        result.status = map_runtime_status(result.runtime_submission.status);
        if (result.status == SpatialRoiCameraProducerSubmitStatus::kSubmitted) {
            ++submitted_;
        } else if (result.status ==
                   SpatialRoiCameraProducerSubmitStatus::kRuntimeIncomplete) {
            ++incomplete_;
        } else if (result.status != SpatialRoiCameraProducerSubmitStatus::kBusy &&
                   result.status !=
                       SpatialRoiCameraProducerSubmitStatus::kDuplicateOrOutOfOrder) {
            ++rejected_;
        }
        if (result.status == SpatialRoiCameraProducerSubmitStatus::kCudaError ||
            result.status ==
                SpatialRoiCameraProducerSubmitStatus::kSourceQuarantined ||
            result.status == SpatialRoiCameraProducerSubmitStatus::kStopped) {
            runtime_->StopAccepting();
            latch_failure(std::string("camera producer runtime returned ") +
                          spatial_roi_runtime_submit_status_name(
                              result.runtime_submission.status));
        }
        return result;
    } catch (const std::exception& exception) {
        ++rejected_;
        latch_failure(exception_reason("camera producer submit", exception));
        result.status = SpatialRoiCameraProducerSubmitStatus::kFailed;
        result.error = first_failure_;
        return result;
    } catch (...) {
        ++rejected_;
        latch_failure(unknown_exception_reason("camera producer submit"));
        result.status = SpatialRoiCameraProducerSubmitStatus::kFailed;
        result.error = first_failure_;
        return result;
    }
}

bool SpatialRoiCameraProducerCoordinator::StopAndDrain(
    std::string* error_out) noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (error_out) {
        try {
            error_out->clear();
        } catch (...) {
        }
    }
    bool success = true;
    bool runtime_fully_drained = runtime_ == nullptr;
    if (runtime_) {
        try {
            std::string runtime_error;
            const bool runtime_success =
                runtime_->StopAcceptingAndDrain(&runtime_error,
                                                &runtime_fully_drained);
            if (!runtime_fully_drained) {
                success = false;
                latch_failure(
                    "camera producer runtime did not complete its drain: " +
                    bounded_reason(runtime_error,
                                   "runtime stopped admission without joining lanes"));
            } else if (!runtime_success) {
                success = false;
                latch_failure(
                    "camera producer runtime drain failed: " +
                    bounded_reason(runtime_error, "ROI lane incomplete"));
            }
        } catch (...) {
            success = false;
            latch_failure("camera producer runtime drain threw");
        }
        // Keep the stopped runtime and its batch-pool allocations alive while
        // the recorder owns session-cached CUDA IPC mappings. They are
        // released only at the explicit post-reap boundary below.
    }
    // A stop-only/re-entrant runtime result cannot make transport/handoff
    // teardown safe. Keep streams intact until a non-lane owner retries and
    // proves that every asynchronous callback has joined.
    if (runtime_fully_drained) {
        stop_streams_best_effort();
    }
    stop_and_drain_completed_ = runtime_fully_drained;
    if (state_ != SpatialRoiCameraProducerState::kFailed) {
        state_ = SpatialRoiCameraProducerState::kStopped;
    }
    if (state_ == SpatialRoiCameraProducerState::kFailed) {
        success = false;
    }
    if (!success && error_out) {
        try {
            *error_out = first_failure_.empty() ? "camera producer drain failed"
                                                : first_failure_;
        } catch (...) {
        }
    }
    return success;
}

bool SpatialRoiCameraProducerCoordinator::
    ReleaseProducerCudaResourcesAfterRecorderReaped(
        std::string* error_out) noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (error_out) {
        try {
            error_out->clear();
        } catch (...) {
        }
    }
    if (producer_resources_released_) {
        return true;
    }
    if (state_ != SpatialRoiCameraProducerState::kStopped &&
        state_ != SpatialRoiCameraProducerState::kFailed) {
        return fail(
            error_out,
            "producer CUDA resources may be released only after the "
            "coordinator is stopped or failed and the recorder is reaped");
    }
    if (!stop_and_drain_completed_) {
        return fail(
            error_out,
            "producer CUDA resources require a completed StopAndDrain "
            "boundary before recorder-reap release");
    }

    const bool success = release_streams_after_recorder_reaped(error_out);
    if (success) {
        runtime_.reset();
        recording_runtime_.reset();
        producer_resources_released_ = true;
    }
    return success;
}

std::shared_ptr<SpatialRoiRecordingRuntime>
SpatialRoiCameraProducerCoordinator::acquisition_runtime() const noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (state_ != SpatialRoiCameraProducerState::kReady) {
        return {};
    }
    return recording_runtime_;
}

bool SpatialRoiCameraProducerCoordinator::MakeAcquisitionSession(
    SpatialRoiAcquisitionSession* session_out) const noexcept
{
    if (!session_out) {
        return false;
    }
    *session_out = SpatialRoiAcquisitionSession{};
    const std::shared_ptr<SpatialRoiRecordingRuntime> runtime =
        acquisition_runtime();
    if (!runtime) {
        return false;
    }
    try {
        const SpatialRoiBatchLimits& limits = runtime->producer_limits();
        session_out->runtime = runtime;
        session_out->recording_id = limits.expected_recording_id;
        session_out->recording_identity_token =
            limits.expected_recording_identity_token;
        session_out->producer_generation =
            limits.expected_producer_generation;
        session_out->camera_id = limits.expected_camera_id;
        session_out->camera_serial = limits.expected_camera_serial;
        return true;
    } catch (...) {
        *session_out = SpatialRoiAcquisitionSession{};
        return false;
    }
}

SpatialRoiCameraProducerSnapshot
SpatialRoiCameraProducerCoordinator::snapshot() const
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    SpatialRoiCameraProducerSnapshot snapshot;
    snapshot.state = state_;
    snapshot.recording_id = contract_.recording_id;
    snapshot.session_id = contract_.session_id;
    snapshot.recording_identity_token = contract_.recording_identity_token;
    snapshot.producer_generation = contract_.producer_generation;
    snapshot.spatial_roi_plan_sha256 = contract_.spatial_roi_plan_sha256;
    snapshot.camera_id = contract_.camera_id;
    snapshot.camera_serial = contract_.camera_serial;
    snapshot.stream_count = streams_ ? streams_->size() : 0;
    snapshot.submit_attempted = submit_attempted_;
    snapshot.submitted = submitted_;
    snapshot.incomplete = incomplete_;
    snapshot.rejected = rejected_;
    snapshot.first_failure = first_failure_;
    return snapshot;
}

SpatialRoiCameraProducerState
SpatialRoiCameraProducerCoordinator::state() const noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return state_;
}

bool SpatialRoiCameraProducerCoordinator::ready() const noexcept
{
    return state() == SpatialRoiCameraProducerState::kReady;
}

bool SpatialRoiCameraProducerCoordinator::failed() const noexcept
{
    return state() == SpatialRoiCameraProducerState::kFailed;
}

std::string SpatialRoiCameraProducerCoordinator::first_failure() const
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return first_failure_;
}

}  // namespace orange::spatial_roi
