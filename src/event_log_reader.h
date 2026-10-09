#pragma once

// Reading the per-frame JSONL event logs (orange.yolo_event / orange.pose_event)
// in both line formats. Version 1 repeats every session-constant block on
// every frame line; version 2 (2026-10-09, see src/event_log_format.h) writes
// one `session_header` line first and, for the detector log, a
// `spatial_mask_policy` line whenever the policy generation changes. Readers
// feed every line through `absorb_non_frame_line`; frame lines are then
// viewed through the `effective_*` helpers, which merge the header's blocks
// under the line's own so v1 and v2 look the same.

#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace event_log_reader {

struct SessionContext {
    bool has_header = false;
    nlohmann::json header = nlohmann::json::object();
    // spatial_mask policies by policy_generation (header + policy lines).
    std::map<uint64_t, nlohmann::json> spatial_mask_policies;
    uint64_t header_lines = 0;
    uint64_t policy_lines = 0;
};

inline bool is_frame_event_kind(const std::string& kind)
{
    return kind == "yolo_result" || kind == "pose_result";
}

inline uint64_t u64_or_zero(const nlohmann::json& value)
{
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) {
        const int64_t v = value.get<int64_t>();
        return v < 0 ? 0 : static_cast<uint64_t>(v);
    }
    return 0;
}

// Returns true when the line is not a frame line (header / policy /
// unknown kind) and has been folded into the context.
inline bool absorb_non_frame_line(const nlohmann::json& event, SessionContext* ctx)
{
    if (!event.is_object()) return false;
    const std::string kind = event.value("event_kind", std::string());
    if (is_frame_event_kind(kind)) return false;
    if (kind == "session_header") {
        ctx->has_header = true;
        ctx->header = event;
        ctx->header_lines++;
        const auto it = event.find("spatial_mask");
        if (it != event.end() && it->is_object()) {
            ctx->spatial_mask_policies[u64_or_zero(it->value("policy_generation", nlohmann::json()))] = *it;
        }
        return true;
    }
    if (kind == "spatial_mask_policy") {
        ctx->policy_lines++;
        const auto it = event.find("spatial_mask");
        if (it != event.end() && it->is_object()) {
            ctx->spatial_mask_policies[u64_or_zero(it->value("policy_generation", nlohmann::json()))] = *it;
        }
        return true;
    }
    return true;
}

// Top-level string: the line's own value, else the header's.
inline std::string effective_string(const nlohmann::json& event, const SessionContext& ctx, const char* key)
{
    const auto it = event.find(key);
    if (it != event.end() && it->is_string()) return it->get<std::string>();
    const auto hit = ctx.header.find(key);
    if (hit != ctx.header.end() && hit->is_string()) return hit->get<std::string>();
    return {};
}

// A block (`yolo` / `pose`): the header's block with the line's keys on top.
inline nlohmann::json effective_block(const nlohmann::json& event, const SessionContext& ctx, const char* block)
{
    nlohmann::json out = nlohmann::json::object();
    const auto hit = ctx.header.find(block);
    if (hit != ctx.header.end() && hit->is_object()) out = *hit;
    const auto it = event.find(block);
    if (it != event.end() && it->is_object()) {
        for (auto kv = it->begin(); kv != it->end(); ++kv) out[kv.key()] = kv.value();
    }
    return out;
}

// The spatial mask as v1 wrote it: policy (by the line's policy_generation)
// plus the line's result / outside_detections.
inline nlohmann::json effective_spatial_mask(const nlohmann::json& event, const SessionContext& ctx, bool* found)
{
    if (found) *found = false;
    const auto it = event.find("spatial_mask");
    if (it == event.end()) return nlohmann::json();
    if (found) *found = true;
    if (!it->is_object()) return *it;
    nlohmann::json out = nlohmann::json::object();
    const uint64_t generation = u64_or_zero(it->value("policy_generation", nlohmann::json()));
    const auto policy = ctx.spatial_mask_policies.find(generation);
    if (policy != ctx.spatial_mask_policies.end()) out = policy->second;
    for (auto kv = it->begin(); kv != it->end(); ++kv) out[kv.key()] = kv.value();
    return out;
}

}  // namespace event_log_reader
