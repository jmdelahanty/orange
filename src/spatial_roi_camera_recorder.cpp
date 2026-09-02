#include "spatial_roi_camera_recorder.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

constexpr std::size_t kRequiredStreamCount = 4;
constexpr std::size_t kMaxFailureReasonBytes = 1024;

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

std::string exception_reason(const char* operation, const std::exception& ex)
{
    return std::string(operation) + " threw: " + ex.what();
}

std::string unknown_exception_reason(const char* operation)
{
    return std::string(operation) + " threw an unknown exception";
}

std::string bounded_snapshot_reason(std::string value)
{
    if (value.size() > kMaxFailureReasonBytes) {
        value.resize(kMaxFailureReasonBytes);
    }
    for (char& byte : value) {
        const unsigned char unsigned_byte = static_cast<unsigned char>(byte);
        if (unsigned_byte < 0x20U || unsigned_byte == 0x7fU) {
            byte = '?';
        }
    }
    return value;
}

}  // namespace

const char* spatial_roi_camera_recorder_state_name(
    SpatialRoiCameraRecorderState state) noexcept
{
    switch (state) {
    case SpatialRoiCameraRecorderState::kConstructed:
        return "constructed";
    case SpatialRoiCameraRecorderState::kStarting:
        return "starting";
    case SpatialRoiCameraRecorderState::kReady:
        return "ready";
    case SpatialRoiCameraRecorderState::kAwaitingEof:
        return "awaiting_eof";
    case SpatialRoiCameraRecorderState::kEofObserved:
        return "eof_observed";
    case SpatialRoiCameraRecorderState::kFinalizing:
        return "finalizing";
    case SpatialRoiCameraRecorderState::kCompleted:
        return "completed";
    case SpatialRoiCameraRecorderState::kFailed:
        return "failed";
    }
    return "unknown";
}

nlohmann::json spatial_roi_camera_recorder_snapshot_to_json(
    const SpatialRoiCameraRecorderSnapshot& snapshot,
    const std::string& event,
    const std::string& terminal_reason)
{
    nlohmann::json streams = nlohmann::json::array();
    for (const auto& stream : snapshot.streams) {
        streams.push_back({
            {"plan_index", stream.plan_index},
            {"logical_stream_id", stream.logical_stream_id},
            {"roi_id", stream.roi_id},
            {"region_id", stream.region_id},
            {"camera_serial", stream.camera_serial},
            {"recording_id", stream.recording_id},
            {"session_id", stream.session_id},
            {"start_attempted", stream.start_attempted},
            {"started", stream.started},
            {"ready", stream.ready},
            {"clean_eof", stream.clean_eof},
            {"stop_admission_attempted", stream.stop_admission_attempted},
            {"admission_stopped", stream.admission_stopped},
            {"drain_attempted", stream.drain_attempted},
            {"drained", stream.drained},
            {"finalize_attempted", stream.finalize_attempted},
            {"finalized", stream.finalized},
            {"failed", stream.failed},
            {"first_failure", bounded_snapshot_reason(stream.first_failure)},
        });
    }
    const bool failed = !terminal_reason.empty() ||
                        snapshot.state ==
                            SpatialRoiCameraRecorderState::kFailed;
    const std::string failure = bounded_snapshot_reason(
        !snapshot.first_failure.empty() ? snapshot.first_failure
                                        : terminal_reason);
    const std::string status =
        failed ? "failed"
               : snapshot.completed ? "complete"
                                    : event == "ready" ? "ready" : "running";
    return {
        {"program", "spatial_roi_camera_recorder"},
        {"event", event},
        {"status", status},
        {"state", failed ? "failed"
                          : spatial_roi_camera_recorder_state_name(snapshot.state)},
        {"product_kind", snapshot.product_kind},
        {"recording_id", snapshot.recording_id},
        {"session_id", snapshot.session_id},
        {"recording_identity_token", snapshot.recording_identity_token},
        {"producer_generation", snapshot.producer_generation},
        {"spatial_roi_plan_sha256", snapshot.spatial_roi_plan_sha256},
        {"camera_id", snapshot.camera_id},
        {"camera_serial", snapshot.camera_serial},
        {"ready", snapshot.ready},
        {"clean_eof", snapshot.clean_eof},
        {"completed", snapshot.completed},
        {"failed", failed},
        {"first_failure_stream_id", snapshot.first_failure_stream_id},
        {"first_failure", failure},
        {"error", failure},
        {"storage_preflight",
         spatial_roi_recorder_storage_preflight_to_json(
             snapshot.storage_preflight)},
        {"streams", std::move(streams)},
    };
}

SpatialRoiCameraRecorder::SpatialRoiCameraRecorder(
    session::spatial_roi::SpatialRoiRecorderCameraContractView contract,
    std::vector<StreamSlot> streams)
    : contract_(std::move(contract)), streams_(std::move(streams))
{
}

SpatialRoiCameraRecorder::~SpatialRoiCameraRecorder()
{
    stop_admission_best_effort();
}

std::unique_ptr<SpatialRoiCameraRecorder> SpatialRoiCameraRecorder::Create(
    const nlohmann::json& candidate_contract,
    const nlohmann::json& independently_verified_plan,
    const std::string& expected_recording_root,
    const session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping&
        expected_gpu_mapping,
    SpatialRoiCameraRecorderStreamCoreFactory factory,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!factory) {
        fail(error_out, "spatial ROI camera recorder core factory is empty");
        return nullptr;
    }

    try {
        session::spatial_roi::SpatialRoiRecorderCameraContractView contract;
        std::string parse_error;
        if (!session::spatial_roi::parse_spatial_roi_recorder_camera_contract(
                candidate_contract,
                independently_verified_plan,
                expected_recording_root,
                expected_gpu_mapping,
                &contract,
                &parse_error)) {
            fail(error_out,
                 "spatial ROI camera recorder contract authentication failed: " +
                     parse_error);
            return nullptr;
        }
        if (!validate_authenticated_view(contract, &parse_error)) {
            fail(error_out,
                 "spatial ROI camera recorder authenticated view is invalid: " +
                     parse_error);
            return nullptr;
        }

        std::vector<StreamSlot> streams;
        streams.reserve(kRequiredStreamCount);
        for (std::size_t index = 0; index < contract.streams.size(); ++index) {
            const auto& stream = contract.streams[index];
            std::string factory_error;
            std::unique_ptr<SpatialRoiCameraRecorderStreamCore> core;
            try {
                core = factory(stream, index, &factory_error);
            } catch (const std::exception& ex) {
                factory_error = exception_reason("stream core factory", ex);
            } catch (...) {
                factory_error =
                    unknown_exception_reason("stream core factory");
            }
            if (!core) {
                fail(error_out,
                     "spatial ROI camera recorder could not construct stream " +
                         stream.logical_stream_id + ": " +
                         bounded_reason(factory_error,
                                        "factory returned a null core"));
                return nullptr;
            }

            StreamSlot slot;
            slot.contract = stream;
            slot.core = std::move(core);
            slot.snapshot.plan_index = index;
            slot.snapshot.logical_stream_id = stream.logical_stream_id;
            slot.snapshot.roi_id = stream.roi_id;
            slot.snapshot.region_id = stream.region_id;
            slot.snapshot.camera_serial = stream.camera_serial;
            slot.snapshot.recording_id = stream.recording_id;
            slot.snapshot.session_id = stream.session_id;
            streams.push_back(std::move(slot));
        }

        return std::unique_ptr<SpatialRoiCameraRecorder>(
            new SpatialRoiCameraRecorder(std::move(contract),
                                         std::move(streams)));
    } catch (const std::exception& ex) {
        fail(error_out,
             exception_reason("spatial ROI camera recorder creation", ex));
        return nullptr;
    } catch (...) {
        fail(error_out,
             unknown_exception_reason("spatial ROI camera recorder creation"));
        return nullptr;
    }
}

bool SpatialRoiCameraRecorder::validate_authenticated_view(
    const session::spatial_roi::SpatialRoiRecorderCameraContractView& view,
    std::string* error_out)
{
    if (view.schema_id !=
            session::spatial_roi::kSpatialRoiRecorderCameraContractSchemaId ||
        view.schema_version !=
            session::spatial_roi::kSpatialRoiRecorderCameraContractSchemaVersion ||
        view.product_kind !=
            session::spatial_roi::kSpatialRoiRecorderCameraProductKind) {
        return fail(error_out,
                    "camera contract schema or product kind is not the fixed-region first slice");
    }
    if (view.storage_preflight_policy.schema_id !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
        view.storage_preflight_policy.schema_version !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !view.storage_preflight_policy.required ||
        view.storage_preflight_policy.reserved_free_bytes == 0) {
        return fail(error_out,
                    "camera contract has no valid nonzero storage preflight policy");
    }
    if (view.recording_id.empty() || view.session_id != view.recording_id ||
        view.recording_identity_token.empty() ||
        view.producer_generation.empty() ||
        view.spatial_roi_plan_sha256.empty() || view.recording_root.empty() ||
        view.artifact_root.empty() || view.camera_id < 0 ||
        view.camera_serial.empty() || view.native_raster.width == 0 ||
        view.native_raster.height == 0 || view.analytics_gpu_id < 0) {
        return fail(error_out,
                    "camera contract shared recording/camera identity is incomplete");
    }
    if (view.stream_count != kRequiredStreamCount ||
        view.stream_order.size() != kRequiredStreamCount ||
        view.streams.size() != kRequiredStreamCount ||
        view.recorder_gpu_by_logical_stream_id.size() !=
            kRequiredStreamCount ||
        view.analytics_gpu_by_camera_serial.size() != 1) {
        return fail(
            error_out,
            "camera recorder requires exactly four authenticated streams and GPU assignments");
    }

    std::set<std::string> logical_stream_ids;
    std::set<std::string> roi_ids;
    std::set<std::string> region_ids;
    for (std::size_t index = 0; index < view.streams.size(); ++index) {
        const auto& stream = view.streams[index];
        if (view.stream_order[index] != stream.logical_stream_id ||
            stream.stream_kind != "spatial_roi" ||
            stream.output_kind != "spatial_roi") {
            return fail(error_out,
                        "camera streams are not fixed-region outputs in authenticated plan order");
        }
        if (stream.camera_id != view.camera_id ||
            stream.camera_serial != view.camera_serial ||
            stream.recording_id != view.recording_id ||
            stream.session_id != view.session_id ||
            stream.recording_identity_token !=
                view.recording_identity_token ||
            stream.producer_generation != view.producer_generation ||
            stream.spatial_roi_plan_sha256 !=
                view.spatial_roi_plan_sha256 ||
            stream.analytics_gpu_id != view.analytics_gpu_id ||
            stream.source_gpu_id != view.analytics_gpu_id) {
            return fail(
                error_out,
                "stream shared recording/camera identity disagrees with its camera contract");
        }
        const auto recorder_gpu =
            view.recorder_gpu_by_logical_stream_id.find(
                stream.logical_stream_id);
        if (recorder_gpu ==
                view.recorder_gpu_by_logical_stream_id.end() ||
            stream.recorder_gpu_id != recorder_gpu->second ||
            stream.assigned_gpu_id != recorder_gpu->second) {
            return fail(error_out,
                        "stream recorder GPU assignment disagrees with its camera contract");
        }
        if (!logical_stream_ids.insert(stream.logical_stream_id).second ||
            !roi_ids.insert(stream.roi_id).second ||
            !region_ids.insert(stream.region_id).second) {
            return fail(error_out,
                        "camera stream, ROI, and region identities must be unique");
        }
    }
    return true;
}

std::string SpatialRoiCameraRecorder::bounded_reason(const std::string& value,
                                                     const char* fallback)
{
    if (!value.empty()) {
        if (value.size() <= kMaxFailureReasonBytes) {
            return value;
        }
        return value.substr(0, kMaxFailureReasonBytes);
    }
    const std::string fallback_value = fallback ? fallback : "operation failed";
    if (fallback_value.size() <= kMaxFailureReasonBytes) {
        return fallback_value;
    }
    return fallback_value.substr(0, kMaxFailureReasonBytes);
}

bool SpatialRoiCameraRecorder::require_state(
    SpatialRoiCameraRecorderState expected,
    const char* operation,
    std::string* error_out)
{
    if (state_ == expected) {
        return true;
    }
    std::string reason = std::string(operation) + " is invalid from state " +
                         spatial_roi_camera_recorder_state_name(state_);
    if (first_failure_.empty()) {
        first_failure_stream_id_ = "camera:" + contract_.camera_serial;
        first_failure_ = bounded_reason(reason, "invalid lifecycle transition");
    }
    state_ = SpatialRoiCameraRecorderState::kFailed;
    stop_admission_best_effort();
    set_error(error_out);
    return false;
}

void SpatialRoiCameraRecorder::record_stream_failure(
    std::size_t index,
    const std::string& reason) noexcept
{
    failure_latched_ = true;
    if (index >= streams_.size()) {
        return;
    }
    streams_[index].snapshot.failed = true;
    try {
        auto& stream = streams_[index];
        const std::string bounded =
            bounded_reason(reason, "stream core operation failed");
        if (stream.snapshot.first_failure.empty()) {
            stream.snapshot.first_failure = bounded;
        }
        if (first_failure_.empty()) {
            first_failure_stream_id_ = stream.contract.logical_stream_id;
            first_failure_ = bounded;
        }
    } catch (...) {
        // Lifecycle failure reporting must not replace the original core
        // failure with an exception.  The state transition still fails shut.
    }
}

void SpatialRoiCameraRecorder::latch_failure(std::size_t index,
                                             const std::string& reason,
                                             bool stop_admission) noexcept
{
    record_stream_failure(index, reason);
    state_ = SpatialRoiCameraRecorderState::kFailed;
    if (stop_admission) {
        stop_admission_best_effort();
    }
}

void SpatialRoiCameraRecorder::stop_admission_best_effort() noexcept
{
    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        if (!stream.snapshot.start_attempted ||
            stream.snapshot.stop_admission_attempted) {
            continue;
        }
        stream.snapshot.stop_admission_attempted = true;
        std::string error;
        try {
            if (stream.core->StopAdmission(&error)) {
                stream.snapshot.admission_stopped = true;
            } else {
                record_stream_failure(
                    index,
                    "stop-admission failed: " +
                        bounded_reason(error, "core rejected stop"));
            }
        } catch (const std::exception& ex) {
            record_stream_failure(
                index, exception_reason("stop-admission", ex));
        } catch (...) {
            record_stream_failure(
                index, unknown_exception_reason("stop-admission"));
        }
    }
}

void SpatialRoiCameraRecorder::set_error(std::string* error_out) const
{
    if (error_out) {
        *error_out = first_failure_;
    }
}

bool SpatialRoiCameraRecorder::Start(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!require_state(
            SpatialRoiCameraRecorderState::kConstructed, "Start", error_out)) {
        return false;
    }
    state_ = SpatialRoiCameraRecorderState::kStarting;

    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        stream.snapshot.start_attempted = true;
        std::string error;
        try {
            if (!stream.core->Start(&error)) {
                latch_failure(
                    index,
                    "start failed: " +
                        bounded_reason(error, "core rejected start"),
                    true);
                set_error(error_out);
                return false;
            }
            stream.snapshot.started = true;
        } catch (const std::exception& ex) {
            latch_failure(index, exception_reason("start", ex), true);
            set_error(error_out);
            return false;
        } catch (...) {
            latch_failure(index, unknown_exception_reason("start"), true);
            set_error(error_out);
            return false;
        }
    }
    return true;
}

bool SpatialRoiCameraRecorder::PollReadiness(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiCameraRecorderState::kReady) {
        return true;
    }
    if (!require_state(
            SpatialRoiCameraRecorderState::kStarting,
            "PollReadiness",
            error_out)) {
        return false;
    }
    const auto& storage_policy = contract_.storage_preflight_policy;
    std::uint64_t expected_required_bytes =
        contract_.aggregate_bounds.max_media_bytes_total;
    bool expected_required_bytes_valid =
        expected_required_bytes <=
        std::numeric_limits<std::uint64_t>::max() -
            contract_.aggregate_bounds.max_evidence_bytes_total;
    if (expected_required_bytes_valid) {
        expected_required_bytes +=
            contract_.aggregate_bounds.max_evidence_bytes_total;
        expected_required_bytes_valid =
            expected_required_bytes <=
            std::numeric_limits<std::uint64_t>::max() -
                storage_policy.reserved_free_bytes;
    }
    if (expected_required_bytes_valid) {
        expected_required_bytes += storage_policy.reserved_free_bytes;
    }
    if (storage_preflight_.schema_id !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaId ||
        storage_preflight_.schema_version !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaVersion ||
        !storage_preflight_.checked || !storage_preflight_.passed ||
        storage_preflight_.status != "passed" ||
        storage_preflight_.policy.schema_id !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
        storage_preflight_.policy.schema_version !=
            session::spatial_roi::kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !storage_preflight_.policy.required ||
        storage_preflight_.policy.reserved_free_bytes == 0 ||
        storage_preflight_.policy.reserved_free_bytes !=
            storage_policy.reserved_free_bytes ||
        storage_preflight_.max_media_bytes_total !=
            contract_.aggregate_bounds.max_media_bytes_total ||
        storage_preflight_.max_evidence_bytes_total !=
            contract_.aggregate_bounds.max_evidence_bytes_total ||
        !expected_required_bytes_valid ||
        storage_preflight_.required_bytes != expected_required_bytes ||
        storage_preflight_.filesystem.block_size_bytes == 0 ||
        storage_preflight_.filesystem.available_blocks >
            storage_preflight_.filesystem.total_blocks ||
        storage_preflight_.available_bytes > storage_preflight_.capacity_bytes) {
        latch_failure(0,
                      "readiness requires a successful storage preflight",
                      true);
        set_error(error_out);
        return false;
    }

    bool all_ready = true;
    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        if (stream.snapshot.ready) {
            continue;
        }
        std::string error;
        try {
            const auto status = stream.core->PollReadiness(&error);
            if (status == SpatialRoiCameraRecorderReadinessStatus::kReady) {
                stream.snapshot.ready = true;
                continue;
            }
            if (status == SpatialRoiCameraRecorderReadinessStatus::kPending) {
                all_ready = false;
                continue;
            }
            latch_failure(
                index,
                "readiness failed: " +
                    bounded_reason(error, "core reported failed readiness"),
                true);
            set_error(error_out);
            return false;
        } catch (const std::exception& ex) {
            latch_failure(index,
                          exception_reason("readiness poll", ex),
                          true);
            set_error(error_out);
            return false;
        } catch (...) {
            latch_failure(index,
                          unknown_exception_reason("readiness poll"),
                          true);
            set_error(error_out);
            return false;
        }
    }
    if (all_ready) {
        state_ = SpatialRoiCameraRecorderState::kReady;
    }
    return true;
}

bool SpatialRoiCameraRecorder::PollEof(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiCameraRecorderState::kEofObserved) {
        return true;
    }
    if (state_ == SpatialRoiCameraRecorderState::kReady) {
        state_ = SpatialRoiCameraRecorderState::kAwaitingEof;
    }
    if (!require_state(SpatialRoiCameraRecorderState::kAwaitingEof,
                       "PollEof",
                       error_out)) {
        return false;
    }

    bool all_clean = true;
    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        if (stream.snapshot.clean_eof) {
            continue;
        }
        std::string error;
        try {
            const auto status = stream.core->PollEof(&error);
            if (status == SpatialRoiCameraRecorderEofStatus::kClean) {
                stream.snapshot.clean_eof = true;
                continue;
            }
            if (status == SpatialRoiCameraRecorderEofStatus::kPending) {
                all_clean = false;
                continue;
            }
            latch_failure(
                index,
                "EOF failed: " +
                    bounded_reason(error, "core reported fatal EOF"),
                true);
            set_error(error_out);
            return false;
        } catch (const std::exception& ex) {
            latch_failure(index, exception_reason("EOF poll", ex), true);
            set_error(error_out);
            return false;
        } catch (...) {
            latch_failure(index,
                          unknown_exception_reason("EOF poll"),
                          true);
            set_error(error_out);
            return false;
        }
    }
    if (all_clean) {
        state_ = SpatialRoiCameraRecorderState::kEofObserved;
    }
    return true;
}

bool SpatialRoiCameraRecorder::DrainAndFinalize(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiCameraRecorderState::kCompleted) {
        return true;
    }
    if (!require_state(SpatialRoiCameraRecorderState::kEofObserved,
                       "DrainAndFinalize",
                       error_out)) {
        return false;
    }
    state_ = SpatialRoiCameraRecorderState::kFinalizing;

    stop_admission_best_effort();

    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        if (!stream.snapshot.started) {
            record_stream_failure(index,
                                  "stream reached finalization without a successful start");
            continue;
        }
        stream.snapshot.drain_attempted = true;
        std::string error;
        try {
            if (stream.core->Drain(&error)) {
                stream.snapshot.drained = true;
            } else {
                record_stream_failure(
                    index,
                    "drain failed: " +
                        bounded_reason(error, "core rejected drain"));
            }
        } catch (const std::exception& ex) {
            record_stream_failure(index, exception_reason("drain", ex));
        } catch (...) {
            record_stream_failure(index,
                                  unknown_exception_reason("drain"));
        }
    }

    // Finalize every started core even when another stream failed to drain.
    // Each core therefore retains its own terminal evidence and identity while
    // the required four-stream aggregate remains incomplete.
    for (std::size_t index = 0; index < streams_.size(); ++index) {
        auto& stream = streams_[index];
        if (!stream.snapshot.started) {
            continue;
        }
        stream.snapshot.finalize_attempted = true;
        std::string error;
        try {
            if (stream.core->Finalize(&error)) {
                stream.snapshot.finalized = true;
            } else {
                record_stream_failure(
                    index,
                    "finalize failed: " +
                        bounded_reason(error, "core rejected finalize"));
            }
        } catch (const std::exception& ex) {
            record_stream_failure(index, exception_reason("finalize", ex));
        } catch (...) {
            record_stream_failure(index,
                                  unknown_exception_reason("finalize"));
        }
    }

    if (!failure_latched_) {
        state_ = SpatialRoiCameraRecorderState::kCompleted;
        return true;
    }
    state_ = SpatialRoiCameraRecorderState::kFailed;
    set_error(error_out);
    return false;
}

SpatialRoiCameraRecorderSnapshot SpatialRoiCameraRecorder::snapshot() const
{
    SpatialRoiCameraRecorderSnapshot result;
    result.state = state_;
    result.product_kind = contract_.product_kind;
    result.recording_id = contract_.recording_id;
    result.session_id = contract_.session_id;
    result.recording_identity_token = contract_.recording_identity_token;
    result.producer_generation = contract_.producer_generation;
    result.spatial_roi_plan_sha256 = contract_.spatial_roi_plan_sha256;
    result.camera_id = contract_.camera_id;
    result.camera_serial = contract_.camera_serial;
    result.first_failure_stream_id = first_failure_stream_id_;
    result.first_failure = first_failure_;
    result.storage_preflight = storage_preflight_;
    result.streams.reserve(streams_.size());
    for (const auto& stream : streams_) {
        result.streams.push_back(stream.snapshot);
    }
    result.ready = !result.streams.empty() &&
                   std::all_of(result.streams.begin(),
                               result.streams.end(),
                               [](const auto& stream) { return stream.ready; });
    result.clean_eof =
        !result.streams.empty() &&
        std::all_of(result.streams.begin(),
                    result.streams.end(),
                    [](const auto& stream) { return stream.clean_eof; });
    result.completed = state_ == SpatialRoiCameraRecorderState::kCompleted;
    return result;
}

}  // namespace orange::spatial_roi::recording
