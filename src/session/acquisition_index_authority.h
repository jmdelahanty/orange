#pragma once

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace orange::session {

// Immutable, per-camera view of a finalized Orange acquisition-index mapping.
//
// This is deliberately a read-only boundary.  Citrus can use it during
// post-recording finalization to convert Shaman-v2 recording_frame_id values
// into Palette's zero-based source_acquisition_frame_index domain without
// interpreting recording_session.json fields itself.
struct AcquisitionIndexAuthority {
    std::string recording_id;
    std::string camera_serial;
    std::uint64_t total_acquisitions = 0;
    std::uint64_t first_recording_frame_id = 0;
    std::uint64_t last_recording_frame_id = 0;
    std::string acquisition_mapping_sha256;
    std::string frame_identity_contract_sha256;
    std::string source_metadata_relative_path;
    std::filesystem::path source_metadata_path;
    std::string source_metadata_sha256;

    bool recording_frame_id_to_source_acquisition_index(
        std::uint64_t recording_frame_id,
        std::int64_t* acquisition_index_out,
        std::string* error_out = nullptr) const;
};

// Resolve one exact per-camera authority from a completed recording manifest.
// The resolver validates both semantic-record digests and the checksum of the
// recording-relative metadata artifact.  No manifest or artifact is modified.
bool resolve_acquisition_index_authority(
    const nlohmann::json& recording_session_manifest,
    const std::filesystem::path& recording_session_manifest_path,
    const std::string& camera_serial,
    AcquisitionIndexAuthority* authority_out,
    std::string* error_out = nullptr);

struct PaletteSourceAcquisitionMapping {
    std::vector<std::int64_t> source_acquisition_frame_index;
    std::string array_content_sha256;
    nlohmann::json mapping_record = nlohmann::json::object();
    std::string mapping_record_sha256;
};

// Build the exact source-acquisition array and closed mapping record accepted
// by Palette schema v5.  recording_frame_ids are raw Shaman-v2 identities for
// the Citrus stimulus rows; they need not be contiguous or unique because
// several stimulus states may legitimately refer to one acquisition frame.
bool build_palette_source_acquisition_mapping(
    const AcquisitionIndexAuthority& authority,
    const std::vector<std::uint64_t>& recording_frame_ids,
    const std::string& source_row_identity_sha256,
    const std::string& source_row_identity_contract_sha256,
    PaletteSourceAcquisitionMapping* mapping_out,
    std::string* error_out = nullptr);

}  // namespace orange::session
