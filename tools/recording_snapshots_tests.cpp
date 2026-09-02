#include "gui/recording_snapshots.h"

#include "camera.h"
#include "crop_and_encode_worker.h"
#include "crop_producer.h"
#include "fnv1a64_fingerprint.h"
#include "gui/spatial_layout/sha256.h"
#include "project.h"
#include "session/recording_observation_identity.h"
#include "shaman_v2_recording_identity.h"
#include "video_capture.h"

#include <cstdlib>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

// recording_snapshots.cpp references CropAndEncodeWorker::SanitizeCropSize, whose
// production definition lives in crop_and_encode_worker.cpp next to the full
// NVENC/FFmpeg pipeline. Linking that here would drag the encoder stack into a
// pure-metadata test, so provide the same definition (the production one is a
// one-line call to the shared inline sanitizer in camera.h).
int CropAndEncodeWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_exact_fixture(const std::filesystem::path& path,
                         const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not write fixture " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_exact_fixture(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read fixture " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string sha256_fixture(const std::filesystem::path& path)
{
    std::string checksum;
    std::string error;
    require(
        orange::gui::spatial_layout::checksum::file_sha256(
            path, &checksum, &error),
        "could not checksum fixture " + path.string() + ": " + error);
    return checksum;
}

std::string fnv1a64_fixture(const std::filesystem::path& path)
{
    const std::string bytes = read_exact_fixture(path);
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= UINT64_C(1099511628211);
    }
    return orange::calibration::format_fnv1a64_fingerprint(hash);
}

struct SpatialRoiSnapshotFixture {
    nlohmann::json recording_outputs_v3;
    nlohmann::json session;
};

enum class SnapshotFixtureProfile {
    legacy_p7_cqp0_gop1,
    p1_vbr_q20_gop1,
    active_p1_vbr_q20_gop25,
};

std::string snapshot_test_digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

nlohmann::json make_complete_recorder_process_status()
{
    const nlohmann::json preflight = {
        {"schema_id", "orange.spatial_roi_recording.storage_preflight"},
        {"schema_version", 1},
        {"checked", true},
        {"passed", true},
        {"status", "passed"},
        {"error", ""},
        {"policy", {
            {"schema_id", "orange.spatial_roi_recorder_storage_preflight_policy"},
            {"schema_version", 1},
            {"required", true},
            {"reserved_free_bytes", 10}}},
        {"artifact_root", {{"device", 1}, {"inode", 3}}},
        {"filesystem", {
            {"block_size_bytes", 1},
            {"total_blocks", 100000},
            {"available_blocks", 100000},
            {"capacity_bytes", 100000},
            {"available_bytes", 100000}}},
        {"budgets", {
            {"max_media_bytes_total", 1000},
            {"max_evidence_bytes_total", 5000},
            {"reserved_free_bytes", 10},
            {"required_bytes", 6010}}}};
    const nlohmann::json empty_child = {
        {"event", ""},
        {"status", ""},
        {"state", ""},
        {"ready", false},
        {"clean_eof", false},
        {"completed", false},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", nlohmann::json::object()}};
    const nlohmann::json child = {
        {"event", "ready"},
        {"status", "ready"},
        {"state", "ready"},
        {"ready", true},
        {"clean_eof", false},
        {"completed", false},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", {{"storage_preflight", preflight}}}};
    const nlohmann::json terminal = {
        {"event", "terminal"},
        {"status", "complete"},
        {"state", "completed"},
        {"ready", true},
        {"clean_eof", true},
        {"completed", true},
        {"failed", false},
        {"first_failure_stream_id", ""},
        {"first_failure", ""},
        {"error", ""},
        {"payload", {{"storage_preflight", preflight}}}};
    return {
        {"schema_id", "orange.spatial_roi_recording.headless_process_status"},
        {"schema_version", 1},
        {"session_state", "finished"},
        {"process_state", "exited"},
        {"pid", 1234},
        {"started", true},
        {"sockets_bound", true},
        {"ready", true},
        {"terminal_seen", true},
        {"exited", true},
        {"reaped", true},
        {"exit_code", 0},
        {"term_signal", 0},
        {"stdout_bytes_read", 100},
        {"cleanup_complete", true},
        {"first_failure", ""},
        {"error", ""},
        {"starting", empty_child},
        {"ready_snapshot", child},
        {"heartbeat", empty_child},
        {"terminal", terminal},
        {"last", terminal}};
}

SpatialRoiSnapshotFixture make_spatial_roi_snapshot_fixture(
    const std::string& status,
    const SnapshotFixtureProfile fixture_profile =
        SnapshotFixtureProfile::legacy_p7_cqp0_gop1)
{
    constexpr std::array<const char*, 12> artifact_kinds = {
        "video", "metadata", "keyframes", "perf", "summary", "status",
        "video_sanity", "finalization", "recorder_log", "transport_sidecar",
        "evidence", "evidence_manifest"};
    constexpr std::array<const char*, 16> count_keys = {
        "detach_successes", "dispatch_admitted", "dispatch_rejected",
        "ack_attempted", "ack_sent", "ack_accepted", "release_attempted",
        "release_sent", "encoded_frames", "failed_frames", "packet_count",
        "encoded_bytes", "keyframes", "ack_write_failures",
        "release_write_failures", "lifecycle_failures"};

    const std::string camera_serial = "2010096";
    const std::string recording_id = "spatial_roi_atomic_run";
    const std::string token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            recording_id);
    const std::string generation = "generation_001";
    const std::string plan_digest = snapshot_test_digest('b');
    const nlohmann::json native_raster = {{"width", 4512}, {"height", 4512}};
    const nlohmann::json authority_layout = {
        {"id", "layout_1"}, {"sha256", snapshot_test_digest('c')}};
    const nlohmann::json authority_materialization = {
        {"id", "materialization_1"}, {"sha256", snapshot_test_digest('d')}};
    const nlohmann::json authority_registration = {
        {"id", "registration_1"}, {"sha256", snapshot_test_digest('e')}};

    nlohmann::json rois = nlohmann::json::array();
    nlohmann::json streams = nlohmann::json::array();
    nlohmann::json spatial_roi_descriptors = nlohmann::json::object();
    nlohmann::json recorder_gpus = nlohmann::json::object();
    nlohmann::json stream_order = nlohmann::json::array();
    const bool active_p1 =
        fixture_profile != SnapshotFixtureProfile::legacy_p7_cqp0_gop1;
    const std::string profile_id =
        fixture_profile == SnapshotFixtureProfile::active_p1_vbr_q20_gop25
            ? "hevc_p1_low_latency_vbr_q20_gop25_v1"
            : (active_p1 ? "hevc_p1_low_latency_vbr_q20_gop1_v1"
                         : "hevc_p7_lossless_cqp0_gop1_v1");
    const std::string preset = active_p1 ? "p1" : "p7";
    const std::string tuning = active_p1 ? "ll" : "lossless";
    const bool lossless = !active_p1;
    const std::string rate_control_mode = active_p1 ? "vbr" : "cqp";
    const int quality_value = active_p1 ? 20 : 0;
    const int gop_length =
        fixture_profile == SnapshotFixtureProfile::active_p1_vbr_q20_gop25
            ? 25
            : 1;
    for (int index = 1; index <= 4; ++index) {
        const std::string stream_id =
            camera_serial + "_spatial_roi_roi_" + std::to_string(index);
        const std::string roi_id = "roi_" + std::to_string(index);
        const std::string region_id = "region_" + std::to_string(index);
        const int recorder_gpu = index + 1;
        const std::string artifact_prefix = "external_spatial_roi_recorder/" +
                                            stream_id + "/";
        const std::string receipt_artifact_prefix = stream_id + "/";
        stream_order.push_back(stream_id);
        recorder_gpus[stream_id] = recorder_gpu;

        const nlohmann::json content_rect = {
            {"x", index * 10}, {"y", index * 12}, {"width", 24}, {"height", 24}};
        const nlohmann::json encoded_content_rect = {
            {"x", 0}, {"y", 0}, {"width", 24}, {"height", 24}};
        const nlohmann::json geometry = {
            {"layout", authority_layout},
            {"materialization", authority_materialization},
            {"registration", authority_registration},
            {"native_raster", native_raster},
            {"content_rect", content_rect},
            {"encoded_raster", {{"width", 26}, {"height", 26}}},
            {"encoded_content_rect", encoded_content_rect},
            {"content_offset", {{"x", 0}, {"y", 0}}},
            {"padding", {{"left", 0}, {"top", 0}, {"right", 2},
                          {"bottom", 2}, {"value_mono8", 0}}},
            {"source_coordinate_space", "camera_native_full_frame_pixels"},
            {"video_coordinate_space", "spatial_roi_encoded_pixels"}};
        const nlohmann::json source_geometry = {
            {"native_raster", native_raster},
            {"content_rect", content_rect},
            {"coordinate_space", "camera_native_full_frame_pixels"}};
        const nlohmann::json encoded_geometry = {
            {"raster", {{"width", 26}, {"height", 26}}},
            {"content_rect", encoded_content_rect},
            {"coordinate_space", "spatial_roi_encoded_pixels"}};
        const nlohmann::json profile = {
            {"profile_id", profile_id},
            {"codec", "hevc"}, {"preset", preset},
            {"tuning", tuning}, {"lossless", lossless},
            {"rate_control_mode", rate_control_mode},
            {"quality_value", quality_value},
            {"gop_length", gop_length}, {"aq", false},
            {"temporal_aq", false}, {"lookahead", false},
            {"lookahead_depth", 0}, {"frame_rate", 100}, {"input_format", "mono8"},
            {"encoded_format", "nv12"}, {"no_resize", true},
            {"luma_preserved_exactly", lossless}, {"neutral_chroma_value", 128}};
        const nlohmann::json identity = {
            {"recording_id", recording_id},
            {"recording_identity_token", token},
            {"producer_generation", generation},
            {"spatial_roi_plan_sha256", plan_digest},
            {"camera_id", 0},
            {"camera_serial", camera_serial},
            {"arena_group_id", "arena_group_1"},
            {"arena_id", index == 1 ? nlohmann::json("arena_1") : nullptr},
            {"region_id", region_id},
            {"roi_id", roi_id},
            {"logical_stream_id", stream_id}};
        rois.push_back({
            {"stream_id", stream_id},
            {"logical_stream_id", stream_id},
            {"roi_id", roi_id},
            {"region_id", region_id},
            {"arena_group_id", "arena_group_1"},
            {"arena_id", index == 1 ? nlohmann::json("arena_1") : nullptr},
            {"geometry", geometry},
            {"source_geometry", source_geometry},
            {"encoded_geometry", encoded_geometry},
            {"encode_profile", profile},
            {"encode_fps", 100},
            {"codec", "hevc"},
            {"tuning", tuning},
            {"analytics_gpu_id", 1},
            {"source_gpu_id", 1},
            {"recorder_gpu_id", recorder_gpu},
            {"assigned_gpu_id", recorder_gpu},
            {"expected_shard_gpu_ids", {recorder_gpu}}});

        nlohmann::json artifact_paths = nlohmann::json::object();
        nlohmann::json receipt_artifacts = nlohmann::json::array();
        for (std::size_t artifact_index = 0;
             artifact_index < artifact_kinds.size(); ++artifact_index) {
            const char* kind = artifact_kinds.at(artifact_index);
            const std::string recording_relative_path =
                artifact_prefix + kind + ".artifact";
            const std::string receipt_relative_path =
                receipt_artifact_prefix + kind + ".artifact";
            artifact_paths[kind] = recording_relative_path;
            receipt_artifacts.push_back({
                {"kind", kind},
                {"relative_path", receipt_relative_path},
                {"size_bytes", static_cast<std::uint64_t>(100 + artifact_index)},
                {"sha256", snapshot_test_digest(
                                static_cast<char>('f' - artifact_index % 6))}});
        }

        nlohmann::json counts = nlohmann::json::object();
        for (const char* key : count_keys) {
            const std::string name(key);
            const bool failure_counter =
                name == "dispatch_rejected" || name == "failed_frames" ||
                name == "ack_write_failures" ||
                name == "release_write_failures" ||
                name == "lifecycle_failures";
            const bool byte_counter = name == "encoded_bytes";
            counts[key] = failure_counter
                              ? static_cast<std::uint64_t>(0)
                              : (byte_counter ? static_cast<std::uint64_t>(400)
                                              : static_cast<std::uint64_t>(4));
        }
        counts["keyframes"] = static_cast<std::uint64_t>(
            gop_length == 1 ? 4 : 1);
        const nlohmann::json ranges = {
            {"recording_frame_id", {{"first", 1}, {"last", 4}}},
            {"roi_stream_frame_index", {{"first", 1}, {"last", 4}}},
            {"has_frames", true}, {"frame_count", 4}};
        const nlohmann::json receipt_stream = {
            {"logical_stream_id", stream_id},
            {"identity", {
                {"recording_id", recording_id}, {"session_id", recording_id},
                {"recording_identity_token", token},
                {"producer_generation", generation},
                {"spatial_roi_plan_sha256", plan_digest}, {"camera_id", 0},
                {"camera_serial", camera_serial}, {"roi_id", roi_id},
                {"region_id", region_id}, {"arena_group_id", "arena_group_1"},
                {"logical_stream_id", stream_id},
                {"assigned_gpu_id", recorder_gpu}, {"assigned_shard_id", 0}}},
            {"counts", counts}, {"ranges", ranges},
            {"finalized_receipt_digest", snapshot_test_digest('0')},
            {"artifacts", receipt_artifacts}};
        streams.push_back(receipt_stream);

        nlohmann::json details = {
            {"stream_id", stream_id},
            {"stream_kind", "spatial_roi"},
            {"identity", identity},
            {"artifact_path_scope", "recording_root_relative"},
            {"artifact_root_relative", "external_spatial_roi_recorder"},
            {"recording_id", recording_id}, {"session_id", recording_id},
            {"recording_identity_token", token},
            {"producer_generation", generation},
            {"spatial_roi_plan_sha256", plan_digest}, {"camera_id", 0},
            {"camera_serial", camera_serial}, {"roi_id", roi_id},
            {"region_id", region_id}, {"arena_group_id", "arena_group_1"},
            {"arena_id", index == 1 ? nlohmann::json("arena_1") : nullptr},
            {"logical_stream_id", stream_id},
            {"frame_identity", {
                {"key_fields", {"recording_identity_token", "producer_generation",
                                 "logical_stream_id", "recording_frame_id",
                                 "roi_stream_frame_index"}},
                {"roi_stream_frame_index", "dense_one_based"},
                {"recording_frame_id_source", "parent_camera_recording"}}},
            {"analytics_gpu_id", 1},
            {"source_gpu_id", 1},
            {"recorder_gpu_id", recorder_gpu},
            {"assigned_gpu_id", recorder_gpu},
            {"geometry", geometry},
            {"geometry_identity", geometry},
            {"source_geometry", source_geometry},
            {"encoded_geometry", {
                {"encoded_raster", geometry.at("encoded_raster")},
                {"encoded_content_rect", geometry.at("encoded_content_rect")},
                {"raster", encoded_geometry.at("raster")},
                {"content_rect", encoded_geometry.at("content_rect")},
                {"coordinate_space", encoded_geometry.at("coordinate_space")}}},
            {"encode_profile", profile},
            {"encode_fps", 100},
            {"gop", gop_length},
            {"rate_control_mode", rate_control_mode},
            {"quality_value", quality_value},
            {"encode_queue_depth", 1},
            {"routing_policy", "single_shard"},
            {"expected_shard_gpu_ids", {recorder_gpu}},
            {"artifacts", artifact_paths}};
        if (status == "complete") {
            details["finalized_receipt"] = receipt_stream;
        }
        spatial_roi_descriptors[stream_id] = {
            {"schema_version", 3}, {"camera_serial", camera_serial},
            {"output_kind", "spatial_roi"}, {"logical_stream_id", stream_id},
            {"role", "runtime_derived_acquisition_input"},
            {"backend", "external_ipc"}, {"status", status},
            {"video", artifact_paths.at("video")},
            {"metadata", artifact_paths.at("metadata")},
            {"keyframes", artifact_paths.at("keyframes")},
            {"perf", artifact_paths.at("perf")},
            {"summary", artifact_paths.at("summary")},
            {"frame_count", 4}, {"first_recording_frame_id", 1},
            {"last_recording_frame_id", 4}, {"recording_frame_id_gaps", 0},
            {"packet_count", 4},
            {"packet_count_source", "spatial_roi_finalized_session_receipt"},
            {"width", 26}, {"height", 26}, {"frame_rate", 100},
            {"codec", "hevc"}, {"container", "mp4"}, {"tuning", tuning},
            {"pixel_source_format", "mono8"}, {"encoded_format", "nv12"},
            {"coordinate_space", "camera_native_full_frame_pixels"},
            {"video_pixel_coordinate_space", "spatial_roi_encoded_pixels"},
            {"source_geometry_coordinate_space", "camera_native_full_frame_pixels"},
            {"details", details}};
    }

    nlohmann::json receipt = {
        {"schema_id", "orange.spatial_roi_recording.finalized_session_receipt"},
        {"schema_version", 1},
        {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
        {"stream_kind", "fixed_region"}, {"status", "complete"},
        {"stream_count", 4}, {"stream_order", stream_order},
        {"identity", {
            {"recording_id", recording_id}, {"session_id", recording_id},
            {"recording_identity_token", token},
            {"producer_generation", generation},
            {"spatial_roi_plan_sha256", plan_digest}, {"camera_id", 0},
            {"camera_serial", camera_serial}, {"stream_count", 4},
            {"stream_order", stream_order}}}};
    receipt["root_authority"] = {
        {"artifact_root_relative", "external_spatial_roi_recorder"},
        {"recording_root_identity", {{"device", 1}, {"inode", 2}}},
        {"artifact_root_identity", {{"device", 1}, {"inode", 3}}},
        {"root_continuity", {
            {"proven", {"opened recording root"}},
            {"not_proven", {"historical continuity"}}}}};
    receipt["streams"] = streams;

    const nlohmann::json full_output = {
        {"schema_version", 1},
        {"camera_serial", camera_serial},
        {"output_kind", "full"},
        {"role", "ingest_authoritative"},
        {"backend", "in_process"},
        {"status", status == "complete" ? "finalized" : status},
        {"video", "full/video.mp4"},
        {"metadata", "full/metadata.json"},
        {"keyframes", "full/keyframes.json"},
        {"perf", "full/perf.csv"},
        {"container", "mp4"},
        {"coordinate_space", "full_frame_pixels"},
        {"frame_count", 4},
        {"first_recording_frame_id", 1},
        {"last_recording_frame_id", 4},
        {"recording_frame_id_gaps", 0},
        {"packet_count", 4},
        {"packet_count_source", "ffprobe_nb_read_packets"}};
    const nlohmann::json producer_status = {
        {"schema_id", "orange.spatial_roi_recording.headless_producer_status"},
        {"schema_version", 1},
        {"state", "stopped"},
        {"recording_id", recording_id},
        {"session_id", recording_id},
        {"recording_identity_token", token},
        {"producer_generation", generation},
        {"spatial_roi_plan_sha256", plan_digest},
        {"camera_id", 0},
        {"camera_serial", camera_serial},
        {"stream_count", 4},
        {"submit_attempted", 4},
        {"submitted", 4},
        {"incomplete", 0},
        {"rejected", 0},
        {"acquisition_armed", false},
        {"first_failure", ""}};

    const nlohmann::json session = {
        {"schema_id", "orange.spatial_roi_recording.session_snapshot"},
        {"schema_version", 3}, {"status", status},
        {"recording_id", recording_id}, {"session_id", recording_id},
        {"recording_identity_token", token}, {"producer_generation", generation},
        {"spatial_roi_plan_sha256", plan_digest}, {"product_kind", "fixed_region"},
        {"stream_count", 4}, {"stream_order", stream_order},
        {"identity", {
            {"recording_id", recording_id}, {"session_id", recording_id},
            {"recording_identity_token", token},
            {"producer_generation", generation},
            {"spatial_roi_plan_sha256", plan_digest}}},
        {"camera", {{"camera_id", 0}, {"camera_serial", camera_serial},
                     {"native_raster", native_raster}}},
        {"camera_id", 0}, {"camera_serial", camera_serial},
        {"native_raster", native_raster},
        {"authorities", {{"layout", authority_layout},
                          {"materialization", authority_materialization},
                          {"registration", authority_registration}}},
        {"gpu_mapping", {
            {"analytics_gpu_by_camera_serial", {{camera_serial, 1}}},
            {"recorder_gpu_by_logical_stream_id", recorder_gpus}}},
        {"artifacts", {
            {"normalized_config", {{"relative_path", "spatial_roi_recording_config.json"},
                                    {"size_bytes", 10},
                                    {"sha256", snapshot_test_digest('1')}}},
            {"verified_plan", {{"relative_path", "spatial_roi_recording_plan.json"},
                                {"size_bytes", 11},
                                {"sha256", snapshot_test_digest('2')}}},
            {"recorder_contract", {{"relative_path", "spatial_roi_recorder_contract.json"},
                                    {"size_bytes", 12},
                                    {"sha256", snapshot_test_digest('3')}}}}},
        {"rois", rois},
        {"finalized_session_receipt", status == "complete" ? receipt : nullptr},
        {"recorder_process_status", status == "complete"
                                         ? make_complete_recorder_process_status()
                                         : nlohmann::json{{"state", "ready"}}},
        {"producer_status", status == "complete"
                                 ? producer_status
                                 : nlohmann::json{{"state", "armed"}}}};

    return {nlohmann::json{
                {"schema_id", "orange.recording_outputs"},
                {"schema_version", 3},
                {"cameras", {{camera_serial, {{"full", full_output},
                                               {"spatial_roi", spatial_roi_descriptors}}}}}},
            session};
}

nlohmann::json make_media_policy_fixture(const std::string& policy_name)
{
    const bool full_frame = policy_name != "fixed_rois_with_registered_context";
    const bool fixed_rois = policy_name != "full_frame_only";
    const bool registered_context =
        policy_name == "fixed_rois_with_registered_context";
    return {
        {"schema_id", "orange.spatial_roi_recording.media_policy"},
        {"schema_version", 1},
        {"media_policy", policy_name},
        {"retained_products", {{"full_frame", full_frame},
                                {"fixed_rois", fixed_rois},
                                {"registered_context", registered_context}}},
        {"sink_backend", nullptr}};
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name))
    {
        if (const char* value = std::getenv(name_.c_str())) {
            had_original_ = true;
            original_ = value;
        }
        unsetenv(name_.c_str());
    }

    ~ScopedEnv()
    {
        if (had_original_) {
            setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void Set(const std::string& value)
    {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void Unset()
    {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    bool had_original_ = false;
    std::string original_;
};

CameraParams make_camera_params()
{
    CameraParams camera_params{};
    camera_params.camera_id = 3;
    camera_params.camera_serial = "700123";
    camera_params.config_path = "/configs/cam700123.json";
    camera_params.gpu_id = 1;
    camera_params.frame_rate = 250;
    return camera_params;
}

void test_detect_model_snapshot_enabled()
{
    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.yolo = true;
    camera_select.yolo_model = nullptr;

    const nlohmann::json snapshot = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");

    require(snapshot.at("enabled").get<bool>(), "detect snapshot should be enabled");
    require(snapshot.at("source").at("ui_selected").get<bool>(),
            "detect snapshot should record ui selection");
    require(snapshot.at("source").at("camera_config_path").get<std::string>() ==
                "/configs/cam700123.json",
            "detect snapshot should propagate camera config path");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "YoloWorker",
            "detect snapshot should name YoloWorker");
    require(runtime.at("backend").get<std::string>() == "tensorrt",
            "enabled detect snapshot should use tensorrt backend");
    require(runtime.at("engine_path").get<std::string>() == "/models/global.engine",
            "detect snapshot should propagate the selected engine path");
    require(runtime.at("model_id").get<std::string>() ==
                build_model_id_from_path("/models/global.engine"),
            "detect snapshot model_id should match build_model_id_from_path");
    require(runtime.at("gpu_id").get<int>() == 1,
            "detect snapshot should propagate gpu_id");
}

void test_detect_model_snapshot_per_camera_override_and_disabled()
{
    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.yolo = true;
    camera_select.yolo_model = "/models/override.engine";

    const nlohmann::json overridden = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");
    require(overridden.at("runtime").at("engine_path").get<std::string>() ==
                "/models/override.engine",
            "per-camera yolo model should override the global selection");

    camera_select.yolo = false;
    const nlohmann::json disabled = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");
    require(!disabled.at("enabled").get<bool>(), "detect snapshot should be disabled");
    require(disabled.at("runtime").at("backend").get<std::string>() == "none",
            "disabled detect snapshot should have backend none");
    require(disabled.at("runtime").at("engine_path").get<std::string>().empty(),
            "disabled detect snapshot should have empty engine path");
    require(disabled.at("runtime").at("model_id").get<std::string>() == "none",
            "disabled detect snapshot should have model_id none");
}

void test_crop_output_snapshot_enabled()
{
    ScopedEnv pool_env("ORANGE_CROP_FRAME_POOL_SIZE");
    ScopedEnv preview_env("ORANGE_CROP_PREVIEW_MAX_FPS");
    pool_env.Set("48");
    preview_env.Set("17");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.crop_and_encode = true;

    const nlohmann::json snapshot =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);

    require(snapshot.at("schema_version").get<int>() == 1,
            "crop snapshot schema_version should be 1");
    require(snapshot.at("enabled").get<bool>(), "crop snapshot should be enabled");
    require(snapshot.at("mode").get<std::string>() == "yolo_centered_square",
            "enabled crop snapshot mode should be yolo_centered_square");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "CropAndEncodeWorker",
            "crop snapshot should name CropAndEncodeWorker");
    require(runtime.at("crop_size_px").get<int>() == sanitize_camera_crop_size_px(512),
            "crop snapshot should sanitize crop size");
    require(runtime.at("crop_frame_pool_size").get<int>() == 48,
            "crop snapshot should resolve pool size from environment");
    require(runtime.at("preview_max_fps").get<int>() ==
                sanitize_camera_crop_preview_max_fps(17),
            "crop snapshot should resolve preview max fps from environment");
    require(runtime.at("frame_rate").get<unsigned int>() == 250u,
            "crop snapshot should propagate frame rate");
    const nlohmann::json& files = runtime.at("files");
    require(files.at("video").get<std::string>() == "Cam700123_crop.mp4",
            "crop snapshot should derive video file name from serial");
    require(files.at("metadata").get<std::string>() == "Cam700123_crop_meta.csv",
            "crop snapshot should derive metadata file name from serial");
    require(files.at("keyframes").get<std::string>() == "Cam700123_crop_keyframe.json",
            "crop snapshot should derive keyframe file name from serial");
}

void test_crop_output_snapshot_pool_size_fallback_and_disabled()
{
    ScopedEnv pool_env("ORANGE_CROP_FRAME_POOL_SIZE");
    ScopedEnv preview_env("ORANGE_CROP_PREVIEW_MAX_FPS");
    pool_env.Set("not-a-number");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.crop_and_encode = true;

    const nlohmann::json enabled =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);
    require(enabled.at("runtime").at("crop_frame_pool_size").get<int>() ==
                CropProducer::kDefaultCropFramePoolSize,
            "invalid pool size env should fall back to the default");

    camera_select.crop_and_encode = false;
    const nlohmann::json disabled =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);
    require(!disabled.at("enabled").get<bool>(), "crop snapshot should be disabled");
    require(disabled.at("mode").get<std::string>() == "disabled",
            "disabled crop snapshot mode should be disabled");
    require(disabled.at("runtime").at("width").get<int>() == 0,
            "disabled crop snapshot width should be 0");
    require(disabled.at("runtime").at("files").is_object() &&
                disabled.at("runtime").at("files").empty(),
            "disabled crop snapshot should have no files");
}

void test_pose_model_snapshot()
{
    ScopedEnv engine_env("ORANGE_POSE_ENGINE_PATH");
    ScopedEnv skeleton_id_env("ORANGE_POSE_SKELETON_ID");
    ScopedEnv skeleton_path_env("ORANGE_POSE_SKELETON_PATH");
    engine_env.Set("/models/pose.engine");
    skeleton_id_env.Set("mouse20");
    skeleton_path_env.Set("/models/mouse20.json");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.pose = true;

    const nlohmann::json snapshot =
        build_gui_pose_model_snapshot(camera_params, camera_select);

    require(snapshot.at("enabled").get<bool>(), "pose snapshot should be enabled");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "PoseWorker",
            "pose snapshot should name PoseWorker");
    require(runtime.at("engine_path").get<std::string>() == "/models/pose.engine",
            "pose snapshot should propagate engine path from environment");
    require(runtime.at("model_id").get<std::string>() ==
                build_model_id_from_path("/models/pose.engine"),
            "pose snapshot model_id should match build_model_id_from_path");
    require(runtime.at("skeleton_id").get<std::string>() == "mouse20",
            "pose snapshot should propagate skeleton id from environment");
    require(runtime.at("skeleton_path").get<std::string>() == "/models/mouse20.json",
            "pose snapshot should propagate skeleton path from environment");
    require(runtime.at("files").at("perf").get<std::string>() ==
                "Cam700123_pose_perf.csv",
            "pose snapshot should derive perf file name from serial");

    camera_select.pose = false;
    const nlohmann::json disabled =
        build_gui_pose_model_snapshot(camera_params, camera_select);
    require(!disabled.at("enabled").get<bool>(), "pose snapshot should be disabled");
    require(disabled.at("runtime").at("skeleton_id").get<std::string>() == "none",
            "disabled pose snapshot skeleton_id should be none");
}

void test_spatial_calibration_artifact_resolution()
{
    require(spatial_calibration_artifact_env_name("700123") ==
                "ORANGE_SPATIAL_CALIBRATION_ARTIFACT_700123",
            "artifact env name should append the camera serial");

    ScopedEnv artifact_env("ORANGE_SPATIAL_CALIBRATION_ARTIFACT_700123");
    require(resolve_gui_spatial_calibration_artifact_path("700123").empty(),
            "unset artifact env should resolve to an empty path");

    artifact_env.Set("/artifacts/cam700123_calibration.json");
    require(resolve_gui_spatial_calibration_artifact_path("700123") ==
                "/artifacts/cam700123_calibration.json",
            "set artifact env should resolve to its value");

    require(resolve_gui_spatial_calibration_artifact_path("").empty(),
            "empty serial should resolve to an empty path");
}

void test_gui_camera_has_acquisition_work()
{
    CameraEachSelect camera_select;
    camera_select.stream_on = false;
    require(!gui_camera_has_acquisition_work(camera_select),
            "idle camera should have no acquisition work");

    camera_select.stream_on = true;
    require(gui_camera_has_acquisition_work(camera_select),
            "streaming camera should have acquisition work");

    camera_select.stream_on = false;
    camera_select.record = true;
    require(gui_camera_has_acquisition_work(camera_select),
            "recording camera should have acquisition work");

    camera_select.record = false;
    camera_select.frame_save_state = State_Write_New_Frame;
    require(gui_camera_has_acquisition_work(camera_select),
            "frame-saving camera should have acquisition work");
}

void test_citrus_runtime_geometry_unavailable_is_nonblocking()
{
    ScopedEnv socket_env("ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET");
    const std::string unique_suffix = std::to_string(static_cast<long long>(getpid()));
    socket_env.Set("/tmp/orange_missing_citrus_daily_" + unique_suffix + ".sock");

    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_recording_snapshot_daily_" + unique_suffix);
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    {
        std::ofstream output(recording_folder / "recording_snapshot.json");
        output << nlohmann::json{{"schema_version", 2}}.dump(2) << '\n';
    }

    update_gui_citrus_runtime_geometry_snapshot(recording_folder.string());

    nlohmann::json snapshot;
    {
        std::ifstream input(recording_folder / "recording_snapshot.json");
        input >> snapshot;
    }
    const auto& citrus = snapshot.at("citrus_runtime_geometry");
    require(citrus.at("capture_status").get<std::string>() == "unavailable",
            "missing Citrus must be recorded explicitly as unavailable");
    require(!citrus.at("recording_blocked_by_capture_failure").get<bool>(),
            "missing Citrus must not block recording");
    require(citrus.at("daily_registration_optional").get<bool>(),
            "daily registration must remain optional");
    require(citrus.at("mode").get<std::string>() == "unknown",
            "unavailable Citrus must not fabricate a runtime mode");
    require(citrus.at("daily_registration_status").get<std::string>() ==
                "unavailable",
            "unavailable Citrus must use the unavailable daily status");

    std::filesystem::remove_all(recording_folder);
}

void test_recording_geometry_assets_materialize_exact_scoped_sources()
{
    ScopedEnv image_copy_env("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES");
    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_assets_" + suffix);
    std::filesystem::remove_all(root);
    const std::filesystem::path sources = root / "sources";
    const std::filesystem::path recording = root / "recording";
    const std::filesystem::path recording_with_images =
        root / "recording_with_images";
    std::filesystem::create_directories(recording);
    std::filesystem::create_directories(recording_with_images);
    write_exact_fixture(
        recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    write_exact_fixture(
        recording_with_images / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");

    const std::filesystem::path homography_candidate =
        sources / "homography" / "candidate.json";
    const std::filesystem::path homography_yaml =
        sources / "homography" / "homography.yml";
    const std::filesystem::path homography_overlay =
        sources / "homography" / "detection_overlay.png";
    const std::filesystem::path homography_capture =
        sources / "homography_capture.png";
    write_exact_fixture(homography_overlay, "fake homography overlay\n");
    write_exact_fixture(homography_capture, "fake homography capture\n");
    write_exact_fixture(
        homography_candidate,
        nlohmann::json{
            {"schema_id", "citrus.calibration.homography_candidate"},
            {"debug_outputs", {{"detection_overlay", "detection_overlay.png"}}},
            {"source", {{"image_path", homography_capture.string()}}},
        }.dump(2) + "\n");
    write_exact_fixture(homography_yaml, "homography_matrix: [1, 0, 0]\n");
    const std::filesystem::path homography_active =
        sources / "homography_active.json";
    const nlohmann::json homography_active_json = {
        {"schema_id", "citrus.calibration.active_homography"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"candidate_json_path", homography_candidate.string()},
        {"candidate_json_checksum", sha256_fixture(homography_candidate)},
        {"homography_yaml_path", homography_yaml.string()},
        {"homography_yaml_checksum", sha256_fixture(homography_yaml)},
    };
    write_exact_fixture(
        homography_active, homography_active_json.dump(2) + "  \n");

    const std::filesystem::path scale_candidate =
        sources / "scale" / "candidate.json";
    const std::filesystem::path scale_overlay =
        sources / "scale" / "overlay.png";
    const std::filesystem::path scale_capture =
        sources / "scale_capture.png";
    const std::filesystem::path scale_observation =
        sources / "scale" / "observation.json";
    write_exact_fixture(scale_candidate, nlohmann::json{
        {"schema_id", "citrus.calibration.projected_surface_scale_candidate"},
    }.dump(2) + "\n");
    write_exact_fixture(scale_overlay, "fake scale overlay\n");
    write_exact_fixture(scale_capture, "fake scale capture\n");
    write_exact_fixture(
        scale_observation,
        nlohmann::json{
            {"schema_id", "orange.calibration.projected_surface_scale_observation"},
            {"artifact_paths", {{"overlay_png", scale_overlay.string()}}},
            {"source_capture", {{"image_path", scale_capture.string()}}},
        }.dump(2) + "\n");
    const std::filesystem::path scale_active = sources / "scale_active.json";
    const nlohmann::json scale_active_json = {
        {"schema_id", "citrus.calibration.active_projected_surface_scale"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"candidate_json_path", scale_candidate.string()},
        {"candidate_json_checksum", sha256_fixture(scale_candidate)},
        {"source_observation", {
            {"path", scale_observation.string()},
            {"sha256", sha256_fixture(scale_observation)},
        }},
    };
    write_exact_fixture(scale_active, scale_active_json.dump(2) + "\n");

    const std::filesystem::path spatial = sources / "spatial_cam1";
    write_exact_fixture(
        spatial / "manifest.json",
        nlohmann::json{
            {"schema_id", "orange.calibration.manifest"},
            {"summary", {
                {"camera_serial", "cam1"},
                {"arena_id", "arena_1"},
            }},
        }.dump(2) + "\n");
    write_exact_fixture(spatial / "measurement.json", "{\"measurement\":1}\n");
    write_exact_fixture(
        spatial / "arena_layout_runtime.json", "{\"arena_runtime\":1}\n");
    write_exact_fixture(
        spatial / "dish_mask_runtime.json", "{\"dish_runtime\":1}\n");
    write_exact_fixture(spatial / "accepted_overlay.png", "fake rim overlay\n");

    const std::filesystem::path tank_design = sources / "palm1.json";
    write_exact_fixture(
        tank_design,
        nlohmann::json{
            {"schema_id", "citrus.tank_design_spec"},
            {"schema_version", 1},
            {"tank_design_id", "palm1"},
            {"dimensions", {{"inner_diameter_mm", 80.0}}},
        }.dump(2) + "\n");

    const std::filesystem::path daily_selection =
        sources / "daily_registration" / "runtime_selection.json";
    const std::filesystem::path daily_registration =
        sources / "daily_registration" / "registration.json";
    const std::filesystem::path daily_candidate =
        sources / "daily_registration" / "candidate.json";
    const std::filesystem::path rim_root =
        sources / "daily_registration" / "rim_cam1";
    const std::filesystem::path rim_observation = rim_root / "observation.json";
    const std::filesystem::path rim_manifest = rim_root / "manifest.json";
    const std::filesystem::path rim_image_set = rim_root / "image_set.json";
    const std::filesystem::path rim_spatial_export =
        rim_root / "exports" / "spatial_dish_mask_runtime_v1.json";
    const std::filesystem::path rim_palette_export =
        rim_root / "exports" / "palette_dish_mask_v2.json";
    const std::filesystem::path rim_review =
        rim_root / "overlays" / "top_rim_fit.png";
    const std::filesystem::path rim_valid =
        rim_root / "overlays" / "valid_detection_region.png";
    const std::filesystem::path rim_hough =
        rim_root / "overlays" / "registration_hough_overlay.png";
    const std::filesystem::path rim_source =
        rim_root / "captures" / "source_frame.png";
    write_exact_fixture(daily_selection, "{\"mode\":\"selected_daily_registration\"}\n");
    write_exact_fixture(daily_registration, "{\"registration_id\":\"dailyreg1\"}\n");
    write_exact_fixture(daily_candidate, "{\"candidate_id\":\"dailycandidate1\"}\n");
    const nlohmann::json accepted_inner_rim = {
        {"coordinate_space", "camera_native_pixels"},
        {"target_plane", "dish_top_rim"},
        {"geometry", {
            {"type", "circle"},
            {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
            {"radius_px", 2100.0}}}};
    const nlohmann::json accepted_mask = {
        {"shape", "circle"},
        {"coordinate_space", "camera_native_pixels"},
        {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
        {"radius_px", 2117.0}};
    const nlohmann::json valid_detection_region = {
        {"coordinate_space", "camera_native_pixels"},
        {"purpose", "bounding_box_centroid_detection_gating"},
        {"offset_direction", "outward"},
        {"geometry", {
            {"type", "circle"},
            {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
            {"radius_px", 2117.0}}}};
    write_exact_fixture(rim_observation, nlohmann::json{
        {"schema_id", "orange.calibration.dish_top_rim_observation"},
        {"schema_version", 2},
        {"artifact_id", "dishrim_cam1"},
        {"camera", {{"serial", "cam1"}, {"width", 4512}, {"height", 4512}}},
        {"accepted_inner_rim_boundary", accepted_inner_rim},
        {"accepted_mask", accepted_mask},
        {"valid_detection_region", valid_detection_region},
        {"operator_review", {{"accepted", true}}},
    }.dump(2) + "\n");
    write_exact_fixture(rim_image_set, "{\"purpose\":\"dish_top_rim_observation\"}\n");
    write_exact_fixture(rim_spatial_export, "{\"enabled\":true,\"schema_version\":1}\n");
    write_exact_fixture(rim_palette_export, "{\"version\":\"2.0\",\"shape\":\"circle\"}\n");
    write_exact_fixture(rim_review, "fake top rim review\n");
    write_exact_fixture(rim_valid, "fake valid detection overlay\n");
    write_exact_fixture(rim_hough, "fake hough overlay\n");
    write_exact_fixture(rim_source, "fake source frame\n");
    write_exact_fixture(rim_manifest, nlohmann::json{
        {"schema_id", "orange.calibration.manifest"},
        {"artifact_id", "dishrim_cam1"},
        {"files", {
            {"image_set_json", "image_set.json"},
            {"spatial_dish_mask_runtime_v1",
             "exports/spatial_dish_mask_runtime_v1.json"},
            {"palette_dish_mask_v2", "exports/palette_dish_mask_v2.json"}}},
    }.dump(2) + "\n");
    write_exact_fixture(sources / "unrelated.json", "{\"must_not_copy\":true}\n");

    const nlohmann::json daily_camera_contract = {
        {"camera_serial", "cam1"},
        {"arena_id", "arena_1"},
        {"status", "resolved"},
        {"rim_observation", {
            {"source_path", rim_observation.string()},
            {"sha256", sha256_fixture(rim_observation)}}},
        {"compact_artifacts", {
            {"manifest", {
                {"source_path", rim_manifest.string()},
                {"sha256", sha256_fixture(rim_manifest)}}},
            {"image_set", {
                {"source_path", rim_image_set.string()},
                {"sha256", sha256_fixture(rim_image_set)}}},
            {"spatial_dish_mask_runtime_v1", {
                {"source_path", rim_spatial_export.string()},
                {"sha256", sha256_fixture(rim_spatial_export)}}},
            {"palette_dish_mask_v2", {
                {"source_path", rim_palette_export.string()},
                {"sha256", sha256_fixture(rim_palette_export)}}}}},
        {"optional_evidence", {
            {"review_overlay", {
                {"source_path", rim_review.string()},
                {"declared_checksum", fnv1a64_fixture(rim_review)}}},
            {"valid_detection_overlay", {
                {"source_path", rim_valid.string()},
                {"declared_checksum", fnv1a64_fixture(rim_valid)}}},
            {"registration_hough_overlay", {
                {"source_path", rim_hough.string()},
                {"declared_checksum", fnv1a64_fixture(rim_hough)}}},
            {"source_frame", {
                {"source_path", rim_source.string()},
                {"declared_checksum", fnv1a64_fixture(rim_source)}}}}},
        {"recording_snapshot_entry", {
            {"artifact_id", "dishrim_cam1"},
            {"artifact_schema_id",
             "orange.calibration.dish_top_rim_observation"},
            {"artifact_schema_version", 2},
            {"camera_serial", "cam1"},
            {"arena_id", "arena_1"},
            {"coordinate_space", "camera_native_pixels"},
            {"accepted_inner_rim_boundary", accepted_inner_rim},
            {"accepted_mask", accepted_mask},
            {"valid_detection_region", valid_detection_region},
            {"source", {
                {"path", rim_observation.string()},
                {"sha256", sha256_fixture(rim_observation)}}},
            {"available_for_downstream_detection_gating", true},
            {"active_in_orange_live_detection_pipeline", false},
            {"gating_semantics",
             "bounding_box_centroid_inside_valid_detection_region"}}}};
    const nlohmann::json daily_geometry_contract = {
        {"schema_id", "orange.recording.daily_registration_geometry"},
        {"schema_version", 1},
        {"status", "selected_resolved"},
        {"mode", "selected_daily_registration"},
        {"registration_id", "dailyreg1"},
        {"runtime_selection", {
            {"source_path", daily_selection.string()},
            {"sha256", sha256_fixture(daily_selection)}}},
        {"registration", {
            {"source_path", daily_registration.string()},
            {"sha256", sha256_fixture(daily_registration)}}},
        {"candidate", {
            {"source_path", daily_candidate.string()},
            {"sha256", sha256_fixture(daily_candidate)}}},
        {"cameras", {{"cam1", daily_camera_contract}}}};

    const nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"captured_at_utc", "2026-07-21T12:00:00Z"},
        {"status", "resolved"},
        {"recording_policy", {{"recording_blocked", false}}},
        {"selection", {{"selected_canvas_name", "shadow"}}},
        {"sources", nlohmann::json::object()},
        {"daily_registration_geometry", daily_geometry_contract},
        {"cameras", {{"cam1", {
            {"camera_serial", "cam1"},
            {"arena_id", "arena_1"},
            {"status", "resolved"},
            {"tank_design", {{"tank_design_id", "palm1"}}},
            {"projection_geometry", {
                {"status", "resolved"},
                {"homography", {
                    {"source_path", homography_active.string()},
                    {"source_sha256", sha256_fixture(homography_active)},
                    {"active_pointer_snapshot", homography_active_json},
                }},
                {"scale_models", {{"projected_surface", {
                    {"source_path", scale_active.string()},
                    {"source_sha256", sha256_fixture(scale_active)},
                    {"active_pointer_snapshot", scale_active_json},
                }}}},
            }},
            {"orange_spatial_calibration", {
                {"status", "resolved"},
                {"source_artifact_dir", spatial.string()},
                {"runtime", {{"dish_mask", {{"status", "resolved"}}}}},
            }},
        }}}},
        {"tank_designs", {{"palm1", {
            {"status", "resolved"},
            {"artifact", {
                {"source_path", tank_design.string()},
                {"sha256", sha256_fixture(tank_design)},
                {"snapshot", nlohmann::json::object()},
            }},
        }}}},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array()},
    };

    std::string write_error;
    require(
        write_recording_geometry_contract(
            recording.string(), contract, &write_error),
        "compact geometry asset materialization should succeed: " + write_error);
    const nlohmann::json written_contract = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_geometry_contract.json"));
    const nlohmann::json compact_reference =
        written_contract.at("materialized_assets");
    require(compact_reference.at("status") == "complete",
            "all compact geometry assets should materialize completely");
    const nlohmann::json compact_manifest = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_geometry_assets" / "manifest.json"));
    require(compact_manifest.at("materialized_file_count") == 19,
            "compact bundle should contain static geometry plus eight daily-registration files");
    require(compact_manifest.at("optional_image_evidence_status") == "not_requested",
            "large image evidence should be off by default");
    require(!std::filesystem::exists(
                recording / "recording_geometry_assets" / "cameras" /
                "Camcam1" / "spatial" / "evidence"),
            "default materialization should not copy optional image evidence");
    require(
        read_exact_fixture(
            recording / "recording_geometry_assets" / "cameras" /
            "Camcam1" / "projection" / "homography_active.json") ==
            read_exact_fixture(homography_active),
        "materialized homography pointer must preserve exact source bytes");
    require(read_exact_fixture(
                recording / "recording_geometry_assets" / "tank_designs" /
                "palm1.json") == read_exact_fixture(tank_design),
            "materialized tank definition must preserve exact source bytes");
    require(read_exact_fixture(
                recording / "recording_geometry_assets" / "cameras" /
                "Camcam1" / "daily_registration" / "rim_observation" /
                "observation.json") == read_exact_fixture(rim_observation),
            "recording must retain exact schema-v2 daily rim observation bytes");
    for (const auto& file : compact_manifest.at("files")) {
        require(file.at("source_path") !=
                    (sources / "unrelated.json").string(),
                "unreferenced source files must not enter the recording bundle");
    }
    nlohmann::json snapshot = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_snapshot.json"));
    require(snapshot.at("recording_geometry_contract").at(
                "materialized_assets").at("sha256") ==
                compact_reference.at("sha256"),
            "recording snapshot should carry the exact asset-manifest reference");
    const auto& snapshot_rim = snapshot.at("calibrations").at("cam1").at(
        "dish_top_rim_observation");
    require(snapshot_rim.at("valid_detection_region").at("geometry").at(
                "radius_px") == 2117.0,
            "recording snapshot must directly expose the outward centroid gate");
    require(snapshot_rim.at("recording_local_assets").at(
                "observation_relative_path") ==
                "recording_geometry_assets/cameras/Camcam1/daily_registration/rim_observation/observation.json",
            "snapshot mask must point to its recording-local exact-byte observation");
    const std::string first_contract_bytes = read_exact_fixture(
        recording / "recording_geometry_contract.json");
    require(write_recording_geometry_contract(
                recording.string(), contract, &write_error),
            "an identical retry should safely reuse the verified asset bundle");
    require(read_exact_fixture(recording / "recording_geometry_contract.json") ==
                first_contract_bytes,
            "an identical retry must preserve the immutable contract bytes");

    image_copy_env.Set("1");
    require(write_recording_geometry_contract(
                recording_with_images.string(), contract, &write_error),
            "opt-in image evidence materialization should succeed: " + write_error);
    const nlohmann::json image_manifest = nlohmann::json::parse(
        read_exact_fixture(
            recording_with_images / "recording_geometry_assets" / "manifest.json"));
    require(image_manifest.at("optional_image_evidence_status") == "complete",
            "all requested image evidence should materialize");
    require(image_manifest.at("optional_requested_file_count").get<int>() >= 5,
            "image evidence should include calibration captures and overlays");
    require(std::filesystem::exists(
                recording_with_images / "recording_geometry_assets" /
                "cameras" / "Camcam1" / "spatial" / "evidence" /
                "accepted_overlay.png"),
            "opt-in materialization should copy the spatial evidence image");
    require(std::filesystem::exists(
                recording_with_images / "recording_geometry_assets" /
                "cameras" / "Camcam1" / "daily_registration" /
                "rim_observation" / "evidence" /
                "valid_detection_region.png"),
            "opt-in materialization should copy daily mask review evidence");
    bool found_fnv_verified_daily_overlay = false;
    for (const auto& file : image_manifest.at("files")) {
        if (file.at("role") == "daily_rim_valid_detection_overlay") {
            found_fnv_verified_daily_overlay =
                file.at("declared_checksum_algorithm") == "fnv1a64" &&
                file.at("declared_checksum_verified").get<bool>();
        }
    }
    require(found_fnv_verified_daily_overlay,
            "daily review evidence must verify its declared FNV-1a checksum");

    const nlohmann::json live_targets = nlohmann::json::array({{
        {"camera_id", "cam1"}, {"arena_id", "arena_1"},
        {"registration_path", daily_registration.string()},
        {"registration_sha256", sha256_fixture(daily_registration)},
        {"applied", true}}});
    const nlohmann::json live_daily_runtime = {
        {"mode", "selected_daily_registration"},
        {"daily_registration_status", "selected_valid"},
        {"targets", live_targets}};
    const nlohmann::json live_runtime = {
        {"schema_id", "orange.recording.citrus_runtime_geometry"},
        {"schema_version", 1},
        {"capture_status", "captured"},
        {"daily_registration", {{"runtime", live_daily_runtime}}}};
    require(update_recording_snapshot_citrus_runtime_geometry(
                recording.string(), live_runtime),
            "runtime geometry should update the direct registered-mask view");
    snapshot = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_snapshot.json"));
    require(snapshot.at("calibrations").at("cam1").at(
                "dish_top_rim_observation").at(
                "citrus_runtime_application").at(
                "selected_daily_registration_applied_by_citrus").get<bool>(),
            "exact live registration identity must mark the persisted mask applied");
    require(snapshot.at("citrus_runtime_geometry").at(
                "registered_dish_masks").at("cam1").at(
                "valid_detection_region").at("geometry").at(
                "radius_px") == 2117.0,
            "runtime snapshot must expose the exact per-camera registered mask");

    std::filesystem::remove_all(root);
}

void test_recording_geometry_asset_failure_is_nonblocking()
{
    ScopedEnv image_copy_env("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES");
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_asset_failure_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "recording");
    write_exact_fixture(
        root / "recording" / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    const std::filesystem::path tank = root / "palm1.json";
    write_exact_fixture(tank, "{\"tank_design_id\":\"palm1\"}\n");
    const nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"status", "resolved"},
        {"recording_policy", {{"recording_blocked", false}}},
        {"cameras", nlohmann::json::object()},
        {"tank_designs", {{"palm1", {
            {"status", "resolved"},
            {"artifact", {
                {"source_path", tank.string()},
                {"sha256", "sha256:" + std::string(64, '0')},
            }},
        }}}},
        {"warnings", nlohmann::json::array()},
    };
    std::string error;
    require(write_recording_geometry_contract(
                (root / "recording").string(), contract, &error),
            "asset checksum failure must not block contract persistence: " + error);
    const nlohmann::json written = nlohmann::json::parse(read_exact_fixture(
        root / "recording" / "recording_geometry_contract.json"));
    require(written.at("status") == "resolved",
            "asset-copy failure must not rewrite numerical geometry status");
    require(written.at("materialized_assets").at("status") == "partial",
            "asset-copy failure should be explicit as a partial bundle");
    const nlohmann::json manifest = nlohmann::json::parse(read_exact_fixture(
        root / "recording" / "recording_geometry_assets" / "manifest.json"));
    require(manifest.at("required_failure_count") == 1,
            "checksum failure should be counted in the asset manifest");
    require(manifest.at("materialized_file_count") == 0,
            "checksum-mismatched source bytes must not be copied");
    require(!written.at("recording_policy").at("recording_blocked").get<bool>(),
            "optional geometry assets must preserve non-blocking recording policy");

    const std::filesystem::path malformed_recording = root / "malformed_recording";
    std::filesystem::create_directories(malformed_recording);
    write_exact_fixture(
        malformed_recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    nlohmann::json malformed_contract = contract;
    malformed_contract["tank_designs"]["palm1"]["artifact"]["source_path"] = 17;
    require(write_recording_geometry_contract(
                malformed_recording.string(), malformed_contract, &error),
            "malformed optional asset metadata must not block contract persistence");
    const nlohmann::json malformed_written = nlohmann::json::parse(
        read_exact_fixture(
            malformed_recording / "recording_geometry_contract.json"));
    require(malformed_written.at("materialized_assets").at("status") ==
                "unavailable",
            "materialization exceptions should become explicit unavailable status");
    require(malformed_written.at("materialized_assets").at("reason") ==
                "materialization_exception",
            "materialization exception status should retain its reason");
    std::filesystem::remove_all(root);
}

void test_standalone_physical_registration_recording_materialization()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_recording_physical_materialization_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    const std::filesystem::path sources = root / "sources";
    const std::filesystem::path recording = root / "recording";
    std::filesystem::create_directories(recording);
    write_exact_fixture(
        recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    const std::filesystem::path pointer = sources / "active.json";
    const std::filesystem::path observation = sources / "observation.json";
    const std::filesystem::path manifest = sources / "manifest.json";
    const std::filesystem::path image_set = sources / "image_set.json";
    const std::filesystem::path spatial_export =
        sources / "spatial_dish_mask_runtime_v1.json";
    const std::filesystem::path palette_export =
        sources / "palette_dish_mask_v2.json";
    write_exact_fixture(pointer, "{\"status\":\"selected\"}\n");
    write_exact_fixture(observation, "{\"artifact_id\":\"rim_physical\"}\n");
    write_exact_fixture(manifest, "{\"status\":\"complete\"}\n");
    write_exact_fixture(image_set, "{\"schema_version\":2}\n");
    write_exact_fixture(spatial_export, "{\"schema_version\":1}\n");
    write_exact_fixture(palette_export, "{\"schema_version\":2}\n");

    const nlohmann::json snapshot_entry = {
        {"artifact_id", "rim_physical"},
        {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
        {"artifact_schema_version", 2},
        {"camera_serial", "cam1"},
        {"arena_id", ""},
        {"coordinate_space", "camera_native_pixels"},
        {"accepted_inner_rim_boundary", {
            {"coordinate_space", "camera_native_pixels"},
            {"target_plane", "dish_top_rim"},
            {"geometry", {
                {"type", "circle"},
                {"center_px", {{"x", 2250.0}, {"y", 2255.0}}},
                {"radius_px", 2100.0},
            }},
        }},
        {"accepted_mask", {
            {"shape", "circle"},
            {"coordinate_space", "camera_native_pixels"},
            {"center_px", {{"x", 2250.0}, {"y", 2255.0}}},
            {"radius_px", 2105.0},
        }},
        {"valid_detection_region", {
            {"coordinate_space", "camera_native_pixels"},
            {"purpose", "bounding_box_centroid_detection_gating"},
            {"offset_direction", "outward"},
            {"geometry", {
                {"type", "circle"},
                {"center_px", {{"x", 2250.0}, {"y", 2255.0}}},
                {"radius_px", 2105.0},
            }},
        }},
        {"operator_review", {{"accepted", true}}},
        {"source", {
            {"path", observation.string()},
            {"sha256", sha256_fixture(observation)},
            {"intended_recording_relative_path",
             "recording_geometry_assets/cameras/Camcam1/physical_registration/observation.json"},
        }},
        {"available_for_downstream_detection_gating", true},
        {"active_in_orange_live_detection_pipeline", true},
        {"orange_live_detection_pipeline_mode", "gate_and_input_mask"},
        {"active_in_orange_neural_input_mask", true},
        {"gating_semantics",
         "bounding_box_centroid_inside_valid_detection_region"},
    };
    const auto source = [](const std::filesystem::path& path) {
        return nlohmann::json{
            {"source_path", path.string()},
            {"sha256", sha256_fixture(path)},
        };
    };
    const nlohmann::json camera_physical = {
        {"camera_serial", "cam1"},
        {"status", "selected_resolved"},
        {"mode", "selected_physical_registration"},
        {"recording_blocked", false},
        {"artifact_id", "rim_physical"},
        {"active_pointer", source(pointer)},
        {"observation", source(observation)},
        {"manifest", source(manifest)},
        {"compact_artifacts", {
            {"image_set", source(image_set)},
            {"spatial_dish_mask_runtime_v1", source(spatial_export)},
            {"palette_dish_mask_v2", source(palette_export)},
        }},
        {"recording_snapshot_entry", snapshot_entry},
    };
    nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"status", "orange_only"},
        {"recording_policy", {{"recording_blocked", false}}},
        {"cameras", {{"cam1", {
            {"camera_serial", "cam1"},
            {"status", "not_configured"},
            {"physical_registration", camera_physical},
            {"projection_registration", {{"status", "not_applicable"}}},
        }}}},
        {"physical_registration_geometry", {
            {"status", "selected_resolved"},
            {"cameras", {{"cam1", camera_physical}}},
        }},
        {"warnings", nlohmann::json::array()},
        {"analytics_runtime", {{"yolo_spatial_mask", {
            {"status", "armed"},
            {"mode", "gate_and_input_mask"},
        }}}},
    };
    std::string error;
    require(write_recording_geometry_contract(
                recording.string(), contract, &error),
            "complete selected physical evidence must persist: " + error);
    require(read_exact_fixture(
                recording / "recording_geometry_assets" / "cameras" /
                "Camcam1" / "physical_registration" / "observation.json") ==
                read_exact_fixture(observation),
            "physical observation must be copied byte-exactly");
    const nlohmann::json snapshot = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_snapshot.json"));
    const auto& recorded = snapshot.at("calibrations").at("cam1").at(
        "dish_top_rim_observation");
    require(recorded.at("physical_registration").at("selection_status") ==
                "selected_resolved",
            "snapshot must expose standalone physical selection authority");
    require(recorded.at("recording_local_assets").at(
                "observation_relative_path") ==
                "recording_geometry_assets/cameras/Camcam1/physical_registration/observation.json",
            "snapshot must point to the standalone recording-local observation");

    const std::filesystem::path failed_recording = root / "failed_recording";
    std::filesystem::create_directories(failed_recording);
    write_exact_fixture(
        failed_recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    std::filesystem::remove(spatial_export);
    require(!write_recording_geometry_contract(
                failed_recording.string(), contract, &error),
            "enabled live masking must fail closed on incomplete copied evidence");
    require(error.find("requires complete") != std::string::npos,
            "live mask evidence failure must explain the required bundle");

    contract["analytics_runtime"]["yolo_spatial_mask"]["mode"] = "off";
    const std::filesystem::path optional_recording = root / "optional_recording";
    std::filesystem::create_directories(optional_recording);
    write_exact_fixture(
        optional_recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    require(write_recording_geometry_contract(
                optional_recording.string(), contract, &error),
            "mask-off full-frame recording must remain valid with partial optional evidence");
    std::filesystem::remove_all(root);
}

void test_recording_geometry_contract_is_always_written()
{
    ScopedEnv explicit_canvas("ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH");
    ScopedEnv guided_canvas("ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH");
    ScopedEnv centering_canvas("ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH");
    ScopedEnv calibration_base("ORANGE_CALIBRATION_BASE_DIR");
    const std::string unique_suffix = std::to_string(static_cast<long long>(getpid()));
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_" + unique_suffix);
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    calibration_base.Set((recording_folder / "isolated_calibrations").string());
    {
        std::ofstream output(recording_folder / "recording_snapshot.json");
        output << nlohmann::json{{"schema_version", 2}}.dump(2) << '\n';
    }
    CameraParams camera = make_camera_params();
    CameraEachSelect selection;
    selection.stream_on = true;
    update_gui_recording_geometry_contract(
        recording_folder.string(), "", &camera, &selection, 1);

    nlohmann::json contract;
    {
        std::ifstream input(recording_folder / "recording_geometry_contract.json");
        input >> contract;
    }
    require(contract.at("schema_id") == "orange.recording.geometry_contract",
            "recording geometry contract should use its stable schema id");
    require(contract.at("status") == "not_configured",
            "Orange-only run should explicitly record missing optional Citrus selection");
    require(!contract.at("recording_policy").at("recording_blocked").get<bool>(),
            "missing optional Citrus selection must never block recording");

    nlohmann::json snapshot;
    {
        std::ifstream input(recording_folder / "recording_snapshot.json");
        input >> snapshot;
    }
    const auto& reference = snapshot.at("recording_geometry_contract");
    require(reference.at("relative_path") == "recording_geometry_contract.json",
            "recording snapshot should reference the local immutable contract");
    require(reference.at("sha256").get<std::string>().rfind("sha256:", 0) == 0,
            "recording snapshot should checksum the exact contract bytes");
    require(contract.at("materialized_assets").at("status") == "empty",
            "a recording without geometry sources should publish an empty asset manifest");
    require(std::filesystem::exists(
                recording_folder / "recording_geometry_assets" / "manifest.json"),
            "the recording-local geometry asset manifest should always be discoverable");
    std::filesystem::remove_all(recording_folder);
}

void test_immutable_recording_start_snapshot_is_exact_and_create_once()
{
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_recording_start_snapshot_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);

    const std::filesystem::path mutable_path =
        recording_folder / "recording_snapshot.json";
    const std::filesystem::path immutable_path =
        recording_folder / "recording_snapshot_start.json";
    const std::string start_bytes =
        "{\n  \"schema_version\": 2,\n  \"recording_id\": \"run_start\",\n"
        "  \"recording_geometry_contract\": {\"sha256\": \"sha256:geometry\"}\n}\n";
    write_exact_fixture(mutable_path, start_bytes);

    nlohmann::json reference;
    std::string error;
    require(
        seal_immutable_recording_start_snapshot(
            recording_folder.string(), &reference, &error),
        "first immutable snapshot seal should pass: " + error);
    require(read_exact_fixture(immutable_path) == start_bytes,
            "immutable start artifact must preserve the exact source bytes");
    require(reference.at("role") == "immutable_recording_start_snapshot",
            "immutable start artifact role");
    require(reference.at("relative_path") == "recording_snapshot_start.json",
            "immutable start artifact relative path");
    require(reference.at("byte_size").get<std::uint64_t>() == start_bytes.size(),
            "immutable start artifact byte size");
    require(reference.at("sha256") == "sha256:" +
                orange::gui::spatial_layout::checksum::sha256_hex(start_bytes),
            "immutable start artifact exact-byte checksum");

    const nlohmann::json mutable_after_seal =
        nlohmann::json::parse(read_exact_fixture(mutable_path));
    require(mutable_after_seal.at("immutable_recording_start_snapshot") == reference,
            "mutable snapshot must reference the sealed artifact");

    std::error_code permission_error;
    const auto immutable_permissions =
        std::filesystem::status(immutable_path, permission_error).permissions();
    require(!permission_error,
            "immutable start artifact permissions must be inspectable");
    require((immutable_permissions & std::filesystem::perms::owner_write) ==
                std::filesystem::perms::none,
            "immutable start artifact must not be owner-writable");

    require(update_recording_snapshot_session_artifacts(
                recording_folder.string(), {{"finalized", true}}),
            "mutable recording snapshot should remain enrichable after sealing");
    require(read_exact_fixture(immutable_path) == start_bytes,
            "later mutable-snapshot enrichment must not change start evidence");

    nlohmann::json repeated_reference;
    require(
        seal_immutable_recording_start_snapshot(
            recording_folder.string(), &repeated_reference, &error),
        "idempotent immutable snapshot verification should pass: " + error);
    require(repeated_reference == reference,
            "repeated seal should return the original exact reference");

    std::filesystem::permissions(
        immutable_path,
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add,
        permission_error);
    require(!permission_error, "test must be able to simulate artifact tampering");
    require(
        !seal_immutable_recording_start_snapshot(
            recording_folder.string(), nullptr, &error),
        "writable immutable start evidence must fail closed");
    require(error.find("writable") != std::string::npos,
            "writable immutable start evidence should report its policy violation");
    write_exact_fixture(
        immutable_path,
        "{\"recording_id\":\"run_start\",\"tampered\":true}\n");
    std::filesystem::permissions(
        immutable_path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace,
        permission_error);
    require(!permission_error,
            "test must restore read-only permissions after byte tampering");
    require(
        !seal_immutable_recording_start_snapshot(
            recording_folder.string(), nullptr, &error),
        "tampered immutable start evidence must fail closed");
    require(error.find("does not match") != std::string::npos,
            "tampered immutable start evidence should report a digest/reference mismatch");

    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_source_streams_follow_record_selection()
{
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_source_stream_snapshot_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);

    CameraParams cameras[2]{};
    cameras[0].camera_serial = "2010095";
    cameras[0].camera_id = 0;
    cameras[0].width = 4512;
    cameras[0].height = 4512;
    cameras[0].frame_rate = 100;
    cameras[1].camera_serial = "2010096";
    cameras[1].camera_id = 1;
    cameras[1].width = 4512;
    cameras[1].height = 4512;
    cameras[1].frame_rate = 100;
    CameraEachSelect selection[2]{};
    selection[0].record = true;
    selection[1].record = false;
    selection[1].yolo = true;

    require(write_recording_snapshot(
                recording_folder.string(),
                "source_stream_run",
                cameras,
                2,
                recording_folder.parent_path().string(),
                false,
                false,
                nullptr,
                "external_ipc",
                nullptr,
                0,
                selection),
            "recording snapshot with record selection should write");
    const nlohmann::json snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("source_camera_streams").size() == 1 &&
                snapshot.at("source_camera_streams").contains("2010095"),
            "only record-selected cameras should become canonical source streams");
    require(!snapshot.at("source_camera_streams").contains("2010096"),
            "analytics-only camera must not become a recording observation source");
    const auto& source = snapshot.at("source_camera_streams").at("2010095");
    require(source.at("source_camera_stream_id") == "2010095" &&
                source.at("source_camera_stream_identity_policy") ==
                    orange::session::
                        kCameraSerialSourceFrameStreamIdentityPolicy,
            "source stream must freeze the camera-serial identity policy");
    require(snapshot.at("recording_outputs").size() == 1 &&
                snapshot.at("recording_outputs").contains("2010095"),
            "initial recording outputs should follow the same record selection");
    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_explicit_roi_only_policy_omits_full_product()
{
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_roi_only_snapshot_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    const std::filesystem::path snapshot_path =
        recording_folder / "recording_snapshot.json";
    const nlohmann::json initial = {
        {"schema_version", 2},
        {"recording_id", "roi_only"},
        {"source_camera_streams",
         {{"2010093", {{"role", "canonical_acquisition_source"}}}}},
        {"recording_outputs",
         {{"2010093",
           {{"full", {{"output_kind", "full"}, {"status", "disabled"}}},
            {"crop", {{"output_kind", "crop"}, {"status", "pending"}}}}}}},
        {"encoders",
         {{"2010093",
           {{"outputs",
             {{"full", {{"output_kind", "full"}}},
              {"crop", {{"output_kind", "crop"}}}}}}}}},
    };
    write_exact_fixture(snapshot_path, initial.dump(2) + "\n");
    const std::string before = read_exact_fixture(snapshot_path);
    require(!omit_recording_snapshot_full_frame_product(
                recording_folder.string(), "full_frame_only"),
            "full-frame omission must reject every non-ROI-only policy");
    require(read_exact_fixture(snapshot_path) == before,
            "rejected omission must not mutate the snapshot");

    require(omit_recording_snapshot_full_frame_product(
                recording_folder.string(),
                "fixed_rois_with_registered_context"),
            "explicit ROI-only policy must remove the initial full product");
    const nlohmann::json snapshot =
        nlohmann::json::parse(read_exact_fixture(snapshot_path));
    require(!snapshot.at("recording_outputs").at("2010093").contains("full") &&
                snapshot.at("recording_outputs").at("2010093").contains("crop"),
            "ROI-only start snapshot must omit full while preserving other products");
    require(!snapshot.at("encoders").at("2010093").at("outputs").contains("full") &&
                snapshot.at("encoders").at("2010093").at("outputs").contains("crop"),
            "encoder compatibility view must not retain a phantom full product");
    require(snapshot.at("source_camera_streams").contains("2010093"),
            "full-product omission must preserve canonical camera identity");
    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_v3_roi_collection_is_additive()
{
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_spatial_roi_snapshot_v3_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);

    CameraParams camera{};
    camera.camera_serial = "2010096";
    camera.camera_id = 0;
    camera.width = 4512;
    camera.height = 4512;
    camera.frame_rate = 100;
    require(write_recording_snapshot(
                recording_folder.string(),
                "spatial_roi_v3_run",
                &camera,
                1,
                recording_folder.parent_path().string(),
                false,
                false,
                nullptr,
                "external_ipc"),
            "v3 snapshot fixture should write");

    nlohmann::json snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    const nlohmann::json legacy_full_before =
        snapshot.at("recording_outputs").at("2010096").at("full");
    const nlohmann::json v3_first = {
        {"schema_id", "orange.recording_outputs"},
        {"schema_version", 3},
        {"cameras", {
            {"2010096", {
                {"full", legacy_full_before},
                {"spatial_roi", {
                    {"2010096_spatial_roi_roi_1", {
                        {"schema_version", 3},
                        {"camera_serial", "2010096"},
                        {"output_kind", "spatial_roi"},
                        {"logical_stream_id", "2010096_spatial_roi_roi_1"},
                        {"role", "sidecar"},
                        {"status", "pending"}
                    }}
                }}
            }}
        }}
    };
    require(update_recording_snapshot_recording_outputs_v3(
                recording_folder.string(),
                v3_first),
            "v3 snapshot updater should write the additive collection");

    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.value("schema_version", 0) == 2,
            "adding v3 outputs must preserve the schema-2 snapshot version");
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                legacy_full_before,
            "v3 output update must not rewrite schema-2 full output");
    require(snapshot.at("recording_outputs_v3").at("schema_version") == 3,
            "snapshot should carry the nested v3 output schema version");
    require(snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                .at("spatial_roi").size() == 1,
            "snapshot should carry the first spatial ROI stream");

    const nlohmann::json v3_second = {
        {"schema_id", "orange.recording_outputs"},
        {"schema_version", 3},
        {"cameras", {
            {"2010096", {
                {"spatial_roi", {
                    {"2010096_spatial_roi_roi_2", {
                        {"camera_serial", "2010096"},
                        {"output_kind", "spatial_roi"},
                        {"logical_stream_id", "2010096_spatial_roi_roi_2"},
                        {"schema_version", 3},
                        {"status", "completed"}
                    }}
                }}
            }}
        }}
    };
    // The historical updater also accepts the versioned envelope, which
    // keeps callers that already use that API source-compatible.
    require(update_recording_snapshot_recording_outputs(
                recording_folder.string(),
                v3_second),
            "legacy snapshot updater should route v3 payloads additively");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    const auto& roi_outputs = snapshot.at("recording_outputs_v3")
                                  .at("cameras")
                                  .at("2010096")
                                  .at("spatial_roi");
    require(roi_outputs.size() == 2,
            "v3 snapshot merge should retain multiple logical ROI streams");
    require(roi_outputs.at("2010096_spatial_roi_roi_2").value(
                "status", std::string()) == "completed",
            "v3 snapshot merge should update the addressed ROI stream");
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                legacy_full_before,
            "repeated v3 updates must leave schema-2 full output untouched");

    nlohmann::json invalid = v3_first;
    invalid["cameras"]["2010096"]["spatial_roi"]
           ["2010096_spatial_roi_roi_1"]["logical_stream_id"] =
        "2010096_spatial_roi_wrong_key";
    require(!update_recording_snapshot_recording_outputs_v3(
                recording_folder.string(),
                invalid),
            "v3 snapshot updater must reject mismatched ROI identity");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("recording_outputs_v3").at("cameras")
                .at("2010096").at("spatial_roi").size() == 2,
            "rejected v3 update must leave committed ROI collection unchanged");

    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_v3_and_session_update_is_atomic()
{
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_spatial_roi_snapshot_atomic_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);

    CameraParams camera{};
    camera.camera_serial = "2010096";
    camera.camera_id = 0;
    camera.width = 4512;
    camera.height = 4512;
    camera.frame_rate = 100;
    require(write_recording_snapshot(
                recording_folder.string(),
                "spatial_roi_atomic_run",
                &camera,
                1,
                recording_folder.parent_path().string(),
                false,
                false,
                nullptr,
                "external_ipc"),
            "atomic v3/session fixture should write");

    const SpatialRoiSnapshotFixture pending_fixture =
        make_spatial_roi_snapshot_fixture("pending");
    const nlohmann::json legacy_session_info = {
        {"recording_mode", "single_clip"},
        {"legacy_status", "retained"}};
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(),
                pending_fixture.recording_outputs_v3,
                legacy_session_info),
            "non-ROI session update should retain compatibility");
    const SpatialRoiSnapshotFixture& fixture = pending_fixture;
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), fixture.recording_outputs_v3,
                {{"spatial_roi_recording", fixture.session}}),
            "combined v3/session update should write");
    nlohmann::json snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("session").at("recording_mode") == "single_clip" &&
                snapshot.at("session").at("legacy_status") == "retained",
            "non-ROI session fields should commit unchanged");
    require(snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                .at("spatial_roi").size() == 4,
            "combined update should commit all four v3 streams");
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                fixture.recording_outputs_v3.at("cameras").at("2010096")
                    .at("full") &&
                snapshot.at("encoders").at("2010096").at("outputs")
                    .at("full") == fixture.recording_outputs_v3.at("cameras")
                        .at("2010096").at("full"),
            "coupled update should project the v3 full output to schema-2 consumers");
    require(snapshot.at("session").at("spatial_roi_recording") ==
                fixture.session,
            "combined update should commit the closed pending session snapshot");
    require(snapshot.at("session").at("recording_backend")
                    .at("spatial_roi_recording") == fixture.session,
            "coupled update should mirror the spatial ROI session under recording_backend");

    const auto rejected_without_mutation = [&](const nlohmann::json& candidate_v3,
                                                const nlohmann::json& candidate_session,
                                                const std::string& message,
                                                const nlohmann::json& expected_session) {
        require(!update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                    recording_folder.string(), candidate_v3,
                    {{"spatial_roi_recording", candidate_session}}),
                message);
        const nlohmann::json after = nlohmann::json::parse(read_exact_fixture(
            recording_folder / "recording_snapshot.json"));
        require(after.at("session").at("spatial_roi_recording") ==
                    expected_session,
                message + ": rejected update changed committed snapshot");
    };

    nlohmann::json minimal_session = fixture.session;
    minimal_session = {{"status", "pending"}};
    rejected_without_mutation(
        fixture.recording_outputs_v3, minimal_session,
        "minimal spatial ROI session payload must reject", fixture.session);

    nlohmann::json mismatched_status = fixture.session;
    mismatched_status["status"] = "failed";
    rejected_without_mutation(
        fixture.recording_outputs_v3, mismatched_status,
        "descriptor/session lifecycle status mismatch must reject", fixture.session);

    nlohmann::json mismatched_identity = fixture.session;
    mismatched_identity["recording_id"] = "substituted_recording";
    rejected_without_mutation(
        fixture.recording_outputs_v3, mismatched_identity,
        "session identity substitution must reject", fixture.session);

    nlohmann::json missing_streams = fixture.recording_outputs_v3;
    missing_streams["cameras"]["2010096"]["spatial_roi"].erase(
        "2010096_spatial_roi_roi_4");
    rejected_without_mutation(
        missing_streams, fixture.session,
        "missing spatial ROI stream must reject", fixture.session);

    nlohmann::json extra_stream = fixture.recording_outputs_v3;
    extra_stream["cameras"]["2010096"]["spatial_roi"]
               ["2010096_spatial_roi_roi_5"] =
        extra_stream["cameras"]["2010096"]["spatial_roi"]
                    ["2010096_spatial_roi_roi_1"];
    rejected_without_mutation(
        extra_stream, fixture.session,
        "extra spatial ROI stream must reject", fixture.session);

    nlohmann::json failed_fixture = make_spatial_roi_snapshot_fixture("failed").session;
    nlohmann::json failed_v3 = make_spatial_roi_snapshot_fixture("failed").recording_outputs_v3;
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), failed_v3,
                {{"spatial_roi_recording", failed_fixture}}),
            "failed spatial ROI coupled update should write with null receipt");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("session").at("spatial_roi_recording").at(
                "finalized_session_receipt").is_null(),
            "failed spatial ROI session must carry a null receipt");

    const SpatialRoiSnapshotFixture complete_fixture =
        make_spatial_roi_snapshot_fixture("complete");
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), complete_fixture.recording_outputs_v3,
                {{"spatial_roi_recording", complete_fixture.session}}),
            "complete spatial ROI coupled update should write with receipt");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("session").at("spatial_roi_recording") ==
                complete_fixture.session,
            "complete coupled update should commit the receipt-gated snapshot");
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                complete_fixture.recording_outputs_v3.at("cameras")
                    .at("2010096").at("full") &&
                snapshot.at("encoders").at("2010096").at("outputs")
                    .at("full") == complete_fixture.recording_outputs_v3
                        .at("cameras").at("2010096").at("full"),
            "terminal coupled update should retain finalized full output in both projections");

    nlohmann::json stale_schema2_full = complete_fixture.recording_outputs_v3
                                             .at("cameras")
                                             .at("2010096")
                                             .at("full");
    stale_schema2_full["stale_terminal_field"] = "stale";
    require(update_recording_snapshot_recording_outputs(
                recording_folder.string(),
                {{"2010096", {{"full", stale_schema2_full}}}}),
            "schema-2 updater should be able to seed a stale full field");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("recording_outputs").at("2010096").at("full")
                .contains("stale_terminal_field"),
            "stale schema-2 full fixture should contain the stale terminal field");
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), complete_fixture.recording_outputs_v3,
                {{"spatial_roi_recording", complete_fixture.session}}),
            "coupled update should replace stale schema-2 full metadata");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(!snapshot.at("recording_outputs").at("2010096").at("full")
                 .contains("stale_terminal_field") &&
                !snapshot.at("encoders").at("2010096").at("outputs")
                     .at("full").contains("stale_terminal_field"),
            "coupled update must remove stale schema-2 full fields from both projections");

    // A non-coupled additive update may leave an old stream and terminal
    // field in the nested index. The authenticated coupled update is
    // authoritative for the four-lane collection and must replace it as a
    // unit rather than overlaying those stale entries.
    nlohmann::json stale_outputs = complete_fixture.recording_outputs_v3;
    stale_outputs["cameras"]["2010096"]["full"]["stale_terminal_field"] =
        "stale";
    stale_outputs["cameras"]["2010096"]["spatial_roi"]
                 ["2010096_spatial_roi_roi_5"] =
        stale_outputs["cameras"]["2010096"]["spatial_roi"]
                    ["2010096_spatial_roi_roi_1"];
    stale_outputs["cameras"]["2010096"]["spatial_roi"]
                 ["2010096_spatial_roi_roi_5"]["logical_stream_id"] =
        "2010096_spatial_roi_roi_5";
    stale_outputs["cameras"]["2010096"]["spatial_roi"]
                 ["2010096_spatial_roi_roi_1"]["stale_terminal_field"] =
        "stale";
    require(update_recording_snapshot_recording_outputs_v3(
                recording_folder.string(), stale_outputs),
            "standalone update should be able to seed stale collection state");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                    .at("spatial_roi").size() == 5,
            "stale collection fixture should contain an extra ROI stream");
    require(snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                    .at("spatial_roi").at("2010096_spatial_roi_roi_1")
                    .contains("stale_terminal_field"),
            "stale collection fixture should contain the stale terminal field");
    require(snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                    .at("full").contains("stale_terminal_field"),
            "stale v3 full fixture should contain the stale terminal field");
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), complete_fixture.recording_outputs_v3,
                {{"spatial_roi_recording", complete_fixture.session}}),
            "coupled update should replace a stale ROI collection");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    const auto& replaced_roi_outputs =
        snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
            .at("spatial_roi");
    require(replaced_roi_outputs.size() == 4 &&
                !replaced_roi_outputs.contains("2010096_spatial_roi_roi_5") &&
                !replaced_roi_outputs.at("2010096_spatial_roi_roi_1")
                     .contains("stale_terminal_field"),
            "coupled update must remove stale streams and terminal fields");
    require(!snapshot.at("recording_outputs_v3").at("cameras").at("2010096")
                 .at("full").contains("stale_terminal_field"),
            "coupled update must remove stale v3 full fields");
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), complete_fixture.recording_outputs_v3,
                {{"recording_backend", {
                    {"spatial_roi_recording", complete_fixture.session}}}}),
            "nested recording_backend spatial ROI snapshot should validate");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("session").at("spatial_roi_recording") ==
                snapshot.at("session").at("recording_backend")
                    .at("spatial_roi_recording"),
            "snapshot session and recording_backend spatial ROI metadata must match exactly");

    nlohmann::json failed_full_outputs = complete_fixture.recording_outputs_v3;
    failed_full_outputs["cameras"]["2010096"]["full"]["status"] = "failed";
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), failed_full_outputs,
                {{"spatial_roi_recording", complete_fixture.session}}),
            "complete ROI snapshot should remain valid with an independently failed full frame");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.is_object(), "failed-full snapshot should remain readable");
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                failed_full_outputs.at("cameras").at("2010096").at("full") &&
                snapshot.at("encoders").at("2010096").at("outputs")
                    .at("full") == failed_full_outputs.at("cameras")
                        .at("2010096").at("full"),
            "failed full output should be projected consistently to schema-2 consumers");
    nlohmann::json failed_full_unavailable_evidence = failed_full_outputs;
    failed_full_unavailable_evidence["cameras"]["2010096"]["full"]
                                    ["packet_count_source"] = "unavailable";
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(),
                failed_full_unavailable_evidence,
                {{"spatial_roi_recording", complete_fixture.session}}),
            "independently failed full output should permit unavailable packet evidence");
    snapshot = nlohmann::json::parse(read_exact_fixture(
        recording_folder / "recording_snapshot.json"));
    require(snapshot.at("recording_outputs").at("2010096").at("full") ==
                failed_full_unavailable_evidence.at("cameras")
                    .at("2010096").at("full") &&
                snapshot.at("encoders").at("2010096").at("outputs")
                    .at("full") == failed_full_unavailable_evidence
                        .at("cameras").at("2010096").at("full"),
            "failed full unavailable evidence should remain exactly projected");
    nlohmann::json bad_full_descriptor = complete_fixture.recording_outputs_v3;
    bad_full_descriptor["cameras"]["2010096"]["full"]["video"] =
        "/absolute/full.mp4";
    rejected_without_mutation(
        bad_full_descriptor, complete_fixture.session,
        "complete finalized full output must use safe relative paths",
        complete_fixture.session);
    bad_full_descriptor = complete_fixture.recording_outputs_v3;
    bad_full_descriptor["cameras"]["2010096"]["full"]["container"] =
        "mkv";
    rejected_without_mutation(
        bad_full_descriptor, complete_fixture.session,
        "complete finalized full output must retain the mp4 container",
        complete_fixture.session);
    bad_full_descriptor = complete_fixture.recording_outputs_v3;
    bad_full_descriptor["cameras"]["2010096"]["full"]["coordinate_space"] =
        "wrong_pixels";
    rejected_without_mutation(
        bad_full_descriptor, complete_fixture.session,
        "complete finalized full output must retain full-frame coordinates",
        complete_fixture.session);
    bad_full_descriptor = complete_fixture.recording_outputs_v3;
    bad_full_descriptor["cameras"]["2010096"]["full"]
                      ["packet_count_source"] = "unverified";
    rejected_without_mutation(
        bad_full_descriptor, complete_fixture.session,
        "complete finalized full output must retain verified packet provenance",
        complete_fixture.session);
    nlohmann::json aliased_full_descriptor = complete_fixture.recording_outputs_v3;
    aliased_full_descriptor["cameras"]["2010096"]["full"]["video"] =
        complete_fixture.recording_outputs_v3.at("cameras").at("2010096")
            .at("spatial_roi").at("2010096_spatial_roi_roi_1")
            .at("details").at("artifacts").at("video");
    rejected_without_mutation(
        aliased_full_descriptor, complete_fixture.session,
        "complete full and ROI artifacts must use disjoint paths",
        complete_fixture.session);
    nlohmann::json missing_complete_full = complete_fixture.recording_outputs_v3;
    missing_complete_full["cameras"]["2010096"].erase("full");
    rejected_without_mutation(
        missing_complete_full, complete_fixture.session,
        "complete ROI snapshot must retain a first-class full output",
        complete_fixture.session);
    nlohmann::json pending_complete_full = complete_fixture.recording_outputs_v3;
    pending_complete_full["cameras"]["2010096"]["full"]["status"] = "pending";
    rejected_without_mutation(
        pending_complete_full, complete_fixture.session,
        "complete ROI snapshot must reject a nonterminal full output",
        complete_fixture.session);
    nlohmann::json failed_full_bad_descriptor = failed_full_outputs;
    failed_full_bad_descriptor["cameras"]["2010096"]["spatial_roi"]
                             ["2010096_spatial_roi_roi_1"]["codec"] = "h264";
    rejected_without_mutation(
        failed_full_bad_descriptor, complete_fixture.session,
        "failed full frame must not bypass ROI descriptor validation",
        complete_fixture.session);

    nlohmann::json substituted_receipt = complete_fixture.session;
    substituted_receipt["finalized_session_receipt"]["stream_order"][0] =
        "wrong_stream";
    rejected_without_mutation(
        complete_fixture.recording_outputs_v3, substituted_receipt,
        "receipt stream-order substitution must reject", complete_fixture.session);

    nlohmann::json substituted_descriptor = complete_fixture.recording_outputs_v3;
    substituted_descriptor["cameras"]["2010096"]["spatial_roi"]
                          ["2010096_spatial_roi_roi_1"]["details"]
                          ["finalized_receipt"]["logical_stream_id"] =
        "wrong_stream";
    rejected_without_mutation(
        substituted_descriptor, complete_fixture.session,
        "descriptor receipt substitution must reject", complete_fixture.session);

    nlohmann::json substituted_geometry = complete_fixture.recording_outputs_v3;
    substituted_geometry["cameras"]["2010096"]["spatial_roi"]
                       ["2010096_spatial_roi_roi_1"]["details"]["geometry"]
                       ["content_rect"]["x"] = 999;
    rejected_without_mutation(
        substituted_geometry, complete_fixture.session,
        "descriptor geometry substitution must reject", complete_fixture.session);

    nlohmann::json substituted_profile = complete_fixture.recording_outputs_v3;
    substituted_profile["cameras"]["2010096"]["spatial_roi"]
                      ["2010096_spatial_roi_roi_1"]["details"]
                      ["encode_profile"]["tuning"] = "lossy";
    rejected_without_mutation(
        substituted_profile, complete_fixture.session,
        "descriptor profile substitution must reject", complete_fixture.session);

    nlohmann::json substituted_gpu = complete_fixture.recording_outputs_v3;
    substituted_gpu["cameras"]["2010096"]["spatial_roi"]
                  ["2010096_spatial_roi_roi_1"]["details"]["recorder_gpu_id"] = 99;
    rejected_without_mutation(
        substituted_gpu, complete_fixture.session,
        "descriptor GPU substitution must reject", complete_fixture.session);

    nlohmann::json substituted_raster = complete_fixture.recording_outputs_v3;
    substituted_raster["cameras"]["2010096"]["spatial_roi"]
                     ["2010096_spatial_roi_roi_1"]["width"] = 27;
    rejected_without_mutation(
        substituted_raster, complete_fixture.session,
        "descriptor raster substitution must reject", complete_fixture.session);

    nlohmann::json substituted_fps = complete_fixture.recording_outputs_v3;
    substituted_fps["cameras"]["2010096"]["spatial_roi"]
                  ["2010096_spatial_roi_roi_1"]["frame_rate"] = 99;
    rejected_without_mutation(
        substituted_fps, complete_fixture.session,
        "descriptor frame-rate substitution must reject", complete_fixture.session);

    nlohmann::json substituted_codec = complete_fixture.recording_outputs_v3;
    substituted_codec["cameras"]["2010096"]["spatial_roi"]
                    ["2010096_spatial_roi_roi_1"]["codec"] = "h264";
    rejected_without_mutation(
        substituted_codec, complete_fixture.session,
        "descriptor codec substitution must reject", complete_fixture.session);

    nlohmann::json substituted_range = complete_fixture.session;
    substituted_range["finalized_session_receipt"]["streams"][0]
                     ["ranges"]["frame_count"] = 2;
    rejected_without_mutation(
        complete_fixture.recording_outputs_v3, substituted_range,
        "receipt range/count substitution must reject", complete_fixture.session);

    nlohmann::json non_dense_range = complete_fixture.session;
    non_dense_range["finalized_session_receipt"]["streams"][0]
                    ["ranges"]["recording_frame_id"]["last"] = 2;
    rejected_without_mutation(
        complete_fixture.recording_outputs_v3, non_dense_range,
        "non-dense receipt range must reject", complete_fixture.session);

    nlohmann::json malformed_descriptor = complete_fixture.recording_outputs_v3;
    malformed_descriptor["cameras"]["2010096"]["spatial_roi"]
                       ["2010096_spatial_roi_roi_1"]["details"]
                       ["identity"]
                       .erase("camera_id");
    rejected_without_mutation(
        malformed_descriptor, complete_fixture.session,
        "malformed descriptor identity must reject without throwing",
        complete_fixture.session);

    nlohmann::json arbitrary_spatial = complete_fixture.session;
    arbitrary_spatial["finalized_session_receipt"] = nullptr;
    rejected_without_mutation(
        complete_fixture.recording_outputs_v3, arbitrary_spatial,
        "complete session without receipt must reject", complete_fixture.session);

    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_v3_selected_profiles_and_direct_fields()
{
    const SnapshotFixtureProfile profiles[] = {
        SnapshotFixtureProfile::legacy_p7_cqp0_gop1,
        SnapshotFixtureProfile::p1_vbr_q20_gop1,
        SnapshotFixtureProfile::active_p1_vbr_q20_gop25};
    for (const SnapshotFixtureProfile profile : profiles) {
        const std::filesystem::path recording_folder =
            std::filesystem::temp_directory_path() /
            ("orange_spatial_roi_profile_snapshot_" +
             std::to_string(static_cast<long long>(getpid())) + "_" +
             std::to_string(static_cast<int>(profile)));
        std::filesystem::remove_all(recording_folder);
        std::filesystem::create_directories(recording_folder);

        CameraParams camera{};
        camera.camera_serial = "2010096";
        camera.camera_id = 0;
        camera.width = 4512;
        camera.height = 4512;
        camera.frame_rate = 100;
        require(write_recording_snapshot(
                    recording_folder.string(), "spatial_roi_profile_run",
                    &camera, 1, recording_folder.parent_path().string(), false,
                    false, nullptr, "external_ipc"),
                "selected-profile fixture should write");
        const SpatialRoiSnapshotFixture fixture =
            make_spatial_roi_snapshot_fixture("complete", profile);
        require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                    recording_folder.string(), fixture.recording_outputs_v3,
                    {{"spatial_roi_recording", fixture.session}}),
                "selected spatial ROI profile should validate");
        std::filesystem::remove_all(recording_folder);
    }

    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_spatial_roi_profile_mismatch_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    CameraParams camera{};
    camera.camera_serial = "2010096";
    camera.camera_id = 0;
    camera.width = 4512;
    camera.height = 4512;
    camera.frame_rate = 100;
    require(write_recording_snapshot(
                recording_folder.string(), "spatial_roi_profile_mismatch_run",
                &camera, 1, recording_folder.parent_path().string(), false,
                false, nullptr, "external_ipc"),
            "profile mismatch fixture should write");
    const SpatialRoiSnapshotFixture fixture =
        make_spatial_roi_snapshot_fixture(
            "complete", SnapshotFixtureProfile::active_p1_vbr_q20_gop25);
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                recording_folder.string(), fixture.recording_outputs_v3,
                {{"spatial_roi_recording", fixture.session}}),
            "active profile baseline should validate");
    const auto rejected_profile_field =
        [&](const char* field, const nlohmann::json& value) {
            nlohmann::json mismatch = fixture.recording_outputs_v3;
            mismatch["cameras"]["2010096"]["spatial_roi"]
                    ["2010096_spatial_roi_roi_1"]["details"][field] = value;
            require(!update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                        recording_folder.string(), mismatch,
                        {{"spatial_roi_recording", fixture.session}}),
                    std::string("mismatched direct ") + field +
                        " must reject");
        };
    rejected_profile_field("gop", 1);
    rejected_profile_field("rate_control_mode", "cqp");
    rejected_profile_field("quality_value", 0);
    std::filesystem::remove_all(recording_folder);
}

void test_recording_snapshot_v3_media_policy_controls_full_output()
{
    const auto seed_snapshot = [](const std::filesystem::path& recording_folder,
                                  const std::string& recording_id) {
        std::filesystem::remove_all(recording_folder);
        std::filesystem::create_directories(recording_folder);
        CameraParams camera{};
        camera.camera_serial = "2010096";
        camera.camera_id = 0;
        camera.width = 4512;
        camera.height = 4512;
        camera.frame_rate = 100;
        require(write_recording_snapshot(
                    recording_folder.string(), recording_id, &camera, 1,
                    recording_folder.parent_path().string(), false, false,
                    nullptr, "external_ipc"),
                "media-policy fixture should write");
    };

    const std::filesystem::path roi_only_folder =
        std::filesystem::temp_directory_path() /
        ("orange_spatial_roi_policy_roi_only_" +
         std::to_string(static_cast<long long>(getpid())));
    seed_snapshot(roi_only_folder, "spatial_roi_policy_roi_only_run");
    require(update_recording_snapshot_session_artifacts(
                roi_only_folder.string(),
                {{"spatial_roi_media_policy",
                  make_media_policy_fixture(
                      "fixed_rois_with_registered_context")}}),
            "ROI-only media policy should persist");
    require(omit_recording_snapshot_full_frame_product(
                roi_only_folder.string(),
                "fixed_rois_with_registered_context"),
            "ROI-only media policy should omit the initial full product");
    SpatialRoiSnapshotFixture roi_only_fixture =
        make_spatial_roi_snapshot_fixture(
            "complete", SnapshotFixtureProfile::active_p1_vbr_q20_gop25);
    roi_only_fixture.recording_outputs_v3["cameras"]["2010096"].erase("full");
    require(update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                roi_only_folder.string(), roi_only_fixture.recording_outputs_v3,
                {{"spatial_roi_recording", roi_only_fixture.session}}),
            "explicit fixed-ROI media policy should allow ROI-only completion");
    const nlohmann::json roi_only_snapshot = nlohmann::json::parse(
        read_exact_fixture(roi_only_folder / "recording_snapshot.json"));
    require(!roi_only_snapshot.at("recording_outputs_v3").at("cameras")
                 .at("2010096").contains("full") &&
                !roi_only_snapshot.at("recording_outputs").at("2010096")
                     .contains("full"),
            "ROI-only completion must retain no full descriptor");
    std::filesystem::remove_all(roi_only_folder);

    const std::filesystem::path combined_folder =
        std::filesystem::temp_directory_path() /
        ("orange_spatial_roi_policy_combined_" +
         std::to_string(static_cast<long long>(getpid())));
    seed_snapshot(combined_folder, "spatial_roi_policy_combined_run");
    require(update_recording_snapshot_session_artifacts(
                combined_folder.string(),
                {{"spatial_roi_media_policy",
                  make_media_policy_fixture("full_frame_and_fixed_rois")}}),
            "combined media policy should persist");
    SpatialRoiSnapshotFixture combined_fixture =
        make_spatial_roi_snapshot_fixture(
            "complete", SnapshotFixtureProfile::active_p1_vbr_q20_gop25);
    combined_fixture.recording_outputs_v3["cameras"]["2010096"].erase("full");
    const std::string combined_snapshot_before = read_exact_fixture(
        combined_folder / "recording_snapshot.json");
    require(!update_recording_snapshot_recording_outputs_v3_and_session_artifacts(
                combined_folder.string(), combined_fixture.recording_outputs_v3,
                {{"spatial_roi_recording", combined_fixture.session}}),
            "combined media policy must require the full descriptor");
    require(read_exact_fixture(combined_folder / "recording_snapshot.json") ==
                combined_snapshot_before,
            "combined missing-full rejection must not mutate the snapshot");
    std::filesystem::remove_all(combined_folder);
}

}  // namespace

int main()
{
    struct NamedTest {
        const char* name;
        void (*fn)();
    };

    const NamedTest tests[] = {
        {"detect_model_snapshot_enabled", &test_detect_model_snapshot_enabled},
        {"detect_model_snapshot_per_camera_override_and_disabled",
         &test_detect_model_snapshot_per_camera_override_and_disabled},
        {"crop_output_snapshot_enabled", &test_crop_output_snapshot_enabled},
        {"crop_output_snapshot_pool_size_fallback_and_disabled",
         &test_crop_output_snapshot_pool_size_fallback_and_disabled},
        {"pose_model_snapshot", &test_pose_model_snapshot},
        {"spatial_calibration_artifact_resolution",
         &test_spatial_calibration_artifact_resolution},
        {"gui_camera_has_acquisition_work", &test_gui_camera_has_acquisition_work},
        {"citrus_runtime_geometry_unavailable_is_nonblocking",
         &test_citrus_runtime_geometry_unavailable_is_nonblocking},
        {"recording_geometry_assets_materialize_exact_scoped_sources",
         &test_recording_geometry_assets_materialize_exact_scoped_sources},
        {"recording_geometry_asset_failure_is_nonblocking",
         &test_recording_geometry_asset_failure_is_nonblocking},
        {"standalone_physical_registration_recording_materialization",
         &test_standalone_physical_registration_recording_materialization},
        {"recording_geometry_contract_is_always_written",
         &test_recording_geometry_contract_is_always_written},
        {"immutable_recording_start_snapshot_is_exact_and_create_once",
         &test_immutable_recording_start_snapshot_is_exact_and_create_once},
        {"recording_snapshot_source_streams_follow_record_selection",
         &test_recording_snapshot_source_streams_follow_record_selection},
        {"recording_snapshot_explicit_roi_only_policy_omits_full_product",
         &test_recording_snapshot_explicit_roi_only_policy_omits_full_product},
        {"recording_snapshot_v3_roi_collection_is_additive",
         &test_recording_snapshot_v3_roi_collection_is_additive},
        {"recording_snapshot_v3_and_session_update_is_atomic",
         &test_recording_snapshot_v3_and_session_update_is_atomic},
        {"recording_snapshot_v3_selected_profiles_and_direct_fields",
         &test_recording_snapshot_v3_selected_profiles_and_direct_fields},
        {"recording_snapshot_v3_media_policy_controls_full_output",
         &test_recording_snapshot_v3_media_policy_controls_full_output},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All recording snapshot tests passed.\n";
    return EXIT_SUCCESS;
}
