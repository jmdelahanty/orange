// Per-camera realtime (detection and pose) products of a recording folder,
// declared in recording_session.json so an intake can bind each camera's
// event logs and perf files to that camera (and to the model that produced
// them) by declaration rather than by file name. Palette request 2026-10-07:
// path, size, sha256, line schema, frame identity key, row counts by kind,
// first/last recording_frame_id, model reference, pose crop size; perf CSVs
// as plain files with their owning product.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace orange::session {

inline constexpr const char* kRealtimeProductsSchemaId = "orange.recording_realtime_products";
inline constexpr int kRealtimeProductsSchemaVersion = 1;

// Scans Cam<serial>_yolo_events.jsonl / Cam<serial>_pose_events.jsonl and the
// per-camera CSV diagnostics under `recording_folder`, reads the model blocks
// from recording_snapshot.json, and returns the block for the manifest. Files
// that do not exist are declared absent; nothing is written. Paths are
// relative to the recording folder.
nlohmann::json build_realtime_products_json(const std::string& recording_folder,
                                            const std::vector<std::string>& camera_serials);

}  // namespace orange::session
