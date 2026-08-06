#include "headless_recording_profile.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

namespace orange::headless {
namespace {

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool read_required_string(const nlohmann::json& value,
                          const char* key,
                          std::string* out,
                          std::string* error_out,
                          const std::string& context)
{
    if (!value.contains(key) || !value[key].is_string() ||
        value[key].get<std::string>().empty()) {
        return fail(error_out, context + "." + key + " must be a non-empty string");
    }
    *out = lower(value[key].get<std::string>());
    return true;
}

bool read_required_positive_int(const nlohmann::json& value,
                                const char* key,
                                int* out,
                                std::string* error_out,
                                const std::string& context)
{
    if (!value.contains(key) ||
        (!value[key].is_number_integer() && !value[key].is_number_unsigned())) {
        return fail(error_out, context + "." + key + " must be a positive integer");
    }
    const long long parsed = value[key].get<long long>();
    if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
        return fail(error_out, context + "." + key + " must be a positive integer");
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool parse_toggle(const nlohmann::json& value,
                  int* out,
                  std::string* error_out,
                  const std::string& context)
{
    if (value.is_boolean()) {
        *out = value.get<bool>() ? 1 : 0;
        return true;
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const int parsed = value.get<int>();
        if (parsed == -1 || parsed == 0 || parsed == 1) {
            *out = parsed;
            return true;
        }
    }
    if (value.is_string()) {
        const std::string parsed = lower(value.get<std::string>());
        if (parsed == "off" || parsed == "false" || parsed == "disabled" || parsed == "0") {
            *out = 0;
            return true;
        }
        if (parsed == "on" || parsed == "true" || parsed == "enabled" || parsed == "1") {
            *out = 1;
            return true;
        }
        if (parsed == "auto" || parsed == "default" || parsed == "inherit") {
            *out = -1;
            return true;
        }
    }
    return fail(error_out, context + " must be off, on, or auto");
}

bool read_required_toggle(const nlohmann::json& value,
                          const char* key,
                          int* out,
                          std::string* error_out,
                          const std::string& context)
{
    if (!value.contains(key)) {
        return fail(error_out, context + "." + key + " is required");
    }
    return parse_toggle(value[key], out, error_out, context + "." + key);
}

bool read_required_rate(const nlohmann::json& value,
                        const char* key,
                        int* out,
                        std::string* error_out,
                        const std::string& context)
{
    if (!value.contains(key)) {
        return fail(error_out, context + "." + key + " is required");
    }
    if (value[key].is_string()) {
        const std::string parsed = lower(value[key].get<std::string>());
        if (parsed == "auto" || parsed == "default" || parsed == "inherit") {
            *out = -1;
            return true;
        }
    }
    return read_required_positive_int(value, key, out, error_out, context);
}

std::string toggle_json_value(const int value)
{
    if (value == 0) {
        return "off";
    }
    if (value == 1) {
        return "on";
    }
    return "auto";
}

nlohmann::json rate_json_value(const int value)
{
    return value > 0 ? nlohmann::json(value) : nlohmann::json("auto");
}

}  // namespace

bool ParseRecordingProfile(const nlohmann::json& value,
                           RecordingProfile* profile_out,
                           std::string* error_out,
                           const std::string& context)
{
    if (error_out) {
        error_out->clear();
    }
    if (!profile_out) {
        return fail(error_out, context + ": internal error: null profile destination");
    }
    if (!value.is_object()) {
        return fail(error_out, context + " must be a JSON object");
    }

    static const std::set<std::string> allowed = {
        "codec", "preset", "tuning", "rate_control_mode", "quality_value",
        "gop_length", "aq", "temporal_aq", "lookahead", "lookahead_depth",
        "target_bitrate_bps", "max_bitrate_bps", "vbv_buffer_size",
        "importance_map",
    };
    for (const auto& item : value.items()) {
        if (allowed.count(item.key()) == 0) {
            return fail(error_out, context + " contains unsupported field: " + item.key());
        }
    }

    RecordingProfile profile;
    if (!read_required_string(value, "codec", &profile.codec, error_out, context) ||
        !read_required_string(value, "preset", &profile.preset, error_out, context) ||
        !read_required_string(value, "tuning", &profile.tuning, error_out, context) ||
        !read_required_string(
            value, "rate_control_mode", &profile.rate_control_mode, error_out, context) ||
        !read_required_positive_int(
            value, "quality_value", &profile.quality_value, error_out, context) ||
        !read_required_positive_int(
            value, "gop_length", &profile.gop_length, error_out, context) ||
        !read_required_toggle(
            value, "aq", &profile.control.aq, error_out, context) ||
        !read_required_toggle(
            value, "temporal_aq", &profile.control.temporal_aq, error_out, context) ||
        !read_required_toggle(
            value, "lookahead", &profile.control.lookahead, error_out, context) ||
        !read_required_rate(
            value, "target_bitrate_bps", &profile.control.target_bitrate_bps,
            error_out, context) ||
        !read_required_rate(
            value, "max_bitrate_bps", &profile.control.max_bitrate_bps,
            error_out, context) ||
        !read_required_rate(
            value, "vbv_buffer_size", &profile.control.vbv_buffer_size,
            error_out, context)) {
        return false;
    }

    if (!value.contains("lookahead_depth") ||
        (!value["lookahead_depth"].is_number_integer() &&
         !value["lookahead_depth"].is_number_unsigned())) {
        return fail(error_out, context + ".lookahead_depth must be a non-negative integer");
    }
    const long long parsed_lookahead_depth =
        value["lookahead_depth"].get<long long>();
    if (parsed_lookahead_depth < 0 || parsed_lookahead_depth > 32) {
        return fail(error_out, context + ".lookahead_depth must be within [0, 32]");
    }
    profile.control.lookahead_depth = static_cast<int>(parsed_lookahead_depth);

    if (profile.codec != "h264" && profile.codec != "hevc") {
        return fail(error_out, context + ".codec must be h264 or hevc");
    }
    if (profile.preset != "p1" && profile.preset != "p3" &&
        profile.preset != "p5" && profile.preset != "p7") {
        return fail(error_out, context + ".preset must be p1, p3, p5, or p7");
    }
    if (profile.tuning != "ll" && profile.tuning != "ull" &&
        profile.tuning != "hq" && profile.tuning != "lossless") {
        return fail(error_out, context + ".tuning must be ll, ull, hq, or lossless");
    }
    if (profile.rate_control_mode != "vbr" &&
        profile.rate_control_mode != "vbr_cq" &&
        profile.rate_control_mode != "cbr" &&
        profile.rate_control_mode != "cqp") {
        return fail(error_out, context +
            ".rate_control_mode must be vbr, vbr_cq, cbr, or cqp");
    }
    if (profile.quality_value > 51) {
        return fail(error_out, context + ".quality_value must be within [1, 51]");
    }
    if (profile.control.aq < 0 || profile.control.temporal_aq < 0 ||
        profile.control.lookahead < 0) {
        return fail(error_out,
            context + ".aq, temporal_aq, and lookahead must be explicitly on or off");
    }
    if (profile.control.lookahead == 0 && profile.control.lookahead_depth != 0) {
        return fail(error_out,
            context + ".lookahead_depth must be 0 when lookahead is off");
    }
    if (profile.control.lookahead == 1 && profile.control.lookahead_depth == 0) {
        return fail(error_out,
            context + ".lookahead_depth must be positive when lookahead is on");
    }
    if (profile.control.target_bitrate_bps > 0 &&
        profile.control.max_bitrate_bps > 0 &&
        profile.control.max_bitrate_bps < profile.control.target_bitrate_bps) {
        return fail(error_out, context +
            ".max_bitrate_bps must be greater than or equal to target_bitrate_bps");
    }
    if ((profile.rate_control_mode == "vbr" ||
         profile.rate_control_mode == "vbr_cq" ||
         profile.rate_control_mode == "cbr") &&
        (profile.control.target_bitrate_bps <= 0 ||
         profile.control.max_bitrate_bps <= 0 ||
         profile.control.vbv_buffer_size <= 0)) {
        return fail(error_out, context +
            " requires explicit positive target/max/VBV values for bitrate-controlled modes");
    }

    if (!value.contains("importance_map")) {
        return fail(error_out, context + ".importance_map is required");
    }
    profile.importance_map.mode = "off";
    profile.importance_map.roi_size_px = ImportanceMapConfig::kDefaultRoiSizePx;
    if (value.contains("importance_map")) {
        const nlohmann::json& map = value["importance_map"];
        if (!map.is_object()) {
            return fail(error_out, context + ".importance_map must be a JSON object");
        }
        for (const auto& item : map.items()) {
            if (item.key() != "mode" && item.key() != "roi_size_px") {
                return fail(error_out, context +
                    ".importance_map contains unsupported field: " + item.key());
            }
        }
        if (!map.contains("mode") || !map["mode"].is_string()) {
            return fail(error_out, context + ".importance_map.mode must be a string");
        }
        profile.importance_map.mode = lower(map["mode"].get<std::string>());
        if (profile.importance_map.mode != "off" &&
            profile.importance_map.mode != "static_roi") {
            return fail(error_out, context +
                ".importance_map.mode must be off or static_roi");
        }
        if (map.contains("roi_size_px")) {
            if (!read_required_positive_int(
                    map, "roi_size_px", &profile.importance_map.roi_size_px,
                    error_out, context + ".importance_map")) {
                return false;
            }
        }
    }

    *profile_out = std::move(profile);
    return true;
}

nlohmann::json BuildRecordingProfileJson(const RecordingProfile& profile)
{
    return {
        {"codec", profile.codec},
        {"preset", profile.preset},
        {"tuning", profile.tuning},
        {"rate_control_mode", profile.rate_control_mode},
        {"quality_value", profile.quality_value},
        {"gop_length", profile.gop_length},
        {"aq", toggle_json_value(profile.control.aq)},
        {"temporal_aq", toggle_json_value(profile.control.temporal_aq)},
        {"lookahead", toggle_json_value(profile.control.lookahead)},
        {"lookahead_depth", profile.control.lookahead_depth},
        {"target_bitrate_bps", rate_json_value(profile.control.target_bitrate_bps)},
        {"max_bitrate_bps", rate_json_value(profile.control.max_bitrate_bps)},
        {"vbv_buffer_size", rate_json_value(profile.control.vbv_buffer_size)},
        {"importance_map", {
            {"mode", profile.importance_map.mode},
            {"roi_size_px", profile.importance_map.roi_size_px},
        }},
    };
}

}  // namespace orange::headless
