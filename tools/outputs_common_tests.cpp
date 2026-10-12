// Tests for src/outputs/common.
#include "outputs/common/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <atomic>
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

    // create_file_once: publish once, identical retry succeeds, different bytes refused.
    const fs::path once = root / "once" / "evidence.json";
    bool existed = true;
    check(create_file_once(once, "v1\n", {}, &error, &existed) && !existed, "create-once publishes: " + error);
    struct stat once_stat{};
    ::stat(once.c_str(), &once_stat);
    check((once_stat.st_mode & 0777) == 0444, "create-once file is read-only");
    check(create_file_once(once, "v1\n", {}, &error, &existed) && existed, "identical retry succeeds");
    check(!create_file_once(once, "v2\n", {}, &error) && error.find("different bytes") != std::string::npos,
          "different bytes are refused");
    check(read(once) == "v1\n", "refused create leaves the original");
    check(temporaries(once.parent_path()) == 0, "create-once leaves no temporaries");
    CreateOnceOptions writable;
    writable.read_only = false;
    writable.durability = Durability::kRenameOnly;
    const fs::path loose = root / "once" / "loose.bin";
    check(create_file_once(loose, "x", writable, &error), "create-once without read-only");
    ::chmod(once.c_str(), 0644);
    check(!create_file_once(once, "v1\n", {}, &error) && error.find("writable") != std::string::npos,
          "existing writable file fails a read-only create-once");

    // Racing creators: exactly one publishes, the rest see identical bytes.
    const fs::path raced = root / "once" / "raced.json";
    std::atomic<int> created{0}, matched{0};
    std::vector<std::thread> racers;
    for (int t = 0; t < 8; ++t) {
        racers.emplace_back([&] {
            bool was = false;
            if (create_file_once(raced, "same\n", {}, nullptr, &was)) {
                (was ? matched : created)++;
            }
        });
    }
    for (auto& t : racers) t.join();
    check(created == 1 && matched == 7, "exactly one racer creates, seven see it identical");

    fs::remove_all(root);
    std::cout << (failures ? "outputs_common_tests FAILED\n" : "outputs_common_tests passed\n");
    return failures ? 1 : 0;
}
