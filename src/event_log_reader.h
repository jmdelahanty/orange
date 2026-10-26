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
#include "timing/written_file_digest.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

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

enum class EventLogProduct { kDetections, kPose };

// One line (without its newline) of Cam<serial>_yolo_events.jsonl (kDetections) or
// Cam<serial>_pose_events.jsonl (kPose), counted the way recording_session.json
// realtime_products declares the log: every line goes through the generated parser; a line
// that is not JSON or does not match the schema is a parse error, never a row; the first
// line's schema_id / schema_version are what the log declares. The event loggers count each
// line as they write it (on their own thread) and realtime_products counts each line when it
// has to re-read a log, so a log is counted the same way whichever did it.
inline void tally_event_line(EventLogProduct product, const std::string& line,
                             orange::timing::EventLogTally* tally)
{
    if (line.empty()) return;
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    if (!tally->first_line_seen) {
        tally->first_line_seen = true;
        if (parsed.is_object()) {
            const auto id = parsed.find("schema_id");
            if (id != parsed.end() && id->is_string()) tally->schema_id = id->get<std::string>();
            const auto version = parsed.find("schema_version");
            if (version != parsed.end() && version->is_number()) tally->schema_version = version->get<int>();
        }
    }
    auto frame_line = [tally](uint64_t frame_id, const std::string& status) {
        tally->row_count++;
        if (frame_id > 0) {
            if (tally->first_recording_frame_id == 0 || frame_id < tally->first_recording_frame_id) {
                tally->first_recording_frame_id = frame_id;
            }
            tally->last_recording_frame_id = std::max(tally->last_recording_frame_id, frame_id);
        }
        tally->rows_by_status[status]++;
    };
    std::string error;
    if (parsed.is_discarded()) {
        tally->parse_errors++;
    } else if (product == EventLogProduct::kDetections) {
        yolo::OrangeYoloEventV3 event;
        if (!yolo::parse(parsed, &event, &error)) {
            tally->parse_errors++;
        } else if (const auto* result = std::get_if<yolo::OrangeYoloEventV3YoloResult>(&event)) {
            frame_line(result->frame.recording_frame_id, result->yolo.status);
        } else {
            tally->header_rows++;  // session_header, spatial_mask_policy
        }
    } else {
        pose::OrangePoseEventV3 event;
        if (!pose::parse(parsed, &event, &error)) {
            tally->parse_errors++;
        } else if (const auto* result = std::get_if<pose::OrangePoseEventV3PoseResult>(&event)) {
            frame_line(result->frame.recording_frame_id, result->pose.status);
        } else {
            tally->header_rows++;  // session_header
        }
    }
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
