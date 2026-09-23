#pragma once
#include "json.hpp"
#include <filesystem>

namespace orange::recording {
inline constexpr const char* kCropOnlyProduct = "registered_context_and_moving_crops";
bool IsCropOnlyRecordingManifest(const nlohmann::json&);
// Read-only control-plane checks. No index or manifest is published/repaired.
nlohmann::json ReadVerifiedCropOnlyRecordingManifest(const std::filesystem::path& root);
void RequireCropOnlyArmEvidence(const std::filesystem::path& root, const nlohmann::json& media_plan);

// Finalization-only. The caller supplies a lifecycle envelope with logical
// cameras and no media artifacts. Resolves media exclusively from immutable
// start membership, complete master/context evidence and validated crop receipts.
// Incomplete evidence yields a failed parent with no fabricated media inventory.
nlohmann::json BuildCropOnlyRecordingManifest(const std::filesystem::path& root,
                                             const nlohmann::json& lifecycle);

// Writes immutable, idempotent parent-local clip manifests and a versioned JSON/
// CSV index. No file named Cam*.mp4 or full-frame metadata is generated. Called
// only after the common completion gates and before publishing the parent.
void PublishCropOnlyRecordingIndex(const std::filesystem::path& root,
                                  nlohmann::json* manifest);
}
