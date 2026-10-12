// Tests for src/outputs/common.
#include "outputs/common/atomic_file.h"
#include "outputs/common/digest.h"
#include "outputs/common/safe_path.h"
#include "outputs/common/csv.h"

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

    // Digests: hex is bare, token is prefixed, for bytes and files alike.
    const std::string abc_hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    check(sha256_hex("abc") == abc_hex, "sha256_hex of abc");
    check(sha256_token("abc") == "sha256:" + abc_hex, "sha256_token of abc");
    write_file_atomic(root / "abc.txt", "abc");
    std::string file_hex, file_token;
    check(file_sha256_hex(root / "abc.txt", &file_hex) && file_hex == abc_hex, "file_sha256_hex is bare hex");
    check(file_sha256_token(root / "abc.txt", &file_token) && file_token == "sha256:" + abc_hex,
          "file_sha256_token is prefixed");
    check(is_sha256_hex(abc_hex) && !is_sha256_hex("sha256:" + abc_hex) && !is_sha256_hex("ABC"),
          "is_sha256_hex");
    check(is_sha256_token("sha256:" + abc_hex) && !is_sha256_token(abc_hex), "is_sha256_token");
    check(canonical_json_text({{"b", 1}, {"a", "x"}}) == "{\"a\":\"x\",\"b\":1}", "canonical JSON is compact, sorted");

    // Safe relative paths: exactly one spelling, never escaping.
    for (const char* good : {"a", "a/b.json", "external_recorder/clips/clip_000001/Cam1_meta.csv"}) {
        check(is_safe_relative_path(good), std::string("safe: ") + good);
    }
    for (const char* bad : {"", ".", "..", "./a", "a/./b", "a//b", "a/", "../a", "a/../b", "/abs", "a/.."}) {
        check(!is_safe_relative_path(bad), std::string("refused: '") + bad + "'");
    }

    // Inside a root: component-wise, symlinks resolved.
    const fs::path rec = root / "rec1";
    fs::create_directories(rec / "sub");
    fs::create_directories(root / "rec10");
    fs::create_directories(root / "outside");
    check(is_inside(rec / "sub" / "x.json", rec), "child file is inside");
    check(is_inside(rec, rec), "root is inside itself");
    check(!is_inside(root / "rec10" / "x", rec), "sibling with a shared prefix is not inside");
    check(!is_inside(rec / ".." / "outside", rec), "dot-dot escape is not inside");
    fs::create_symlink(root / "outside", rec / "link");
    check(!is_inside(rec / "link" / "x.json", rec), "symlink escape is not inside");

    // CSV (RFC 4180).
    using V = std::vector<std::string>;
    check(split_csv_row("1,2,3") == V({"1", "2", "3"}), "plain row");
    check(split_csv_row("a,b,") == V({"a", "b", ""}), "trailing empty field kept");
    check(split_csv_row("a,b\r") == V({"a", "b"}), "CRLF line ending dropped");
    check(split_csv_row("") == V({""}), "empty line is one empty field");
    check(split_csv_row("\"x,y\",\"say \"\"hi\"\"\",z") == V({"x,y", "say \"hi\"", "z"}), "quoted fields");
    V parsed;
    check(!parse_csv_row("\"open,1", &parsed), "unterminated quote refused");
    check(!parse_csv_row("a\"b,1", &parsed), "quote inside unquoted field refused");
    check(split_csv_row(csv_row({"plain", "comma,inside", "quote\"inside"})) ==
              V({"plain", "comma,inside", "quote\"inside"}), "csv_row round-trips");
    check(csv_row({"1", "2"}) == "1,2", "numeric rows are unchanged by escaping");
    check(csv_column({"a", "b"}, "b") == 1 && csv_column({"a"}, "z") == static_cast<std::size_t>(-1), "csv_column");

    fs::remove_all(root);
    std::cout << (failures ? "outputs_common_tests FAILED\n" : "outputs_common_tests passed\n");
    return failures ? 1 : 0;
}
