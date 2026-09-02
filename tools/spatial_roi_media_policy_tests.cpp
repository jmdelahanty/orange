#include "session/spatial_roi_media_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using orange::session::spatial_roi::MediaPolicy;
using orange::session::spatial_roi::MediaPolicyKind;
using orange::session::spatial_roi::default_media_policy;
using orange::session::spatial_roi::media_policy_requires_fixed_rois;
using orange::session::spatial_roi::media_policy_requires_full_frame;
using orange::session::spatial_roi::media_policy_requires_registered_context;
using orange::session::spatial_roi::media_policy_to_json;
using orange::session::spatial_roi::parse_media_policy;
using orange::session::spatial_roi::resolve_media_policy;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void require_parse_failure(nlohmann::json value, const std::string& message)
{
    MediaPolicy parsed;
    std::string error;
    require(!parse_media_policy(value, &parsed, &error), message);
    require(!error.empty(), message + " should report an error");
}

void test_policy_requirements()
{
    const MediaPolicy full = default_media_policy(false);
    require(media_policy_requires_full_frame(full),
            "full-frame-only policy should require full frame");
    require(!media_policy_requires_fixed_rois(full),
            "full-frame-only policy should disable fixed ROIs");
    require(!media_policy_requires_registered_context(full),
            "full-frame-only policy should not require context");

    const MediaPolicy combined = default_media_policy(true);
    require(media_policy_requires_full_frame(combined),
            "spatial ROI default must retain full frame");
    require(media_policy_requires_fixed_rois(combined),
            "spatial ROI default must retain fixed ROIs");
    require(!media_policy_requires_registered_context(combined),
            "combined default must not require context");

    const MediaPolicy roi_only = orange::session::spatial_roi::make_media_policy(
        MediaPolicyKind::kFixedRoisWithRegisteredContext, "external_ipc");
    require(!media_policy_requires_full_frame(roi_only),
            "ROI-only policy must not require full frame");
    require(media_policy_requires_fixed_rois(roi_only),
            "ROI-only policy must require fixed ROIs");
    require(media_policy_requires_registered_context(roi_only),
            "ROI-only policy must require registered context");
}

void test_closed_round_trip_and_backend_separation()
{
    const MediaPolicy source = orange::session::spatial_roi::make_media_policy(
        MediaPolicyKind::kFixedRoisWithRegisteredContext, "external_ipc");
    const nlohmann::json wire = media_policy_to_json(source);
    require(wire.at("media_policy") == "fixed_rois_with_registered_context",
            "wire policy name mismatch");
    require(wire.at("retained_products").at("full_frame") == false &&
                wire.at("retained_products").at("fixed_rois") == true &&
                wire.at("retained_products").at("registered_context") == true,
            "wire retained products mismatch");
    require(wire.at("sink_backend") == "external_ipc",
            "sink backend should be explicit and independent");

    MediaPolicy parsed;
    std::string error;
    require(parse_media_policy(wire, &parsed, &error), error);
    require(parsed == source, "media policy should round-trip exactly");

    nlohmann::json backend_changed = wire;
    backend_changed["sink_backend"] = "in_process";
    require(parse_media_policy(backend_changed, &parsed, &error), error);
    require(parsed.kind == source.kind && parsed.retained_products ==
                source.retained_products && parsed.sink_backend == "in_process",
            "changing sink backend must not change retained products");
}

void test_closed_schema_rejects_inconsistent_or_unknown_values()
{
    const nlohmann::json valid = media_policy_to_json(default_media_policy(true));

    nlohmann::json inconsistent = valid;
    inconsistent["retained_products"]["full_frame"] = false;
    require_parse_failure(inconsistent,
                           "inconsistent retained products must be rejected");

    nlohmann::json unknown_field = valid;
    unknown_field["unexpected"] = true;
    require_parse_failure(unknown_field,
                           "unknown policy fields must be rejected");

    nlohmann::json wrong_schema = valid;
    wrong_schema["schema_version"] = 2;
    require_parse_failure(wrong_schema,
                           "unsupported policy schema must be rejected");

    nlohmann::json bad_backend = valid;
    bad_backend["sink_backend"] = "";
    require_parse_failure(bad_backend,
                           "empty sink backend text must be rejected");

    nlohmann::json old_name = valid;
    old_name["media_policy"] = "crop_only";
    require_parse_failure(old_name,
                           "unpromoted legacy policy names must be rejected");
}

void test_absent_policy_resolution_preserves_compatibility()
{
    MediaPolicy resolved;
    std::string error;
    require(resolve_media_policy(nullptr, true, &resolved, &error), error);
    require(resolved.kind == MediaPolicyKind::kFullFrameAndFixedRois,
            "absent spatial-ROI policy must preserve combined behavior");
    require(media_policy_requires_full_frame(resolved) &&
                media_policy_requires_fixed_rois(resolved),
            "combined compatibility default must retain both products");

    require(resolve_media_policy(nullptr, false, &resolved, &error), error);
    require(resolved.kind == MediaPolicyKind::kFullFrameOnly,
            "absent non-ROI policy must resolve to full frame only");

    require(resolve_media_policy("fixed_rois_with_registered_context",
                                 true,
                                 &resolved,
                                 &error),
            error);
    require(resolved.kind ==
                MediaPolicyKind::kFixedRoisWithRegisteredContext,
            "policy-name shorthand should resolve explicitly");
}

}  // namespace

int main()
{
    try {
        test_policy_requirements();
        test_closed_round_trip_and_backend_separation();
        test_closed_schema_rejects_inconsistent_or_unknown_values();
        test_absent_policy_resolution_preserves_compatibility();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
