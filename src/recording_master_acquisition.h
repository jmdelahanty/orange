#pragma once

#include "recording_master_journal.h"

namespace orange::recording {

// Closed, opt-in recording configuration. CPU placement is an explicit caller
// choice; this module never silently puts a disk writer on an acquisition core.
struct MasterAcquisitionConfig {
    bool enabled = false;
    std::size_t queue_capacity = 4096;
    std::vector<int> writer_cpu_ids;
    static MasterAcquisitionConfig Parse(const nlohmann::json& value);
    nlohmann::json ToJson() const;
};

// One acquisition producer and one serialized control-plane owner. Headless
// retains this until thread join; GUI uses a stable MasterSourceSlot lease.
class MasterAcquisitionJournal {
public:
    explicit MasterAcquisitionJournal(MasterJournalOptions options,
                                      std::shared_ptr<MasterJournalIo> io = {});

    class Iteration {
    public:
        Iteration(MasterAcquisitionJournal* owner, bool recording_active) noexcept;
        ~Iteration();
        Iteration(const Iteration&) = delete;
        Iteration& operator=(const Iteration&) = delete;
        bool Active() const noexcept { return active_; }
        bool FirstRecordingIteration() const noexcept { return first_recording_; }
        void Submit(const MasterFrameFact& fact) noexcept;
    private:
        MasterAcquisitionJournal* owner_;
        bool active_ = false;
        bool entered_ = false;
        bool first_recording_ = false;
    };

    void RequestStop() noexcept;
    bool SourceQuiescent() const noexcept;
    void MarkStopTimeout() noexcept { stop_timed_out_.store(true); }
    const nlohmann::json& Finalize(bool normal_finish);
    const std::string& CameraSerial() const noexcept { return camera_serial_; }
    const std::string& RecordingId() const noexcept { return recording_id_; }
    const std::string& ProducerInstanceId() const noexcept { return producer_instance_id_; }
    uint64_t StreamGeneration() const noexcept { return stream_generation_; }
    const std::filesystem::path& ManifestPath() const noexcept { return journal_.ManifestPath(); }
    MasterJournalCounters Counters() const noexcept { return journal_.Counters(); }

private:
    std::string camera_serial_, recording_id_, producer_instance_id_;
    uint64_t stream_generation_ = 0;
    MasterFrameJournal journal_;
    // Sequential consistency is intentional: stop + in_iteration form the
    // admission/quiescence handshake. Do not weaken these two atomics alone.
    std::atomic<bool> stopping_{false}, in_iteration_{false}, stop_timed_out_{false};
    bool previously_active_ = false; // acquisition-thread only
    bool ever_active_ = false; // fresh parent run, not a pause/resume boundary
};

// Immutable membership within one recording. Headless installs before threads
// start; GUI publishes the per-recording entries through stable source slots.
class MasterAcquisitionSet {
public:
    void Prepare(const MasterAcquisitionConfig& config,
                 const std::filesystem::path& root,
                 const std::vector<std::string>& camera_serials);
    MasterAcquisitionJournal* Find(const std::string& serial) const noexcept;
    void RequestStop() noexcept;
    bool SourceQuiescent() const noexcept;
    bool WaitForSourceStop(std::chrono::milliseconds timeout);
    // After source quiescence (or thread join); attempts every camera on failure.
    bool Finalize(bool normal_finish, std::string* error);
    nlohmann::json StartEvidence(const std::string& profile = "headless_acquisition_loop_v1") const;
    bool Enabled() const noexcept { return !entries_.empty(); }
private:
    MasterAcquisitionConfig config_;
    std::vector<std::unique_ptr<MasterAcquisitionJournal>> entries_;
};

// Control-plane gate for completed parent manifests. Reads only the explicitly
// required product declared in the recording snapshot; absent opt-in is a no-op.
// A failed/missing seal downgrades the parent to failed, never to legacy success.
void ApplyRequiredMasterJournalGate(const std::filesystem::path& root,
                                   nlohmann::json* manifest);

} // namespace orange::recording
