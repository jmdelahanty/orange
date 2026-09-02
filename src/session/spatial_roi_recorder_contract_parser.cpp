#include "session/spatial_roi_recorder_contract_parser.h"

#include "session/spatial_roi_recorder_contract.h"
#include "session/spatial_roi_recording_config.h"
#include "spatial_roi_recorder_cuda_detach.h"
#include "shaman_v2_recording_identity.h"
#include "spatial_roi_frame_contract.h"
#include "spatial_roi_ipc_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;
using FrameRaster = orange::spatial_roi::SpatialRoiFrameRaster;
using FrameRect = orange::spatial_roi::SpatialRoiFrameRect;
using FramePadding = orange::spatial_roi::SpatialRoiFramePadding;

constexpr std::size_t kMaxContractBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxJsonDepth = 64;
constexpr std::size_t kMaxJsonEvents = 200000;
constexpr std::size_t kMaxJsonStringBytes = 64U * 1024U;
constexpr std::size_t kMaxJsonContainerItems = 32768;
constexpr std::uint32_t kMaxQueueFrames = 4096;
constexpr std::uint64_t kMaxWriterQueuePackets = 4096;
constexpr std::uint64_t kMaxWriterQueueBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxOperationTimeoutMs = 60000;
constexpr std::size_t kMaxSocketPathBytes = 107;
constexpr char kExpectedProtocol[] = "orange.spatial_roi.external_recorder_ipc";
constexpr char kExpectedCadence[] = "every_recording_frame";
constexpr char kExpectedPixelFormat[] = "mono8";
constexpr char kExpectedLifetime[] = "deferred_release";
constexpr char kExpectedPlanDigestPrefix[] = "sha256:";

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(const int fd) noexcept : fd_(fd) {}
    ~ScopedFileDescriptor()
    {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

class BoundedContractJsonSax final : public nlohmann::json_sax<json> {
public:
    bool null() override { return event(); }
    bool boolean(bool) override { return event(); }
    bool number_integer(number_integer_t) override { return event(); }
    bool number_unsigned(number_unsigned_t) override { return event(); }
    bool number_float(number_float_t, const string_t& text) override
    {
        return text.size() <= kMaxJsonStringBytes && event();
    }
    bool string(string_t& value) override
    {
        return value.size() <= kMaxJsonStringBytes && event();
    }
    bool binary(binary_t&) override { return false; }

    bool start_object(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({true, {}});
        return true;
    }

    bool key(string_t& value) override
    {
        if (value.size() > kMaxJsonStringBytes || stack_.empty() ||
            !stack_.back().object || !event()) {
            return false;
        }
        if (stack_.back().keys.size() >= kMaxJsonContainerItems) {
            return false;
        }
        return stack_.back().keys.insert(value).second;
    }

    bool end_object() override
    {
        if (stack_.empty() || !stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool start_array(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({false, {}});
        return true;
    }

    bool end_array() override
    {
        if (stack_.empty() || stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool parse_error(std::size_t,
                     const std::string&,
                     const nlohmann::detail::exception&) override
    {
        return false;
    }

private:
    struct Container {
        bool object = false;
        std::unordered_set<std::string> keys;
    };

    bool event() noexcept
    {
        if (events_ >= kMaxJsonEvents) {
            return false;
        }
        ++events_;
        return true;
    }

    bool start_container(const std::size_t elements) noexcept
    {
        return stack_.size() < kMaxJsonDepth &&
               (elements == static_cast<std::size_t>(-1) ||
                elements <= kMaxJsonContainerItems) &&
               event();
    }

    std::size_t events_ = 0;
    std::vector<Container> stack_;
};

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

std::string field(const std::string& path, const std::string& key)
{
    return path.empty() ? key : path + "." + key;
}

bool exact_keys(const json& value,
                const std::set<std::string>& required,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const auto& key : required) {
        if (!value.contains(key)) {
            return fail(error_out, field(path, key) + " is required");
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!required.count(it.key())) {
            return fail(error_out,
                        field(path, it.key()) +
                            " is not allowed by the strict spatial ROI contract");
        }
    }
    return true;
}

bool safe_identifier(const std::string& value, std::size_t max_length = 64)
{
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [&](unsigned char c) {
        return alnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool recording_id(const std::string& value)
{
    if (value.empty() || value.size() > 512 ||
        std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, kExpectedPlanDigestPrefix) != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    });
}

bool read_string(const json& object,
                 const char* key,
                 const std::string& path,
                 std::string* out,
                 std::string* error_out,
                 bool nonempty = false)
{
    const auto& value = object.at(key);
    if (!value.is_string() || (nonempty && value.get<std::string>().empty())) {
        return fail(error_out,
                    field(path, key) +
                        (nonempty ? " must be a nonempty string"
                                  : " must be a string"));
    }
    if (out) {
        *out = value.get<std::string>();
    }
    return true;
}

bool read_bool(const json& object,
               const char* key,
               const std::string& path,
               bool* out,
               std::string* error_out)
{
    if (!object.at(key).is_boolean()) {
        return fail(error_out, field(path, key) + " must be a boolean");
    }
    if (out) {
        *out = object.at(key).get<bool>();
    }
    return true;
}

bool json_u64(const json& value, std::uint64_t* out)
{
    if (!out || value.is_boolean() ||
        (!value.is_number_unsigned() && !value.is_number_integer())) {
        return false;
    }
    if (value.is_number_unsigned()) {
        *out = value.get<std::uint64_t>();
        return true;
    }
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool checked_add(const std::uint64_t left,
                 const std::uint64_t right,
                 std::uint64_t* out)
{
    if (!out || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *out = left + right;
    return true;
}

bool checked_multiply(const std::uint64_t left,
                      const std::uint64_t right,
                      std::uint64_t* out)
{
    if (!out || (left != 0 &&
                 right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool read_u64(const json& object,
              const char* key,
              const std::string& path,
              std::uint64_t* out,
              std::string* error_out,
              std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max(),
              bool positive = false)
{
    std::uint64_t value = 0;
    if (!json_u64(object.at(key), &value) || value > max_value ||
        (positive && value == 0)) {
        return fail(error_out,
                    field(path, key) +
                        (positive ? " must be a positive integer"
                                  : " must be a nonnegative integer"));
    }
    if (out) {
        *out = value;
    }
    return true;
}

bool read_u32(const json& object,
              const char* key,
              const std::string& path,
              std::uint32_t* out,
              std::string* error_out,
              bool positive = false)
{
    std::uint64_t value = 0;
    if (!read_u64(object,
                  key,
                  path,
                  &value,
                  error_out,
                  std::numeric_limits<std::uint32_t>::max(),
                  positive)) {
        return false;
    }
    if (out) {
        *out = static_cast<std::uint32_t>(value);
    }
    return true;
}

bool read_int(const json& object,
              const char* key,
              const std::string& path,
              int* out,
              std::string* error_out)
{
    std::uint64_t value = 0;
    if (!read_u64(object,
                  key,
                  path,
                  &value,
                  error_out,
                  static_cast<std::uint64_t>(std::numeric_limits<int>::max()))) {
        return false;
    }
    if (out) {
        *out = static_cast<int>(value);
    }
    return true;
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty() || value.size() > 512 || value.front() == '/' ||
        value.find('\\') != std::string::npos || value.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t slash = value.find('/', start);
        const std::string part = value.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) {
                return c < 0x20 || c == 0x7f;
            })) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool safe_absolute_path(const std::string& value,
                        std::filesystem::path* path_out,
                        std::string* error_out,
                        const std::string& path)
{
    if (value.empty() || value.find('\0') != std::string::npos) {
        return fail(error_out, field(path, "path") + " must be a nonempty path");
    }
    const std::filesystem::path parsed(value);
    if (!parsed.is_absolute() || parsed == parsed.root_path() ||
        parsed.lexically_normal() != parsed) {
        return fail(error_out,
                    field(path, "path") +
                        " must be an absolute non-root normalized path");
    }
    if (path_out) {
        *path_out = parsed;
    }
    return true;
}

bool relative_to(const std::filesystem::path& root,
                 const std::string& absolute,
                 std::string* relative_out,
                 std::string* error_out,
                 const std::string& path)
{
    std::filesystem::path candidate;
    if (!safe_absolute_path(absolute, &candidate, error_out, path)) {
        return false;
    }
    const std::filesystem::path relative = candidate.lexically_relative(root);
    const std::string relative_string = relative.generic_string();
    if (!safe_relative_path(relative_string) || relative.empty() ||
        (relative_string != "" &&
         (relative_string == ".." || relative_string.rfind("../", 0) == 0))) {
        return fail(error_out,
                    path + " must be a safe path contained below artifact_root");
    }
    if (relative_out) {
        *relative_out = relative_string;
    }
    return true;
}

bool parse_raster(const json& value,
                  FrameRaster* out,
                  const std::string& path,
                  std::string* error_out)
{
    if (!exact_keys(value, {"width", "height"}, path, error_out) ||
        !read_u32(value, "width", path, &out->width, error_out, true) ||
        !read_u32(value, "height", path, &out->height, error_out, true)) {
        return false;
    }
    return true;
}

bool parse_rect(const json& value,
                FrameRect* out,
                const std::string& path,
                std::string* error_out)
{
    if (!exact_keys(value, {"x", "y", "width", "height"}, path, error_out) ||
        !read_u32(value, "x", path, &out->x, error_out) ||
        !read_u32(value, "y", path, &out->y, error_out) ||
        !read_u32(value, "width", path, &out->width, error_out, true) ||
        !read_u32(value, "height", path, &out->height, error_out, true)) {
        return false;
    }
    return true;
}

bool parse_padding(const json& value,
                   FramePadding* out,
                   const std::string& path,
                   std::string* error_out)
{
    if (!exact_keys(value,
                    {"left", "top", "right", "bottom", "value_mono8"},
                    path,
                    error_out) ||
        !read_u32(value, "left", path, &out->left, error_out) ||
        !read_u32(value, "top", path, &out->top, error_out) ||
        !read_u32(value, "right", path, &out->right, error_out) ||
        !read_u32(value, "bottom", path, &out->bottom, error_out)) {
        return false;
    }
    std::uint64_t value_mono8 = 0;
    if (!read_u64(value,
                  "value_mono8",
                  path,
                  &value_mono8,
                  error_out,
                  255)) {
        return false;
    }
    out->value_mono8 = static_cast<std::uint8_t>(value_mono8);
    return true;
}

bool parse_authority(const json& value,
                     SpatialRoiRecorderAuthorityView* out,
                     const std::string& path,
                     std::string* error_out)
{
    if (!exact_keys(value, {"id", "sha256"}, path, error_out) ||
        !read_string(value, "id", path, &out->id, error_out, true) ||
        !read_string(value, "sha256", path, &out->sha256, error_out, true)) {
        return false;
    }
    if (!safe_identifier(out->id) || !sha256(out->sha256)) {
        return fail(error_out,
                    path + " must contain a safe id and sha256:<64 lowercase hex>");
    }
    return true;
}

bool parse_arena_id(const json& value,
                    bool* has_out,
                    std::string* id_out,
                    const std::string& path,
                    std::string* error_out)
{
    if (value.is_null()) {
        *has_out = false;
        id_out->clear();
        return true;
    }
    if (!value.is_string()) {
        return fail(error_out, path + " must be a string or null");
    }
    *id_out = value.get<std::string>();
    if (!safe_identifier(*id_out)) {
        return fail(error_out, path + " must be a safe identifier when present");
    }
    *has_out = true;
    return true;
}

bool parse_geometry(const json& value,
                    SpatialRoiRecorderGeometryView* out,
                    const std::string& path,
                    std::string* error_out)
{
    if (!exact_keys(value,
                    {"layout", "materialization", "registration", "native_raster",
                     "content_rect", "encoded_raster", "encoded_content_rect",
                     "content_offset", "padding", "source_coordinate_space",
                     "video_coordinate_space"},
                    path,
                    error_out) ||
        !parse_authority(value.at("layout"),
                         &out->layout,
                         field(path, "layout"),
                         error_out) ||
        !parse_authority(value.at("materialization"),
                         &out->materialization,
                         field(path, "materialization"),
                         error_out) ||
        !parse_authority(value.at("registration"),
                         &out->registration,
                         field(path, "registration"),
                         error_out) ||
        !parse_raster(value.at("native_raster"),
                      &out->native_raster,
                      field(path, "native_raster"),
                      error_out) ||
        !parse_rect(value.at("content_rect"),
                    &out->content_rect,
                    field(path, "content_rect"),
                    error_out) ||
        !parse_raster(value.at("encoded_raster"),
                      &out->encoded_raster,
                      field(path, "encoded_raster"),
                      error_out) ||
        !parse_rect(value.at("encoded_content_rect"),
                    &out->encoded_content_rect,
                    field(path, "encoded_content_rect"),
                    error_out) ||
        !parse_padding(value.at("padding"),
                       &out->padding,
                       field(path, "padding"),
                       error_out) ||
        !read_string(value,
                     "source_coordinate_space",
                     path,
                     &out->source_coordinate_space,
                     error_out,
                     true) ||
        !read_string(value,
                     "video_coordinate_space",
                     path,
                     &out->video_coordinate_space,
                     error_out,
                     true)) {
        return false;
    }

    const json& offset = value.at("content_offset");
    if (!exact_keys(offset, {"x", "y"}, field(path, "content_offset"), error_out) ||
        !read_u32(offset,
                  "x",
                  field(path, "content_offset"),
                  &out->content_offset_x,
                  error_out) ||
        !read_u32(offset,
                  "y",
                  field(path, "content_offset"),
                  &out->content_offset_y,
                  error_out)) {
        return false;
    }
    const auto fits = [](const FrameRaster& raster, const FrameRect& rect) {
        return static_cast<std::uint64_t>(rect.x) + rect.width <= raster.width &&
               static_cast<std::uint64_t>(rect.y) + rect.height <= raster.height;
    };
    if (!fits(out->native_raster, out->content_rect) ||
        !fits(out->encoded_raster, out->encoded_content_rect) ||
        out->encoded_content_rect.x != 0 || out->encoded_content_rect.y != 0 ||
        out->content_offset_x != 0 || out->content_offset_y != 0 ||
        out->padding.left != 0 || out->padding.top != 0 ||
        out->encoded_content_rect.width != out->content_rect.width ||
        out->encoded_content_rect.height != out->content_rect.height ||
        static_cast<std::uint64_t>(out->encoded_content_rect.width) +
                out->padding.right != out->encoded_raster.width ||
        static_cast<std::uint64_t>(out->encoded_content_rect.height) +
                out->padding.bottom != out->encoded_raster.height ||
        out->padding.value_mono8 != 0 ||
        (out->encoded_raster.width % 2U) != 0U ||
        (out->encoded_raster.height % 2U) != 0U ||
        static_cast<std::uint64_t>(out->encoded_raster.width) *
                out->encoded_raster.height >
            orange::spatial_roi::ipc::kSpatialRoiIpcMaxPackedMono8Bytes) {
        return fail(error_out,
                    path + " contains unsupported non-origin or inconsistent ROI geometry");
    }
    return true;
}

bool parse_frame_identity(const json& value,
                          SpatialRoiRecorderStreamView* out,
                          const std::string& path,
                          std::string* error_out)
{
    if (!exact_keys(value,
                    {"key_fields", "roi_stream_frame_index", "recording_frame_id_source"},
                    path,
                    error_out) ||
        !value.at("key_fields").is_array() ||
        !read_string(value,
                     "roi_stream_frame_index",
                     path,
                     &out->roi_stream_frame_index_mode,
                     error_out,
                     true) ||
        !read_string(value,
                     "recording_frame_id_source",
                     path,
                     &out->recording_frame_id_source,
                     error_out,
                     true)) {
        return false;
    }
    const std::vector<std::string> expected = {
        "recording_identity_token",
        "producer_generation",
        "logical_stream_id",
        "recording_frame_id",
        "roi_stream_frame_index",
    };
    if (value.at("key_fields").size() != expected.size()) {
        return fail(error_out, path + ".key_fields has the wrong length");
    }
    out->frame_identity_key_fields.clear();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!value.at("key_fields").at(i).is_string() ||
            value.at("key_fields").at(i).get<std::string>() != expected.at(i)) {
            return fail(error_out, path + ".key_fields is not the closed v1 sequence");
        }
        out->frame_identity_key_fields.push_back(expected.at(i));
    }
    if (out->roi_stream_frame_index_mode != "dense_one_based" ||
        out->recording_frame_id_source != "parent_camera_recording") {
        return fail(error_out, path + " contains an unsupported frame identity mode");
    }
    return true;
}

bool parse_identity(const json& value,
                    SpatialRoiRecorderStreamView* out,
                    const std::string& path,
                    std::string* error_out)
{
    if (!exact_keys(value,
                    {"recording_id", "recording_identity_token", "producer_generation",
                     "spatial_roi_plan_sha256", "camera_id", "camera_serial",
                     "arena_group_id", "arena_id", "region_id", "roi_id",
                     "logical_stream_id"},
                    path,
                    error_out) ||
        !read_string(value, "recording_id", path, &out->recording_id, error_out, true) ||
        !read_string(value,
                     "recording_identity_token",
                     path,
                     &out->recording_identity_token,
                     error_out,
                     true) ||
        !read_string(value,
                     "producer_generation",
                     path,
                     &out->producer_generation,
                     error_out,
                     true) ||
        !read_string(value,
                     "spatial_roi_plan_sha256",
                     path,
                     &out->spatial_roi_plan_sha256,
                     error_out,
                     true) ||
        !read_int(value, "camera_id", path, &out->camera_id, error_out) ||
        !read_string(value, "camera_serial", path, &out->camera_serial, error_out, true) ||
        !read_string(value, "arena_group_id", path, &out->arena_group_id, error_out, true) ||
        !parse_arena_id(value.at("arena_id"),
                        &out->has_arena_id,
                        &out->arena_id,
                        field(path, "arena_id"),
                        error_out) ||
        !read_string(value, "region_id", path, &out->region_id, error_out, true) ||
        !read_string(value, "roi_id", path, &out->roi_id, error_out, true) ||
        !read_string(value,
                     "logical_stream_id",
                     path,
                     &out->logical_stream_id,
                     error_out,
                     true)) {
        return false;
    }
    if (!recording_id(out->recording_id) || !sha256(out->recording_identity_token) ||
        !safe_identifier(out->producer_generation) ||
        !sha256(out->spatial_roi_plan_sha256) || !safe_identifier(out->camera_serial) ||
        !safe_identifier(out->arena_group_id) || !safe_identifier(out->region_id) ||
        !safe_identifier(out->roi_id) || !safe_identifier(out->logical_stream_id)) {
        return fail(error_out, path + " contains an invalid identity field");
    }
    return true;
}

bool parse_encode_profile(const json& value,
                          SpatialRoiRecorderEncodeProfileView* out,
                          const std::string& path,
                          const int contract_schema_version,
                          std::string* error_out)
{
    const std::set<std::string> legacy_keys = {
        "profile_id", "codec", "preset", "tuning", "lossless",
        "rate_control_mode", "quality_value", "gop_length", "frame_rate",
        "input_format", "encoded_format", "no_resize",
        "luma_preserved_exactly", "neutral_chroma_value"};
    std::set<std::string> expected_keys = legacy_keys;
    if (contract_schema_version == kSpatialRoiRecorderContractSchemaVersion) {
        expected_keys.insert("aq");
        expected_keys.insert("temporal_aq");
        expected_keys.insert("lookahead");
        expected_keys.insert("lookahead_depth");
    }
    if (!exact_keys(value, expected_keys, path, error_out) ||
        !read_string(value, "profile_id", path, &out->profile_id, error_out, true) ||
        !read_string(value, "codec", path, &out->codec, error_out, true) ||
        !read_string(value, "preset", path, &out->preset, error_out, true) ||
        !read_string(value, "tuning", path, &out->tuning, error_out, true) ||
        !read_bool(value, "lossless", path, &out->lossless, error_out) ||
        !read_string(value,
                     "rate_control_mode",
                     path,
                     &out->rate_control_mode,
                     error_out,
                     true) ||
        !read_u32(value, "quality_value", path, &out->quality_value, error_out) ||
        !read_u32(value, "gop_length", path, &out->gop_length, error_out, true) ||
        !read_u32(value, "frame_rate", path, &out->frame_rate, error_out, true) ||
        !read_string(value, "input_format", path, &out->input_format, error_out, true) ||
        !read_string(value, "encoded_format", path, &out->encoded_format, error_out, true) ||
        !read_bool(value, "no_resize", path, &out->no_resize, error_out) ||
        !read_bool(value,
                   "luma_preserved_exactly",
                   path,
                   &out->luma_preserved_exactly,
                   error_out) ||
        !read_u32(value,
                  "neutral_chroma_value",
                  path,
                  &out->neutral_chroma_value,
                  error_out)) {
        return false;
    }
    if (contract_schema_version == kLegacySpatialRoiRecorderContractSchemaVersion) {
        // Contract v4 predated explicit encoder-control fields. All profiles
        // admitted by that schema were operationally defined with these
        // controls disabled, so retain that exact legacy inference.
        out->aq = false;
        out->temporal_aq = false;
        out->lookahead = false;
        out->lookahead_depth = 0;
    } else if (!read_bool(value, "aq", path, &out->aq, error_out) ||
               !read_bool(value,
                          "temporal_aq",
                          path,
                          &out->temporal_aq,
                          error_out) ||
               !read_bool(value,
                          "lookahead",
                          path,
                          &out->lookahead,
                          error_out) ||
               !read_u32(value,
                         "lookahead_depth",
                         path,
                         &out->lookahead_depth,
                         error_out)) {
        return false;
    }
    const EncodeProfile legacy = legacy_lossless_encode_profile();
    const EncodeProfile legacy_low_latency =
        legacy_low_latency_vbr_gop1_encode_profile();
    const EncodeProfile low_latency = low_latency_vbr_encode_profile();
    const auto matches = [&](const EncodeProfile& expected) {
        return out->profile_id == expected.name &&
            out->codec == expected.codec && out->preset == expected.preset &&
            out->tuning == expected.tuning && out->lossless == expected.lossless &&
            out->rate_control_mode == expected.rate_control_mode &&
            out->quality_value == expected.quality_value &&
            out->gop_length == expected.gop_length && out->aq == expected.aq &&
            out->temporal_aq == expected.temporal_aq &&
            out->lookahead == expected.lookahead &&
            out->lookahead_depth == expected.lookahead_depth;
    };
    const bool is_legacy = matches(legacy);
    const bool is_legacy_low_latency = matches(legacy_low_latency);
    const bool is_low_latency = matches(low_latency);
    if ((contract_schema_version == kLegacySpatialRoiRecorderContractSchemaVersion &&
         !is_legacy) ||
        (contract_schema_version == kSpatialRoiRecorderContractSchemaVersion &&
         !is_legacy && !is_legacy_low_latency && !is_low_latency) ||
        (contract_schema_version != kLegacySpatialRoiRecorderContractSchemaVersion &&
         contract_schema_version != kSpatialRoiRecorderContractSchemaVersion) ||
        out->frame_rate == 0 || out->input_format != "mono8" ||
        out->encoded_format != "nv12" || !out->no_resize ||
        out->luma_preserved_exactly != out->lossless ||
        out->neutral_chroma_value != 128) {
        return fail(error_out, path + " is not an allowed immutable HEVC profile");
    }
    return true;
}

bool expected_nv12_queue_bytes(const SpatialRoiRecorderGeometryView& geometry,
                               const std::uint32_t queue_frames,
                               std::uint64_t* bytes_out,
                               const std::string& path,
                               std::string* error_out)
{
    std::uint64_t pixels = 0;
    std::uint64_t bytes_per_frame = 0;
    std::uint64_t queue_bytes = 0;
    if (!bytes_out || (geometry.encoded_raster.width % 2U) != 0U ||
        (geometry.encoded_raster.height % 2U) != 0U || queue_frames == 0 ||
        !checked_multiply(geometry.encoded_raster.width,
                          geometry.encoded_raster.height,
                          &pixels) ||
        !checked_add(pixels, pixels / 2U, &bytes_per_frame) ||
        !checked_multiply(bytes_per_frame, queue_frames, &queue_bytes) ||
        queue_bytes == 0) {
        return fail(error_out,
                    path + " detached NV12 encode queue byte budget overflowed");
    }
    *bytes_out = queue_bytes;
    return true;
}

bool expected_detach_pool_bytes(
    const SpatialRoiRecorderGeometryView& geometry,
    const std::uint32_t pool_frames,
    std::uint64_t* bytes_out,
    const std::string& path,
    std::string* error_out)
{
    std::uint64_t pixels = 0;
    std::uint64_t nv12_bytes = 0;
    std::uint64_t bytes_per_slot = 0;
    std::uint64_t pool_bytes = 0;
    if (!bytes_out || pool_frames == 0 ||
        !checked_multiply(geometry.encoded_raster.width,
                          geometry.encoded_raster.height,
                          &pixels) ||
        !checked_add(pixels, pixels / 2U, &nv12_bytes) ||
        !checked_add(pixels, nv12_bytes, &bytes_per_slot) ||
        !checked_multiply(bytes_per_slot, pool_frames, &pool_bytes) ||
        pool_bytes == 0 ||
        pool_bytes > orange::spatial_roi::ipc::
            kSpatialRoiRecorderCudaDetachMaxPoolBytes) {
        return fail(error_out,
                    path +
                        " recorder Mono8+NV12 detach-pool byte budget is invalid");
    }
    *bytes_out = pool_bytes;
    return true;
}

bool parse_expected_artifacts(const json& value,
                              const std::string& artifact_root,
                              SpatialRoiRecorderStreamView* out,
                              const std::string& path,
                              std::string* error_out)
{
    const std::set<std::string> keys = {
        "video", "metadata", "keyframes", "perf", "summary", "status",
        "video_sanity", "finalization", "recorder_log", "transport_sidecar",
        "evidence", "evidence_manifest"};
    if (!exact_keys(value, keys, path, error_out)) {
        return false;
    }
    const std::filesystem::path root(artifact_root);
    for (const auto& key : keys) {
        std::string absolute;
        if (!read_string(value, key.c_str(), path, &absolute, error_out, true)) {
            return false;
        }
        std::string relative;
        if (!relative_to(root,
                         absolute,
                         &relative,
                         error_out,
                         field(path, key))) {
            return false;
        }
        out->artifacts[key] = {std::move(absolute), std::move(relative)};
    }
    return true;
}

bool parse_ipc(const json& value,
               SpatialRoiRecorderIpcView* out,
               const std::string& path,
               std::string* error_out)
{
    out->features.clear();
    if (!exact_keys(value,
                    {"protocol", "version", "features", "source_lifetime_mode", "ack",
                     "release", "drain_finalize", "bounds"},
                    path,
                    error_out) ||
        !read_string(value, "protocol", path, &out->protocol, error_out, true) ||
        !read_int(value, "version", path, &out->version, error_out) ||
        !read_string(value,
                     "source_lifetime_mode",
                     path,
                     &out->source_lifetime_mode,
                     error_out,
                     true)) {
        return false;
    }
    if (out->protocol != kExpectedProtocol || out->version != 2 ||
        out->source_lifetime_mode != kExpectedLifetime) {
        return fail(error_out, path + " has the wrong protocol identity");
    }
    const auto& expected_features =
        orange::spatial_roi::ipc::spatial_roi_ipc_required_features();
    const json& features = value.at("features");
    if (!features.is_array() || features.size() != expected_features.size()) {
        return fail(error_out, path + ".features must be the exact active feature list");
    }
    for (std::size_t i = 0; i < expected_features.size(); ++i) {
        if (!features.at(i).is_string() ||
            features.at(i).get<std::string>() != expected_features.at(i)) {
            return fail(error_out, path + ".features must exclude drain_finalize");
        }
        out->features.push_back(expected_features.at(i));
    }

    const json& ack = value.at("ack");
    const std::string ack_path = field(path, "ack");
    if (!exact_keys(ack, {"message_kind", "accepted_true", "accepted_false"}, ack_path,
                    error_out) ||
        !read_string(ack, "message_kind", ack_path, nullptr, error_out, true) ||
        ack.at("message_kind") != "ACK") {
        return fail(error_out, ack_path + " must define message_kind ACK");
    }
    const auto parse_ack_case = [&](const char* key, const char* means) {
        const json& item = ack.at(key);
        const std::string item_path = field(ack_path, key);
        if (!exact_keys(item, {"means", "source_safe_after_ack", "release_required"},
                        item_path, error_out) ||
            !read_string(item, "means", item_path, nullptr, error_out, true) ||
            item.at("means") != means ||
            !read_bool(item, "source_safe_after_ack", item_path, nullptr, error_out) ||
            item.at("source_safe_after_ack") != false ||
            !read_bool(item, "release_required", item_path, nullptr, error_out) ||
            item.at("release_required") != true) {
            return false;
        }
        return true;
    };
    if (!parse_ack_case("accepted_true", "recorder_accepted_frame_and_retains_source_access") ||
        !parse_ack_case("accepted_false",
                        "recorder_rejected_frame_but_source_access_is_not_yet_released")) {
        return false;
    }

    const json& release = value.at("release");
    const std::string release_path = field(path, "release");
    if (!exact_keys(release,
                    {"message_kind", "means", "source_safe_after_release",
                     "required_after_accepted_ack", "required_after_rejected_ack",
                     "does_not_mean_encode_or_disk_complete"},
                    release_path,
                    error_out) ||
        release.at("message_kind") != "RELEASE" ||
        release.at("means") != "recorder_finished_with_source_allocation" ||
        release.at("source_safe_after_release") != true ||
        release.at("required_after_accepted_ack") != true ||
        release.at("required_after_rejected_ack") != true ||
        release.at("does_not_mean_encode_or_disk_complete") != true) {
        return fail(error_out, release_path + " does not close ACK/RELEASE ownership");
    }

    const json& drain = value.at("drain_finalize");
    const std::string drain_path = field(path, "drain_finalize");
    if (!exact_keys(drain,
                    {"status", "operational", "message_order", "drain_request",
                     "drain_status", "finalize_request", "finalize_status"},
                    drain_path,
                    error_out) ||
        drain.at("status") != "defined_not_negotiated" ||
        drain.at("operational") != false ||
        drain.at("message_order") != json{"DRAIN_REQUEST", "DRAIN_STATUS",
                                           "FINALIZE_REQUEST", "FINALIZE_STATUS"}) {
        return fail(error_out,
                    drain_path + " must remain defined_not_negotiated and non-operational");
    }
    const auto check_drain_object = [&](const char* key,
                                        const std::set<std::string>& required,
                                        const json& expected) {
        const json& item = drain.at(key);
        const std::string item_path = field(drain_path, key);
        if (!exact_keys(item, required, item_path, error_out)) {
            return false;
        }
        for (const auto& [name, value_expected] : expected.items()) {
            if (!item.contains(name) || item.at(name) != value_expected) {
                return fail(error_out, item_path + " has an invalid closed field");
            }
        }
        return true;
    };
    if (!check_drain_object("drain_request",
                            {"message_kind", "sender", "receiver", "correlation",
                             "reason_required"},
                            {{"message_kind", "DRAIN_REQUEST"},
                             {"sender", "producer"},
                             {"receiver", "recorder"},
                             {"correlation", "stream_identity_and_drain_sequence"},
                             {"reason_required", true}}) ||
        !check_drain_object("drain_status",
                            {"message_kind", "sender", "receiver", "states", "correlation",
                             "reason_required", "finalize_request_allowed_only_when"},
                            {{"message_kind", "DRAIN_STATUS"},
                             {"sender", "recorder"},
                             {"receiver", "producer"},
                             {"states", json{"draining", "drained", "failed"}},
                             {"correlation", "stream_identity_and_drain_sequence"},
                             {"reason_required", true},
                             {"finalize_request_allowed_only_when", "state=drained"}}) ||
        !check_drain_object("finalize_request",
                            {"message_kind", "sender", "receiver", "requires", "correlation",
                             "nonce", "reason_required"},
                            {{"message_kind", "FINALIZE_REQUEST"},
                             {"sender", "producer"},
                             {"receiver", "recorder"},
                             {"requires", "matching_drained_status"},
                             {"correlation", "stream_identity_and_drain_sequence"},
                             {"nonce", "fresh_16_byte_lower_hex"},
                             {"reason_required", true}}) ||
        !check_drain_object("finalize_status",
                            {"message_kind", "sender", "receiver", "states", "correlation",
                             "nonce", "reason_required", "session_finalized_only_when"},
                            {{"message_kind", "FINALIZE_STATUS"},
                             {"sender", "recorder"},
                             {"receiver", "producer"},
                             {"states", json{"finalized", "failed"}},
                             {"correlation", "stream_identity_drain_sequence_and_finalize_nonce"},
                             {"nonce", "must_equal_request"},
                             {"reason_required", true},
                             {"session_finalized_only_when", "state=finalized"}})) {
        return false;
    }

    const json& bounds = value.at("bounds");
    const std::string bounds_path = field(path, "bounds");
    if (!exact_keys(bounds,
                    {"queue_capacity_frames_per_stream", "max_outstanding_frames_per_stream",
                     "max_queue_capacity_frames_per_stream", "queue_capacity_frames_total",
                     "max_outstanding_frames_total", "overflow_action", "producer_backpressure"},
                    bounds_path,
                    error_out) ||
        !read_u32(bounds,
                  "queue_capacity_frames_per_stream",
                  bounds_path,
                  &out->queue_capacity_frames_per_stream,
                  error_out,
                  true) ||
        !read_u32(bounds,
                  "max_outstanding_frames_per_stream",
                  bounds_path,
                  &out->max_outstanding_frames_per_stream,
                  error_out,
                  true) ||
        !read_u32(bounds,
                  "max_queue_capacity_frames_per_stream",
                  bounds_path,
                  &out->max_queue_capacity_frames_per_stream,
                  error_out,
                  true) ||
        !read_u64(bounds,
                  "queue_capacity_frames_total",
                  bounds_path,
                  &out->queue_capacity_frames_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(bounds,
                  "max_outstanding_frames_total",
                  bounds_path,
                  &out->max_outstanding_frames_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        bounds.at("overflow_action") != "reject_frame_without_releasing_prior_frames" ||
        bounds.at("producer_backpressure") != "nonblocking_fail_closed") {
        return fail(error_out, bounds_path + " does not match the bounded v2 contract");
    }
    if (out->queue_capacity_frames_per_stream > kMaxQueueFrames ||
        out->max_outstanding_frames_per_stream > kMaxQueueFrames ||
        out->max_queue_capacity_frames_per_stream != kMaxQueueFrames ||
        out->max_outstanding_frames_per_stream != out->queue_capacity_frames_per_stream) {
        return fail(error_out, bounds_path + " has invalid per-stream queue bounds");
    }
    return true;
}

bool parse_storage_preflight_policy(
    const json& value,
    SpatialRoiRecorderStoragePreflightPolicyView* out,
    const std::string& path,
    std::string* error_out)
{
    if (!out ||
        !exact_keys(value,
                    {"schema_id", "schema_version", "required",
                     "reserved_free_bytes"},
                    path,
                    error_out) ||
        !read_string(value,
                     "schema_id",
                     path,
                     &out->schema_id,
                     error_out,
                     true) ||
        !read_int(value,
                  "schema_version",
                  path,
                  &out->schema_version,
                  error_out) ||
        !read_bool(value, "required", path, &out->required, error_out) ||
        !read_u64(value,
                  "reserved_free_bytes",
                  path,
                  &out->reserved_free_bytes,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true)) {
        return false;
    }
    if (out->schema_id !=
            kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
        out->schema_version !=
            kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
        !out->required || out->reserved_free_bytes == 0) {
        return fail(error_out,
                    path + " does not match the required nonzero reserve policy");
    }
    return true;
}

bool parse_aggregate_bounds(const json& value,
                            SpatialRoiRecorderAggregateBoundsView* out,
                            const std::string& path,
                            std::string* error_out)
{
    if (!exact_keys(value,
                    {"max_detach_pool_bytes_total",
                     "max_queue_bytes_total",
                     "writer_queue_max_packets_total",
                     "writer_queue_max_bytes_total",
                     "operation_timeout_ms_per_stream",
                     "max_media_bytes_total",
                     "max_evidence_bytes_total"},
                    path,
                    error_out) ||
        !read_u64(value,
                  "max_detach_pool_bytes_total",
                  path,
                  &out->max_detach_pool_bytes_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "max_queue_bytes_total",
                  path,
                  &out->max_queue_bytes_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "writer_queue_max_packets_total",
                  path,
                  &out->writer_queue_max_packets_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "writer_queue_max_bytes_total",
                  path,
                  &out->writer_queue_max_bytes_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u32(value,
                  "operation_timeout_ms_per_stream",
                  path,
                  &out->operation_timeout_ms_per_stream,
                  error_out,
                  true) ||
        !read_u64(value,
                  "max_media_bytes_total",
                  path,
                  &out->max_media_bytes_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "max_evidence_bytes_total",
                  path,
                  &out->max_evidence_bytes_total,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true)) {
        return false;
    }
    if (out->operation_timeout_ms_per_stream !=
        kSpatialRoiRecorderOperationTimeoutMs) {
        return fail(error_out,
                    path + " has an unsupported operation timeout policy");
    }
    return true;
}

bool parse_stream(const json& value,
                  const std::string& key,
                  const std::string& artifact_root,
                  const int contract_schema_version,
                  SpatialRoiRecorderStreamView* out,
                  std::string* error_out)
{
    const std::string path = "streams." + key;
    SpatialRoiRecorderStreamView identity_view;
    const std::set<std::string> keys = {
        "stream_id", "logical_stream_id", "stream_kind", "output_kind", "camera_id",
        "camera_serial", "env_key", "socket_path", "analytics_gpu_id", "recorder_gpu_id",
        "source_gpu_id", "assigned_gpu_id", "roi_id", "region_id", "arena_group_id",
        "arena_id", "recording_id", "session_id", "recording_identity_token",
        "producer_generation", "spatial_roi_plan_sha256", "frame_identity", "identity",
        "geometry_identity", "encode_profile", "encode_fps", "codec", "tuning",
        "rate_control_mode", "quality_value", "gop", "encode_queue_depth",
        "detach_pool_frames", "max_detach_pool_bytes", "max_queue_bytes",
        "writer_queue_max_packets", "writer_queue_max_bytes",
        "operation_timeout_ms", "max_frames_per_stream",
        "max_media_bytes_per_stream", "max_evidence_bytes_per_stream",
        "routing_policy",
        "expected_shard_gpu_ids", "recording_control", "rollover", "mp4", "metadata_csv",
        "keyframe_json", "perf_csv", "summary_json", "status_json", "video_sanity_json",
        "finalization_json", "recorder_log", "transport_sidecar", "evidence_jsonl",
        "evidence_manifest_json", "expected_artifacts"};
    if (!exact_keys(value, keys, path, error_out) ||
        !read_string(value, "stream_id", path, &out->stream_id, error_out, true) ||
        !read_string(value, "logical_stream_id", path, &out->logical_stream_id, error_out, true) ||
        !read_string(value, "stream_kind", path, &out->stream_kind, error_out, true) ||
        !read_string(value, "output_kind", path, &out->output_kind, error_out, true) ||
        !read_int(value, "camera_id", path, &out->camera_id, error_out) ||
        !read_string(value, "camera_serial", path, &out->camera_serial, error_out, true) ||
        !read_string(value, "env_key", path, &out->env_key, error_out, true) ||
        !read_string(value, "socket_path", path, &out->socket_path, error_out, true) ||
        !read_int(value, "analytics_gpu_id", path, &out->analytics_gpu_id, error_out) ||
        !read_int(value, "recorder_gpu_id", path, &out->recorder_gpu_id, error_out) ||
        !read_int(value, "source_gpu_id", path, &out->source_gpu_id, error_out) ||
        !read_int(value, "assigned_gpu_id", path, &out->assigned_gpu_id, error_out) ||
        !read_string(value, "roi_id", path, &out->roi_id, error_out, true) ||
        !read_string(value, "region_id", path, &out->region_id, error_out, true) ||
        !read_string(value, "arena_group_id", path, &out->arena_group_id, error_out, true) ||
        !parse_arena_id(value.at("arena_id"),
                        &out->has_arena_id,
                        &out->arena_id,
                        field(path, "arena_id"),
                        error_out) ||
        !read_string(value, "recording_id", path, &out->recording_id, error_out, true) ||
        !read_string(value, "session_id", path, &out->session_id, error_out, true) ||
        !read_string(value,
                     "recording_identity_token",
                     path,
                     &out->recording_identity_token,
                     error_out,
                     true) ||
        !read_string(value,
                     "producer_generation",
                     path,
                     &out->producer_generation,
                     error_out,
                     true) ||
        !read_string(value,
                     "spatial_roi_plan_sha256",
                     path,
                     &out->spatial_roi_plan_sha256,
                     error_out,
                     true) ||
        !parse_frame_identity(value.at("frame_identity"), out, field(path, "frame_identity"), error_out) ||
        !parse_identity(value.at("identity"),
                        &identity_view,
                        field(path, "identity"),
                        error_out) ||
        !parse_geometry(value.at("geometry_identity"),
                        &out->geometry,
                        field(path, "geometry_identity"),
                        error_out) ||
        !parse_encode_profile(value.at("encode_profile"),
                              &out->encode_profile,
                              field(path, "encode_profile"),
                              contract_schema_version,
                              error_out) ||
        !read_u32(value, "encode_fps", path, &out->encode_fps, error_out, true) ||
        !read_string(value, "codec", path, &out->codec, error_out, true) ||
        !read_string(value, "tuning", path, &out->tuning, error_out, true) ||
        !read_string(value, "rate_control_mode", path, &out->rate_control_mode, error_out, true) ||
        !read_u32(value, "quality_value", path, &out->quality_value, error_out) ||
        !read_u32(value, "gop", path, &out->gop, error_out, true) ||
        !read_u32(value, "encode_queue_depth", path, &out->encode_queue_depth, error_out, true) ||
        !read_u32(value,
                  "detach_pool_frames",
                  path,
                  &out->detach_pool_frames,
                  error_out,
                  true) ||
        !read_u64(value,
                  "max_detach_pool_bytes",
                  path,
                  &out->max_detach_pool_bytes,
                  error_out,
                  orange::spatial_roi::ipc::
                      kSpatialRoiRecorderCudaDetachMaxPoolBytes,
                  true) ||
        !read_u64(value,
                  "max_queue_bytes",
                  path,
                  &out->max_queue_bytes,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "writer_queue_max_packets",
                  path,
                  &out->writer_queue_max_packets,
                  error_out,
                  kMaxWriterQueuePackets,
                  true) ||
        !read_u64(value,
                  "writer_queue_max_bytes",
                  path,
                  &out->writer_queue_max_bytes,
                  error_out,
                  kMaxWriterQueueBytes,
                  true) ||
        !read_u32(value,
                  "operation_timeout_ms",
                  path,
                  &out->operation_timeout_ms,
                  error_out,
                  true) ||
        !read_u64(value,
                  "max_frames_per_stream",
                  path,
                  &out->max_frames_per_stream,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "max_media_bytes_per_stream",
                  path,
                  &out->max_media_bytes_per_stream,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_u64(value,
                  "max_evidence_bytes_per_stream",
                  path,
                  &out->max_evidence_bytes_per_stream,
                  error_out,
                  std::numeric_limits<std::uint64_t>::max(),
                  true) ||
        !read_string(value, "routing_policy", path, &out->routing_policy, error_out, true) ||
        !read_string(value, "mp4", path, nullptr, error_out, true) ||
        !read_string(value, "metadata_csv", path, nullptr, error_out, true) ||
        !read_string(value, "keyframe_json", path, nullptr, error_out, true) ||
        !read_string(value, "perf_csv", path, nullptr, error_out, true) ||
        !read_string(value, "summary_json", path, nullptr, error_out, true) ||
        !read_string(value, "status_json", path, nullptr, error_out, true) ||
        !read_string(value, "video_sanity_json", path, nullptr, error_out, true) ||
        !read_string(value, "finalization_json", path, nullptr, error_out, true) ||
        !read_string(value, "recorder_log", path, nullptr, error_out, true) ||
        !read_string(value, "transport_sidecar", path, nullptr, error_out, true) ||
        !read_string(value, "evidence_jsonl", path, nullptr, error_out, true) ||
        !read_string(value, "evidence_manifest_json", path, nullptr, error_out, true) ||
        !parse_expected_artifacts(value.at("expected_artifacts"),
                                  artifact_root,
                                  out,
                                  field(path, "expected_artifacts"),
                                  error_out)) {
        return false;
    }
    if (out->stream_id != key || out->stream_id != out->logical_stream_id ||
        out->stream_kind != "spatial_roi" || out->output_kind != "spatial_roi" ||
        !safe_identifier(out->camera_serial) || !safe_identifier(out->roi_id) ||
        !safe_identifier(out->region_id) || !safe_identifier(out->arena_group_id) ||
        out->recording_id.empty() || !recording_id(out->recording_id) ||
        out->session_id != out->recording_id ||
        !sha256(out->recording_identity_token) ||
        out->recording_identity_token !=
            orange::shaman_v2_recording_identity::token_for_recording_id(
                out->recording_id) ||
        !safe_identifier(out->producer_generation) ||
        !sha256(out->spatial_roi_plan_sha256) || out->analytics_gpu_id < 0 ||
        out->recorder_gpu_id < 0 || out->source_gpu_id != out->analytics_gpu_id ||
        out->assigned_gpu_id != out->recorder_gpu_id ||
        out->logical_stream_id !=
            expected_logical_stream_id(out->camera_serial, out->roi_id) ||
        out->env_key != "spatial_roi_" + out->logical_stream_id ||
        out->socket_path != expected_socket_path(
            out->recording_identity_token, out->logical_stream_id) ||
        out->socket_path.size() > kMaxSocketPathBytes ||
        out->geometry.source_coordinate_space != "camera_native_full_frame_pixels" ||
        out->geometry.video_coordinate_space != "spatial_roi_encoded_pixels" ||
        out->codec != out->encode_profile.codec ||
        out->tuning != out->encode_profile.tuning ||
        out->rate_control_mode != out->encode_profile.rate_control_mode ||
        out->quality_value != out->encode_profile.quality_value ||
        out->gop != out->encode_profile.gop_length ||
        out->routing_policy != "single_shard" || out->encode_profile.frame_rate != out->encode_fps ||
        out->encode_fps == 0 || out->encode_queue_depth == 0 ||
        out->encode_queue_depth > kMaxQueueFrames ||
        out->detach_pool_frames != out->encode_queue_depth ||
        out->writer_queue_max_packets !=
            kSpatialRoiRecorderWriterQueueMaxPackets ||
        out->writer_queue_max_bytes != kSpatialRoiRecorderWriterQueueMaxBytes ||
        out->operation_timeout_ms != kSpatialRoiRecorderOperationTimeoutMs ||
        out->operation_timeout_ms > kMaxOperationTimeoutMs) {
        return fail(error_out, path + " contains an invalid stream identity/profile");
    }
    std::uint64_t expected_detach_bytes = 0;
    std::uint64_t expected_queue_bytes = 0;
    if (!expected_nv12_queue_bytes(out->geometry,
                                   out->encode_queue_depth,
                                   &expected_queue_bytes,
                                   field(path, "max_queue_bytes"),
                                   error_out) ||
        !expected_detach_pool_bytes(out->geometry,
                                    out->detach_pool_frames,
                                    &expected_detach_bytes,
                                    field(path, "max_detach_pool_bytes"),
                                    error_out) ||
        out->max_detach_pool_bytes != expected_detach_bytes ||
        out->max_queue_bytes != expected_queue_bytes) {
        return fail(error_out,
                    path +
                        " detach-pool or encode-queue bytes do not match the closed allocation policy");
    }
    if (identity_view.recording_id != out->recording_id ||
        identity_view.recording_identity_token != out->recording_identity_token ||
        identity_view.producer_generation != out->producer_generation ||
        identity_view.spatial_roi_plan_sha256 != out->spatial_roi_plan_sha256 ||
        identity_view.camera_id != out->camera_id ||
        identity_view.camera_serial != out->camera_serial ||
        identity_view.arena_group_id != out->arena_group_id ||
        identity_view.has_arena_id != out->has_arena_id ||
        identity_view.arena_id != out->arena_id ||
        identity_view.region_id != out->region_id ||
        identity_view.roi_id != out->roi_id ||
        identity_view.logical_stream_id != out->logical_stream_id) {
        return fail(error_out, path + ".identity disagrees with stream fields");
    }
    if (!value.at("expected_shard_gpu_ids").is_array() ||
        value.at("expected_shard_gpu_ids").size() != 1) {
        return fail(error_out, path + ".expected_shard_gpu_ids must contain one GPU id");
    }
    std::uint64_t shard_gpu = 0;
    if (!json_u64(value.at("expected_shard_gpu_ids").at(0), &shard_gpu) ||
        shard_gpu > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        static_cast<int>(shard_gpu) != out->recorder_gpu_id) {
        return fail(error_out, path + ".expected_shard_gpu_ids must equal recorder_gpu_id");
    }
    out->expected_shard_gpu_ids = {out->recorder_gpu_id};

    const json& control = value.at("recording_control");
    if (!exact_keys(control, {"record_for_seconds", "clip_seconds"},
                    field(path, "recording_control"), error_out) ||
        control.at("record_for_seconds") != 0 || control.at("clip_seconds") != 0) {
        return fail(error_out, field(path, "recording_control") + " must be non-rolling");
    }
    const json& rollover = value.at("rollover");
    if (!exact_keys(rollover, {"requested", "status", "implementation"},
                    field(path, "rollover"), error_out) ||
        rollover.at("requested") != false || rollover.at("status") != "not_requested" ||
        rollover.at("implementation") != "none") {
        return fail(error_out, field(path, "rollover") + " must be non-rolling");
    }

    const std::map<std::string, std::string> direct_artifacts = {
        {"mp4", "video"},          {"metadata_csv", "metadata"},
        {"keyframe_json", "keyframes"}, {"perf_csv", "perf"},
        {"summary_json", "summary"}, {"status_json", "status"},
        {"video_sanity_json", "video_sanity"}, {"finalization_json", "finalization"},
        {"recorder_log", "recorder_log"}, {"transport_sidecar", "transport_sidecar"},
        {"evidence_jsonl", "evidence"},
        {"evidence_manifest_json", "evidence_manifest"}};
    for (const auto& [direct, artifact] : direct_artifacts) {
        if (value.at(direct).get<std::string>() != out->artifacts.at(artifact).absolute_path) {
            return fail(error_out, path + "." + direct + " disagrees with expected_artifacts");
        }
    }
    return true;
}

bool parse_spatial_roi_recorder_contract_structure(
    const nlohmann::json& value,
    const std::string& logical_stream_id,
    SpatialRoiRecorderContractView* contract_out,
    std::string* error_out)
{
    if (!contract_out) {
        return fail(error_out, "spatial ROI contract parser destination is null");
    }
    *contract_out = SpatialRoiRecorderContractView{};
    if (error_out) {
        error_out->clear();
    }
    if (logical_stream_id.empty() || !safe_identifier(logical_stream_id)) {
        return fail(error_out, "logical_stream_id selector must be a safe nonempty identifier");
    }
    try {
        const std::set<std::string> top_keys = {
            "schema_id", "schema_version", "contract_scope", "strict", "backend", "mode",
            "supervise_processes", "require_summary", "require_status", "require_video_sanity",
            "require_protocol_hello", "require_frame_identity_proof", "require_gop_routing",
            "require_storage_preflight", "storage_preflight_policy",
            "preserve_shard_mp4s", "recording_id", "session_id",
            "recording_identity_token", "producer_generation", "spatial_roi_plan_sha256",
            "recording_root", "artifact_root", "source_cadence", "source_pixel_format",
            "stream_count", "stream_order", "ipc_v2", "aggregate_bounds",
            "recording_control", "rollover", "gpu_mapping", "streams"};
        if (!exact_keys(value, top_keys, "contract", error_out) ||
            !read_string(value, "schema_id", "contract", &contract_out->schema_id, error_out, true) ||
            !read_int(value, "schema_version", "contract", &contract_out->schema_version, error_out) ||
            !read_string(value, "contract_scope", "contract", &contract_out->contract_scope, error_out, true) ||
            !read_bool(value, "strict", "contract", &contract_out->strict, error_out) ||
            !read_string(value, "backend", "contract", &contract_out->backend, error_out, true) ||
            !read_string(value, "mode", "contract", &contract_out->mode, error_out, true) ||
            !read_bool(value, "supervise_processes", "contract", &contract_out->supervise_processes, error_out) ||
            !read_bool(value, "require_summary", "contract", &contract_out->require_summary, error_out) ||
            !read_bool(value, "require_status", "contract", &contract_out->require_status, error_out) ||
            !read_bool(value, "require_video_sanity", "contract", &contract_out->require_video_sanity, error_out) ||
            !read_bool(value, "require_protocol_hello", "contract", &contract_out->require_protocol_hello, error_out) ||
            !read_bool(value, "require_frame_identity_proof", "contract", &contract_out->require_frame_identity_proof, error_out) ||
            !read_bool(value, "require_gop_routing", "contract", &contract_out->require_gop_routing, error_out) ||
            !read_bool(value, "require_storage_preflight", "contract", &contract_out->require_storage_preflight, error_out) ||
            !parse_storage_preflight_policy(
                value.at("storage_preflight_policy"),
                &contract_out->storage_preflight_policy,
                "contract.storage_preflight_policy",
                error_out) ||
            !read_bool(value, "preserve_shard_mp4s", "contract", &contract_out->preserve_shard_mp4s, error_out) ||
            !read_string(value, "recording_id", "contract", &contract_out->recording_id, error_out, true) ||
            !read_string(value, "session_id", "contract", &contract_out->session_id, error_out, true) ||
            !read_string(value, "recording_identity_token", "contract", &contract_out->recording_identity_token, error_out, true) ||
            !read_string(value, "producer_generation", "contract", &contract_out->producer_generation, error_out, true) ||
            !read_string(value, "spatial_roi_plan_sha256", "contract", &contract_out->spatial_roi_plan_sha256, error_out, true) ||
            !read_string(value, "recording_root", "contract", &contract_out->recording_root, error_out, true) ||
            !read_string(value, "artifact_root", "contract", &contract_out->artifact_root, error_out, true) ||
            !read_string(value, "source_cadence", "contract", &contract_out->source_cadence, error_out, true) ||
            !read_string(value, "source_pixel_format", "contract", &contract_out->source_pixel_format, error_out, true) ||
            !read_u32(value, "stream_count", "contract", &contract_out->stream_count, error_out, true) ||
            !parse_ipc(value.at("ipc_v2"), &contract_out->ipc_v2, "contract.ipc_v2", error_out) ||
            !parse_aggregate_bounds(value.at("aggregate_bounds"),
                                    &contract_out->aggregate_bounds,
                                    "contract.aggregate_bounds",
                                    error_out)) {
            return false;
        }
        const bool legacy_contract =
            contract_out->schema_version ==
            kLegacySpatialRoiRecorderContractSchemaVersion;
        const bool current_contract =
            contract_out->schema_version ==
            kSpatialRoiRecorderContractSchemaVersion;
        const char* expected_scope =
            legacy_contract ? kLegacySpatialRoiRecorderContractScope
                            : kSpatialRoiRecorderContractScope;
        const char* expected_mode =
            legacy_contract ? kLegacySpatialRoiRecorderContractMode
                            : kSpatialRoiRecorderContractMode;
        const char* expected_backend = legacy_contract ? kLegacyBackend : kBackend;
        if (contract_out->schema_id != kSpatialRoiRecorderContractSchemaId ||
            (!legacy_contract && !current_contract) ||
            contract_out->contract_scope != expected_scope ||
            !contract_out->strict ||
            contract_out->backend != expected_backend ||
            contract_out->mode != expected_mode ||
            !contract_out->supervise_processes || !contract_out->require_summary ||
            !contract_out->require_status || !contract_out->require_video_sanity ||
            !contract_out->require_protocol_hello || !contract_out->require_frame_identity_proof ||
            contract_out->require_gop_routing || !contract_out->require_storage_preflight ||
            contract_out->storage_preflight_policy.schema_id !=
                kSpatialRoiRecorderStoragePreflightPolicySchemaId ||
            contract_out->storage_preflight_policy.schema_version !=
                kSpatialRoiRecorderStoragePreflightPolicySchemaVersion ||
            !contract_out->storage_preflight_policy.required ||
            contract_out->storage_preflight_policy.reserved_free_bytes == 0 ||
            contract_out->preserve_shard_mp4s || !recording_id(contract_out->recording_id) ||
            contract_out->session_id != contract_out->recording_id ||
            !sha256(contract_out->recording_identity_token) ||
            contract_out->recording_identity_token !=
                orange::shaman_v2_recording_identity::token_for_recording_id(
                    contract_out->recording_id) ||
            !safe_identifier(contract_out->producer_generation) ||
            !sha256(contract_out->spatial_roi_plan_sha256) ||
            contract_out->source_cadence != kExpectedCadence ||
            contract_out->source_pixel_format != kExpectedPixelFormat) {
            return fail(error_out,
                        legacy_contract
                            ? "contract identity or strict flags do not match schema v4"
                            : "contract identity or strict flags do not match schema v5");
        }
        std::filesystem::path recording_root;
        std::filesystem::path artifact_root;
        if (!safe_absolute_path(contract_out->recording_root,
                                &recording_root,
                                error_out,
                                "contract.recording_root") ||
            !safe_absolute_path(contract_out->artifact_root,
                                &artifact_root,
                                error_out,
                                "contract.artifact_root")) {
            return false;
        }
        const std::string artifact_relative = artifact_root.lexically_relative(recording_root).generic_string();
        if (!safe_relative_path(artifact_relative) || artifact_relative != "external_spatial_roi_recorder") {
            return fail(error_out, "contract.artifact_root must be recording_root/external_spatial_roi_recorder");
        }

        const json& stream_order = value.at("stream_order");
        if (!stream_order.is_array() || stream_order.size() != contract_out->stream_count) {
            return fail(error_out, "contract.stream_order must match stream_count");
        }
        std::set<std::string> ordered_ids;
        contract_out->stream_order.clear();
        for (const auto& item : stream_order) {
            if (!item.is_string() || !safe_identifier(item.get<std::string>()) ||
                !ordered_ids.insert(item.get<std::string>()).second) {
                return fail(error_out, "contract.stream_order must contain unique safe stream IDs");
            }
            contract_out->stream_order.push_back(item.get<std::string>());
        }

        const json& control = value.at("recording_control");
        if (!exact_keys(control, {"record_for_seconds", "clip_seconds"},
                        "contract.recording_control", error_out) ||
            control.at("record_for_seconds") != 0 || control.at("clip_seconds") != 0) {
            return fail(error_out, "contract.recording_control must be non-rolling");
        }
        const json& rollover = value.at("rollover");
        if (!exact_keys(rollover, {"requested", "status", "implementation"},
                        "contract.rollover", error_out) ||
            rollover.at("requested") != false || rollover.at("status") != "not_requested" ||
            rollover.at("implementation") != "none") {
            return fail(error_out, "contract.rollover must be non-rolling");
        }

        const json& mapping = value.at("gpu_mapping");
        if (!exact_keys(mapping,
                        {"analytics_gpu_by_camera_serial", "recorder_gpu_by_logical_stream_id"},
                        "contract.gpu_mapping",
                        error_out)) {
            return false;
        }
        const auto parse_gpu_map = [&](const json& object,
                                       const std::string& path,
                                       auto* destination) {
            if (!object.is_object() || object.empty()) {
                return fail(error_out, path + " must be a nonempty object");
            }
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!safe_identifier(it.key())) {
                    return fail(error_out, path + " contains an unsafe key");
                }
                int gpu = -1;
                if (!it.value().is_number_integer() && !it.value().is_number_unsigned()) {
                    return fail(error_out, path + "." + it.key() + " must be a GPU integer");
                }
                std::uint64_t gpu_u64 = 0;
                if (!json_u64(it.value(), &gpu_u64) ||
                    gpu_u64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                    return fail(error_out, path + "." + it.key() + " must be nonnegative");
                }
                gpu = static_cast<int>(gpu_u64);
                (*destination)[it.key()] = gpu;
            }
            return true;
        };
        if (!parse_gpu_map(mapping.at("analytics_gpu_by_camera_serial"),
                           "contract.gpu_mapping.analytics_gpu_by_camera_serial",
                           &contract_out->analytics_gpu_by_camera_serial) ||
            !parse_gpu_map(mapping.at("recorder_gpu_by_logical_stream_id"),
                           "contract.gpu_mapping.recorder_gpu_by_logical_stream_id",
                           &contract_out->recorder_gpu_by_logical_stream_id)) {
            return false;
        }

        const json& streams = value.at("streams");
        if (!streams.is_object() || streams.size() != contract_out->stream_count ||
            streams.empty()) {
            return fail(error_out, "contract.streams must match stream_count and be nonempty");
        }
        contract_out->streams.clear();
        std::set<std::string> camera_serials;
        std::set<std::string> artifact_paths;
        std::uint64_t max_detach_pool_bytes_total = 0;
        std::uint64_t max_queue_bytes_total = 0;
        std::uint64_t writer_queue_max_packets_total = 0;
        std::uint64_t writer_queue_max_bytes_total = 0;
        std::uint64_t max_media_bytes_total = 0;
        std::uint64_t max_evidence_bytes_total = 0;
        std::uint64_t max_frames_per_stream = 0;
        std::uint64_t max_media_bytes_per_stream = 0;
        std::uint64_t max_evidence_bytes_per_stream = 0;
        bool has_recording_limits = false;
        for (const std::string& stream_id : contract_out->stream_order) {
            if (!streams.contains(stream_id)) {
                return fail(error_out, "contract.streams is missing stream_order entry " + stream_id);
            }
            SpatialRoiRecorderStreamView stream;
            if (!parse_stream(streams.at(stream_id),
                              stream_id,
                              contract_out->artifact_root,
                              contract_out->schema_version,
                              &stream,
                              error_out)) {
                return false;
            }
            if (stream.recording_id != contract_out->recording_id ||
                stream.recording_identity_token != contract_out->recording_identity_token ||
                stream.producer_generation != contract_out->producer_generation ||
                stream.spatial_roi_plan_sha256 != contract_out->spatial_roi_plan_sha256 ||
                stream.encode_queue_depth !=
                    contract_out->ipc_v2.queue_capacity_frames_per_stream ||
                stream.recorder_gpu_id !=
                    contract_out->recorder_gpu_by_logical_stream_id.at(stream.logical_stream_id) ||
                !contract_out->analytics_gpu_by_camera_serial.count(stream.camera_serial) ||
                stream.analytics_gpu_id !=
                    contract_out->analytics_gpu_by_camera_serial.at(stream.camera_serial)) {
                return fail(error_out, "contract stream identity/GPU mapping disagrees with parent");
            }
            if (stream.operation_timeout_ms !=
                    contract_out->aggregate_bounds.operation_timeout_ms_per_stream ||
                !checked_add(max_detach_pool_bytes_total,
                             stream.max_detach_pool_bytes,
                             &max_detach_pool_bytes_total) ||
                !checked_add(max_queue_bytes_total,
                             stream.max_queue_bytes,
                             &max_queue_bytes_total) ||
                !checked_add(writer_queue_max_packets_total,
                             stream.writer_queue_max_packets,
                             &writer_queue_max_packets_total) ||
                !checked_add(writer_queue_max_bytes_total,
                             stream.writer_queue_max_bytes,
                             &writer_queue_max_bytes_total) ||
                !checked_add(max_media_bytes_total,
                             stream.max_media_bytes_per_stream,
                             &max_media_bytes_total) ||
                !checked_add(max_evidence_bytes_total,
                             stream.max_evidence_bytes_per_stream,
                             &max_evidence_bytes_total)) {
                return fail(error_out,
                            "contract stream recorder bounds overflow or disagree with parent");
            }
            if (!has_recording_limits) {
                max_frames_per_stream = stream.max_frames_per_stream;
                max_media_bytes_per_stream =
                    stream.max_media_bytes_per_stream;
                max_evidence_bytes_per_stream =
                    stream.max_evidence_bytes_per_stream;
                has_recording_limits = true;
            } else if (stream.max_frames_per_stream != max_frames_per_stream ||
                       stream.max_media_bytes_per_stream !=
                           max_media_bytes_per_stream ||
                       stream.max_evidence_bytes_per_stream !=
                           max_evidence_bytes_per_stream) {
                return fail(
                    error_out,
                    "contract streams must share one authenticated recording_limits policy");
            }
            for (const auto& [kind, artifact] : stream.artifacts) {
                (void)kind;
                if (!artifact_paths.insert(artifact.absolute_path).second) {
                    return fail(error_out,
                                "contract artifact paths must be globally unique");
                }
            }
            camera_serials.insert(stream.camera_serial);
            contract_out->streams.push_back(std::move(stream));
        }
        if (streams.size() != ordered_ids.size() ||
            contract_out->recorder_gpu_by_logical_stream_id.size() != ordered_ids.size() ||
            contract_out->analytics_gpu_by_camera_serial.size() != camera_serials.size()) {
            return fail(error_out, "contract GPU mapping does not exactly cover its streams");
        }
        if (contract_out->ipc_v2.queue_capacity_frames_total !=
                static_cast<std::uint64_t>(contract_out->stream_count) *
                    contract_out->ipc_v2.queue_capacity_frames_per_stream ||
            contract_out->ipc_v2.max_outstanding_frames_total !=
                contract_out->ipc_v2.queue_capacity_frames_total ||
            contract_out->aggregate_bounds.max_detach_pool_bytes_total !=
                max_detach_pool_bytes_total ||
            contract_out->aggregate_bounds.max_queue_bytes_total !=
                max_queue_bytes_total ||
            contract_out->aggregate_bounds.writer_queue_max_packets_total !=
                writer_queue_max_packets_total ||
            contract_out->aggregate_bounds.writer_queue_max_bytes_total !=
                writer_queue_max_bytes_total ||
            contract_out->aggregate_bounds.max_media_bytes_total !=
                max_media_bytes_total ||
            contract_out->aggregate_bounds.max_evidence_bytes_total !=
                max_evidence_bytes_total) {
            return fail(error_out,
                        "contract aggregate bounds disagree with its streams");
        }
        const auto selected = std::find_if(
            contract_out->streams.begin(), contract_out->streams.end(),
            [&](const auto& stream) { return stream.logical_stream_id == logical_stream_id; });
        if (selected == contract_out->streams.end()) {
            return fail(error_out, "requested logical_stream_id is not present exactly once");
        }
        contract_out->selected_stream = *selected;
        return true;
    } catch (const std::exception& ex) {
        return fail(error_out, std::string("spatial ROI recorder contract parse failed: ") + ex.what());
    } catch (...) {
        return fail(error_out, "spatial ROI recorder contract parse failed: unknown exception");
    }
}

bool read_bounded_contract_file(const std::filesystem::path& path,
                                json* value_out,
                                std::string* error_out)
{
    const int raw_fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
        return fail(error_out,
                    "failed to open spatial ROI recorder contract as a non-symlink regular file");
    }
    ScopedFileDescriptor fd(raw_fd);

    struct stat status {};
    if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return fail(error_out,
                    "spatial ROI recorder contract path is not a regular file");
    }
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaxContractBytes) {
        return fail(error_out,
                    "spatial ROI recorder contract file exceeds bounded size");
    }

    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 64U * 1024U> chunk{};
    while (true) {
        const std::size_t remaining =
            (kMaxContractBytes + 1U) - bytes.size();
        const std::size_t requested = std::min(remaining, chunk.size());
        const ssize_t count = ::read(fd.get(), chunk.data(), requested);
        if (count > 0) {
            bytes.append(chunk.data(), static_cast<std::size_t>(count));
            if (bytes.size() > kMaxContractBytes) {
                return fail(error_out,
                            "spatial ROI recorder contract file exceeds bounded size");
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return fail(error_out,
                    "failed while reading spatial ROI recorder contract file");
    }

    BoundedContractJsonSax validator;
    if (!json::sax_parse(bytes, &validator)) {
        return fail(
            error_out,
            "spatial ROI recorder contract JSON exceeds structural bounds, contains duplicate keys, or is invalid");
    }

    const json value = json::parse(bytes, nullptr, false);
    if (value.is_discarded()) {
        return fail(error_out,
                    "spatial ROI recorder contract file is invalid JSON");
    }
    *value_out = value;
    return true;
}

}  // namespace

bool parse_spatial_roi_recorder_contract(
    const nlohmann::json& value,
    const nlohmann::json& verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderContractView* contract_out,
    std::string* error_out)
{
    if (!contract_out) {
        return fail(error_out, "spatial ROI contract parser destination is null");
    }
    *contract_out = SpatialRoiRecorderContractView{};
    try {
        json expected_contract;
        std::string authority_error;
        if (!build_spatial_roi_recorder_contract(verified_plan,
                                                 expected_recording_root,
                                                 expected_gpu_mapping,
                                                 &expected_contract,
                                                 &authority_error)) {
            return fail(error_out,
                        "spatial ROI recorder authority is invalid: " +
                            authority_error);
        }
        if (value != expected_contract) {
            return fail(
                error_out,
                "spatial ROI recorder contract does not exactly match its verified plan, recording root, and GPU mapping");
        }
        return parse_spatial_roi_recorder_contract_structure(
            value, logical_stream_id, contract_out, error_out);
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI recorder authority verification failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI recorder authority verification failed: unknown exception");
    }
}

bool parse_spatial_roi_recorder_contract_file(
    const std::filesystem::path& path,
    const nlohmann::json& verified_plan,
    const std::string& expected_recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderContractView* contract_out,
    std::string* error_out)
{
    if (!contract_out) {
        return fail(error_out, "spatial ROI contract parser destination is null");
    }
    *contract_out = SpatialRoiRecorderContractView{};
    if (path.empty()) {
        return fail(error_out, "spatial ROI recorder contract path is empty");
    }
    if (path.native().find('\0') != std::filesystem::path::string_type::npos) {
        return fail(error_out,
                    "spatial ROI recorder contract path contains a NUL byte");
    }
    try {
        json value;
        if (!read_bounded_contract_file(path, &value, error_out)) {
            return false;
        }
        return parse_spatial_roi_recorder_contract(value,
                                                   verified_plan,
                                                   expected_recording_root,
                                                   expected_gpu_mapping,
                                                   logical_stream_id,
                                                   contract_out,
                                                   error_out);
    } catch (const std::exception& ex) {
        return fail(error_out, std::string("spatial ROI recorder contract file read failed: ") + ex.what());
    } catch (...) {
        return fail(error_out, "spatial ROI recorder contract file read failed: unknown exception");
    }
}

}  // namespace orange::session::spatial_roi
