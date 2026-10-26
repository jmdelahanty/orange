// Tests src/timing/log_writer.h: ordering across rotation, drop counting on a
// full ring, flush barriers, multi-producer channels, and that a stalled
// channel never blocks another channel's producer, and a sealed folder is
// never written again; the running digest each channel publishes at close
// (timing/written_file_digest.h), its checks against the disk, and its cost.
#include "timing/log_writer.h"

#include "gui/spatial_layout/sha256.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}
std::string slurp(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
struct Row {
    uint64_t frame = 0;
    int64_t value = 0;
};
void format_row(const Row& row, std::string& out)
{
    out += std::to_string(row.frame);
    out += ',';
    out += std::to_string(row.value);
    out += '\n';
}
}  // namespace

int main()
{
    using namespace orange::timing;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("orange_log_writer_tests_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto wait = std::chrono::milliseconds(5000);

    // 1. Rotation: records before Open(b) land in a, after it in b.
    {
        auto channel = LogChannel<Row>::Create({"rotation", 64}, format_row);
        channel->Open((dir / "a.csv").string(), "frame,value\n");
        for (uint64_t i = 1; i <= 3; ++i) channel->Emit({i, static_cast<int64_t>(i) * 10});
        channel->Open((dir / "b.csv").string(), "frame,value\n");
        for (uint64_t i = 4; i <= 5; ++i) channel->Emit({i, -static_cast<int64_t>(i)});
        expect(channel->Retire(wait), "rotation retire");
        expect(slurp(dir / "a.csv") == "frame,value\n1,10\n2,20\n3,30\n", "rotation file a");
        expect(slurp(dir / "b.csv") == "frame,value\n4,-4\n5,-5\n", "rotation file b");
        const auto c = channel->counters();
        expect(c.offered == 5 && c.written == 5 && c.dropped_queue_full == 0, "rotation counters");
        expect(c.files_opened == 2, "rotation opened two files");
    }

    // 2. Records with no open file are discarded and counted, never written.
    {
        auto channel = LogChannel<Row>::Create({"nofile", 16}, format_row);
        channel->Emit({1, 1});
        channel->Open((dir / "c.csv").string(), "h\n");
        channel->Emit({2, 2});
        expect(channel->Retire(wait), "nofile retire");
        expect(slurp(dir / "c.csv") == "h\n2,2\n", "nofile content");
        expect(channel->counters().discarded_no_file == 1, "nofile discarded count");
    }

    // 3. A full ring drops and counts; Emit never blocks.
    {
        // A formatter that stalls the writer makes the ring fill.
        std::atomic<bool> release{false};
        auto channel = LogChannel<Row>::Create(
            {"full", 8}, [&](const Row& row, std::string& out) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                format_row(row, out);
            });
        channel->Open((dir / "d.csv").string(), "");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int accepted = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 100; ++i) accepted += channel->Emit({i, 0}) ? 1 : 0;
        const auto spent = std::chrono::steady_clock::now() - start;
        expect(spent < std::chrono::milliseconds(50), "Emit does not block on a full ring");
        release.store(true);
        expect(channel->Retire(wait), "full retire");
        const auto c = channel->counters();
        expect(accepted <= 9 && accepted >= 8, "full ring accepts about its capacity");
        expect(c.dropped_queue_full == 100 - static_cast<uint64_t>(accepted), "full drops counted");
        expect(c.written == static_cast<uint64_t>(accepted), "full written = accepted");
        expect(c.queue_high_water <= 8, "high water within capacity");
    }

    // 4. While one channel stalls the (single) writer, another channel's
    //    producer never blocks and its rows queue in its own ring until the
    //    writer resumes; nothing is dropped while the ring has room.
    {
        std::atomic<bool> release{false};
        auto slow = LogChannel<Row>::Create(
            {"slow", 8}, [&](const Row& row, std::string& out) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                format_row(row, out);
            });
        auto fast = LogChannel<Row>::Create({"fast", 64}, format_row);
        slow->Open((dir / "slow.csv").string(), "");
        fast->Open((dir / "fast.csv").string(), "");
        slow->Emit({1, 1});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // The writer is stuck in slow's formatter: fast queues, does not drop.
        for (uint64_t i = 0; i < 32; ++i) fast->Emit({i, 0});
        expect(fast->counters().dropped_queue_full == 0, "fast channel did not drop");
        release.store(true);
        expect(slow->Retire(wait) && fast->Retire(wait), "slow/fast retire");
        expect(fast->counters().written == 32, "fast channel wrote everything");
    }

    // 5. Flush barrier: the file holds every earlier row once the ticket applies.
    {
        auto channel = LogChannel<Row>::Create({"flush", 64}, format_row);
        channel->Open((dir / "e.csv").string(), "");
        channel->Emit({7, 7});
        expect(channel->WaitApplied(channel->Flush(), wait), "flush applied");
        expect(slurp(dir / "e.csv") == "7,7\n", "flush wrote the row");
        expect(channel->Retire(wait), "flush retire");
    }

    // 6. Multi-producer channel: all rows from four threads arrive.
    {
        LogChannelOptions options{"multi", 1 << 14};
        options.multi_producer = true;
        auto channel = LogChannel<Row>::Create(options, format_row);
        channel->Open((dir / "f.csv").string(), "");
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (uint64_t i = 0; i < 1000; ++i) channel->Emit({i, t});
            });
        }
        for (auto& thread : threads) thread.join();
        expect(channel->Retire(wait), "multi retire");
        const auto c = channel->counters();
        expect(c.written + c.dropped_queue_full == 4000, "multi accounted for every row");
        expect(c.dropped_queue_full == 0, "multi no drops with a large ring");
    }

    // 7. Post runs on the writer, after the Close queued before it; FlushAll
    //    covers every channel; Open creates missing parent folders.
    {
        auto channel = LogChannel<Row>::Create({"post", 64}, format_row);
        const fs::path nested = dir / "new_folder" / "g.csv";
        channel->Open(nested.string(), "h\n");
        channel->Emit({1, 2});
        channel->Close();
        std::string seen;
        std::thread::id job_thread;
        channel->Post([&] {
            seen = slurp(nested);
            job_thread = std::this_thread::get_id();
        });
        expect(LogWriter::Instance().FlushAll(wait), "FlushAll applied");
        expect(seen == "h\n1,2\n", "Post ran after the file was closed and complete");
        expect(job_thread != std::this_thread::get_id(), "Post ran on the writer thread");
        expect(channel->Retire(wait), "post retire");
    }

    // 8. SealFolder (finalize, before hashing): the open file under the folder
    //    is flushed and closed, later records are discarded, a later Open
    //    there is refused without truncating, other folders are untouched.
    {
        const fs::path sealed = dir / "sealed_recording";
        auto inside = LogChannel<Row>::Create({"sealed_inside", 64}, format_row);
        auto outside = LogChannel<Row>::Create({"sealed_outside", 64}, format_row);
        inside->Open((sealed / "perf.csv").string(), "h\n");  // queued before the seal: honoured
        outside->Open((dir / "sealed_recording_next" / "perf.csv").string(), "h\n");
        inside->Emit({1, 1});
        outside->Emit({1, 1});
        expect(LogWriter::Instance().SealFolder(sealed.string() + "/", wait), "SealFolder applied");
        expect(slurp(sealed / "perf.csv") == "h\n1,1\n", "sealed file complete at the seal");
        inside->Emit({2, 2});  // after the digest: must not reach the file
        inside->Open((sealed / "perf.csv").string(), "h\n");  // must not truncate
        inside->Emit({3, 3});
        outside->Emit({2, 2});
        expect(LogWriter::Instance().FlushAll(wait), "FlushAll after the seal");
        expect(slurp(sealed / "perf.csv") == "h\n1,1\n", "sealed file unchanged after the seal");
        const auto c = inside->counters();
        expect(c.discarded_no_file == 2 && c.opens_refused_sealed == 1, "sealed rows discarded, reopen refused");
        expect(slurp(dir / "sealed_recording_next" / "perf.csv") == "h\n1,1\n2,2\n",
               "a sibling folder with the same name prefix is not sealed");
        const std::string key = LogWriter::FolderKey(sealed.string() + "/");
        expect(LogWriter::PathUnder((sealed / "x" / "y.csv").string(), key) &&
               !LogWriter::PathUnder(sealed.string(), key) &&
               !LogWriter::PathUnder((dir / "sealed_recording_next" / "perf.csv").string(), key),
               "PathUnder covers paths under the folder only");
        expect(inside->Retire(wait) && outside->Retire(wait), "sealed retire");
    }

    // 9. Running digest: a channel hashes what it writes; the digest becomes
    //    usable at finalize only once the folder is sealed, and only while the
    //    file on disk is what the channel closed.
    {
        using orange::gui::spatial_layout::checksum::sha256_hex;
        auto& registry = WrittenFileDigestRegistry::Instance();
        const fs::path folder = dir / "digest_recording";
        const std::string path = (folder / "Cam1_pipeline_perf.csv").string();
        auto channel = LogChannel<Row>::Create({"digest", 1 << 12}, format_row);
        channel->Open(path, "frame,value\n");
        for (uint64_t i = 1; i <= 3000; ++i) channel->Emit({i, static_cast<int64_t>(i * 7)});
        expect(channel->WaitApplied(channel->Close(), wait), "digest close");
        expect(!registry.Sealed(path), "a closed but unsealed file has no usable digest");
        expect(LogWriter::Instance().SealFolder(folder.string(), wait), "digest seal");
        const auto digest = registry.Sealed(path);
        const std::string bytes = slurp(path);
        expect(digest && digest->sha256 == sha256_hex(bytes) && digest->bytes == bytes.size() &&
                   digest->lines == 3001 && digest->failure.empty() && !digest->tally,
               "sealed digest = sha256 / size / lines of the file bytes");
        std::string reason;
        expect(digest && VerifyAgainstDisk(*digest, path, &reason), "untouched file verifies: " + reason);
        expect(digest && digest->head == "frame,value\n" &&
                   digest->tail == bytes.substr(bytes.size() - RunningFileDigest::kTailBytes),
               "first line and tail kept");
        // Any change after the close sends finalize back to a full read.
        const auto mtime = fs::last_write_time(path);
        { std::ofstream(path, std::ios::app) << "x"; }
        expect(digest && !VerifyAgainstDisk(*digest, path, &reason) && reason.find("size") != std::string::npos,
               "an appended byte is refused: " + reason);
        fs::resize_file(path, bytes.size());
        {
            std::fstream edit(path, std::ios::in | std::ios::out | std::ios::binary);
            edit.seekp(static_cast<std::streamoff>(bytes.size() - 2));
            edit.put('9');
        }
        fs::last_write_time(path, mtime);  // even with the mtime put back
        expect(digest && !VerifyAgainstDisk(*digest, path, &reason) && reason.find("tail") != std::string::npos,
               "a same-size edit in the tail is refused: " + reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // past the coarse mtime tick
        { std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes; }
        expect(digest && !VerifyAgainstDisk(*digest, path, &reason), "a rewrite with the same bytes is refused (mtime)");
        fs::remove(path);
        { std::ofstream(dir / "elsewhere.csv", std::ios::binary) << bytes; }
        fs::create_symlink(dir / "elsewhere.csv", path);
        expect(digest && !VerifyAgainstDisk(*digest, path, &reason) && reason == "not a regular file",
               "a symlink is refused: " + reason);
        // Reopening a path drops its digest (the writer will publish a new one).
        registry.Closed(*digest);
        registry.SealUnder(LogWriter::FolderKey(folder.string()));
        expect(registry.Sealed(path).has_value(), "republished digest sealed");
        registry.Opened(path);
        expect(!registry.Sealed(path), "an open drops the published digest");
        expect(channel->Retire(wait), "digest retire");
    }

    // 10. A write the stream refuses poisons the digest: never usable.
    {
        RunningFileDigest digest;
        digest.Append("a,b\n");
        digest.Fail("write failed");
        digest.Append("c,d\n");
        const fs::path path = dir / "poisoned.csv";
        { std::ofstream(path, std::ios::binary) << "a,b\n"; }
        const WrittenFileDigest frozen = digest.Freeze(path.string(), "test");
        std::string reason;
        expect(frozen.bytes == 4 && !VerifyAgainstDisk(frozen, path.string(), &reason) &&
                   reason.find("write failed") != std::string::npos,
               "a failed write is never verified: " + reason);
    }

    // 11. Cost on the writer thread: hashing as the channel writes. Rows in
    //     64 KiB chunks (the channel's write size) and ~900-byte lines (an
    //     event log line); printed per MB, bounded loosely so a regression to
    //     something expensive fails.
    {
        std::string row;
        for (int i = 0; i < 9; ++i) row += "12345678901234567890,123456789012345678901234567890,1.234,5.678,0.000,";
        row.back() = '\n';
        std::string chunk;
        while (chunk.size() < 64 * 1024) chunk += row;
        const size_t total = 64u << 20;
        for (const auto& [label, piece] : {std::pair<const char*, const std::string*>{"64 KiB chunks", &chunk},
                                           std::pair<const char*, const std::string*>{"single lines", &row}}) {
            RunningFileDigest digest;
            const auto start = std::chrono::steady_clock::now();
            for (size_t written = 0; written < total; written += piece->size()) digest.Append(*piece);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const double ms_per_mb = ms / (static_cast<double>(digest.bytes()) / (1 << 20));
            std::cout << "running digest, " << label << ": " << ms_per_mb << " ms per MiB ("
                      << 1000.0 / ms_per_mb << " MiB/s)" << std::endl;
            expect(ms_per_mb < 50.0, std::string("running digest cost per MiB is bounded: ") + label);
        }
    }

    fs::remove_all(dir);
    if (failures) {
        std::cerr << failures << " log writer test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "timing log writer tests passed" << std::endl;
    return 0;
}
