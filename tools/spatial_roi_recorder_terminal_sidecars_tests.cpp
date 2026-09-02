#define ORANGE_SPATIAL_ROI_VIDEO_SANITY_TESTING 1

#include "spatial_roi_recorder_terminal_sidecars.h"

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"
#include "session/spatial_roi_recording_config.h"
#include "session/spatial_roi_recorder_contract.h"
#include "shaman_v2_recording_identity.h"
#include "spatial_roi_recorder_video_sanity.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace recording = orange::spatial_roi::recording;
namespace contract = orange::session::spatial_roi;
using json = nlohmann::json;

constexpr char kVideoBytes[] = "descriptor-bound-gop-profile-video";

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

}  // namespace

namespace orange::spatial_roi::recording {

// The production video-sanity constructor is private. This test-only factory
// is enabled solely in this focused test translation unit and retains the same
// descriptor identity/capability shape as the real probe.
class SpatialRoiRecorderVideoSanityTestFactory final {
public:
    static std::shared_ptr<const SpatialRoiRecorderVideoSanityResult> Create(
        const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& root,
        const std::string& relative_path,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint64_t frame_count,
        std::string* error_out)
    {
        std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
        if (!root || !root->OpenExistingFile(
                         relative_path,
                         SpatialRoiRecorderArtifactFileAccess::kReadOnly,
                         &file,
                         error_out) ||
            !file) {
            return nullptr;
        }
        struct stat status {};
        if (::fstat(file->borrowed_fd(), &status) != 0 || status.st_size <= 0) {
            if (error_out) {
                *error_out = "test video stat failed";
            }
            return nullptr;
        }
        auto shared_file = std::shared_ptr<SpatialRoiRecorderArtifactFile>(
            std::move(file));
        const std::uint64_t size = static_cast<std::uint64_t>(status.st_size);
        return std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>(
            new SpatialRoiRecorderVideoSanityResult(
                root,
                shared_file,
                root->artifact_root_identity(),
                shared_file->identity(),
                relative_path,
                size,
                "sha256:" +
                    orange::gui::spatial_layout::checksum::sha256_hex(
                        std::string(kVideoBytes)),
                "0.01",
                "100/1",
                "1/10000",
                true,
                0,
                0,
                "mov,mp4,m4a,3gp,3g2,mj2",
                "hevc",
                "hevc@test",
                "yuvj420p",
                "pc",
                8,
                "4:2:0",
                width,
                height,
                frame_count,
                {{0, 64.0, 10.0, 0, 200, 0.1, 0.0,
                  static_cast<std::uint64_t>(width) * height}}));
    }
};

}  // namespace orange::spatial_roi::recording

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempTree final {
public:
    TempTree()
    {
        std::string pattern = "/tmp/orange_roi_sidecars_test_XXXXXX";
        std::vector<char> storage(pattern.begin(), pattern.end());
        storage.push_back('\0');
        const char* made = ::mkdtemp(storage.data());
        require(made != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        path_ = made;
    }

    ~TempTree()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot> make_root(
    const fs::path& recording_root)
{
    require(fs::create_directory(recording_root),
            "could not create recording root");
    std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> root;
    std::string error;
    require(recording::SpatialRoiRecorderArtifactRoot::Open(
                recording_root,
                {"video.mp4", "metadata.csv", "keyframes.json", "finalization.json",
                 "perf.csv", "summary.json", "status.json", "sanity.json",
                 "recorder.log", "transport.jsonl", "evidence.jsonl",
                 "manifest.json"},
                &root, &error),
            "could not open artifact root: " + error);
    return std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot>(
        std::move(root));
}

void write_video(const std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot>& root)
{
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> file;
    std::string error;
    require(root->CreateFile("video.mp4", &file, &error),
            "could not create video fixture: " + error);
    std::size_t offset = 0;
    while (offset < sizeof(kVideoBytes) - 1U) {
        const ssize_t count = ::write(file->borrowed_fd(),
                                      kVideoBytes + offset,
                                      sizeof(kVideoBytes) - 1U - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0,
                std::string("video fixture write failed: ") +
                    std::strerror(errno));
        offset += static_cast<std::size_t>(count);
    }
    require(file->Seal(&error), "could not seal video fixture: " + error);
}

json geometry_fixture()
{
    return {
        {"layout", {{"id", "layout_1"}, {"sha256", digest('c')}}},
        {"materialization",
         {{"id", "materialization_1"}, {"sha256", digest('d')}}},
        {"registration",
         {{"id", "registration_1"}, {"sha256", digest('e')}}},
        {"native_raster", {{"width", 640}, {"height", 480}}},
        {"content_rect", {{"x", 0}, {"y", 0}, {"width", 4}, {"height", 4}}},
        {"encoded_raster", {{"width", 4}, {"height", 4}}},
        {"encoded_content_rect",
         {{"x", 0}, {"y", 0}, {"width", 4}, {"height", 4}}},
        {"content_offset", {{"x", 0}, {"y", 0}}},
        {"padding", {{"left", 0}, {"top", 0}, {"right", 0},
                      {"bottom", 0}, {"value_mono8", 0}}},
        {"source_coordinate_space", "camera_native_full_frame_pixels"},
        {"video_coordinate_space", "spatial_roi_encoded_pixels"},
    };
}

json profile_fixture(const std::string& profile_id,
                     const std::string& preset,
                     const std::string& tuning,
                     const bool lossless,
                     const std::string& rate_control_mode,
                     const std::uint32_t quality_value,
                     const std::uint32_t gop_length,
                     const bool luma_preserved_exactly)
{
    return {
        {"profile_id", profile_id},
        {"codec", "hevc"},
        {"preset", preset},
        {"tuning", tuning},
        {"lossless", lossless},
        {"rate_control_mode", rate_control_mode},
        {"quality_value", quality_value},
        {"gop_length", gop_length},
        {"aq", false},
        {"temporal_aq", false},
        {"lookahead", false},
        {"lookahead_depth", 0},
        {"frame_rate", 100},
        {"input_format", "mono8"},
        {"encoded_format", "nv12"},
        {"no_resize", true},
        {"luma_preserved_exactly", luma_preserved_exactly},
        {"neutral_chroma_value", 128},
    };
}

recording::SpatialRoiRecorderEvidenceBinding make_binding(
    const fs::path& recording_root,
    const json& profile)
{
    recording::SpatialRoiRecorderEvidenceBinding binding;
    binding.contract_schema_id = contract::kSpatialRoiRecorderContractSchemaId;
    binding.contract_schema_version = contract::kSpatialRoiRecorderContractSchemaVersion;
    binding.contract_sha256 = digest('a');
    binding.contract_mode = contract::kSpatialRoiRecorderContractMode;
    binding.plan_schema_id = contract::kPlanSchemaId;
    binding.plan_schema_version = contract::kPlanSchemaVersion;
    binding.plan_sha256 = digest('b');
    binding.recording_id = "2026_09_01_00_00_00";
    binding.session_id = binding.recording_id;
    binding.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            binding.recording_id);
    binding.producer_generation = "generation_001";
    binding.camera_id = 7;
    binding.camera_serial = "2010096";
    binding.analytics_gpu_id = 2;
    binding.source_gpu_id = 2;
    binding.roi_id = "roi_1";
    binding.region_id = "region_1";
    binding.arena_group_id = "arena_group_1";
    binding.has_arena_id = false;
    binding.logical_stream_id = "2010096_spatial_roi_roi_1";
    binding.geometry_identity = geometry_fixture();
    binding.encode_profile = profile;
    binding.recorder_gpu_id = 3;
    binding.assigned_gpu_id = 3;
    binding.assigned_shard_id = 0;
    binding.routing_policy = "single_shard";
    binding.recording_root = recording_root.string();
    binding.artifact_root =
        (recording_root / recording::kSpatialRoiRecorderArtifactDirectory).string();
    binding.max_frames_per_stream = 8;
    binding.max_media_bytes_per_stream = 1024 * 1024;
    binding.max_evidence_bytes_per_stream = 8 * 1024 * 1024;
    binding.expected_artifacts = {
        {"video", "video.mp4"},
        {"metadata", "metadata.csv"},
        {"keyframes", "keyframes.json"},
        {"perf", "perf.csv"},
        {"summary", "summary.json"},
        {"status", "status.json"},
        {"video_sanity", "sanity.json"},
        {"finalization", "finalization.json"},
        {"recorder_log", "recorder.log"},
        {"transport_sidecar", "transport.jsonl"},
        {"evidence", "evidence.jsonl"},
        {"evidence_manifest", "manifest.json"},
    };
    return binding;
}

bool run_profile_case(const json& profile, const bool expected_success)
{
    TempTree tree;
    const fs::path recording_root = tree.path() / "recording";
    const auto root = make_root(recording_root);
    write_video(root);
    const auto binding = make_binding(recording_root, profile);
    std::string error;
    const auto probe =
        orange::spatial_roi::recording::SpatialRoiRecorderVideoSanityTestFactory::Create(
            root, "video.mp4", 4, 4, 1, &error);
    require(probe != nullptr, "could not create sanity fixture: " + error);

    recording::SpatialRoiRecorderTerminalCandidateSidecarConfig config;
    config.artifact_root = root;
    config.binding = &binding;
    config.video_sanity = probe.get();
    recording::SpatialRoiRecorderTerminalCandidateSidecarResult result;
    error.clear();
    const bool succeeded =
        recording::write_spatial_roi_recorder_terminal_candidate_sidecars(
            config, &result, &error);
    require(succeeded == expected_success,
            "unexpected sidecar profile acceptance: " + error);
    if (expected_success) {
        require(result.artifacts.size() == 6 && result.candidate_bytes > 0,
                "accepted profile did not publish six sidecars");
    } else {
        require(result.artifacts.empty() && result.candidate_bytes == 0,
                "rejected profile left a sidecar result");
    }
    return succeeded;
}

void test_null_result_destination()
{
    recording::SpatialRoiRecorderTerminalCandidateSidecarConfig config;
    std::string error;
    require(!recording::write_spatial_roi_recorder_terminal_candidate_sidecars(
                config, nullptr, &error),
            "null result destination was accepted");
    require(!error.empty(), "null result destination did not report an error");
}

void test_probe_result_is_required_and_nonforgeable()
{
    TempTree tree;
    const auto root = make_root(tree.path() / "recording");
    recording::SpatialRoiRecorderEvidenceBinding binding;
    recording::SpatialRoiRecorderTerminalCandidateSidecarConfig config;
    config.artifact_root = root;
    config.binding = &binding;

    recording::SpatialRoiRecorderTerminalCandidateSidecarResult result;
    result.candidate_bytes = 123;
    result.artifacts["forged"] = "forged";
    std::string error;
    require(!recording::write_spatial_roi_recorder_terminal_candidate_sidecars(
                config, &result, &error),
            "missing descriptor-bound probe result was accepted");
    require(result.artifacts.empty() && result.candidate_bytes == 0 &&
                !error.empty(),
            "missing probe result did not fail closed or reset output");
}

void test_invalid_authority_is_rejected_before_creation()
{
    TempTree tree;
    const auto root = make_root(tree.path() / "recording");
    recording::SpatialRoiRecorderTerminalCandidateSidecarConfig config;
    config.artifact_root = root;
    recording::SpatialRoiRecorderTerminalCandidateSidecarResult result;
    std::string error;
    require(!recording::write_spatial_roi_recorder_terminal_candidate_sidecars(
                config, &result, &error),
            "missing binding was accepted");
    require(result.artifacts.empty() && result.candidate_bytes == 0 &&
                !fs::exists(root->opened_recording_root() /
                             recording::kSpatialRoiRecorderArtifactDirectory /
                             "perf.csv"),
            "invalid authority left candidate sidecar residue");
}

void test_schema_contract_constants_are_closed()
{
    require(recording::kSpatialRoiRecorderTerminalCandidateSchemaVersion == 1,
            "unexpected terminal candidate schema version");
    require(std::string(recording::kSpatialRoiRecorderPendingManifestState) ==
                "pending_manifest",
            "candidate state is not pending_manifest");
}

void test_accepts_all_immutable_profiles_and_rejects_false_luma()
{
    require(run_profile_case(
                profile_fixture("hevc_p7_lossless_cqp0_gop1_v1", "p7",
                                "lossless", true, "cqp", 0, 1, true),
                true),
            "lossless GOP1 profile was rejected");
    require(run_profile_case(
                profile_fixture("hevc_p1_low_latency_vbr_q20_gop1_v1", "p1",
                                "ll", false, "vbr", 20, 1, false),
                true),
            "legacy P1 GOP1 profile was rejected");
    require(run_profile_case(
                profile_fixture("hevc_p1_low_latency_vbr_q20_gop25_v1", "p1",
                                "ll", false, "vbr", 20, 25, false),
                true),
            "P1 GOP25 profile was rejected");
    require(!run_profile_case(
                profile_fixture("hevc_p1_low_latency_vbr_q20_gop25_v1", "p1",
                                "ll", false, "vbr", 20, 25, true),
                false),
            "lossy GOP25 profile falsely claiming exact luma was accepted");
    for (const char* control : {"aq", "temporal_aq", "lookahead"}) {
        json mutated = profile_fixture(
            "hevc_p1_low_latency_vbr_q20_gop25_v1", "p1", "ll", false,
            "vbr", 20, 25, false);
        mutated[control] = true;
        require(!run_profile_case(mutated, false),
                std::string("GOP25 profile with enabled ") + control +
                    " was accepted");
    }
    json nonzero_lookahead = profile_fixture(
        "hevc_p1_low_latency_vbr_q20_gop25_v1", "p1", "ll", false,
        "vbr", 20, 25, false);
    nonzero_lookahead["lookahead_depth"] = 1;
    require(!run_profile_case(nonzero_lookahead, false),
            "GOP25 profile with nonzero lookahead depth was accepted");
}

}  // namespace

int main()
{
    try {
        test_null_result_destination();
        test_probe_result_is_required_and_nonforgeable();
        test_invalid_authority_is_rejected_before_creation();
        test_schema_contract_constants_are_closed();
        test_accepts_all_immutable_profiles_and_rejects_false_luma();
        std::cout << "[PASS] accepts_all_immutable_profiles_and_rejects_false_luma\n";
        std::cout << "spatial_roi_recorder_terminal_sidecars_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_recorder_terminal_sidecars_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
