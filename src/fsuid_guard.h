#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <sys/types.h>

#ifdef __linux__
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace orange {

// The invoking user's ids when Orange runs as root under sudo (SUDO_UID /
// SUDO_GID); false when not root, not under sudo, or the invoker is root.
inline bool sudo_invoker_ids(uid_t* uid_out, gid_t* gid_out) {
#ifdef __linux__
    if (geteuid() != 0) {
        return false;
    }
    const char* uid_env = std::getenv("SUDO_UID");
    const char* gid_env = std::getenv("SUDO_GID");
    if (!uid_env || !gid_env) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long uid_ul = std::strtoul(uid_env, &end, 10);
    if (errno != 0 || end == uid_env) {
        return false;
    }
    errno = 0;
    const unsigned long gid_ul = std::strtoul(gid_env, &end, 10);
    if (errno != 0 || end == gid_env) {
        return false;
    }
    const uid_t uid = static_cast<uid_t>(uid_ul);
    const gid_t gid = static_cast<gid_t>(gid_ul);
    if (uid == 0 && gid == 0) {
        return false;
    }
    *uid_out = uid;
    *gid_out = gid;
    return true;
#else
    (void)uid_out;
    (void)gid_out;
    return false;
#endif
}

// Hands every root-owned entry under `root` (and `root` itself) to the sudo
// invoker, without following symlinks and without touching modes. The
// backstop for writers that ran outside a ScopedFsuid (setfsuid is per
// thread, so every writer needs its own guard). Returns the number of entries
// changed; a no-op when not running as root under sudo.
inline std::size_t normalize_recording_ownership(const std::filesystem::path& root) {
    std::size_t changed = 0;
#ifdef __linux__
    uid_t uid = 0;
    gid_t gid = 0;
    if (root.empty() || !sudo_invoker_ids(&uid, &gid)) {
        return 0;
    }
    auto fix = [&](const std::filesystem::path& path) {
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0 || st.st_uid != 0) {
            return;
        }
        if (::lchown(path.c_str(), uid, gid) == 0) {
            ++changed;
        }
    };
    fix(root);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        fix(it->path());
    }
#else
    (void)root;
#endif
    return changed;
}

class ScopedFsuid {
public:
    ScopedFsuid() {
#ifdef __linux__
        uid_t target_uid = 0;
        gid_t target_gid = 0;
        if (!sudo_invoker_ids(&target_uid, &target_gid)) {
            return;
        }

        prev_fsgid_ = setfsgid(target_gid);
        prev_fsuid_ = setfsuid(target_uid);
        active_ = true;
#endif
    }

    ~ScopedFsuid() {
#ifdef __linux__
        if (!active_) {
            return;
        }
        setfsuid(prev_fsuid_);
        setfsgid(prev_fsgid_);
#endif
    }

    bool active() const { return active_; }

private:
#ifdef __linux__
    uid_t prev_fsuid_{0};
    gid_t prev_fsgid_{0};
#endif
    bool active_{false};
};
} // namespace orange
