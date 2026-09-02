#include "spatial_roi_recorder_terminal_sidecars.h"

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"
#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace orange::spatial_roi::recording {
namespace {

using json = nlohmann::json;
using orange::gui::spatial_layout::checksum::StreamingSha256;

constexpr std::array<std::string_view, 12> kArtifactKinds = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar",
    "evidence", "evidence_manifest"};

constexpr std::array<std::string_view, 6> kSidecarKinds = {
    "perf", "summary", "status", "video_sanity", "recorder_log",
    "transport_sidecar"};

constexpr std::array<std::string_view, 11> kGeometryKeys = {
    "layout", "materialization", "registration", "native_raster",
    "content_rect", "encoded_raster", "encoded_content_rect",
    "content_offset", "padding", "source_coordinate_space",
    "video_coordinate_space"};

// Keep this list identical to the closed recorder-contract profile projection.
// A profile name alone is not authority: every immutable encoder field must be
// present and validated before terminal artifacts are created.
constexpr std::array<std::string_view, 18> kProfileKeys = {
    "profile_id", "codec", "preset", "tuning", "lossless",
    "rate_control_mode", "quality_value", "gop_length", "frame_rate",
    "input_format", "encoded_format", "no_resize", "luma_preserved_exactly",
    "neutral_chroma_value", "aq", "temporal_aq", "lookahead",
    "lookahead_depth"};

constexpr std::size_t kMaxSidecarBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxIdentifierBytes = 512;
constexpr std::size_t kMaxDurationBytes = 64;
constexpr std::uint32_t kMaxRasterDimension = 32768;
constexpr std::uint64_t kMaxRasterPixels = 268ULL * 1000ULL * 1000ULL;

bool fail(std::string* error_out, std::string message)
{
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

void clear_error(std::string* error_out)
{
    if (error_out != nullptr) {
        error_out->clear();
    }
}

bool safe_text(const std::string& value, const std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
        std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
            return ch < 0x20 || ch == 0x7f;
        });
}

bool safe_identifier(const std::string& value)
{
    return safe_text(value, kMaxIdentifierBytes) &&
        std::isalnum(static_cast<unsigned char>(value.front())) &&
        std::all_of(value.begin() + 1, value.end(), [](const unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
        });
}

bool safe_recording_id(const std::string& value)
{
    return safe_text(value, kMaxIdentifierBytes) &&
        !std::isspace(static_cast<unsigned char>(value.front())) &&
        !std::isspace(static_cast<unsigned char>(value.back()));
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(),
                       [](const unsigned char ch) {
                           return (ch >= '0' && ch <= '9') ||
                               (ch >= 'a' && ch <= 'f');
                       });
}

bool exact_geometry_keys(const json& value, std::string* error_out)
{
    if (!value.is_object() || value.size() != kGeometryKeys.size()) {
        return fail(error_out, "binding.geometry is not the exact closed object");
    }
    for (const std::string_view key : kGeometryKeys) {
        if (!value.contains(key)) {
            return fail(error_out,
                        "binding.geometry is missing closed key " +
                            std::string(key));
        }
    }
    return true;
}

bool exact_profile_keys(const json& value, std::string* error_out)
{
    if (!value.is_object() || value.size() != kProfileKeys.size()) {
        return fail(error_out, "binding.encode_profile is not closed");
    }
    for (const std::string_view key : kProfileKeys) {
        if (!value.contains(key)) {
            return fail(error_out,
                        "binding.encode_profile is missing " + std::string(key));
        }
    }
    return true;
}

bool unsigned_json(const json& value, std::uint64_t* output)
{
    if (value.is_boolean() ||
        (!value.is_number_unsigned() && !value.is_number_integer())) {
        return false;
    }
    if (value.is_number_unsigned()) {
        if (output != nullptr) {
            *output = value.get<std::uint64_t>();
        }
        return true;
    }
    const std::int64_t signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        return false;
    }
    if (output != nullptr) {
        *output = static_cast<std::uint64_t>(signed_value);
    }
    return true;
}

bool finite_range(const double value, const double minimum, const double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_duration(const std::string& value, double* parsed_out = nullptr)
{
    if (value.empty() || value.size() > kMaxDurationBytes ||
        std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
            return !(std::isdigit(ch) || ch == '.' || ch == '-' || ch == '+' ||
                     ch == 'e' || ch == 'E');
        })) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    const bool valid = errno == 0 && end == value.c_str() + value.size() &&
        std::isfinite(parsed) && parsed > 0.0;
    if (valid && parsed_out != nullptr) {
        *parsed_out = parsed;
    }
    return valid;
}

bool positive_canonical_rational(const std::string& value,
                                 std::uint64_t* numerator_out = nullptr,
                                 std::uint64_t* denominator_out = nullptr)
{
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const std::size_t slash = value.find('/');
    if (slash == 0 || slash == std::string::npos ||
        slash + 1 >= value.size() || value.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    const auto parse_part = [&](const std::string_view part,
                                std::uint64_t* output) {
        if (part.empty() || (part.size() > 1 && part.front() == '0') ||
            std::any_of(part.begin(), part.end(), [](const unsigned char ch) {
                return !std::isdigit(ch);
            })) {
            return false;
        }
        std::string owned(part);
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 10);
        if (errno != 0 || end != owned.c_str() + owned.size() || parsed == 0) {
            return false;
        }
        *output = static_cast<std::uint64_t>(parsed);
        return true;
    };
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
    if (!parse_part(std::string_view(value).substr(0, slash), &numerator) ||
        !parse_part(std::string_view(value).substr(slash + 1), &denominator) ||
        std::gcd(numerator, denominator) != 1) {
        return false;
    }
    if (numerator_out != nullptr) *numerator_out = numerator;
    if (denominator_out != nullptr) *denominator_out = denominator;
    return true;
}

bool valid_relative_path(const std::string& path)
{
    // The retained artifact root has already checked the component grammar;
    // these checks keep the binding check independent of that implementation
    // detail and keep paths from entering JSON/log output unexpectedly.
    return safe_text(path, kSpatialRoiRecorderArtifactMaxPathBytes) &&
        path.front() != '/' && path.find("//") == std::string::npos &&
        path.find("..") == std::string::npos;
}

bool checked_raster(const json& geometry,
                   std::uint32_t* width_out,
                   std::uint32_t* height_out,
                   std::string* error_out)
{
    if (!exact_geometry_keys(geometry, error_out)) {
        return false;
    }
    const json& raster = geometry.at("encoded_raster");
    if (!raster.is_object() || raster.size() != 2 ||
        !raster.contains("width") || !raster.contains("height")) {
        return fail(error_out, "binding.geometry.encoded_raster is not closed");
    }
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    if (!unsigned_json(raster.at("width"), &width) ||
        !unsigned_json(raster.at("height"), &height) || width == 0 ||
        height == 0 || width > kMaxRasterDimension || height > kMaxRasterDimension ||
        (width & 1U) != 0U || (height & 1U) != 0U ||
        height > std::numeric_limits<std::uint64_t>::max() / width ||
        width * height > kMaxRasterPixels) {
        return fail(error_out,
                    "binding.geometry.encoded_raster must be a bounded even raster");
    }
    if (width_out != nullptr) {
        *width_out = static_cast<std::uint32_t>(width);
    }
    if (height_out != nullptr) {
        *height_out = static_cast<std::uint32_t>(height);
    }
    return true;
}

bool validate_profile(const json& profile, std::string* error_out)
{
    if (!exact_profile_keys(profile, error_out)) {
        return false;
    }

    // The three accepted profiles are deliberately enumerated as complete
    // tuples. In particular, the P1 profiles are lossy and must never carry a
    // claim that luma was preserved exactly.
    const auto is_string_value = [&](const char* key) {
        return profile.at(key).is_string();
    };
    if (!is_string_value("profile_id") || !is_string_value("codec") ||
        !is_string_value("preset") || !is_string_value("tuning") ||
        !is_string_value("rate_control_mode") ||
        !is_string_value("input_format") ||
        !is_string_value("encoded_format") ||
        !profile.at("lossless").is_boolean() ||
        !profile.at("no_resize").is_boolean() ||
        !profile.at("luma_preserved_exactly").is_boolean() ||
        !profile.at("aq").is_boolean() ||
        !profile.at("temporal_aq").is_boolean() ||
        !profile.at("lookahead").is_boolean()) {
        return fail(error_out, "binding.encode_profile has invalid field types");
    }

    std::uint64_t gop = 0;
    std::uint64_t fps = 0;
    std::uint64_t quality = 0;
    std::uint64_t chroma = 0;
    std::uint64_t lookahead_depth = 0;
    if (!unsigned_json(profile.at("quality_value"), &quality) ||
        !unsigned_json(profile.at("gop_length"), &gop) ||
        !unsigned_json(profile.at("frame_rate"), &fps) || fps == 0 ||
        !unsigned_json(profile.at("neutral_chroma_value"), &chroma) ||
        !unsigned_json(profile.at("lookahead_depth"), &lookahead_depth) ||
        chroma != 128 || profile.at("codec") != "hevc" ||
        profile.at("input_format") != "mono8" ||
        profile.at("encoded_format") != "nv12" ||
        !profile.at("no_resize").get<bool>() ||
        profile.at("luma_preserved_exactly") != profile.at("lossless") ||
        profile.at("aq").get<bool>() ||
        profile.at("temporal_aq").get<bool>() ||
        profile.at("lookahead").get<bool>() || lookahead_depth != 0) {
        return fail(error_out,
                    "binding.encode_profile has invalid derived values");
    }

    const bool legacy_lossless =
        profile.at("profile_id") == "hevc_p7_lossless_cqp0_gop1_v1" &&
        profile.at("codec") == "hevc" && profile.at("preset") == "p7" &&
        profile.at("tuning") == "lossless" &&
        profile.at("lossless").get<bool>() &&
        profile.at("rate_control_mode") == "cqp" && quality == 0 && gop == 1;
    const bool p1_low_latency_gop1 =
        profile.at("profile_id") == "hevc_p1_low_latency_vbr_q20_gop1_v1" &&
        profile.at("codec") == "hevc" && profile.at("preset") == "p1" &&
        profile.at("tuning") == "ll" &&
        !profile.at("lossless").get<bool>() &&
        profile.at("rate_control_mode") == "vbr" && quality == 20 && gop == 1;
    const bool p1_low_latency_gop25 =
        profile.at("profile_id") == "hevc_p1_low_latency_vbr_q20_gop25_v1" &&
        profile.at("codec") == "hevc" && profile.at("preset") == "p1" &&
        profile.at("tuning") == "ll" &&
        !profile.at("lossless").get<bool>() &&
        profile.at("rate_control_mode") == "vbr" && quality == 20 && gop == 25;
    if (!legacy_lossless && !p1_low_latency_gop1 && !p1_low_latency_gop25) {
        return fail(error_out,
                    "binding.encode_profile is not an allowed immutable HEVC profile");
    }
    return true;
}

bool validate_binding(const SpatialRoiRecorderTerminalCandidateSidecarConfig& config,
                      std::uint32_t* width_out,
                      std::uint32_t* height_out,
                      std::string* error_out)
{
    if (!config.artifact_root || !config.artifact_root->valid() ||
        config.binding == nullptr || config.video_sanity == nullptr) {
        return fail(error_out,
                    "terminal candidate sidecars require root, binding, and probe result");
    }
    const auto& binding = *config.binding;
    if (binding.contract_schema_id !=
            orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaId ||
        binding.contract_schema_version !=
            orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaVersion ||
        !canonical_sha256(binding.contract_sha256) ||
        binding.contract_mode != orange::session::spatial_roi::kSpatialRoiRecorderContractMode ||
        binding.plan_schema_id != orange::session::spatial_roi::kPlanSchemaId ||
        binding.plan_schema_version != orange::session::spatial_roi::kPlanSchemaVersion ||
        !canonical_sha256(binding.plan_sha256) || !safe_recording_id(binding.recording_id) ||
        !safe_recording_id(binding.session_id) || binding.session_id != binding.recording_id ||
        !canonical_sha256(binding.recording_identity_token) ||
        binding.recording_identity_token !=
            orange::shaman_v2_recording_identity::token_for_recording_id(
                binding.recording_id) ||
        !safe_identifier(binding.producer_generation) || binding.camera_id < 0 ||
        !safe_identifier(binding.camera_serial) || binding.analytics_gpu_id < 0 ||
        binding.source_gpu_id < 0 || !safe_identifier(binding.roi_id) ||
        !safe_identifier(binding.region_id) || !safe_identifier(binding.arena_group_id) ||
        (binding.has_arena_id && !safe_identifier(binding.arena_id)) ||
        !safe_identifier(binding.logical_stream_id) ||
        binding.routing_policy != "single_shard" || binding.recorder_gpu_id < 0 ||
        binding.assigned_gpu_id != binding.recorder_gpu_id ||
        binding.source_gpu_id != binding.analytics_gpu_id ||
        binding.assigned_shard_id != 0 || binding.max_frames_per_stream == 0 ||
        binding.max_frames_per_stream > kSpatialRoiRecorderEvidenceMaxFrames ||
        binding.max_media_bytes_per_stream == 0 ||
        binding.max_media_bytes_per_stream >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        binding.max_evidence_bytes_per_stream == 0 ||
        binding.max_evidence_bytes_per_stream > kSpatialRoiRecorderEvidenceMaxFileBytes ||
        !safe_text(binding.recording_root, kSpatialRoiRecorderArtifactMaxPathBytes) ||
        !safe_text(binding.artifact_root, kSpatialRoiRecorderArtifactMaxPathBytes) ||
        binding.recording_root.front() != '/' || binding.artifact_root.front() != '/') {
        return fail(error_out, "terminal candidate binding identity or limits are invalid");
    }
    const std::filesystem::path expected_recording_root =
        std::filesystem::path(binding.recording_root).lexically_normal();
    const std::filesystem::path opened_recording_root =
        config.artifact_root->opened_recording_root().lexically_normal();
    const std::filesystem::path expected_artifact_root =
        (expected_recording_root / kSpatialRoiRecorderArtifactDirectory).lexically_normal();
    if (expected_recording_root != opened_recording_root ||
        std::filesystem::path(binding.artifact_root).lexically_normal() !=
            expected_artifact_root) {
        return fail(error_out, "terminal candidate root path is not contract-bound");
    }
    if (!validate_profile(binding.encode_profile, error_out) ||
        !checked_raster(binding.geometry_identity, width_out, height_out, error_out)) {
        return false;
    }

    if (binding.expected_artifacts.size() != kArtifactKinds.size()) {
        return fail(error_out, "binding.expected_artifacts must contain exactly twelve artifacts");
    }
    std::set<std::string> paths;
    for (const std::string_view kind : kArtifactKinds) {
        const auto found = binding.expected_artifacts.find(std::string(kind));
        if (found == binding.expected_artifacts.end() ||
            !valid_relative_path(found->second) ||
            !config.artifact_root->IsAllowed(found->second) ||
            !paths.insert(found->second).second) {
            return fail(error_out,
                        "binding.expected_artifacts is not the exact authorized twelve-artifact set");
        }
    }
    return true;
}

bool hash_retained_video(SpatialRoiRecorderArtifactFile& file,
                         const std::uint64_t maximum,
                         std::uint64_t* size_out,
                         std::string* sha_out,
                         std::string* error_out)
{
    if (!size_out || !sha_out) {
        return fail(error_out, "video hash output is null");
    }
    *size_out = 0;
    sha_out->clear();
    struct stat before {};
    if (::fstat(file.borrowed_fd(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0) {
        return fail(error_out, "contract video is not a nonempty regular file");
    }
    const auto raw_size = static_cast<std::uintmax_t>(before.st_size);
    if (raw_size > maximum || raw_size > std::numeric_limits<std::uint64_t>::max()) {
        return fail(error_out, "contract video exceeds the authenticated media bound");
    }
    const std::uint64_t size = static_cast<std::uint64_t>(raw_size);
    if (::lseek(file.borrowed_fd(), 0, SEEK_SET) < 0) {
        return fail(error_out, "could not seek retained contract video");
    }
    StreamingSha256 hasher;
    std::array<std::uint8_t, 1024U * 1024U> buffer{};
    std::uint64_t consumed = 0;
    while (consumed < size) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - consumed));
        const ssize_t count = ::read(file.borrowed_fd(), buffer.data(), requested);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, "could not hash retained contract video");
        }
        if (count == 0) {
            return fail(error_out, "retained contract video truncated during hash");
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
        consumed += static_cast<std::uint64_t>(count);
    }
    struct stat after {};
    if (::fstat(file.borrowed_fd(), &after) != 0 ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size || !file.VerifyCurrentBinding(error_out)) {
        return fail(error_out, "contract video changed while being hashed");
    }
    *size_out = size;
    *sha_out = "sha256:" + hasher.final_hex();
    return true;
}

bool validate_probe(const SpatialRoiRecorderTerminalCandidateSidecarConfig& config,
                    const std::uint32_t expected_width,
                    const std::uint32_t expected_height,
                    std::unique_ptr<SpatialRoiRecorderArtifactFile>* video_file_out,
                    std::string* error_out)
{
    if (!video_file_out) {
        return fail(error_out, "video descriptor output is null");
    }
    video_file_out->reset();
    const auto& binding = *config.binding;
    const auto& probe = *config.video_sanity;
    const std::string& video_path = binding.expected_artifacts.at("video");
    std::uint64_t expected_fps = 0;
    double duration = 0.0;
    std::uint64_t time_base_numerator = 0;
    std::uint64_t time_base_denominator = 0;
    if (probe.artifact_root_identity() != config.artifact_root->artifact_root_identity() ||
        probe.relative_path() != video_path || probe.width() != expected_width ||
        probe.height() != expected_height || probe.frame_count() == 0 ||
        probe.frame_count() > binding.max_frames_per_stream || probe.size_bytes() == 0 ||
        probe.size_bytes() > binding.max_media_bytes_per_stream ||
        !canonical_sha256(probe.sha256()) ||
        !valid_duration(probe.duration_seconds(), &duration) ||
        !safe_text(probe.container(), 128) || !safe_text(probe.codec(), 128) ||
        !safe_text(probe.decoder(), 256) || probe.codec() != "hevc" ||
        !unsigned_json(binding.encode_profile.at("frame_rate"), &expected_fps) ||
        expected_fps == 0 ||
        probe.frame_rate() != std::to_string(expected_fps) + "/1" ||
        !positive_canonical_rational(probe.time_base(),
                                     &time_base_numerator,
                                     &time_base_denominator) ||
        (probe.pixel_format() != "yuv420p" &&
         probe.pixel_format() != "yuvj420p" &&
         probe.pixel_format() != "nv12") ||
        probe.color_range() != "pc" || probe.bit_depth() != 8 ||
        probe.chroma_subsampling() != "4:2:0" ||
        probe.samples().empty() || probe.samples().size() > 5) {
        return fail(error_out, "descriptor-bound video sanity result does not match binding");
    }
    const double expected_duration =
        static_cast<double>(probe.frame_count()) / static_cast<double>(expected_fps);
    const double one_frame = 1.0 / static_cast<double>(expected_fps);
    if (!std::isfinite(expected_duration) ||
        std::fabs(duration - expected_duration) > one_frame) {
        return fail(error_out,
                    "descriptor-bound video duration does not match cadence/cardinality");
    }
    if (probe.has_decoded_pts()) {
        if (probe.first_decoded_pts() > probe.last_decoded_pts() ||
            (probe.frame_count() > 1 &&
             probe.first_decoded_pts() == probe.last_decoded_pts())) {
            return fail(error_out,
                        "descriptor-bound decoded PTS endpoints are invalid");
        }
        const long double span_seconds =
            (static_cast<long double>(probe.last_decoded_pts()) -
             static_cast<long double>(probe.first_decoded_pts())) *
            static_cast<long double>(time_base_numerator) /
            static_cast<long double>(time_base_denominator);
        const long double expected_span =
            static_cast<long double>(probe.frame_count() - 1U) /
            static_cast<long double>(expected_fps);
        if (!std::isfinite(static_cast<double>(span_seconds)) ||
            std::fabs(span_seconds - expected_span) >
                static_cast<long double>(one_frame) +
                    static_cast<long double>(time_base_numerator) /
                        static_cast<long double>(time_base_denominator)) {
            return fail(error_out,
                        "descriptor-bound decoded PTS span does not match cadence");
        }
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(expected_width) * expected_height;
    double max_stddev = 0.0;
    double max_black = 0.0;
    std::uint64_t previous_index = 0;
    bool first = true;
    for (const auto& sample : probe.samples()) {
        if (sample.requested_frame_index >= probe.frame_count() ||
            (!first && sample.requested_frame_index <= previous_index) ||
            !finite_range(sample.mean, 0.0, 255.0) ||
            !finite_range(sample.stddev, 0.0, 255.0) || sample.min > sample.max ||
            sample.max > 255 || !finite_range(sample.black_fraction_lt8, 0.0, 1.0) ||
            !finite_range(sample.white_fraction_gt247, 0.0, 1.0) ||
            sample.decoded_bytes != pixels) {
            return fail(error_out, "descriptor-bound video sanity sample is invalid");
        }
        max_stddev = std::max(max_stddev, sample.stddev);
        max_black = std::max(max_black, sample.black_fraction_lt8);
        previous_index = sample.requested_frame_index;
        first = false;
    }
    if (max_stddev < 5.0 || max_black >= 0.98) {
        return fail(error_out, "descriptor-bound video sanity thresholds are not met");
    }

    std::unique_ptr<SpatialRoiRecorderArtifactFile> video_file;
    if (!config.artifact_root->OpenExistingFile(
            video_path, SpatialRoiRecorderArtifactFileAccess::kReadOnly,
            &video_file, error_out) || !video_file || !video_file->valid() ||
        video_file->artifact_root_identity() != config.artifact_root->artifact_root_identity() ||
        video_file->identity() != probe.video_identity()) {
        return fail(error_out, "probe video inode is not the current contract video");
    }
    std::uint64_t actual_size = 0;
    std::string actual_sha;
    if (!hash_retained_video(*video_file, binding.max_media_bytes_per_stream,
                             &actual_size, &actual_sha, error_out) ||
        actual_size != probe.size_bytes() || actual_sha != probe.sha256()) {
        return fail(error_out, "probe video size or digest does not match retained video");
    }
    *video_file_out = std::move(video_file);
    return true;
}

json candidate_base(const char* schema_id,
                    const std::string& logical_stream_id,
                    const std::uint64_t frame_count)
{
    return {
        {"schema_id", schema_id},
        {"schema_version", kSpatialRoiRecorderTerminalCandidateSchemaVersion},
        {"state", kSpatialRoiRecorderPendingManifestState},
        {"certifying", false},
        {"requires_finalized_evidence_manifest", true},
        {"commit_marker", "evidence_manifest"},
        {"commit_marker_state", "required_finalized"},
        {"logical_stream_id", logical_stream_id},
        {"frame_count", frame_count},
    };
}

bool build_sidecars(const SpatialRoiRecorderTerminalCandidateSidecarConfig& config,
                    const std::uint32_t width,
                    const std::uint32_t height,
                    std::map<std::string, std::string>* bytes_out,
                    std::uint64_t* total_out,
                    std::string* error_out)
{
    if (!bytes_out || !total_out) {
        return fail(error_out, "terminal sidecar byte output is null");
    }
    bytes_out->clear();
    *total_out = 0;
    const auto& binding = *config.binding;
    const auto& probe = *config.video_sanity;
    const std::uint64_t frames = probe.frame_count();
    const std::string& stream = binding.logical_stream_id;
    const std::string& video_path = binding.expected_artifacts.at("video");

    json summary = candidate_base(kSpatialRoiRecorderSummarySchemaId, stream, frames);
    summary["status"] = kSpatialRoiRecorderPendingManifestState;

    json status = candidate_base(kSpatialRoiRecorderStatusSchemaId, stream, frames);
    status["terminal"] = false;

    double mean_sum = 0.0;
    double max_stddev = 0.0;
    double max_black = 0.0;
    json samples = json::array();
    for (const auto& sample : probe.samples()) {
        mean_sum += sample.mean;
        max_stddev = std::max(max_stddev, sample.stddev);
        max_black = std::max(max_black, sample.black_fraction_lt8);
        samples.push_back(json{
            {"requested_frame_index", sample.requested_frame_index},
            {"mean", sample.mean},
            {"stddev", sample.stddev},
            {"min", sample.min},
            {"max", sample.max},
            {"black_fraction_lt8", sample.black_fraction_lt8},
            {"white_fraction_gt247", sample.white_fraction_gt247},
            {"decoded_bytes", sample.decoded_bytes},
        });
    }
    json sanity = candidate_base(kSpatialRoiRecorderVideoSanitySchemaId, stream, frames);
    sanity["video_path"] = video_path;
    sanity["video_size_bytes"] = probe.size_bytes();
    sanity["video_sha256"] = probe.sha256();
    sanity["video_device"] = probe.video_identity().device;
    sanity["video_inode"] = probe.video_identity().inode;
    sanity["content_checked"] = true;
    sanity["content_valid"] = true;
    sanity["status"] = "pass";
    sanity["width"] = width;
    sanity["height"] = height;
    sanity["nb_frames"] = frames;
    sanity["container"] = {
        {"size", std::to_string(probe.size_bytes())},
        {"duration", probe.duration_seconds()},
    };
    sanity["container_name"] = probe.container();
    sanity["codec"] = probe.codec();
    sanity["decoder"] = probe.decoder();
    sanity["timeline"] = {
        {"frame_rate", probe.frame_rate()},
        {"time_base", probe.time_base()},
        {"has_decoded_pts", probe.has_decoded_pts()},
        {"first_decoded_pts", probe.has_decoded_pts()
                                  ? json(probe.first_decoded_pts())
                                  : json(nullptr)},
        {"last_decoded_pts", probe.has_decoded_pts()
                                 ? json(probe.last_decoded_pts())
                                 : json(nullptr)},
    };
    sanity["pixel_semantics"] = {
        {"pixel_format", probe.pixel_format()},
        {"color_range", probe.color_range()},
        {"bit_depth", probe.bit_depth()},
        {"chroma_subsampling", probe.chroma_subsampling()},
    };
    sanity["sampled_frame_count"] = probe.samples().size();
    sanity["mean_luma"] = mean_sum / probe.samples().size();
    sanity["max_stddev"] = max_stddev;
    sanity["max_black_fraction_lt8"] = max_black;
    sanity["thresholds"] = {
        {"max_black_fraction_lt8", 0.98},
        {"min_max_stddev", 5.0},
    };
    sanity["sampled_frames"] = std::move(samples);

    json transport = candidate_base(kSpatialRoiRecorderTransportSchemaId, stream, frames);

    (*bytes_out)["perf"] = "metric,value\n";
    (*bytes_out)["perf"] += std::string("schema_id,") + kSpatialRoiRecorderPerfSchemaId +
        "\nschema_version," +
        std::to_string(kSpatialRoiRecorderTerminalCandidateSchemaVersion) +
        "\nstate," + kSpatialRoiRecorderPendingManifestState +
        "\ncertifying,false\nrequires_finalized_evidence_manifest,true"
        "\ncommit_marker,evidence_manifest\ncommit_marker_state,required_finalized\nlogical_stream_id," +
        stream + "\nframe_count," + std::to_string(frames) + "\n";
    (*bytes_out)["summary"] = summary.dump() + "\n";
    (*bytes_out)["status"] = status.dump() + "\n";
    (*bytes_out)["video_sanity"] = sanity.dump() + "\n";
    (*bytes_out)["recorder_log"] =
        std::string("schema_id=") + kSpatialRoiRecorderLogSchemaId +
        " schema_version=" +
        std::to_string(kSpatialRoiRecorderTerminalCandidateSchemaVersion) +
        " state=" + kSpatialRoiRecorderPendingManifestState +
        " certifying=false requires_finalized_evidence_manifest=true"
        " commit_marker=evidence_manifest" +
        " commit_marker_state=required_finalized logical_stream_id=" + stream +
        " frame_count=" + std::to_string(frames) + "\n";
    (*bytes_out)["transport_sidecar"] = transport.dump() + "\n";

    for (const auto& [kind, value] : *bytes_out) {
        (void)kind;
        if (value.empty() || value.size() > kMaxSidecarBytes ||
            *total_out > std::numeric_limits<std::uint64_t>::max() - value.size()) {
            return fail(error_out, "terminal candidate sidecar exceeds local byte bound");
        }
        *total_out += static_cast<std::uint64_t>(value.size());
    }
    if (*total_out > binding.max_evidence_bytes_per_stream) {
        return fail(error_out,
                    "terminal candidate sidecars alone exceed the evidence budget");
    }
    // This is intentionally only the local six-file sum. The authenticated
    // aggregate evidence budget also charges metadata, keyframes, finalization,
    // JSONL evidence, and the finalized manifest; only evidence finalization
    // can certify that aggregate.
    return true;
}

bool write_all(const int fd, const std::string& bytes, std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset,
                                      bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out,
                        "terminal candidate sidecar write failed: " +
                            std::string(std::strerror(errno)));
        }
        if (count == 0) {
            return fail(error_out, "terminal candidate sidecar write made no progress");
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

bool write_spatial_roi_recorder_terminal_candidate_sidecars(
    const SpatialRoiRecorderTerminalCandidateSidecarConfig& config,
    SpatialRoiRecorderTerminalCandidateSidecarResult* result_out,
    std::string* error_out)
{
    if (result_out == nullptr) {
        return fail(error_out, "terminal sidecar result destination is null");
    }
    *result_out = {};
    clear_error(error_out);
    try {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (!validate_binding(config, &width, &height, error_out)) {
            return false;
        }
        std::unique_ptr<SpatialRoiRecorderArtifactFile> video_file;
        if (!validate_probe(config, width, height, &video_file, error_out)) {
            return false;
        }
        (void)video_file;

        std::map<std::string, std::string> bytes;
        std::uint64_t total = 0;
        if (!build_sidecars(config, width, height, &bytes, &total, error_out)) {
            return false;
        }
        std::map<std::string, std::string> paths;
        for (const std::string_view kind : kSidecarKinds) {
            paths.emplace(std::string(kind), config.binding->expected_artifacts.at(
                                             std::string(kind)));
        }

        std::vector<std::unique_ptr<SpatialRoiRecorderArtifactFile>> files;
        files.reserve(kSidecarKinds.size());
        for (const std::string_view kind_view : kSidecarKinds) {
            const std::string kind(kind_view);
            std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
            if (!config.artifact_root->CreateFile(paths.at(kind), &file,
                                                  error_out) ||
                !file || !file->valid()) {
                return fail(error_out,
                            error_out != nullptr && !error_out->empty()
                                ? *error_out
                                : "terminal candidate sidecar creation failed");
            }
            if (!write_all(file->borrowed_fd(), bytes.at(kind), error_out) ||
                !file->Seal(error_out)) {
                // Any already-created files are deliberately retained as
                // pending residue. Their state can never be interpreted as a
                // completed recording and they are never resumed here.
                return false;
            }
            files.push_back(std::move(file));
        }
        result_out->artifacts = std::move(paths);
        result_out->candidate_bytes = total;
        clear_error(error_out);
        return true;
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("terminal candidate sidecar publication failed: ") +
                        exception.what());
    } catch (...) {
        return fail(error_out, "terminal candidate sidecar publication failed");
    }
}

}  // namespace orange::spatial_roi::recording
