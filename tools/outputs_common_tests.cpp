// Tests for src/outputs/common.
#include "outputs/common/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    failures += ok ? 0 : 1;
}
std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
std::size_t temporaries(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        n += e.path().extension() == ".tmp" ? 1 : 0;
    }
    return n;
}
}  // namespace

int main() {
    using namespace orange::outputs;
    const fs::path root = fs::temp_directory_path() / ("orange_outputs_common_" + std::to_string(::getpid()));
    fs::remove_all(root);

    const fs::path doc = root / "nested" / "doc.json";
    std::string error;
    check(write_json_atomic(doc, {{"b", 2}, {"a", 1}}, {}, &error) && error.empty(),
          "write_json_atomic creates parents and writes: " + error);
    check(read(doc) == "{\n  \"a\": 1,\n  \"b\": 2\n}\n", "JSON text is dump(2) plus newline");

    check(write_file_atomic(doc, "replaced\n", AtomicWriteOptions::durable(), &error), "fsync replace succeeds");
    check(read(doc) == "replaced\n", "replace is complete");
    check(temporaries(doc.parent_path()) == 0, "no temporary left after success");

    AtomicWriteOptions mode;
    mode.perms = fs::perms::owner_read | fs::perms::group_read;
    check(write_file_atomic(root / "ro.txt", "x", mode, &error), "write with explicit permissions");
    struct stat st{};
    ::stat((root / "ro.txt").c_str(), &st);
    check((st.st_mode & 0777) == 0440, "permissions applied before the rename");

    // Failure leaves the target untouched and no temporary.
    fs::create_directories(root / "blocked");
    fs::permissions(root / "blocked", fs::perms::owner_read | fs::perms::owner_exec);
    const bool refused = !write_file_atomic(root / "blocked" / "x.json", "y", {}, &error);
    fs::permissions(root / "blocked", fs::perms::owner_all);
    check(refused || ::geteuid() == 0, "unwritable directory is refused (skipped as root)");
    check(temporaries(root / "blocked") == 0, "refused write leaves no temporary");

    // Concurrent writers to one target: every result is one complete version.
    const fs::path shared = root / "shared.json";
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                write_json_atomic(shared, {{"writer", t}, {"payload", std::string(4096, 'a' + t)}});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    const auto final_doc = nlohmann::json::parse(read(shared), nullptr, false);
    check(!final_doc.is_discarded() && final_doc.at("payload").get<std::string>().size() == 4096,
          "concurrent writers never leave a torn file");
    check(temporaries(root) == 0, "concurrent writers leave no temporaries");

    fs::remove_all(root);
    std::cout << (failures ? "outputs_common_tests FAILED\n" : "outputs_common_tests passed\n");
    return failures ? 1 : 0;
}
