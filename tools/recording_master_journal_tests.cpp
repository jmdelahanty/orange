#include "recording_master_journal.h"
#include "recording_master_crop_coverage.h"
#include "gui/spatial_layout/sha256.h"
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sched.h>
#include <unistd.h>

namespace {
using namespace orange::recording;
using json = nlohmann::json;
namespace fs = std::filesystem;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    fs::path root;
    Fixture() {
        std::string name = (fs::temp_directory_path() / "orange_master_journal_test_XXXXXX").string();
        require(::mkdtemp(name.data()) != nullptr, "mkdtemp failed");
        root = name;
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    MasterJournalOptions options() const {
        MasterJournalOptions result;
        result.recording_directory = root;
        result.recording_id = "parent-recording";
        result.camera_serial = "02010093";
        result.producer_instance_id = "fixture-process-epoch";
        result.stream_generation = 17;
        return result;
    }
};
MasterFrameFact frame(uint64_t id) {
    MasterFrameFact result;
    result.recording_frame_id = id;
    result.local_frame_id_present = result.camera_frame_id_present = true;
    result.camera_timestamp_present = result.host_receive_steady_present = result.host_realtime_present = true;
    result.local_frame_id = 7000 + id;
    result.camera_frame_id = 14000 + id;
    result.camera_timestamp_ns = 1700000000000000000ULL + id;
    result.host_receive_steady_ns = 9000000 + id;
    result.host_realtime_ns = 1700000000000000100ULL + id;
    return result;
}
json read_json(const fs::path& path) { std::ifstream in(path); json result; in >> result; return result; }
std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void finalized_reference(const MasterFrameJournal& journal, const json& result) {
    require(read_json(journal.ManifestPath()) == result, "terminal manifest mismatch");
    std::string hash, error;
    require(orange::gui::spatial_layout::checksum::file_sha256(journal.CsvPath(), &hash, &error), "hash failed");
    require(result.at("csv").at("sha256") == hash, "exact CSV digest mismatch");
    require(result.at("csv").at("size_bytes") == fs::file_size(journal.CsvPath()), "CSV size mismatch");
}
template <typename F> void refuses(F action) {
    bool refused = false;
    try { action(); } catch (const std::exception&) { refused = true; }
    require(refused, "unsafe operation accepted");
}

class BlockingIo : public MasterJournalIo {
public:
    void Write(int fd, const std::string& bytes) override {
        if (bytes != MasterFrameJournal::CsvHeader()) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            changed.notify_all();
            require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; }), "test writer timed out");
        }
        MasterJournalIo::Write(fd, bytes);
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "writer did not start");
    }
    void Release() { std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all(); }
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
};
class FailingIo : public MasterJournalIo {
public:
    enum class At { header, row, partial_row, sync, close, changed_bytes };
    explicit FailingIo(At point) : at(point) {}
    void Write(int fd, const std::string& bytes) override {
        if ((at == At::header && bytes == MasterFrameJournal::CsvHeader()) ||
            (at == At::row && bytes != MasterFrameJournal::CsvHeader()))
            throw std::runtime_error("injected write failure");
        if (at == At::partial_row && bytes != MasterFrameJournal::CsvHeader()) {
            MasterJournalIo::Write(fd, bytes.substr(0, 7));
            throw std::runtime_error("injected partial write failure");
        }
        MasterJournalIo::Write(fd, bytes);
    }
    void Sync(int fd) override {
        if (at == At::sync) throw std::runtime_error("injected fsync failure");
        if (at == At::changed_bytes) MasterJournalIo::Write(fd, "unaccounted bytes\n");
        MasterJournalIo::Sync(fd);
    }
    void Close(int fd) override {
        MasterJournalIo::Close(fd);
        if (at == At::close) throw std::runtime_error("injected close failure");
    }
    At at;
};

void exact_rows_and_identity() {
    Fixture f;
    MasterFrameJournal journal(f.options());
    for (uint64_t id = 1; id <= 100; ++id) require(journal.TrySubmit(frame(id)), "unexpected journal rejection");
    const json result = journal.Finalize(true);
    require(result.at("status") == "complete", "normal journal not complete");
    require(result.at("recording_id") == "parent-recording" && result.at("camera_serial") == "02010093" &&
            result.at("stream_generation") == 17, "identity changed");
    require(result.at("counters").at("written") == 100, "written rows mismatch");
    require(result.at("source").at("assigned_ids_dense_from_one") == true, "dense IDs not preserved");
    const auto csv = read_text(journal.CsvPath());
    require(csv.find("0,1,0,1,1,7001,1,14001,1,1700000000000000001,1,9000001,1,1700000000000000101\n") !=
            std::string::npos, "timestamp or separate ID precision lost");
    require(!fs::exists(f.root / "Cam02010093.mp4"), "journal unexpectedly depends on video");
    finalized_reference(journal, result);
    require(journal.Finalize(false) == result, "repeated finalize changed original seal");
    require(!journal.TrySubmit(frame(101)) && journal.Counters().rejected_closed == 1, "post-close offer accepted");
    require(read_json(journal.ManifestPath()) == result, "post-close misuse rewrote seal");
}

void pre_assignment_errors_and_pause() {
    Fixture f;
    MasterFrameJournal journal(f.options());
    require(journal.TrySubmit(frame(1)), "frame 1");
    MasterFrameFact rejected = frame(0);
    rejected.kind = MasterFactKind::received_unassigned;
    rejected.reject_reason = MasterRejectReason::detector_event_unavailable;
    require(journal.TrySubmit(rejected), "received rejection not recorded");
    MasterFrameFact event;
    event.kind = MasterFactKind::resource_starvation_before_receive;
    event.reject_reason = MasterRejectReason::worker_entry_unavailable;
    require(journal.TrySubmit(event), "pre-receive starvation not recorded");
    event.kind = MasterFactKind::receive_error;
    event.reject_reason = MasterRejectReason::receive_failed;
    require(journal.TrySubmit(event), "GetFrame error not recorded");
    event.kind = MasterFactKind::pause; event.reject_reason = MasterRejectReason::none;
    require(journal.TrySubmit(event), "pause not recorded");
    event.kind = MasterFactKind::resume;
    require(journal.TrySubmit(event), "resume not recorded");
    require(journal.TrySubmit(frame(2)), "frame 2");
    const auto& result = journal.Finalize(true);
    require(result.at("status") == "complete", "complete observation journal confused with loss-free acquisition");
    require(result.at("source").at("assigned_frame_offers") == 2 &&
            result.at("source").at("received_unassigned_offers") == 1 &&
            result.at("source").at("source_error_offers") == 2, "source domains merged");
    require(result.at("hardware_exposure_coverage") == "not_certified", "invented exposure proof");
}

void overflow_never_seals_complete() {
    Fixture f;
    auto options = f.options(); options.queue_capacity = 2;
    auto io = std::make_shared<BlockingIo>();
    MasterFrameJournal journal(options, io);
    require(journal.TrySubmit(frame(1)), "first offer failed"); io->Wait();
    require(journal.TrySubmit(frame(2)) && journal.TrySubmit(frame(3)), "bounded capacity incorrect");
    require(!journal.TrySubmit(frame(4)), "overflow did not reject");
    io->Release();
    const auto& result = journal.Finalize(true);
    require(result.at("status") == "incomplete", "overflow falsely sealed complete");
    require(result.at("counters").at("offered") == 4 && result.at("counters").at("written") == 3 &&
            result.at("counters").at("queue_high_water") == 2 &&
            result.at("source").at("assigned_frame_offers") == 4, "overflow shrank source obligations");
    require(result.at("rejected_observations").at("first") == 3 &&
            result.at("rejected_observations").at("last") == 3, "overflow observation not accounted");
    finalized_reference(journal, result);
}

void failure_matrix() {
    for (auto at : {FailingIo::At::row, FailingIo::At::partial_row, FailingIo::At::sync,
                    FailingIo::At::close, FailingIo::At::changed_bytes}) {
        Fixture f;
        MasterFrameJournal journal(f.options(), std::make_shared<FailingIo>(at));
        for (int id = 1; id <= 5; ++id) require(journal.TrySubmit(frame(id)), "test queue full");
        const auto& result = journal.Finalize(true);
        require(result.at("status") == "incomplete", "I/O failure or changed bytes sealed complete");
        require(result.at("counters").at("offered") == 5, "I/O failure lost offer accounting");
        if (at == FailingIo::At::row || at == FailingIo::At::partial_row)
            require(result.at("counters").at("discarded_after_io_failure") == 5, "failed writer did not drain");
        finalized_reference(journal, result);
    }
    Fixture f;
    refuses([&] { MasterFrameJournal j(f.options(), std::make_shared<FailingIo>(FailingIo::At::header)); });
    require(!fs::exists(f.root / "Cam02010093_master_frames_v1.json"), "failed startup sealed");
}

void incomplete_and_sparse_domains() {
    for (const std::vector<uint64_t>& ids : {std::vector<uint64_t>{2, 3}, {1, 3}, {1, 1}, {1, 2, 1}}) {
        Fixture f; MasterFrameJournal journal(f.options());
        for (auto id : ids) require(journal.TrySubmit(frame(id)), "source fact discarded to hide discontinuity");
        const auto& result = journal.Finalize(true);
        require(result.at("status") == "complete", "source anomaly was not journaled faithfully");
        require(result.at("source").at("assigned_ids_dense_from_one") == false, "sparse/reset IDs promoted to dense v1");
    }
    Fixture interrupted;
    MasterFrameJournal journal(interrupted.options());
    require(journal.TrySubmit(frame(1)), "offer failed");
    require(journal.Finalize(false).at("status") == "incomplete", "interrupted journal sealed");
    Fixture empty;
    MasterFrameJournal no_frames(empty.options());
    require(no_frames.Finalize(true).at("status") == "incomplete", "empty journal sealed");
    Fixture abandoned;
    { MasterFrameJournal forgotten(abandoned.options()); require(forgotten.TrySubmit(frame(1)), "offer failed"); }
    require(read_json(abandoned.root / "Cam02010093_master_frames_v1.json").at("status") == "incomplete",
            "destructor promoted unfinished recording");
}

void present_zero_and_invalid_facts() {
    Fixture f; MasterFrameJournal journal(f.options());
    auto zero = frame(1); zero.camera_frame_id = 0; zero.camera_timestamp_ns = 0;
    require(journal.TrySubmit(zero), "present zero was treated as absent");
    auto invalid = frame(2); invalid.camera_timestamp_present = false;
    require(!journal.TrySubmit(invalid), "absent value contradiction accepted");
    invalid = frame(0);
    require(!journal.TrySubmit(invalid), "assigned zero ID accepted");
    invalid.kind = static_cast<MasterFactKind>(77);
    require(!journal.TrySubmit(invalid), "unknown kind accepted");
    const auto& result = journal.Finalize(true);
    require(result.at("status") == "incomplete" && result.at("counters").at("rejected_invalid") == 3,
            "invalid rows hidden");
}

void prearm_and_custody() {
    Fixture f;
    auto options = f.options(); options.queue_capacity = 1;
    refuses([&] { MasterFrameJournal journal(options); });
    options = f.options(); options.camera_serial = "../unsafe";
    refuses([&] { MasterFrameJournal journal(options); });
    options = f.options(); options.producer_instance_id.clear();
    refuses([&] { MasterFrameJournal journal(options); });
    options = f.options(); options.writer_cpu_ids = {-1};
    refuses([&] { MasterFrameJournal journal(options); });
    require(fs::is_empty(f.root), "prearm failure created artifacts");
    MasterFrameJournal original(f.options());
    require(original.TrySubmit(frame(1)), "offer failed");
    const auto& result = original.Finalize(true);
    const auto before = read_text(original.CsvPath());
    refuses([&] { MasterFrameJournal duplicate(f.options()); });
    require(read_text(original.CsvPath()) == before, "restart overwrote old master");
    finalized_reference(original, result);
    Fixture symlink;
    fs::create_symlink(original.CsvPath(), symlink.root / original.CsvPath().filename());
    refuses([&] { MasterFrameJournal through_link(symlink.options()); });
    require(read_text(original.CsvPath()) == before, "symlink target overwritten");
}

void writer_affinity() {
    cpu_set_t set; CPU_ZERO(&set);
    require(::sched_getaffinity(0, sizeof(set), &set) == 0, "cannot inspect test CPU mask");
    int selected = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) { selected = i; break; }
    require(selected >= 0, "empty CPU mask");
    Fixture f; auto options = f.options(); options.writer_cpu_ids = {selected};
    MasterFrameJournal journal(options); require(journal.TrySubmit(frame(1)), "offer failed");
    const auto& writer = journal.Finalize(true).at("writer");
    require(writer.at("effective_cpu_ids") == json::array({selected}) &&
            writer.at("affinity_policy") == "explicit_caller_cpu_role", "writer affinity not captured");
}

void final_tail_drain() {
    // Exercise the empty-queue / stop publication boundary repeatedly.
    for (int run = 0; run < 100; ++run) {
        Fixture f; MasterFrameJournal journal(f.options());
        for (int id = 1; id <= 257; ++id) require(journal.TrySubmit(frame(id)), "tail offer failed");
        const auto& result = journal.Finalize(true);
        require(result.at("status") == "complete" && result.at("counters").at("written") == 257,
                "terminal tail lost during drain");
    }
}

void crop_clip_correspondence() {
    Fixture f;
    MasterFrameJournal journal(f.options());
    require(journal.TrySubmit(frame(1)), "frame 1");
    auto unassigned = frame(0); unassigned.kind = MasterFactKind::received_unassigned;
    unassigned.reject_reason = MasterRejectReason::detector_event_unavailable;
    require(journal.TrySubmit(unassigned), "unassigned fact");
    for (int id = 2; id <= 5; ++id) require(journal.TrySubmit(frame(id)), "assigned fact");
    const json master = journal.Finalize(true);
    const std::string header = "recording_frame_id,timestamp,timestamp_sys,crop_video_frame_index,session_crop_video_frame_index,blank_frame\n";
    auto crop_row = [&](uint64_t id, uint64_t local) {
        const auto fact = frame(id);
        return std::to_string(id) + "," + std::to_string(fact.camera_timestamp_ns) + "," +
            std::to_string(fact.host_realtime_ns) + "," + std::to_string(local) + "," +
            std::to_string(id - 1) + "," + (id == 3 ? "1\n" : "0\n");
    };
    const auto first = header + crop_row(1, 0) + crop_row(2, 1) + crop_row(3, 2);
    const auto tail = header + crop_row(4, 0) + crop_row(5, 1);
    std::ofstream(f.root / "crop0.csv") << first;
    std::ofstream(f.root / "crop1.csv") << tail;
    std::vector<MovingCropClipMetadata> clips = {
        {"parent-recording", "02010093", 0, "clip_0000", "crop0.csv"},
        {"parent-recording", "02010093", 1, "clip_0001", "crop1.csv"}};
    const auto result = CheckMovingCropMetadataCoverage(f.root, master, clips);
    require(result.at("status") == "matched" && result.at("crop_rows") == 5 &&
            result.at("media_finalization_evaluated") == false, "crop coverage claim wrong");
    require(result.at("clips").at(1).at("first_session_crop_video_frame_index") == 3,
            "clip-local and session origins confused");
    // The unassigned observation is not a crop obligation; master observation
    // index therefore diverges from recording_frame_id - 1 after the first row.
    require(result.at("correspondence_sha256") == "sha256:" +
        orange::gui::spatial_layout::checksum::sha256_hex(
            "0,0,0,1,0\n0,1,1,2,2\n0,2,2,3,3\n1,0,3,4,4\n1,1,4,5,5\n"),
        "master observation index was aliased to recording or crop index");
    auto reject_tail = [&](const std::string& text) {
        std::ofstream(f.root / "crop1.csv") << text;
        refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, clips); });
    };
    reject_tail(header + crop_row(4, 0)); // missing terminal suffix
    reject_tail(header + crop_row(5, 0)); // missing first row after rollover
    reject_tail(header + crop_row(4, 3) + crop_row(5, 4)); // local index failed to reset
    reject_tail(header + crop_row(4, 0) + crop_row(4, 1)); // duplicate source
    reject_tail(tail + crop_row(6, 2)); // output beyond master stop
    reject_tail(tail.substr(0, tail.size() - 1)); // partial final CSV row
    auto altered_time = tail;
    altered_time.replace(altered_time.find("1700000000000000004"), 19, "1700000000000000005");
    reject_tail(altered_time);
    auto reset_session = tail;
    reset_session.replace(reset_session.find(",0,3,0"), 6, ",0,0,0");
    reject_tail(reset_session);
    std::ofstream(f.root / "crop1.csv") << tail;
    auto wrong = clips; wrong[1].camera_serial = "2010093";
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, wrong); });
    wrong = clips; wrong[1].recording_id = "different";
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, wrong); });
    wrong = clips; wrong[1].clip_index = 0;
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, wrong); });
    wrong = clips; wrong[1].clip_id = wrong[0].clip_id;
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, wrong); });
    wrong = clips; wrong[1].metadata_relative_path = "../crop1.csv";
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, wrong); });
    auto incomplete = master; incomplete["status"] = "incomplete";
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, incomplete, clips); });
    std::ofstream(journal.CsvPath(), std::ios::app) << "changed\n";
    refuses([&] { (void)CheckMovingCropMetadataCoverage(f.root, master, clips); });
}
} // namespace

int main() {
    try {
        exact_rows_and_identity(); pre_assignment_errors_and_pause(); overflow_never_seals_complete();
        failure_matrix(); incomplete_and_sparse_domains(); present_zero_and_invalid_facts();
        prearm_and_custody(); writer_affinity(); final_tail_drain(); crop_clip_correspondence();
        std::cout << "master journal: 10 groups passed (including clip/failure matrices and 100 tail-drain runs)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
