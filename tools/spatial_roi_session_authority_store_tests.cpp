#include "spatial_roi_session_authority_store.h"

#include "gui/spatial_layout/sha256.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using orange::session::spatial_roi::SpatialRoiSessionAuthorityReceipt;
using orange::session::spatial_roi::SpatialRoiSessionAuthorityStore;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempTree final {
public:
    TempTree()
    {
        std::string pattern = "/tmp/orange_spatial_roi_authority_XXXXXX";
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        const char* created = ::mkdtemp(mutable_pattern.data());
        require(created != nullptr,
                std::string("mkdtemp failed: ") + std::strerror(errno));
        path_ = created;
    }

    ~TempTree()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

fs::path make_root(const TempTree& tree, const std::string& leaf = "recording")
{
    const fs::path root = tree.path() / leaf;
    require(fs::create_directory(root), "failed to create recording root");
    return root;
}

std::unique_ptr<SpatialRoiSessionAuthorityStore> open_existing(
    const fs::path& root,
    const std::vector<std::string>& allowed)
{
    std::unique_ptr<SpatialRoiSessionAuthorityStore> store;
    std::string error;
    require(SpatialRoiSessionAuthorityStore::OpenExisting(
                root, allowed, &store, &error),
            "open existing failed: " + error);
    require(store != nullptr && store->valid(),
            "open existing returned no valid store");
    return store;
}

void require_open_rejected(
    const fs::path& root,
    const std::vector<std::string>& allowed,
    const std::string& context)
{
    std::unique_ptr<SpatialRoiSessionAuthorityStore> store;
    std::string error;
    require(!SpatialRoiSessionAuthorityStore::OpenExisting(
                root, allowed, &store, &error),
            context + " was unexpectedly accepted");
    require(store == nullptr && !error.empty(),
            context + " did not fail closed");
}

void test_publish_receipt_retry_and_no_overwrite()
{
    TempTree tree;
    const fs::path root = make_root(tree);
    auto store = open_existing(root, {"snapshot.json", "plan.json"});
    const std::string bytes = "{\"recording_id\":\"r1\"}\n";
    SpatialRoiSessionAuthorityReceipt receipt;
    std::string error;
    require(store->PublishBytes("snapshot.json", bytes, &receipt, &error),
            "publish failed: " + error);
    require(receipt.relative_path == "snapshot.json" &&
                receipt.size_bytes == bytes.size() &&
                receipt.sha256 ==
                    "sha256:" +
                        orange::gui::spatial_layout::checksum::sha256_hex(bytes),
            "published receipt does not describe exact bytes");
    require(receipt.ToJson() ==
                nlohmann::json{{"relative_path", "snapshot.json"},
                               {"size_bytes", bytes.size()},
                               {"sha256", receipt.sha256}},
            "receipt JSON is not closed as expected");

    std::string read_back;
    SpatialRoiSessionAuthorityReceipt verified;
    require(store->ReadAndVerify(receipt, &read_back, &verified, &error),
            "read/verify failed: " + error);
    require(read_back == bytes && verified == receipt,
            "read/verify did not return the exact receipt");

    SpatialRoiSessionAuthorityReceipt retry;
    require(store->PublishBytes("snapshot.json", bytes, &retry, &error),
            "identical retry failed: " + error);
    require(retry == receipt, "identical retry changed the receipt");
    require(!store->PublishBytes("snapshot.json", "changed", &retry, &error) &&
                !error.empty(),
            "changed existing authority was unexpectedly accepted");
    std::ifstream input(root / "snapshot.json", std::ios::binary);
    const std::string persisted((std::istreambuf_iterator<char>(input)), {});
    require(persisted == bytes, "changed retry overwrote the authority file");
}

void test_publish_json_and_path_allow_list()
{
    TempTree tree;
    auto store = open_existing(make_root(tree), {"authority.json"});
    SpatialRoiSessionAuthorityReceipt receipt;
    std::string error;
    require(store->PublishJson(
                "authority.json", nlohmann::json{{"b", 2}, {"a", 1}},
                &receipt, &error),
            "JSON publish failed: " + error);
    require(receipt.size_bytes == 13,
            "JSON receipt size does not use compact canonical bytes");
    require(store->IsAllowed("authority.json") &&
                !store->IsAllowed("other.json"),
            "allow-list lookup is not exact");

    for (const char* unsafe : {"../authority.json", "sub/authority.json",
                               "/authority.json", "", ".", ".."}) {
        require(!store->PublishBytes(unsafe, "x", &receipt, &error),
                std::string("unsafe authority leaf was accepted: ") + unsafe);
    }
    require_open_rejected(tree.path() / "missing",
                          {"../authority.json"},
                          "unsafe allow-list path");
    require_open_rejected(make_root(tree, "other"),
                          {"authority.json", "authority.json"},
                          "duplicate allow-list path");
}

void test_root_and_leaf_symlink_refusal()
{
    TempTree tree;
    const fs::path real_root = make_root(tree, "real");
    const fs::path root_link = tree.path() / "root_link";
    require(::symlink(real_root.c_str(), root_link.c_str()) == 0,
            "failed to create root symlink");
    require_open_rejected(root_link, {"snapshot.json"}, "root symlink");
    require_open_rejected(fs::path("/"), {"snapshot.json"}, "root '/' ");

    const fs::path non_directory = tree.path() / "not_a_directory";
    {
        std::ofstream output(non_directory);
        output << "not a directory";
    }
    require_open_rejected(non_directory, {"snapshot.json"}, "non-directory root");

    require(::symlink("target", (real_root / "snapshot.json").c_str()) == 0,
            "failed to create leaf symlink");
    require_open_rejected(real_root, {"snapshot.json"}, "leaf symlink");
}

void test_preexisting_changed_file_is_refused()
{
    TempTree tree;
    const fs::path root = make_root(tree);
    {
        std::ofstream output(root / "snapshot.json", std::ios::binary);
        output << "original";
    }
    auto store = open_existing(root, {"snapshot.json"});
    SpatialRoiSessionAuthorityReceipt receipt;
    std::string error;
    require(!store->PublishBytes("snapshot.json", "replacement", &receipt, &error) &&
                error.find("different bytes") != std::string::npos,
            "preexisting changed authority was unexpectedly accepted");
    std::ifstream input(root / "snapshot.json", std::ios::binary);
    const std::string persisted((std::istreambuf_iterator<char>(input)), {});
    require(persisted == "original",
            "preexisting changed authority was overwritten");
}

void test_open_or_create_no_follow_and_root_swap()
{
    TempTree tree;
    const fs::path nested = tree.path() / "new" / "recording";
    std::unique_ptr<SpatialRoiSessionAuthorityStore> store;
    std::string error;
    require(SpatialRoiSessionAuthorityStore::OpenOrCreate(
                nested, {"snapshot.json"}, &store, &error),
            "OpenOrCreate failed: " + error);
    require(fs::is_directory(nested.parent_path()) && fs::is_directory(nested),
            "OpenOrCreate did not create missing components");

    const fs::path swap_parent = tree.path() / "swap_parent";
    require(fs::create_directory(swap_parent), "failed to create swap parent");
    const fs::path real_parent = swap_parent / "real_parent";
    require(fs::create_directory(real_parent), "failed to create real parent");
    const fs::path parent_link = swap_parent / "parent_link";
    require(::symlink(real_parent.c_str(), parent_link.c_str()) == 0,
            "failed to create parent symlink");
    const fs::path symlinked = parent_link / "recording";
    std::unique_ptr<SpatialRoiSessionAuthorityStore> rejected;
    require(!SpatialRoiSessionAuthorityStore::OpenOrCreate(
                symlinked, {"snapshot.json"}, &rejected, &error),
            "OpenOrCreate followed a symlink component");

    const fs::path root = make_root(tree, "swap");
    store = open_existing(root, {"snapshot.json"});
    const fs::path moved = tree.path() / "swap_old";
    std::error_code rename_error;
    fs::rename(root, moved, rename_error);
    require(!rename_error, "failed to move root for swap test");
    require(fs::create_directory(root), "failed to replace root for swap test");
    SpatialRoiSessionAuthorityReceipt receipt;
    require(!store->PublishBytes("snapshot.json", "bytes", &receipt, &error) &&
                error.find("binding") != std::string::npos,
            "root path swap was not detected");
}

void test_hard_link_alias_is_not_authoritative()
{
    TempTree tree;
    const fs::path root = make_root(tree);
    auto store = open_existing(root, {"snapshot.json"});
    SpatialRoiSessionAuthorityReceipt receipt;
    std::string error;
    require(store->PublishBytes("snapshot.json", "bytes", &receipt, &error),
            "initial publish failed: " + error);
    require(::link((root / "snapshot.json").c_str(),
                   (tree.path() / "alias").c_str()) == 0,
            "failed to create authority hard-link alias");
    require(!store->PublishBytes("snapshot.json", "bytes", &receipt, &error),
            "hard-linked authority was accepted on retry");
    require(error.find("hard-link") != std::string::npos,
            "hard-link rejection did not report the alias");
}

}  // namespace

int main()
{
    const struct TestCase {
        const char* name;
        void (*function)();
    } tests[] = {
        {"publish_receipt_retry_and_no_overwrite",
         test_publish_receipt_retry_and_no_overwrite},
        {"publish_json_and_path_allow_list",
         test_publish_json_and_path_allow_list},
        {"root_and_leaf_symlink_refusal", test_root_and_leaf_symlink_refusal},
        {"preexisting_changed_file_is_refused",
         test_preexisting_changed_file_is_refused},
        {"open_or_create_no_follow_and_root_swap",
         test_open_or_create_no_follow_and_root_swap},
        {"hard_link_alias_is_not_authoritative",
         test_hard_link_alias_is_not_authoritative},
    };
    for (const TestCase& test : tests) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": "
                      << exception.what() << '\n';
            return 1;
        }
    }
    return 0;
}
