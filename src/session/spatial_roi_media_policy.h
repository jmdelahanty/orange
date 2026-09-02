#pragma once

#include "json.hpp"

#include <string>

namespace orange::session::spatial_roi {

// This schema describes retained recording products.  It intentionally does
// not select, imply, or authenticate a writer/transport implementation.
inline constexpr const char* kMediaPolicySchemaId =
    "orange.spatial_roi_recording.media_policy";
inline constexpr int kMediaPolicySchemaVersion = 1;

inline constexpr const char* kFullFrameOnlyMediaPolicy = "full_frame_only";
inline constexpr const char* kFullFrameAndFixedRoisMediaPolicy =
    "full_frame_and_fixed_rois";
inline constexpr const char* kFixedRoisWithRegisteredContextMediaPolicy =
    "fixed_rois_with_registered_context";

inline constexpr const char* kMediaPolicyFullFrameOnly =
    kFullFrameOnlyMediaPolicy;
inline constexpr const char* kMediaPolicyFullFrameAndFixedRois =
    kFullFrameAndFixedRoisMediaPolicy;
inline constexpr const char* kMediaPolicyFixedRoisWithRegisteredContext =
    kFixedRoisWithRegisteredContextMediaPolicy;

// Descriptive aliases make the schema constants discoverable alongside the
// other spatial-ROI contracts without creating a second schema.
inline constexpr const char* kSpatialRoiMediaPolicySchemaId =
    kMediaPolicySchemaId;
inline constexpr int kSpatialRoiMediaPolicySchemaVersion =
    kMediaPolicySchemaVersion;

enum class MediaPolicyKind {
    kFullFrameOnly,
    kFullFrameAndFixedRois,
    kFixedRoisWithRegisteredContext,
};

using SpatialRoiMediaPolicyKind = MediaPolicyKind;

// These fields are deliberately explicit in the wire schema.  A consumer
// must not infer a retained product from sink_backend (or vice versa).
struct RetainedProducts {
    bool full_frame = false;
    bool fixed_rois = false;
    bool registered_context = false;

    bool operator==(const RetainedProducts& other) const noexcept
    {
        return full_frame == other.full_frame &&
               fixed_rois == other.fixed_rois &&
               registered_context == other.registered_context;
    }

    bool operator!=(const RetainedProducts& other) const noexcept
    {
        return !(*this == other);
    }
};

using RetainedMediaProducts = RetainedProducts;

// A resolved media policy may carry an empty sink_backend until the caller
// resolves the independently selected sink.  Empty is serialized as JSON
// null; it is not a request to use a fallback backend.
struct MediaPolicy {
    int schema_version = kMediaPolicySchemaVersion;
    MediaPolicyKind kind = MediaPolicyKind::kFullFrameOnly;
    RetainedProducts retained_products;
    std::string sink_backend;

    bool operator==(const MediaPolicy& other) const noexcept
    {
        return schema_version == other.schema_version &&
               kind == other.kind &&
               retained_products == other.retained_products &&
               sink_backend == other.sink_backend;
    }

    bool operator!=(const MediaPolicy& other) const noexcept
    {
        return !(*this == other);
    }
};

// Prefixing the type is useful at call sites that combine this namespace with
// another media-policy namespace.
using SpatialRoiMediaPolicy = MediaPolicy;

const char* media_policy_kind_to_string(MediaPolicyKind kind) noexcept;
bool media_policy_kind_from_string(const std::string& value,
                                   MediaPolicyKind* kind_out);

RetainedProducts retained_products_for_media_policy(MediaPolicyKind kind) noexcept;

MediaPolicy make_media_policy(MediaPolicyKind kind,
                              const std::string& sink_backend = {});

// The spatial-ROI default preserves the currently deployed combined workload:
// a full-frame stream plus the fixed ROI streams.  Callers that know spatial
// ROI is disabled can use the bool overload to resolve the ordinary
// full-frame-only default.
MediaPolicy default_media_policy();
MediaPolicy default_media_policy(bool spatial_roi_enabled);

// Closed-schema validation and serialization.  Unknown fields, unknown
// policy values, inconsistent retained_products, and invalid sink-backend
// text are rejected.  sink_backend may be JSON null to represent a backend
// that is resolved by the surrounding recording configuration.
bool validate_media_policy(const MediaPolicy& policy,
                           std::string* error_out = nullptr);
bool parse_media_policy(const nlohmann::json& value,
                        MediaPolicy* policy_out,
                        std::string* error_out = nullptr);
nlohmann::json media_policy_to_json(const MediaPolicy& policy);

// Resolve either a v1 policy envelope, the persisted policy-name shorthand,
// or an absent/null value.  The shorthand is intentionally accepted only at
// this resolution seam; persisted/exported policy artifacts must use the
// closed versioned envelope produced by media_policy_to_json().
bool resolve_media_policy(const nlohmann::json& configured_policy,
                          bool spatial_roi_enabled,
                          MediaPolicy* policy_out,
                          std::string* error_out = nullptr);

bool media_policy_requires_full_frame(const MediaPolicy& policy) noexcept;
bool media_policy_requires_fixed_rois(const MediaPolicy& policy) noexcept;
bool media_policy_requires_registered_context(const MediaPolicy& policy) noexcept;

// Enum overloads are convenient for admission/preflight code that has not
// yet materialized the complete policy object.
bool media_policy_requires_full_frame(MediaPolicyKind kind) noexcept;
bool media_policy_requires_fixed_rois(MediaPolicyKind kind) noexcept;
bool media_policy_requires_registered_context(MediaPolicyKind kind) noexcept;

}  // namespace orange::session::spatial_roi
