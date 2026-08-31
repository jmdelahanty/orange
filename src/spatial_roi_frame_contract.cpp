#include "spatial_roi_frame_contract.h"

#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace orange::spatial_roi {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxIdentifierLength = 64;
constexpr std::size_t kMaxRecordingIdLength = 512;
constexpr std::size_t kMaxRoutingPolicyLength = 64;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool exact_keys(const json& value,
                const std::set<std::string>& required,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const std::string& key : required) {
        if (!value.contains(key)) {
            return fail(error_out, path + "." + key + " is required");
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (required.count(it.key()) == 0) {
            return fail(error_out,
                        path + "." + it.key() +
                            " is not allowed by spatial ROI schema v1");
        }
    }
    return true;
}

bool is_ascii_alnum(const unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool is_safe_identifier(const std::string& value,
                        const std::size_t max_length = kMaxIdentifierLength)
{
    if (value.empty() || value.size() > max_length ||
        !is_ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(
        value.begin() + 1,
        value.end(),
        [](const unsigned char ch) {
            return is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.';
        });
}

// Keep this contract's validation aligned with the verified-plan naming rule
// without linking the plan materializer into the host-only contract target.
std::string expected_logical_stream_id(const std::string& camera_serial,
                                       const std::string& roi_id)
{
    return camera_serial + "_spatial_roi_" + roi_id;
}

bool is_recording_id(const std::string& value)
{
    if (value.empty() || value.size() > kMaxRecordingIdLength ||
        std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        return false;
    }
    return std::none_of(
        value.begin(),
        value.end(),
        [](const unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

bool is_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(
        value.begin() + 7,
        value.end(),
        [](const unsigned char ch) {
            return (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f');
        });
}

bool is_nonnegative_u64(const json& value, std::uint64_t* out)
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

bool is_positive_u64(const json& value, std::uint64_t* out)
{
    return is_nonnegative_u64(value, out) && *out > 0;
}

bool is_nonnegative_int(const json& value, int* out)
{
    std::uint64_t parsed = 0;
    if (!is_nonnegative_u64(value, &parsed) ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool read_string(const json& object,
                 const char* key,
                 const std::string& path,
                 std::string* out,
                 std::string* error_out)
{
    const json& value = object.at(key);
    if (!value.is_string()) {
        return fail(error_out, path + "." + key + " must be a string");
    }
    *out = value.get<std::string>();
    return true;
}

bool read_u64(const json& object,
              const char* key,
              const std::string& path,
              std::uint64_t* out,
              std::string* error_out,
              const bool positive = false)
{
    const json& value = object.at(key);
    const bool valid = positive ? is_positive_u64(value, out)
                                : is_nonnegative_u64(value, out);
    if (!valid) {
        return fail(error_out,
                    path + "." + key +
                        (positive ? " must be a positive integer"
                                  : " must be a non-negative integer"));
    }
    return true;
}

bool read_u32(const json& object,
              const char* key,
              const std::string& path,
              std::uint32_t* out,
              std::string* error_out,
              const bool positive = false)
{
    std::uint64_t parsed = 0;
    if (!read_u64(object, key, path, &parsed, error_out, positive) ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            return fail(error_out,
                        path + "." + key + " exceeds uint32 capacity");
        }
        return false;
    }
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool read_int(const json& object,
              const char* key,
              const std::string& path,
              int* out,
              std::string* error_out)
{
    if (!is_nonnegative_int(object.at(key), out)) {
        return fail(error_out,
                    path + "." + key + " must be a non-negative integer");
    }
    return true;
}

bool parse_raster(const json& value,
                  SpatialRoiFrameRaster* out,
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
                SpatialRoiFrameRect* out,
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
                   SpatialRoiFramePadding* out,
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
    if (!read_u64(value, "value_mono8", path, &value_mono8, error_out) ||
        value_mono8 > 255) {
        return fail(error_out,
                    path + ".value_mono8 must be an integer in [0,255]");
    }
    out->value_mono8 = static_cast<std::uint8_t>(value_mono8);
    return true;
}

json raster_to_json(const SpatialRoiFrameRaster& raster)
{
    return {{"width", raster.width}, {"height", raster.height}};
}

json rect_to_json(const SpatialRoiFrameRect& rect)
{
    return {{"x", rect.x},
            {"y", rect.y},
            {"width", rect.width},
            {"height", rect.height}};
}

json padding_to_json(const SpatialRoiFramePadding& padding)
{
    return {{"left", padding.left},
            {"top", padding.top},
            {"right", padding.right},
            {"bottom", padding.bottom},
            {"value_mono8", padding.value_mono8}};
}

bool checked_extent(const std::uint32_t origin,
                    const std::uint32_t extent,
                    std::uint64_t* end_out)
{
    const std::uint64_t end = static_cast<std::uint64_t>(origin) + extent;
    if (end > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *end_out = end;
    return true;
}

bool rect_fits(const SpatialRoiFrameRaster& raster,
               const SpatialRoiFrameRect& rect)
{
    if (raster.width == 0 || raster.height == 0 || rect.width == 0 ||
        rect.height == 0) {
        return false;
    }
    std::uint64_t right = 0;
    std::uint64_t bottom = 0;
    return checked_extent(rect.x, rect.width, &right) &&
           checked_extent(rect.y, rect.height, &bottom) &&
           right <= raster.width && bottom <= raster.height;
}

bool checked_pixels(const SpatialRoiFrameRaster& raster,
                    std::uint64_t* pixels_out)
{
    if (raster.width == 0 || raster.height == 0 ||
        raster.height > std::numeric_limits<std::uint64_t>::max() /
                             raster.width) {
        return false;
    }
    *pixels_out = static_cast<std::uint64_t>(raster.width) * raster.height;
    return true;
}

bool valid_outcome(const std::string& value)
{
    return value == "accepted" || value == "encoded" || value == "failed";
}

}  // namespace

std::size_t SpatialRoiFrameKeyHash::operator()(
    const SpatialRoiFrameKey& key) const noexcept
{
    const std::size_t stream_hash = std::hash<std::string>{}(key.logical_stream_id);
    const std::size_t frame_hash = std::hash<std::uint64_t>{}(key.recording_frame_id);
    return stream_hash ^ (frame_hash + static_cast<std::size_t>(0x9e3779b9) +
                          (stream_hash << 6) + (stream_hash >> 2));
}

bool validate_spatial_roi_frame_descriptor(
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out)
{
    auto reject = [&](const std::string& reason) {
        return fail(error_out, "spatial ROI frame descriptor invalid: " + reason);
    };

    if (!is_recording_id(descriptor.recording_id)) {
        return reject("recording_id must be printable text of at most 512 bytes");
    }
    if (!is_sha256(descriptor.recording_identity_token) ||
        descriptor.recording_identity_token !=
            shaman_v2_recording_identity::token_for_recording_id(
                descriptor.recording_id)) {
        return reject("recording_identity_token does not match recording_id");
    }
    if (!is_safe_identifier(descriptor.producer_generation)) {
        return reject("producer_generation must be a safe identifier");
    }
    if (descriptor.camera_id < 0 ||
        !is_safe_identifier(descriptor.camera_serial)) {
        return reject("camera_id must be non-negative and camera_serial safe");
    }
    if (descriptor.local_frame_id == 0 || descriptor.camera_frame_id == 0 ||
        descriptor.recording_frame_id == 0 ||
        descriptor.roi_stream_frame_index == 0 ||
        descriptor.camera_timestamp_ns == 0 || descriptor.timestamp_sys_ns == 0) {
        return reject("source frame ids, ROI frame index, and clocks must be positive");
    }
    if (!is_safe_identifier(descriptor.roi_id) ||
        !is_safe_identifier(descriptor.region_id) ||
        !is_safe_identifier(descriptor.arena_group_id) ||
        (!descriptor.arena_id.empty() && !is_safe_identifier(descriptor.arena_id)) ||
        !is_safe_identifier(descriptor.logical_stream_id)) {
        return reject("ROI, arena, and logical stream identifiers are invalid");
    }
    if (descriptor.logical_stream_id !=
        expected_logical_stream_id(descriptor.camera_serial, descriptor.roi_id)) {
        return reject("logical_stream_id does not match the verified v1 naming rule");
    }
    if (!is_sha256(descriptor.spatial_roi_plan_sha256)) {
        return reject("spatial_roi_plan_sha256 must be sha256:<lowercase hex>");
    }
    if (!rect_fits(descriptor.native_raster, descriptor.content_rect)) {
        return reject("content_rect does not fit native_raster");
    }
    if (!rect_fits(descriptor.encoded_raster,
                   descriptor.encoded_content_rect)) {
        return reject("encoded_content_rect does not fit encoded_raster");
    }
    if (descriptor.content_rect.width !=
            descriptor.encoded_content_rect.width ||
        descriptor.content_rect.height !=
            descriptor.encoded_content_rect.height) {
        return reject("encoded content dimensions must equal native content dimensions");
    }
    std::uint64_t encoded_right = 0;
    std::uint64_t encoded_bottom = 0;
    if (!checked_extent(descriptor.encoded_content_rect.x,
                        descriptor.encoded_content_rect.width,
                        &encoded_right) ||
        !checked_extent(descriptor.encoded_content_rect.y,
                        descriptor.encoded_content_rect.height,
                        &encoded_bottom)) {
        return reject("encoded geometry overflows uint32");
    }
    if (descriptor.padding.left != descriptor.encoded_content_rect.x ||
        descriptor.padding.top != descriptor.encoded_content_rect.y ||
        descriptor.padding.right !=
            descriptor.encoded_raster.width - encoded_right ||
        descriptor.padding.bottom !=
            descriptor.encoded_raster.height - encoded_bottom ||
        descriptor.padding.value_mono8 != 0) {
        return reject("padding does not exactly describe the encoded raster");
    }
    // The verified v1 plan materializes content at the encoded origin and
    // permits only right/bottom alignment padding. The padding value itself
    // is always zero-filled Mono8.
    if (descriptor.encoded_content_rect.x != 0 ||
        descriptor.encoded_content_rect.y != 0 ||
        descriptor.padding.left != 0 || descriptor.padding.top != 0) {
        return reject(
            "v1 encoded content must start at (0,0) with no left/top padding");
    }
    if (descriptor.source_pixel_format != kSpatialRoiMono8PixelFormat) {
        return reject("source_pixel_format must be mono8");
    }
    std::uint64_t encoded_pixels = 0;
    if (!checked_pixels(descriptor.encoded_raster, &encoded_pixels) ||
        descriptor.bytes != encoded_pixels) {
        return reject("bytes must equal encoded_raster.width * encoded_raster.height");
    }
    if (descriptor.source_gpu_id < 0 || descriptor.assigned_gpu_id < 0 ||
        descriptor.assigned_shard_id < 0 ||
        !is_safe_identifier(descriptor.routing_policy, kMaxRoutingPolicyLength)) {
        return reject("routing and GPU fields are invalid");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_frame_metadata(
    const SpatialRoiFrameMetadata& metadata,
    std::string* error_out)
{
    if (!validate_spatial_roi_frame_descriptor(metadata.frame, error_out)) {
        return false;
    }
    if (!valid_outcome(metadata.submission_outcome)) {
        return fail(error_out,
                    "spatial ROI frame metadata submission_outcome must be "
                    "accepted, encoded, or failed");
    }
    if (metadata.output_frame_index == 0) {
        return fail(error_out,
                    "spatial ROI frame metadata output_frame_index must be positive");
    }
    if (metadata.output_frame_index != metadata.frame.roi_stream_frame_index) {
        return fail(error_out,
                    "output_frame_index must equal roi_stream_frame_index for an "
                    "admitted v1 ROI frame");
    }
    if (metadata.submission_outcome == "encoded" &&
        (metadata.packet_count == 0 || metadata.encoded_bytes == 0)) {
        return fail(error_out,
                    "encoded metadata requires positive packet_count and encoded_bytes");
    }
    if (metadata.submission_outcome != "encoded" &&
        (metadata.packet_count != 0 || metadata.encoded_bytes != 0)) {
        return fail(error_out,
                    "non-encoded metadata must not claim packets or encoded bytes");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

nlohmann::json spatial_roi_frame_descriptor_to_json(
    const SpatialRoiFrameDescriptor& descriptor)
{
    return {
        {"schema_id", kSpatialRoiFrameDescriptorSchemaId},
        {"schema_version", kSpatialRoiFrameDescriptorSchemaVersion},
        {"recording_id", descriptor.recording_id},
        {"recording_identity_token", descriptor.recording_identity_token},
        {"producer_generation", descriptor.producer_generation},
        {"camera_id", descriptor.camera_id},
        {"camera_serial", descriptor.camera_serial},
        {"local_frame_id", descriptor.local_frame_id},
        {"camera_frame_id", descriptor.camera_frame_id},
        {"recording_frame_id", descriptor.recording_frame_id},
        {"roi_stream_frame_index", descriptor.roi_stream_frame_index},
        {"camera_timestamp_ns", descriptor.camera_timestamp_ns},
        {"timestamp_sys_ns", descriptor.timestamp_sys_ns},
        {"roi_id", descriptor.roi_id},
        {"region_id", descriptor.region_id},
        {"arena_group_id", descriptor.arena_group_id},
        {"arena_id", descriptor.arena_id},
        {"logical_stream_id", descriptor.logical_stream_id},
        {"spatial_roi_plan_sha256", descriptor.spatial_roi_plan_sha256},
        {"native_raster", raster_to_json(descriptor.native_raster)},
        {"content_rect", rect_to_json(descriptor.content_rect)},
        {"encoded_raster", raster_to_json(descriptor.encoded_raster)},
        {"encoded_content_rect", rect_to_json(descriptor.encoded_content_rect)},
        {"padding", padding_to_json(descriptor.padding)},
        {"source_pixel_format", descriptor.source_pixel_format},
        {"bytes", descriptor.bytes},
        {"source_gpu_id", descriptor.source_gpu_id},
        {"assigned_gpu_id", descriptor.assigned_gpu_id},
        {"assigned_shard_id", descriptor.assigned_shard_id},
        {"routing_policy", descriptor.routing_policy},
    };
}

bool spatial_roi_frame_descriptor_from_json(
    const nlohmann::json& value,
    SpatialRoiFrameDescriptor* descriptor_out,
    std::string* error_out)
{
    if (!descriptor_out) {
        return fail(error_out, "spatial ROI frame descriptor destination is null");
    }
    static const std::set<std::string> keys = {
        "schema_id", "schema_version", "recording_id",
        "recording_identity_token", "producer_generation", "camera_id",
        "camera_serial", "local_frame_id", "camera_frame_id",
        "recording_frame_id", "roi_stream_frame_index", "camera_timestamp_ns",
        "timestamp_sys_ns", "roi_id", "region_id", "arena_group_id", "arena_id",
        "logical_stream_id", "spatial_roi_plan_sha256", "native_raster",
        "content_rect", "encoded_raster", "encoded_content_rect", "padding",
        "source_pixel_format", "bytes", "source_gpu_id", "assigned_gpu_id",
        "assigned_shard_id", "routing_policy"};
    if (!exact_keys(value, keys, "frame_descriptor", error_out)) {
        return false;
    }
    if (!value.at("schema_id").is_string() ||
        value.at("schema_id").get<std::string>() !=
            kSpatialRoiFrameDescriptorSchemaId) {
        return fail(error_out, "frame_descriptor.schema_id is not spatial ROI v1");
    }
    std::uint64_t schema_version = 0;
    if (!is_nonnegative_u64(value.at("schema_version"), &schema_version) ||
        schema_version != kSpatialRoiFrameDescriptorSchemaVersion) {
        return fail(error_out, "frame_descriptor.schema_version must be 1");
    }
    SpatialRoiFrameDescriptor parsed;
    if (!read_string(value, "recording_id", "frame_descriptor", &parsed.recording_id, error_out) ||
        !read_string(value, "recording_identity_token", "frame_descriptor", &parsed.recording_identity_token, error_out) ||
        !read_string(value, "producer_generation", "frame_descriptor", &parsed.producer_generation, error_out) ||
        !read_int(value, "camera_id", "frame_descriptor", &parsed.camera_id, error_out) ||
        !read_string(value, "camera_serial", "frame_descriptor", &parsed.camera_serial, error_out) ||
        !read_u64(value, "local_frame_id", "frame_descriptor", &parsed.local_frame_id, error_out, true) ||
        !read_u64(value, "camera_frame_id", "frame_descriptor", &parsed.camera_frame_id, error_out, true) ||
        !read_u64(value, "recording_frame_id", "frame_descriptor", &parsed.recording_frame_id, error_out, true) ||
        !read_u64(value, "roi_stream_frame_index", "frame_descriptor", &parsed.roi_stream_frame_index, error_out, true) ||
        !read_u64(value, "camera_timestamp_ns", "frame_descriptor", &parsed.camera_timestamp_ns, error_out, true) ||
        !read_u64(value, "timestamp_sys_ns", "frame_descriptor", &parsed.timestamp_sys_ns, error_out, true) ||
        !read_string(value, "roi_id", "frame_descriptor", &parsed.roi_id, error_out) ||
        !read_string(value, "region_id", "frame_descriptor", &parsed.region_id, error_out) ||
        !read_string(value, "arena_group_id", "frame_descriptor", &parsed.arena_group_id, error_out) ||
        !read_string(value, "arena_id", "frame_descriptor", &parsed.arena_id, error_out) ||
        !read_string(value, "logical_stream_id", "frame_descriptor", &parsed.logical_stream_id, error_out) ||
        !read_string(value, "spatial_roi_plan_sha256", "frame_descriptor", &parsed.spatial_roi_plan_sha256, error_out) ||
        !parse_raster(value.at("native_raster"), &parsed.native_raster, "frame_descriptor.native_raster", error_out) ||
        !parse_rect(value.at("content_rect"), &parsed.content_rect, "frame_descriptor.content_rect", error_out) ||
        !parse_raster(value.at("encoded_raster"), &parsed.encoded_raster, "frame_descriptor.encoded_raster", error_out) ||
        !parse_rect(value.at("encoded_content_rect"), &parsed.encoded_content_rect, "frame_descriptor.encoded_content_rect", error_out) ||
        !parse_padding(value.at("padding"), &parsed.padding, "frame_descriptor.padding", error_out) ||
        !read_string(value, "source_pixel_format", "frame_descriptor", &parsed.source_pixel_format, error_out) ||
        !read_u64(value, "bytes", "frame_descriptor", &parsed.bytes, error_out) ||
        !read_int(value, "source_gpu_id", "frame_descriptor", &parsed.source_gpu_id, error_out) ||
        !read_int(value, "assigned_gpu_id", "frame_descriptor", &parsed.assigned_gpu_id, error_out) ||
        !read_int(value, "assigned_shard_id", "frame_descriptor", &parsed.assigned_shard_id, error_out) ||
        !read_string(value, "routing_policy", "frame_descriptor", &parsed.routing_policy, error_out)) {
        return false;
    }
    if (!validate_spatial_roi_frame_descriptor(parsed, error_out)) {
        return false;
    }
    *descriptor_out = std::move(parsed);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

nlohmann::json spatial_roi_frame_metadata_to_json(
    const SpatialRoiFrameMetadata& metadata)
{
    return {
        {"schema_id", kSpatialRoiFrameMetadataSchemaId},
        {"schema_version", kSpatialRoiFrameMetadataSchemaVersion},
        {"frame_descriptor", spatial_roi_frame_descriptor_to_json(metadata.frame)},
        {"submission_outcome", metadata.submission_outcome},
        {"output_frame_index", metadata.output_frame_index},
        {"packet_count", metadata.packet_count},
        {"encoded_bytes", metadata.encoded_bytes},
    };
}

bool spatial_roi_frame_metadata_from_json(
    const nlohmann::json& value,
    SpatialRoiFrameMetadata* metadata_out,
    std::string* error_out)
{
    if (!metadata_out) {
        return fail(error_out, "spatial ROI frame metadata destination is null");
    }
    static const std::set<std::string> keys = {
        "schema_id", "schema_version", "frame_descriptor", "submission_outcome",
        "output_frame_index", "packet_count", "encoded_bytes"};
    if (!exact_keys(value, keys, "frame_metadata", error_out)) {
        return false;
    }
    if (!value.at("schema_id").is_string() ||
        value.at("schema_id").get<std::string>() !=
            kSpatialRoiFrameMetadataSchemaId) {
        return fail(error_out, "frame_metadata.schema_id is not spatial ROI v1");
    }
    std::uint64_t schema_version = 0;
    if (!is_nonnegative_u64(value.at("schema_version"), &schema_version) ||
        schema_version != kSpatialRoiFrameMetadataSchemaVersion) {
        return fail(error_out, "frame_metadata.schema_version must be 1");
    }
    SpatialRoiFrameMetadata parsed;
    if (!spatial_roi_frame_descriptor_from_json(
            value.at("frame_descriptor"), &parsed.frame, error_out) ||
        !read_string(value,
                     "submission_outcome",
                     "frame_metadata",
                     &parsed.submission_outcome,
                     error_out) ||
        !read_u64(value,
                  "output_frame_index",
                  "frame_metadata",
                  &parsed.output_frame_index,
                  error_out,
                  true) ||
        !read_u64(value,
                  "packet_count",
                  "frame_metadata",
                  &parsed.packet_count,
                  error_out) ||
        !read_u64(value,
                  "encoded_bytes",
                  "frame_metadata",
                  &parsed.encoded_bytes,
                  error_out) ||
        !validate_spatial_roi_frame_metadata(parsed, error_out)) {
        return false;
    }
    *metadata_out = std::move(parsed);
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiFrameIdentityRegistry::note(const SpatialRoiFrameKey& key,
                                           std::string* error_out)
{
    if (!is_safe_identifier(key.logical_stream_id) ||
        key.recording_frame_id == 0) {
        return fail(error_out,
                    "spatial ROI frame key requires a safe logical_stream_id "
                    "and positive recording_frame_id");
    }
    if (!keys_.insert(key).second) {
        return fail(error_out,
                    "duplicate spatial ROI frame key logical_stream_id=" +
                        key.logical_stream_id +
                        " recording_frame_id=" +
                        std::to_string(key.recording_frame_id));
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiFrameIdentityRegistry::note(
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out)
{
    if (!validate_spatial_roi_frame_descriptor(descriptor, error_out)) {
        return false;
    }
    return note(descriptor.key(), error_out);
}

bool SpatialRoiFrameIdentityRegistry::contains(const SpatialRoiFrameKey& key) const
{
    return keys_.find(key) != keys_.end();
}

}  // namespace orange::spatial_roi
