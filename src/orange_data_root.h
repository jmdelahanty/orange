#pragma once

// The person's Orange data root (~/orange_data of whoever started Orange), one
// rule for every caller (docs/opt_install_plan_2026-11-14.md, step c). Python
// mirrors it in scripts/orange_paths.py; keep the two in step.
//
//   1. ORANGE_DATA_ROOT, when set: an override for tests. The sudo wrappers do
//      not forward it, so release runs never see it.
//   2. Under sudo (SUDO_USER set): that user's home from the passwd database.
//   3. Running as root without SUDO_USER: refuse. Root's own home is never an
//      Orange data root.
//   4. Otherwise: the real uid's home from the passwd database (not $HOME).
// The root is <home>/orange_data. Shared data on the data drive (recordings,
// rig calibrations) comes from the installed host configuration later (plan
// phase 4); this resolver is only the per-person root.

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace orange::data_root {

// Empty on failure, with the reason in *error_out.
inline std::filesystem::path resolve(std::string* error_out = nullptr)
{
    auto fail = [error_out](const std::string& message) {
        if (error_out) {
            *error_out = message;
        }
        return std::filesystem::path();
    };
    if (error_out) {
        error_out->clear();
    }
    if (const char* override_root = std::getenv("ORANGE_DATA_ROOT");
        override_root != nullptr && override_root[0] != '\0') {
        return std::filesystem::path(override_root);
    }
    if (const char* sudo_user = std::getenv("SUDO_USER");
        sudo_user != nullptr && sudo_user[0] != '\0') {
        const passwd* pw = ::getpwnam(sudo_user);
        if (pw == nullptr || pw->pw_dir == nullptr || pw->pw_dir[0] == '\0') {
            return fail(std::string("cannot resolve the home of SUDO_USER `") + sudo_user + "`");
        }
        return std::filesystem::path(pw->pw_dir) / "orange_data";
    }
    if (::getuid() == 0) {
        return fail("running as root without SUDO_USER: refusing to use root's home as the "
                    "Orange data root (start Orange through sudo from your own account)");
    }
    const passwd* pw = ::getpwuid(::getuid());
    if (pw == nullptr || pw->pw_dir == nullptr || pw->pw_dir[0] == '\0') {
        return fail("cannot resolve the home of uid " + std::to_string(::getuid()));
    }
    return std::filesystem::path(pw->pw_dir) / "orange_data";
}

}  // namespace orange::data_root
