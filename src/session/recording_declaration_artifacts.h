#pragma once

// The declaration files a recording folder gets at record start, before the
// start snapshot is sealed, written the same way for a GUI start and an
// orange_client run (first slice of moving recording-folder preparation out
// of the camera start paths; Jeremy, 2026-11-15):
//
//   recording_snapshot.json  session.subject_references (one entry per
//                            recording camera, looked up once, bounded)
//   subject_confirmation.json  who declared the recording (session::
//                            BuildSubjectConfirmation), when one exists
//   run_spec/<sha256>.json   the exact experiment-spec bytes a spec-declared
//                            run read (requester.spec_copy_relative_path)
//
// No camera, GPU or process state is touched, so tests run it on a
// temporary folder.

#include <functional>
#include <string>
#include <vector>

#include "json.hpp"
#include "recording_subject_reference.h"

namespace orange::session {

struct RecordingDeclarationArtifacts {
    std::vector<std::string> recording_serials;
    recording::SubjectReferencesConfig subject_references;
    recording::ZebrobotLookupConfig zebrobot;
    // orange.recording_subject_confirmation; null = none written.
    nlohmann::json subject_confirmation = nullptr;
    // Spec-declared runs only: the bytes read, their sha256 and the relative
    // path they are sealed at (run_spec/<sha256>.json). Empty = none.
    std::string spec_bytes;
    std::string spec_sha256;
    std::string spec_copy_relative_path;
};

// Merges a block into the folder's mutable start snapshot (production:
// update_recording_snapshot_session_artifacts in project.cpp).
using SnapshotSessionUpdate = std::function<bool(const std::string& folder, const nlohmann::json& session)>;

// Writes the three artifacts above. Files belong to the sudo invoker. Throws
// std::runtime_error naming what failed; a spec copy whose bytes do not hash
// to spec_sha256, or whose path is not run_spec/<spec_sha256>.json, is refused
// before anything is written.
void WriteRecordingDeclarationArtifacts(const std::string& recording_folder,
                                        const RecordingDeclarationArtifacts& artifacts,
                                        const SnapshotSessionUpdate& update_snapshot,
                                        const std::string& queried_at_utc,
                                        const recording::HttpGetFn& http_get = recording::HttpGet);

}  // namespace orange::session
