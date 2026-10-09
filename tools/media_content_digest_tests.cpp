// Unit tests for src/media_content_digest: receipt format, bounded queue,
// bounded finish, no partial files.
#include "media_content_digest.h"
#include "gui/spatial_layout/sha256.h"
#include "json.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace orange::media_digest;

namespace {
int failures = 0;
void require(bool ok, const std::string& what)
{
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << std::endl; }
}
void write_bytes(const fs::path& p, size_t n, char fill)
{
    std::ofstream out(p, std::ios::binary);
    std::string chunk(1 << 16, fill);
    while (n > 0) { const size_t k = std::min(n, chunk.size()); out.write(chunk.data(), k); n -= k; }
}
}  // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "orange_media_content_digest_tests";
    fs::remove_all(root);
    fs::create_directories(root / "external_recorder" / "clips" / "clip_000000");
    const fs::path video = root / "external_recorder" / "clips" / "clip_000000" / "Cam2010093_external.mp4";
    write_bytes(video, 5u * 1024u * 1024u + 123u, 'x');

    // Synchronous receipt: format and digest.
    {
        std::string sha, error;
        require(write_content_digest_receipt({video.string(), root.string(), "sess-1"}, &sha, &error), "receipt written: " + error);
        std::string expected_checksum;
        require(orange::gui::spatial_layout::checksum::file_sha256(video, &expected_checksum, &error), "reference hash");
        require("sha256:" + sha == expected_checksum, "digest equals the streaming file hash");
        const fs::path receipt = receipt_path_for(video.string());
        require(fs::exists(receipt), "receipt beside the video");
        const nlohmann::json j = nlohmann::json::parse(std::ifstream(receipt));
        require(j.size() == 10, "closed schema: exactly ten fields");
        require(j.at("schema_id") == "orange.media_content_digest" && j.at("schema_version") == 1 &&
                j.at("algorithm") == "sha256" && j.at("sha256") == sha &&
                j.at("size_bytes") == 5u * 1024u * 1024u + 123u &&
                j.at("computed") == "after_finalization_readback" &&
                j.at("video_path") == "external_recorder/clips/clip_000000/Cam2010093_external.mp4" &&
                j.at("session_id") == "sess-1" && j.at("producer_pid").get<int>() > 0 &&
                j.at("computed_at_utc").get<std::string>().size() > 20, "receipt fields");
        require(!fs::exists(receipt.string() + ".tmp." + std::to_string(getpid())), "no temp file left");
        fs::remove(receipt);
    }
    // Missing file: failure, nothing left behind.
    {
        std::string sha, error;
        require(!write_content_digest_receipt({(root / "missing.mp4").string(), root.string(), "s"}, &sha, &error), "missing video fails");
        require(!fs::exists(receipt_path_for((root / "missing.mp4").string())), "no receipt for a missing video");
    }
    // Hasher: bounded queue, counters, bounded finish.
    {
        ContentDigestHasher hasher(2);
        require(!hasher.try_enqueue({video.string(), root.string(), "s"}), "enqueue before start is abandoned");
        hasher.start();
        require(hasher.try_enqueue({video.string(), root.string(), "s"}), "first enqueue accepted");
        hasher.finish(std::chrono::seconds(10));
        const ReceiptCounters c = hasher.counters();
        require(c.requested == 2 && c.written == 1 && c.abandoned == 1 && c.failed == 0,
                "counters: requested 2 written 1 abandoned 1 failed 0");
        require(fs::exists(receipt_path_for(video.string())), "receipt written by the hasher thread");
        fs::remove(receipt_path_for(video.string()));
        require(!hasher.try_enqueue({video.string(), root.string(), "s"}), "enqueue after finish is abandoned");
    }
    // Queue capacity respected; finish with zero wait abandons the rest.
    {
        ContentDigestHasher hasher(1);
        hasher.start();
        // Fill with a big file so the thread is busy, then overflow.
        const fs::path big = root / "big.mp4";
        write_bytes(big, 64u * 1024u * 1024u, 'b');
        int accepted = 0;
        for (int i = 0; i < 6; ++i) accepted += hasher.try_enqueue({big.string(), root.string(), "s"}) ? 1 : 0;
        require(accepted >= 1 && accepted <= 2, "at most one running plus one queued");
        hasher.finish(std::chrono::milliseconds(0));
        const ReceiptCounters c = hasher.counters();
        require(c.requested == 6 && c.written + c.abandoned + c.failed == 6, "every request accounted for");
        require(!fs::exists(receipt_path_for(big.string()) + ".tmp." + std::to_string(getpid())), "no partial receipt");
    }
    fs::remove_all(root);
    if (failures) { std::cerr << failures << " media content digest test(s) failed" << std::endl; return 1; }
    std::cout << "All media content digest tests passed." << std::endl;
    return 0;
}
