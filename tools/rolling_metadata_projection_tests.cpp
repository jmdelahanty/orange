#include "session/rolling_metadata_projection.h"
#include "recording_metadata_csv.h"
#include <iostream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    fs::path root;
    json manifest;
    Fixture() {
        std::string path = (fs::temp_directory_path() / "orange_rolling_projection_test_XXXXXX").string();
        if (!::mkdtemp(path.data())) throw std::runtime_error("mkdtemp failed");
        root = path;
        manifest = {{"session_id", "recording"}, {"mode", "rolling_clips"},
            {"status", "completed"}, {"recording", {{"drain_completed", true}}},
            {"cameras", json::array({"2010096"})}, {"clips", json::array()}};
        for (int i = 0; i < 2; ++i) {
            const std::string dir = "clips/clip_" + std::to_string(i);
            fs::create_directories(root / dir);
            // Synthetic metadata fixture: video presence/declared parity only,
            // deliberately not a decoder or real encoder identity test.
            std::ofstream(root / dir / "Cam2010096.mp4") << "fixture";
            std::ofstream csv(root / dir / "Cam2010096_meta.csv");
            orange::recording_metadata::write_header(csv);
            const int first = i == 0 ? 1 : 3, last = i == 0 ? 2 : 3;
            for (int frame = first; frame <= last; ++frame)
                orange::recording_metadata::write_row(csv, frame, 1787695084226944090ULL + frame,
                                                       1787695047236290646ULL + frame);
            manifest["clips"].push_back({{"session_id", "recording"},
                {"clip_id", "clip_" + std::to_string(i)}, {"clip_index", i},
                {"directory", dir}, {"status", "completed"}, {"drain_completed", true},
                {"final_clip", i == 1}, {"rollover", {{"pending_next_clip", false}}},
                {"camera_artifacts", {{"2010096", {
                    {"metadata", dir + "/Cam2010096_meta.csv"}, {"video", dir + "/Cam2010096.mp4"},
                    {"frame_count", last - first + 1}, {"first_recording_frame_id", first},
                    {"last_recording_frame_id", last}, {"packet_count", last - first + 1},
                    {"packet_count_source", "fixture_declared"}}}}}});
        }
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path metadata(int clip) { return root / "clips" / ("clip_" + std::to_string(clip)) / "Cam2010096_meta.csv"; }
    json project() { return orange::session::project_dense_rolling_metadata(manifest, root / "recording_session.json"); }
};
void reject(Fixture& f) {
    bool failed = false;
    try { (void)f.project(); } catch (const std::exception&) { failed = true; }
    require(failed, "invalid fixture unexpectedly admitted");
    for (const auto& p : fs::directory_iterator(f.root))
        require(p.path().extension() != ".csv" &&
                p.path().filename().string().find(".rolling_projection_") != 0,
                "invalid single-camera fixture published a projection or leaked staging");
}
}  // namespace

int main(int argc, char** argv) {
    const bool emit_fixture = argc == 2 && std::string(argv[1]) == "--emit-fixture";
    if (argc != 1 && !emit_fixture) return 2;
    try {
        {
            Fixture f;
            auto p = f.project();
            if (emit_fixture) std::cout << p.at("rolling_metadata_projection").dump() << '\n';
            const auto& stream = p["rolling_metadata_projection"]["camera_streams"]["2010096"];
            require(stream["sources"].size() == 2 && stream["sources"][1]["frame_count"] == 1 &&
                    stream["sources"][1]["first_parent_output_index"] == 2 &&
                    stream["sources"][1]["first_clip_output_index"] == 0,
                    "partial final clip must keep parent correspondence and clip-local origin");
            std::ostringstream whole;
            orange::recording_metadata::write_header(whole);
            for (int frame = 1; frame <= 3; ++frame)
                orange::recording_metadata::write_row(whole, frame, 1787695084226944090ULL + frame,
                                                       1787695047236290646ULL + frame);
            std::string actual, error;
            require(orange::gui::spatial_layout::checksum::read_file(f.root /
                stream["projection"]["relative_path"].get<std::string>(), &actual, &error), "projection missing");
            require(actual == whole.str(), "whole/rolling must preserve exact rows and timestamps");
            require(f.project()["rolling_metadata_projection_sha256"] == p["rolling_metadata_projection_sha256"],
                    "retry must reuse identical content-addressed projection");
            std::string digest;
            require(orange::gui::spatial_layout::checksum::file_sha256(f.metadata(1), &digest, &error) &&
                    stream["sources"][1]["metadata_sha256"] == digest,
                    "clip reference must bind exact input bytes");
            require(!p["camera_artifacts"]["2010096"].contains("video"),
                    "projection must not invent a parent video");
        }
        for (const auto& row : {"2,11,22,2\n", "4,11,22,4\n", "1,11,22,1\n",
                               "4,11,22,3\n", "3,1.5,22,3\n", "3,11,22,3", ""}) {
            Fixture f;
            { std::ofstream csv(f.metadata(1)); orange::recording_metadata::write_header(csv); csv << row; }
            reject(f);
        }
        for (int scenario = 0; scenario < 8; ++scenario) {
            Fixture f;
            auto& clip = f.manifest["clips"][1];
            if (scenario == 0) clip["clip_index"] = 0;
            if (scenario == 1) clip["clip_id"] = "clip_0";
            if (scenario == 2) clip["session_id"] = "other";
            if (scenario == 3) clip["drain_completed"] = false;
            if (scenario == 4) clip["final_clip"] = false;
            if (scenario == 5) clip["camera_artifacts"]["2010096"]["packet_count"] = 2;
            if (scenario == 6) clip["camera_artifacts"]["2010096"]["metadata"] = "clips/clip_0/Cam2010096_meta.csv";
            if (scenario == 7) f.manifest["status"] = "interrupted";
            reject(f);
        }
        (emit_fixture ? std::cerr : std::cout) << "rolling_metadata_projection_tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
