// src/timing/log_writer.h
//
// One writer thread per process, one bounded queue per log file.
// Design: docs/unified_timing_and_diagnostics_design_2026_10_11.md, section 5.2.
//
// Hot side (acquisition, YOLO, pose, handoff threads):
//   LogChannel<Record>::Emit(record) copies the record into the channel's
//   bounded single-producer ring. No allocation, no formatting, no lock, no
//   syscall. A full ring drops the record and counts it; it never blocks.
//   Open/Rotate/Close only queue a control entry (a small allocation for the
//   path, at folder changes only); the file work happens on the writer thread.
//
// Writer side: LogWriter::Instance() owns one thread at normal priority off
// the isolated CPUs. Every few milliseconds it drains every channel: applies
// control entries in order with the records (a record emitted before a
// rotation lands in the old file), formats rows with the channel's formatter,
// writes, and flushes each file on a timer and at close. Files are created
// under ScopedFsuid, so they belong to the sudo invoker.
//
// A slow file fills only its own ring; other channels keep draining.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fsuid_guard.h"
#include "timing/clock.h"

namespace orange::timing {

struct LogChannelCounters {
    uint64_t offered = 0;
    uint64_t written = 0;
    uint64_t dropped_queue_full = 0;   // ring full at Emit
    uint64_t discarded_no_file = 0;    // emitted while no file was open (or open failed)
    uint64_t queue_high_water = 0;
    uint64_t files_opened = 0;
    uint64_t open_failures = 0;
    uint64_t write_failures = 0;
};

// One file a channel wrote (or failed to open), kept for the recording's
// diagnostics_index.json. Row counts are those of this file only; a drop is
// attributed to the file that was current when the writer saw it.
struct LogFileRecord {
    std::string path;
    std::string channel;
    std::string header_line;        // first line of the header (the CSV columns)
    uint64_t rows_written = 0;
    uint64_t rows_dropped_queue_full = 0;
    uint64_t rows_discarded_no_file = 0;
    uint64_t write_failures = 0;
    bool open_failed = false;
    uint64_t opened_realtime_ns = 0;
    uint64_t closed_realtime_ns = 0;
};

class LogChannelBase {
public:
    virtual ~LogChannelBase() = default;
    // Writer thread only: the file currently open ("" when none).
    virtual const std::string& OpenPath() const = 0;
    // Any thread: queue a flush barrier / wait for it (not on a hot thread).
    virtual uint64_t Flush() = 0;
    virtual bool WaitApplied(uint64_t ticket, std::chrono::milliseconds timeout) = 0;
    // Writer thread only. Returns true if it did any work.
    virtual bool Service(std::chrono::steady_clock::time_point now) = 0;
    // Writer thread only, at shutdown: drain, flush and close.
    virtual void Shutdown() = 0;
};

// The process-wide writer thread.
class LogWriter {
public:
    static LogWriter& Instance()
    {
        static LogWriter writer;
        return writer;
    }

    void Register(const std::shared_ptr<LogChannelBase>& channel)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.push_back(channel);
        if (!thread_.joinable()) {
            running_.store(true, std::memory_order_release);
            thread_ = std::thread(&LogWriter::Run, this);
        }
    }

    void Unregister(const LogChannelBase* channel)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                       [channel](const std::shared_ptr<LogChannelBase>& c) {
                                           return c.get() == channel;
                                       }),
                        channels_.end());
    }

    // Not for hot threads: every record emitted, every Open/Close/Post queued
    // before this call is applied, written and flushed when it returns true.
    // Finalize calls it before hashing or reading the per-frame logs.
    bool FlushAll(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
    {
        std::vector<std::shared_ptr<LogChannelBase>> channels;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            channels = channels_;
        }
        std::vector<uint64_t> tickets;
        tickets.reserve(channels.size());
        for (const auto& channel : channels) {
            tickets.push_back(channel->Flush());
        }
        bool ok = true;
        for (size_t i = 0; i < channels.size(); ++i) {
            ok = channels[i]->WaitApplied(tickets[i], timeout) && ok;
        }
        return ok;
    }

    // Files written by every channel, kept per recording folder for the
    // diagnostics index (any thread).
    void RecordFile(LogFileRecord record)
    {
        std::lock_guard<std::mutex> lock(records_mutex_);
        files_.push_back(std::move(record));
    }
    std::vector<LogFileRecord> FilesUnder(const std::string& folder) const
    {
        std::vector<LogFileRecord> out;
        const std::string prefix = folder.empty() || folder.back() == '/' ? folder : folder + "/";
        std::lock_guard<std::mutex> lock(records_mutex_);
        for (const auto& file : files_) {
            if (file.path.compare(0, prefix.size(), prefix) == 0) {
                out.push_back(file);
            }
        }
        return out;
    }

    // Monotonic/realtime pairs per recording folder: at recording start and
    // stop (callers), and once a minute for every folder a channel is
    // writing into (the writer thread).
    struct LabelledAnchor {
        std::string label;
        ClockAnchor anchor;
    };
    void AddAnchor(const std::string& folder, const std::string& label)
    {
        if (folder.empty()) {
            return;
        }
        const ClockAnchor anchor = anchor_now();
        std::lock_guard<std::mutex> lock(records_mutex_);
        anchors_[folder].push_back({label, anchor});
    }
    // Adds an anchor only when the folder has none yet (its first log file).
    void AddFirstAnchor(const std::string& folder, const std::string& label)
    {
        if (folder.empty()) {
            return;
        }
        const ClockAnchor anchor = anchor_now();
        std::lock_guard<std::mutex> lock(records_mutex_);
        auto& list = anchors_[folder];
        if (list.empty()) {
            list.push_back({label, anchor});
        }
    }
    std::vector<LabelledAnchor> AnchorsFor(const std::string& folder) const
    {
        std::lock_guard<std::mutex> lock(records_mutex_);
        const auto it = anchors_.find(folder);
        return it == anchors_.end() ? std::vector<LabelledAnchor>{} : it->second;
    }
    // Drops what was kept for a folder once its index is written.
    void ForgetFolder(const std::string& folder)
    {
        const std::string prefix = folder.empty() || folder.back() == '/' ? folder : folder + "/";
        std::lock_guard<std::mutex> lock(records_mutex_);
        anchors_.erase(folder);
        files_.erase(std::remove_if(files_.begin(), files_.end(),
                                    [&](const LogFileRecord& f) {
                                        return f.path.compare(0, prefix.size(), prefix) == 0;
                                    }),
                     files_.end());
    }

    static constexpr std::chrono::seconds kAnchorInterval{60};

    // Wakes the writer now instead of at its next poll (control entries).
    void Wake() { wake_.store(true, std::memory_order_release); }

    static constexpr std::chrono::milliseconds kPollInterval{5};

    ~LogWriter()
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    LogWriter() = default;

    // Same rule as AsyncLineSink: normal priority, never on an isolated CPU,
    // whatever the thread that first registered a channel was running as.
    static void DetachFromCreatorScheduling()
    {
        sched_param param{};
        param.sched_priority = 0;
        (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
        cpu_set_t mask;
        CPU_ZERO(&mask);
        const long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
        for (long cpu = 0; cpu < cpu_count && cpu < CPU_SETSIZE; ++cpu) {
            CPU_SET(static_cast<int>(cpu), &mask);
        }
        std::ifstream isolated("/sys/devices/system/cpu/isolated");
        std::string spec;
        if (isolated && std::getline(isolated, spec)) {
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t comma = spec.find(',', pos);
                if (comma == std::string::npos) comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                if (item.empty()) continue;
                const size_t dash = item.find('-');
                const int lo = std::atoi(item.substr(0, dash).c_str());
                const int hi =
                    dash == std::string::npos ? lo : std::atoi(item.substr(dash + 1).c_str());
                for (int cpu = lo; cpu <= hi && cpu < CPU_SETSIZE; ++cpu) {
                    CPU_CLR(cpu, &mask);
                }
            }
        }
        if (CPU_COUNT(&mask) > 0) {
            (void)pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
        }
        (void)pthread_setname_np(pthread_self(), "orange_logwr");
    }

    void Run()
    {
        DetachFromCreatorScheduling();
        while (running_.load(std::memory_order_acquire)) {
            std::vector<std::shared_ptr<LogChannelBase>> channels;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                channels = channels_;
            }
            const auto now = std::chrono::steady_clock::now();
            bool worked = false;
            for (const auto& channel : channels) {
                worked = channel->Service(now) || worked;
            }
            if (now - last_anchor_ >= kAnchorInterval) {
                last_anchor_ = now;
                std::vector<std::string> folders;
                for (const auto& channel : channels) {
                    const std::string& path = channel->OpenPath();
                    if (path.empty()) {
                        continue;
                    }
                    const std::string folder = std::filesystem::path(path).parent_path().string();
                    if (std::find(folders.begin(), folders.end(), folder) == folders.end()) {
                        folders.push_back(folder);
                    }
                }
                for (const auto& folder : folders) {
                    AddAnchor(folder, "periodic");
                }
            }
            if (!worked && !wake_.exchange(false, std::memory_order_acq_rel)) {
                std::this_thread::sleep_for(kPollInterval);
            }
        }
        std::vector<std::shared_ptr<LogChannelBase>> channels;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            channels = channels_;
        }
        for (const auto& channel : channels) {
            channel->Shutdown();
        }
    }

    std::mutex mutex_;
    std::vector<std::shared_ptr<LogChannelBase>> channels_;
    mutable std::mutex records_mutex_;
    std::vector<LogFileRecord> files_;
    std::map<std::string, std::vector<LabelledAnchor>> anchors_;
    std::chrono::steady_clock::time_point last_anchor_ = std::chrono::steady_clock::now();
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
};

struct LogChannelOptions {
    std::string name;                         // for diagnostics and the index
    size_t capacity = 8192;                   // records; rounded up to a power of two
    std::chrono::milliseconds flush_interval{250};
    bool multi_producer = false;              // serialise Emit with a mutex
};

// One log file (rotating across recording folders) fed by one ring.
template <typename Record>
class LogChannel : public LogChannelBase,
                   public std::enable_shared_from_this<LogChannel<Record>> {
public:
    using Formatter = std::function<void(const Record&, std::string&)>;

    static std::shared_ptr<LogChannel> Create(LogChannelOptions options, Formatter formatter)
    {
        std::shared_ptr<LogChannel> channel(new LogChannel(std::move(options), std::move(formatter)));
        LogWriter::Instance().Register(channel);
        return channel;
    }

    ~LogChannel() override = default;

    // Hot side. Never blocks, never allocates (single producer).
    bool Emit(const Record& record)
    {
        std::unique_lock<std::mutex> lock(emit_mutex_, std::defer_lock);
        if (options_.multi_producer) {
            lock.lock();
        }
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= ring_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ring_[head & mask_] = record;
        head_.store(head + 1, std::memory_order_release);
        const uint64_t depth = head + 1 - tail;
        if (depth > high_water_.load(std::memory_order_relaxed)) {
            high_water_.store(depth, std::memory_order_relaxed);
        }
        return true;
    }

    // Closes the current file (if any) and opens `path`, writing `header`.
    // Records emitted before this call go to the previous file.
    void Open(const std::string& path, const std::string& header)
    {
        PushControl(Control::kOpen, path, header);
    }

    // Closes the current file after every record emitted before this call.
    // Returns a ticket for WaitApplied.
    uint64_t Close() { return PushControl(Control::kClose, {}, {}); }

    // Flush barrier: every record emitted before this call is written and
    // flushed once the ticket is applied.
    uint64_t Flush() override { return PushControl(Control::kFlush, {}, {}); }

    // Runs `job` on the writer thread, in order with the records and the other
    // control entries (for example a summary written after Close). For work
    // that must not run on a hot thread; never for per-record work.
    uint64_t Post(std::function<void()> job)
    {
        return PushControl(Control::kRun, {}, {}, std::move(job));
    }

    // Not for hot threads: waits until the writer applied `ticket`.
    bool WaitApplied(uint64_t ticket, std::chrono::milliseconds timeout) override
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (applied_.load(std::memory_order_acquire) < ticket) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Close, wait, and detach from the writer. Call before destroying the
    // owner; not from a hot thread.
    bool Retire(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        const bool ok = WaitApplied(Close(), timeout);
        LogWriter::Instance().Unregister(this);
        return ok;
    }

    LogChannelCounters counters() const
    {
        LogChannelCounters c;
        c.offered = head_.load(std::memory_order_relaxed) + dropped_.load(std::memory_order_relaxed);
        c.written = written_.load(std::memory_order_relaxed);
        c.dropped_queue_full = dropped_.load(std::memory_order_relaxed);
        c.discarded_no_file = discarded_.load(std::memory_order_relaxed);
        c.queue_high_water = high_water_.load(std::memory_order_relaxed);
        c.files_opened = files_opened_.load(std::memory_order_relaxed);
        c.open_failures = open_failures_.load(std::memory_order_relaxed);
        c.write_failures = write_failures_.load(std::memory_order_relaxed);
        return c;
    }
    const std::string& name() const { return options_.name; }
    const std::string& OpenPath() const override { return file_path_; }

    // Writer thread.
    bool Service(std::chrono::steady_clock::time_point now) override
    {
        bool worked = false;
        const uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        while (true) {
            ApplyControlsUpTo(tail, &worked);
            if (tail == head) {
                break;
            }
            const Record& record = ring_[tail & mask_];
            if (file_.is_open()) {
                formatter_(record, buffer_);
                ++pending_rows_;
            } else {
                discarded_.fetch_add(1, std::memory_order_relaxed);
            }
            ++tail;
            tail_.store(tail, std::memory_order_release);
            worked = true;
            if (buffer_.size() >= kWriteChunk) {
                WriteBuffer();
            }
        }
        if (!buffer_.empty() || pending_rows_ > 0) {
            if (now - last_flush_ >= options_.flush_interval) {
                WriteBuffer();
                file_.flush();
                last_flush_ = now;
            }
        }
        return worked;
    }

    void Shutdown() override
    {
        bool worked = false;
        Service(std::chrono::steady_clock::now());
        ApplyControlsUpTo(head_.load(std::memory_order_acquire), &worked);
        CloseFile();
    }

private:
    enum class Control { kOpen, kClose, kFlush, kRun };
    struct ControlEntry {
        uint64_t position;  // apply before the record at this ring index
        uint64_t ticket;
        Control kind;
        std::string path;
        std::string header;
        std::function<void()> job;
    };
    static constexpr size_t kWriteChunk = 64 * 1024;

    LogChannel(LogChannelOptions options, Formatter formatter)
        : options_(std::move(options)), formatter_(std::move(formatter))
    {
        size_t capacity = 1;
        while (capacity < std::max<size_t>(options_.capacity, 2)) {
            capacity <<= 1;
        }
        ring_.resize(capacity);
        mask_ = capacity - 1;
        buffer_.reserve(kWriteChunk * 2);
    }

    uint64_t PushControl(Control kind, std::string path, std::string header,
                         std::function<void()> job = {})
    {
        std::unique_lock<std::mutex> emit_lock(emit_mutex_, std::defer_lock);
        if (options_.multi_producer) {
            emit_lock.lock();
        }
        uint64_t ticket = 0;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            ticket = ++issued_;
            controls_.push_back(ControlEntry{head_.load(std::memory_order_relaxed), ticket, kind,
                                             std::move(path), std::move(header), std::move(job)});
        }
        LogWriter::Instance().Wake();
        return ticket;
    }

    void ApplyControlsUpTo(uint64_t position, bool* worked)
    {
        while (true) {
            ControlEntry entry;
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                if (controls_.empty() || controls_.front().position > position) {
                    return;
                }
                entry = std::move(controls_.front());
                controls_.pop_front();
            }
            *worked = true;
            switch (entry.kind) {
            case Control::kOpen:
                CloseFile();
                OpenFile(entry.path, entry.header);
                break;
            case Control::kClose:
                CloseFile();
                break;
            case Control::kFlush:
                WriteBuffer();
                if (file_.is_open()) {
                    file_.flush();
                }
                break;
            case Control::kRun:
                if (entry.job) {
                    entry.job();
                }
                break;
            }
            applied_.store(entry.ticket, std::memory_order_release);
        }
    }

    void OpenFile(const std::string& path, const std::string& header)
    {
        orange::ScopedFsuid fsuid_guard;  // the file belongs to the sudo invoker
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        file_.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file_.is_open()) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            LogFileRecord failed;
            failed.path = path;
            failed.channel = options_.name;
            failed.header_line = header.substr(0, header.find('\n'));
            failed.open_failed = true;
            failed.opened_realtime_ns = realtime_now_ns();
            LogWriter::Instance().RecordFile(std::move(failed));
            return;
        }
        file_path_ = path;
        LogWriter::Instance().AddFirstAnchor(parent.string(), "first_log_file_opened");
        file_header_line_ = header.substr(0, header.find('\n'));
        file_opened_realtime_ns_ = realtime_now_ns();
        at_open_written_ = written_.load(std::memory_order_relaxed) + pending_rows_;
        at_open_dropped_ = dropped_.load(std::memory_order_relaxed);
        at_open_discarded_ = discarded_.load(std::memory_order_relaxed);
        at_open_write_failures_ = write_failures_.load(std::memory_order_relaxed);
        files_opened_.fetch_add(1, std::memory_order_relaxed);
        buffer_ += header;
        WriteBuffer();
        file_.flush();
    }

    void WriteBuffer()
    {
        if (buffer_.empty()) {
            return;
        }
        if (file_.is_open()) {
            file_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            if (!file_) {
                write_failures_.fetch_add(1, std::memory_order_relaxed);
                file_.clear();
            }
        }
        written_.fetch_add(pending_rows_, std::memory_order_relaxed);
        pending_rows_ = 0;
        buffer_.clear();
    }

    void CloseFile()
    {
        WriteBuffer();
        if (file_.is_open()) {
            file_.flush();
            file_.close();
            LogFileRecord record;
            record.path = file_path_;
            record.channel = options_.name;
            record.header_line = file_header_line_;
            record.rows_written = written_.load(std::memory_order_relaxed) - at_open_written_;
            record.rows_dropped_queue_full = dropped_.load(std::memory_order_relaxed) - at_open_dropped_;
            record.rows_discarded_no_file =
                discarded_.load(std::memory_order_relaxed) - at_open_discarded_;
            record.write_failures = write_failures_.load(std::memory_order_relaxed) - at_open_write_failures_;
            record.opened_realtime_ns = file_opened_realtime_ns_;
            record.closed_realtime_ns = realtime_now_ns();
            LogWriter::Instance().RecordFile(std::move(record));
        }
        file_path_.clear();
    }

    const LogChannelOptions options_;
    const Formatter formatter_;
    std::vector<Record> ring_;
    size_t mask_ = 0;
    std::mutex emit_mutex_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> high_water_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> discarded_{0};
    std::atomic<uint64_t> files_opened_{0};
    std::atomic<uint64_t> open_failures_{0};
    std::atomic<uint64_t> write_failures_{0};

    std::mutex control_mutex_;
    std::deque<ControlEntry> controls_;
    uint64_t issued_ = 0;
    std::atomic<uint64_t> applied_{0};

    // Writer thread only.
    std::ofstream file_;
    std::string file_path_;
    std::string file_header_line_;
    uint64_t file_opened_realtime_ns_ = 0;
    uint64_t at_open_written_ = 0;
    uint64_t at_open_dropped_ = 0;
    uint64_t at_open_discarded_ = 0;
    uint64_t at_open_write_failures_ = 0;
    std::string buffer_;
    uint64_t pending_rows_ = 0;
    std::chrono::steady_clock::time_point last_flush_{};
};

}  // namespace orange::timing
