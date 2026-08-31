#include "session/spatial_roi_recorder_contract_parser.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;
namespace spatial_roi = orange::session::spatial_roi;

constexpr char kRecordingRoot[] = "/tmp/orange_roi_parser_test";
constexpr char kFirstStream[] = "2010096_spatial_roi_roi_1";
constexpr char kSecondStream[] = "2010096_spatial_roi_roi_2";

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

spatial_roi::RoiConfig make_roi(const std::string& roi_id,
                                const std::string& region_id,
                                const spatial_roi::Rect rect)
{
    spatial_roi::RoiConfig roi;
    roi.roi_id = roi_id;
    roi.region_id = region_id;
    roi.has_arena_id = true;
    roi.arena_id = "arena_" + roi_id;
    roi.required = true;
    roi.content_rect = rect;
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id("2010096", roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem("2010096", roi_id);
    return roi;
}

struct Fixture {
    json plan;
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    json contract;
};

Fixture make_fixture()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 64;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "2010096";
    camera.native_raster = {100, 100};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('2')};
    camera.registration = {"registration_1", digest('3')};
    camera.rois.push_back(make_roi("roi_1", "region_1", {0, 0, 10, 10}));
    camera.rois.push_back(make_roi("roi_2", "region_2", {20, 0, 12, 10}));
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "parser-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_1";

    Fixture fixture;
    std::string error;
    require(spatial_roi::build_plan(
                config, context, &fixture.plan, nullptr, &error),
            error);
    fixture.mapping.analytics_gpu_by_camera_serial.emplace("2010096", 5);
    fixture.mapping.recorder_gpu_by_logical_stream_id.emplace(kFirstStream, 6);
    fixture.mapping.recorder_gpu_by_logical_stream_id.emplace(kSecondStream, 7);
    require(spatial_roi::build_spatial_roi_recorder_contract(
                fixture.plan,
                kRecordingRoot,
                fixture.mapping,
                &fixture.contract,
                &error),
            error);
    return fixture;
}

bool parses(const Fixture& fixture,
            const json& candidate,
            const std::string& stream_id,
            spatial_roi::SpatialRoiRecorderContractView* view,
            std::string* error)
{
    return spatial_roi::parse_spatial_roi_recorder_contract(
        candidate,
        fixture.plan,
        kRecordingRoot,
        fixture.mapping,
        stream_id,
        view,
        error);
}

void require_rejected(const Fixture& fixture,
                      const json& candidate,
                      const std::string& message)
{
    spatial_roi::SpatialRoiRecorderContractView view;
    std::string error;
    require(!parses(fixture, candidate, kFirstStream, &view, &error), message);
    require(!error.empty(), message + " had no diagnostic");
}

void parses_one_plan_bound_selected_stream()
{
    const Fixture fixture = make_fixture();
    spatial_roi::SpatialRoiRecorderContractView view;
    std::string error;
    require(parses(fixture, fixture.contract, kSecondStream, &view, &error), error);
    require(view.schema_version == 1 && view.stream_count == 2,
            "contract envelope was not parsed");
    require(view.ipc_v2.features ==
                std::vector<std::string>{"cuda_ipc", "packed_mono8",
                                         "ack_release", "terminal_error"},
            "active feature view changed");
    require(view.selected_stream.logical_stream_id == kSecondStream &&
                view.selected_stream.roi_id == "roi_2" &&
                view.selected_stream.region_id == "region_2",
            "requested stream identity was not selected");
    require(view.selected_stream.artifacts.at("video").relative_path ==
                "Cam2010096_spatial_roi_roi_2.mp4",
            "artifact relative path was not derived");
    require(view.selected_stream.geometry.content_rect.x == 20 &&
                view.selected_stream.geometry.encoded_raster.width == 12,
            "verified geometry was not exposed");
    require(view.selected_stream.encode_profile.luma_preserved_exactly &&
                view.selected_stream.encode_profile.neutral_chroma_value == 128,
            "Mono8-to-NV12 luma/chroma contract was not parsed");
}

void rejects_schema_feature_and_selector_fallbacks()
{
    const Fixture fixture = make_fixture();
    json candidate = fixture.contract;
    candidate["schema_id"] = "orange.external_recorder.contract";
    require_rejected(fixture, candidate, "legacy contract was accepted");

    candidate = fixture.contract;
    candidate["ipc_v2"]["features"].push_back("drain_finalize");
    require_rejected(fixture, candidate, "unimplemented feature was accepted");

    candidate = fixture.contract;
    candidate["ipc_v2"]["drain_finalize"]["operational"] = true;
    require_rejected(fixture, candidate, "operational drain/finalize was accepted");

    candidate = fixture.contract;
    candidate["unexpected"] = true;
    require_rejected(fixture, candidate, "unknown contract field was accepted");

    spatial_roi::SpatialRoiRecorderContractView view;
    std::string error;
    require(!parses(fixture, fixture.contract, "", &view, &error),
            "empty selector was accepted");
    require(!parses(fixture, fixture.contract, "missing_stream", &view, &error),
            "missing selector silently fell back to another stream");
}

void rejects_mutations_even_when_internal_copies_are_changed()
{
    const Fixture fixture = make_fixture();
    json candidate = fixture.contract;
    const std::string fake_token = digest('b');
    candidate["recording_identity_token"] = fake_token;
    for (auto& [stream_id, stream] : candidate["streams"].items()) {
        (void)stream_id;
        stream["recording_identity_token"] = fake_token;
        stream["identity"]["recording_identity_token"] = fake_token;
    }
    require_rejected(fixture, candidate,
                     "self-consistent but wrong recording token was accepted");

    candidate = fixture.contract;
    const std::string fake_plan = digest('c');
    candidate["spatial_roi_plan_sha256"] = fake_plan;
    for (auto& [stream_id, stream] : candidate["streams"].items()) {
        (void)stream_id;
        stream["spatial_roi_plan_sha256"] = fake_plan;
        stream["identity"]["spatial_roi_plan_sha256"] = fake_plan;
    }
    require_rejected(fixture, candidate,
                     "contract geometry detached from verified plan was accepted");

    candidate = fixture.contract;
    candidate["streams"][kFirstStream]["encode_queue_depth"] =
        std::numeric_limits<std::uint32_t>::max();
    require_rejected(fixture, candidate, "unbounded stream queue was accepted");

    candidate = fixture.contract;
    candidate["streams"][kFirstStream]["geometry_identity"]["padding"]
             ["value_mono8"] = 255;
    require_rejected(fixture, candidate, "nonzero Mono8 padding was accepted");

    candidate = fixture.contract;
    candidate["streams"][kFirstStream]["geometry_identity"]["encoded_raster"]
             ["width"] = 11;
    require_rejected(fixture, candidate, "odd NV12 raster was accepted");

    candidate = fixture.contract;
    candidate["streams"][kFirstStream]["camera_id"] = 4;
    candidate["streams"][kFirstStream]["identity"]["camera_id"] = 4;
    require_rejected(fixture, candidate,
                     "cross-stream camera identity drift was accepted");

    candidate = fixture.contract;
    const std::string first_video =
        candidate["streams"][kFirstStream]["mp4"].get<std::string>();
    candidate["streams"][kSecondStream]["mp4"] = first_video;
    candidate["streams"][kSecondStream]["expected_artifacts"]["video"] =
        first_video;
    require_rejected(fixture, candidate, "colliding artifact path was accepted");

    candidate = fixture.contract;
    candidate["streams"][kFirstStream]["unexpected"] = true;
    require_rejected(fixture, candidate, "unknown stream field was accepted");
}

void rejects_wrong_external_authority()
{
    Fixture fixture = make_fixture();
    spatial_roi::SpatialRoiRecorderContractView view;
    std::string error;
    require(parses(fixture, fixture.contract, kFirstStream, &view, &error),
            error);
    require(!view.streams.empty(), "accepted view fixture was unexpectedly empty");
    require(!spatial_roi::parse_spatial_roi_recorder_contract(
                 fixture.contract,
                 fixture.plan,
                 "/tmp/a-different-recording-root",
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "wrong authoritative recording root was accepted");
    require(view.streams.empty() && view.selected_stream.logical_stream_id.empty(),
            "failed authority validation retained a stale accepted view");

    auto wrong_mapping = fixture.mapping;
    wrong_mapping.recorder_gpu_by_logical_stream_id[kFirstStream] = 9;
    require(!spatial_roi::parse_spatial_roi_recorder_contract(
                 fixture.contract,
                 fixture.plan,
                 kRecordingRoot,
                 wrong_mapping,
                 kFirstStream,
                 &view,
                 &error),
            "wrong authoritative GPU mapping was accepted");

    fixture.plan["plan"]["resolved_cameras"]["2010096"]["rois"][0]
                ["content_rect"]["x"] = 1;
    require(!spatial_roi::parse_spatial_roi_recorder_contract(
                 fixture.contract,
                 fixture.plan,
                 kRecordingRoot,
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "digest-invalid verified plan was accepted");
}

void parses_only_bounded_nonsymlink_regular_file()
{
    const Fixture fixture = make_fixture();
    const std::filesystem::path path =
        "/tmp/spatial_roi_parser_test_contract.json";
    const std::filesystem::path symlink_path =
        "/tmp/spatial_roi_parser_test_contract_link.json";
    const std::filesystem::path duplicate_path =
        "/tmp/spatial_roi_parser_test_contract_duplicate.json";
    const std::filesystem::path oversized_path =
        "/tmp/spatial_roi_parser_test_contract_oversized.json";
    std::filesystem::remove(path);
    std::filesystem::remove(symlink_path);
    std::filesystem::remove(duplicate_path);
    std::filesystem::remove(oversized_path);
    {
        std::ofstream output(path, std::ios::binary);
        require(output.good(), "failed to create parser test contract");
        output << fixture.contract.dump();
    }

    spatial_roi::SpatialRoiRecorderContractView view;
    std::string error;
    require(spatial_roi::parse_spatial_roi_recorder_contract_file(
                path,
                fixture.plan,
                kRecordingRoot,
                fixture.mapping,
                kFirstStream,
                &view,
                &error),
            error);

    std::filesystem::create_symlink(path, symlink_path);
    require(!spatial_roi::parse_spatial_roi_recorder_contract_file(
                 symlink_path,
                 fixture.plan,
                 kRecordingRoot,
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "symlink contract path was accepted");

    std::string nul_path_bytes = "/tmp/spatial_roi_parser_test_contract.json";
    nul_path_bytes.push_back('\0');
    nul_path_bytes += "ignored";
    require(!spatial_roi::parse_spatial_roi_recorder_contract_file(
                 std::filesystem::path(nul_path_bytes),
                 fixture.plan,
                 kRecordingRoot,
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "embedded-NUL contract path was accepted");

    {
        std::ofstream output(duplicate_path, std::ios::binary);
        require(output.good(), "failed to create duplicate-key fixture");
        const std::string canonical = fixture.contract.dump();
        output << "{\"schema_id\":\"duplicate\"," << canonical.substr(1);
    }
    require(!spatial_roi::parse_spatial_roi_recorder_contract_file(
                 duplicate_path,
                 fixture.plan,
                 kRecordingRoot,
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "duplicate JSON object key was accepted");

    {
        std::ofstream output(oversized_path, std::ios::binary);
        require(output.good(), "failed to create oversized contract fixture");
        output.seekp(16U * 1024U * 1024U);
        output.put('x');
    }
    require(!spatial_roi::parse_spatial_roi_recorder_contract_file(
                 oversized_path,
                 fixture.plan,
                 kRecordingRoot,
                 fixture.mapping,
                 kFirstStream,
                 &view,
                 &error),
            "oversized contract file was accepted");

    std::filesystem::remove(path);
    std::filesystem::remove(symlink_path);
    std::filesystem::remove(duplicate_path);
    std::filesystem::remove(oversized_path);
}

}  // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"parses_one_plan_bound_selected_stream",
         parses_one_plan_bound_selected_stream},
        {"rejects_schema_feature_and_selector_fallbacks",
         rejects_schema_feature_and_selector_fallbacks},
        {"rejects_mutations_even_when_internal_copies_are_changed",
         rejects_mutations_even_when_internal_copies_are_changed},
        {"rejects_wrong_external_authority", rejects_wrong_external_authority},
        {"parses_only_bounded_nonsymlink_regular_file",
         parses_only_bounded_nonsymlink_regular_file},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures
                  << " spatial ROI contract parser test(s) failed\n";
        return 1;
    }
    std::cout << "5 spatial ROI contract parser tests passed\n";
    return 0;
}
