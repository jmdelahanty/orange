#include "spatial_roi_camera_recorder_process.h"
#include "spatial_roi_camera_recorder.h"

#include "session/spatial_roi_recorder_contract.h"
#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace spatial_roi = orange::session::spatial_roi;
namespace recording = orange::spatial_roi::recording;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

spatial_roi::RoiConfig make_roi(const std::string& camera_serial,
                                const std::string& roi_id,
                                const std::string& region_id,
                                const spatial_roi::Rect& rect)
{
    spatial_roi::RoiConfig roi;
    roi.roi_id = roi_id;
    roi.region_id = region_id;
    roi.has_arena_id = true;
    roi.arena_id = "arena_" + roi_id;
    roi.required = true;
    roi.content_rect = rect;
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id(camera_serial, roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem(camera_serial, roi_id);
    return roi;
}

nlohmann::json make_plan()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = 1000;
    config.recording_limits.max_media_bytes_per_stream = 1000000;
    config.recording_limits.max_evidence_bytes_per_stream = 100000;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 128;
    config.admission.max_total_media_bytes = 8000000;
    config.admission.max_total_evidence_bytes = 800000;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "2010096";
    camera.native_raster = {100, 100};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('2')};
    camera.registration = {"registration_1", digest('3')};
    camera.rois.push_back(
        make_roi(camera.camera_serial, "roi_1", "region_1", {0, 0, 10, 10}));
    camera.rois.push_back(
        make_roi(camera.camera_serial, "roi_2", "region_2", {20, 0, 11, 12}));
    camera.rois.push_back(
        make_roi(camera.camera_serial, "roi_3", "region_3", {40, 0, 12, 13}));
    camera.rois.push_back(
        make_roi(camera.camera_serial, "roi_4", "region_4", {60, 0, 13, 14}));
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "roi-process-supervisor-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error), error);
    return plan;
}

spatial_roi::SpatialRoiRecorderRuntimeGpuMapping make_mapping()
{
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    mapping.analytics_gpu_by_camera_serial.emplace("2010096", 5);
    for (const char* roi_id : {"roi_1", "roi_2", "roi_3", "roi_4"}) {
        mapping.recorder_gpu_by_logical_stream_id.emplace(
            spatial_roi::expected_logical_stream_id("2010096", roi_id), 6);
    }
    return mapping;
}

struct Fixture final {
    nlohmann::json plan = make_plan();
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping = make_mapping();
    nlohmann::json contract;
    std::string root =
        "/tmp/orange_process_supervisor_recording_" + std::to_string(::getpid());
    std::string contract_path = root + ".contract.json";
    std::string plan_path = root + ".plan.json";
    std::string token = plan.at("plan").at("recording_identity_token").get<std::string>();
    std::vector<std::string> stream_ids;
    std::string runtime_path;
    std::vector<std::string> socket_paths;

    Fixture()
    {
        std::string error;
        require(spatial_roi::build_spatial_roi_recorder_contract(
                    plan, root, mapping, &contract, &error),
                error);
        runtime_path = spatial_roi::expected_socket_runtime_directory(token);
        for (const char* roi_id : {"roi_1", "roi_2", "roi_3", "roi_4"}) {
            stream_ids.push_back(
                spatial_roi::expected_logical_stream_id("2010096", roi_id));
            socket_paths.push_back(
                spatial_roi::expected_socket_path(token, stream_ids.back()));
        }
        std::ofstream contract_file(contract_path);
        std::ofstream plan_file(plan_path);
        require(contract_file.good() && plan_file.good(),
                "failed to create supervisor JSON fixture files");
        contract_file << contract.dump();
        plan_file << plan.dump();
    }

    ~Fixture()
    {
        (void)::unlink(contract_path.c_str());
        (void)::unlink(plan_path.c_str());
        for (const std::string& path : socket_paths) (void)::unlink(path.c_str());
        (void)::rmdir(runtime_path.c_str());
    }
};

enum class FakeStoragePreflightMode {
    kValid,
    kMissingReady,
    kMismatchedTerminal,
    kMismatchedSchedulingTerminal,
    kContradictoryReady,
    kDuplicateReady,
    kReadyAfterTerminal,
};

nlohmann::json valid_storage_preflight(const Fixture& fixture)
{
    const auto& policy = fixture.contract.at("storage_preflight_policy");
    const auto& aggregate = fixture.contract.at("aggregate_bounds");
    const std::uint64_t media_bytes =
        aggregate.at("max_media_bytes_total").get<std::uint64_t>();
    const std::uint64_t evidence_bytes =
        aggregate.at("max_evidence_bytes_total").get<std::uint64_t>();
    const std::uint64_t reserved_free_bytes =
        policy.at("reserved_free_bytes").get<std::uint64_t>();
    const std::uint64_t required_bytes =
        media_bytes + evidence_bytes + reserved_free_bytes;
    return {
        {"schema_id", spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaId},
        {"schema_version", spatial_roi::kSpatialRoiRecorderStoragePreflightSchemaVersion},
        {"checked", true},
        {"passed", true},
        {"status", "passed"},
        {"error", ""},
        {"policy", policy},
        {"artifact_root", {{"device", std::uint64_t{1}},
                            {"inode", std::uint64_t{2}}}},
        {"filesystem", {
            {"block_size_bytes", std::uint64_t{1}},
            {"total_blocks", required_bytes},
            {"available_blocks", required_bytes},
            {"capacity_bytes", required_bytes},
            {"available_bytes", required_bytes},
        }},
        {"budgets", {
            {"max_media_bytes_total", media_bytes},
            {"max_evidence_bytes_total", evidence_bytes},
            {"reserved_free_bytes", reserved_free_bytes},
            {"required_bytes", required_bytes},
        }},
    };
}

recording::SpatialRoiRecorderStoragePreflightResult
valid_typed_storage_preflight(const Fixture& fixture)
{
    const nlohmann::json value = valid_storage_preflight(fixture);
    recording::SpatialRoiRecorderStoragePreflightResult result;
    result.checked = true;
    result.passed = true;
    result.status = "passed";
    result.error.clear();
    result.policy.schema_id =
        value.at("policy").at("schema_id").get<std::string>();
    result.policy.schema_version =
        value.at("policy").at("schema_version").get<int>();
    result.policy.required = value.at("policy").at("required").get<bool>();
    result.policy.reserved_free_bytes =
        value.at("policy").at("reserved_free_bytes").get<std::uint64_t>();
    result.artifact_root_identity.device = 1;
    result.artifact_root_identity.inode = 2;
    result.filesystem.block_size_bytes = 1;
    result.filesystem.total_blocks =
        value.at("filesystem").at("total_blocks").get<std::uint64_t>();
    result.filesystem.available_blocks =
        value.at("filesystem").at("available_blocks").get<std::uint64_t>();
    result.capacity_bytes =
        value.at("filesystem").at("capacity_bytes").get<std::uint64_t>();
    result.available_bytes =
        value.at("filesystem").at("available_bytes").get<std::uint64_t>();
    result.max_media_bytes_total =
        value.at("budgets").at("max_media_bytes_total").get<std::uint64_t>();
    result.max_evidence_bytes_total =
        value.at("budgets").at("max_evidence_bytes_total").get<std::uint64_t>();
    result.required_bytes =
        value.at("budgets").at("required_bytes").get<std::uint64_t>();
    return result;
}

nlohmann::json valid_inherited_scheduling()
{
    return {
        {"schema_id", "orange.spatial_roi_recorder.scheduling"},
        {"schema_version", 1},
        {"scope", "camera_recorder_inherited_thread_mask"},
        {"configuration_mode", "inherited"},
        {"configuration_source", "inherited_process_affinity"},
        {"requested_cpu_list", nullptr},
        {"canonical_requested_cpu_list", nullptr},
        {"affinity_syscall_succeeded", false},
        {"affinity_applied", false},
        {"effective_mask_verified", false},
        {"effective_cpu_list", "0"},
        {"effective_cpus", nlohmann::json::array({0})},
        {"kernel_isolated_cpu_list", nullptr},
        {"kernel_isolated_cpus", nlohmann::json::array()},
        {"kernel_isolation_observed", false},
        {"kernel_isolation_observation_error",
         "test fixture does not inspect host kernel isolation"},
        {"scheduler", {{"policy", "SCHED_OTHER"}, {"priority", 0}}},
        {"application_phase",
         "before_recorder_authority_and_worker_initialization"},
        {"thread_inheritance",
         "recorder_threads_created_after_this_snapshot_inherit_the_effective_mask"},
        {"error", nullptr},
    };
}

void write_status(const int fd, const char* event, const char* status,
                  const char* state, const bool ready, const bool clean_eof,
                  const bool completed,
                  const nlohmann::json* storage_preflight = nullptr,
                  const nlohmann::json* scheduling_override = nullptr)
{
    nlohmann::json value = {
        {"event", event},
        {"status", status},
        {"state", state},
        {"ready", ready},
        {"clean_eof", clean_eof},
        {"completed", completed},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"scheduling", scheduling_override != nullptr
                           ? *scheduling_override
                           : valid_inherited_scheduling()},
    };
    if (storage_preflight) {
        value["storage_preflight"] = *storage_preflight;
    }
    const std::string line = value.dump() + "\n";
    (void)::write(fd, line.data(), line.size());
}

void write_json_status(const int fd, const nlohmann::json& value)
{
    const std::string line = value.dump() + "\n";
    (void)::write(fd, line.data(), line.size());
}

pid_t spawn_fake_complete_child(const Fixture& fixture,
                                const std::vector<std::string>&,
                                const int stdout_fd,
                                std::string*,
                                const FakeStoragePreflightMode mode =
                                    FakeStoragePreflightMode::kValid)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    if (::dup2(stdout_fd, STDOUT_FILENO) < 0) _exit(126);
    if (stdout_fd != STDOUT_FILENO) (void)::close(stdout_fd);
    if (::mkdir(fixture.runtime_path.c_str(), 0700) != 0) _exit(125);
    (void)::chmod(fixture.runtime_path.c_str(), 0700);
    std::vector<int> sockets;
    for (const std::string& path : fixture.socket_paths) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) _exit(124);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) _exit(123);
        std::memcpy(address.sun_path, path.data(), path.size());
        address.sun_path[path.size()] = '\0';
        const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path.size() + 1U);
        if (::bind(fd,
                   reinterpret_cast<const sockaddr*>(&address),
                   address_length) != 0) {
            _exit(errno > 0 && errno < 256 ? errno : 122);
        }
        if (::listen(fd, 1) != 0) _exit(121);
        if (::chmod(path.c_str(), 0600) != 0) _exit(120);
        sockets.push_back(fd);
    }
    const nlohmann::json preflight = valid_storage_preflight(fixture);
    if (mode == FakeStoragePreflightMode::kReadyAfterTerminal) {
        write_status(STDOUT_FILENO,
                     "terminal",
                     "failed",
                     "failed",
                     false,
                     false,
                     false);
        write_status(STDOUT_FILENO,
                     "ready",
                     "ready",
                     "ready",
                     true,
                     false,
                     false,
                     &preflight);
        for (const int fd : sockets) (void)::close(fd);
        _exit(0);
    }
    if (mode == FakeStoragePreflightMode::kValid) {
        recording::SpatialRoiCameraRecorderSnapshot snapshot;
        snapshot.state = recording::SpatialRoiCameraRecorderState::kReady;
        snapshot.product_kind = "fixed_region";
        snapshot.recording_id =
            fixture.plan.at("plan").at("recording_id").get<std::string>();
        snapshot.session_id = snapshot.recording_id;
        snapshot.recording_identity_token = fixture.token;
        snapshot.producer_generation =
            fixture.plan.at("plan").at("producer_generation").get<std::string>();
        snapshot.spatial_roi_plan_sha256 =
            fixture.contract.at("spatial_roi_plan_sha256").get<std::string>();
        snapshot.camera_id = 3;
        snapshot.camera_serial = "2010096";
        snapshot.ready = true;
        snapshot.storage_preflight = valid_typed_storage_preflight(fixture);
        nlohmann::json ready =
            recording::spatial_roi_camera_recorder_snapshot_to_json(
                snapshot, "ready");
        ready["scheduling"] = valid_inherited_scheduling();
        write_json_status(STDOUT_FILENO, ready);
    } else if (mode == FakeStoragePreflightMode::kContradictoryReady) {
        nlohmann::json contradictory = {
            {"event", "ready"},
            {"status", "ready"},
            {"state", "ready"},
            {"ready", true},
            {"clean_eof", true},
            {"completed", false},
            {"failed", false},
            {"first_failure_stream_id", ""},
            {"first_failure", ""},
            {"error", ""},
            {"storage_preflight", preflight},
            {"scheduling", valid_inherited_scheduling()},
        };
        write_json_status(STDOUT_FILENO, contradictory);
    } else {
        write_status(STDOUT_FILENO,
                     "ready",
                     "ready",
                     "ready",
                     true,
                     false,
                     false,
                     mode == FakeStoragePreflightMode::kMissingReady
                         ? nullptr
                         : &preflight);
    }
    if (mode == FakeStoragePreflightMode::kDuplicateReady) {
        write_status(STDOUT_FILENO,
                     "ready",
                     "ready",
                     "ready",
                     true,
                     false,
                     false,
                     &preflight);
    }
    write_status(STDOUT_FILENO, "heartbeat", "running", "awaiting_eof", true,
                 false, false, &preflight);
    // Keep the complete terminal line in a later read so the supervisor tests
    // terminal validation after the ready phase rather than depending on pipe
    // packetization.
    ::usleep(100000);
    if (mode == FakeStoragePreflightMode::kMismatchedTerminal) {
        nlohmann::json mismatched = preflight;
        mismatched["budgets"]["required_bytes"] =
            mismatched["budgets"]["required_bytes"].get<std::uint64_t>() + 1U;
        write_status(STDOUT_FILENO,
                     "terminal",
                     "complete",
                     "completed",
                     true,
                     true,
                     true,
                     &mismatched);
    } else if (mode ==
               FakeStoragePreflightMode::kMismatchedSchedulingTerminal) {
        nlohmann::json mismatched_scheduling = valid_inherited_scheduling();
        mismatched_scheduling["effective_cpu_list"] = "1";
        mismatched_scheduling["effective_cpus"] = nlohmann::json::array({1});
        write_status(STDOUT_FILENO,
                     "terminal",
                     "complete",
                     "completed",
                     true,
                     true,
                     true,
                     &preflight,
                     &mismatched_scheduling);
    } else {
        write_status(STDOUT_FILENO,
                     "terminal",
                     "complete",
                     "completed",
                     true,
                     true,
                     true,
                     &preflight);
    }
    for (const int fd : sockets) (void)::close(fd);
    _exit(0);
}

pid_t spawn_fake_json_child(const std::string& line,
                            const int stdout_fd,
                            std::string*)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    if (::dup2(stdout_fd, STDOUT_FILENO) < 0) _exit(126);
    if (stdout_fd != STDOUT_FILENO) (void)::close(stdout_fd);
    (void)::write(STDOUT_FILENO, line.data(), line.size());
    _exit(0);
}

recording::SpatialRoiCameraRecorderProcessConfig make_config(const Fixture& fixture)
{
    recording::SpatialRoiCameraRecorderProcessConfig config;
    config.contract_path = fixture.contract_path;
    config.verified_plan_path = fixture.plan_path;
    config.expected_recording_root = fixture.root;
    config.recorder_executable = "/bin/true";
    config.expected_artifact_root_identity_available = true;
    config.expected_artifact_root_identity = {1, 2};
    config.gpu_mapping = fixture.mapping;
    config.expected_producer_pid = ::getpid();
    config.expected_producer_uid = ::geteuid();
    config.eof_timeout = std::chrono::seconds(1);
    config.readiness_timeout = std::chrono::seconds(1);
    config.poll_interval = std::chrono::milliseconds(20);
    config.heartbeat_interval = std::chrono::milliseconds(20);
    config.accept_timeout = std::chrono::milliseconds(100);
    config.ipc_timeout = std::chrono::seconds(1);
    config.video_probe_timeout = std::chrono::seconds(1);
    config.socket_wait_timeout = std::chrono::seconds(1);
    config.ready_wait_timeout = std::chrono::seconds(1);
    config.clean_exit_timeout = std::chrono::seconds(1);
    config.term_grace_timeout = std::chrono::milliseconds(100);
    config.kill_reap_timeout = std::chrono::milliseconds(100);
    config.supervisor_poll_interval = std::chrono::milliseconds(5);
    return config;
}

void test_authentication_socket_phase_and_json_snapshots()
{
    Fixture fixture;
    auto config = make_config(fixture);
    std::vector<std::string> launched_argv;
    config.spawn_override = [&](const auto& argv, const int fd, std::string* error) {
        launched_argv = argv;
        return spawn_fake_complete_child(fixture, argv, fd, error);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(!launched_argv.empty() && launched_argv.front() == "/bin/true",
            "supervisor did not construct the fixed child argv");
    const auto has_arg = [&](const std::string& value) {
        return std::find(launched_argv.begin(), launched_argv.end(), value) !=
               launched_argv.end();
    };
    const auto arg_value = [&](const std::string& option) {
        const auto found =
            std::find(launched_argv.begin(), launched_argv.end(), option);
        return found != launched_argv.end() &&
                       std::next(found) != launched_argv.end()
                   ? *std::next(found)
                   : std::string();
    };
    require(has_arg("--eof-timeout-ms") && has_arg("1000") &&
                has_arg("--expected-producer-pid") &&
                has_arg(std::to_string(::getpid())) &&
                has_arg("--expected-producer-uid") &&
                has_arg(std::to_string(::geteuid())),
            "mandatory child authority arguments were omitted");
    require(arg_value("--cpu-affinity").empty() &&
                arg_value("--cpu-affinity-source").empty(),
            "default-inherit recorder unexpectedly emitted affinity arguments");
    require(process->WaitForFourSockets(&error), error);
    require(process->state() ==
                recording::SpatialRoiCameraRecorderProcessState::kSocketsBound,
            "socket-bound phase was not distinct from ready");
    require(process->WaitUntilReady(&error), error);
    require(process->status().ready &&
                process->status().ready_snapshot.event == "ready",
            "ready JSON snapshot was not retained");
    require(process->WaitForCleanExit(&error), error);
    require(process->status().heartbeat.event == "heartbeat" &&
                process->status().terminal.event == "terminal" &&
                process->status().terminal.completed &&
                process->status().terminal.clean_eof &&
                process->status().exit_code == 0 && process->status().reaped,
            "heartbeat/terminal/clean-exit snapshots were not retained");
    require(process->Stop(&error), error);
    for (const std::string& path : fixture.socket_paths) {
        struct stat status {};
        require(::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode),
                "supervisor unexpectedly unlinked a socket path");
    }
}

void test_cpu_affinity_argv_is_canonical()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.cpu_affinity = "35-37,3-5";
    config.cpu_affinity_source =
        "ORANGE_SPATIAL_ROI_RECORDER_CPU_AFFINITY_CAM_2010096";
    std::vector<std::string> launched_argv;
    config.spawn_override = [&](const auto& argv,
                                const int stdout_fd,
                                std::string*) {
        launched_argv = argv;
        const pid_t child = ::fork();
        if (child == 0) {
            (void)::close(stdout_fd);
            while (true) ::pause();
        }
        return child;
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    const auto arg_value = [&](const std::string& option) {
        const auto found =
            std::find(launched_argv.begin(), launched_argv.end(), option);
        return found != launched_argv.end() &&
                       std::next(found) != launched_argv.end()
                   ? *std::next(found)
                   : std::string();
    };
    require(arg_value("--cpu-affinity") == "3-5,35-37" &&
                arg_value("--cpu-affinity-source") ==
                    "ORANGE_SPATIAL_ROI_RECORDER_CPU_AFFINITY_CAM_2010096",
            "optional recorder CPU affinity was not canonicalized and forwarded exactly");
    require(process->Stop(&error), error);
}

void test_cpu_affinity_requires_bounded_source()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.cpu_affinity = "3-5,35-37";
    bool launched = false;
    config.spawn_override = [&](const auto&, const int, std::string*) {
        launched = true;
        return static_cast<pid_t>(-1);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(!process->Start(&error) && !launched &&
                error.find("authority source") != std::string::npos,
            "CPU affinity without an authority source reached child launch");
}

void test_ready_requires_valid_storage_preflight()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.spawn_override = [&](const auto& argv, const int fd, std::string* error) {
        return spawn_fake_complete_child(
            fixture, argv, fd, error, FakeStoragePreflightMode::kMissingReady);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    const bool sockets_ready = process->WaitForFourSockets(&error);
    const bool ready = sockets_ready && process->WaitUntilReady(&error);
    require(!ready &&
                error.find("storage preflight") != std::string::npos &&
                process->status().state ==
                    recording::SpatialRoiCameraRecorderProcessState::kFailed,
            "ready=true without storage preflight was accepted");
}

void test_complete_terminal_requires_matching_storage_preflight()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.spawn_override = [&](const auto& argv, const int fd, std::string* error) {
        return spawn_fake_complete_child(
            fixture,
            argv,
            fd,
            error,
            FakeStoragePreflightMode::kMismatchedTerminal);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(process->WaitForFourSockets(&error), error);
    require(process->WaitUntilReady(&error), error);
    require(!process->WaitForCleanExit(&error) &&
                error.find("storage preflight") != std::string::npos &&
                process->status().state ==
                    recording::SpatialRoiCameraRecorderProcessState::kFailed,
            "complete terminal accepted mismatched storage preflight evidence");
}

void test_complete_terminal_requires_matching_scheduling()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.spawn_override = [&](const auto& argv,
                                const int fd,
                                std::string* error) {
        return spawn_fake_complete_child(
            fixture,
            argv,
            fd,
            error,
            FakeStoragePreflightMode::kMismatchedSchedulingTerminal);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(process->WaitForFourSockets(&error), error);
    require(process->WaitUntilReady(&error), error);
    require(!process->WaitForCleanExit(&error) &&
                error.find("scheduling") != std::string::npos &&
                process->status().state ==
                    recording::SpatialRoiCameraRecorderProcessState::kFailed,
            "complete terminal accepted scheduling that differed from ready evidence");
}

void test_ready_event_is_clean_and_one_shot()
{
    for (const auto mode : {FakeStoragePreflightMode::kContradictoryReady,
                            FakeStoragePreflightMode::kDuplicateReady}) {
        Fixture fixture;
        auto config = make_config(fixture);
        config.spawn_override = [&, mode](const auto& argv,
                                          const int fd,
                                          std::string* error) {
            return spawn_fake_complete_child(fixture, argv, fd, error, mode);
        };
        std::string error;
        auto process = recording::SpatialRoiCameraRecorderProcess::Create(
            std::move(config), &error);
        require(process != nullptr, error);
        require(process->Start(&error), error);
        const bool sockets_ready = process->WaitForFourSockets(&error);
        const bool ready = sockets_ready && process->WaitUntilReady(&error);
        require(!ready && process->status().state ==
                             recording::SpatialRoiCameraRecorderProcessState::kFailed,
                "contradictory or duplicate ready event was accepted");
        require(error.find(mode == FakeStoragePreflightMode::kContradictoryReady
                               ? "clean readiness"
                               : "duplicate ready") != std::string::npos,
                "adversarial ready event did not identify its ordering/flag violation");
    }
}

void test_ready_after_terminal_is_rejected()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.spawn_override = [&](const auto& argv, const int fd, std::string* error) {
        return spawn_fake_complete_child(
            fixture, argv, fd, error, FakeStoragePreflightMode::kReadyAfterTerminal);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    const bool sockets_ready = process->WaitForFourSockets(&error);
    const bool ready = sockets_ready && process->WaitUntilReady(&error);
    require(!ready &&
                error.find("ready after") != std::string::npos &&
                process->status().state ==
                    recording::SpatialRoiCameraRecorderProcessState::kFailed,
            "ready event after terminal was accepted");
}

void test_authentication_fails_before_launch()
{
    Fixture fixture;
    fixture.contract["recording_id"] = "tampered";
    std::ofstream contract_file(fixture.contract_path);
    contract_file << fixture.contract.dump();
    bool launched = false;
    auto config = make_config(fixture);
    config.spawn_override = [&](const auto&, const int, std::string*) {
        launched = true;
        return static_cast<pid_t>(-1);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(!process->Start(&error) && !launched,
            "tampered contract reached the child launcher");
    require(process->status().state ==
                recording::SpatialRoiCameraRecorderProcessState::kFailed,
            "pre-launch authentication failure was not terminal");
}

void test_socket_timeout_escalates_and_reaps_exact_child()
{
    Fixture fixture;
    auto config = make_config(fixture);
    config.socket_wait_timeout = std::chrono::milliseconds(40);
    config.term_grace_timeout = std::chrono::milliseconds(20);
    config.kill_reap_timeout = std::chrono::milliseconds(100);
    config.spawn_override = [](const auto&, const int stdout_fd, std::string*) {
        const pid_t child = ::fork();
        if (child == 0) {
            if (stdout_fd != STDOUT_FILENO) (void)::close(stdout_fd);
            while (true) ::pause();
        }
        return child;
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(!process->WaitForFourSockets(&error),
            "socket timeout unexpectedly reported success");
    require(process->status().reaped && process->status().exited,
            "socket timeout did not reap its exact child PID");
}

void test_duplicate_lifecycle_key_is_rejected_before_dom_parse()
{
    Fixture fixture;
    auto config = make_config(fixture);
    const std::string duplicate =
        "{\"event\":\"heartbeat\",\"event\":\"heartbeat\","
        "\"ready\":false}\n";
    config.spawn_override = [duplicate](const auto&, const int fd, std::string* error) {
        return spawn_fake_json_child(duplicate, fd, error);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(!process->WaitForFourSockets(&error),
            "duplicate lifecycle key was accepted");
    require(process->status().reaped &&
                error.find("duplicate") != std::string::npos,
            "duplicate lifecycle key did not produce a bounded-parser error");
}

void test_deep_lifecycle_payload_is_rejected_before_dom_parse()
{
    Fixture fixture;
    std::string deep = "{\"event\":\"heartbeat\",\"payload\":";
    for (int depth = 0; depth < 80; ++depth) deep += '[';
    deep += "false";
    for (int depth = 0; depth < 80; ++depth) deep += ']';
    deep += "}\n";

    auto config = make_config(fixture);
    config.spawn_override = [deep](const auto&, const int fd, std::string* error) {
        return spawn_fake_json_child(deep, fd, error);
    };
    std::string error;
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), &error);
    require(process != nullptr, error);
    require(process->Start(&error), error);
    require(!process->WaitForFourSockets(&error),
            "deep lifecycle payload was accepted");
    require(process->status().reaped &&
                error.find("structural bounds") != std::string::npos,
            "deep lifecycle payload did not produce a bounded-parser error");
}

}  // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"authentication_socket_phase_and_json_snapshots",
         test_authentication_socket_phase_and_json_snapshots},
        {"cpu_affinity_argv_is_canonical",
         test_cpu_affinity_argv_is_canonical},
        {"cpu_affinity_requires_bounded_source",
         test_cpu_affinity_requires_bounded_source},
        {"ready_requires_valid_storage_preflight",
         test_ready_requires_valid_storage_preflight},
        {"complete_terminal_requires_matching_storage_preflight",
         test_complete_terminal_requires_matching_storage_preflight},
        {"complete_terminal_requires_matching_scheduling",
         test_complete_terminal_requires_matching_scheduling},
        {"ready_event_is_clean_and_one_shot",
         test_ready_event_is_clean_and_one_shot},
        {"ready_after_terminal_is_rejected",
         test_ready_after_terminal_is_rejected},
        {"authentication_fails_before_launch", test_authentication_fails_before_launch},
        {"socket_timeout_escalates_and_reaps_exact_child",
         test_socket_timeout_escalates_and_reaps_exact_child},
        {"duplicate_lifecycle_key_is_rejected_before_dom_parse",
         test_duplicate_lifecycle_key_is_rejected_before_dom_parse},
        {"deep_lifecycle_payload_is_rejected_before_dom_parse",
         test_deep_lifecycle_payload_is_rejected_before_dom_parse},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }
    std::cout << tests.size() - static_cast<std::size_t>(failures)
              << " spatial ROI process supervisor tests passed\n";
    return failures == 0 ? 0 : 1;
}
