#include "session/spatial_roi_recorder_runtime_config.h"

#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;
namespace roi = orange::session::spatial_roi;

constexpr const char* kStreamOne = "2010096_spatial_roi_roi_1";
constexpr const char* kStreamTwo = "2010096_spatial_roi_roi_2";

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

roi::RecorderRuntimeConfig fixture()
{
    roi::RecorderRuntimeConfig config;
    config.recorder_gpu_by_logical_stream_id.emplace(kStreamOne, 1);
    config.recorder_gpu_by_logical_stream_id.emplace(kStreamTwo, 2);
    return config;
}

json fixture_json()
{
    json value;
    std::string error;
    require(roi::serialize_recorder_runtime_config(fixture(), &value, &error),
            "fixture serialization failed: " + error);
    return value;
}

void round_trip_preserves_closed_value()
{
    const json serialized = fixture_json();
    require(serialized.size() == 4,
            "schema v1 serializer must emit exactly four fields");
    require(serialized.at("schema_id") == roi::kRecorderRuntimeConfigSchemaId,
            "schema ID mismatch");
    require(serialized.at("schema_version") ==
                roi::kRecorderRuntimeConfigSchemaVersion,
            "schema version mismatch");
    require(serialized.at("mode") ==
                roi::kRecorderRuntimeModeExplicitPerStream,
            "runtime mode mismatch");
    require(serialized.at("recorder_gpu_by_logical_stream_id").size() == 2 &&
                serialized.at("recorder_gpu_by_logical_stream_id").at(kStreamOne) ==
                    1 &&
                serialized.at("recorder_gpu_by_logical_stream_id").at(kStreamTwo) ==
                    2,
            "runtime GPU map mismatch");

    roi::RecorderRuntimeConfig parsed;
    std::string error = "stale";
    require(roi::parse_recorder_runtime_config(serialized, &parsed, &error),
            "round-trip parse failed: " + error);
    require(error.empty(), "successful parse must clear stale error text");
    require(parsed.mode == fixture().mode &&
                parsed.recorder_gpu_by_logical_stream_id ==
                    fixture().recorder_gpu_by_logical_stream_id,
            "round-trip config mismatch");
}

void closed_envelope_rejects_missing_and_unknown_fields()
{
    std::string error;
    roi::RecorderRuntimeConfig parsed;

    json missing = fixture_json();
    missing.erase("mode");
    require(!roi::parse_recorder_runtime_config(missing, &parsed, &error) &&
                error.find("mode") != std::string::npos,
            "missing mode must fail closed");

    json unknown = fixture_json();
    unknown["encode_profile"] = json::object();
    require(!roi::parse_recorder_runtime_config(unknown, &parsed, &error) &&
                error.find("encode_profile") != std::string::npos,
            "unknown encode profile must fail the placement-only schema");

    require(!roi::parse_recorder_runtime_config(json::array(), &parsed, &error),
            "non-object root must fail");
}

void schema_and_mode_are_exact()
{
    std::string error;
    roi::RecorderRuntimeConfig parsed;

    json candidate = fixture_json();
    candidate["schema_id"] = "orange.spatial_roi_recording.runtime";
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error),
            "wrong schema ID must fail");

    candidate = fixture_json();
    candidate["schema_version"] = 2;
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error),
            "future schema version must fail");

    candidate = fixture_json();
    candidate["schema_version"] = 1.0;
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error),
            "floating-point schema version must fail");

    candidate = fixture_json();
    candidate["mode"] = "source_gpu";
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error) &&
                error.find("explicit_per_stream") != std::string::npos,
            "non-explicit mode must fail");
}

void map_must_be_nonempty_object()
{
    std::string error;
    roi::RecorderRuntimeConfig parsed;

    json candidate = fixture_json();
    candidate["recorder_gpu_by_logical_stream_id"] = json::array();
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error),
            "array placement must fail");

    candidate = fixture_json();
    candidate["recorder_gpu_by_logical_stream_id"] = json::object();
    require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error) &&
                error.find("nonempty") != std::string::npos,
            "empty explicit placement must fail");
}

void logical_stream_ids_must_be_safe()
{
    std::string error;
    roi::RecorderRuntimeConfig parsed;
    const std::string overlong(65, 'a');
    for (const std::string& unsafe :
         {std::string(), std::string("_roi"), std::string("roi/1"),
          std::string("roi 1"), overlong, std::string("roi\xC3\xA9")}) {
        json candidate = fixture_json();
        candidate["recorder_gpu_by_logical_stream_id"] =
            json::object({{unsafe, 1}});
        require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error),
                "unsafe logical stream ID must fail");
    }

    json boundary = fixture_json();
    boundary["recorder_gpu_by_logical_stream_id"] =
        json::object({{std::string(64, 'a'), 255}});
    require(roi::parse_recorder_runtime_config(boundary, &parsed, &error),
            "64-byte safe stream ID and GPU 255 must be accepted: " + error);
}

void gpu_ids_must_be_bounded_integers()
{
    std::string error;
    roi::RecorderRuntimeConfig parsed;
    for (const json& invalid :
         {json(-1), json(256), json(1.0), json(true), json("1"), json(nullptr)}) {
        json candidate = fixture_json();
        candidate["recorder_gpu_by_logical_stream_id"][kStreamOne] = invalid;
        require(!roi::parse_recorder_runtime_config(candidate, &parsed, &error) &&
                    error.find("[0,255]") != std::string::npos,
                "invalid GPU ID must fail with the bounded-integer contract");
    }

    json zero = fixture_json();
    zero["recorder_gpu_by_logical_stream_id"][kStreamOne] = 0;
    require(roi::parse_recorder_runtime_config(zero, &parsed, &error),
            "GPU zero must be accepted: " + error);
}

void failures_do_not_mutate_destinations()
{
    roi::RecorderRuntimeConfig parsed;
    parsed.mode = "sentinel";
    parsed.recorder_gpu_by_logical_stream_id.emplace("sentinel", 7);
    std::string error;

    json invalid = fixture_json();
    invalid["mode"] = "invalid";
    require(!roi::parse_recorder_runtime_config(invalid, &parsed, &error),
            "invalid parse must fail");
    require(parsed.mode == "sentinel" &&
                parsed.recorder_gpu_by_logical_stream_id ==
                    std::map<std::string, int>{{"sentinel", 7}},
            "failed parse mutated destination");

    json output = {{"sentinel", true}};
    roi::RecorderRuntimeConfig invalid_config = fixture();
    invalid_config.recorder_gpu_by_logical_stream_id.clear();
    require(!roi::serialize_recorder_runtime_config(
                invalid_config, &output, &error),
            "invalid serialization must fail");
    require(output == json({{"sentinel", true}}),
            "failed serialization mutated destination");
}

void null_destinations_fail_cleanly()
{
    std::string error;
    require(!roi::parse_recorder_runtime_config(fixture_json(), nullptr, &error) &&
                error.find("destination is null") != std::string::npos,
            "null parse destination must fail cleanly");
    require(!roi::serialize_recorder_runtime_config(fixture(), nullptr, &error) &&
                error.find("destination is null") != std::string::npos,
            "null serialization destination must fail cleanly");
}

}  // namespace

int main()
{
    round_trip_preserves_closed_value();
    closed_envelope_rejects_missing_and_unknown_fields();
    schema_and_mode_are_exact();
    map_must_be_nonempty_object();
    logical_stream_ids_must_be_safe();
    gpu_ids_must_be_bounded_integers();
    failures_do_not_mutate_destinations();
    null_destinations_fail_cleanly();
    return 0;
}
