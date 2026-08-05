#include "external_recorder_contract_utils.h"
#include "external_recorder_ipc_protocol.h"
#include "external_recorder_supervisor.h"
#include "encoder_pipeline.h"
#include "video_capture.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

CameraParams make_camera(const std::string& serial,
                         int analytics_gpu,
                         std::vector<int> shard_gpus)
{
    CameraParams camera{};
    camera.camera_serial = serial;
    camera.gpu_id = analytics_gpu;
    camera.width = 4512;
    camera.height = 4512;
    camera.frame_rate = 100;
    camera.recording.encode.codec = "hevc";
    camera.recording.encode.preset = "p1";
    camera.recording.encode.tuning = "ll";
    camera.recording.encode.gop_length = 25;
    camera.recording.strategy.mode = "split_gop";
    camera.recording.strategy.split_gop.enabled = true;
    camera.recording.strategy.split_gop.encoder_gpu_ids = std::move(shard_gpus);
    return camera;
}

void parses_ipc_protocol_hello_lines()
{
    const std::string recorder_hello =
        orange::external_recorder::ipc::build_recorder_hello_line(
            "session 001",
            "2010095",
            100,
            25);
    orange::external_recorder::ipc::HelloFields recorder_fields;
    require(orange::external_recorder::ipc::parse_recorder_hello_line(
                recorder_hello,
                &recorder_fields),
            "recorder hello should parse");
    require(recorder_fields.protocol == orange::external_recorder::ipc::kProtocolName,
            "recorder hello protocol mismatch");
    require(recorder_fields.version == orange::external_recorder::ipc::kProtocolVersion,
            "recorder hello version mismatch");
    require(recorder_fields.role == "recorder",
            "recorder hello role mismatch");
    std::string identity_error;
    require(orange::external_recorder::ipc::validate_recording_config_identity(
                recorder_fields, 100, 25, &identity_error),
            "recorder identity should validate: " + identity_error);

    const std::string client_hello =
        orange::external_recorder::ipc::build_client_hello_line(
            "2010095",
            "session 001",
            "2010095_crop",
            "orange crop",
            100,
            1);
    orange::external_recorder::ipc::HelloFields client_fields;
    require(orange::external_recorder::ipc::parse_client_hello_line(
                client_hello,
                &client_fields),
            "client hello should parse");
    require(client_fields.role == "orange_crop",
            "client hello role should be tokenized");
    require(client_fields.session_id == "session_001",
            "client hello session should be tokenized");
    require(orange::external_recorder::ipc::validate_recording_config_identity(
                client_fields, 100, 1, &identity_error),
            "client identity should validate: " + identity_error);

    const std::string recorder_status =
        orange::external_recorder::ipc::build_recorder_status_line(
            "session 001",
            "2010095 crop",
            "running",
            12,
            300,
            299,
            280,
            0,
            false);
    orange::external_recorder::ipc::RecorderStatusFields status_fields;
    require(orange::external_recorder::ipc::parse_recorder_status_line(
                recorder_status,
                &status_fields),
            "recorder status should parse");
    require(status_fields.protocol == orange::external_recorder::ipc::kProtocolName,
            "recorder status protocol mismatch");
    require(status_fields.version == orange::external_recorder::ipc::kProtocolVersion,
            "recorder status version mismatch");
    require(status_fields.session_id == "session_001",
            "recorder status session should be tokenized");
    require(status_fields.stream_id == "2010095_crop",
            "recorder status stream should be tokenized");
    require(status_fields.heartbeat_sequence == 12,
            "recorder status heartbeat mismatch");
    require(status_fields.frames_received == 300,
            "recorder status frames received mismatch");
    require(status_fields.acks_sent == 299,
            "recorder status ACK count mismatch");
    require(status_fields.frames_encoded == 280,
            "recorder status encoded count mismatch");
    require(!status_fields.worker_failed,
            "recorder status worker failed flag mismatch");

    const std::string client_drain_control =
        orange::external_recorder::ipc::build_client_control_line(
            "2010095",
            "session 001",
            "2010095 crop",
            "orange crop",
            orange::external_recorder::ipc::kClientControlDrain,
            "recording draining");
    orange::external_recorder::ipc::ClientControlFields control_fields;
    require(orange::external_recorder::ipc::parse_client_control_line(
                client_drain_control,
                &control_fields),
            "client control should parse");
    require(control_fields.protocol == orange::external_recorder::ipc::kProtocolName,
            "client control protocol mismatch");
    require(control_fields.role == "orange_crop",
            "client control role should be tokenized");
    require(control_fields.session_id == "session_001",
            "client control session should be tokenized");
    require(control_fields.stream_id == "2010095_crop",
            "client control stream should be tokenized");
    require(control_fields.command == orange::external_recorder::ipc::kClientControlDrain,
            "client drain control command mismatch");
    require(control_fields.reason == "recording_draining",
            "client drain control reason should be tokenized");

    const std::string client_finalize_control =
        orange::external_recorder::ipc::build_client_control_line(
            "2010095",
            "session 001",
            "2010095 crop",
            "orange crop",
            orange::external_recorder::ipc::kClientControlFinalize,
            "recording drained");
    require(orange::external_recorder::ipc::parse_client_control_line(
                client_finalize_control,
                &control_fields),
            "client finalize control should parse");
    require(control_fields.command == orange::external_recorder::ipc::kClientControlFinalize,
            "client finalize control command mismatch");
    require(control_fields.reason == "recording_drained",
            "client finalize control reason should be tokenized");

    orange::external_recorder::ipc::HelloFields bad_fields;
    require(!orange::external_recorder::ipc::parse_client_hello_line(
                "CLIENT_HELLO protocol=wrong version=1 role=orange",
                &bad_fields),
            "invalid protocol should fail");
}

void rejects_gop_25_30_contract_mismatch_before_unbounded_buffering()
{
    const std::string sender_hello =
        orange::external_recorder::ipc::build_client_hello_line(
            "2010095",
            "session_mismatch",
            "2010095",
            "orange_full_frame",
            100,
            30);
    orange::external_recorder::ipc::HelloFields sender_fields;
    require(orange::external_recorder::ipc::parse_client_hello_line(
                sender_hello, &sender_fields),
            "30-frame sender hello should parse");
    std::string error;
    require(!orange::external_recorder::ipc::validate_recording_config_identity(
                sender_fields, 100, 25, &error),
            "25-frame recorder must reject a 30-frame sender during HELLO");
    require(error.find("resolved_gop_length mismatch") != std::string::npos,
            "HELLO mismatch should identify the GOP disagreement");

    for (uint64_t frame_id = 1; frame_id <= 25; ++frame_id) {
        const uint64_t zero_based = frame_id - 1;
        require(orange::external_recorder::ipc::validate_frame_grouping(
                    frame_id,
                    zero_based / 30,
                    static_cast<uint32_t>(zero_based % 30),
                    25,
                    &error),
                "25/30 descriptor mismatch must not appear before frame 26");
    }
    require(!orange::external_recorder::ipc::validate_frame_grouping(
                26, 0, 25, 25, &error),
            "descriptor validation must reject the first mismatched boundary frame");
    require(error.find("recording_frame_id=26") != std::string::npos,
            "descriptor mismatch should identify frame 26");

    orange::external_recorder::ipc::SubmittedFrameIdentityRegistry identities;
    require(identities.note({26, 0, 25}, &error),
            "submission identity should accept the sender's canonical assignment");
    require(identities.note({27, 1, 0}, &error),
            "submission identity should accept the following GOP assignment");
    orange::external_recorder::ipc::SubmittedFrameIdentity returned_identity;
    require(identities.consume(27, &returned_identity, &error),
            "NVENC completion may arrive out of submission order");
    require(returned_identity.recording_frame_id == 27 &&
                returned_identity.gop_index == 1 &&
                returned_identity.frame_index_within_gop == 0,
            "completion must preserve the exact submitted identity");
    require(identities.consume(26, &returned_identity, &error),
            "NVENC output timestamp should resolve the earlier submission");
    require(returned_identity.gop_index == 0 &&
                returned_identity.frame_index_within_gop == 25,
            "merger must not recompute GOP identity from its own GOP length");
    require(identities.empty(),
            "every emitted packet should consume one submitted identity");
    require(!identities.consume(26, &returned_identity, &error),
            "a duplicate NVENC completion must fail closed");
    require(error.find("unknown or already-consumed") != std::string::npos,
            "duplicate completion should identify the missing registry entry");
    require(identities.note({28, 1, 1}, &error),
            "new submitted identity should be accepted");
    require(!identities.note({28, 99, 99}, &error),
            "duplicate submitted frame identity must fail closed");

    require(orange::external_recorder::ipc::validate_pending_gop_budget(
                8, 268435456, 8, 268435456, &error),
            "pending budget should allow its exact configured boundary");
    require(!orange::external_recorder::ipc::validate_pending_gop_budget(
                9, 1, 8, 268435456, &error),
            "pending budget must reject a ninth GOP");
    require(!orange::external_recorder::ipc::validate_pending_gop_budget(
                1, 268435457, 8, 268435456, &error),
            "pending budget must reject bytes above the hard limit");
    require(orange::external_recorder::ipc::validate_pending_frontier_age(
                1, 2000, 2000, &error),
            "frontier-age budget should allow its exact configured boundary");
    require(!orange::external_recorder::ipc::validate_pending_frontier_age(
                1, 2001, 2000, &error),
            "frontier-age budget must reject a stalled pending GOP");
    require(error.find("frontier age") != std::string::npos,
            "frontier-age rejection should identify the stalled frontier");
    require(orange::external_recorder::ipc::validate_pending_frontier_age(
                0, 60000, 2000, &error),
            "an empty pending frontier must not fail based on elapsed wall time");
}

void materializes_contract_and_supervisor_plan()
{
    const nlohmann::json wrapped = {
        {"external_recorder_contract", {
            {"schema_id", "orange.external_recorder.contract"},
            {"schema_version", 1},
            {"mode", "diagnostic_ipc_v1"}
        }}
    };
    const nlohmann::json extracted =
        orange::external_recorder::ExtractExternalRecorderContractObject(wrapped);
    require(extracted.value("schema_id", "") == "orange.external_recorder.contract",
            "wrapped contract extraction failed");

    CameraParams cameras[2] = {
        make_camera("2010095", 5, {5, 6}),
        make_camera("2010096", 7, {7, 8}),
    };
    CameraEachSelect selected[2]{};
    selected[0].record = true;
    selected[1].record = true;

    nlohmann::json overrides = {
        {"artifact_root", "{recording_folder}/external_recorder"},
        {"streams", {
            {"2010096", {
                {"mp4", "{recording_folder}/custom_{recording_id}.mp4"}
            }}
        }}
    };

    orange::external_recorder::CameraContractMaterializationInput input;
    input.contract_config = &overrides;
    input.recording_folder = "/tmp/orange_contract_utils_test";
    input.recording_id = "session_001";
    input.cameras_params = cameras;
    input.cameras_select = selected;
    input.num_cameras = 2;

    ResolvedRecordingConfig resolved[2];
    resolved[0].encode = cameras[0].recording.encode;
    resolved[1].encode = cameras[1].recording.encode;
    resolved[0].encode.gop_length = 30;
    resolved[0].encode.preset = "p3";
    input.resolved_recording_configs = resolved;
    input.num_resolved_recording_configs = 2;

    const nlohmann::json contract =
        orange::external_recorder::MaterializeExternalRecorderContractForCameras(input);
    require(contract.value("schema_id", "") == "orange.external_recorder.contract",
            "contract schema_id mismatch");
    require(contract.value("artifact_root", "") ==
                "/tmp/orange_contract_utils_test/external_recorder",
            "artifact_root template was not expanded");
    require(contract.value("require_status", false),
            "materialized contract should require recorder status sidecars");
    require(contract.value("require_status_runtime", false),
            "materialized contract should require supervised runtime status");
    require(contract.value("require_storage_preflight", false),
            "materialized contract should require storage preflight telemetry");
    require(contract.value("require_protocol_hello", false),
            "materialized contract should require IPC protocol hello telemetry");
    require(contract.value("preserve_shard_mp4s", true) == false,
            "materialized contract should delete shard MP4s by default after merge");
    require(contract["streams"].size() == 2, "expected two contract streams");
    require(contract["streams"]["2010095"].value("routing_policy", "") == "gop_modulo",
            "2010095 should route by GOP modulo");
    require(contract["streams"]["2010095"].value("stream_kind", "") == "full_frame",
            "2010095 stream kind should default to full_frame");
    require(contract["streams"]["2010095"].value("output_kind", "") == "full",
            "2010095 output kind should default to full");
    require(contract["streams"]["2010095"].value("camera_serial", "") == "2010095",
            "2010095 camera serial should default to real serial");
    require(contract["streams"]["2010095"].value("env_key", "") == "2010095",
            "2010095 env key should default to serial");
    require(contract["streams"]["2010095"]["expected_shard_gpu_ids"] == nlohmann::json::array({5, 6}),
            "2010095 shard GPU ids mismatch");
    require(contract["streams"]["2010096"].value("mp4", "") ==
                "/tmp/orange_contract_utils_test/custom_session_001.mp4",
            "stream override path template was not expanded");
    require(contract["streams"]["2010095"].value("status_json", "") ==
                "/tmp/orange_contract_utils_test/external_recorder/Cam2010095_external_status.json",
            "2010095 status sidecar path mismatch");
    require(contract["streams"]["2010095"].value("metadata_csv", "") ==
                "/tmp/orange_contract_utils_test/external_recorder/Cam2010095_external_meta.csv",
            "2010095 frame metadata sidecar path mismatch");
    require(contract["streams"]["2010096"].value("status_json", "") ==
                "/tmp/orange_contract_utils_test/external_recorder/Cam2010096_external_status.json",
            "2010096 status sidecar path mismatch");
    require(contract["streams"]["2010095"].value("gop", 0) == 30,
            "materialization must use the frozen resolved GOP");
    require(contract["streams"]["2010095"].value("preset", "") == "p3",
            "materialization must use the frozen resolved encoder profile");
    require(contract["streams"]["2010095"].value("recording_config_source", "") ==
                "resolved_recording_config",
            "materialization should identify the frozen runtime source");
    require(contract["streams"]["2010095"].value(
                "recording_config_fingerprint", "") ==
                orange::external_recorder::ipc::build_recording_config_fingerprint(100, 30),
            "materialized fingerprint must bind frame rate and resolved GOP");
    require(contract["streams"]["2010095"].value(
                "max_pending_frontier_age_ms", 0) == 2000,
            "materialized contract must bound pending-frontier age");
    require(contract["streams"]["2010095"].value(
                "max_writer_queue_packets", 0) == 512,
            "materialized contract must bound writer packets");
    require(contract["streams"]["2010095"].value(
                "max_writer_queue_bytes", 0) == 134217728,
            "materialized contract must bound writer bytes");

    orange::external_recorder::SupervisorPlanOptions plan_options;
    orange::external_recorder::SupervisorPlan plan;
    std::string error;
    require(orange::external_recorder::BuildSupervisorPlanFromContract(
                contract,
                plan_options,
                &plan,
                &error),
            "supervisor plan failed: " + error);
    require(plan.streams.size() == 2, "expected two supervisor streams");
    require(plan.streams[0].gop == 30,
            "supervisor plan must preserve the frozen resolved GOP");
    require(plan.streams[0].max_pending_gops == 8,
            "supervisor plan must carry the hard pending-GOP limit");
    require(plan.streams[0].max_pending_bytes == 268435456,
            "supervisor plan must carry the hard pending-byte limit");
    require(plan.streams[0].max_pending_frontier_age_ms == 2000,
            "supervisor plan must carry the pending-frontier age limit");
    require(plan.streams[0].max_writer_queue_packets == 512,
            "supervisor plan must carry the writer packet limit");
    require(plan.streams[0].max_writer_queue_bytes == 134217728,
            "supervisor plan must carry the writer byte limit");
    require(plan.streams[0].stream_kind == "full_frame",
            "supervisor stream kind should default to full_frame");
    require(plan.streams[0].output_kind == "full",
            "supervisor output kind should default to full");
    require(plan.streams[0].env_key == plan.streams[0].camera_serial,
            "supervisor env key should default to real serial");
    require(!plan.preserve_shard_mp4s,
            "supervisor plan should carry default shard MP4 retention policy");
}

void preserves_configured_recording_control_when_input_is_default()
{
    CameraParams cameras[1] = {
        make_camera("2010096", 7, {7, 8}),
    };
    CameraEachSelect selected[1]{};
    selected[0].record = true;

    const nlohmann::json configured_rollover = {
        {"requested", true},
        {"status", "supported"},
        {"implementation", orange::external_recorder::kExternalRecorderRollingImplementation}
    };
    nlohmann::json overrides = {
        {"recording_control", {
            {"record_for_seconds", 6},
            {"clip_seconds", 2}
        }},
        {"rollover", configured_rollover}
    };

    orange::external_recorder::CameraContractMaterializationInput input;
    input.contract_config = &overrides;
    input.recording_folder = "/tmp/orange_contract_utils_recording_control";
    input.recording_id = "session_recording_control";
    input.cameras_params = cameras;
    input.cameras_select = selected;
    input.num_cameras = 1;

    const nlohmann::json contract =
        orange::external_recorder::MaterializeExternalRecorderContractForCameras(input);
    require(contract["recording_control"]["record_for_seconds"] == 6,
            "configured record_for_seconds should be preserved");
    require(contract["recording_control"]["clip_seconds"] == 2,
            "configured clip_seconds should be preserved");
    require(contract["rollover"]["requested"] == true,
            "configured rollover should be preserved");
    require(contract.value("require_merged_mp4", true) == false,
            "rolling clips should be authoritative without a merged session MP4");
    require(contract["streams"]["2010096"]["recording_control"]["record_for_seconds"] == 6,
            "configured record_for_seconds should propagate to materialized stream");
    require(contract["streams"]["2010096"]["recording_control"]["clip_seconds"] == 2,
            "configured clip_seconds should propagate to materialized stream");

    orange::external_recorder::SupervisorPlan plan;
    std::string error;
    require(orange::external_recorder::BuildSupervisorPlanFromContract(
                contract,
                {},
                &plan,
                &error),
            "supervisor plan failed for preserved recording control: " + error);
    require(plan.streams.size() == 1, "expected one preserved-control stream");
    require(plan.streams[0].record_for_seconds == 6,
            "preserved record_for_seconds should flow into plan");
    require(plan.streams[0].clip_seconds == 2,
            "preserved clip_seconds should flow into plan");
}

void explicit_input_recording_control_overrides_config()
{
    CameraParams cameras[1] = {
        make_camera("2010096", 7, {7, 8}),
    };
    CameraEachSelect selected[1]{};
    selected[0].record = true;

    nlohmann::json overrides = {
        {"recording_control", {
            {"record_for_seconds", 6},
            {"clip_seconds", 2}
        }}
    };

    orange::external_recorder::CameraContractMaterializationInput input;
    input.contract_config = &overrides;
    input.recording_folder = "/tmp/orange_contract_utils_recording_control";
    input.recording_id = "session_recording_control";
    input.cameras_params = cameras;
    input.cameras_select = selected;
    input.num_cameras = 1;
    input.recording_control.record_for_seconds = 9;
    input.recording_control.clip_seconds = 3;

    const nlohmann::json contract =
        orange::external_recorder::MaterializeExternalRecorderContractForCameras(input);
    require(contract["recording_control"]["record_for_seconds"] == 9,
            "explicit input record_for_seconds should override config");
    require(contract["recording_control"]["clip_seconds"] == 3,
            "explicit input clip_seconds should override config");
    require(contract["streams"]["2010096"]["recording_control"]["record_for_seconds"] == 9,
            "explicit input record_for_seconds should propagate to stream");
    require(contract["streams"]["2010096"]["recording_control"]["clip_seconds"] == 3,
            "explicit input clip_seconds should propagate to stream");
    require(contract.value("require_merged_mp4", true) == false,
            "explicit rolling control should disable the merged-session requirement");
}

void binds_selected_daily_circle_to_static_dish_prior()
{
    CameraParams cameras[1] = {
        make_camera("2010096", 7, {7, 8}),
    };
    CameraEachSelect selected[1]{};
    selected[0].record = true;
    nlohmann::json overrides = {
        {"importance_map", {
            {"mode", "static_dish_prior"},
            {"geometry_source", "selected_daily_registration"},
        }},
    };
    orange::external_recorder::CameraContractMaterializationInput input;
    input.contract_config = &overrides;
    input.recording_folder = "/tmp/orange_qp_binding";
    input.recording_id = "session_qp_binding";
    input.cameras_params = cameras;
    input.cameras_select = selected;
    input.num_cameras = 1;
    nlohmann::json contract =
        orange::external_recorder::MaterializeExternalRecorderContractForCameras(input);
    require(contract["streams"]["2010096"]["importance_map"]["mode"] ==
                "static_dish_prior",
            "top-level QP policy should propagate to materialized streams");

    const nlohmann::json geometry = {
        {"cameras", {
            {"2010096", {
                {"daily_registration_geometry", {
                    {"status", "resolved"},
                    {"recording_snapshot_entry", {
                        {"artifact_id", "dishrim_test"},
                        {"accepted_mask", {
                            {"shape", "circle"},
                            {"center_px", {{"x", 2215.5}, {"y", 2234.75}}},
                            {"radius_px", 2154.5},
                        }},
                        {"source", {
                            {"path", "/calibration/original/observation.json"},
                            {"sha256", "sha256:abc"},
                            {"intended_recording_relative_path",
                             "recording_geometry_assets/cameras/Cam2010096/observation.json"},
                        }},
                        {"calibration_ref", {
                            {"fingerprint", "fnv1a64:def"},
                        }},
                    }},
                }},
            }},
        }},
    };
    std::string error;
    require(orange::external_recorder::BindExternalRecorderDishPriorFromRecordingGeometry(
                &contract, geometry, input.recording_folder, &error),
            "daily geometry binding should succeed: " + error);
    const nlohmann::json& map = contract["streams"]["2010096"]["importance_map"];
    require(map["geometry"]["center_x_px"] == 2215.5,
            "bound QP map should use the accepted-mask center");
    require(map["geometry"]["radius_px"] == 2154.5,
            "bound QP map should use the accepted-mask radius");
    require(map["source"]["artifact_path"] ==
                "/tmp/orange_qp_binding/recording_geometry_assets/cameras/"
                "Cam2010096/observation.json",
            "bound QP map should point at the recording-local immutable source");
    require(map["source"]["artifact_sha256"] == "sha256:abc",
            "bound QP map should preserve source checksum");

    orange::external_recorder::SupervisorPlan plan;
    require(orange::external_recorder::BuildSupervisorPlanFromContract(
                contract, {}, &plan, &error),
            "bound QP contract should build a supervisor plan: " + error);
    require(plan.streams[0].importance_map.enabled(),
            "bound plan should enable the QP map");
}

void writes_failfast_artifacts()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_contract_utils_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(root);

    nlohmann::json contract = {
        {"schema_id", "orange.external_recorder.contract"},
        {"schema_version", 1},
        {"mode", "diagnostic_ipc_v1"},
        {"artifact_root", (root / "external_recorder").string()},
        {"session_id", "session_002"},
        {"require_summary", true},
        {"require_video_sanity", true},
        {"require_merged_mp4", true},
        {"require_gop_routing", true},
        {"require_status", true},
        {"require_status_runtime", true},
        {"require_storage_preflight", true},
        {"require_protocol_hello", true},
        {"streams", {
            {"2010095", {
                {"stream_id", "2010095"},
                {"camera_serial", "2010095"},
                {"analytics_gpu_id", 5},
                {"recorder_gpu_id", 5},
                {"expected_shard_gpu_ids", nlohmann::json::array({5, 6})},
                {"routing_policy", "gop_modulo"},
                {"summary_json", (root / "external_recorder/Cam2010095_external_summary.json").string()},
                {"video_sanity_json", (root / "external_recorder/Cam2010095_external_video_sanity.json").string()},
                {"mp4", (root / "external_recorder/Cam2010095_external.mp4").string()},
                {"gop_routing_csv", (root / "external_recorder/Cam2010095_external_gop_routing.csv").string()}
            }}
        }}
    };

    orange::external_recorder::FailFastArtifactOptions options;
    options.recording_folder = root.string();
    options.recording_id = "session_002";
    options.reason = "expected fail-fast";
    options.contract = contract;

    const orange::external_recorder::FailFastArtifactResult result =
        orange::external_recorder::WriteExternalRecorderFailFastArtifacts(options);
    require(result.ok, "fail-fast artifact write failed: " + result.error_message);
    require(std::filesystem::exists(result.external_recorder_contract_path),
            "missing contract artifact");
    require(std::filesystem::exists(result.external_recorder_supervisor_plan_path),
            "missing supervisor plan artifact");
    require(std::filesystem::exists(result.recording_session_path),
            "missing recording_session artifact");

    std::ifstream input(result.recording_session_path);
    nlohmann::json session;
    input >> session;
    require(session.value("status", "") == "failed", "session status mismatch");
    require(session.value("reason", "") == "expected fail-fast", "session reason mismatch");

    std::filesystem::remove_all(root);
}

void writes_supervised_session_artifacts()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_contract_utils_supervised_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(root);

    nlohmann::json contract = {
        {"schema_id", "orange.external_recorder.contract"},
        {"schema_version", 1},
        {"mode", "diagnostic_ipc_v1"},
        {"artifact_root", root.string()},
        {"session_id", "session_003"},
        {"streams", {
            {"2010095", {
                {"stream_id", "2010095"},
                {"camera_serial", "2010095"},
                {"analytics_gpu_id", 5},
                {"recorder_gpu_id", 5},
                {"expected_shard_gpu_ids", nlohmann::json::array({5, 6})},
                {"routing_policy", "gop_modulo"},
                {"summary_json", (root / "Cam2010095_external_summary.json").string()},
                {"video_sanity_json", (root / "Cam2010095_external_video_sanity.json").string()},
                {"mp4", (root / "Cam2010095_external.mp4").string()},
                {"gop_routing_csv", (root / "Cam2010095_external_gop_routing.csv").string()}
            }}
        }}
    };

    orange::external_recorder::SupervisorPlanOptions plan_options;
    orange::external_recorder::SupervisorPlan plan;
    std::string error;
    require(orange::external_recorder::BuildSupervisorPlanFromContract(
                contract,
                plan_options,
                &plan,
                &error),
            "supervised plan failed: " + error);

    orange::external_recorder::SupervisedSessionArtifactOptions options;
    options.artifact_root = root.string();
    options.contract = contract;
    options.supervisor_plan = &plan;

    const orange::external_recorder::SupervisedSessionArtifactResult result =
        orange::external_recorder::WriteExternalRecorderSupervisedSessionArtifacts(options);
    require(result.ok, "supervised artifact write failed: " + result.error_message);
    require(std::filesystem::exists(result.external_recorder_session_path),
            "missing supervised session artifact");
    require(std::filesystem::exists(result.external_recorder_supervisor_plan_path),
            "missing supervised plan artifact");

    std::ifstream session_input(result.external_recorder_session_path);
    nlohmann::json session;
    session_input >> session;
    require(session.value("schema_id", "") == "orange.external_recorder.contract",
            "supervised session schema mismatch");

    std::ifstream plan_input(result.external_recorder_supervisor_plan_path);
    nlohmann::json plan_json;
    plan_input >> plan_json;
    require(plan_json.value("schema_id", "") == "orange.external_recorder.supervisor_plan",
            "supervised plan schema mismatch");

    std::filesystem::remove_all(root);
}

void writes_runtime_handoff_and_finalization_artifacts()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_contract_utils_lifecycle_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(root);

    orange::external_recorder::SupervisorRuntimeState runtime;
    runtime.artifact_root = root.string();
    runtime.session_id = "session_004";
    orange::external_recorder::SupervisorRuntimeArtifactOptions runtime_options;
    runtime_options.artifact_root = root.string();
    runtime_options.runtime = &runtime;
    const orange::external_recorder::ArtifactWriteResult runtime_result =
        orange::external_recorder::WriteExternalRecorderSupervisorRuntimeArtifact(
            runtime_options);
    require(runtime_result.ok, "runtime artifact write failed: " + runtime_result.error_message);
    require(std::filesystem::exists(runtime_result.path), "missing runtime artifact");

    orange::external_recorder::VerifierHandoffArtifactOptions handoff_options;
    handoff_options.artifact_root = root.string();
    handoff_options.analytics_root = "/tmp/orange_analytics_root";
    handoff_options.verifier_path = "/repo/scripts/verify_external_recorder_session.py";
    handoff_options.require_video_sanity = true;
    handoff_options.require_status = true;
    handoff_options.require_status_runtime = true;
    handoff_options.require_storage_preflight = true;
    handoff_options.require_protocol_hello = true;
    const nlohmann::json handoff =
        orange::external_recorder::BuildExternalRecorderVerifierHandoff(handoff_options);
    require(handoff.value("schema_id", "") == "orange.external_recorder.verifier_handoff",
            "handoff schema mismatch");
    require(handoff["command"] == nlohmann::json::array({
                "/repo/scripts/verify_external_recorder_session.py",
                root.string(),
                "--analytics-root",
                "/tmp/orange_analytics_root",
                "--require-recorder-status",
                "--require-recorder-runtime-status",
                "--require-recorder-storage-preflight",
                "--require-recorder-protocol-hello"}),
            "handoff command mismatch");
    require(handoff.value("requires_status", false),
            "handoff should record status requirement");
    require(handoff.value("requires_status_runtime", false),
            "handoff should record runtime status requirement");
    require(handoff.value("requires_storage_preflight", false),
            "handoff should record storage preflight requirement");
    require(handoff.value("requires_protocol_hello", false),
            "handoff should record IPC protocol hello requirement");
    const orange::external_recorder::ArtifactWriteResult handoff_result =
        orange::external_recorder::WriteExternalRecorderVerifierHandoffArtifact(
            handoff_options);
    require(handoff_result.ok, "handoff artifact write failed: " + handoff_result.error_message);
    require(std::filesystem::exists(handoff_result.path), "missing handoff artifact");

    const nlohmann::json video_sanity = {
        {"2010095", {{"pass", true}}}
    };
    const nlohmann::json verifier = {
        {"pass", true},
        {"command", "verify"}
    };
    orange::external_recorder::FinalizationManifestOptions finalization_options;
    finalization_options.experiment_root = "/tmp/orange_analytics_root";
    finalization_options.artifact_root = root.string();
    finalization_options.run_id = "run_004";
    finalization_options.status = "pass";
    finalization_options.started_at_utc = "2026-05-07T00:00:00Z";
    finalization_options.finished_at_utc = "2026-05-07T00:00:01Z";
    finalization_options.video_sanity = &video_sanity;
    finalization_options.verifier = &verifier;
    const nlohmann::json finalization =
        orange::external_recorder::BuildExternalRecorderFinalizationManifest(
            finalization_options);
    require(finalization.value("schema_id", "") == "orange.external_recorder.finalization",
            "finalization schema mismatch");
    require(finalization.value("status", "") == "pass", "finalization status mismatch");
    require(finalization.contains("video_sanity"), "finalization missing video_sanity");
    require(finalization.contains("verifier"), "finalization missing verifier");
    const orange::external_recorder::ArtifactWriteResult finalization_result =
        orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
            root.string(),
            finalization);
    require(finalization_result.ok,
            "finalization artifact write failed: " + finalization_result.error_message);
    require(std::filesystem::exists(finalization_result.path),
            "missing finalization artifact");

    std::filesystem::remove_all(root);
}

}  // namespace

int main()
{
    try {
        parses_ipc_protocol_hello_lines();
        std::cout << "[PASS] parses_ipc_protocol_hello_lines\n";
        rejects_gop_25_30_contract_mismatch_before_unbounded_buffering();
        std::cout << "[PASS] rejects_gop_25_30_contract_mismatch_before_unbounded_buffering\n";
        materializes_contract_and_supervisor_plan();
        std::cout << "[PASS] materializes_contract_and_supervisor_plan\n";
        preserves_configured_recording_control_when_input_is_default();
        std::cout << "[PASS] preserves_configured_recording_control_when_input_is_default\n";
        explicit_input_recording_control_overrides_config();
        std::cout << "[PASS] explicit_input_recording_control_overrides_config\n";
        binds_selected_daily_circle_to_static_dish_prior();
        std::cout << "[PASS] binds_selected_daily_circle_to_static_dish_prior\n";
        writes_failfast_artifacts();
        std::cout << "[PASS] writes_failfast_artifacts\n";
        writes_supervised_session_artifacts();
        std::cout << "[PASS] writes_supervised_session_artifacts\n";
        writes_runtime_handoff_and_finalization_artifacts();
        std::cout << "[PASS] writes_runtime_handoff_and_finalization_artifacts\n";
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
