#pragma once

// Support files a running Orange binary opens or runs: fonts, and the Python
// checks it starts (post-finalize validator, recorder verifier, video sanity).
// They live in the source tree for a development build and in
// <release>/share/orange for an /opt release (docs/opt_install_plan_2026-11-14.md,
// step b). The install copies fonts/, scripts/, docs/schemas/ and contracts/
// unchanged, so a relative path like "scripts/x.py" means the same file in both.
//
// Search order, first existing file wins:
//   1. <exe dir>/../share/orange   an installed release (<release>/bin/<binary>)
//   2. <exe dir>/../..             a build tree (<tree>/targets/release/<binary>)
//   3. the current folder          the old behaviour, kept last for compatibility
// Searching from the executable first means a binary always uses the support
// files of the tree or release it belongs to, whatever folder it starts in.

#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace orange::runtime_paths {

inline std::filesystem::path executable_path()
{
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
}

inline std::vector<std::filesystem::path> support_roots()
{
    std::vector<std::filesystem::path> roots;
    const std::filesystem::path exe = executable_path();
    if (!exe.empty()) {
        const std::filesystem::path exe_dir = exe.parent_path();
        roots.push_back(exe_dir.parent_path() / "share" / "orange");
        roots.push_back(exe_dir.parent_path().parent_path());
    }
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        roots.push_back(cwd);
    }
    return roots;
}

// Absolute path of `relative` under the first support root that has it, or an
// empty path when none does.
inline std::filesystem::path find_support_file(const std::string& relative)
{
    for (const std::filesystem::path& root : support_roots()) {
        std::error_code ec;
        const std::filesystem::path candidate = root / relative;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return std::filesystem::absolute(candidate, ec).lexically_normal();
        }
    }
    return {};
}

// find_support_file(), falling back to `relative` itself (resolved against the
// current folder by whoever opens it) when no root has the file.
inline std::string support_file_or_relative(const std::string& relative)
{
    const std::filesystem::path found = find_support_file(relative);
    return found.empty() ? relative : found.string();
}

}  // namespace orange::runtime_paths
