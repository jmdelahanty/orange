#pragma once

// Source stamp of the tree this binary was built from, written at build time by
// cmake/write_build_info.cmake (git commit, branch, describe and whether tracked
// files differed from the commit). Unlike the run-time worktree lookup, it stays
// true when the binary is copied elsewhere (an /opt release) or when the source
// tree has moved on since the build.

#include <string_view>

namespace orange::build_info {

struct BuildStamp {
    bool available;          // false when the build tree was not a git checkout
    std::string_view commit;
    std::string_view commit_short;  // `git rev-parse --short HEAD` at build time
    std::string_view branch;
    std::string_view describe;
    bool dirty_known;
    bool dirty_tracked;      // tracked files differed from `commit` when built
};

const BuildStamp& stamp();

}  // namespace orange::build_info
