#include "recording_master_acquisition.h"
#include "gui/spatial_layout/sha256.h"
#include <fstream>
#include <set>
#include <stdexcept>
#include <sched.h>

namespace orange::recording {
static_assert(std::atomic<bool>::is_always_lock_free, "source admission requires lock-free flags");
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
json read_json(const fs::path& path) {
    std::ifstream in(path);
    require(in.good(), "master journal required artifact cannot be read");
    json result;
    in >> result;
    in >> std::ws;
    require(in.eof(), "master journal artifact has trailing data");
    return result;
}
json reference(const fs::path& path) {
    require(fs::is_regular_file(fs::symlink_status(path)), "master journal artifact is not a regular file");
    std::string hash, error;
    require(orange::gui::spatial_layout::checksum::file_sha256(path, &hash, &error),
            "master journal artifact cannot be hashed");
    return {{"relative_path", path.filename().string()},
            {"size_bytes", fs::file_size(path)}, {"sha256", hash}};
}
uint64_t integer(const json& value, uint64_t low, uint64_t high) {
    require(value.is_number_integer() && !value.is_boolean(), "master journal field must be an integer");
    require(!value.is_number_integer() || value.is_number_unsigned() || value.get<int64_t>() >= 0,
            "master journal field cannot be negative");
    const auto result = value.get<uint64_t>();
    require(result >= low && result <= high, "master journal integer out of range");
    return result;
}
}

MasterAcquisitionConfig MasterAcquisitionConfig::Parse(const json& value) {
    require(value.is_object(), "fixed.master_frame_journal must be an object");
    for (const auto& item : value.items()) {
        require(item.key() == "schema_version" || item.key() == "enabled" ||
                item.key() == "queue_capacity" || item.key() == "writer_cpu_ids",
                "unknown fixed.master_frame_journal field");
    }
    require(integer(value.at("schema_version"), 1, 1) == 1, "unsupported master journal schema");
    require(value.at("enabled").is_boolean(), "master journal enabled must be boolean");
    MasterAcquisitionConfig result;
    result.enabled = value.at("enabled").get<bool>();
    if (value.contains("queue_capacity")) result.queue_capacity = integer(value.at("queue_capacity"), 2, 65536);
    if (value.contains("writer_cpu_ids")) {
        require(value.at("writer_cpu_ids").is_array(), "master journal CPUs must be an array");
        std::set<int> unique;
        for (const auto& cpu : value.at("writer_cpu_ids")) {
            const int id = static_cast<int>(integer(cpu, 0, CPU_SETSIZE - 1));
            require(unique.insert(id).second, "duplicate master journal writer CPU");
            result.writer_cpu_ids.push_back(id);
        }
    }
    require(!result.enabled || !result.writer_cpu_ids.empty(), "enabled master journal needs explicit housekeeping writer_cpu_ids");
    return result;
}
json MasterAcquisitionConfig::ToJson() const {
    return {{"schema_version", 1}, {"enabled", enabled},
            {"queue_capacity", queue_capacity}, {"writer_cpu_ids", writer_cpu_ids}};
}

MasterAcquisitionJournal::MasterAcquisitionJournal(MasterJournalOptions options,
                                                   std::shared_ptr<MasterJournalIo> io)
    : camera_serial_(options.camera_serial), recording_id_(options.recording_id),
      producer_instance_id_(options.producer_instance_id), stream_generation_(options.stream_generation),
      journal_(std::move(options), std::move(io)) {}

MasterAcquisitionJournal::Iteration::Iteration(MasterAcquisitionJournal* owner,
                                               bool recording_active) noexcept : owner_(owner) {
    if (!owner_) return;
    owner_->in_iteration_.store(true);
    if (owner_->stopping_.load()) {
        owner_->in_iteration_.store(false);
        return;
    }
    entered_ = true;
    active_ = recording_active;
    first_recording_ = active_ && !owner_->ever_active_;
    owner_->ever_active_ = owner_->ever_active_ || active_;
    if (active_ != owner_->previously_active_) {
        MasterFrameFact transition;
        transition.kind = active_ ? MasterFactKind::resume : MasterFactKind::pause;
        (void)owner_->journal_.TrySubmit(transition);
        owner_->previously_active_ = active_;
    }
}
MasterAcquisitionJournal::Iteration::~Iteration() {
    if (entered_) owner_->in_iteration_.store(false);
}
void MasterAcquisitionJournal::Iteration::Submit(const MasterFrameFact& fact) noexcept {
    if (active_) (void)owner_->journal_.TrySubmit(fact);
}
void MasterAcquisitionJournal::RequestStop() noexcept { stopping_.store(true); }
bool MasterAcquisitionJournal::SourceQuiescent() const noexcept {
    return stopping_.load() && !in_iteration_.load();
}
const json& MasterAcquisitionJournal::Finalize(bool normal_finish) {
    require(SourceQuiescent(), "cannot finalize master journal with acquisition admitted");
    return journal_.Finalize(normal_finish && !stop_timed_out_.load());
}

void MasterAcquisitionSet::Prepare(const MasterAcquisitionConfig& config, const fs::path& root,
                                   const std::vector<std::string>& serials) {
    require(entries_.empty(), "master journal set cannot be replaced in a live session");
    config_ = MasterAcquisitionConfig::Parse(config.ToJson());
    if (!config.enabled) return;
    require(!serials.empty(), "master journal requires selected cameras");
    const fs::path canonical = fs::canonical(root);
    require(canonical != canonical.root_path(), "invalid master recording root");
    std::ifstream uuid_file("/proc/sys/kernel/random/uuid");
    std::string producer;
    std::getline(uuid_file, producer);
    require(producer.size() == 36, "cannot obtain master journal producer instance UUID");
    std::set<std::string> unique;
    // Generation zero within a fresh per-recording journal producer identity.
    // GUI camera threads may continue across these runs; this token does not
    // assert a hardware/thread restart. In-run reconnect needs a new generation.
    for (const auto& serial : serials) {
        require(unique.insert(serial).second, "duplicate master journal camera");
        MasterJournalOptions options;
        options.recording_directory = canonical;
        options.recording_id = canonical.filename().string();
        options.camera_serial = serial;
        options.producer_instance_id = producer;
        options.queue_capacity = config.queue_capacity;
        options.writer_cpu_ids = config.writer_cpu_ids;
        entries_.push_back(std::make_unique<MasterAcquisitionJournal>(options));
    }
}
MasterAcquisitionJournal* MasterAcquisitionSet::Find(const std::string& serial) const noexcept {
    for (const auto& entry : entries_) if (entry->CameraSerial() == serial) return entry.get();
    return nullptr;
}
void MasterAcquisitionSet::RequestStop() noexcept {
    for (const auto& entry : entries_) entry->RequestStop();
}
bool MasterAcquisitionSet::SourceQuiescent() const noexcept {
    for (const auto& entry : entries_) if (!entry->SourceQuiescent()) return false;
    return true;
}
bool MasterAcquisitionSet::WaitForSourceStop(std::chrono::milliseconds timeout) {
    RequestStop();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!SourceQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (SourceQuiescent()) return true;
    for (const auto& entry : entries_) entry->MarkStopTimeout();
    return false;
}
bool MasterAcquisitionSet::Finalize(bool normal_finish, std::string* error) {
    RequestStop();
    bool ok = true;
    for (const auto& entry : entries_) {
        try {
            if (entry->Finalize(normal_finish).at("status") != "complete") {
                ok = false;
                if (error) *error += entry->CameraSerial() + ": incomplete master journal; ";
            }
        } catch (const std::exception& ex) {
            ok = false;
            if (error) *error += entry->CameraSerial() + ": " + ex.what() + "; ";
        }
    }
    return ok;
}
json MasterAcquisitionSet::StartEvidence(const std::string& profile) const {
    require(profile == "headless_acquisition_loop_v1" || profile == "gui_acquisition_loop_v1",
            "unsupported master acquisition profile");
    json cameras = json::array();
    for (const auto& entry : entries_) cameras.push_back({
        {"camera_serial", entry->CameraSerial()}, {"recording_id", entry->RecordingId()},
        {"producer_instance_id", entry->ProducerInstanceId()}, {"stream_generation", entry->StreamGeneration()},
        {"descriptor_relative_path", entry->ManifestPath().filename().string()}});
    return {{"schema_version", 1}, {"required", Enabled()},
            {"profile", profile},
            {"config", config_.ToJson()}, {"cameras", cameras}};
}

void ApplyRequiredMasterJournalGate(const fs::path& root, json* manifest) {
    // Immutable start is authoritative even if a later mutable refresh loses
    // the opt-in. The mutable copy is only a failed-prearm fallback.
    const auto immutable_path = root / "recording_snapshot_start.json";
    const auto snapshot_path = fs::exists(immutable_path)
        ? immutable_path : root / "recording_snapshot.json";
    if (!fs::exists(snapshot_path)) return;
    const auto snapshot = read_json(snapshot_path);
    if (!snapshot.contains("session") || !snapshot.at("session").contains("master_frame_journal")) return;
    const auto& start = snapshot.at("session").at("master_frame_journal");
    if (!start.at("required").get<bool>()) return;
    json result = {{"schema_version", 1}, {"required", true},
                   {"profile", start.value("profile", std::string())},
                   {"status", "pending"}, {"cameras", json::array()}};
    const auto status = manifest->value("status", std::string());
    if (status == "completed" || status == "failed" || status == "interrupted") {
        try {
            require(start.at("schema_version") == 1 &&
                    (start.at("profile") == "headless_acquisition_loop_v1" ||
                     start.at("profile") == "gui_acquisition_loop_v1"),
                    "unsupported required master journal profile");
            require(start.at("cameras").is_array() && !start.at("cameras").empty(), "empty master journal camera set");
            std::set<std::string> serials;
            for (const auto& camera : start.at("cameras")) {
                const std::string serial = camera.at("camera_serial").get<std::string>();
                require(!serial.empty() && serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") == std::string::npos,
                        "invalid master journal camera serial");
                require(serials.insert(serial).second, "duplicate required master journal camera");
                const std::string filename = "Cam" + serial + "_master_frames_v1.json";
                require(camera.at("descriptor_relative_path") == filename, "unexpected master journal descriptor path");
                const auto descriptor_ref = reference(root / filename);
                const auto descriptor = read_json(root / filename);
                require(descriptor.at("schema_id") == "orange.recording.master_frame_journal" &&
                        descriptor.at("schema_version") == 1 && descriptor.at("status") == "complete" &&
                        descriptor.at("terminal") == true && descriptor.at("producer_finished_normally") == true,
                        "required master journal is not complete");
                require(descriptor.at("camera_serial") == serial && descriptor.at("recording_id") == camera.at("recording_id") &&
                        descriptor.at("recording_id") == manifest->at("session_id"), "master journal parent/camera mismatch");
                require(descriptor.at("producer_instance_id") == camera.at("producer_instance_id") &&
                        descriptor.at("stream_generation") == camera.at("stream_generation"), "master journal producer/generation mismatch");
                const auto csv_ref = reference(root / ("Cam" + serial + "_master_frames_v1.csv"));
                require(csv_ref == descriptor.at("csv"), "master journal CSV digest mismatch");
                require(descriptor.at("incomplete_reasons").is_array() && descriptor.at("incomplete_reasons").empty() &&
                        descriptor.at("io").at("fsync_succeeded") == true &&
                        descriptor.at("io").at("close_succeeded") == true && descriptor.at("io").at("error").is_null(),
                        "master journal completion contradicts failure evidence");
                const auto& counts = descriptor.at("counters");
                const auto offered = integer(counts.at("offered"), 1, UINT64_MAX);
                require(integer(counts.at("accepted"), 1, UINT64_MAX) == offered &&
                        integer(counts.at("written"), 1, UINT64_MAX) == offered,
                        "master journal offered/written parity mismatch");
                for (const auto* key : {"rejected_queue_full", "rejected_invalid", "rejected_closed", "discarded_after_io_failure"})
                    require(integer(counts.at(key), 0, 0) == 0, "master journal rejected source facts");
                require(reference(root / filename) == descriptor_ref, "master journal descriptor changed during validation");
                result["cameras"].push_back({{"camera_serial", serial}, {"descriptor", descriptor_ref}, {"csv", csv_ref}});
            }
            result["status"] = "complete";
        } catch (const std::exception& ex) {
            result["status"] = "failed";
            result["reason"] = ex.what();
            if (status == "completed") (*manifest)["status"] = "failed";
        }
    }
    (*manifest)["master_frame_journal"] = std::move(result);
}
} // namespace orange::recording
