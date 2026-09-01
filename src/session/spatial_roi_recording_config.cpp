#include "session/spatial_roi_recording_config.h"

#include "gui/spatial_layout/sha256.h"
#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;

constexpr std::uint32_t kMaxIdentifierLength = 64;
constexpr std::uint32_t kMaxLogicalStreamIdLength = 64;
constexpr std::uint32_t kMaxArtifactStemLength = 160;
constexpr std::uint32_t kMaxRecordingIdLength = 512;
constexpr std::size_t kMaxUnixSocketPathLength = 107;
constexpr std::uint64_t kMono8BytesPerPixel = 1;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::string field_path(const std::string& parent, const std::string& child)
{
    return parent.empty() ? child : parent + "." + child;
}

bool exact_keys(const json& value,
                const std::set<std::string>& required,
                const std::set<std::string>& optional,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const std::string& key : required) {
        if (!value.contains(key)) {
            return fail(error_out, field_path(path, key) + " is required");
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (required.count(it.key()) == 0 && optional.count(it.key()) == 0) {
            return fail(error_out,
                        field_path(path, it.key()) + " is not allowed by schema v2");
        }
    }
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
        return fail(error_out, field_path(path, key) + " must be a string");
    }
    *out = value.get<std::string>();
    return true;
}

bool read_bool(const json& object,
               const char* key,
               const std::string& path,
               bool* out,
               std::string* error_out)
{
    const json& value = object.at(key);
    if (!value.is_boolean()) {
        return fail(error_out, field_path(path, key) + " must be a boolean");
    }
    *out = value.get<bool>();
    return true;
}

bool json_nonnegative_u64(const json& value, std::uint64_t* out)
{
    if (value.is_number_unsigned()) {
        *out = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const std::int64_t signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool read_u64(const json& object,
              const char* key,
              const std::string& path,
              std::uint64_t max_value,
              std::uint64_t* out,
              std::string* error_out)
{
    std::uint64_t value = 0;
    if (!json_nonnegative_u64(object.at(key), &value) || value > max_value) {
        return fail(error_out,
                    field_path(path, key) + " must be a nonnegative integer <= " +
                        std::to_string(max_value));
    }
    *out = value;
    return true;
}

bool read_u32(const json& object,
              const char* key,
              const std::string& path,
              std::uint32_t* out,
              std::string* error_out)
{
    std::uint64_t value = 0;
    if (!read_u64(object,
                  key,
                  path,
                  std::numeric_limits<std::uint32_t>::max(),
                  &value,
                  error_out)) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool read_nonnegative_int(const json& object,
                          const char* key,
                          const std::string& path,
                          int* out,
                          std::string* error_out)
{
    std::uint64_t value = 0;
    if (!read_u64(object,
                  key,
                  path,
                  static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
                  &value,
                  error_out)) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool is_safe_identifier(const std::string& value,
                        const std::size_t max_length = kMaxIdentifierLength)
{
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    const auto is_ascii_alnum = [](const unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') ||
               (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [&](const unsigned char ch) {
        return is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

bool is_nonempty_printable_text(const std::string& value)
{
    if (value.empty() || value.size() > 512) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

// Recording IDs are semantic identity, not derived filesystem names.  The
// canonical recording-observation identity contract permits printable text up
// to 512 bytes, while all names derived for sockets/artifacts remain subject
// to the narrower safe-identifier checks below.
bool is_valid_recording_id(const std::string& value)
{
    if (value.empty() || value.size() > kMaxRecordingIdLength ||
        std::isspace(static_cast<unsigned char>(value.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool is_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](const unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool is_safe_relative_path(const std::string& value)
{
    if (value.empty() || value.size() > 512 || value.front() == '/' ||
        value.find('\\') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t slash = value.find('/', start);
        const std::string part = value.substr(
            start,
            slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." ||
            !is_nonempty_printable_text(part)) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool checked_add(const std::uint64_t left,
                 const std::uint64_t right,
                 std::uint64_t* out)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *out = left + right;
    return true;
}

bool checked_multiply(const std::uint64_t left,
                      const std::uint64_t right,
                      std::uint64_t* out)
{
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *out = left * right;
    return true;
}

bool round_up(const std::uint32_t value,
              const std::uint32_t alignment,
              std::uint32_t* out)
{
    if (alignment == 0 || value == 0) {
        return false;
    }
    const std::uint64_t rounded =
        ((static_cast<std::uint64_t>(value) + alignment - 1) / alignment) * alignment;
    if (rounded > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *out = static_cast<std::uint32_t>(rounded);
    return true;
}

bool rects_overlap(const Rect& left, const Rect& right)
{
    const std::uint64_t left_right =
        static_cast<std::uint64_t>(left.x) + left.width;
    const std::uint64_t right_right =
        static_cast<std::uint64_t>(right.x) + right.width;
    const std::uint64_t left_bottom =
        static_cast<std::uint64_t>(left.y) + left.height;
    const std::uint64_t right_bottom =
        static_cast<std::uint64_t>(right.y) + right.height;
    return left.x < right_right && right.x < left_right &&
           left.y < right_bottom && right.y < left_bottom;
}

bool parse_raster(const json& value,
                  Raster* out,
                  const std::string& path,
                  std::string* error_out)
{
    if (!exact_keys(value, {"width", "height"}, {}, path, error_out) ||
        !read_u32(value, "width", path, &out->width, error_out) ||
        !read_u32(value, "height", path, &out->height, error_out)) {
        return false;
    }
    return true;
}

bool parse_rect(const json& value,
                Rect* out,
                const std::string& path,
                std::string* error_out)
{
    if (!exact_keys(value, {"x", "y", "width", "height"}, {}, path, error_out) ||
        !read_u32(value, "x", path, &out->x, error_out) ||
        !read_u32(value, "y", path, &out->y, error_out) ||
        !read_u32(value, "width", path, &out->width, error_out) ||
        !read_u32(value, "height", path, &out->height, error_out)) {
        return false;
    }
    return true;
}

bool parse_authority_ref(const json& value,
                         AuthorityRef* out,
                         const std::string& path,
                         std::string* error_out)
{
    if (!exact_keys(value, {"id", "sha256"}, {}, path, error_out) ||
        !read_string(value, "id", path, &out->id, error_out) ||
        !read_string(value, "sha256", path, &out->sha256, error_out)) {
        return false;
    }
    return true;
}

bool parse_artifact_ref(const json& value,
                        ArtifactRef* out,
                        const std::string& path,
                        std::string* error_out)
{
    if (!exact_keys(value, {"relative_path", "sha256"}, {}, path, error_out) ||
        !read_string(value, "relative_path", path, &out->relative_path, error_out) ||
        !read_string(value, "sha256", path, &out->sha256, error_out)) {
        return false;
    }
    return true;
}

bool parse_roi(const json& value,
               RoiConfig* out,
               const std::string& path,
               std::string* error_out)
{
    if (!exact_keys(value,
                    {"roi_id", "region_id", "required", "content_rect",
                     "logical_stream_id", "artifact_stem"},
                    {"arena_id", "region_mask"},
                    path,
                    error_out) ||
        !read_string(value, "roi_id", path, &out->roi_id, error_out) ||
        !read_string(value, "region_id", path, &out->region_id, error_out) ||
        !read_bool(value, "required", path, &out->required, error_out) ||
        !parse_rect(value.at("content_rect"),
                    &out->content_rect,
                    field_path(path, "content_rect"),
                    error_out) ||
        !read_string(value,
                     "logical_stream_id",
                     path,
                     &out->logical_stream_id,
                     error_out) ||
        !read_string(value, "artifact_stem", path, &out->artifact_stem, error_out)) {
        return false;
    }
    out->has_arena_id = value.contains("arena_id");
    if (out->has_arena_id &&
        !read_string(value, "arena_id", path, &out->arena_id, error_out)) {
        return false;
    }
    out->has_region_mask = value.contains("region_mask");
    if (out->has_region_mask &&
        !parse_artifact_ref(value.at("region_mask"),
                            &out->region_mask,
                            field_path(path, "region_mask"),
                            error_out)) {
        return false;
    }
    return true;
}

bool parse_camera(const json& value,
                  CameraConfig* out,
                  const std::string& path,
                  std::string* error_out)
{
    if (!exact_keys(value,
                    {"camera_id", "camera_serial", "native_raster",
                     "source_frame_rate", "arena_group_id", "layout",
                     "materialization", "registration", "allow_roi_overlap",
                     "rois"},
                    {},
                    path,
                    error_out) ||
        !read_nonnegative_int(value, "camera_id", path, &out->camera_id, error_out) ||
        !read_string(value, "camera_serial", path, &out->camera_serial, error_out) ||
        !parse_raster(value.at("native_raster"),
                      &out->native_raster,
                      field_path(path, "native_raster"),
                      error_out) ||
        !read_u32(value,
                  "source_frame_rate",
                  path,
                  &out->source_frame_rate,
                  error_out) ||
        !read_string(value, "arena_group_id", path, &out->arena_group_id, error_out) ||
        !parse_authority_ref(value.at("layout"),
                             &out->layout,
                             field_path(path, "layout"),
                             error_out) ||
        !parse_authority_ref(value.at("materialization"),
                             &out->materialization,
                             field_path(path, "materialization"),
                             error_out) ||
        !parse_authority_ref(value.at("registration"),
                             &out->registration,
                             field_path(path, "registration"),
                             error_out) ||
        !read_bool(value,
                   "allow_roi_overlap",
                   path,
                   &out->allow_roi_overlap,
                   error_out)) {
        return false;
    }
    if (!value.at("rois").is_array()) {
        return fail(error_out, field_path(path, "rois") + " must be an array");
    }
    out->rois.clear();
    out->rois.reserve(value.at("rois").size());
    for (std::size_t index = 0; index < value.at("rois").size(); ++index) {
        RoiConfig roi;
        if (!parse_roi(value.at("rois").at(index),
                       &roi,
                       field_path(path, "rois") + "[" + std::to_string(index) + "]",
                       error_out)) {
            return false;
        }
        out->rois.push_back(std::move(roi));
    }
    return true;
}

json raster_to_json(const Raster& value)
{
    return {{"width", value.width}, {"height", value.height}};
}

json rect_to_json(const Rect& value)
{
    return {
        {"x", value.x},
        {"y", value.y},
        {"width", value.width},
        {"height", value.height},
    };
}

json authority_to_json(const AuthorityRef& value)
{
    return {{"id", value.id}, {"sha256", value.sha256}};
}

json roi_to_json(const RoiConfig& roi)
{
    json value = {
        {"roi_id", roi.roi_id},
        {"region_id", roi.region_id},
        {"required", roi.required},
        {"content_rect", rect_to_json(roi.content_rect)},
        {"logical_stream_id", roi.logical_stream_id},
        {"artifact_stem", roi.artifact_stem},
    };
    if (roi.has_arena_id) {
        value["arena_id"] = roi.arena_id;
    }
    if (roi.has_region_mask) {
        value["region_mask"] = {
            {"relative_path", roi.region_mask.relative_path},
            {"sha256", roi.region_mask.sha256},
        };
    }
    return value;
}

json camera_to_json(const CameraConfig& camera)
{
    json rois = json::array();
    for (const RoiConfig& roi : camera.rois) {
        rois.push_back(roi_to_json(roi));
    }
    return {
        {"camera_id", camera.camera_id},
        {"camera_serial", camera.camera_serial},
        {"native_raster", raster_to_json(camera.native_raster)},
        {"source_frame_rate", camera.source_frame_rate},
        {"arena_group_id", camera.arena_group_id},
        {"layout", authority_to_json(camera.layout)},
        {"materialization", authority_to_json(camera.materialization)},
        {"registration", authority_to_json(camera.registration)},
        {"allow_roi_overlap", camera.allow_roi_overlap},
        {"rois", std::move(rois)},
    };
}

std::string canonical_sha256(const json& value)
{
    const std::string bytes = value.dump(
        -1, ' ', false, json::error_handler_t::strict);
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

std::string expected_socket_path(const std::string& logical_stream_id)
{
    return "/tmp/orange_external_recorder_" + logical_stream_id + ".sock";
}

json resolved_cameras_to_json(const Config& config)
{
    json cameras = json::object();
    for (const auto& [camera_key, camera] : config.cameras) {
        json rois = json::array();
        for (const RoiConfig& roi : camera.rois) {
            std::uint32_t encoded_width = 0;
            std::uint32_t encoded_height = 0;
            (void)round_up(
                roi.content_rect.width, config.output_alignment_px, &encoded_width);
            (void)round_up(
                roi.content_rect.height, config.output_alignment_px, &encoded_height);
            const std::string artifact_root = "external_spatial_roi_recorder/";
            json resolved_roi = roi_to_json(roi);
            resolved_roi["encoded_raster"] = {
                {"width", encoded_width},
                {"height", encoded_height},
            };
            resolved_roi["content_offset"] = {{"x", 0}, {"y", 0}};
            // Mirror SpatialRoiOutputGeometry::encoded_content_rect so the
            // producer can bind the immutable plan without reconstructing
            // placement from padding fields.
            resolved_roi["encoded_content_rect"] = {
                {"x", 0},
                {"y", 0},
                {"width", roi.content_rect.width},
                {"height", roi.content_rect.height},
            };
            resolved_roi["padding"] = {
                {"right", encoded_width - roi.content_rect.width},
                {"bottom", encoded_height - roi.content_rect.height},
                {"value_mono8", config.padding_value_mono8},
            };
            resolved_roi["no_scaling"] = true;
            resolved_roi["socket_path"] = expected_socket_path(roi.logical_stream_id);
            resolved_roi["expected_artifacts"] = {
                {"video", artifact_root + roi.artifact_stem + ".mp4"},
                {"metadata", artifact_root + roi.artifact_stem + "_meta.csv"},
                {"keyframes", artifact_root + roi.artifact_stem + "_keyframe.json"},
                {"perf", artifact_root + roi.artifact_stem + "_perf.csv"},
                {"summary", artifact_root + roi.artifact_stem + "_summary.json"},
                {"finalization",
                 artifact_root + roi.artifact_stem + ".mp4.finalization.json"},
            };
            rois.push_back(std::move(resolved_roi));
        }
        json resolved_camera = camera_to_json(camera);
        resolved_camera["rois"] = std::move(rois);
        cameras[camera_key] = std::move(resolved_camera);
    }
    return cameras;
}

bool validate_authority(const AuthorityRef& authority,
                        const std::string& path,
                        std::string* error_out)
{
    if (!is_safe_identifier(authority.id)) {
        return fail(error_out, path + ".id is not a safe identifier");
    }
    if (!is_sha256(authority.sha256)) {
        return fail(error_out, path + ".sha256 must be sha256:<64 lowercase hex>");
    }
    return true;
}

bool accumulate_metric(std::uint64_t value,
                       std::uint64_t* total,
                       const std::string& name,
                       std::string* error_out)
{
    std::uint64_t next = 0;
    if (!checked_add(*total, value, &next)) {
        return fail(error_out,
                    name + " accumulation overflowed uint64 capacity");
    }
    *total = next;
    return true;
}

}  // namespace

Config default_config()
{
    return Config{};
}

std::string expected_logical_stream_id(const std::string& camera_serial,
                                       const std::string& roi_id)
{
    return camera_serial + "_spatial_roi_" + roi_id;
}

std::string expected_artifact_stem(const std::string& camera_serial,
                                   const std::string& roi_id)
{
    return "Cam" + camera_serial + "_spatial_roi_" + roi_id;
}

bool parse_config(const nlohmann::json& value,
                  Config* config_out,
                  std::string* error_out)
{
    if (!config_out) {
        return fail(error_out, "spatial ROI config destination is null");
    }
    try {
    const std::string root = "spatial_roi_recording";
    if (!exact_keys(value,
                    {"schema_id", "schema_version", "enabled", "backend", "strict",
                     "source_cadence", "pixel_contract", "buffering",
                     "recording_limits", "admission", "cameras"},
                    {},
                    root,
                    error_out)) {
        return false;
    }
    std::uint64_t schema_version = 0;
    if (!value.at("schema_id").is_string() ||
        value.at("schema_id").get<std::string>() != kConfigSchemaId ||
        !json_nonnegative_u64(value.at("schema_version"), &schema_version) ||
        schema_version != kConfigSchemaVersion) {
        return fail(error_out, "spatial ROI config schema_id/schema_version mismatch");
    }

    Config parsed;
    if (!read_bool(value, "enabled", root, &parsed.enabled, error_out) ||
        !read_string(value, "backend", root, &parsed.backend, error_out) ||
        !read_bool(value, "strict", root, &parsed.strict, error_out) ||
        !read_string(value,
                     "source_cadence",
                     root,
                     &parsed.source_cadence,
                     error_out)) {
        return false;
    }

    const json& pixel = value.at("pixel_contract");
    const std::string pixel_path = field_path(root, "pixel_contract");
    std::uint32_t padding_value = 0;
    if (!exact_keys(pixel,
                    {"source_format", "no_resize", "no_color_conversion",
                     "output_alignment_px", "padding_value_mono8"},
                    {},
                    pixel_path,
                    error_out) ||
        !read_string(pixel,
                     "source_format",
                     pixel_path,
                     &parsed.source_pixel_format,
                     error_out) ||
        !read_bool(pixel, "no_resize", pixel_path, &parsed.no_resize, error_out) ||
        !read_bool(pixel,
                   "no_color_conversion",
                   pixel_path,
                   &parsed.no_color_conversion,
                   error_out) ||
        !read_u32(pixel,
                  "output_alignment_px",
                  pixel_path,
                  &parsed.output_alignment_px,
                  error_out) ||
        !read_u32(pixel,
                  "padding_value_mono8",
                  pixel_path,
                  &padding_value,
                  error_out)) {
        return false;
    }
    if (padding_value != 0) {
        return fail(error_out,
                    pixel_path + ".padding_value_mono8 must be exactly 0 in schema v2");
    }
    parsed.padding_value_mono8 = static_cast<std::uint8_t>(padding_value);

    const json& buffering = value.at("buffering");
    const std::string buffering_path = field_path(root, "buffering");
    if (!exact_keys(buffering,
                    {"pool_frames_per_stream", "queue_frames_per_stream"},
                    {},
                    buffering_path,
                    error_out) ||
        !read_u32(buffering,
                  "pool_frames_per_stream",
                  buffering_path,
                  &parsed.buffering.pool_frames_per_stream,
                  error_out) ||
        !read_u32(buffering,
                  "queue_frames_per_stream",
                  buffering_path,
                  &parsed.buffering.queue_frames_per_stream,
                  error_out)) {
        return false;
    }

    const json& recording_limits = value.at("recording_limits");
    const std::string recording_limits_path =
        field_path(root, "recording_limits");
    if (!exact_keys(recording_limits,
                    {"max_frames_per_stream", "max_media_bytes_per_stream",
                     "max_evidence_bytes_per_stream"},
                    {},
                    recording_limits_path,
                    error_out) ||
        !read_u64(recording_limits,
                  "max_frames_per_stream",
                  recording_limits_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.recording_limits.max_frames_per_stream,
                  error_out) ||
        !read_u64(recording_limits,
                  "max_media_bytes_per_stream",
                  recording_limits_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.recording_limits.max_media_bytes_per_stream,
                  error_out) ||
        !read_u64(recording_limits,
                  "max_evidence_bytes_per_stream",
                  recording_limits_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.recording_limits.max_evidence_bytes_per_stream,
                  error_out)) {
        return false;
    }

    const json& admission = value.at("admission");
    const std::string admission_path = field_path(root, "admission");
    if (!exact_keys(admission,
                    {"max_rois_per_camera", "max_total_rois",
                     "max_total_pixel_rate", "max_total_encoder_streams",
                     "max_total_pool_bytes", "max_total_queue_frames",
                     "max_total_media_bytes", "max_total_evidence_bytes"},
                    {},
                    admission_path,
                    error_out) ||
        !read_u32(admission,
                  "max_rois_per_camera",
                  admission_path,
                  &parsed.admission.max_rois_per_camera,
                  error_out) ||
        !read_u32(admission,
                  "max_total_rois",
                  admission_path,
                  &parsed.admission.max_total_rois,
                  error_out) ||
        !read_u64(admission,
                  "max_total_pixel_rate",
                  admission_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.admission.max_total_pixel_rate,
                  error_out) ||
        !read_u32(admission,
                  "max_total_encoder_streams",
                  admission_path,
                  &parsed.admission.max_total_encoder_streams,
                  error_out) ||
        !read_u64(admission,
                  "max_total_pool_bytes",
                  admission_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.admission.max_total_pool_bytes,
                  error_out) ||
        !read_u64(admission,
                  "max_total_queue_frames",
                  admission_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.admission.max_total_queue_frames,
                  error_out) ||
        !read_u64(admission,
                  "max_total_media_bytes",
                  admission_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.admission.max_total_media_bytes,
                  error_out) ||
        !read_u64(admission,
                  "max_total_evidence_bytes",
                  admission_path,
                  std::numeric_limits<std::uint64_t>::max(),
                  &parsed.admission.max_total_evidence_bytes,
                  error_out)) {
        return false;
    }

    const json& cameras = value.at("cameras");
    if (!cameras.is_object()) {
        return fail(error_out, field_path(root, "cameras") + " must be an object");
    }
    for (auto it = cameras.begin(); it != cameras.end(); ++it) {
        CameraConfig camera;
        const std::string camera_path =
            field_path(root, "cameras") + "." + it.key();
        if (!parse_camera(it.value(), &camera, camera_path, error_out)) {
            return false;
        }
        parsed.cameras[it.key()] = std::move(camera);
    }

    if (!validate_config(parsed, nullptr, error_out)) {
        return false;
    }
    *config_out = std::move(parsed);
    return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI config serialization failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI config serialization failed: unknown exception");
    }
}

nlohmann::json config_to_json(const Config& config)
{
    json cameras = json::object();
    for (const auto& [camera_key, camera] : config.cameras) {
        cameras[camera_key] = camera_to_json(camera);
    }
    return {
        {"schema_id", kConfigSchemaId},
        {"schema_version", kConfigSchemaVersion},
        {"enabled", config.enabled},
        {"backend", config.backend},
        {"strict", config.strict},
        {"source_cadence", config.source_cadence},
        {"pixel_contract",
         {
             {"source_format", config.source_pixel_format},
             {"no_resize", config.no_resize},
             {"no_color_conversion", config.no_color_conversion},
             {"output_alignment_px", config.output_alignment_px},
             {"padding_value_mono8", config.padding_value_mono8},
         }},
        {"buffering",
         {
             {"pool_frames_per_stream", config.buffering.pool_frames_per_stream},
             {"queue_frames_per_stream", config.buffering.queue_frames_per_stream},
         }},
        {"recording_limits",
         {
             {"max_frames_per_stream",
              config.recording_limits.max_frames_per_stream},
             {"max_media_bytes_per_stream",
              config.recording_limits.max_media_bytes_per_stream},
             {"max_evidence_bytes_per_stream",
              config.recording_limits.max_evidence_bytes_per_stream},
         }},
        {"admission",
         {
             {"max_rois_per_camera", config.admission.max_rois_per_camera},
             {"max_total_rois", config.admission.max_total_rois},
             {"max_total_pixel_rate", config.admission.max_total_pixel_rate},
             {"max_total_encoder_streams",
              config.admission.max_total_encoder_streams},
             {"max_total_pool_bytes", config.admission.max_total_pool_bytes},
             {"max_total_queue_frames", config.admission.max_total_queue_frames},
             {"max_total_media_bytes", config.admission.max_total_media_bytes},
             {"max_total_evidence_bytes",
              config.admission.max_total_evidence_bytes},
         }},
        {"cameras", std::move(cameras)},
    };
}

bool validate_config(const Config& config,
                     AdmissionUsage* usage_out,
                     std::string* error_out)
{
    if (!config.strict) {
        return fail(error_out,
                    "spatial ROI schema v2 requires strict=true; best-effort admission is unsupported");
    }
    if (config.backend != kBackend) {
        return fail(error_out,
                    "spatial ROI backend must be independent_lossless_external_ipc");
    }
    if (config.source_cadence != kSourceCadence) {
        return fail(error_out,
                    "spatial ROI source_cadence must be every_recording_frame");
    }
    if (config.source_pixel_format != kSourcePixelFormat) {
        return fail(error_out, "spatial ROI source_format must be mono8");
    }
    if (!config.no_resize || !config.no_color_conversion) {
        return fail(error_out,
                    "spatial ROI v2 requires no_resize and no_color_conversion");
    }
    if (config.padding_value_mono8 != 0) {
        return fail(error_out,
                    "spatial ROI schema v2 requires padding_value_mono8=0");
    }
    static constexpr std::uint32_t kAllowedAlignments[] = {1, 2, 4, 8, 16};
    if (std::find(std::begin(kAllowedAlignments),
                  std::end(kAllowedAlignments),
                  config.output_alignment_px) == std::end(kAllowedAlignments)) {
        return fail(error_out,
                    "spatial ROI output_alignment_px must be one of 1,2,4,8,16");
    }
    if (config.buffering.pool_frames_per_stream == 0 ||
        config.buffering.queue_frames_per_stream == 0) {
        return fail(error_out,
                    "spatial ROI pool and queue frames per stream must be positive");
    }
    if (config.recording_limits.max_frames_per_stream == 0 ||
        config.recording_limits.max_media_bytes_per_stream == 0 ||
        config.recording_limits.max_evidence_bytes_per_stream == 0) {
        return fail(error_out,
                    "spatial ROI recording limits must be positive");
    }
    if (config.recording_limits.max_frames_per_stream >
            kMaxFramesPerStream ||
        config.recording_limits.max_media_bytes_per_stream >
            kMaxMediaBytesPerStream ||
        config.recording_limits.max_evidence_bytes_per_stream >
            kMaxEvidenceBytesPerStream) {
        return fail(error_out,
                    "spatial ROI recording limits exceed recorder implementation ceilings");
    }
    if (config.admission.max_rois_per_camera == 0 ||
        config.admission.max_total_rois == 0 ||
        config.admission.max_total_pixel_rate == 0 ||
        config.admission.max_total_encoder_streams == 0 ||
        config.admission.max_total_pool_bytes == 0 ||
        config.admission.max_total_queue_frames == 0 ||
        config.admission.max_total_media_bytes == 0 ||
        config.admission.max_total_evidence_bytes == 0) {
        return fail(error_out, "spatial ROI admission limits must be positive");
    }
    if (config.enabled && config.cameras.empty()) {
        return fail(error_out,
                    "enabled spatial ROI recording requires at least one camera");
    }

    AdmissionUsage usage;
    if (config.cameras.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error_out, "spatial ROI camera count exceeds uint32 capacity");
    }
    usage.camera_count = static_cast<std::uint32_t>(config.cameras.size());
    std::set<int> camera_ids;
    std::set<std::string> global_stream_ids;
    std::set<std::string> global_artifact_stems;

    for (const auto& [camera_key, camera] : config.cameras) {
        const std::string path = "cameras." + camera_key;
        if (!is_safe_identifier(camera_key) ||
            !is_safe_identifier(camera.camera_serial) ||
            camera_key != camera.camera_serial) {
            return fail(error_out,
                        path + " key and camera_serial must be the same safe identifier");
        }
        if (camera.camera_id < 0 || !camera_ids.insert(camera.camera_id).second) {
            return fail(error_out, path + ".camera_id must be nonnegative and unique");
        }
        if (camera.native_raster.width == 0 || camera.native_raster.height == 0) {
            return fail(error_out, path + ".native_raster dimensions must be positive");
        }
        if (camera.source_frame_rate == 0) {
            return fail(error_out, path + ".source_frame_rate must be positive");
        }
        if (!is_safe_identifier(camera.arena_group_id)) {
            return fail(error_out, path + ".arena_group_id is not a safe identifier");
        }
        if (!validate_authority(camera.layout, path + ".layout", error_out) ||
            !validate_authority(
                camera.materialization, path + ".materialization", error_out) ||
            !validate_authority(
                camera.registration, path + ".registration", error_out)) {
            return false;
        }
        if (config.enabled && camera.rois.empty()) {
            return fail(error_out, path + " requires at least one ROI when enabled");
        }
        if (camera.rois.size() > config.admission.max_rois_per_camera) {
            return fail(error_out,
                        path + " ROI count exceeds admission.max_rois_per_camera");
        }

        std::set<std::string> roi_ids;
        for (std::size_t index = 0; index < camera.rois.size(); ++index) {
            const RoiConfig& roi = camera.rois[index];
            const std::string roi_path =
                path + ".rois[" + std::to_string(index) + "]";
            if (!is_safe_identifier(roi.roi_id) ||
                !roi_ids.insert(roi.roi_id).second) {
                return fail(error_out,
                            roi_path + ".roi_id must be safe and unique per camera");
            }
            if (!roi.required) {
                return fail(error_out,
                            roi_path + ".required=false is unsupported; schema v2 requires every admitted ROI");
            }
            if (!is_safe_identifier(roi.region_id)) {
                return fail(error_out, roi_path + ".region_id is not a safe identifier");
            }
            if (roi.has_arena_id && !is_safe_identifier(roi.arena_id)) {
                return fail(error_out, roi_path + ".arena_id is not a safe identifier");
            }
            if (roi.content_rect.width == 0 || roi.content_rect.height == 0) {
                return fail(error_out,
                            roi_path + ".content_rect dimensions must be positive");
            }
            const std::uint64_t right =
                static_cast<std::uint64_t>(roi.content_rect.x) +
                roi.content_rect.width;
            const std::uint64_t bottom =
                static_cast<std::uint64_t>(roi.content_rect.y) +
                roi.content_rect.height;
            if (right > camera.native_raster.width ||
                bottom > camera.native_raster.height) {
                return fail(error_out,
                            roi_path + ".content_rect lies outside native_raster");
            }
            if (roi.has_region_mask &&
                (!is_safe_relative_path(roi.region_mask.relative_path) ||
                 !is_sha256(roi.region_mask.sha256))) {
                return fail(error_out,
                            roi_path +
                                ".region_mask requires a safe relative_path and sha256 digest");
            }

            const std::string logical_stream_id =
                expected_logical_stream_id(camera.camera_serial, roi.roi_id);
            const std::string artifact_stem =
                expected_artifact_stem(camera.camera_serial, roi.roi_id);
            if (roi.logical_stream_id != logical_stream_id ||
                !is_safe_identifier(
                    roi.logical_stream_id, kMaxLogicalStreamIdLength)) {
                return fail(error_out,
                            roi_path + ".logical_stream_id must equal " +
                                logical_stream_id + " and fit the v2 limit");
            }
            if (roi.artifact_stem != artifact_stem ||
                !is_safe_identifier(roi.artifact_stem, kMaxArtifactStemLength)) {
                return fail(error_out,
                            roi_path + ".artifact_stem must equal " + artifact_stem);
            }
            if (expected_socket_path(roi.logical_stream_id).size() >
                kMaxUnixSocketPathLength) {
                return fail(error_out,
                            roi_path + ".logical_stream_id makes the socket path too long");
            }
            if (!global_stream_ids.insert(roi.logical_stream_id).second ||
                !global_artifact_stems.insert(roi.artifact_stem).second) {
                return fail(error_out,
                            roi_path + " stream ID or artifact stem collides globally");
            }

            for (std::size_t prior = 0; prior < index; ++prior) {
                if (!camera.allow_roi_overlap &&
                    rects_overlap(camera.rois[prior].content_rect, roi.content_rect)) {
                    return fail(error_out,
                                roi_path + " overlaps an earlier ROI but overlap is disabled");
                }
            }

            std::uint32_t encoded_width = 0;
            std::uint32_t encoded_height = 0;
            if (!round_up(roi.content_rect.width,
                          config.output_alignment_px,
                          &encoded_width) ||
                !round_up(roi.content_rect.height,
                          config.output_alignment_px,
                          &encoded_height)) {
                return fail(error_out,
                            roi_path + " cannot be padded to the requested alignment");
            }
            std::uint64_t content_pixels = 0;
            std::uint64_t encoded_pixels = 0;
            std::uint64_t content_rate = 0;
            std::uint64_t encoded_rate = 0;
            std::uint64_t pool_bytes = 0;
            if (!checked_multiply(roi.content_rect.width,
                                  roi.content_rect.height,
                                  &content_pixels) ||
                !checked_multiply(encoded_width, encoded_height, &encoded_pixels) ||
                !checked_multiply(
                    content_pixels, camera.source_frame_rate, &content_rate) ||
                !checked_multiply(
                    encoded_pixels, camera.source_frame_rate, &encoded_rate) ||
                !checked_multiply(encoded_pixels,
                                  config.buffering.pool_frames_per_stream,
                                  &pool_bytes) ||
                !checked_multiply(pool_bytes, kMono8BytesPerPixel, &pool_bytes)) {
                return fail(error_out, roi_path + " resource calculation overflowed");
            }
            if (!accumulate_metric(content_rate,
                                   &usage.content_pixel_rate,
                                   "content pixel rate",
                                   error_out) ||
                !accumulate_metric(encoded_rate,
                                   &usage.encoded_pixel_rate,
                                   "encoded pixel rate",
                                   error_out) ||
                !accumulate_metric(
                    pool_bytes, &usage.pool_bytes, "pool bytes", error_out) ||
                !accumulate_metric(config.buffering.queue_frames_per_stream,
                                   &usage.queue_frames,
                                   "queue frames",
                                   error_out) ||
                !accumulate_metric(
                    config.recording_limits.max_media_bytes_per_stream,
                    &usage.media_bytes,
                    "media bytes",
                    error_out) ||
                !accumulate_metric(
                    config.recording_limits.max_evidence_bytes_per_stream,
                    &usage.evidence_bytes,
                    "evidence bytes",
                    error_out)) {
                return false;
            }
            if (usage.roi_count == std::numeric_limits<std::uint32_t>::max()) {
                return fail(error_out, "spatial ROI count exceeds uint32 capacity");
            }
            ++usage.roi_count;
            ++usage.encoder_stream_count;
        }
    }

    if (usage.roi_count > config.admission.max_total_rois) {
        return fail(error_out, "spatial ROI count exceeds admission.max_total_rois");
    }
    if (usage.encoder_stream_count > config.admission.max_total_encoder_streams) {
        return fail(error_out,
                    "spatial ROI encoder count exceeds admission.max_total_encoder_streams");
    }
    if (usage.encoded_pixel_rate > config.admission.max_total_pixel_rate) {
        return fail(error_out,
                    "spatial ROI padded pixel rate exceeds admission.max_total_pixel_rate");
    }
    if (usage.pool_bytes > config.admission.max_total_pool_bytes) {
        return fail(error_out,
                    "spatial ROI pool bytes exceed admission.max_total_pool_bytes");
    }
    if (usage.queue_frames > config.admission.max_total_queue_frames) {
        return fail(error_out,
                    "spatial ROI queue frames exceed admission.max_total_queue_frames");
    }
    if (usage.media_bytes > config.admission.max_total_media_bytes) {
        return fail(error_out,
                    "spatial ROI media bytes exceed admission.max_total_media_bytes");
    }
    if (usage.evidence_bytes > config.admission.max_total_evidence_bytes) {
        return fail(
            error_out,
            "spatial ROI evidence bytes exceed admission.max_total_evidence_bytes");
    }
    if (usage_out) {
        *usage_out = usage;
    }
    return true;
}

nlohmann::json admission_usage_to_json(const AdmissionUsage& usage)
{
    return {
        {"camera_count", usage.camera_count},
        {"roi_count", usage.roi_count},
        {"encoder_stream_count", usage.encoder_stream_count},
        {"content_pixel_rate", usage.content_pixel_rate},
        {"encoded_pixel_rate", usage.encoded_pixel_rate},
        {"pool_bytes", usage.pool_bytes},
        {"queue_frames", usage.queue_frames},
        {"media_bytes", usage.media_bytes},
        {"evidence_bytes", usage.evidence_bytes},
    };
}

bool build_plan(const Config& config,
                const PlanContext& context,
                nlohmann::json* plan_out,
                AdmissionUsage* usage_out,
                std::string* error_out)
{
    if (!plan_out) {
        return fail(error_out, "spatial ROI plan destination is null");
    }
    try {
    if (!config.enabled) {
        return fail(error_out,
                    "spatial ROI recording plan requires enabled=true");
    }
    if (!is_valid_recording_id(context.recording_id) ||
        !is_sha256(context.recording_identity_token) ||
        !is_nonempty_printable_text(context.generated_at_utc) ||
        !is_safe_identifier(context.producer_generation)) {
        return fail(error_out,
                    "spatial ROI plan context has invalid recording, token, time, or generation");
    }
    const std::string expected_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    if (context.recording_identity_token != expected_identity_token) {
        return fail(error_out,
                    "spatial ROI recording_identity_token does not bind to recording_id");
    }
    AdmissionUsage usage;
    if (!validate_config(config, &usage, error_out)) {
        return false;
    }
    const json payload = {
        {"schema_id", kPlanSchemaId},
        {"schema_version", kPlanSchemaVersion},
        {"plan_scope", kPlanScope},
        {"recording_id", context.recording_id},
        {"recording_identity_token", context.recording_identity_token},
        {"generated_at_utc", context.generated_at_utc},
        {"producer_generation", context.producer_generation},
        {"configuration", config_to_json(config)},
        {"admission_usage", admission_usage_to_json(usage)},
        {"resolved_cameras", resolved_cameras_to_json(config)},
    };
    *plan_out = {
        {"schema_id", kPlanSchemaId},
        {"schema_version", kPlanSchemaVersion},
        {"canonicalization", kCanonicalization},
        {"plan_sha256", canonical_sha256(payload)},
        {"plan", payload},
    };
    if (usage_out) {
        *usage_out = usage;
    }
    return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI plan serialization failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI plan serialization failed: unknown exception");
    }
}

bool verify_plan(const nlohmann::json& plan, std::string* error_out)
{
    try {
    std::uint64_t envelope_schema_version = 0;
    if (!exact_keys(plan,
                    {"schema_id", "schema_version", "canonicalization",
                     "plan_sha256", "plan"},
                    {},
                    "spatial_roi_plan",
                    error_out) ||
        !plan.at("schema_id").is_string() ||
        plan.at("schema_id").get<std::string>() != kPlanSchemaId ||
        !json_nonnegative_u64(
            plan.at("schema_version"), &envelope_schema_version) ||
        envelope_schema_version != kPlanSchemaVersion ||
        !plan.at("canonicalization").is_string() ||
        plan.at("canonicalization").get<std::string>() != kCanonicalization ||
        !plan.at("plan_sha256").is_string() ||
        !is_sha256(plan.at("plan_sha256").get<std::string>())) {
        return fail(error_out, "spatial ROI plan envelope is invalid");
    }

    const json& payload = plan.at("plan");
    std::uint64_t payload_schema_version = 0;
    if (!exact_keys(payload,
                    {"schema_id", "schema_version", "plan_scope", "recording_id",
                     "recording_identity_token", "generated_at_utc",
                     "producer_generation", "configuration", "admission_usage",
                     "resolved_cameras"},
                    {},
                    "spatial_roi_plan.plan",
                    error_out) ||
        !payload.at("schema_id").is_string() ||
        payload.at("schema_id").get<std::string>() != kPlanSchemaId ||
        !json_nonnegative_u64(
            payload.at("schema_version"), &payload_schema_version) ||
        payload_schema_version != kPlanSchemaVersion ||
        !payload.at("plan_scope").is_string() ||
        payload.at("plan_scope").get<std::string>() != kPlanScope ||
        !payload.at("recording_id").is_string() ||
        !is_valid_recording_id(payload.at("recording_id").get<std::string>()) ||
        !payload.at("recording_identity_token").is_string() ||
        !is_sha256(payload.at("recording_identity_token").get<std::string>()) ||
        !payload.at("generated_at_utc").is_string() ||
        !is_nonempty_printable_text(payload.at("generated_at_utc").get<std::string>()) ||
        !payload.at("producer_generation").is_string() ||
        !is_safe_identifier(payload.at("producer_generation").get<std::string>())) {
        return fail(error_out, "spatial ROI plan payload is invalid");
    }
    if (payload.at("recording_identity_token").get<std::string>() !=
        orange::shaman_v2_recording_identity::token_for_recording_id(
            payload.at("recording_id").get<std::string>())) {
        return fail(error_out,
                    "spatial ROI plan recording_identity_token does not bind to recording_id");
    }

    Config config;
    AdmissionUsage usage;
    if (!parse_config(payload.at("configuration"), &config, error_out) ||
        !validate_config(config, &usage, error_out)) {
        return false;
    }
    if (!config.enabled) {
        return fail(error_out,
                    "spatial ROI recording plan requires enabled=true");
    }
    if (payload.at("configuration") != config_to_json(config) ||
        payload.at("admission_usage") != admission_usage_to_json(usage) ||
        payload.at("resolved_cameras") != resolved_cameras_to_json(config)) {
        return fail(error_out,
                    "spatial ROI plan normalized configuration or resolution mismatch");
    }
    if (plan.at("plan_sha256").get<std::string>() != canonical_sha256(payload)) {
        return fail(error_out, "spatial ROI plan_sha256 mismatch");
    }
    return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI plan serialization failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI plan serialization failed: unknown exception");
    }
}

bool parse_verified_plan(const nlohmann::json& plan,
                         SpatialRoiRecordingPlan* plan_out,
                         std::string* error_out)
{
    if (!plan_out) {
        return fail(error_out, "spatial ROI recording plan destination is null");
    }
    try {
    if (!verify_plan(plan, error_out)) {
        return false;
    }

    const json& payload = plan.at("plan");
    Config config;
    if (!parse_config(payload.at("configuration"), &config, error_out)) {
        return false;
    }

    SpatialRoiRecordingPlan parsed;
    parsed.plan_sha256 = plan.at("plan_sha256").get<std::string>();
    parsed.recording_id = payload.at("recording_id").get<std::string>();
    parsed.recording_identity_token =
        payload.at("recording_identity_token").get<std::string>();
    parsed.producer_generation =
        payload.at("producer_generation").get<std::string>();
    parsed.pool_frames_per_stream = config.buffering.pool_frames_per_stream;
    parsed.recording_limits = config.recording_limits;
    const json& admission_usage = payload.at("admission_usage");
    parsed.admission_usage.camera_count =
        admission_usage.at("camera_count").get<std::uint32_t>();
    parsed.admission_usage.roi_count =
        admission_usage.at("roi_count").get<std::uint32_t>();
    parsed.admission_usage.encoder_stream_count =
        admission_usage.at("encoder_stream_count").get<std::uint32_t>();
    parsed.admission_usage.content_pixel_rate =
        admission_usage.at("content_pixel_rate").get<std::uint64_t>();
    parsed.admission_usage.encoded_pixel_rate =
        admission_usage.at("encoded_pixel_rate").get<std::uint64_t>();
    parsed.admission_usage.pool_bytes =
        admission_usage.at("pool_bytes").get<std::uint64_t>();
    parsed.admission_usage.queue_frames =
        admission_usage.at("queue_frames").get<std::uint64_t>();
    parsed.admission_usage.media_bytes =
        admission_usage.at("media_bytes").get<std::uint64_t>();
    parsed.admission_usage.evidence_bytes =
        admission_usage.at("evidence_bytes").get<std::uint64_t>();

    const json& resolved_cameras = payload.at("resolved_cameras");
    for (auto camera_it = resolved_cameras.begin();
         camera_it != resolved_cameras.end();
         ++camera_it) {
        const json& camera = camera_it.value();
        SpatialRoiPlanCameraDescriptor parsed_camera;
        parsed_camera.camera_id = camera.at("camera_id").get<int>();
        parsed_camera.camera_serial =
            camera.at("camera_serial").get<std::string>();
        parsed_camera.native_raster = {
            camera.at("native_raster").at("width").get<std::uint32_t>(),
            camera.at("native_raster").at("height").get<std::uint32_t>(),
        };
        parsed_camera.arena_group_id =
            camera.at("arena_group_id").get<std::string>();

        const json& rois = camera.at("rois");
        parsed_camera.rois.reserve(rois.size());
        for (const json& roi : rois) {
            SpatialRoiPlanRoiDescriptor parsed_roi;
            parsed_roi.roi_id = roi.at("roi_id").get<std::string>();
            parsed_roi.region_id = roi.at("region_id").get<std::string>();
            parsed_roi.has_arena_id = roi.contains("arena_id");
            if (parsed_roi.has_arena_id) {
                parsed_roi.arena_id = roi.at("arena_id").get<std::string>();
            }
            parsed_roi.arena_group_id = parsed_camera.arena_group_id;
            parsed_roi.logical_stream_id =
                roi.at("logical_stream_id").get<std::string>();
            const json& source_rect = roi.at("content_rect");
            parsed_roi.source_rect = {
                source_rect.at("x").get<std::uint32_t>(),
                source_rect.at("y").get<std::uint32_t>(),
                source_rect.at("width").get<std::uint32_t>(),
                source_rect.at("height").get<std::uint32_t>(),
            };
            const json& encoded_raster = roi.at("encoded_raster");
            parsed_roi.encoded_raster = {
                encoded_raster.at("width").get<std::uint32_t>(),
                encoded_raster.at("height").get<std::uint32_t>(),
            };
            const json& encoded_content_rect =
                roi.at("encoded_content_rect");
            parsed_roi.encoded_content_rect = {
                encoded_content_rect.at("x").get<std::uint32_t>(),
                encoded_content_rect.at("y").get<std::uint32_t>(),
                encoded_content_rect.at("width").get<std::uint32_t>(),
                encoded_content_rect.at("height").get<std::uint32_t>(),
            };

            std::uint64_t pixels = 0;
            if (!checked_multiply(parsed_roi.encoded_raster.width,
                                  parsed_roi.encoded_raster.height,
                                  &pixels) ||
                !checked_multiply(pixels,
                                  parsed.pool_frames_per_stream,
                                  &parsed_roi.pool_bytes) ||
                !checked_add(parsed_camera.pool_bytes,
                             parsed_roi.pool_bytes,
                             &parsed_camera.pool_bytes)) {
                return fail(error_out,
                            "spatial ROI producer pool byte calculation overflowed");
            }
            parsed_camera.rois.push_back(std::move(parsed_roi));
        }
        parsed.cameras.emplace(camera_it.key(), std::move(parsed_camera));
    }

    std::uint64_t producer_pool_bytes = 0;
    for (const auto& [camera_serial, camera] : parsed.cameras) {
        (void)camera_serial;
        if (!checked_add(producer_pool_bytes,
                         camera.pool_bytes,
                         &producer_pool_bytes)) {
            return fail(error_out,
                        "spatial ROI producer pool byte calculation overflowed");
        }
    }
    if (producer_pool_bytes != parsed.admission_usage.pool_bytes) {
        return fail(error_out,
                    "spatial ROI plan producer pool bytes do not match admission_usage");
    }

    *plan_out = std::move(parsed);
    if (error_out) {
        error_out->clear();
    }
    return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI plan serialization failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI plan serialization failed: unknown exception");
    }
}

}  // namespace orange::session::spatial_roi
