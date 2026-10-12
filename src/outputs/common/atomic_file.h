#pragma once

// One way to write an output file so readers never see a partial one: bytes go to a unique
// temporary next to the target, then rename() replaces the target. Durability is the caller's
// explicit choice (Durability below), because fsync during a recording can force the kernel to
// flush unrelated dirty recorder data (see AGENTS.md, page-cache writeback).
//
// Callers that need the files owned by the sudo user keep their own orange::ScopedFsuid.

#include "json.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace orange::outputs {

enum class Durability {
    // rename() is atomic; contents reach disk with normal writeback. For files updated while
    // recording (status, summaries, snapshots).
    kRenameOnly,
    // fsync the file before the rename and the directory after it. For finalized documents that
    // must survive a power loss once the call returns (manifests, receipts, finalization records).
    kFsync,
};

struct AtomicWriteOptions {
    Durability durability = Durability::kRenameOnly;
    // Mode of the new file; unset keeps the 0644-before-umask default.
    std::optional<std::filesystem::perms> perms;
    bool create_parent_directories = true;
    // Owner of the new file (fchown before the rename), e.g. the sudo invoker for root writers.
    std::optional<std::pair<uid_t, gid_t>> owner;

    static AtomicWriteOptions durable() {
        AtomicWriteOptions options;
        options.durability = Durability::kFsync;
        return options;
    }
};

namespace detail {

inline bool set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

inline bool write_all(int fd, const std::string& bytes, std::string* error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return set_error(error, std::string("write failed: ") + std::strerror(errno));
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

inline std::filesystem::path unique_temporary(const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> sequence{0};
    return target.parent_path() /
           ("." + target.filename().string() + "." + std::to_string(::getpid()) + "." +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
}

}  // namespace detail

// Replaces `path` with `bytes` atomically. On failure the target is unchanged and no temporary is
// left behind.
inline bool write_file_atomic(const std::filesystem::path& path, const std::string& bytes,
                              const AtomicWriteOptions& options = {}, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    if (error) {
        error->clear();
    }
    if (path.empty() || path.filename().empty()) {
        return detail::set_error(error, "atomic write needs a file path");
    }
    fs::path parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::error_code ec;
    if (options.create_parent_directories) {
        fs::create_directories(parent, ec);
        if (ec) {
            return detail::set_error(error, "cannot create " + parent.string() + ": " + ec.message());
        }
    }
    const fs::path temporary = detail::unique_temporary(parent / path.filename());
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        return detail::set_error(error, "cannot create " + temporary.string() + ": " + std::strerror(errno));
    }
    bool ok = detail::write_all(fd, bytes, error);
    if (ok && options.owner && ::fchown(fd, options.owner->first, options.owner->second) != 0) {
        ok = detail::set_error(error, std::string("cannot set owner: ") + std::strerror(errno));
    }
    if (ok && options.durability == Durability::kFsync && ::fsync(fd) != 0) {
        ok = detail::set_error(error, std::string("fsync failed: ") + std::strerror(errno));
    }
    if (::close(fd) != 0 && ok) {
        ok = detail::set_error(error, std::string("close failed: ") + std::strerror(errno));
    }
    if (ok && options.perms) {
        fs::permissions(temporary, *options.perms, fs::perm_options::replace, ec);
        if (ec) {
            ok = detail::set_error(error, "cannot set permissions: " + ec.message());
        }
    }
    if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) {
        ok = detail::set_error(error, "cannot replace " + path.string() + ": " + std::strerror(errno));
    }
    if (!ok) {
        fs::remove(temporary, ec);
        return false;
    }
    if (options.durability == Durability::kFsync) {
        // The file is already in place; a failure here means it may not survive a power loss.
        const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) {
            return detail::set_error(error, "replaced, but cannot open " + parent.string() + " to fsync");
        }
        const bool synced = ::fsync(dir) == 0;
        const int saved = errno;
        ::close(dir);
        if (!synced) {
            return detail::set_error(error, std::string("replaced, but directory fsync failed: ") +
                                                std::strerror(saved));
        }
    }
    return true;
}

// The JSON text form every Orange JSON output uses: 2-space indent and a trailing newline.
inline std::string json_file_text(const nlohmann::json& value) {
    return value.dump(2) + "\n";
}

inline bool write_json_atomic(const std::filesystem::path& path, const nlohmann::json& value,
                              const AtomicWriteOptions& options = {}, std::string* error = nullptr) {
    return write_file_atomic(path, json_file_text(value), options, error);
}

}  // namespace orange::outputs
