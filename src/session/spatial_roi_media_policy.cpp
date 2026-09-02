#include "session/spatial_roi_media_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;

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
                            " is not allowed by this schema");
        }
    }
    return true;
}

bool printable_backend(const std::string& value)
{
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool read_bool(const json& object,
               const char* key,
               const std::string& path,
               bool* out,
               std::string* error_out)
{
    const json& value = object.at(key);
    if (!value.is_boolean()) {
        return fail(error_out, path + "." + key + " must be a boolean");
    }
    *out = value.get<bool>();
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

bool read_schema_version(const json& object, int* out, std::string* error_out)
{
    const json& value = object.at("schema_version");
    if (!value.is_number_integer()) {
        return fail(error_out, "schema_version must be an integer");
    }
    const std::int64_t version = value.get<std::int64_t>();
    if (version < 0 ||
        version > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return fail(error_out, "schema_version is outside the supported range");
    }
    *out = static_cast<int>(version);
    return true;
}

bool parse_retained_products(const json& value,
                             RetainedProducts* products_out,
                             std::string* error_out)
{
    if (!exact_keys(value,
                    {"fixed_rois", "full_frame", "registered_context"},
                    "retained_products",
                    error_out)) {
        return false;
    }
    return read_bool(value,
                     "full_frame",
                     "retained_products",
                     &products_out->full_frame,
                     error_out) &&
           read_bool(value,
                     "fixed_rois",
                     "retained_products",
                     &products_out->fixed_rois,
                     error_out) &&
           read_bool(value,
                     "registered_context",
                     "retained_products",
                     &products_out->registered_context,
                     error_out);
}

}  // namespace

const char* media_policy_kind_to_string(const MediaPolicyKind kind) noexcept
{
    switch (kind) {
    case MediaPolicyKind::kFullFrameOnly:
        return kFullFrameOnlyMediaPolicy;
    case MediaPolicyKind::kFullFrameAndFixedRois:
        return kFullFrameAndFixedRoisMediaPolicy;
    case MediaPolicyKind::kFixedRoisWithRegisteredContext:
        return kFixedRoisWithRegisteredContextMediaPolicy;
    }
    return "";
}

bool media_policy_kind_from_string(const std::string& value,
                                   MediaPolicyKind* kind_out)
{
    if (!kind_out) {
        return false;
    }
    if (value == kFullFrameOnlyMediaPolicy) {
        *kind_out = MediaPolicyKind::kFullFrameOnly;
        return true;
    }
    if (value == kFullFrameAndFixedRoisMediaPolicy) {
        *kind_out = MediaPolicyKind::kFullFrameAndFixedRois;
        return true;
    }
    if (value == kFixedRoisWithRegisteredContextMediaPolicy) {
        *kind_out = MediaPolicyKind::kFixedRoisWithRegisteredContext;
        return true;
    }
    return false;
}

RetainedProducts retained_products_for_media_policy(
    const MediaPolicyKind kind) noexcept
{
    switch (kind) {
    case MediaPolicyKind::kFullFrameOnly:
        return {true, false, false};
    case MediaPolicyKind::kFullFrameAndFixedRois:
        return {true, true, false};
    case MediaPolicyKind::kFixedRoisWithRegisteredContext:
        return {false, true, true};
    }
    return {};
}

MediaPolicy make_media_policy(const MediaPolicyKind kind,
                              const std::string& sink_backend)
{
    MediaPolicy policy;
    policy.kind = kind;
    policy.retained_products = retained_products_for_media_policy(kind);
    policy.sink_backend = sink_backend;
    return policy;
}

MediaPolicy default_media_policy()
{
    return make_media_policy(MediaPolicyKind::kFullFrameAndFixedRois);
}

MediaPolicy default_media_policy(const bool spatial_roi_enabled)
{
    return spatial_roi_enabled
        ? default_media_policy()
        : make_media_policy(MediaPolicyKind::kFullFrameOnly);
}

bool validate_media_policy(const MediaPolicy& policy,
                           std::string* error_out)
{
    if (policy.schema_version != kMediaPolicySchemaVersion) {
        return fail(error_out,
                    "schema_version must be " +
                        std::to_string(kMediaPolicySchemaVersion));
    }
    const char* name = media_policy_kind_to_string(policy.kind);
    if (!name || *name == '\0') {
        return fail(error_out, "media_policy has an unknown value");
    }
    const RetainedProducts expected =
        retained_products_for_media_policy(policy.kind);
    if (policy.retained_products != expected) {
        return fail(error_out,
                    "retained_products does not match media_policy");
    }
    if (!policy.sink_backend.empty() &&
        !printable_backend(policy.sink_backend)) {
        return fail(error_out,
                    "sink_backend must be nonempty printable text <= 128 bytes");
    }
    return true;
}

bool parse_media_policy(const nlohmann::json& value,
                        MediaPolicy* policy_out,
                        std::string* error_out)
{
    if (!policy_out) {
        return fail(error_out, "policy output must not be null");
    }
    if (!exact_keys(value,
                    {"media_policy",
                     "retained_products",
                     "schema_id",
                     "schema_version",
                     "sink_backend"},
                    "media_policy",
                    error_out)) {
        return false;
    }

    std::string schema_id;
    if (!read_string(value, "schema_id", "media_policy", &schema_id, error_out)) {
        return false;
    }
    if (schema_id != kMediaPolicySchemaId) {
        return fail(error_out,
                    "schema_id must be " + std::string(kMediaPolicySchemaId));
    }

    MediaPolicy parsed;
    if (!read_schema_version(value, &parsed.schema_version, error_out)) {
        return false;
    }
    if (parsed.schema_version != kMediaPolicySchemaVersion) {
        return fail(error_out,
                    "unsupported media policy schema_version " +
                        std::to_string(parsed.schema_version));
    }

    std::string policy_name;
    if (!read_string(value,
                     "media_policy",
                     "media_policy",
                     &policy_name,
                     error_out) ||
        !media_policy_kind_from_string(policy_name, &parsed.kind)) {
        return fail(error_out,
                    "media_policy.media_policy must be one of full_frame_only, "
                    "full_frame_and_fixed_rois, "
                    "fixed_rois_with_registered_context");
    }
    if (!parse_retained_products(value.at("retained_products"),
                                 &parsed.retained_products,
                                 error_out)) {
        return false;
    }

    const json& sink_backend = value.at("sink_backend");
    if (sink_backend.is_null()) {
        parsed.sink_backend.clear();
    } else if (sink_backend.is_string()) {
        parsed.sink_backend = sink_backend.get<std::string>();
        if (!printable_backend(parsed.sink_backend)) {
            return fail(error_out,
                        "media_policy.sink_backend must be nonempty printable "
                        "text <= 128 bytes or null");
        }
    } else {
        return fail(error_out,
                    "media_policy.sink_backend must be a string or null");
    }

    if (!validate_media_policy(parsed, error_out)) {
        return false;
    }
    *policy_out = std::move(parsed);
    return true;
}

nlohmann::json media_policy_to_json(const MediaPolicy& policy)
{
    return {
        {"schema_id", kMediaPolicySchemaId},
        {"schema_version", policy.schema_version},
        {"media_policy", media_policy_kind_to_string(policy.kind)},
        {"retained_products",
         {{"full_frame", policy.retained_products.full_frame},
          {"fixed_rois", policy.retained_products.fixed_rois},
          {"registered_context", policy.retained_products.registered_context}}},
        {"sink_backend",
         policy.sink_backend.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(policy.sink_backend)}};
}

bool resolve_media_policy(const nlohmann::json& configured_policy,
                          const bool spatial_roi_enabled,
                          MediaPolicy* policy_out,
                          std::string* error_out)
{
    if (!policy_out) {
        return fail(error_out, "policy output must not be null");
    }
    if (configured_policy.is_null()) {
        *policy_out = default_media_policy(spatial_roi_enabled);
        return true;
    }
    if (configured_policy.is_string()) {
        MediaPolicyKind kind;
        if (!media_policy_kind_from_string(
                configured_policy.get<std::string>(), &kind)) {
            return fail(error_out,
                        "configured media policy has an unsupported value");
        }
        *policy_out = make_media_policy(kind);
        return true;
    }
    if (!configured_policy.is_object()) {
        return fail(error_out,
                    "configured media policy must be null, a policy name, or a "
                    "versioned policy object");
    }
    return parse_media_policy(configured_policy, policy_out, error_out);
}

bool media_policy_requires_full_frame(const MediaPolicy& policy) noexcept
{
    return policy.retained_products.full_frame;
}

bool media_policy_requires_fixed_rois(const MediaPolicy& policy) noexcept
{
    return policy.retained_products.fixed_rois;
}

bool media_policy_requires_registered_context(const MediaPolicy& policy) noexcept
{
    return policy.retained_products.registered_context;
}

bool media_policy_requires_full_frame(const MediaPolicyKind kind) noexcept
{
    return retained_products_for_media_policy(kind).full_frame;
}

bool media_policy_requires_fixed_rois(const MediaPolicyKind kind) noexcept
{
    return retained_products_for_media_policy(kind).fixed_rois;
}

bool media_policy_requires_registered_context(const MediaPolicyKind kind) noexcept
{
    return retained_products_for_media_policy(kind).registered_context;
}

}  // namespace orange::session::spatial_roi
