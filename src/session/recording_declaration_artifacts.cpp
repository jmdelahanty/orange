#include "session/recording_declaration_artifacts.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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
    // A declared dish needs MetaZebrobot configured (every start path checks
    // this earlier; this is the last check before any file exists).
    recording::RequireZebrobotForDeclaredDishes(artifacts.subject_references, artifacts.zebrobot,
                                                &artifacts.recording_serials);
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

    for (const auto& file : artifacts.declaration_files) {
        // Each must be a run_plan/ path named by the sha256 of its bytes.
        const std::string name = std::filesystem::path(file.relative_path).stem().string();
        if (file.relative_path.rfind("run_plan/", 0) != 0 || file.relative_path.find("..") != std::string::npos ||
            name != file.sha256 || outputs::sha256_hex(file.bytes) != file.sha256) {
            throw std::runtime_error("declaration file " + file.relative_path +
                                     " is not run_plan/<sha256 of its bytes>");
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
    for (const auto& file : artifacts.declaration_files) {
        write_bytes(root / file.relative_path, file.bytes);
    }
}

std::vector<RecordingDeclarationArtifacts::SealedFile> ReadDeclarationFiles(const RecordingStartDeclaration& declaration,
                                                                           const std::filesystem::path& files_root)
{
    std::vector<RecordingDeclarationArtifacts::SealedFile> files;
    const auto copies = DeclarationFileCopies(declaration);
    if (copies.empty()) return files;
    if (files_root.empty() || !files_root.is_absolute())
        throw std::runtime_error("the declaration files root must be an absolute path");
    std::set<std::string> named;
    for (const auto& copy : copies) named.insert(copy.relative_path);
    std::error_code walk_ec;
    for (std::filesystem::recursive_directory_iterator it(files_root / "run_plan", walk_ec), end;
         !walk_ec && it != end; it.increment(walk_ec)) {
        if (!it->is_regular_file()) continue;
        const std::string relative = std::filesystem::relative(it->path(), files_root).generic_string();
        if (!named.count(relative)) throw std::runtime_error("run_plan file not named by the declaration: " + relative);
    }
    for (const auto& copy : copies) {
        std::ifstream input(files_root / copy.relative_path, std::ios::binary);
        if (!input) throw std::runtime_error("missing declaration file " + copy.relative_path);
        std::ostringstream contents;
        contents << input.rdbuf();
        const std::string bytes = contents.str();
        if (outputs::sha256_hex(bytes) != copy.sha256)
            throw std::runtime_error("declaration file " + copy.relative_path +
                                     " does not hash to the sha256 the declaration names");
        files.push_back({copy.relative_path, bytes, copy.sha256});
    }
    return files;
}

}  // namespace orange::session
