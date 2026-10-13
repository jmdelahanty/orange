#pragma once

// Reading the per-frame JSONL event logs (orange.yolo_event / orange.pose_event v3) through the
// generated parsers (tools/schema_codegen, docs/schemas/orange_{yolo,pose}_event_v3.schema.json).
// Each line is one variant of the log's line kinds; a line that does not match the published
// schema exactly (keys, types, enums, consts, schema_id / version) is refused with the parser's
// message. A detector log carries a session_header first and a spatial_mask_policy line whenever
// the policy generation changes; YoloSession keeps the policies by generation, parsed with the
// generated reader of orange.analytics.spatial_mask_runtime v1, so a frame line's spatial_mask
// can be read with its policy.

#include "json.hpp"
#include "orange_outputs/orange_analytics_spatial_mask_runtime_v1.h"
#include "orange_outputs/orange_pose_event_v3.h"
#include "orange_outputs/orange_yolo_event_v3.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace event_log_reader {

namespace yolo = orange::outputs::gen::yolo_event_v3;
namespace pose = orange::outputs::gen::pose_event_v3;

enum class LineStatus { kOk, kInvalidJson, kSchemaMismatch };

inline LineStatus parse_yolo_line(const std::string& text, yolo::OrangeYoloEventV3* out, std::string* error)
{
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return LineStatus::kInvalidJson;
    return yolo::parse(parsed, out, error) ? LineStatus::kOk : LineStatus::kSchemaMismatch;
}

inline LineStatus parse_pose_line(const std::string& text, pose::OrangePoseEventV3* out, std::string* error)
{
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return LineStatus::kInvalidJson;
    return pose::parse(parsed, out, error) ? LineStatus::kOk : LineStatus::kSchemaMismatch;
}

namespace mask = orange::outputs::gen::spatial_mask_runtime_v1;

// The detector log's session state: its header and the spatial mask policies by generation,
// each read with the generated parser of orange.analytics.spatial_mask_runtime v1.
struct YoloSession {
    std::optional<yolo::OrangeYoloEventV3SessionHeader> header;
    std::map<uint64_t, mask::OrangeAnalyticsSpatialMaskRuntimeV1> spatial_mask_policies;
    // Generations whose policy does not match the published schema, with the parser's message.
    std::map<uint64_t, std::string> policy_errors;

    void Absorb(const yolo::OrangeYoloEventV3SessionHeader& line)
    {
        header = line;
        AbsorbPolicy(line.spatial_mask.policy_generation, yolo::to_json(line.spatial_mask));
    }
    void Absorb(const yolo::OrangeYoloEventV3SpatialMaskPolicy& line)
    {
        AbsorbPolicy(line.spatial_mask.policy_generation, yolo::to_json(line.spatial_mask));
    }
    // The policy a frame line names, or nullptr when no header / policy line declared a valid one.
    const mask::OrangeAnalyticsSpatialMaskRuntimeV1* Policy(const yolo::OrangeYoloEventV3YoloResult& line) const
    {
        const auto it = spatial_mask_policies.find(line.spatial_mask.policy_generation);
        return it == spatial_mask_policies.end() ? nullptr : &it->second;
    }

private:
    void AbsorbPolicy(uint64_t generation, const nlohmann::json& policy)
    {
        mask::OrangeAnalyticsSpatialMaskRuntimeV1 parsed;
        std::string error;
        spatial_mask_policies.erase(generation);
        policy_errors.erase(generation);
        if (mask::parse(policy, &parsed, &error, "/spatial_mask")) {
            spatial_mask_policies.emplace(generation, std::move(parsed));
        } else {
            policy_errors.emplace(generation, error);
        }
    }
};

}  // namespace event_log_reader
