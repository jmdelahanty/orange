// src/timing/written_file_digest.h
//
// Hash while writing (2026-10-18). Finalize used to re-read and SHA-256 every
// per-frame log of a recording after the stop (110-117 s of a 120 s GUI
// finalize after a 90-minute four-camera recording). Instead, the thread that
// writes a file (the LogWriter thread, the YOLO and pose event loggers) keeps
// a RunningFileDigest over exactly the bytes it wrote: SHA-256, byte and line
// counts, the first line and the last bytes, and for an event log the row
// tally realtime_products declares. When it closes the file it publishes the
// frozen digest here; LogWriter::SealFolder marks the folder's entries sealed
// once every writer applied the seal. Finalize uses a sealed entry only after
// VerifyAgainstDisk (same inode, size and mtime as at close, regular file, same
// first line and tail); anything else re-reads the file as before.
//
// Writers only: RunningFileDigest (one per open file, writer thread only).
// Any thread: the registry (mutex; touched at open, close, seal, finalize).
#pragma once

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"

namespace orange::timing {

// "path" without trailing separators, lexically normal (the registry key and
// LogWriter::FolderKey).
inline std::string NormalPathKey(const std::string& path)
{
    std::string key = std::filesystem::path(path).lexically_normal().string();
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

// True if `path` lies strictly under the folder `key` (a NormalPathKey).
inline bool PathUnderKey(const std::string& path, const std::string& key)
{
    const std::string candidate = NormalPathKey(path);
    return !key.empty() && candidate.size() > key.size() &&
           candidate.compare(0, key.size(), key) == 0 && candidate[key.size()] == '/';
}

// The rows of a Cam<serial>_yolo_events.jsonl / _pose_events.jsonl as
// recording_session.json realtime_products declares them. Filled line by line
// by event_log_reader::tally_event_line, on the logger thread as each line is
// written, or by realtime_products when it re-reads a log.
struct EventLogTally {
    bool first_line_seen = false;
    std::string schema_id;     // declared by the first line
    int schema_version = 0;
    uint64_t header_rows = 0;  // lines of the schema that are not frame lines
    uint64_t row_count = 0;    // frame lines
    uint64_t parse_errors = 0; // lines that are not JSON or do not match the schema
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    std::map<std::string, uint64_t> rows_by_status;
};

// What a writer wrote into one file, frozen when it closed the file.
struct WrittenFileDigest {
    std::string path;     // NormalPathKey of the path the writer opened
    std::string writer;   // channel or logger name
    std::string sha256;   // lowercase hex of every byte written since the truncating open
    uint64_t bytes = 0;
    uint64_t lines = 0;
    std::string failure;  // non-empty: a write, close or stat failed; never usable
    // File identity right after the close.
    uint64_t device = 0;
    uint64_t inode = 0;
    int64_t mtime_ns = 0;
    std::string head;     // the first line with its newline (at most kHeadBytes)
    std::string tail;     // the last bytes written (at most kTailBytes)
    std::optional<EventLogTally> tally;
    bool sealed = false;  // LogWriter::SealFolder completed for its folder
};

// Writer thread only: the running state of one open file.
class RunningFileDigest {
public:
    static constexpr size_t kHeadBytes = 64 * 1024;
    static constexpr size_t kTailBytes = 4096;

    // A truncating open starts over.
    void Reset() { *this = RunningFileDigest(); }

    // Bytes the stream accepted, in order (after a write that did not fail).
    void Append(const char* data, size_t size)
    {
        if (!failure_.empty() || size == 0) {
            return;
        }
        sha_.update(reinterpret_cast<const std::uint8_t*>(data), size);
        bytes_ += size;
        lines_ += static_cast<uint64_t>(std::count(data, data + size, '\n'));
        if (!head_complete_) {
            const char* newline = std::find(data, data + size, '\n');
            const size_t take = std::min<size_t>(
                static_cast<size_t>(newline - data) + (newline != data + size ? 1 : 0),
                kHeadBytes - head_.size());
            head_.append(data, take);
            head_complete_ = newline != data + size || head_.size() >= kHeadBytes;
        }
        if (size >= kTailBytes) {
            tail_.assign(data + size - kTailBytes, kTailBytes);
        } else {
            tail_.append(data, size);
            if (tail_.size() > 2 * kTailBytes) {
                tail_.erase(0, tail_.size() - kTailBytes);
            }
        }
    }
    void Append(const std::string& bytes) { Append(bytes.data(), bytes.size()); }

    // The bytes on disk may differ from the bytes hashed from now on.
    void Fail(const std::string& reason)
    {
        if (failure_.empty()) {
            failure_ = reason.empty() ? "write failed" : reason;
        }
    }
    bool failed() const { return !failure_.empty(); }
    uint64_t bytes() const { return bytes_; }

    // Event logs: the row tally, carried with the digest.
    EventLogTally& tally()
    {
        if (!tally_) tally_.emplace();
        return *tally_;
    }

    // After the file is closed: the digest as of now, with the file's identity.
    WrittenFileDigest Freeze(const std::string& path, const std::string& writer) const
    {
        WrittenFileDigest out;
        out.path = NormalPathKey(path);
        out.writer = writer;
        auto sha = sha_;  // final_hex consumes the state; the running copy stays usable
        out.sha256 = sha.final_hex();
        out.bytes = bytes_;
        out.lines = lines_;
        out.failure = failure_;
        out.head = head_;
        out.tail = tail_.size() > kTailBytes ? tail_.substr(tail_.size() - kTailBytes) : tail_;
        out.tally = tally_;
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            if (out.failure.empty()) out.failure = "stat after close failed";
        } else {
            out.device = static_cast<uint64_t>(st.st_dev);
            out.inode = static_cast<uint64_t>(st.st_ino);
            out.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
            if (out.failure.empty() && static_cast<uint64_t>(st.st_size) != bytes_) {
                out.failure = "size at close " + std::to_string(st.st_size) + " != bytes written " +
                              std::to_string(bytes_);
            }
        }
        return out;
    }

private:
    orange::gui::spatial_layout::checksum::StreamingSha256 sha_;
    uint64_t bytes_ = 0;
    uint64_t lines_ = 0;
    std::string head_;
    bool head_complete_ = false;
    std::string tail_;
    std::string failure_;
    std::optional<EventLogTally> tally_;
};

// Process-wide: the digests writers published at close.
class WrittenFileDigestRegistry {
public:
    static WrittenFileDigestRegistry& Instance()
    {
        // Never destroyed: writer threads publish while the process exits
        // (LogWriter's destructor joins its thread, which closes every file).
        static WrittenFileDigestRegistry* registry = new WrittenFileDigestRegistry();
        return *registry;
    }

    // Writer, before it opens `path` (truncating or appending): whatever was
    // published for it no longer describes the file.
    void Opened(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(NormalPathKey(path));
    }

    // Writer, after it closed the file.
    void Closed(WrittenFileDigest digest)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = digest.path;
        entries_[std::move(key)] = std::move(digest);
    }

    // LogWriter::SealFolder, once every writer applied the seal of `folder_key`
    // (a NormalPathKey): no writer of this process opens a file there again.
    void SealUnder(const std::string& folder_key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [path, digest] : entries_) {
            if (PathUnderKey(path, folder_key)) {
                digest.sealed = true;
            }
        }
    }

    // Finalize: the sealed digest of `path`, if this process wrote and sealed it.
    std::optional<WrittenFileDigest> Sealed(const std::string& path) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(NormalPathKey(path));
        if (it == entries_.end() || !it->second.sealed) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    WrittenFileDigestRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, WrittenFileDigest> entries_;
};

// Finalize: true when `path` still holds what the writer hashed. Cheap: one
// lstat and two short reads (the first line and the last kTailBytes). The
// file must be a regular file (not a symlink) with the device, inode, size and
// mtime it had right after the writer closed it, and the same first line and
// tail. `reason` says why not.
inline bool VerifyAgainstDisk(const WrittenFileDigest& digest, const std::string& path,
                              std::string* reason)
{
    auto refuse = [&](const std::string& why) {
        if (reason) *reason = why;
        return false;
    };
    if (!digest.failure.empty()) return refuse("writer: " + digest.failure);
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return refuse("lstat failed");
    if (!S_ISREG(st.st_mode)) return refuse("not a regular file");
    const int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (static_cast<uint64_t>(st.st_dev) != digest.device || static_cast<uint64_t>(st.st_ino) != digest.inode) {
        return refuse("file replaced since the writer closed it");
    }
    if (static_cast<uint64_t>(st.st_size) != digest.bytes) {
        return refuse("size " + std::to_string(st.st_size) + " != bytes hashed " + std::to_string(digest.bytes));
    }
    if (mtime_ns != digest.mtime_ns) return refuse("modified since the writer closed it");
    std::ifstream in(path, std::ios::binary);
    if (!in) return refuse("cannot open");
    std::string head(digest.head.size(), '\0');
    if (!head.empty() && !in.read(head.data(), static_cast<std::streamsize>(head.size()))) {
        return refuse("cannot read the first line");
    }
    if (head != digest.head) return refuse("first line differs from the bytes written");
    std::string tail(digest.tail.size(), '\0');
    if (!tail.empty()) {
        in.seekg(static_cast<std::streamoff>(digest.bytes - tail.size()));
        if (!in.read(tail.data(), static_cast<std::streamsize>(tail.size()))) {
            return refuse("cannot read the tail");
        }
    }
    if (tail != digest.tail) return refuse("tail differs from the bytes written");
    return true;
}

// Finalize: how each declared file's digest was obtained (the writer's running
// digest checked against the disk, or a full re-read), for the finalize timing
// report. Orange-internal: nothing of it goes into a published block.
struct DigestSourceReport {
    nlohmann::json files = nlohmann::json::array();
    uint64_t writer_files = 0;
    uint64_t reread_files = 0;
    uint64_t writer_bytes = 0;
    uint64_t reread_bytes = 0;

    void Add(const std::string& name, bool from_writer, uint64_t bytes, const std::string& reason)
    {
        nlohmann::json entry = {{"path", name},
                                {"digest_source", from_writer ? "writer_running_digest" : "finalize_reread"},
                                {"size_bytes", bytes}};
        if (!from_writer) entry["reason"] = reason;
        files.push_back(std::move(entry));
        (from_writer ? writer_files : reread_files)++;
        (from_writer ? writer_bytes : reread_bytes) += bytes;
    }

    nlohmann::json ToJson() const
    {
        return {{"writer_running_digest_files", writer_files},
                {"writer_running_digest_bytes", writer_bytes},
                {"finalize_reread_files", reread_files},
                {"finalize_reread_bytes", reread_bytes},
                {"files", files}};
    }
};

}  // namespace orange::timing
