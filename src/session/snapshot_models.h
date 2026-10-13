#pragma once

// Reading the recording snapshot's models block (models.<serial>.detect / .pose) through the
// parsers generated from the published orange.recording_detect_model.v1 and
// orange.recording_pose_model.v2 (docs/schemas, tools/schema_codegen). The block has no
// schema_id of its own; build_gui_detect_model_snapshot / build_gui_pose_model_snapshot write it
// with the same generated code. A block that does not match its schema is left out and reported,
// never read field by field into defaults.

#include "json.hpp"
#include "orange_outputs/orange_recording_detect_model_v1.h"
#include "orange_outputs/orange_recording_pose_model_v2.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orange::session {

namespace detect_model = orange::outputs::gen::recording_detect_model_v1;
namespace pose_model = orange::outputs::gen::recording_pose_model_v2;

struct CameraModels {
    std::optional<detect_model::OrangeRecordingDetectModelV1> detect;
    std::optional<pose_model::OrangeRecordingPoseModelV2> pose;
};

// Camera serial -> its parsed blocks; ordered by serial like the JSON object.
using SnapshotModels = std::map<std::string, CameraModels>;

// Parse a snapshot's `models` value. Blocks that do not match their schema are left out, with
// "models.<serial>.<kind>: <parser message>" appended to *errors (when given).
inline SnapshotModels parse_snapshot_models(const nlohmann::json& models, std::vector<std::string>* errors)
{
    SnapshotModels out;
    if (!models.is_object()) {
        if (errors && !models.is_null()) errors->push_back("models: expected an object");
        return out;
    }
    for (auto camera = models.begin(); camera != models.end(); ++camera) {
        const std::string key = "models." + camera.key();
        if (!camera.value().is_object()) {
            if (errors) errors->push_back(key + ": expected an object");
            continue;
        }
        CameraModels& parsed = out[camera.key()];
        std::string error;
        if (const auto it = camera.value().find("detect"); it != camera.value().end()) {
            detect_model::OrangeRecordingDetectModelV1 block;
            if (detect_model::parse(*it, &block, &error, key + ".detect")) {
                parsed.detect = std::move(block);
            } else if (errors) {
                errors->push_back(error);
            }
        }
        if (const auto it = camera.value().find("pose"); it != camera.value().end()) {
            pose_model::OrangeRecordingPoseModelV2 block;
            if (pose_model::parse(*it, &block, &error, key + ".pose")) {
                parsed.pose = std::move(block);
            } else if (errors) {
                errors->push_back(error);
            }
        }
    }
    return out;
}

// The models block of a snapshot file (recording_snapshot_start.json or recording_snapshot.json);
// empty when the file is missing or not JSON.
inline SnapshotModels load_snapshot_models(const std::filesystem::path& snapshot_file,
                                           std::vector<std::string>* errors)
{
    std::ifstream in(snapshot_file);
    if (!in) return {};
    const nlohmann::json snapshot = nlohmann::json::parse(in, nullptr, false);
    if (snapshot.is_discarded() || !snapshot.is_object()) {
        if (errors) errors->push_back(snapshot_file.string() + ": not a JSON object");
        return {};
    }
    const auto it = snapshot.find("models");
    return it == snapshot.end() ? SnapshotModels{} : parse_snapshot_models(*it, errors);
}

}  // namespace orange::session
