#pragma once

#include <cerrno>
#include <cstdlib>
#include <sys/types.h>

#ifdef __linux__
#include <sys/fsuid.h>
#include <unistd.h>
#endif

namespace orange {
class ScopedFsuid {
public:
    ScopedFsuid() {
#ifdef __linux__
        if (geteuid() != 0) {
            return;
        }

        const char* uid_env = std::getenv("SUDO_UID");
        const char* gid_env = std::getenv("SUDO_GID");
        if (!uid_env || !gid_env) {
            return;
        }

        char* end = nullptr;
        errno = 0;
        unsigned long uid_ul = std::strtoul(uid_env, &end, 10);
        if (errno != 0 || end == uid_env) {
            return;
        }
        errno = 0;
        unsigned long gid_ul = std::strtoul(gid_env, &end, 10);
        if (errno != 0 || end == gid_env) {
            return;
        }

        uid_t target_uid = static_cast<uid_t>(uid_ul);
        gid_t target_gid = static_cast<gid_t>(gid_ul);
        if (target_uid == 0 && target_gid == 0) {
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
