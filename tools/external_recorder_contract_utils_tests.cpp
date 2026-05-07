#include "external_recorder_contract_utils.h"
#include "external_recorder_supervisor.h"
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

void materializes_contract_and_supervisor_plan()
{
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

    const nlohmann::json contract =
        orange::external_recorder::MaterializeExternalRecorderContractForCameras(input);
    require(contract.value("schema_id", "") == "orange.external_recorder.contract",
            "contract schema_id mismatch");
    require(contract.value("artifact_root", "") ==
                "/tmp/orange_contract_utils_test/external_recorder",
            "artifact_root template was not expanded");
    require(contract["streams"].size() == 2, "expected two contract streams");
    require(contract["streams"]["2010095"].value("routing_policy", "") == "gop_modulo",
            "2010095 should route by GOP modulo");
    require(contract["streams"]["2010095"]["expected_shard_gpu_ids"] == nlohmann::json::array({5, 6}),
            "2010095 shard GPU ids mismatch");
    require(contract["streams"]["2010096"].value("mp4", "") ==
                "/tmp/orange_contract_utils_test/custom_session_001.mp4",
            "stream override path template was not expanded");

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

}  // namespace

int main()
{
    try {
        materializes_contract_and_supervisor_plan();
        std::cout << "[PASS] materializes_contract_and_supervisor_plan\n";
        writes_failfast_artifacts();
        std::cout << "[PASS] writes_failfast_artifacts\n";
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
