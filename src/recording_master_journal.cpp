#include "recording_master_journal.h"
#include "gui/spatial_layout/sha256.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace orange::recording {
namespace fs = std::filesystem;
namespace checksum = orange::gui::spatial_layout::checksum;
using json = nlohmann::json;
static_assert(std::atomic<uint64_t>::is_always_lock_free, "journal requires lock-free counters");
static_assert(std::is_trivially_copyable<MasterFrameFact>::value, "hot-path facts must be scalar copies");
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void system_check(bool ok, const char* operation) {
    if (!ok) throw std::system_error(errno, std::generic_category(), operation);
}
bool identity(const std::string& value) {
    return !value.empty() && value.size() <= 1024 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32; });
}
std::string csv_row(uint64_t index, const MasterFrameFact& f) {
    std::ostringstream out;
    out << index << ',' << static_cast<uint32_t>(f.kind) << ','
        << static_cast<uint32_t>(f.reject_reason) << ',' << f.recording_frame_id << ','
        << f.local_frame_id_present << ',' << f.local_frame_id << ','
        << f.camera_frame_id_present << ',' << f.camera_frame_id << ','
        << f.camera_timestamp_present << ',' << f.camera_timestamp_ns << ','
        << f.host_receive_steady_present << ',' << f.host_receive_steady_ns << ','
        << f.host_realtime_present << ',' << f.host_realtime_ns << '\n';
    return out.str();
}
// Terminal descriptor is create-once, synced, and never overwrites a prior seal.
void publish(const fs::path& destination, const std::string& bytes) {
    std::string staging = (destination.parent_path() / ".master_journal_XXXXXX").string();
    int fd = ::mkstemp(staging.data());
    system_check(fd >= 0, "create journal descriptor staging");
    try {
        MasterJournalIo io;
        io.Write(fd, bytes);
        io.Sync(fd);
        const int closing = fd;
        fd = -1;
        io.Close(closing);
        system_check(::link(staging.c_str(), destination.c_str()) == 0, "publish journal descriptor");
        const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        system_check(directory >= 0, "open journal directory");
        const int synced = ::fsync(directory);
        const int saved_errno = errno;
        ::close(directory);
        errno = saved_errno;
        system_check(synced == 0, "sync journal directory");
        ::unlink(staging.c_str());
    } catch (...) {
        if (fd >= 0) ::close(fd);
        ::unlink(staging.c_str());
        throw;
    }
}
} // namespace

void MasterJournalIo::Write(int fd, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        system_check(n > 0, "write master journal");
        done += static_cast<std::size_t>(n);
    }
}
void MasterJournalIo::Sync(int fd) { system_check(::fsync(fd) == 0, "sync master journal"); }
void MasterJournalIo::Close(int fd) { system_check(::close(fd) == 0, "close master journal"); }

const char* MasterFrameJournal::CsvHeader() noexcept {
    return "observation_index,kind,reject_reason,recording_frame_id,local_frame_id_present,local_frame_id,"
           "camera_frame_id_present,camera_frame_id,camera_timestamp_present,camera_timestamp_ns,"
           "host_receive_steady_present,host_receive_steady_ns,host_realtime_present,host_realtime_ns\n";
}

MasterFrameJournal::MasterFrameJournal(MasterJournalOptions options, std::shared_ptr<MasterJournalIo> io)
    : options_(std::move(options)), io_(io ? std::move(io) : std::make_shared<MasterJournalIo>()) {
    check(identity(options_.recording_id) && identity(options_.camera_serial) &&
          identity(options_.producer_instance_id), "master journal missing/invalid identity");
    check(std::all_of(options_.camera_serial.begin(), options_.camera_serial.end(), [](unsigned char c) {
              return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z') || c == '_' || c == '-';
          }), "camera serial is not a safe filename component");
    check(options_.queue_capacity >= 2 && options_.queue_capacity <= 65536,
          "master journal capacity outside 2..65536");
    check(fs::is_directory(options_.recording_directory), "recording directory must already exist");
    options_.recording_directory = fs::canonical(options_.recording_directory);
    check(options_.recording_directory != options_.recording_directory.root_path(),
          "master journal cannot use filesystem root");
    for (const int cpu : options_.writer_cpu_ids)
        check(cpu >= 0 && cpu < CPU_SETSIZE, "invalid journal writer CPU");
    const std::string stem = "Cam" + options_.camera_serial + "_master_frames_v1";
    csv_path_ = options_.recording_directory / (stem + ".csv");
    manifest_path_ = options_.recording_directory / (stem + ".json");
    check(!fs::exists(manifest_path_) && !fs::is_symlink(manifest_path_),
          "master journal descriptor already exists");
    queue_.resize(options_.queue_capacity); // fully allocated before producer arm
    std::promise<void> startup;
    auto ready = startup.get_future();
    worker_ = std::thread(&MasterFrameJournal::Run, this, std::move(startup));
    try { ready.get(); }
    catch (...) { worker_.join(); throw; }
}

bool MasterFrameJournal::Valid(const MasterFrameFact& f) noexcept {
    if ((!f.local_frame_id_present && f.local_frame_id != 0) ||
        (!f.camera_frame_id_present && f.camera_frame_id != 0) ||
        (!f.camera_timestamp_present && f.camera_timestamp_ns != 0) ||
        (!f.host_receive_steady_present && f.host_receive_steady_ns != 0) ||
        (!f.host_realtime_present && f.host_realtime_ns != 0)) return false;
    const bool source_frame = f.kind == MasterFactKind::assigned_frame ||
                              f.kind == MasterFactKind::received_unassigned;
    if (source_frame && !f.local_frame_id_present) return false;
    if (!source_frame && (f.recording_frame_id || f.local_frame_id_present ||
                         f.camera_frame_id_present || f.camera_timestamp_present ||
                         f.host_receive_steady_present)) return false;
    switch (f.kind) {
    case MasterFactKind::assigned_frame:
        return f.recording_frame_id > 0 && f.reject_reason == MasterRejectReason::none;
    case MasterFactKind::received_unassigned:
        return f.recording_frame_id == 0 && (f.reject_reason == MasterRejectReason::worker_entry_unavailable ||
            f.reject_reason == MasterRejectReason::readiness_event_unavailable ||
            f.reject_reason == MasterRejectReason::detector_event_unavailable);
    case MasterFactKind::resource_starvation_before_receive:
        return f.reject_reason == MasterRejectReason::worker_entry_unavailable ||
               f.reject_reason == MasterRejectReason::readiness_event_unavailable;
    case MasterFactKind::receive_error:
        return f.reject_reason == MasterRejectReason::receive_failed;
    case MasterFactKind::pause:
    case MasterFactKind::resume:
        return f.reject_reason == MasterRejectReason::none;
    }
    return false;
}

bool MasterFrameJournal::TrySubmit(const MasterFrameFact& fact) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        rejected_closed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto observation = offered_.fetch_add(1, std::memory_order_relaxed);
    auto rejection = [&] {
        if (rejected_full_.load(std::memory_order_relaxed) + rejected_invalid_.load(std::memory_order_relaxed) == 1)
            first_rejected_observation_ = observation;
        last_rejected_observation_ = observation;
    };
    if (!Valid(fact)) {
        rejected_invalid_.fetch_add(1, std::memory_order_relaxed);
        rejection();
        return false;
    }
    if (fact.kind == MasterFactKind::assigned_frame) {
        ++assigned_offered_;
        if (fact.recording_frame_id <= last_assigned_id_) assignment_order_valid_ = false;
        else {
            const auto gap = fact.recording_frame_id - last_assigned_id_ - 1;
            if (gap > std::numeric_limits<uint64_t>::max() - assigned_gaps_) {
                assigned_gaps_ = std::numeric_limits<uint64_t>::max();
                assignment_order_valid_ = false;
            } else assigned_gaps_ += gap;
        }
        last_assigned_id_ = fact.recording_frame_id;
    } else if (fact.kind == MasterFactKind::received_unassigned) ++received_unassigned_;
    else if (fact.kind == MasterFactKind::pause) ++pauses_;
    else if (fact.kind == MasterFactKind::resume) ++resumes_;
    else ++source_errors_;
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head - tail == queue_.size()) {
        rejected_full_.fetch_add(1, std::memory_order_relaxed);
        rejection();
        return false;
    }
    queue_[head % queue_.size()] = {observation, fact};
    const auto depth = head - tail + 1;
    if (depth > high_water_.load(std::memory_order_relaxed))
        high_water_.store(depth, std::memory_order_relaxed);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    head_.store(head + 1, std::memory_order_release);
    return true;
}

void MasterFrameJournal::Run(std::promise<void> startup) {
    checksum::StreamingSha256 written_hash;
    try {
        if (!options_.writer_cpu_ids.empty()) {
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int cpu : options_.writer_cpu_ids) CPU_SET(cpu, &set);
            const int result = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
            if (result != 0) throw std::system_error(result, std::generic_category(), "journal writer affinity");
        }
        cpu_set_t effective;
        CPU_ZERO(&effective);
        const int result = ::pthread_getaffinity_np(::pthread_self(), sizeof(effective), &effective);
        if (result != 0) throw std::system_error(result, std::generic_category(), "journal writer effective affinity");
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &effective)) effective_cpu_ids_.push_back(cpu);
        for (int cpu : options_.writer_cpu_ids)
            check(CPU_ISSET(cpu, &effective), "requested journal writer CPU was not applied");
        fd_ = ::open(csv_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        system_check(fd_ >= 0, "create master journal");
        io_->Write(fd_, CsvHeader());
        written_hash.update(std::string(CsvHeader()));
        startup.set_value();
    } catch (...) {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        startup.set_exception(std::current_exception());
        return;
    }
    for (;;) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            if (stopping_.load(std::memory_order_acquire) &&
                tail == head_.load(std::memory_order_acquire)) break;
            // The non-realtime writer polls; producer does not wake a thread or
            // contend on a condition-variable mutex on every camera frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const Entry entry = queue_[tail % queue_.size()];
        tail_.store(tail + 1, std::memory_order_release);
        if (io_error_.empty()) {
            try {
                const auto bytes = csv_row(entry.observation_index, entry.fact);
                io_->Write(fd_, bytes);
                written_hash.update(bytes);
                written_.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) { io_error_ = e.what(); }
            catch (...) { io_error_ = "unknown journal write failure"; }
            if (!io_error_.empty()) discarded_.fetch_add(1, std::memory_order_relaxed);
        } else discarded_.fetch_add(1, std::memory_order_relaxed);
    }
    try { io_->Sync(fd_); csv_synced_ = true; }
    catch (const std::exception& e) { if (io_error_.empty()) io_error_ = e.what(); }
    catch (...) { if (io_error_.empty()) io_error_ = "unknown journal sync failure"; }
    const int closing = fd_;
    fd_ = -1;
    try { io_->Close(closing); csv_closed_ = true; }
    catch (const std::exception& e) { if (io_error_.empty()) io_error_ = e.what(); }
    catch (...) { if (io_error_.empty()) io_error_ = "unknown journal close failure"; }
    written_sha256_ = "sha256:" + written_hash.final_hex();
}

MasterJournalCounters MasterFrameJournal::Counters() const noexcept {
    return {offered_.load(), accepted_.load(), rejected_full_.load(), rejected_invalid_.load(),
            rejected_closed_.load(), written_.load(), discarded_.load(), high_water_.load()};
}

const json& MasterFrameJournal::Finalize(bool normal) {
    if (finalized_) return final_record_;
    stopping_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    const auto counters = Counters();
    bool complete = normal && counters.offered > 0 && counters.offered == counters.written &&
        counters.rejected_closed == 0 && io_error_.empty() && csv_synced_ && csv_closed_;
    std::string digest, hash_error;
    std::error_code size_error;
    const auto bytes = fs::file_size(csv_path_, size_error);
    const bool hashed = !size_error && checksum::file_sha256(csv_path_, &digest, &hash_error);
    const bool bytes_match = hashed && digest == written_sha256_;
    complete = complete && bytes_match;
    json reasons = json::array();
    if (!normal) reasons.push_back("producer_not_normally_finished");
    if (!counters.offered) reasons.push_back("empty_observation_domain");
    if (counters.rejected_queue_full) reasons.push_back("queue_overflow");
    if (counters.rejected_invalid) reasons.push_back("invalid_source_fact");
    if (counters.rejected_closed) reasons.push_back("submission_after_close");
    if (counters.offered != counters.written) reasons.push_back("offered_written_mismatch");
    if (!io_error_.empty() || !csv_synced_ || !csv_closed_) reasons.push_back("journal_io_failure");
    if (!hashed) reasons.push_back("journal_hash_failed");
    else if (!bytes_match) reasons.push_back("journal_bytes_changed_or_partial_write");
    final_record_ = {
        {"schema_id", "orange.recording.master_frame_journal"}, {"schema_version", 1},
        {"status", complete ? "complete" : "incomplete"}, {"terminal", true},
        {"recording_id", options_.recording_id}, {"camera_serial", options_.camera_serial},
        {"producer_instance_id", options_.producer_instance_id}, {"stream_generation", options_.stream_generation},
        {"observation_boundary", "caller_submitted_acquisition_facts_v1"},
        {"observation_index_policy", "zero_based_offer_order_including_rejected_offers"},
        {"timestamp_authority", "original_scalars_presence_only_not_physical_timing_certification"},
        {"hardware_exposure_coverage", "not_certified"}, {"producer_finished_normally", normal},
        {"csv", hashed ? json{{"relative_path", csv_path_.filename().string()},
                              {"size_bytes", bytes}, {"sha256", digest}} : json(nullptr)},
        {"io", {{"fsync_succeeded", csv_synced_}, {"close_succeeded", csv_closed_},
                 {"error", io_error_.empty() ? json(nullptr) : json(io_error_)}}},
        {"counters", {{"offered", counters.offered}, {"accepted", counters.accepted},
            {"rejected_queue_full", counters.rejected_queue_full}, {"rejected_invalid", counters.rejected_invalid},
            {"rejected_closed", counters.rejected_closed}, {"written", counters.written},
            {"discarded_after_io_failure", counters.discarded_after_io_failure},
            {"queue_high_water", counters.queue_high_water}}},
        {"source", {{"assigned_frame_offers", assigned_offered_},
            {"received_unassigned_offers", received_unassigned_}, {"source_error_offers", source_errors_},
            {"pause_offers", pauses_}, {"resume_offers", resumes_},
            {"last_recording_frame_id", last_assigned_id_}, {"assigned_id_gap_count", assigned_gaps_},
            {"assigned_id_order_valid", assignment_order_valid_},
            {"assigned_ids_dense_from_one", assigned_offered_ > 0 && assignment_order_valid_ && assigned_gaps_ == 0}}},
        {"rejected_observations", {{"first", counters.rejected_queue_full + counters.rejected_invalid
            ? json(first_rejected_observation_) : json(nullptr)},
            {"last", counters.rejected_queue_full + counters.rejected_invalid
            ? json(last_rejected_observation_) : json(nullptr)}}},
        {"writer", {{"queue_capacity", options_.queue_capacity},
            {"affinity_policy", options_.writer_cpu_ids.empty() ? "inherited" : "explicit_caller_cpu_role"},
            {"requested_cpu_ids", options_.writer_cpu_ids}, {"effective_cpu_ids", effective_cpu_ids_}}},
        {"incomplete_reasons", reasons}
    };
    // A publication error is surfaced, never hidden by a cached complete result.
    // The CSV stays available for diagnosis; no old descriptor is overwritten.
    publish(manifest_path_, final_record_.dump() + "\n");
    finalized_ = true;
    return final_record_;
}

MasterFrameJournal::~MasterFrameJournal() {
    if (!finalized_) {
        try { (void)Finalize(false); }
        catch (...) { if (worker_.joinable()) worker_.join(); }
    }
}
} // namespace orange::recording
