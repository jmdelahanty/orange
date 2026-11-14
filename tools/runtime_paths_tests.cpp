// src/runtime_paths.h: a binary finds its support files in the /opt release or
// build tree it belongs to, before the folder it was started in.
//
//   runtime_paths_tests                     run the tests
//   runtime_paths_tests --resolve <rel>     print find_support_file(rel) (used by the tests)

#include "runtime_paths.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

namespace fs = std::filesystem;
using orange::runtime_paths::find_support_file;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x\n";
}

std::string run_resolve(const fs::path& binary, const fs::path& cwd, const std::string& relative)
{
    const std::string command = "cd '" + cwd.string() + "' && '" + binary.string() +
                                "' --resolve '" + relative + "'";
    FILE* pipe = ::popen(command.c_str(), "r");
    require(pipe != nullptr, "popen failed");
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    const int status = ::pclose(pipe);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "resolve child failed: " + command);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

void test_tree_build_finds_its_tree()
{
    // This test binary is <tree>/targets/release/runtime_paths_tests.
    const fs::path found = find_support_file("scripts/validate_gui_ptp_recording.py");
    require(!found.empty(), "a tree build finds scripts/ in its tree");
    require(fs::exists(found.parent_path().parent_path() / "fonts" / "Roboto-Regular.ttf"),
            "the same tree has the fonts");
    require(find_support_file("fonts/does-not-exist.ttf").empty(), "a missing file resolves empty");
}

void test_installed_release_wins_over_cwd(const fs::path& self)
{
    const fs::path root = fs::temp_directory_path() /
                          ("orange_runtime_paths_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path release = root / "releases" / "orange-vtest";
    const fs::path other = root / "elsewhere";
    touch(release / "share" / "orange" / "scripts" / "probe.py");
    touch(other / "scripts" / "probe.py");
    touch(other / "scripts" / "only_in_cwd.py");
    fs::create_directories(release / "bin");
    const fs::path installed = release / "bin" / "runtime_paths_tests";
    fs::copy_file(self, installed, fs::copy_options::overwrite_existing);
    fs::permissions(installed, fs::perms::owner_all, fs::perm_options::add);

    const std::string from_release = run_resolve(installed, other, "scripts/probe.py");
    require(from_release == (release / "share" / "orange" / "scripts" / "probe.py").string(),
            "an installed binary uses its release's share/orange, not the cwd: got " + from_release);
    const std::string fallback = run_resolve(installed, other, "scripts/only_in_cwd.py");
    require(fallback == (other / "scripts" / "only_in_cwd.py").string(),
            "the cwd is still the last resort: got " + fallback);
    const std::string missing = run_resolve(installed, other, "scripts/nowhere.py");
    require(missing.empty(), "a file in no root resolves empty: got " + missing);
    fs::remove_all(root);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--resolve") {
        std::cout << find_support_file(argv[2]).string() << "\n";
        return 0;
    }
    try {
        test_tree_build_finds_its_tree();
        test_installed_release_wins_over_cwd(orange::runtime_paths::executable_path());
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
    std::cout << "runtime_paths_tests passed\n";
    return 0;
}
