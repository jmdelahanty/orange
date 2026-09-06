#include "gui_recording_evidence.h"
#include "scoped_housekeeping_cpu.h"
#include "fsuid_guard.h"
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace orange::recording {
using json = nlohmann::json;
namespace {
json read_app_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return json::object();
    // App preferences are not authority artifacts; preserve the existing app
    // loader's ability to read a user-managed symlink. Authority bundle import
    // separately rejects aliases. Saving through a symlink is never implicit.
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("app config must be a regular file no larger than 1 MiB");
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read app config");
    auto root = json::parse(in);
    if (!root.is_object() || (root.contains("recording") && !root.at("recording").is_object()))
        throw std::runtime_error("app config recording section must be an object");
    return root;
}
}
GuiRecordingEvidenceConfig ReadGuiRecordingEvidenceConfig(const std::filesystem::path& path) {
    ScopedFsuid filesystem_identity;
    const auto root = read_app_config(path);
    if (root.contains("recording") && root.at("recording").contains("registered_context_recording"))
        return GuiRecordingEvidenceConfig::Parse(root.at("recording").at("registered_context_recording"));
    return {};
}
void SaveGuiRecordingEvidenceConfig(const std::filesystem::path& path, const GuiRecordingEvidenceConfig& config) {
    ScopedFsuid filesystem_identity;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
        throw std::runtime_error("app config is a symlink; edit its resolved target explicitly instead of replacing the link");
    const auto validated = GuiRecordingEvidenceConfig::Parse(config.ToJson());
    const auto before = read_app_config(path);
    auto root = before;
    root["recording"]["registered_context_recording"] = validated.ToJson();
    if (!root.contains("schema_id")) root["schema_id"] = "orange.app.config";
    if (!root.contains("schema_version")) root["schema_version"] = 1;
    const auto bytes = root.dump(2) + '\n';
    const auto temp = path.string() + ".registered-context-" + std::to_string(getpid()) + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create exclusive app config temporary file");
    bool open = true;
    try {
        if (std::filesystem::exists(path) && ::fchmod(fd,
                static_cast<mode_t>(std::filesystem::status(path).permissions()) & 0777) != 0)
            throw std::runtime_error("cannot preserve app config permissions");
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("cannot write app config");
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("cannot sync app config");
        open = false;
        if (::close(fd) != 0) throw std::runtime_error("cannot close app config");
        if (read_app_config(path) != before) throw std::runtime_error("app config changed during context save; retry");
        std::filesystem::rename(temp, path);
        const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) throw std::runtime_error("saved app config but cannot open directory for sync");
        const bool synced = ::fsync(directory) == 0;
        const bool closed = ::close(directory) == 0;
        if (!synced || !closed) throw std::runtime_error("saved app config but directory sync/close failed");
    } catch (...) {
        if (open) ::close(fd);
        ::unlink(temp.c_str()); // only the exclusive temporary created above
        throw;
    }
}
GuiRecordingEvidenceConfig GuiRecordingEvidenceConfig::Parse(const json& j) {
    if (!j.is_object() || !j.contains("schema_version") || j.at("schema_version").is_boolean() ||
        !j.at("schema_version").is_number_integer() || j.at("schema_version") != 1 ||
        !j.contains("enabled") || !j.at("enabled").is_boolean())
        throw std::runtime_error("GUI registered-context recording requires schema_version 1 and boolean enabled");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "schema_version" && it.key() != "enabled" &&
            it.key() != "master_frame_journal" && it.key() != "registered_scene_context")
            throw std::runtime_error("unknown GUI registered-context recording field: " + it.key());
    GuiRecordingEvidenceConfig c;
    c.enabled = j.at("enabled").get<bool>();
    if (c.enabled || j.contains("master_frame_journal") || j.contains("registered_scene_context")) {
        c.master = MasterAcquisitionConfig::Parse(j.at("master_frame_journal"));
        c.context = RegisteredContextConfig::Parse(j.at("registered_scene_context"));
        if (!c.master.enabled || !c.context.enabled || !c.context.ReusesDailyContext())
            throw std::runtime_error("GUI context recording requires an enabled master journal and saved daily-registration context");
    }
    return c;
}
json GuiRecordingEvidenceConfig::ToJson() const {
    json j = {{"schema_version", 1}, {"enabled", enabled}};
    if (master.enabled || context.enabled) {
        j["master_frame_journal"] = master.ToJson();
        j["registered_scene_context"] = context.ToJson();
    }
    return j;
}
void GuiRecordingEvidence::Prepare(const GuiRecordingEvidenceConfig& input,
    const std::filesystem::path& root, std::vector<RegisteredContextCamera> cameras,
    const json& geometry) {
    const auto config = GuiRecordingEvidenceConfig::Parse(input.ToJson());
    if (!config.enabled) return;
    if (journals) throw std::runtime_error("GUI recording evidence already prepared");
    ScopedHousekeepingCpu affinity(config.context.worker_cpu_ids.front());
    std::vector<std::string> serials;
    for (const auto& c : cameras) serials.push_back(c.serial);
    journals = std::make_shared<MasterAcquisitionSet>();
    journals->Prepare(config.master, root, serials);
    for (auto& c : cameras) {
        const auto* master = journals->Find(c.serial);
        c.producer_instance_id = master->ProducerInstanceId();
        c.stream_generation = master->StreamGeneration();
    }
    RegisteredContextSet context;
    context.Prepare(config.context, root, cameras, geometry);
    if (!context.Complete()) throw std::runtime_error("GUI context import is incomplete");
    artifacts = {{"master_frame_journal", journals->StartEvidence("gui_acquisition_loop_v1")},
                 {"registered_scene_context", context.StartEvidence()},
                 {"gui_registered_context_recording", config.ToJson()}};
    affinity.Restore();
}
} // namespace orange::recording
