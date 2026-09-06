#pragma once

#include "json.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace orange::recording {
// Resolved by a future parent/clip finalizer, not guessed from filenames. All
// paths are relative to its already bound recording root. This first utility
// verifies metadata only; it does not replace media/encoder/receipt validators.
struct MovingCropClipMetadata {
    std::string recording_id;
    std::string camera_serial;
    uint64_t clip_index = 0;
    std::string clip_id;
    std::filesystem::path metadata_relative_path;
};

// Control-plane utility for a MasterFrameJournal::Finalize result. Throws on
// mismatch (including missing suffix). O(1) frame storage, O(clips) references.
// Not an untrusted-manifest schema loader or a new Palette acquisition index.
nlohmann::json CheckMovingCropMetadataCoverage(
    const std::filesystem::path& recording_root,
    const nlohmann::json& finalized_master,
    const std::vector<MovingCropClipMetadata>& clips);

// Control-plane checks only, after acquisition, detector logging, crop workers,
// and the external recorder have drained. This is a necessary metadata gate,
// not a substitute for returned-identity, container or decoded-media validation.
// Creates clip-local metadata projections and a create-once receipt; never edits
// the original crop CSV or recorder files. Throws on incomplete evidence.
nlohmann::json FinalizeMovingCropMetadata(
    const std::filesystem::path& recording_root,
    const std::string& camera_serial,
    const std::filesystem::path& recorder_summary_relative_path,
    bool workers_and_recorder_stopped_normally);

// Parent finalizer gate. Rechecks bound artifacts, camera/parent/producer identity
// and clip membership against the current complete master descriptor. Returns
// the receipt reference; throws if missing, incomplete or mutated.
nlohmann::json RequireMovingCropMetadataReceipt(
    const std::filesystem::path& recording_root,
    const nlohmann::json& finalized_master);
void ApplyRequiredMovingCropMetadataGate(
    const std::filesystem::path& recording_root, nlohmann::json* parent_manifest);
}
