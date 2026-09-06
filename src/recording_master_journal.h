#pragma once

#include "json.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace orange::recording {

// An observed acquisition fact, not an encoded output or inferred exposure.
enum class MasterFactKind : uint32_t {
    assigned_frame = 1,
    received_unassigned = 2,
    resource_starvation_before_receive = 3,
    receive_error = 4,
    pause = 5,
    resume = 6,
};
enum class MasterRejectReason : uint32_t {
    none = 0,
    worker_entry_unavailable = 1,
    readiness_event_unavailable = 2,
    detector_event_unavailable = 3,
    receive_failed = 4,
};

struct MasterFrameFact {
    MasterFactKind kind = MasterFactKind::assigned_frame;
    MasterRejectReason reject_reason = MasterRejectReason::none;
    uint64_t recording_frame_id = 0; // zero means no ID was assigned
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t host_receive_steady_ns = 0;
    uint64_t host_realtime_ns = 0;
    // Presence does not certify clock synchronization, units from the device,
    // exposure phase, or a valid physical timestamp. Zero may be a present value.
    bool local_frame_id_present = false;
    bool camera_frame_id_present = false;
    bool camera_timestamp_present = false;
    bool host_receive_steady_present = false;
    bool host_realtime_present = false;
};

struct MasterJournalOptions {
    std::filesystem::path recording_directory;
    std::string recording_id;
    std::string camera_serial;
    std::string producer_instance_id;
    uint64_t stream_generation = 0;
    std::size_t queue_capacity = 4096;
    // Resolved by the caller's existing CPU-role/admission policy before arm.
    // Empty means inherit, explicitly recorded; never chooses isolated cores.
    std::vector<int> writer_cpu_ids;
};

struct MasterJournalCounters {
    uint64_t offered = 0;
    uint64_t accepted = 0;
    uint64_t rejected_queue_full = 0;
    uint64_t rejected_invalid = 0;
    uint64_t rejected_closed = 0;
    uint64_t written = 0;
    uint64_t discarded_after_io_failure = 0;
    uint64_t queue_high_water = 0;
};

// Narrow I/O seam for deterministic failure/backpressure tests. Only the writer
// thread invokes these methods; the acquisition caller never does.
class MasterJournalIo {
public:
    virtual ~MasterJournalIo() = default;
    virtual void Write(int fd, const std::string& bytes);
    virtual void Sync(int fd);
    virtual void Close(int fd);
};

// One instance per parent recording/camera/generation. Construct before arming;
// single acquisition producer calls TrySubmit. Finalize only after that producer
// has quiesced. No frame/image pointers, device operations, disk I/O, dynamic
// allocation, mutexes, notifications, or waiting inside TrySubmit.
class MasterFrameJournal {
public:
    explicit MasterFrameJournal(MasterJournalOptions options,
                                std::shared_ptr<MasterJournalIo> io = {});
    ~MasterFrameJournal();
    MasterFrameJournal(const MasterFrameJournal&) = delete;
    MasterFrameJournal& operator=(const MasterFrameJournal&) = delete;

    bool TrySubmit(const MasterFrameFact& fact) noexcept;
    MasterJournalCounters Counters() const noexcept;
    // Flushes/drains on the control thread. Interrupted/destructor finalization
    // cannot yield a complete journal. Returns/persists a closed candidate v1
    // record; it never modifies recording_session.json or acquisition mapping v1.
    const nlohmann::json& Finalize(bool producer_finished_normally);
    const std::filesystem::path& CsvPath() const noexcept { return csv_path_; }
    const std::filesystem::path& ManifestPath() const noexcept { return manifest_path_; }
    static const char* CsvHeader() noexcept;

private:
    struct Entry { uint64_t observation_index = 0; MasterFrameFact fact; };
    void Run(std::promise<void> startup);
    static bool Valid(const MasterFrameFact& fact) noexcept;
    MasterJournalOptions options_;
    std::shared_ptr<MasterJournalIo> io_;
    std::vector<Entry> queue_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> offered_{0}, accepted_{0}, rejected_full_{0}, rejected_invalid_{0};
    std::atomic<uint64_t> rejected_closed_{0}, written_{0}, discarded_{0}, high_water_{0};
    uint64_t assigned_offered_ = 0, received_unassigned_ = 0, source_errors_ = 0;
    uint64_t pauses_ = 0, resumes_ = 0, last_assigned_id_ = 0, assigned_gaps_ = 0;
    bool assignment_order_valid_ = true;
    uint64_t first_rejected_observation_ = 0, last_rejected_observation_ = 0;
    std::filesystem::path csv_path_, manifest_path_;
    int fd_ = -1;
    std::thread worker_;
    std::string io_error_;
    std::string written_sha256_;
    std::vector<int> effective_cpu_ids_;
    bool csv_synced_ = false, csv_closed_ = false, finalized_ = false;
    nlohmann::json final_record_;
};

} // namespace orange::recording
