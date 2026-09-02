#include "spatial_roi_registered_scene_context.h"

#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;
namespace checksum = orange::gui::spatial_layout::checksum;

constexpr std::uint64_t kMaxContextBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxTextBytes = 512;

constexpr std::array<std::string_view, 15> kDescriptorKeys = {
    "schema_id", "schema_version", "canonicalization", "capture_role",
    "status", "failure_reason", "recording", "camera", "source_frame",
    "native_raster", "coordinate_space", "pixel_format", "geometry_binding",
    "capture_invariants", "artifact"};
constexpr std::array<std::string_view, 4> kRecordingKeys = {
    "recording_id", "session_id", "recording_identity_token",
    "producer_generation"};
constexpr std::array<std::string_view, 5> kCameraKeys = {
    "camera_id", "camera_serial", "source_camera_stream_id", "stream_epoch_id",
    "camera_configuration_sha256"};
constexpr std::array<std::string_view, 6> kSourceFrameKeys = {
    "source_frame_id", "local_frame_id", "camera_frame_id",
    "recording_frame_id", "camera_timestamp_ns", "timestamp_sys_ns"};
constexpr std::array<std::string_view, 3> kRasterKeys = {
    "width", "height", "stride_bytes"};
constexpr std::array<std::string_view, 3> kGeometryKeys = {
    "layout", "materialization", "registration"};
constexpr std::array<std::string_view, 2> kReferenceKeys = {"id", "sha256"};
constexpr std::array<std::string_view, 7> kInvariantKeys = {
    "daily_registration_accepted", "registration_authority_status",
    "dish_setup_complete", "subject_presence", "nir_illumination_fixed",
    "camera_configuration_fixed", "rig_fixed"};
constexpr std::array<std::string_view, 3> kArtifactKeys = {
    "relative_path", "size_bytes", "sha256"};

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

void clear_error(std::string* error_out)
{
    if (error_out != nullptr) {
        error_out->clear();
    }
}

template <std::size_t N>
bool exact_keys(const json& value,
                const std::array<std::string_view, N>& expected,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const std::string_view key : expected) {
        if (!value.contains(std::string(key))) {
            return fail(error_out, path + "." + std::string(key) +
                                  " is required");
        }
    }
    if (value.size() != expected.size()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            bool allowed = false;
            for (const std::string_view key : expected) {
                if (it.key() == key) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return fail(error_out, path + "." + it.key() +
                                      " is not allowed by the registered "
                                      "scene context schema");
            }
        }
        return fail(error_out, path + " has an unexpected key count");
    }
    return true;
}

bool safe_text(const std::string& value)
{
    if (value.empty() || value.size() > kMaxTextBytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool safe_identifier(const std::string& value)
{
    if (!safe_text(value) ||
        !std::isalnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](const unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

bool safe_flat_relative_path(const std::string& value)
{
    if (!safe_text(value) || value == "." || value == ".." ||
        value.front() == '/' || value.front() == '\\' ||
        value.find('/') != std::string::npos ||
        value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos) {
        return false;
    }
    const std::filesystem::path path(value);
    return !path.is_absolute() && !path.has_root_name() &&
        !path.has_root_directory() && path.filename() == path &&
        path.lexically_normal().generic_string() == value;
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](const unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool read_string(const json& object,
                 const char* key,
                 const std::string& path,
                 std::string* output,
                 std::string* error_out,
                 const bool identifier = false)
{
    if (!object.at(key).is_string()) {
        return fail(error_out, path + "." + key + " must be a string");
    }
    const std::string value = object.at(key).get<std::string>();
    if (!(identifier ? safe_identifier(value) : safe_text(value))) {
        return fail(error_out, path + "." + key + " contains unsafe text");
    }
    *output = value;
    return true;
}

bool read_sha256(const json& object,
                 const char* key,
                 const std::string& path,
                 std::string* output,
                 std::string* error_out)
{
    if (!object.at(key).is_string() ||
        !canonical_sha256(object.at(key).get<std::string>())) {
        return fail(error_out, path + "." + key +
                              " must be canonical sha256:<lowercase hex>");
    }
    *output = object.at(key).get<std::string>();
    return true;
}

bool read_u64(const json& object,
              const char* key,
              const std::string& path,
              std::uint64_t* output,
              std::string* error_out,
              const bool positive = false)
{
    const json& value = object.at(key);
    if (value.is_boolean() ||
        (!value.is_number_unsigned() && !value.is_number_integer())) {
        return fail(error_out, path + "." + key +
                              (positive ? " must be a positive integer"
                                        : " must be a non-negative integer"));
    }
    if (value.is_number_unsigned()) {
        *output = value.get<std::uint64_t>();
    } else {
        const std::int64_t parsed = value.get<std::int64_t>();
        if (parsed < 0) {
            return fail(error_out, path + "." + key +
                                  " must not be negative");
        }
        *output = static_cast<std::uint64_t>(parsed);
    }
    if (positive && *output == 0) {
        return fail(error_out, path + "." + key + " must be positive");
    }
    return true;
}

bool read_u32(const json& object,
              const char* key,
              const std::string& path,
              std::uint32_t* output,
              std::string* error_out,
              const bool positive = false)
{
    std::uint64_t value = 0;
    if (!read_u64(object, key, path, &value, error_out, positive) ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        if (error_out != nullptr && error_out->empty()) {
            *error_out = path + "." + key + " exceeds uint32 range";
        }
        return false;
    }
    *output = static_cast<std::uint32_t>(value);
    return true;
}

bool read_int(const json& object,
              const char* key,
              const std::string& path,
              int* output,
              std::string* error_out,
              const bool positive = false)
{
    const json& value = object.at(key);
    if (value.is_boolean() ||
        (!value.is_number_integer() && !value.is_number_unsigned())) {
        return fail(error_out, path + "." + key + " must be an integer");
    }
    std::int64_t parsed = 0;
    if (value.is_number_unsigned()) {
        const std::uint64_t unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(
                                 std::numeric_limits<int>::max())) {
            return fail(error_out, path + "." + key + " exceeds int range");
        }
        parsed = static_cast<std::int64_t>(unsigned_value);
    } else {
        parsed = value.get<std::int64_t>();
    }
    if ((positive ? parsed <= 0 : parsed < 0) ||
        parsed > std::numeric_limits<int>::max()) {
        return fail(error_out, path + "." + key +
                              (positive ? " must be positive"
                                        : " must be non-negative"));
    }
    *output = static_cast<int>(parsed);
    return true;
}

bool is_schema_version(const json& value, const int expected)
{
    if (value.is_boolean() ||
        (!value.is_number_integer() && !value.is_number_unsigned())) {
        return false;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() == static_cast<std::uint64_t>(expected);
    }
    return value.get<std::int64_t>() == static_cast<std::int64_t>(expected);
}

bool read_bool(const json& object,
               const char* key,
               const std::string& path,
               bool* output,
               std::string* error_out)
{
    if (!object.at(key).is_boolean()) {
        return fail(error_out, path + "." + key + " must be a boolean");
    }
    *output = object.at(key).get<bool>();
    return true;
}

bool validate_reference(const RegisteredSceneContextReference& reference,
                        const std::string& name,
                        std::string* error_out)
{
    if (!safe_identifier(reference.id)) {
        return fail(error_out, name + ".id is not a safe identifier");
    }
    if (!canonical_sha256(reference.sha256)) {
        return fail(error_out, name + ".sha256 is not canonical");
    }
    return true;
}

bool validate_metadata(const RegisteredSceneContextDescriptor& descriptor,
                       std::string* error_out)
{
    if (descriptor.status != "complete" && descriptor.status != "failed") {
        return fail(error_out, "context status must be complete or failed");
    }
    if ((descriptor.status == "complete" && !descriptor.failure_reason.empty()) ||
        (descriptor.status == "failed" && !safe_text(descriptor.failure_reason))) {
        return fail(error_out, "context failure_reason does not match status");
    }
    if (!safe_text(descriptor.recording_id) ||
        !safe_text(descriptor.session_id) ||
        !safe_identifier(descriptor.producer_generation) ||
        !canonical_sha256(descriptor.recording_identity_token)) {
        return fail(error_out, "recording identity is invalid");
    }
    if (descriptor.camera_id < 0 || !safe_identifier(descriptor.camera_serial) ||
        !safe_identifier(descriptor.source_camera_stream_id) ||
        !safe_identifier(descriptor.stream_epoch_id) ||
        !canonical_sha256(descriptor.camera_configuration_sha256)) {
        return fail(error_out, "camera identity or configuration digest is invalid");
    }

    const RegisteredSceneContextSourceFrame& source = descriptor.source_frame;
    if (source.source_frame_id == 0 || source.local_frame_id == 0 ||
        source.camera_frame_id == 0 || source.camera_timestamp_ns == 0 ||
        source.timestamp_sys_ns == 0) {
        return fail(error_out, "source frame identities and timestamps must be positive");
    }

    const RegisteredSceneContextRaster& raster = descriptor.native_raster;
    if (raster.width == 0 || raster.height == 0 ||
        raster.stride_bytes < raster.width) {
        return fail(error_out, "native raster dimensions or stride are invalid");
    }
    if (static_cast<std::uint64_t>(raster.stride_bytes) * raster.height >
        kMaxContextBytes) {
        return fail(error_out, "native raster exceeds context byte bound");
    }
    if (descriptor.pixel_format != kRegisteredSceneContextPixelFormat) {
        return fail(error_out, "registered scene context pixel format must be Mono8");
    }
    if (descriptor.coordinate_space != "camera_native_px") {
        return fail(error_out, "registered scene context coordinate space is invalid");
    }
    if (!validate_reference(descriptor.layout, "layout", error_out) ||
        !validate_reference(descriptor.materialization, "materialization", error_out) ||
        !validate_reference(descriptor.registration, "registration", error_out)) {
        return false;
    }

    const RegisteredSceneContextCaptureInvariants& invariants = descriptor.invariants;
    const bool accepted_registration =
        invariants.registration_authority_status ==
        "accepted_for_experiment";
    const bool diagnostic_registration =
        invariants.registration_authority_status ==
        "diagnostic_not_physical_acceptance";
    if ((!accepted_registration && !diagnostic_registration) ||
        invariants.daily_registration_accepted != accepted_registration ||
        !invariants.dish_setup_complete ||
        (invariants.subject_presence != "absent" &&
         invariants.subject_presence != "present" &&
         invariants.subject_presence != "unknown") ||
        !invariants.nir_illumination_fixed ||
        !invariants.camera_configuration_fixed || !invariants.rig_fixed) {
        return fail(error_out,
                    "registered scene context requires an explicit accepted or "
                    "diagnostic registration status consistent with its daily "
                    "acceptance flag, complete dish setup, a closed subject-"
                    "presence state, and fixed NIR/camera/rig state");
    }

    return true;
}

bool validate_common(const RegisteredSceneContextDescriptor& descriptor,
                     std::string* error_out)
{
    if (!validate_metadata(descriptor, error_out)) {
        return false;
    }
    const RegisteredSceneContextArtifact& artifact = descriptor.artifact;
    if (descriptor.status == "failed") {
        if (!artifact.relative_path.empty() || artifact.size_bytes != 0 ||
            !artifact.sha256.empty()) {
            return fail(error_out, "failed context must not contain an artifact receipt");
        }
        return true;
    }
    if (!safe_flat_relative_path(artifact.relative_path) || artifact.size_bytes == 0 ||
        artifact.size_bytes > kMaxContextBytes ||
        !canonical_sha256(artifact.sha256)) {
        return fail(error_out, "context artifact receipt is invalid");
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(descriptor.native_raster.stride_bytes) *
        descriptor.native_raster.height;
    if (artifact.size_bytes != expected_bytes) {
        return fail(error_out, "context artifact size does not match native raster");
    }
    return true;
}

bool parse_reference(const json& object,
                     const std::string& path,
                     RegisteredSceneContextReference* output,
                     std::string* error_out)
{
    if (!exact_keys(object, kReferenceKeys, path, error_out) ||
        !read_string(object, "id", path, &output->id, error_out, true) ||
        !read_sha256(object, "sha256", path, &output->sha256, error_out)) {
        return false;
    }
    return true;
}

}  // namespace

bool validate_registered_scene_context_descriptor(
    const RegisteredSceneContextDescriptor& descriptor,
    std::string* error_out)
{
    clear_error(error_out);
    return validate_common(descriptor, error_out);
}

nlohmann::json registered_scene_context_descriptor_to_json(
    const RegisteredSceneContextDescriptor& descriptor)
{
    return {
        {"schema_id", kRegisteredSceneContextSchemaId},
        {"schema_version", kRegisteredSceneContextSchemaVersion},
        {"canonicalization", kRegisteredSceneContextCanonicalization},
        {"capture_role", kRegisteredSceneContextCaptureRole},
        {"status", descriptor.status},
        {"failure_reason", descriptor.failure_reason},
        {"recording", {
            {"recording_id", descriptor.recording_id},
            {"session_id", descriptor.session_id},
            {"recording_identity_token", descriptor.recording_identity_token},
            {"producer_generation", descriptor.producer_generation}}},
        {"camera", {
            {"camera_id", descriptor.camera_id},
            {"camera_serial", descriptor.camera_serial},
            {"source_camera_stream_id", descriptor.source_camera_stream_id},
            {"stream_epoch_id", descriptor.stream_epoch_id},
            {"camera_configuration_sha256", descriptor.camera_configuration_sha256}}},
        {"source_frame", {
            {"source_frame_id", descriptor.source_frame.source_frame_id},
            {"local_frame_id", descriptor.source_frame.local_frame_id},
            {"camera_frame_id", descriptor.source_frame.camera_frame_id},
            {"recording_frame_id", descriptor.source_frame.recording_frame_id},
            {"camera_timestamp_ns", descriptor.source_frame.camera_timestamp_ns},
            {"timestamp_sys_ns", descriptor.source_frame.timestamp_sys_ns}}},
        {"native_raster", {
            {"width", descriptor.native_raster.width},
            {"height", descriptor.native_raster.height},
            {"stride_bytes", descriptor.native_raster.stride_bytes}}},
        {"coordinate_space", descriptor.coordinate_space},
        {"pixel_format", descriptor.pixel_format},
        {"geometry_binding", {
            {"layout", { {"id", descriptor.layout.id},
                          {"sha256", descriptor.layout.sha256}}},
            {"materialization", { {"id", descriptor.materialization.id},
                                   {"sha256", descriptor.materialization.sha256}}},
            {"registration", { {"id", descriptor.registration.id},
                                {"sha256", descriptor.registration.sha256}}}}},
        {"capture_invariants", {
            {"daily_registration_accepted", descriptor.invariants.daily_registration_accepted},
            {"registration_authority_status",
             descriptor.invariants.registration_authority_status},
            {"dish_setup_complete", descriptor.invariants.dish_setup_complete},
            {"subject_presence", descriptor.invariants.subject_presence},
            {"nir_illumination_fixed", descriptor.invariants.nir_illumination_fixed},
            {"camera_configuration_fixed", descriptor.invariants.camera_configuration_fixed},
            {"rig_fixed", descriptor.invariants.rig_fixed}}},
        {"artifact", {
            {"relative_path", descriptor.artifact.relative_path},
            {"size_bytes", descriptor.artifact.size_bytes},
            {"sha256", descriptor.artifact.sha256}}}};
}

bool registered_scene_context_descriptor_from_json(
    const nlohmann::json& value,
    RegisteredSceneContextDescriptor* descriptor_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (descriptor_out == nullptr) {
        return fail(error_out, "registered scene context descriptor output is null");
    }
    *descriptor_out = {};
    if (!exact_keys(value, kDescriptorKeys, "registered_scene_context", error_out)) {
        return false;
    }
    if (!value.at("schema_id").is_string() ||
        value.at("schema_id").get<std::string>() != kRegisteredSceneContextSchemaId) {
        return fail(error_out, "registered scene context schema_id is invalid");
    }
    if (!is_schema_version(value.at("schema_version"),
                           kRegisteredSceneContextSchemaVersion)) {
        return fail(error_out, "registered scene context schema_version is invalid");
    }
    if (!value.at("canonicalization").is_string() ||
        value.at("canonicalization").get<std::string>() !=
            kRegisteredSceneContextCanonicalization) {
        return fail(error_out, "registered scene context canonicalization is invalid");
    }
    if (!value.at("capture_role").is_string() ||
        value.at("capture_role").get<std::string>() != kRegisteredSceneContextCaptureRole) {
        return fail(error_out, "registered scene context schema identity is invalid");
    }
    if (!value.at("status").is_string() ||
        !value.at("failure_reason").is_string()) {
        return fail(error_out,
                    "registered scene context status and failure_reason must be strings");
    }
    descriptor_out->status = value.at("status").get<std::string>();
    descriptor_out->failure_reason = value.at("failure_reason").get<std::string>();
    if ((descriptor_out->status != "complete" && descriptor_out->status != "failed") ||
        (!descriptor_out->failure_reason.empty() &&
         !safe_text(descriptor_out->failure_reason))) {
        return fail(error_out, "registered scene context status is invalid");
    }

    const json& recording = value.at("recording");
    if (!exact_keys(recording, kRecordingKeys, "recording_scene_context.recording", error_out) ||
        !read_string(recording, "recording_id", "recording_scene_context.recording",
                     &descriptor_out->recording_id, error_out) ||
        !read_string(recording, "session_id", "recording_scene_context.recording",
                     &descriptor_out->session_id, error_out) ||
        !read_sha256(recording, "recording_identity_token", "recording_scene_context.recording",
                     &descriptor_out->recording_identity_token, error_out) ||
        !read_string(recording, "producer_generation", "recording_scene_context.recording",
                     &descriptor_out->producer_generation, error_out, true)) {
        return false;
    }

    const json& camera = value.at("camera");
    if (!exact_keys(camera, kCameraKeys, "recording_scene_context.camera", error_out) ||
        !read_int(camera, "camera_id", "recording_scene_context.camera",
                  &descriptor_out->camera_id, error_out) ||
        !read_string(camera, "camera_serial", "recording_scene_context.camera",
                     &descriptor_out->camera_serial, error_out, true) ||
        !read_string(camera, "source_camera_stream_id", "recording_scene_context.camera",
                     &descriptor_out->source_camera_stream_id, error_out, true) ||
        !read_string(camera, "stream_epoch_id", "recording_scene_context.camera",
                     &descriptor_out->stream_epoch_id, error_out, true) ||
        !read_sha256(camera, "camera_configuration_sha256", "recording_scene_context.camera",
                     &descriptor_out->camera_configuration_sha256, error_out)) {
        return false;
    }

    const json& source = value.at("source_frame");
    if (!exact_keys(source, kSourceFrameKeys, "recording_scene_context.source_frame", error_out) ||
        !read_u64(source, "source_frame_id", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.source_frame_id, error_out, true) ||
        !read_u64(source, "local_frame_id", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.local_frame_id, error_out, true) ||
        !read_u64(source, "camera_frame_id", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.camera_frame_id, error_out, true) ||
        !read_u64(source, "recording_frame_id", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.recording_frame_id, error_out) ||
        !read_u64(source, "camera_timestamp_ns", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.camera_timestamp_ns, error_out, true) ||
        !read_u64(source, "timestamp_sys_ns", "recording_scene_context.source_frame",
                  &descriptor_out->source_frame.timestamp_sys_ns, error_out, true)) {
        return false;
    }

    const json& raster = value.at("native_raster");
    if (!exact_keys(raster, kRasterKeys, "recording_scene_context.native_raster", error_out) ||
        !read_u32(raster, "width", "recording_scene_context.native_raster",
                  &descriptor_out->native_raster.width, error_out, true) ||
        !read_u32(raster, "height", "recording_scene_context.native_raster",
                  &descriptor_out->native_raster.height, error_out, true) ||
        !read_u32(raster, "stride_bytes", "recording_scene_context.native_raster",
                  &descriptor_out->native_raster.stride_bytes, error_out, true)) {
        return false;
    }
    if (!value.at("pixel_format").is_string()) {
        return fail(error_out, "recording_scene_context.pixel_format must be a string");
    }
    if (!value.at("coordinate_space").is_string()) {
        return fail(error_out, "recording_scene_context.coordinate_space must be a string");
    }
    descriptor_out->coordinate_space = value.at("coordinate_space").get<std::string>();
    descriptor_out->pixel_format = value.at("pixel_format").get<std::string>();

    const json& geometry = value.at("geometry_binding");
    if (!exact_keys(geometry, kGeometryKeys, "recording_scene_context.geometry_binding", error_out) ||
        !parse_reference(geometry.at("layout"), "recording_scene_context.geometry_binding.layout",
                         &descriptor_out->layout, error_out) ||
        !parse_reference(geometry.at("materialization"),
                         "recording_scene_context.geometry_binding.materialization",
                         &descriptor_out->materialization, error_out) ||
        !parse_reference(geometry.at("registration"),
                         "recording_scene_context.geometry_binding.registration",
                         &descriptor_out->registration, error_out)) {
        return false;
    }

    const json& invariants = value.at("capture_invariants");
    if (!exact_keys(invariants, kInvariantKeys,
                    "recording_scene_context.capture_invariants", error_out) ||
        !read_bool(invariants, "daily_registration_accepted",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.daily_registration_accepted, error_out) ||
        !read_string(invariants, "registration_authority_status",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.registration_authority_status,
                   error_out, true) ||
        !read_bool(invariants, "dish_setup_complete",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.dish_setup_complete, error_out) ||
        !read_string(invariants, "subject_presence",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.subject_presence, error_out) ||
        !read_bool(invariants, "nir_illumination_fixed",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.nir_illumination_fixed, error_out) ||
        !read_bool(invariants, "camera_configuration_fixed",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.camera_configuration_fixed, error_out) ||
        !read_bool(invariants, "rig_fixed",
                   "recording_scene_context.capture_invariants",
                   &descriptor_out->invariants.rig_fixed, error_out)) {
        return false;
    }

    const json& artifact = value.at("artifact");
    if (!exact_keys(artifact, kArtifactKeys, "recording_scene_context.artifact", error_out)) {
        return false;
    }
    if (descriptor_out->status == "failed") {
        const json& size = artifact.at("size_bytes");
        const bool zero_size =
            (size.is_number_unsigned() && size.get<std::uint64_t>() == 0) ||
            (size.is_number_integer() && size.get<std::int64_t>() == 0);
        if (!artifact.at("relative_path").is_string() ||
            !artifact.at("relative_path").get<std::string>().empty() ||
            !zero_size ||
            !artifact.at("sha256").is_string() ||
            !artifact.at("sha256").get<std::string>().empty()) {
            return fail(error_out,
                        "failed context artifact must be empty and unmaterialized");
        }
    } else if (!read_string(artifact, "relative_path", "recording_scene_context.artifact",
                            &descriptor_out->artifact.relative_path, error_out) ||
               !read_u64(artifact, "size_bytes", "recording_scene_context.artifact",
                         &descriptor_out->artifact.size_bytes, error_out, true) ||
               !read_sha256(artifact, "sha256", "recording_scene_context.artifact",
                            &descriptor_out->artifact.sha256, error_out)) {
        return false;
    }

    return validate_common(*descriptor_out, error_out);
}

bool publish_registered_scene_context(
    const SpatialRoiSessionAuthorityStore& authority,
    const std::string& descriptor_relative_path,
    const RegisteredSceneContextDescriptor& descriptor,
    const std::string& mono8_bytes,
    RegisteredSceneContextPublication* publication_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (publication_out == nullptr) {
        return fail(error_out, "registered scene context publication output is null");
    }
    *publication_out = {};
    if (!authority.valid()) {
        return fail(error_out, "registered scene context authority store is invalid");
    }
    if (descriptor.status != "complete" || !descriptor.failure_reason.empty()) {
        return fail(error_out, "only complete context captures can be published");
    }
    if (!safe_flat_relative_path(descriptor_relative_path) ||
        descriptor_relative_path == descriptor.artifact.relative_path) {
        return fail(error_out, "context descriptor path is invalid or aliases image artifact");
    }
    if (descriptor.artifact.size_bytes != 0 || !descriptor.artifact.sha256.empty()) {
        return fail(error_out, "input context descriptor must not contain an artifact receipt");
    }
    if (!validate_metadata(descriptor, error_out) ||
        !safe_flat_relative_path(descriptor.artifact.relative_path)) {
        return false;
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(descriptor.native_raster.stride_bytes) *
        descriptor.native_raster.height;
    if (mono8_bytes.size() != expected_bytes) {
        return fail(error_out, "Mono8 byte count does not match native raster");
    }

    SpatialRoiSessionAuthorityReceipt image_receipt;
    if (!authority.PublishBytes(descriptor.artifact.relative_path, mono8_bytes,
                                &image_receipt, error_out)) {
        return false;
    }
    RegisteredSceneContextDescriptor published = descriptor;
    published.artifact.relative_path = image_receipt.relative_path;
    published.artifact.size_bytes = image_receipt.size_bytes;
    published.artifact.sha256 = image_receipt.sha256;
    if (!validate_common(published, error_out)) {
        return false;
    }
    SpatialRoiSessionAuthorityReceipt descriptor_receipt;
    if (!authority.PublishJson(descriptor_relative_path,
                               registered_scene_context_descriptor_to_json(published),
                               &descriptor_receipt, error_out)) {
        return false;
    }
    publication_out->descriptor = std::move(published);
    publication_out->descriptor_receipt = std::move(descriptor_receipt);
    return true;
}

bool read_registered_scene_context_bytes(
    const SpatialRoiSessionAuthorityStore& authority,
    const RegisteredSceneContextDescriptor& descriptor,
    std::string* mono8_bytes_out,
    std::string* error_out)
{
    clear_error(error_out);
    if (mono8_bytes_out == nullptr) {
        return fail(error_out, "Mono8 output is null");
    }
    mono8_bytes_out->clear();
    if (!validate_common(descriptor, error_out)) {
        return false;
    }
    if (descriptor.status != "complete") {
        return fail(error_out, "failed context has no readable Mono8 artifact");
    }
    SpatialRoiSessionAuthorityReceipt expected = {
        descriptor.artifact.relative_path,
        descriptor.artifact.size_bytes,
        descriptor.artifact.sha256};
    if (!authority.ReadAndVerify(expected, mono8_bytes_out, nullptr, error_out)) {
        return false;
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(descriptor.native_raster.stride_bytes) *
        descriptor.native_raster.height;
    if (mono8_bytes_out->size() != expected_bytes) {
        mono8_bytes_out->clear();
        return fail(error_out, "verified context bytes do not match native raster");
    }
    return true;
}

}  // namespace orange::session::spatial_roi
