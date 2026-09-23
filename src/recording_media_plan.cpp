#include "recording_media_plan.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace orange::recording {
using json = nlohmann::json;
const char* RecordingMediaModeName(RecordingMediaMode mode) {
    switch (mode) {
        case RecordingMediaMode::FullFrame: return "full_frame";
        case RecordingMediaMode::FullFrameAndMovingCrops: return "full_frame_and_moving_crops";
        case RecordingMediaMode::RegisteredContextAndMovingCrops: return "registered_context_and_moving_crops";
    }
    throw std::runtime_error("invalid recording media mode");
}
RecordingMediaSelection RecordingMediaSelection::Parse(const json& j) {
    if (j.is_null()) return {};
    if (!j.is_object() || j.size() != 2 || !j.contains("schema_version") ||
        !j.at("schema_version").is_number_integer() || j.at("schema_version") != 1 ||
        !j.contains("mode") || !j.at("mode").is_string())
        throw std::runtime_error("recording media_products requires exactly schema_version 1 and mode");
    for (auto mode : {RecordingMediaMode::FullFrame, RecordingMediaMode::FullFrameAndMovingCrops,
                      RecordingMediaMode::RegisteredContextAndMovingCrops})
        if (j.at("mode") == RecordingMediaModeName(mode)) return {mode};
    throw std::runtime_error("unknown recording media_products mode");
}
json RecordingMediaSelection::ToJson() const {
    if (!mode) return nullptr;
    return {{"schema_version", 1}, {"mode", RecordingMediaModeName(*mode)}};
}
bool RecordingMediaSelection::FullFrame() const {
    return mode != RecordingMediaMode::RegisteredContextAndMovingCrops;
}
bool RecordingMediaSelection::MovingCrops(bool existing) const {
    return mode ? *mode != RecordingMediaMode::FullFrame : existing;
}
bool RecordingMediaSelection::RequiresContext() const { return !FullFrame(); }
bool RecordingMediaCameraPlan::FullFrame() const { return mode != RecordingMediaMode::RegisteredContextAndMovingCrops; }
bool RecordingMediaCameraPlan::MovingCrops() const { return mode != RecordingMediaMode::FullFrame; }
RecordingMediaPlan RecordingMediaPlan::Resolve(const RecordingMediaSelection& selection,
        const std::vector<RecordingMediaCameraInput>& inputs) {
    RecordingMediaPlan plan;
    plan.selection = RecordingMediaSelection::Parse(selection.ToJson());
    std::set<std::string> seen;
    for (const auto& c : inputs) {
        if (!c.participates) continue;
        if (c.serial.empty() || !seen.insert(c.serial).second)
            throw std::runtime_error("recording media plan requires unique nonempty camera serials");
        const auto mode = selection.mode.value_or(c.moving_crops
            ? RecordingMediaMode::FullFrameAndMovingCrops : RecordingMediaMode::FullFrame);
        plan.cameras.push_back({c.serial, mode});
    }
    return plan;
}
bool RecordingMediaPlan::HasFullFrame() const {
    return std::any_of(cameras.begin(), cameras.end(), [](const auto& c) { return c.FullFrame(); });
}
bool RecordingMediaPlan::HasMovingCrops() const {
    return std::any_of(cameras.begin(), cameras.end(), [](const auto& c) { return c.MovingCrops(); });
}
bool RecordingMediaPlan::RequiresContext() const {
    return std::any_of(cameras.begin(), cameras.end(), [](const auto& c) { return !c.FullFrame(); });
}
const RecordingMediaCameraPlan* RecordingMediaPlan::Find(const std::string& serial) const {
    const auto it = std::find_if(cameras.begin(), cameras.end(), [&](const auto& c) { return c.serial == serial; });
    return it == cameras.end() ? nullptr : &*it;
}
json RecordingMediaPlan::ToJson() const {
    json result = {{"schema_id", "orange.recording.media_plan"}, {"schema_version", 1},
        {"selection", selection.ToJson()}, {"cameras", json::array()}};
    for (const auto& c : cameras) {
        json outputs = json::array();
        if (c.FullFrame()) outputs.push_back("full_frame_video");
        if (c.MovingCrops()) outputs.push_back("moving_crop_video");
        if (!c.FullFrame()) outputs.push_back("registered_context_image");
        result["cameras"].push_back({{"camera_serial", c.serial}, {"mode", RecordingMediaModeName(c.mode)},
            {"media_products", outputs}, {"master_frame_record_required", !c.FullFrame()}});
    }
    return result;
}
void RequireCropOnlyRecordingInputs(const RecordingMediaSelection& selection, bool master_enabled,
        bool context_enabled, bool real_full_rate_detector, bool external_moving_crops) {
    if (!selection.RequiresContext()) return;
    if (!master_enabled || !context_enabled || !real_full_rate_detector || !external_moving_crops)
        throw std::runtime_error("crop-only recording requires registered_scene_context, an independent master_frame_journal, "
            "real full-rate YOLO with event logging, and supervised external moving crops");
}
} // namespace orange::recording
