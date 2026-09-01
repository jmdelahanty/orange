#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace spatial_roi = orange::session::spatial_roi;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

spatial_roi::RoiConfig make_roi(const std::string& camera_serial,
                                const std::string& roi_id,
                                const std::string& region_id,
                                const spatial_roi::Rect& rect)
{
    spatial_roi::RoiConfig roi;
    roi.roi_id = roi_id;
    roi.region_id = region_id;
    roi.required = true;
    roi.content_rect = rect;
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id(camera_serial, roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem(camera_serial, roi_id);
    return roi;
}

spatial_roi::Config make_config()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.output_alignment_px = 2;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = 1000;
    config.recording_limits.max_media_bytes_per_stream = 10000;
    config.recording_limits.max_evidence_bytes_per_stream = 2000;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 128;
    config.admission.max_total_media_bytes = 20000;
    config.admission.max_total_evidence_bytes = 4000;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 0;
    camera.camera_serial = "2010096";
    camera.native_raster = {100, 100};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('2')};
    camera.registration = {"registration_1", digest('3')};
    camera.rois.push_back(make_roi(
        camera.camera_serial, "roi_1", "region_1", {0, 0, 3, 5}));
    camera.rois.push_back(make_roi(
        camera.camera_serial, "roi_2", "region_2", {20, 20, 4, 4}));
    config.cameras.emplace(camera.camera_serial, std::move(camera));
    return config;
}

void default_is_off_and_closed_round_trip_works()
{
    const spatial_roi::Config defaults = spatial_roi::default_config();
    require(!defaults.enabled, "spatial ROI recording must default off");
    require(spatial_roi::kConfigSchemaVersion == 2 &&
                spatial_roi::kPlanSchemaVersion == 2,
            "long-run admission requires config and plan schema v2");
    require(defaults.recording_limits.max_frames_per_stream ==
                spatial_roi::kDefaultMaxFramesPerStream &&
                defaults.recording_limits.max_media_bytes_per_stream ==
                    spatial_roi::kDefaultMaxMediaBytesPerStream &&
                defaults.recording_limits.max_evidence_bytes_per_stream ==
                    spatial_roi::kDefaultMaxEvidenceBytesPerStream,
            "disabled defaults must retain the documented per-stream ceilings");
    require(defaults.admission.max_total_media_bytes ==
                spatial_roi::kDefaultMaxTotalMediaBytes &&
                defaults.admission.max_total_evidence_bytes ==
                    spatial_roi::kDefaultMaxTotalEvidenceBytes,
            "disabled defaults must admit the default maximum stream count");
    std::string error;
    require(spatial_roi::validate_config(defaults, nullptr, &error), error);

    const spatial_roi::Config source = make_config();
    const nlohmann::json wire = spatial_roi::config_to_json(source);
    spatial_roi::Config parsed;
    require(spatial_roi::parse_config(wire, &parsed, &error), error);
    require(spatial_roi::config_to_json(parsed) == wire,
            "config parse/build round trip changed normalized JSON");

    nlohmann::json unknown = wire;
    unknown["future_field"] = true;
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "unknown top-level field must fail closed");
    require(error.find("not allowed") != std::string::npos,
            "unknown-field failure should be explicit");

    unknown = wire;
    unknown["cameras"]["2010096"]["rois"][0]["future_field"] = true;
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "unknown ROI field must fail closed");

    unknown = wire;
    unknown["recording_limits"]["future_field"] = true;
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "unknown recording_limits field must fail closed");

    unknown = wire;
    unknown.erase("recording_limits");
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "missing recording_limits policy must fail closed");

    unknown = wire;
    unknown["recording_limits"].erase("max_media_bytes_per_stream");
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "missing recording_limits member must fail closed");

    unknown = wire;
    unknown["admission"].erase("max_total_media_bytes");
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "missing aggregate media ceiling must fail closed");

    unknown = wire;
    unknown["admission"].erase("max_total_evidence_bytes");
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "missing aggregate evidence ceiling must fail closed");

    unknown = wire;
    unknown["schema_version"] = 1;
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "schema-v1 config must not be interpreted as schema v2");

    unknown = wire;
    unknown["schema_version"] = std::numeric_limits<std::uint64_t>::max();
    require(!spatial_roi::parse_config(unknown, &parsed, &error),
            "oversized schema version must fail without throwing");
}

void enabled_requires_camera_and_roi()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    std::string error;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "enabled empty config must fail");

    config = make_config();
    config.cameras.begin()->second.rois.clear();
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "enabled camera with no ROIs must fail");

    config.enabled = false;
    require(spatial_roi::validate_config(config, nullptr, &error),
            "disabled config may retain an empty camera definition");
}

void disabled_config_cannot_build_or_verify_plan()
{
    spatial_roi::Config config = make_config();
    config.enabled = false;
    spatial_roi::PlanContext context;
    context.recording_id = "disabled-plan-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(!spatial_roi::build_plan(config, context, &plan, nullptr, &error),
            "disabled config must not produce a recording plan");
    require(error.find("enabled=true") != std::string::npos,
            "disabled plan rejection did not explain the arming contract");

    config.enabled = true;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error), error);
    plan["plan"]["configuration"]["enabled"] = false;
    require(!spatial_roi::verify_plan(plan, &error),
            "verification must reject a plan whose configuration is disabled");
    require(error.find("enabled=true") != std::string::npos,
            "disabled verification rejection did not explain the arming contract");
}

void identity_and_artifact_names_are_safe_and_exact()
{
    spatial_roi::Config config = make_config();
    std::string error;
    config.cameras.begin()->second.rois[0].logical_stream_id = "../escape";
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "unsafe stream ID must fail");

    config = make_config();
    config.cameras.begin()->second.rois[0].artifact_stem = "custom_name";
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "noncanonical artifact stem must fail");

    config = make_config();
    config.cameras.begin()->second.rois[0].has_region_mask = true;
    config.cameras.begin()->second.rois[0].region_mask = {
        "../mask.png", digest('4')};
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "traversing region-mask path must fail");

    config.cameras.begin()->second.rois[0].region_mask.relative_path =
        "recording_geometry_assets/masks/region_1.png";
    require(spatial_roi::validate_config(config, nullptr, &error), error);

    config.cameras.begin()->second.layout.sha256 = "SHA256:not-lowercase";
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "invalid authority digest must fail");
}

void bounds_duplicates_and_overlap_fail_closed()
{
    std::string error;
    spatial_roi::Config config = make_config();
    config.cameras.begin()->second.rois[0].content_rect = {99, 99, 2, 2};
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "out-of-bounds ROI must fail");

    config = make_config();
    config.cameras.begin()->second.rois[1].roi_id = "roi_1";
    config.cameras.begin()->second.rois[1].logical_stream_id =
        spatial_roi::expected_logical_stream_id("2010096", "roi_1");
    config.cameras.begin()->second.rois[1].artifact_stem =
        spatial_roi::expected_artifact_stem("2010096", "roi_1");
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "duplicate ROI ID must fail");

    config = make_config();
    config.cameras.begin()->second.rois[1].content_rect = {2, 4, 4, 4};
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "undeclared overlap must fail");

    config.cameras.begin()->second.allow_roi_overlap = true;
    require(spatial_roi::validate_config(config, nullptr, &error),
            "explicit overlap must be allowed");

    config = make_config();
    config.cameras.begin()->second.rois[1].content_rect = {3, 0, 4, 4};
    require(spatial_roi::validate_config(config, nullptr, &error),
            "edge-touching ROIs must not count as overlap");
}

void alignment_and_admission_usage_are_exact()
{
    const spatial_roi::Config config = make_config();
    spatial_roi::AdmissionUsage usage;
    std::string error;
    require(spatial_roi::validate_config(config, &usage, &error), error);
    require(usage.camera_count == 1, "camera count mismatch");
    require(usage.roi_count == 2, "ROI count mismatch");
    require(usage.encoder_stream_count == 2, "encoder count mismatch");
    // ROI 1: content 3*5, padded 4*6. ROI 2: 4*4 unchanged.
    require(usage.content_pixel_rate == (15 + 16) * 100,
            "content pixel rate mismatch");
    require(usage.encoded_pixel_rate == (24 + 16) * 100,
            "padded pixel rate mismatch");
    require(usage.pool_bytes == (24 + 16) * 4,
            "pool byte calculation mismatch");
    require(usage.queue_frames == 2 * 8,
            "aggregate queue frame calculation mismatch");
    require(usage.media_bytes == 20000,
            "aggregate media-byte admission mismatch");
    require(usage.evidence_bytes == 4000,
            "aggregate evidence-byte admission mismatch");
}

void each_admission_limit_is_enforced()
{
    std::string error;
    spatial_roi::Config config = make_config();
    config.admission.max_rois_per_camera = 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "per-camera ROI limit must be enforced");

    config = make_config();
    config.admission.max_total_rois = 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "total ROI limit must be enforced");

    config = make_config();
    config.admission.max_total_encoder_streams = 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "encoder stream limit must be enforced");

    config = make_config();
    config.admission.max_total_pixel_rate = 3999;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "padded pixel-rate limit must be enforced");

    config = make_config();
    config.admission.max_total_pool_bytes = 159;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "pool byte limit must be enforced");

    config = make_config();
    config.admission.max_total_queue_frames = 15;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "aggregate queue-frame limit must be enforced");

    config = make_config();
    config.admission.max_total_media_bytes = 19999;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "aggregate media-byte limit must be enforced");

    config = make_config();
    config.admission.max_total_evidence_bytes = 3999;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "aggregate evidence-byte limit must be enforced");
}

void recording_limits_are_positive_and_aggregate_overflow_is_rejected()
{
    std::string error;
    spatial_roi::Config config = make_config();
    config.recording_limits.max_frames_per_stream = 0;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "zero max_frames_per_stream must fail");

    config = make_config();
    config.recording_limits.max_media_bytes_per_stream = 0;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "zero max_media_bytes_per_stream must fail");

    config = make_config();
    config.recording_limits.max_evidence_bytes_per_stream = 0;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "zero max_evidence_bytes_per_stream must fail");

    config = make_config();
    config.recording_limits.max_frames_per_stream =
        spatial_roi::kMaxFramesPerStream + 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "frame limit beyond the recorder implementation ceiling must fail");

    config = make_config();
    config.recording_limits.max_media_bytes_per_stream =
        spatial_roi::kMaxMediaBytesPerStream + 1;
    config.admission.max_total_media_bytes =
        std::numeric_limits<std::uint64_t>::max();
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "media limit beyond the recorder implementation ceiling must fail");

    config = make_config();
    config.recording_limits.max_evidence_bytes_per_stream =
        spatial_roi::kMaxEvidenceBytesPerStream + 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "evidence limit beyond the recorder implementation ceiling must fail");

    config = make_config();
    config.admission.max_total_media_bytes = 0;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "zero aggregate media ceiling must fail");

    config = make_config();
    config.admission.max_total_evidence_bytes = 0;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "zero aggregate evidence ceiling must fail");

    nlohmann::json wire = spatial_roi::config_to_json(make_config());
    wire["recording_limits"]["max_frames_per_stream"] = 0;
    spatial_roi::Config parsed;
    require(!spatial_roi::parse_config(wire, &parsed, &error),
            "zero wire recording limit must fail");

    config = make_config();
    config.recording_limits.max_media_bytes_per_stream =
        spatial_roi::kMaxMediaBytesPerStream;
    config.admission.max_total_media_bytes =
        std::numeric_limits<std::uint64_t>::max();
    config.cameras.begin()->second.rois.push_back(make_roi(
        config.cameras.begin()->second.camera_serial,
        "roi_3",
        "region_3",
        {40, 40, 4, 4}));
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "aggregate media-byte overflow must fail before admission");
    require(error.find("overflow") != std::string::npos,
            "media-byte overflow rejection should be explicit");

    config = make_config();
    config.recording_limits.max_evidence_bytes_per_stream =
        std::numeric_limits<std::uint64_t>::max();
    config.admission.max_total_evidence_bytes =
        std::numeric_limits<std::uint64_t>::max();
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "unimplementable evidence-byte ceiling must fail before admission");
}

void pixel_contract_is_nonnegotiable_in_v2()
{
    std::string error;
    spatial_roi::Config config = make_config();
    config.no_resize = false;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "resize-enabled config must fail");

    config = make_config();
    config.no_color_conversion = false;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "color-conversion config must fail");

    config = make_config();
    config.output_alignment_px = 3;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "unsupported output alignment must fail");

    config = make_config();
    config.padding_value_mono8 = 1;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "nonzero Mono8 padding must fail in schema v2");

    nlohmann::json wire = spatial_roi::config_to_json(make_config());
    wire["pixel_contract"]["padding_value_mono8"] = 1;
    spatial_roi::Config parsed;
    require(!spatial_roi::parse_config(wire, &parsed, &error),
            "nonzero wire Mono8 padding must fail in schema v2");
}

void unsupported_best_effort_contract_values_fail_closed()
{
    std::string error;
    spatial_roi::Config config = make_config();
    config.strict = false;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "strict=false must fail closed until best-effort admission is implemented");

    config = make_config();
    config.cameras.begin()->second.rois[0].required = false;
    require(!spatial_roi::validate_config(config, nullptr, &error),
            "required=false must fail closed until optional ROI handling is implemented");

    spatial_roi::Config parsed;
    nlohmann::json wire = spatial_roi::config_to_json(make_config());
    wire["strict"] = false;
    require(!spatial_roi::parse_config(wire, &parsed, &error),
            "wire strict=false must fail closed");

    wire = spatial_roi::config_to_json(make_config());
    wire["cameras"]["2010096"]["rois"][0]["required"] = false;
    require(!spatial_roi::parse_config(wire, &parsed, &error),
            "wire required=false must fail closed");
}

void recording_identity_uses_canonical_limit_and_token_binding()
{
    const spatial_roi::Config config = make_config();
    spatial_roi::PlanContext context;
    context.recording_id = std::string(512, 'r');
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = "5aa9c13";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error), error);
    require(spatial_roi::verify_plan(plan, &error), error);

    spatial_roi::PlanContext too_long = context;
    too_long.recording_id.push_back('x');
    too_long.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            too_long.recording_id);
    require(!spatial_roi::build_plan(config, too_long, &plan, nullptr, &error),
            "recording IDs over the canonical 512-byte limit must fail");

    spatial_roi::PlanContext mismatched = context;
    mismatched.recording_identity_token = digest('a');
    require(!spatial_roi::build_plan(config, mismatched, &plan, nullptr, &error),
            "recording identity token must be cryptographically bound to recording ID");

    nlohmann::json tampered;
    require(spatial_roi::build_plan(config, context, &tampered, nullptr, &error), error);
    tampered["plan"]["recording_identity_token"] = digest('b');
    require(!spatial_roi::verify_plan(tampered, &error),
            "plan verification must reject a token for a different recording ID");
}

void plan_is_deterministic_closed_and_digest_bound()
{
    const spatial_roi::Config config = make_config();
    spatial_roi::PlanContext context;
    context.recording_id = "2026_08_30_19_00_00";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = "5aa9c13";

    nlohmann::json first;
    nlohmann::json second;
    std::string error;
    require(spatial_roi::build_plan(config, context, &first, nullptr, &error), error);
    require(spatial_roi::build_plan(config, context, &second, nullptr, &error), error);
    require(first == second, "same normalized inputs must produce the same plan");
    require(spatial_roi::verify_plan(first, &error), error);
    require(first.at("schema_version") == 2 &&
                first.at("plan").at("schema_version") == 2,
            "plan envelope and payload must both declare schema v2");
    require(first.at("plan").at("configuration").at("recording_limits") ==
                nlohmann::json({
                    {"max_frames_per_stream", 1000},
                    {"max_media_bytes_per_stream", 10000},
                    {"max_evidence_bytes_per_stream", 2000},
                }),
            "plan must authenticate the exact recording_limits policy");
    require(first.at("plan").at("admission_usage").at("media_bytes") ==
                20000 &&
                first.at("plan").at("admission_usage").at("evidence_bytes") ==
                    4000,
            "plan must authenticate checked long-run aggregate usage");

    spatial_roi::SpatialRoiRecordingPlan parsed_plan;
    require(spatial_roi::parse_verified_plan(first, &parsed_plan, &error), error);
    require(parsed_plan.recording_limits.max_frames_per_stream == 1000 &&
                parsed_plan.recording_limits.max_media_bytes_per_stream == 10000 &&
                parsed_plan.recording_limits.max_evidence_bytes_per_stream == 2000 &&
                parsed_plan.admission_usage.media_bytes == 20000 &&
                parsed_plan.admission_usage.evidence_bytes == 4000,
            "verified plan view omitted authenticated long-run bounds");

    const nlohmann::json& resolved_roi =
        first.at("plan")
            .at("resolved_cameras")
            .at("2010096")
            .at("rois")
            .at(0);
    require(resolved_roi.at("encoded_raster").at("width") == 4,
            "plan did not record aligned width");
    require(resolved_roi.at("encoded_raster").at("height") == 6,
            "plan did not record aligned height");
    require(resolved_roi.at("padding").at("right") == 1,
            "plan did not record right padding");
    require(resolved_roi.at("padding").at("bottom") == 1,
            "plan did not record bottom padding");
    require(resolved_roi.at("encoded_content_rect") ==
                nlohmann::json({{"x", 0}, {"y", 0}, {"width", 3}, {"height", 5}}),
            "plan did not expose producer encoded-content placement");
    require(resolved_roi.at("no_scaling") == true,
            "plan must assert no scaling");

    const nlohmann::json& resolved_camera =
        first.at("plan").at("resolved_cameras").at("2010096");
    require(resolved_camera.at("camera_id") == 0,
            "plan omitted source camera numeric identity");
    require(resolved_camera.at("camera_serial") == "2010096",
            "plan omitted source camera serial identity");
    require(resolved_camera.at("native_raster") ==
                nlohmann::json({{"width", 100}, {"height", 100}}),
            "plan omitted source camera raster binding");
    require(first.at("plan").at("configuration").at("pixel_contract")
                .at("source_format") == "mono8",
            "plan omitted source pixel-format binding");

    nlohmann::json tampered = first;
    tampered["plan"]["resolved_cameras"]["2010096"]["rois"][0]
            ["encoded_raster"]["width"] = 8;
    require(!spatial_roi::verify_plan(tampered, &error),
            "tampered resolved geometry must fail verification");

    tampered = first;
    tampered["future_field"] = true;
    require(!spatial_roi::verify_plan(tampered, &error),
            "unknown plan-envelope field must fail closed");

    tampered = first;
    tampered["plan_sha256"] = digest('b');
    require(!spatial_roi::verify_plan(tampered, &error),
            "wrong plan digest must fail verification");

    tampered = first;
    tampered["plan"]["configuration"]["recording_limits"]
            ["max_media_bytes_per_stream"] = 10001;
    require(!spatial_roi::verify_plan(tampered, &error),
            "mutated per-stream media admission must fail verification");

    tampered = first;
    tampered["plan"]["admission_usage"]["evidence_bytes"] = 4001;
    require(!spatial_roi::verify_plan(tampered, &error),
            "mutated aggregate evidence usage must fail deterministic verification");

    tampered = first;
    tampered["schema_version"] = 1;
    require(!spatial_roi::verify_plan(tampered, &error),
            "schema-v1 plan envelope must not verify as schema v2");

    tampered = first;
    tampered["plan"]["schema_version"] = 1;
    require(!spatial_roi::verify_plan(tampered, &error),
            "schema-v1 plan payload must not verify as schema v2");
}

void bool_plan_apis_catch_malformed_utf8()
{
    spatial_roi::Config config = make_config();
    spatial_roi::PlanContext context;
    context.recording_id = "malformed-utf8-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-30T23:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error),
            error);
    // nlohmann::json::dump rejects this invalid UTF-8 sequence while the
    // earlier printable-text checks intentionally permit bytes above ASCII.
    plan["plan"]["generated_at_utc"] = std::string("\xc3\x28", 2);
    bool returned = false;
    try {
        returned = !spatial_roi::verify_plan(plan, &error);
    } catch (...) {
        throw std::runtime_error(
            "verify_plan must convert malformed UTF-8 exceptions to false");
    }
    require(returned && error.find("serialization failed") != std::string::npos,
            "malformed UTF-8 verification should return a serialization error");

    spatial_roi::SpatialRoiRecordingPlan parsed;
    try {
        returned = !spatial_roi::parse_verified_plan(plan, &parsed, &error);
    } catch (...) {
        throw std::runtime_error(
            "parse_verified_plan must convert malformed UTF-8 exceptions to false");
    }
    require(returned, "malformed UTF-8 parse should fail closed");
}

}  // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"default_is_off_and_closed_round_trip_works",
         default_is_off_and_closed_round_trip_works},
        {"enabled_requires_camera_and_roi", enabled_requires_camera_and_roi},
        {"disabled_config_cannot_build_or_verify_plan",
         disabled_config_cannot_build_or_verify_plan},
        {"identity_and_artifact_names_are_safe_and_exact",
         identity_and_artifact_names_are_safe_and_exact},
        {"bounds_duplicates_and_overlap_fail_closed",
         bounds_duplicates_and_overlap_fail_closed},
        {"alignment_and_admission_usage_are_exact",
         alignment_and_admission_usage_are_exact},
        {"each_admission_limit_is_enforced", each_admission_limit_is_enforced},
        {"recording_limits_are_positive_and_aggregate_overflow_is_rejected",
         recording_limits_are_positive_and_aggregate_overflow_is_rejected},
        {"pixel_contract_is_nonnegotiable_in_v2",
         pixel_contract_is_nonnegotiable_in_v2},
        {"unsupported_best_effort_contract_values_fail_closed",
         unsupported_best_effort_contract_values_fail_closed},
        {"recording_identity_uses_canonical_limit_and_token_binding",
         recording_identity_uses_canonical_limit_and_token_binding},
        {"plan_is_deterministic_closed_and_digest_bound",
         plan_is_deterministic_closed_and_digest_bound},
        {"bool_plan_apis_catch_malformed_utf8",
         bool_plan_apis_catch_malformed_utf8},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " spatial ROI config test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " spatial ROI config tests passed\n";
    return 0;
}
