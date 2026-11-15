#include "session/recording_declaration_artifacts.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "fsuid_guard.h"
#include "outputs/common/digest.h"

namespace orange::session {

namespace {

// Files and folders belong to the sudo invoker (6af7c8b).
void write_bytes(const std::filesystem::path& path, const std::string& bytes)
{
    ScopedFsuid fsuid_guard;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("failed to write " + path.string());
}

}  // namespace

void WriteRecordingDeclarationArtifacts(const std::string& recording_folder,
                                        const RecordingDeclarationArtifacts& artifacts,
                                        const SnapshotSessionUpdate& update_snapshot,
                                        const std::string& queried_at_utc,
                                        const recording::HttpGetFn& http_get)
{
    const bool spec_copy = !artifacts.spec_copy_relative_path.empty();
    if (spec_copy) {
        // Refuse an inconsistent copy before any file exists.
        if (outputs::sha256_hex(artifacts.spec_bytes) != artifacts.spec_sha256) {
            throw std::runtime_error("declared spec copy: the bytes do not hash to spec_sha256");
        }
        if (artifacts.spec_copy_relative_path != "run_spec/" + artifacts.spec_sha256 + ".json") {
            throw std::runtime_error("declared spec copy: the path must be run_spec/<spec_sha256>.json, not " +
                                     artifacts.spec_copy_relative_path);
        }
    }

    const nlohmann::json block = recording::BuildSubjectReferences(
        artifacts.subject_references.Resolve(artifacts.recording_serials), artifacts.zebrobot, queried_at_utc,
        http_get);
    if (!update_snapshot || !update_snapshot(recording_folder, {{"subject_references", block}})) {
        throw std::runtime_error("failed to persist subject references");
    }

    const std::filesystem::path root(recording_folder);
    if (artifacts.subject_confirmation.is_object()) {
        write_bytes(root / "subject_confirmation.json", artifacts.subject_confirmation.dump(2) + "\n");
    }
    if (spec_copy) {
        write_bytes(root / artifacts.spec_copy_relative_path, artifacts.spec_bytes);
    }
}

}  // namespace orange::session
