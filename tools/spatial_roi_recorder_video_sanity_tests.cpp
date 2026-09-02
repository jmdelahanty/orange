#include "spatial_roi_recorder_video_sanity.h"

#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace recording = orange::spatial_roi::recording;

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
        std::string pattern = "/tmp/orange_spatial_roi_video_test_XXXXXX";
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        const char* created = ::mkdtemp(mutable_pattern.data());
        require(created != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        path_ = created;
    }

    ~TempTree()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot> open_root(
    const fs::path& path)
{
    require(fs::create_directory(path), "could not create recording root");
    std::unique_ptr<recording::SpatialRoiRecorderArtifactRoot> root;
    std::string error;
    require(recording::SpatialRoiRecorderArtifactRoot::Open(
                path, {"video.mp4"}, &root, &error),
            "could not open artifact root: " + error);
    return std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot>(
        std::move(root));
}

void write_file(const recording::SpatialRoiRecorderArtifactRoot& root,
                const std::string& bytes)
{
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> file;
    std::string error;
    require(root.CreateFile("video.mp4", &file, &error),
            "could not create fixture: " + error);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(file->borrowed_fd(),
                                      bytes.data() + offset,
                                      bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0,
                std::string("fixture write failed: ") + std::strerror(errno));
        offset += static_cast<std::size_t>(count);
    }
    require(::fsync(file->borrowed_fd()) == 0,
            std::string("fixture fsync failed: ") + std::strerror(errno));
}

recording::SpatialRoiRecorderVideoSanityRequest request_for(
    const std::shared_ptr<recording::SpatialRoiRecorderArtifactRoot>& root)
{
    recording::SpatialRoiRecorderVideoSanityRequest request;
    request.artifact_root = root;
    request.video_relative_path = "video.mp4";
    request.encoded_width = 16;
    request.encoded_height = 12;
    request.expected_frame_count = 3;
    request.max_media_bytes = 16U * 1024U * 1024U;
    request.expected_frame_rate = 100.0;
    request.timeout = std::chrono::seconds(10);
    return request;
}

void require_rejected(recording::SpatialRoiRecorderVideoSanityRequest request,
                      const std::string& context)
{
    std::unique_ptr<recording::SpatialRoiRecorderVideoSanityResult> result;
    std::string error;
    require(!recording::SpatialRoiRecorderVideoSanityProbe::Run(
                request, &result, &error),
            context + " was unexpectedly accepted");
    require(result == nullptr && !error.empty(),
            context + " did not fail closed with an error");
}

void require_sample_equal(
    const recording::SpatialRoiRecorderVideoSanitySample& first,
    const recording::SpatialRoiRecorderVideoSanitySample& second,
    const std::string& context)
{
    require(first.requested_frame_index == second.requested_frame_index &&
                first.mean == second.mean && first.stddev == second.stddev &&
                first.min == second.min && first.max == second.max &&
                first.black_fraction_lt8 == second.black_fraction_lt8 &&
                first.white_fraction_gt247 == second.white_fraction_gt247 &&
                first.decoded_bytes == second.decoded_bytes,
            context + " sample differs across repeat runs");
}

void require_result_equal(
    const recording::SpatialRoiRecorderVideoSanityResult& first,
    const recording::SpatialRoiRecorderVideoSanityResult& second)
{
    require(first.artifact_root_identity() == second.artifact_root_identity() &&
                first.video_identity() == second.video_identity() &&
                first.relative_path() == second.relative_path() &&
                first.size_bytes() == second.size_bytes() &&
                first.sha256() == second.sha256() &&
                first.duration_seconds() == second.duration_seconds() &&
                first.frame_rate() == second.frame_rate() &&
                first.time_base() == second.time_base() &&
                first.has_decoded_pts() == second.has_decoded_pts() &&
                first.first_decoded_pts() == second.first_decoded_pts() &&
                first.last_decoded_pts() == second.last_decoded_pts() &&
                first.container() == second.container() &&
                first.codec() == second.codec() &&
                first.decoder() == second.decoder() &&
                first.pixel_format() == second.pixel_format() &&
                first.color_range() == second.color_range() &&
                first.bit_depth() == second.bit_depth() &&
                first.chroma_subsampling() == second.chroma_subsampling() &&
                first.width() == second.width() && first.height() == second.height() &&
                first.frame_count() == second.frame_count() &&
                first.samples().size() == second.samples().size(),
            "repeat video sanity result metadata differs");
    for (std::size_t index = 0; index < first.samples().size(); ++index) {
        require_sample_equal(first.samples().at(index),
                             second.samples().at(index),
                             "repeat video sanity result");
    }
}

void rejects_invalid_arguments()
{
    TempTree tree;
    const auto root = open_root(tree.path() / "recording");
    auto request = request_for(root);
    request.artifact_root.reset();
    require_rejected(request, "null artifact root");

    request = request_for(root);
    request.video_relative_path = "invented.mp4";
    require_rejected(request, "unlisted video path");

    request = request_for(root);
    request.encoded_width = 0;
    require_rejected(request, "zero width");
    request = request_for(root);
    request.expected_frame_count = 0;
    require_rejected(request, "zero expected frame count");
    request = request_for(root);
    request.expected_frame_rate = 0.0;
    require_rejected(request, "zero expected frame rate");
    request = request_for(root);
    request.max_media_bytes = 0;
    require_rejected(request, "zero media bound");
    request = request_for(root);
    request.timeout = std::chrono::milliseconds(0);
    require_rejected(request, "zero timeout");
    request = request_for(root);
    request.timeout = std::chrono::hours(2);
    require_rejected(request, "unbounded timeout");
}

void rejects_non_video_and_truncation()
{
    TempTree tree;
    const auto root = open_root(tree.path() / "recording");
    write_file(*root, "this is neither MP4 nor HEVC");
    require_rejected(request_for(root), "non-video media");

    // A plausible MP4 header with no complete atom must still be rejected;
    // this guards against accepting a filename or a demuxer probe alone.
    TempTree second_tree;
    const auto second_root = open_root(second_tree.path() / "recording");
    write_file(*second_root,
               "\x00\x00\x00\x18" "ftypmp42" "\x00\x00\x00\x00" "mp42isom"
               "\x00\x00");
    require_rejected(request_for(second_root), "truncated MP4");
}

void verifies_retained_identity_binding()
{
    TempTree tree;
    const auto root = open_root(tree.path() / "recording");
    write_file(*root, "identity fixture");
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> retained;
    std::string error;
    require(root->OpenExistingFile(
                "video.mp4",
                recording::SpatialRoiRecorderArtifactFileAccess::kReadOnly,
                &retained,
                &error),
            "could not retain identity fixture: " + error);
    const fs::path artifact = tree.path() / "recording" /
                              recording::kSpatialRoiRecorderArtifactDirectory;
    std::error_code rename_error;
    fs::rename(artifact / "video.mp4", artifact / "old.mp4", rename_error);
    require(!rename_error,
            "could not replace identity fixture: " + rename_error.message());
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> replacement;
    require(root->CreateFile("video.mp4", &replacement, &error),
            "could not create replacement identity fixture: " + error);
    require(!retained->VerifyCurrentBinding(&error),
            "retained descriptor accepted an inode replacement");
}

// This is a one-frame, 384x384 HEVC-in-MP4 fixture. It is checked in as text
// so the positive probe test is deterministic and never depends on an
// optional host encoder such as libx265 or on CUDA/NVENC availability.
constexpr const char* kDeterministicHevcFixtureBase64 = R"HEVC_FIXTURE(
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
APz9+PgAAA8DoAABABdAAQwB//8hQAAAAwCQAAADAAADAJmsCaEAAQAqQgEBIUAAAAMAkAAAAwAAAwCZoAwIBgWWtKQhGS4wFsCA
AAADAIAAADIEogABAAdEAcD3xMyQAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAR6hWAEeoVgAAAAGHN0dHMAAAAAAAAAAQAA
AAEAAAOEAAAAHHN0c2MAAAAAAAAAAQAAAAEAAAABAAAAAQAAABRzdHN6AAAAAAABbuMAAAABAAAAFHN0Y28AAAAAAAAAAQAAACwA
AAP6dWR0YQAAA/JtZXRhAAAAAAAAACFoZGxyAAAAAAAAAABtZGlyYXBwbAAAAAAAAAAAAAAAA8VpbHN0AAAAJ6luYW0AAAAfZGF0
YQAAAAEAAAAAQ2FtMjAxMDA5MyBjcm9wAAAAJal0b28AAAAdZGF0YQAAAAEAAAAATGF2ZjU4Ljc2LjEwMAAAA3GpY210AAADaWRh
dGEAAAABAAAAAG52ZW5jIGNvZGVjPWhldmM7IHByZXNldD1wNzsgdHVuaW5nPWxvc3NsZXNzOyByZXM9Mzg0eDM4NDsgZnBzPTEw
MDsgY29sb3I9MDsgZ29wPTE7IHNvdXJjZV9waXhlbF9jb250cmFjdD1vcmFuZ2UuY3JvcC5tb25vOC52MTsgc291cmNlX3BpeGVs
X2Zvcm1hdD1tb25vODsgc291cmNlX3BpeGVsX2R0eXBlPXVpbnQ4OyBzb3VyY2VfcGl4ZWxfcmFuZ2U9MF8yNTU7IHNvdXJjZV9j
b2xvcl9zcGFjZT1saW5lYXJfZ3JheTsgc291cmNlX2NoYW5uZWxfb3JkZXI9Z3JheTsgc291cmNlX21lbW9yeV9sYXlvdXQ9SHhX
OyBzb3VyY2Vfd2lkdGg9Mzg0OyBzb3VyY2VfaGVpZ2h0PTM4NDsgc291cmNlX2Nvb3JkaW5hdGVfb3JpZ2luPXRvcF9sZWZ0OyBz
b3VyY2Vfb3JpZ2luPWFuYWx5dGljc19jcm9wOyBzb3VyY2VfdHJhbnNmb3JtX3RvX2VuY29kZXI9Y3JvcF9tb25vOF90b19udjEy
OyBlbmNvZGVyX2lucHV0X2Zvcm1hdD1udjEyOyBlbmNvZGVkX3BpeF9mbXQ9eXV2NDIwcDsgZW5jb2RlZF9jb2xvcl9yYW5nZT1w
Yzsgb3V0cHV0X2tpbmQ9Y3JvcDsgcm9sZT1ydW50aW1lX2Rlcml2ZWRfYWNxdWlzaXRpb25faW5wdXQ7IGlucHV0X2Zvcm1hdD1u
djEyOyBzb3VyY2VfZm9ybWF0PW1vbm84OyBjb29yZGluYXRlX3NwYWNlPWZ1bGxfZnJhbWVfcGl4ZWxzOyB2aWRlb19waXhlbF9j
b29yZGluYXRlX3NwYWNlPWNyb3BfZnJhbWVfcGl4ZWxzOyBzb3VyY2VfZ2VvbWV0cnlfY29vcmRpbmF0ZV9zcGFjZT1mdWxsX2Zy
YW1lX3BpeGVsczsgc2VsZWN0aW9uX3BvbGljeT1sYXJnZXN0X2RldGVjdGlvbl9ieV9jb25maWRlbmNlOyBibGFua19mcmFtZV9w
b2xpY3k9ZW5jb2RlX2JsYWNrX2ZyYW1lX3doZW5fbm9fZGV0ZWN0aW9u

)HEVC_FIXTURE";

std::string decode_fixture_base64()
{
    std::array<int, 256> values{};
    values.fill(-1);
    for (int index = 0; index < 26; ++index) {
        values[static_cast<unsigned char>('A' + index)] = index;
        values[static_cast<unsigned char>('a' + index)] = 26 + index;
    }
    for (int index = 0; index < 10; ++index) {
        values[static_cast<unsigned char>('0' + index)] = 52 + index;
    }
    values[static_cast<unsigned char>('+')] = 62;
    values[static_cast<unsigned char>('/')] = 63;

    std::string compact;
    for (const unsigned char character :
         std::string(kDeterministicHevcFixtureBase64)) {
        if (character != '\n' && character != '\r' && character != ' ' &&
            character != '\t') {
            compact.push_back(static_cast<char>(character));
        }
    }
    require(!compact.empty() && compact.size() % 4U == 0U,
            "deterministic fixture base64 is malformed");
    std::string bytes;
    bytes.reserve((compact.size() / 4U) * 3U);
    for (std::size_t offset = 0; offset < compact.size(); offset += 4U) {
        const unsigned char first = static_cast<unsigned char>(compact[offset]);
        const unsigned char second = static_cast<unsigned char>(compact[offset + 1U]);
        const unsigned char third = static_cast<unsigned char>(compact[offset + 2U]);
        const unsigned char fourth = static_cast<unsigned char>(compact[offset + 3U]);
        require(values[first] >= 0 && values[second] >= 0,
                "deterministic fixture base64 has invalid symbols");
        const int third_value = third == '=' ? 0 : values[third];
        const int fourth_value = fourth == '=' ? 0 : values[fourth];
        require(third_value >= 0 && fourth_value >= 0,
                "deterministic fixture base64 has invalid padding");
        const std::uint32_t bits =
            (static_cast<std::uint32_t>(values[first]) << 18U) |
            (static_cast<std::uint32_t>(values[second]) << 12U) |
            (static_cast<std::uint32_t>(third_value) << 6U) |
            static_cast<std::uint32_t>(fourth_value);
        bytes.push_back(static_cast<char>((bits >> 16U) & 0xffU));
        if (third != '=') {
            bytes.push_back(static_cast<char>((bits >> 8U) & 0xffU));
        }
        if (fourth != '=') {
            bytes.push_back(static_cast<char>(bits & 0xffU));
        }
    }
    return bytes;
}

std::string fixture_with_zero_duration(const std::string& bytes)
{
    std::string result = bytes;
    for (const auto& box : {std::pair<const char*, std::size_t>{"mvhd", 20U},
                            {"mdhd", 20U},
                            {"stts", 16U}}) {
        const std::size_t offset = result.find(box.first);
        require(offset != std::string::npos && offset + box.second + 4U <= result.size(),
                "deterministic fixture has no bounded duration box");
        // The checked-in fixture uses version-zero boxes. Keep timescales and
        // the sample table structure intact while removing every duration
        // source that libavformat could use to infer a positive value.
        result[offset + box.second] = '\0';
        result[offset + box.second + 1U] = '\0';
        result[offset + box.second + 2U] = '\0';
        result[offset + box.second + 3U] = '\0';
    }
    return result;
}

std::string fixture_with_non_hevc_sample_entry(const std::string& bytes)
{
    std::string result = bytes;
    const std::size_t sample_entry = result.find("hvc1");
    require(sample_entry != std::string::npos,
            "deterministic fixture has no HEVC sample entry");
    result.replace(sample_entry, 4U, "avc1");
    return result;
}

void accepts_deterministic_fixture()
{
    TempTree tree;
    const std::string bytes = decode_fixture_base64();
    require(!bytes.empty(), "deterministic fixture is empty");
    const auto root = open_root(tree.path() / "recording");
    write_file(*root, bytes);
    auto request = request_for(root);
    request.encoded_width = 384;
    request.encoded_height = 384;
    request.expected_frame_count = 1;
    request.max_media_bytes = bytes.size();
    std::unique_ptr<recording::SpatialRoiRecorderVideoSanityResult> result;
    std::string error;
    require(recording::SpatialRoiRecorderVideoSanityProbe::Run(
                request, &result, &error),
            "deterministic HEVC fixture was rejected: " + error);
    require(result != nullptr, "deterministic fixture returned no result");
    std::unique_ptr<recording::SpatialRoiRecorderArtifactFile> expected_file;
    require(root->OpenExistingFile(
                "video.mp4",
                recording::SpatialRoiRecorderArtifactFileAccess::kReadOnly,
                &expected_file,
                &error),
            "could not reopen deterministic fixture: " + error);
    require(result->artifact_root_identity() == root->artifact_root_identity() &&
                result->video_identity() == expected_file->identity() &&
                result->relative_path() == "video.mp4" &&
                result->size_bytes() == 95742U &&
                result->sha256() ==
                    "sha256:77d7cb89d77a5331adc5a66abc232f1fcbce644ae73b727852dd857ab47d41b4" &&
                result->frame_rate() == "100/1" &&
                result->time_base() == "1/90000" && result->has_decoded_pts() &&
                result->first_decoded_pts() == 0 &&
                result->last_decoded_pts() == 0 &&
                result->container() == "mov,mp4,m4a,3gp,3g2,mj2" &&
                result->codec() == "hevc" &&
                result->decoder().rfind("hevc@", 0) == 0 &&
                result->pixel_format() == "yuvj420p" &&
                result->color_range() == "pc" && result->bit_depth() == 8U &&
                result->chroma_subsampling() == "4:2:0" &&
                result->width() == 384U && result->height() == 384U &&
                result->frame_count() == 1U && result->samples().size() == 1U,
            "deterministic fixture returned incorrect provenance");
    require(result->duration_seconds() == "0.01",
            "deterministic fixture returned noncanonical duration text");
    const double duration = std::stod(result->duration_seconds());
    require(std::isfinite(duration) && duration == 0.01,
            "deterministic fixture returned incorrect numeric duration");
    const auto& fixture_sample = result->samples().at(0);
    require(fixture_sample.requested_frame_index == 0U &&
                fixture_sample.mean == 171.4700927734375 &&
                fixture_sample.stddev == 25.180076561100911 &&
                fixture_sample.min == 42U && fixture_sample.max == 210U &&
                fixture_sample.black_fraction_lt8 == 0.0 &&
                fixture_sample.white_fraction_gt247 == 0.0 &&
                fixture_sample.decoded_bytes == 147456U,
            "deterministic fixture returned incorrect luma sample");

    std::unique_ptr<recording::SpatialRoiRecorderVideoSanityResult> repeat;
    error.clear();
    require(recording::SpatialRoiRecorderVideoSanityProbe::Run(
                request, &repeat, &error),
            "deterministic fixture repeat probe was rejected: " + error);
    require(repeat != nullptr, "deterministic fixture repeat returned no result");
    require_result_equal(*result, *repeat);

    auto cadence_request = request_for(root);
    cadence_request.encoded_width = 384;
    cadence_request.encoded_height = 384;
    cadence_request.expected_frame_count = 1;
    cadence_request.expected_frame_rate = 50.0;
    cadence_request.max_media_bytes = bytes.size();
    require_rejected(cadence_request,
                     "deterministic fixture mismatched frame cadence");

    auto dimension_request = request_for(root);
    dimension_request.encoded_width = 383;
    dimension_request.encoded_height = 384;
    dimension_request.expected_frame_count = 1;
    dimension_request.max_media_bytes = bytes.size();
    require_rejected(dimension_request,
                     "deterministic fixture mismatched container raster");

    request.max_media_bytes = bytes.size() - 1U;
    require_rejected(request,
                     "deterministic fixture over authenticated media bound");

    TempTree format_tree;
    const auto format_root = open_root(format_tree.path() / "recording");
    const std::string non_hevc = fixture_with_non_hevc_sample_entry(bytes);
    write_file(*format_root, non_hevc);
    auto format_request = request_for(format_root);
    format_request.encoded_width = 384;
    format_request.encoded_height = 384;
    format_request.expected_frame_count = 1;
    format_request.max_media_bytes = non_hevc.size();
    require_rejected(format_request,
                     "deterministic fixture non-HEVC sample entry");

    TempTree duration_tree;
    const auto duration_root = open_root(duration_tree.path() / "recording");
    const std::string zero_duration = fixture_with_zero_duration(bytes);
    write_file(*duration_root, zero_duration);
    auto duration_request = request_for(duration_root);
    duration_request.encoded_width = 384;
    duration_request.encoded_height = 384;
    duration_request.expected_frame_count = 1;
    duration_request.max_media_bytes = zero_duration.size();
    require_rejected(duration_request, "zero-duration container");
}

}  // namespace

int main()
{
    try {
        rejects_invalid_arguments();
        rejects_non_video_and_truncation();
        verifies_retained_identity_binding();
        accepts_deterministic_fixture();
        std::cout << "spatial_roi_recorder_video_sanity_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_recorder_video_sanity_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
