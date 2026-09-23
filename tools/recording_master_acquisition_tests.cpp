#include "recording_master_acquisition.h"
#include "recording_master_source_slot.h"
#include <fstream>
#include <iostream>
#include <future>
#include <sched.h>
#include <unistd.h>

namespace {
using namespace orange::recording;
using json = nlohmann::json;
namespace fs = std::filesystem;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<class F> void refuses(F f) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    require(threw, "unsafe operation accepted");
}
struct Fixture {
    fs::path root;
    Fixture() {
        std::string tmp = (fs::temp_directory_path() / "orange_master_acquisition_XXXXXX").string();
        require(mkdtemp(tmp.data()) != nullptr, "mkdtemp failed");
        root = tmp;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    MasterJournalOptions options() const {
        MasterJournalOptions o;
        o.recording_directory = root;
        o.recording_id = root.filename().string();
        o.camera_serial = "02010093";
        o.producer_instance_id = "test-producer";
        return o;
    }
};
MasterFrameFact frame(uint64_t id) {
    MasterFrameFact f;
    f.recording_frame_id = id;
    f.local_frame_id = id + 400;
    f.camera_frame_id = id + 9000;
    f.camera_timestamp_ns = 1800000000000000000ULL + id;
    f.host_realtime_ns = 1800000000000000040ULL + id;
    f.host_receive_steady_ns = 12345 + id;
    f.local_frame_id_present = f.camera_frame_id_present = true;
    f.camera_timestamp_present = f.host_realtime_present = f.host_receive_steady_present = true;
    return f;
}
void put_json(const fs::path& p, const json& j) { std::ofstream out(p); out << j.dump(2) << '\n'; }
MasterAcquisitionConfig config() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    require(sched_getaffinity(0, sizeof(mask), &mask) == 0, "get affinity failed");
    MasterAcquisitionConfig c;
    c.enabled = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &mask)) {
        c.writer_cpu_ids = {cpu};
        break;
    }
    return c;
}
void config_contract() {
    require(!MasterAcquisitionConfig::Parse(MasterAcquisitionConfig{}.ToJson()).enabled, "default enabled");
    auto valid = config().ToJson();
    require(MasterAcquisitionConfig::Parse(valid).ToJson() == valid, "config roundtrip failed");
    for (auto node : {json(nullptr), json::array(), json{{"schema_version", 1}},
                     json{{"schema_version", 1}, {"enabled", true}}})
        refuses([&] { MasterAcquisitionConfig::Parse(node); });
    for (auto key : {"schema_version", "queue_capacity"}) {
        for (auto bad : {json(-1), json(true), json(1.5), json("1"), json(UINT64_MAX)}) {
            auto node = valid; node[key] = bad;
            refuses([&] { MasterAcquisitionConfig::Parse(node); });
        }
    }
    for (auto cpus : {json::array(), json::array({0, 0}), json::array({-1}), json::array({true}), json::array({CPU_SETSIZE})}) {
        auto node = valid; node["writer_cpu_ids"] = cpus;
        refuses([&] { MasterAcquisitionConfig::Parse(node); });
    }
    valid["typo"] = true;
    refuses([&] { MasterAcquisitionConfig::Parse(valid); });
}
void observed_domain() {
    Fixture fixture;
    MasterAcquisitionJournal journal(fixture.options());
    { MasterAcquisitionJournal::Iteration warmup(&journal, false); warmup.Submit(frame(42)); }
    require(journal.Counters().offered == 0, "warmup became recording");
    {
        MasterAcquisitionJournal::Iteration active(&journal, true);
        require(active.FirstRecordingIteration(), "first parent iteration was not marked");
        active.Submit(frame(1));
        auto rejected = frame(0);
        rejected.kind = MasterFactKind::received_unassigned;
        rejected.reject_reason = MasterRejectReason::detector_event_unavailable;
        active.Submit(rejected);
        MasterFrameFact starved;
        starved.kind = MasterFactKind::resource_starvation_before_receive;
        starved.reject_reason = MasterRejectReason::worker_entry_unavailable;
        active.Submit(starved);
        MasterFrameFact failure;
        failure.kind = MasterFactKind::receive_error;
        failure.reject_reason = MasterRejectReason::receive_failed;
        active.Submit(failure);
    }
    { MasterAcquisitionJournal::Iteration pause(&journal, false); pause.Submit(frame(42)); }
    { MasterAcquisitionJournal::Iteration resume(&journal, true);
      require(!resume.FirstRecordingIteration(), "pause reset parent numbering"); resume.Submit(frame(2)); }
    // Clip rollover does not alter admission or parent frame numbering.
    { MasterAcquisitionJournal::Iteration next_clip(&journal, true); next_clip.Submit(frame(3)); }
    refuses([&] { journal.Finalize(true); });
    journal.RequestStop();
    require(journal.SourceQuiescent(), "empty source not quiescent");
    { MasterAcquisitionJournal::Iteration stale_record_flag(&journal, true);
      require(!stale_record_flag.Active(), "stopped source readmitted"); stale_record_flag.Submit(frame(4)); }
    const auto& final = journal.Finalize(true);
    require(final.at("status") == "complete", "source journal not complete");
    require(final.at("source").at("assigned_frame_offers") == 3, "assigned domain wrong");
    require(final.at("counters").at("offered") == 9, "transition/source facts lost");
}
void stop_tail_handshake() {
    // Control stop while a source frame is in progress must preserve the tail
    // and cannot claim quiescence or seal until the frame's scope has ended.
    for (int attempt = 0; attempt < 100; ++attempt) {
        Fixture fixture;
        MasterAcquisitionJournal journal(fixture.options());
        std::promise<void> entered, release;
        auto permission = release.get_future();
        std::thread producer([&] {
            MasterAcquisitionJournal::Iteration scope(&journal, true);
            entered.set_value();
            permission.wait();
            scope.Submit(frame(1));
        });
        entered.get_future().wait();
        journal.RequestStop();
        const bool erroneously_quiet = journal.SourceQuiescent();
        bool rejected_close = false;
        try { journal.Finalize(true); } catch (const std::exception&) { rejected_close = true; }
        release.set_value();
        producer.join();
        require(!erroneously_quiet && rejected_close, "stop raced source tail");
        require(journal.SourceQuiescent(), "producer exit did not acknowledge stop");
        require(journal.Finalize(true).at("source").at("assigned_frame_offers") == 1, "tail missing");
    }
}
void exceptional_exit_and_timeout() {
    Fixture f;
    MasterAcquisitionJournal journal(f.options());
    try {
        MasterAcquisitionJournal::Iteration scope(&journal, true);
        scope.Submit(frame(1));
        throw std::runtime_error("synthetic acquisition failure");
    } catch (const std::exception&) {}
    journal.RequestStop();
    require(journal.SourceQuiescent(), "exception leaked source admission");
    journal.MarkStopTimeout();
    require(journal.Finalize(true).at("status") == "incomplete", "timeout promoted to complete");
}
void required_parent_gate() {
    Fixture fixture;
    MasterAcquisitionSet set;
    set.Prepare(config(), fixture.root, {"02010093", "2010094"});
    refuses([&] { set.Prepare(config(), fixture.root, {"02010093"}); });
    const auto start = set.StartEvidence();
    const json snapshot = {{"session", {{"master_frame_journal", start}}}};
    put_json(fixture.root / "recording_snapshot_start.json", snapshot);
    put_json(fixture.root / "recording_snapshot.json", json::object());
    json parent = {{"status", "completed"}, {"session_id", fixture.root.filename().string()}};
    auto missing = parent;
    ApplyRequiredMasterJournalGate(fixture.root, &missing);
    require(missing.at("status") == "failed", "unsealed required journals accepted");
    for (auto serial : {"02010093", "2010094"}) {
        MasterAcquisitionJournal::Iteration scope(set.Find(serial), true);
        scope.Submit(frame(1)); scope.Submit(frame(2));
    }
    require(set.WaitForSourceStop(std::chrono::milliseconds(20)), "source stop failed");
    std::string error;
    require(set.Finalize(true, &error), "multi camera close failed");
    auto accepted = parent;
    ApplyRequiredMasterJournalGate(fixture.root, &accepted);
    require(accepted.at("status") == "completed" && accepted.at("master_frame_journal").at("status") == "complete",
            "complete parent refused");
    require(accepted.at("master_frame_journal").at("cameras").size() == 2, "one camera omitted");
    put_json(fixture.root / "recording_snapshot_start.json",
        {{"session", {{"master_frame_journal", set.StartEvidence("gui_acquisition_loop_v1")}}}});
    auto gui = parent;
    ApplyRequiredMasterJournalGate(fixture.root, &gui);
    require(gui.at("status") == "completed" && gui.at("master_frame_journal").at("profile") == "gui_acquisition_loop_v1",
        "GUI profile rejected or mislabeled");
    refuses([&] { set.StartEvidence("unknown-profile"); });
    const auto descriptor_path = fixture.root / "Cam2010094_master_frames_v1.json";
    json original_descriptor;
    { std::ifstream in(descriptor_path); in >> original_descriptor; }
    std::vector<json> contradictory;
    auto changed = original_descriptor; changed["producer_instance_id"] = "other-producer"; contradictory.push_back(changed);
    changed = original_descriptor; changed["stream_generation"] = 9; contradictory.push_back(changed);
    changed = original_descriptor; changed["recording_id"] = "other-parent"; contradictory.push_back(changed);
    changed = original_descriptor; changed["counters"]["written"] = 1; contradictory.push_back(changed);
    changed = original_descriptor; changed["counters"]["rejected_queue_full"] = 1; contradictory.push_back(changed);
    changed = original_descriptor; changed["io"]["error"] = "disk failure"; contradictory.push_back(changed);
    changed = original_descriptor; changed["status"] = "incomplete"; contradictory.push_back(changed);
    for (const auto& bad : contradictory) {
        put_json(descriptor_path, bad);
        auto refused = parent;
        ApplyRequiredMasterJournalGate(fixture.root, &refused);
        require(refused.at("status") == "failed", "contradictory descriptor accepted");
    }
    put_json(descriptor_path, original_descriptor);
    const auto moved_descriptor = fixture.root / "saved_descriptor.json";
    fs::rename(descriptor_path, moved_descriptor);
    fs::create_symlink(moved_descriptor, descriptor_path);
    auto symlink = parent;
    ApplyRequiredMasterJournalGate(fixture.root, &symlink);
    require(symlink.at("status") == "failed", "symlinked descriptor accepted");
    fs::remove(descriptor_path);
    fs::rename(moved_descriptor, descriptor_path);
    { std::ofstream out(fixture.root / "Cam2010094_master_frames_v1.csv", std::ios::app); out << "changed\n"; }
    auto corrupt = parent;
    ApplyRequiredMasterJournalGate(fixture.root, &corrupt);
    require(corrupt.at("status") == "failed", "changed journal CSV accepted");
    auto pending = parent; pending["status"] = "recording";
    ApplyRequiredMasterJournalGate(fixture.root, &pending);
    require(pending.at("master_frame_journal").at("status") == "pending", "nonterminal result certified");
}
void empty_disabled_and_interrupted() {
    Fixture fixture;
    MasterAcquisitionSet disabled;
    disabled.Prepare({}, fixture.root, {});
    require(!disabled.Enabled() && disabled.SourceQuiescent(), "disabled mode has lifecycle obligations");
    require(disabled.Find("02010093") == nullptr, "disabled source exists");
    json old = {{"status", "completed"}, {"session_id", "old-recording"}};
    const auto before = old;
    ApplyRequiredMasterJournalGate(fixture.root, &old);
    require(old == before, "nonopted-in parent changed");
    MasterAcquisitionSet interrupted;
    interrupted.Prepare(config(), fixture.root, {"02010093"});
    { MasterAcquisitionJournal::Iteration scope(interrupted.Find("02010093"), true); scope.Submit(frame(1)); }
    std::string error;
    require(!interrupted.Finalize(false, &error) && !error.empty(), "interruption accepted");
}
void gui_slot_tail_and_rearm() {
    MasterSourceSlot slot;
    { MasterSourceSlot::Lease idle(&slot); require(!idle.Get(), "disabled slot is active"); }
    for (int run = 0; run < 100; ++run) {
        Fixture f;
        auto journal = std::make_unique<MasterAcquisitionJournal>(f.options());
        slot.Attach(journal.get());
        std::promise<void> entered, release;
        auto ready = entered.get_future(); auto finish = release.get_future();
        std::thread source([&] {
            MasterSourceSlot::Lease lease(&slot);
            MasterAcquisitionJournal::Iteration iteration(lease.Get(), true);
            entered.set_value(); finish.wait(); iteration.Submit(frame(1));
        });
        ready.wait(); journal->RequestStop(); slot.Detach();
        const bool retired_too_early = slot.Idle();
        bool refused_attach = false;
        try { slot.Attach(journal.get()); } catch (const std::exception&) { refused_attach = true; }
        release.set_value(); source.join();
        require(!retired_too_early && refused_attach, "live GUI source storage was reusable");
        require(slot.Idle() && journal->SourceQuiescent(), "GUI source lease leaked");
        require(journal->Finalize(true).at("source").at("assigned_frame_offers") == 1,
            "GUI tail lost or another run's frame leaked");
        journal.reset();
        { MasterSourceSlot::Lease inactive(&slot); require(!inactive.Get(), "retired journal leaked into idle stream"); }
    }
}
void gui_slot_streaming_replacement_stress() {
    MasterSourceSlot slot;
    std::atomic<bool> done{false};
    std::thread source([&] {
        while (!done.load()) {
            MasterSourceSlot::Lease lease(&slot);
            MasterAcquisitionJournal::Iteration iteration(lease.Get(), false);
        }
    });
    for (int run = 0; run < 100; ++run) {
        Fixture f;
        auto journal = std::make_unique<MasterAcquisitionJournal>(f.options());
        // A late, non-dereferencing hazard publication may briefly keep Idle
        // false after a detach; retirement always waits it out before rearm.
        while (!slot.Idle()) std::this_thread::yield();
        slot.Attach(journal.get());
        journal->RequestStop(); slot.Detach();
        while (!slot.Idle()) std::this_thread::yield();
        journal.reset();
    }
    done.store(true); source.join();
    require(slot.Idle(), "streaming replacement leaked source hazard");
}
}
int main() {
    try {
        config_contract();
        observed_domain();
        stop_tail_handshake();
        exceptional_exit_and_timeout();
        required_parent_gate();
        empty_disabled_and_interrupted();
        gui_slot_tail_and_rearm();
        gui_slot_streaming_replacement_stress();
        std::cout << "8 master acquisition integration groups passed\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
