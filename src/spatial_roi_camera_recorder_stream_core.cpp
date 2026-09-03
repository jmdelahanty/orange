#include "spatial_roi_camera_recorder_stream_core.h"

#include "spatial_roi_recorder_frame_journal_bridge.h"
#include "spatial_roi_recorder_terminal_sidecars.h"
#include "spatial_roi_recorder_video_sanity.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace orange::spatial_roi::recording {
namespace {

constexpr std::size_t kEncoderArtifactCount = 4;
constexpr std::size_t kFinalizeArtifactCount = 10;
constexpr std::array<const char*, kEncoderArtifactCount> kEncoderArtifacts = {
    "video", "metadata", "keyframes", "finalization"};
constexpr std::array<const char*, kFinalizeArtifactCount> kFinalizeArtifacts = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar"};
constexpr auto kMaximumAcceptTimeout = std::chrono::minutes(5);
constexpr auto kMaximumProbeTimeout = std::chrono::hours(1);
constexpr std::size_t kMaximumReasonBytes = 1024;

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        if (message.size() > kMaximumReasonBytes) {
            message.resize(kMaximumReasonBytes);
        }
        *error_out = std::move(message);
    }
    return false;
}

std::string exception_text(const char* operation, const std::exception& ex)
{
    std::string result = operation ? operation : "spatial ROI stream operation";
    result += " threw: ";
    result += ex.what();
    return result;
}

std::string unknown_exception_text(const char* operation)
{
    return std::string(operation ? operation : "spatial ROI stream operation") +
           " threw an unknown exception";
}

std::string socket_leaf(const std::string& path)
{
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? std::string() : path.substr(separator + 1);
}

nlohmann::json authority_json(
    const session::spatial_roi::SpatialRoiRecorderAuthorityView& authority)
{
    return {{"id", authority.id}, {"sha256", authority.sha256}};
}

nlohmann::json raster_json(const SpatialRoiFrameRaster& raster)
{
    return {{"width", raster.width}, {"height", raster.height}};
}

nlohmann::json rect_json(const SpatialRoiFrameRect& rect)
{
    return {{"x", rect.x}, {"y", rect.y}, {"width", rect.width},
            {"height", rect.height}};
}

nlohmann::json geometry_json(
    const session::spatial_roi::SpatialRoiRecorderGeometryView& geometry)
{
    return {
        {"layout", authority_json(geometry.layout)},
        {"materialization", authority_json(geometry.materialization)},
        {"registration", authority_json(geometry.registration)},
        {"native_raster", raster_json(geometry.native_raster)},
        {"content_rect", rect_json(geometry.content_rect)},
        {"encoded_raster", raster_json(geometry.encoded_raster)},
        {"encoded_content_rect", rect_json(geometry.encoded_content_rect)},
        {"content_offset", {{"x", geometry.content_offset_x},
                            {"y", geometry.content_offset_y}}},
        {"padding", {{"left", geometry.padding.left},
                     {"top", geometry.padding.top},
                     {"right", geometry.padding.right},
                     {"bottom", geometry.padding.bottom},
                     {"value_mono8", geometry.padding.value_mono8}}},
        {"source_coordinate_space", geometry.source_coordinate_space},
        {"video_coordinate_space", geometry.video_coordinate_space},
    };
}

nlohmann::json encode_profile_json(
    const session::spatial_roi::SpatialRoiRecorderEncodeProfileView& profile)
{
    return {
        {"profile_id", profile.profile_id},
        {"codec", profile.codec},
        {"preset", profile.preset},
        {"tuning", profile.tuning},
        {"lossless", profile.lossless},
        {"rate_control_mode", profile.rate_control_mode},
        {"quality_value", profile.quality_value},
        {"gop_length", profile.gop_length},
        {"aq", profile.aq},
        {"temporal_aq", profile.temporal_aq},
        {"lookahead", profile.lookahead},
        {"lookahead_depth", profile.lookahead_depth},
        {"frame_rate", profile.frame_rate},
        {"input_format", profile.input_format},
        {"encoded_format", profile.encoded_format},
        {"no_resize", profile.no_resize},
        {"luma_preserved_exactly", profile.luma_preserved_exactly},
        {"neutral_chroma_value", profile.neutral_chroma_value},
    };
}

}  // namespace

std::unique_ptr<SpatialRoiConcreteCameraRecorderStreamCore>
SpatialRoiConcreteCameraRecorderStreamCore::Create(
    SpatialRoiCameraRecorderStreamCoreConfig config,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    try {
        std::string validation_error;
        if (!validate_config(config, &validation_error)) {
            fail(error_out, validation_error);
            return nullptr;
        }
        return std::unique_ptr<SpatialRoiConcreteCameraRecorderStreamCore>(
            new SpatialRoiConcreteCameraRecorderStreamCore(std::move(config)));
    } catch (const std::exception& ex) {
        fail(error_out, exception_text("spatial ROI stream creation", ex));
    } catch (...) {
        fail(error_out, unknown_exception_text("spatial ROI stream creation"));
    }
    return nullptr;
}

SpatialRoiConcreteCameraRecorderStreamCore::
    SpatialRoiConcreteCameraRecorderStreamCore(
        SpatialRoiCameraRecorderStreamCoreConfig config)
    : config_(std::move(config)),
      evidence_binding_(config_.evidence_binding)
{
}

SpatialRoiConcreteCameraRecorderStreamCore::~SpatialRoiConcreteCameraRecorderStreamCore()
{
    // A supervisor normally reaches clean EOF and calls Finalize.  The
    // destructor still makes a best-effort, fail-closed teardown for partial
    // startup. RequestShutdown wakes the sole transport owner without racing
    // its mutable state; Close is deferred until after the owner is joined.
    try {
        (void)StopAdmission(nullptr);
    } catch (...) {
        admission_stopped_ = true;
        cancellation_requested_ = true;
        if (transport_ && session_thread_.joinable() && !session_done_) {
            (void)transport_->RequestShutdown(nullptr);
        }
    }
    if (session_thread_.joinable()) {
        session_thread_.join();
    }
    if (transport_) {
        transport_->Close();
    }
    session_.reset();
    transport_.reset();
    listener_.reset();
    encoder_.reset();
    detach_pool_.reset();
    journal_.reset();
    evidence_writer_.reset();
}

bool SpatialRoiConcreteCameraRecorderStreamCore::validate_config(
    const SpatialRoiCameraRecorderStreamCoreConfig& config,
    std::string* error_out)
{
    if (!config.artifact_root || !config.artifact_root->valid() ||
        !config.runtime_directory || !config.runtime_directory->valid()) {
        return fail(error_out,
                    "stream core requires shared authenticated artifact and runtime authorities");
    }
    if (config.expected_producer_pid <= 0 ||
        config.expected_producer_uid == static_cast<uid_t>(-1)) {
        return fail(error_out,
                    "stream core requires credentials for the spawned producer");
    }
    if (config.accept_timeout.count() <= 0 ||
        config.accept_timeout > kMaximumAcceptTimeout ||
        config.ipc_response_timeout.count() <= 0 ||
        config.ipc_response_timeout > kMaximumAcceptTimeout ||
        config.video_probe_timeout.count() <= 0 ||
        config.video_probe_timeout > kMaximumProbeTimeout) {
        return fail(error_out, "stream core wait bounds are outside the supported range");
    }
    std::string binding_error;
    if (!validate_spatial_roi_recorder_evidence_binding(
            config.evidence_binding, &binding_error)) {
        return fail(error_out,
                    binding_error.empty() ? "stream evidence binding is invalid"
                                          : binding_error);
    }

    const auto& stream = config.stream;
    const ipc::SpatialRoiIpcStreamIdentity expected = stream_identity(stream);
    std::string identity_error;
    if (!ipc::validate_spatial_roi_ipc_stream_identity(expected,
                                                        &identity_error)) {
        return fail(error_out,
                    identity_error.empty() ? "stream identity is invalid"
                                            : identity_error);
    }
    if (stream.stream_kind != "spatial_roi" || stream.output_kind != "spatial_roi" ||
        stream.socket_path.empty() || socket_leaf(stream.socket_path).empty() ||
        stream.encode_fps == 0 || stream.encode_queue_depth == 0 ||
        stream.detach_pool_frames == 0 || stream.max_detach_pool_bytes == 0 ||
        stream.max_queue_bytes == 0 || stream.operation_timeout_ms == 0 ||
        stream.max_frames_per_stream == 0 || stream.max_media_bytes_per_stream == 0 ||
        stream.max_evidence_bytes_per_stream == 0) {
        return fail(error_out, "stream contract has incomplete recorder admission fields");
    }
    if (config.artifact_root->opened_recording_root().lexically_normal() !=
        std::filesystem::path(config.evidence_binding.recording_root).lexically_normal() ||
        std::filesystem::path(config.evidence_binding.artifact_root).lexically_normal() !=
            (config.artifact_root->opened_recording_root() /
             kSpatialRoiRecorderArtifactDirectory)
                .lexically_normal()) {
        return fail(error_out, "stream artifact authority does not match evidence binding");
    }
    if (config.evidence_binding.logical_stream_id != stream.logical_stream_id ||
        config.evidence_binding.recording_id != stream.recording_id ||
        config.evidence_binding.session_id != stream.session_id ||
        config.evidence_binding.recording_identity_token !=
            stream.recording_identity_token ||
        config.evidence_binding.producer_generation != stream.producer_generation ||
        config.evidence_binding.camera_id != stream.camera_id ||
        config.evidence_binding.camera_serial != stream.camera_serial ||
        config.evidence_binding.roi_id != stream.roi_id ||
        config.evidence_binding.region_id != stream.region_id ||
        config.evidence_binding.arena_group_id != stream.arena_group_id ||
        config.evidence_binding.has_arena_id != stream.has_arena_id ||
        config.evidence_binding.arena_id != stream.arena_id ||
        config.evidence_binding.analytics_gpu_id != stream.analytics_gpu_id ||
        config.evidence_binding.source_gpu_id != stream.source_gpu_id ||
        config.evidence_binding.recorder_gpu_id != stream.recorder_gpu_id ||
        config.evidence_binding.assigned_gpu_id != stream.assigned_gpu_id ||
        config.evidence_binding.plan_sha256 != stream.spatial_roi_plan_sha256 ||
        config.evidence_binding.routing_policy != stream.routing_policy) {
        return fail(error_out,
                    "stream evidence binding does not match authenticated stream identity");
    }
    if (config.evidence_binding.assigned_shard_id != 0 ||
        config.evidence_binding.geometry_identity != geometry_json(stream.geometry) ||
        config.evidence_binding.encode_profile !=
            encode_profile_json(stream.encode_profile) ||
        config.evidence_binding.max_frames_per_stream !=
            stream.max_frames_per_stream ||
        config.evidence_binding.max_media_bytes_per_stream !=
            stream.max_media_bytes_per_stream ||
        config.evidence_binding.max_evidence_bytes_per_stream !=
            stream.max_evidence_bytes_per_stream) {
        return fail(error_out,
                    "stream evidence geometry, profile, shard, or limits do not match the authenticated stream");
    }
    static const std::vector<std::string> kExpectedFrameIdentityFields = {
        "recording_identity_token", "producer_generation", "logical_stream_id",
        "recording_frame_id", "roi_stream_frame_index"};
    if (stream.stream_id != stream.logical_stream_id || stream.env_key.empty() ||
        stream.frame_identity_key_fields != kExpectedFrameIdentityFields ||
        stream.roi_stream_frame_index_mode != "dense_one_based" ||
        stream.recording_frame_id_source != "parent_camera_recording" ||
        stream.encode_profile.profile_id.empty() ||
        stream.codec != stream.encode_profile.codec ||
        stream.tuning != stream.encode_profile.tuning ||
        stream.encode_fps != stream.encode_profile.frame_rate ||
        stream.gop != stream.encode_profile.gop_length ||
        stream.rate_control_mode != stream.encode_profile.rate_control_mode ||
        stream.quality_value != stream.encode_profile.quality_value ||
        stream.expected_shard_gpu_ids !=
            std::vector<int>{stream.recorder_gpu_id}) {
        return fail(error_out,
                    "stream identity, frame policy, encode profile, or shard projection is inconsistent");
    }
    if (config.evidence_binding.expected_artifacts.size() != stream.artifacts.size()) {
        return fail(error_out, "stream artifact binding has a different artifact count");
    }
    for (const auto& [kind, artifact] : stream.artifacts) {
        const auto found = config.evidence_binding.expected_artifacts.find(kind);
        if (found == config.evidence_binding.expected_artifacts.end() ||
            found->second != artifact.relative_path) {
            return fail(error_out,
                        "stream artifact binding is not the exact contract artifact set");
        }
    }
    const auto& runtime_paths = config.runtime_directory->socket_paths();
    if (std::find(runtime_paths.begin(), runtime_paths.end(), stream.socket_path) ==
        runtime_paths.end()) {
        return fail(error_out,
                    "stream socket path is not one of the shared authenticated runtime endpoints");
    }
    if (stream.assigned_gpu_id < 0 || stream.source_gpu_id < 0 ||
        stream.recorder_gpu_id < 0 || stream.analytics_gpu_id < 0) {
        return fail(error_out, "stream GPU assignments must be non-negative");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::Start(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    try {
        if (state_ == State::kFailed) {
            const std::string retained = first_failure();
            return fail(error_out, retained.empty() ? "stream core has failed"
                                                     : retained);
        }
        if (state_ == State::kConstructed &&
            admission_stopped_.load(std::memory_order_acquire)) {
            return fail(error_out,
                        "stream admission was stopped before recorder start");
        }
        if (state_ != State::kConstructed) {
            return true;
        }
        const std::string leaf = socket_leaf(config_.stream.socket_path);
        ipc::SpatialRoiUnixSocketTransportConfig transport_config;
        transport_config.expected_peer_pid = config_.expected_producer_pid;
        transport_config.expected_peer_uid = config_.expected_producer_uid;
        transport_config.write_timeout = config_.ipc_response_timeout;
        std::string listener_error;
        listener_ = ipc::SpatialRoiUnixSocketListener::Create(
            config_.runtime_directory->borrowed_directory_fd(),
            static_cast<dev_t>(config_.runtime_directory->device()),
            static_cast<ino_t>(config_.runtime_directory->inode()),
            config_.stream.socket_path,
            leaf,
            std::move(transport_config),
            &listener_error);
        if (!listener_) {
            return latch_failure(listener_error.empty() ? "stream socket bind failed"
                                                         : listener_error,
                                 error_out);
        }
        state_ = State::kBound;
        return true;
    } catch (const std::exception& ex) {
        return latch_failure(exception_text("stream socket bind", ex), error_out);
    } catch (...) {
        return latch_failure(unknown_exception_text("stream socket bind"), error_out);
    }
}

SpatialRoiCameraRecorderReadinessStatus
SpatialRoiConcreteCameraRecorderStreamCore::PollReadiness(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == State::kFailed) {
        if (error_out) *error_out = first_failure();
        return SpatialRoiCameraRecorderReadinessStatus::kFailed;
    }
    if (state_ == State::kNegotiated || state_ == State::kEofObserved ||
        state_ == State::kDrained || state_ == State::kFinalized) {
        return SpatialRoiCameraRecorderReadinessStatus::kReady;
    }
    if (state_ == State::kConstructed) {
        if (!Start(error_out)) {
            return SpatialRoiCameraRecorderReadinessStatus::kFailed;
        }
    }
    if (!listener_) {
        (void)latch_failure("stream listener is unavailable", error_out);
        return SpatialRoiCameraRecorderReadinessStatus::kFailed;
    }
    auto accepted = listener_->AcceptOneResult(config_.accept_timeout);
    if (accepted.timed_out()) {
        return SpatialRoiCameraRecorderReadinessStatus::kPending;
    }
    if (!accepted.accepted()) {
        (void)latch_failure(accepted.error.empty() ? "stream accept failed"
                                                   : accepted.error,
                            error_out);
        return SpatialRoiCameraRecorderReadinessStatus::kFailed;
    }
    transport_ = std::move(accepted.transport);
    listener_.reset();
    try {
        ipc::SpatialRoiRecorderIpcSessionConfig session_config;
        session_config.expected_stream = stream_identity(config_.stream);
        session_config.queue_capacity_frames = config_.stream.encode_queue_depth;
        session_config.response_timeout = config_.ipc_response_timeout;
        ipc::SpatialRoiRecorderIpcDispatch dispatch =
            [this](const ipc::SpatialRoiIpcFrame& frame) {
                return dispatch_frame(frame);
            };
        ipc::SpatialRoiRecorderIpcFrameOutcomeObserver observer =
            [this](const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
                   std::string* observer_error) {
                return on_transport_outcome(outcome, observer_error);
            };
        session_ = std::make_unique<ipc::SpatialRoiRecorderIpcSession>(
            *transport_, std::move(session_config), std::move(dispatch),
            std::move(observer));
        if (!session_->valid()) {
            (void)latch_failure(session_->error().empty()
                                    ? "stream IPC session construction failed"
                                    : session_->error(),
                                error_out);
            return SpatialRoiCameraRecorderReadinessStatus::kFailed;
        }
        std::string negotiation_error;
        if (!session_->Negotiate(&negotiation_error)) {
            (void)latch_failure(negotiation_error.empty()
                                    ? "stream IPC negotiation failed"
                                    : negotiation_error,
                                error_out);
            return SpatialRoiCameraRecorderReadinessStatus::kFailed;
        }
        // Authenticate the producer before creating output files or touching
        // CUDA. A producer may queue a FRAME after the recorder HELLO while
        // this bounded resource-arming step completes; the transport remains
        // the sole admission buffer until the owner thread starts.
        if (!arm_resources(error_out)) {
            (void)latch_failure(error_out && !error_out->empty()
                                    ? *error_out
                                    : "stream recorder resource arming failed",
                                error_out);
            return SpatialRoiCameraRecorderReadinessStatus::kFailed;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::kNegotiated;
        }
        session_thread_ = std::thread([this] { run_session(); });
        return SpatialRoiCameraRecorderReadinessStatus::kReady;
    } catch (const std::exception& ex) {
        latch_failure(exception_text("stream readiness", ex), error_out);
    } catch (...) {
        latch_failure(unknown_exception_text("stream readiness"), error_out);
    }
    return SpatialRoiCameraRecorderReadinessStatus::kFailed;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::arm_resources(
    std::string* error_out)
{
    if (!config_.artifact_root || !config_.artifact_root->valid()) {
        return fail(error_out, "stream artifact authority is unavailable");
    }
    encoder::SpatialRoiLosslessEncoderArtifactBundle artifacts;
    artifacts.artifact_root = config_.artifact_root;
    for (const char* kind : kEncoderArtifacts) {
        const auto path = evidence_binding_.expected_artifacts.find(kind);
        if (path == evidence_binding_.expected_artifacts.end()) {
            return fail(error_out, std::string("missing contract artifact: ") + kind);
        }
        std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
        std::string artifact_error;
        if (!config_.artifact_root->CreateFile(path->second, &file,
                                               &artifact_error)) {
            return fail(error_out,
                        artifact_error.empty()
                            ? std::string("could not create artifact: ") + kind
                            : artifact_error);
        }
        if (std::string(kind) == "video") artifacts.video = std::move(file);
        else if (std::string(kind) == "metadata") artifacts.metadata_csv = std::move(file);
        else if (std::string(kind) == "keyframes") artifacts.keyframes_json = std::move(file);
        else artifacts.finalization_json = std::move(file);
    }

    SpatialRoiRecorderEvidenceWriterConfig evidence_config;
    evidence_config.artifact_root = config_.artifact_root;
    evidence_config.evidence_relative_path =
        evidence_binding_.expected_artifacts.at("evidence");
    evidence_config.manifest_relative_path =
        evidence_binding_.expected_artifacts.at("evidence_manifest");
    evidence_config.max_frames = static_cast<std::size_t>(
        evidence_binding_.max_frames_per_stream);
    std::unique_ptr<SpatialRoiRecorderEvidenceWriter> evidence_writer;
    if (!SpatialRoiRecorderEvidenceWriter::Open(
            std::move(evidence_config), evidence_binding_, &evidence_writer,
            error_out)) {
        return false;
    }
    evidence_writer_ = std::move(evidence_writer);
    const ipc::SpatialRoiIpcStreamIdentity expected = stream_identity(config_.stream);
    const std::size_t max_frames = static_cast<std::size_t>(
        evidence_binding_.max_frames_per_stream);
    std::size_t journal_pending_bound = 0;
    if (!derive_spatial_roi_frame_journal_pending_bound(
            max_frames,
            static_cast<std::size_t>(config_.stream.encode_queue_depth),
            &journal_pending_bound,
            error_out)) {
        return false;
    }
    journal_ = std::make_unique<SpatialRoiRecorderFrameJournal>(
        *evidence_writer_, expected, max_frames, journal_pending_bound);
    if (!journal_->valid()) {
        return fail(error_out, journal_->error());
    }

    ipc::SpatialRoiRecorderCudaDetachConfig detach_config;
    detach_config.expected_stream = expected;
    detach_config.recorder_gpu_id = config_.stream.recorder_gpu_id;
    detach_config.expected_source_gpu_id = config_.stream.source_gpu_id;
    detach_config.expected_assigned_shard_id = 0;
    detach_config.expected_geometry = stream_geometry(config_.stream);
    detach_config.slot_count = config_.stream.detach_pool_frames;
    detach_config.max_pool_bytes = config_.stream.max_detach_pool_bytes;
    detach_config.operation_timeout_ms = config_.stream.operation_timeout_ms;
    detach_pool_ = std::make_unique<ipc::SpatialRoiRecorderCudaDetachPool>(
        std::move(detach_config));
    if (!detach_pool_->valid()) {
        return fail(error_out, detach_pool_->error());
    }

    encoder::SpatialRoiLosslessEncoderConfig encoder_config;
    encoder_config.stream = expected;
    encoder_config.geometry = stream_geometry(config_.stream);
    encoder_config.source_gpu_id = config_.stream.source_gpu_id;
    encoder_config.recorder_gpu_id = config_.stream.recorder_gpu_id;
    encoder_config.assigned_shard_id = 0;
    encoder_config.profile_id = config_.stream.encode_profile.profile_id;
    encoder_config.codec = config_.stream.codec;
    encoder_config.preset = config_.stream.encode_profile.preset;
    encoder_config.tuning = config_.stream.tuning;
    encoder_config.lossless = config_.stream.encode_profile.lossless;
    encoder_config.rate_control_mode = config_.stream.rate_control_mode;
    encoder_config.quality_value = config_.stream.quality_value;
    encoder_config.gop_length = config_.stream.gop;
    encoder_config.aq = config_.stream.encode_profile.aq;
    encoder_config.temporal_aq = config_.stream.encode_profile.temporal_aq;
    encoder_config.lookahead = config_.stream.encode_profile.lookahead;
    encoder_config.lookahead_depth =
        config_.stream.encode_profile.lookahead_depth;
    encoder_config.fps = config_.stream.encode_fps;
    encoder_config.input_format = config_.stream.encode_profile.input_format;
    encoder_config.encoded_format = config_.stream.encode_profile.encoded_format;
    encoder_config.no_resize = config_.stream.encode_profile.no_resize;
    encoder_config.luma_preserved_exactly =
        config_.stream.encode_profile.luma_preserved_exactly;
    encoder_config.neutral_chroma_value =
        config_.stream.encode_profile.neutral_chroma_value;
    encoder_config.queue_capacity = config_.stream.encode_queue_depth;
    encoder_config.max_queue_bytes = config_.stream.max_queue_bytes;
    encoder_config.writer_queue_max_packets =
        static_cast<std::size_t>(config_.stream.writer_queue_max_packets);
    encoder_config.writer_queue_max_bytes =
        static_cast<std::size_t>(config_.stream.writer_queue_max_bytes);
    encoder_config.operation_timeout_ms = config_.stream.operation_timeout_ms;
    encoder_config.artifacts = std::move(artifacts);
    encoder_config.max_frames_per_stream = config_.stream.max_frames_per_stream;
    encoder_config.max_media_bytes_per_stream =
        config_.stream.max_media_bytes_per_stream;
    encoder_config.frame_result_callback =
        [this](const encoder::SpatialRoiLosslessFrameResult& result,
               std::string* result_error) {
            return on_encoder_result(result, result_error);
        };
    try {
        encoder_ = std::make_unique<encoder::SpatialRoiLosslessEncoder>(
            std::move(encoder_config));
    } catch (const std::exception& ex) {
        return fail(error_out, exception_text("lossless encoder construction", ex));
    } catch (...) {
        return fail(error_out, unknown_exception_text("lossless encoder construction"));
    }
    if (!encoder_ || !encoder_->valid()) {
        return fail(error_out, encoder_ ? encoder_->error()
                                        : "lossless encoder is unavailable");
    }
    return true;
}

ipc::SpatialRoiRecorderIpcDispatchResult
SpatialRoiConcreteCameraRecorderStreamCore::dispatch_frame(
    const ipc::SpatialRoiIpcFrame& frame)
{
    ipc::SpatialRoiRecorderIpcDispatchResult result;
    try {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        if (admission_stopped_ || !detach_pool_ || !encoder_ ||
            !detach_pool_->valid() || !encoder_->valid()) {
            result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected;
            result.detach_status = "stopped";
            result.source_release_safe = true;
            result.reason = admission_stopped_ ? "stream admission stopped"
                                               : "stream recorder is not ready";
            return result;
        }
        auto detached = detach_pool_->TryDetach(frame);
        result.detach_status =
            ipc::spatial_roi_recorder_detach_status_name(detached.status);
        result.source_release_safe = detached.source_release_safe();
        if (detached.detached()) {
            std::string encoder_error;
            if (encoder_->Enqueue(std::move(detached.frame), frame.descriptor,
                                  &encoder_error)) {
                result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kEnqueued;
                result.detach_status = "detached";
                result.source_release_safe = true;
                result.reason.clear();
                return result;
            }
            result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected;
            result.source_release_safe = true;
            result.detach_status = "detached";
            result.reason = bounded_reason(
                encoder_error, "encoder admission rejected frame");
            return result;
        }
        if (detached.status == ipc::SpatialRoiRecorderDetachStatus::kSourceQuarantined ||
            !detached.source_release_safe()) {
            result.status =
                ipc::SpatialRoiRecorderIpcDispatchStatus::kSourceOwnershipUncertain;
            result.detach_status = "source_quarantined";
            result.source_release_safe = false;
            result.reason = bounded_reason(
                detached.error, "source ownership is uncertain");
            return result;
        }
        result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected;
        result.source_release_safe = true;
        result.reason = bounded_reason(
            detached.error, "recorder detach rejected frame");
        return result;
    } catch (const std::exception& ex) {
        result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected;
        result.detach_status = "cuda_error";
        result.source_release_safe = true;
        result.reason = bounded_reason(exception_text("frame dispatch", ex),
                                       "frame dispatch failed");
        return result;
    } catch (...) {
        result.status = ipc::SpatialRoiRecorderIpcDispatchStatus::kRejected;
        result.detach_status = "cuda_error";
        result.source_release_safe = true;
        result.reason = "frame dispatch failed";
        return result;
    }
}

bool SpatialRoiConcreteCameraRecorderStreamCore::on_encoder_result(
    const encoder::SpatialRoiLosslessFrameResult& result,
    std::string* error_out)
{
    try {
        if (!journal_) {
            return fail(error_out, "frame journal is unavailable");
        }
        return journal_->AcceptEncoderResult(result, error_out);
    } catch (const std::exception& ex) {
        return fail(error_out, exception_text("encoder result journal", ex));
    } catch (...) {
        return fail(error_out, unknown_exception_text("encoder result journal"));
    }
}

bool SpatialRoiConcreteCameraRecorderStreamCore::on_transport_outcome(
    const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
    std::string* error_out)
{
    try {
        if (!journal_) {
            return fail(error_out, "frame journal is unavailable");
        }
        SpatialRoiRecorderFrameTransportOutcome canonical;
        if (!make_spatial_roi_recorder_frame_transport_outcome(
                outcome, &canonical, error_out)) {
            return false;
        }
        return journal_->AcceptTransportOutcome(canonical, error_out);
    } catch (const std::exception& ex) {
        return fail(error_out, exception_text("transport outcome journal", ex));
    } catch (...) {
        return fail(error_out,
                    unknown_exception_text("transport outcome journal"));
    }
}

void SpatialRoiConcreteCameraRecorderStreamCore::run_session() noexcept
{
    ipc::SpatialRoiRecorderIpcSessionResult result;
    try {
        result = session_ ? session_->Run()
                          : ipc::SpatialRoiRecorderIpcSessionResult{};
    } catch (const std::exception& ex) {
        result.status = ipc::SpatialRoiRecorderIpcSessionStatus::kFatal;
        try {
            result.error = exception_text("stream IPC session", ex);
        } catch (...) {
            result.error.clear();
        }
    } catch (...) {
        result.status = ipc::SpatialRoiRecorderIpcSessionStatus::kFatal;
        try {
            result.error = unknown_exception_text("stream IPC session");
        } catch (...) {
            result.error.clear();
        }
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancellation_requested_) {
            result.status = ipc::SpatialRoiRecorderIpcSessionStatus::kFatal;
            try {
                result.error = "stream session stopped by recorder lifecycle";
            } catch (...) {
                result.error.clear();
            }
        }
        session_result_ = std::move(result);
        session_result_valid_ = true;
        session_done_.store(true, std::memory_order_release);
        return;
    } catch (...) {
        // session_done_ still publishes completion. PollEof treats a completed
        // session without a retained result as fatal instead of waiting forever.
    }
    session_done_.store(true, std::memory_order_release);
}

SpatialRoiCameraRecorderEofStatus
SpatialRoiConcreteCameraRecorderStreamCore::PollEof(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (state_ == State::kFailed) {
        if (error_out) *error_out = first_failure();
        return SpatialRoiCameraRecorderEofStatus::kFailed;
    }
    if (state_ == State::kConstructed || state_ == State::kBound) {
        return SpatialRoiCameraRecorderEofStatus::kPending;
    }
    if (state_ == State::kEofObserved || state_ == State::kDrained ||
        state_ == State::kFinalized) {
        return SpatialRoiCameraRecorderEofStatus::kClean;
    }

    // Encoder and transport callbacks only stage their bounded results.  The
    // recorder's polling thread is the non-callback owner of evidence I/O, so
    // continuously retire every dense, completed prefix while acquisition is
    // live.  Deferring this work until Drain() would make the pending bound a
    // recording-length limit instead of an in-flight-work limit.
    if (journal_) {
        std::string journal_error;
        if (!journal_->DrainReady(0, &journal_error)) {
            latch_failure(
                journal_error.empty() ? "frame evidence drain failed"
                                      : journal_error,
                error_out);
            return SpatialRoiCameraRecorderEofStatus::kFailed;
        }
    }

    bool done = false;
    bool result_valid = false;
    ipc::SpatialRoiRecorderIpcSessionResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done = session_done_.load(std::memory_order_acquire);
        result_valid = done && session_result_valid_;
        if (result_valid) result = session_result_;
    }
    if (!done) return SpatialRoiCameraRecorderEofStatus::kPending;
    if (!result_valid) {
        latch_failure("stream session result could not be retained", error_out);
        return SpatialRoiCameraRecorderEofStatus::kFailed;
    }
    if (!result.clean_eof()) {
        latch_failure(result.error.empty() ? "stream IPC session failed" : result.error,
                      error_out);
        return SpatialRoiCameraRecorderEofStatus::kFailed;
    }
    state_ = State::kEofObserved;
    return SpatialRoiCameraRecorderEofStatus::kClean;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::StopAdmission(
    std::string* error_out)
{
    if (error_out) error_out->clear();

    // Publish cancellation and wake a blocked transport owner before waiting
    // for admission.  A dispatch may hold admission_mutex_ while doing the
    // bounded CUDA detach/enqueue operation; the eventfd cannot interrupt a
    // CUDA driver call, so the outer supervisor still needs a process bound.
    bool interrupt_owner = false;
    if (transport_ && session_thread_.joinable()) {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_owner = !session_done_.load(std::memory_order_acquire);
        if (interrupt_owner) cancellation_requested_ = true;
    }
    std::string shutdown_error;
    const bool shutdown_ok =
        !interrupt_owner || transport_->RequestShutdown(&shutdown_error);

    bool already_stopped = false;
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        already_stopped = admission_stopped_.exchange(true);
        if (detach_pool_) detach_pool_->Stop();
    }
    if (listener_) listener_->Close();
    if (!shutdown_ok) {
        return latch_failure(
            shutdown_error.empty() ? "stream transport shutdown request failed"
                                   : shutdown_error,
            error_out);
    } else if (transport_ && !session_thread_.joinable()) {
        transport_->Close();
    }
    if (state_ == State::kFailed) {
        if (error_out) *error_out = first_failure();
        return false;
    }
    if (already_stopped) return true;
    return true;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::Drain(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!require_not_failed("Drain", error_out)) return false;
    if (state_ == State::kDrained || state_ == State::kFinalized) return true;
    if (PollEof(error_out) != SpatialRoiCameraRecorderEofStatus::kClean) {
        return fail(error_out, "stream Drain requires clean producer EOF");
    }
    if (!StopAdmission(error_out)) return false;
    if (session_thread_.joinable()) session_thread_.join();
    if (!session_result_.clean_eof()) {
        return latch_failure(session_result_.error.empty()
                                 ? "stream session did not drain cleanly"
                                 : session_result_.error,
                             error_out);
    }
    if (journal_ && !journal_->Flush(error_out)) {
        return latch_failure(journal_->error(), error_out);
    }
    drained_ = true;
    state_ = State::kDrained;
    return true;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::Finalize(std::string* error_out)
{
    if (error_out) error_out->clear();
    if (!require_not_failed("Finalize", error_out)) return false;
    if (state_ == State::kFinalized) return true;
    if (!Drain(error_out)) return false;
    if (!encoder_ || !encoder_->Finalize()) {
        return latch_failure(encoder_ ? encoder_->error()
                                      : "lossless encoder is unavailable",
                             error_out);
    }
    // No detached recorder slot or source mapping can be referenced after the
    // encoder owner has drained. Close the bounded session import cache while
    // the producer process and its export pool are still alive; a cleanup
    // failure is terminal and must prevent a successful recorder receipt.
    std::string import_cleanup_error;
    if (!detach_pool_ ||
        !detach_pool_->CloseCachedImports(&import_cleanup_error)) {
        return latch_failure(
            import_cleanup_error.empty()
                ? "recorder CUDA IPC import cache cleanup failed"
                : import_cleanup_error,
            error_out);
    }
    const auto terminal = encoder_->terminal_snapshot();
    if (!terminal || !terminal->successful) {
        return latch_failure("lossless encoder terminal snapshot is not successful",
                             error_out);
    }
    if (!journal_ || !journal_->Flush(error_out) || !journal_->final_ready()) {
        return latch_failure(journal_ ? journal_->error()
                                      : "frame journal is unavailable",
                             error_out);
    }

    SpatialRoiRecorderVideoSanityRequest probe_request;
    probe_request.artifact_root = config_.artifact_root;
    probe_request.video_relative_path =
        evidence_binding_.expected_artifacts.at("video");
    probe_request.encoded_width = config_.stream.geometry.encoded_raster.width;
    probe_request.encoded_height = config_.stream.geometry.encoded_raster.height;
    probe_request.expected_frame_count = terminal->counts.encoded_frames;
    probe_request.max_media_bytes = evidence_binding_.max_media_bytes_per_stream;
    probe_request.expected_frame_rate =
        static_cast<double>(config_.stream.encode_fps);
    probe_request.timeout = config_.video_probe_timeout;
    std::unique_ptr<SpatialRoiRecorderVideoSanityResult> probe;
    if (!probe_spatial_roi_recorder_video_sanity(probe_request, &probe,
                                                 error_out) || !probe) {
        return latch_failure(error_out && !error_out->empty()
                                 ? *error_out
                                 : "descriptor-bound video sanity probe failed",
                             error_out);
    }
    SpatialRoiRecorderTerminalCandidateSidecarConfig sidecar_config;
    sidecar_config.artifact_root = config_.artifact_root;
    sidecar_config.binding = &evidence_binding_;
    sidecar_config.video_sanity = probe.get();
    SpatialRoiRecorderTerminalCandidateSidecarResult sidecars;
    if (!write_spatial_roi_recorder_terminal_candidate_sidecars(
            sidecar_config, &sidecars, error_out)) {
        return latch_failure(error_out && !error_out->empty()
                                 ? *error_out
                                 : "terminal candidate sidecar publication failed",
                             error_out);
    }
    video_sanity_result_ = std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>(
        std::move(probe));

    SpatialRoiRecorderFinalizeRequest request;
    request.terminal_state = "complete";
    request.encoder_terminal_snapshot = terminal;
    request.video_sanity_result = video_sanity_result_;
    request.artifacts.reserve(kFinalizeArtifactCount);
    for (const char* kind : kFinalizeArtifacts) {
        request.artifacts.push_back(
            {kind, evidence_binding_.expected_artifacts.at(kind)});
    }
    nlohmann::json manifest;
    if (!evidence_writer_ ||
        !evidence_writer_->Finalize(request, &manifest, error_out)) {
        return latch_failure(evidence_writer_ ? evidence_writer_->error()
                                              : "evidence writer is unavailable",
                             error_out);
    }
    finalized_ = true;
    state_ = State::kFinalized;
    return true;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::require_not_failed(
    const char* operation,
    std::string* error_out)
{
    if (state_ != State::kFailed) return true;
    const std::string retained = first_failure();
    const std::string reason = retained.empty()
                                   ? std::string(operation) + " cannot run after failure"
                                   : retained;
    return fail(error_out, reason);
}

bool SpatialRoiConcreteCameraRecorderStreamCore::latch_failure(
    const std::string& reason,
    std::string* error_out)
{
    if (state_ != State::kFailed) {
        state_ = State::kFailed;
        std::lock_guard<std::mutex> lock(mutex_);
        first_failure_ = bounded_reason(reason, "spatial ROI stream failure");
    }

    // As in StopAdmission, wake the session owner before taking the admission
    // mutex.  This improves cancellation latency when dispatch is in flight;
    // it cannot interrupt a CUDA driver call that never returns, so teardown
    // remains subject to the process supervisor's outer bound.
    bool interrupt_owner = false;
    if (transport_ && session_thread_.joinable()) {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_owner = !session_done_.load(std::memory_order_acquire);
        if (interrupt_owner) cancellation_requested_ = true;
    }
    if (interrupt_owner) {
        (void)transport_->RequestShutdown(nullptr);
    }
    {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        admission_stopped_ = true;
        if (detach_pool_) detach_pool_->Stop();
    }
    if (listener_) listener_->Close();
    if (!interrupt_owner && transport_ && !session_thread_.joinable()) {
        transport_->Close();
    }
    if (error_out) *error_out = first_failure();
    return false;
}

std::string SpatialRoiConcreteCameraRecorderStreamCore::bounded_reason(
    const std::string& value,
    const char* fallback)
{
    std::string result = value.empty()
                             ? (fallback ? std::string(fallback)
                                         : std::string("spatial ROI stream failure"))
                             : value;
    if (result.size() > kMaximumReasonBytes) result.resize(kMaximumReasonBytes);
    for (char& byte : result) {
        const unsigned char value_byte = static_cast<unsigned char>(byte);
        if (value_byte < 0x20 || value_byte == 0x7f) byte = '?';
    }
    return result;
}

ipc::SpatialRoiIpcStreamIdentity
SpatialRoiConcreteCameraRecorderStreamCore::stream_identity(
    const session::spatial_roi::SpatialRoiRecorderStreamView& stream)
{
    ipc::SpatialRoiIpcStreamIdentity value;
    value.recording_id = stream.recording_id;
    value.recording_identity_token = stream.recording_identity_token;
    value.producer_generation = stream.producer_generation;
    value.camera_id = stream.camera_id;
    value.camera_serial = stream.camera_serial;
    value.roi_id = stream.roi_id;
    value.region_id = stream.region_id;
    value.arena_group_id = stream.arena_group_id;
    value.arena_id = stream.arena_id;
    value.logical_stream_id = stream.logical_stream_id;
    value.spatial_roi_plan_sha256 = stream.spatial_roi_plan_sha256;
    return value;
}

ipc::SpatialRoiRecorderCudaDetachGeometry
SpatialRoiConcreteCameraRecorderStreamCore::stream_geometry(
    const session::spatial_roi::SpatialRoiRecorderStreamView& stream)
{
    ipc::SpatialRoiRecorderCudaDetachGeometry value;
    value.native_raster = stream.geometry.native_raster;
    value.content_rect = stream.geometry.content_rect;
    value.encoded_raster = stream.geometry.encoded_raster;
    value.encoded_content_rect = stream.geometry.encoded_content_rect;
    value.padding = stream.geometry.padding;
    value.routing_policy = stream.routing_policy;
    return value;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::started() const noexcept
{
    return state_ != State::kConstructed;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::negotiated() const noexcept
{
    return state_ == State::kNegotiated || state_ == State::kEofObserved ||
           state_ == State::kDrained || state_ == State::kFinalized;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::clean_eof() const noexcept
{
    return state_ == State::kEofObserved || state_ == State::kDrained ||
           state_ == State::kFinalized;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::completed() const noexcept
{
    return state_ == State::kFinalized;
}

bool SpatialRoiConcreteCameraRecorderStreamCore::failed() const noexcept
{
    return state_ == State::kFailed;
}

std::string SpatialRoiConcreteCameraRecorderStreamCore::first_failure() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return first_failure_;
}

}  // namespace orange::spatial_roi::recording
