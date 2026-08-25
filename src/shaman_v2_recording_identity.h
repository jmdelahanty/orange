#pragma once

#include "gui/spatial_layout/sha256.h"

#include <nlohmann/json.hpp>

#include <string>

namespace orange::shaman_v2_recording_identity {

inline constexpr const char* kSchemaId =
    "orange.shaman_v2.recording_identity";
inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr const char* kCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kScope = "recording_session";

inline nlohmann::json token_subject(const std::string& recording_id)
{
    return {
        {"canonicalization", kCanonicalization},
        {"recording_id", recording_id},
        {"schema_id", kSchemaId},
        {"schema_version", kSchemaVersion},
        {"scope", kScope},
    };
}

inline std::string token_for_recording_id(const std::string& recording_id)
{
    if (recording_id.empty()) {
        return {};
    }
    const std::string canonical = token_subject(recording_id).dump(
        -1, ' ', false, nlohmann::json::error_handler_t::strict);
    return "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(canonical);
}

inline nlohmann::json binding_record(const std::string& recording_id)
{
    nlohmann::json record = token_subject(recording_id);
    record["recording_identity_token"] = token_for_recording_id(recording_id);
    return record;
}

} // namespace orange::shaman_v2_recording_identity
