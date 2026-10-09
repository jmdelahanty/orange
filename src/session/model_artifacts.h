#pragma once

// Model artifacts materialized into the recording folder (2026-10-09, Palette
// intake item 7): for every distinct engine that ran (models.<serial>.detect /
// .pose in the recording snapshot) the engine manifest, and for INT8 engines
// the calibration record and the trtexec calibration cache, are copied to
// `models/<engine_sha256>/` inside the recording folder and declared in
// `recording_session.json.model_artifacts` (schema
// orange.recording_model_artifacts v1) with path, size and sha256. One copy per
// engine sha256; a camera's models_key names which engine it used. Runs on the
// finalization path only (small files, never on a hot thread). The calibration
// frames stay on the rig.

#include <string>
#include <vector>

#include "json.hpp"

namespace orange::session {

// `models` is the snapshot's models block (models.<serial>.<kind>). Copies the
// files and returns the declaration; missing manifests are declared with
// status "absent" and nothing is copied for them.
nlohmann::json materialize_model_artifacts(const std::string& recording_folder,
                                           const nlohmann::json& models);

}  // namespace orange::session
