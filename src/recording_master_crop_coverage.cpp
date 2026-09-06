#include "recording_master_crop_coverage.h"
#include "recording_master_journal.h"
#include "gui/spatial_layout/sha256.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>

namespace orange::recording {
namespace fs = std::filesystem;
namespace checksum = orange::gui::spatial_layout::checksum;
using json = nlohmann::json;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
std::vector<std::string> fields(std::string row) {
    if (!row.empty() && row.back() == '\r') row.pop_back();
    std::vector<std::string> result;
    std::size_t begin = 0;
    for (;;) {
        const auto end = row.find(',', begin);
        result.push_back(row.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) return result;
        begin = end + 1;
    }
}
uint64_t integer(const std::string& field) {
    uint64_t value = 0;
    const auto read = std::from_chars(field.data(), field.data() + field.size(), value);
    require(!field.empty() && read.ec == std::errc{} && read.ptr == field.data() + field.size(),
            "crop/master metadata integer invalid");
    return value;
}
bool line(std::istream& in, std::string* text) {
    char buffer[65538];
    in.getline(buffer, sizeof(buffer));
    require(!in.bad(), "metadata read failed");
    if (in.eof()) { require(in.gcount() == 0, "partial metadata row"); return false; }
    require(!in.fail() && in.gcount() > 1, "oversized/empty metadata row");
    text->assign(buffer, static_cast<std::size_t>(in.gcount() - 1));
    return true;
}
fs::path resolve(const fs::path& root, const fs::path& relative) {
    require(!relative.empty() && !relative.is_absolute() && relative.lexically_normal() == relative,
            "metadata path is not normalized/relative");
    const auto spelling = relative.generic_string();
    require(spelling.find('\\') == std::string::npos &&
            std::all_of(spelling.begin(), spelling.end(), [](unsigned char c) { return c >= 32; }),
            "metadata path contains controls/backslash");
    fs::path current = root;
    for (const auto& part : relative) {
        require(part != "." && part != ".." && !part.empty(), "metadata path escapes root");
        current /= part;
        require(!fs::is_symlink(current), "metadata symlink refused");
    }
    require(fs::is_regular_file(current), "metadata file missing");
    return current;
}
json artifact(const fs::path& root, const fs::path& relative) {
    const auto path = resolve(root, relative);
    std::string digest, error;
    require(checksum::file_sha256(path, &digest, &error), "metadata hash failed");
    return {{"relative_path", relative.generic_string()}, {"size_bytes", fs::file_size(path)}, {"sha256", digest}};
}
} // namespace

json CheckMovingCropMetadataCoverage(const fs::path& recording_root, const json& master,
                                     const std::vector<MovingCropClipMetadata>& clips) {
    const auto root = fs::canonical(recording_root);
    require(master.at("schema_id") == "orange.recording.master_frame_journal" &&
            master.at("schema_version") == 1 && master.at("status") == "complete" &&
            master.at("terminal") == true && master.at("producer_finished_normally") == true &&
            master.at("observation_boundary") == "caller_submitted_acquisition_facts_v1",
            "master journal is not a complete supported recording");
    require(master.at("source").at("assigned_ids_dense_from_one") == true,
            "full-rate first profile requires dense assigned master IDs");
    const fs::path master_path = master.at("csv").at("relative_path").get<std::string>();
    require(artifact(root, master_path) == master.at("csv"), "master CSV binding mismatch");
    require(!clips.empty(), "moving crop collection is empty");
    std::ifstream source(resolve(root, master_path));
    std::string row;
    require(line(source, &row) && row + "\n" == MasterFrameJournal::CsvHeader(), "unsupported master CSV header");
    uint64_t observations = 0, assigned = 0, outputs = 0;
    std::vector<std::string> source_fields;
    auto next_assigned = [&]() {
        while (line(source, &row)) {
            source_fields = fields(row);
            require(source_fields.size() == 14, "master row shape invalid");
            require(integer(source_fields[0]) == observations++, "master observation sequence gap");
            const auto kind = integer(source_fields[1]);
            require(kind >= 1 && kind <= 6, "master fact kind invalid");
            if (kind != 1) {
                require(integer(source_fields[3]) == 0, "unassigned observation has a recording ID");
                continue;
            }
            require(integer(source_fields[3]) == ++assigned, "master assigned sequence invalid");
            require(integer(source_fields[8]) == 1 && integer(source_fields[12]) == 1,
                    "master timestamp absent; correspondence cannot be checked");
            return true;
        }
        return false;
    };
    std::set<std::string> ids, paths;
    checksum::StreamingSha256 correspondence;
    json clip_results = json::array();
    for (std::size_t index = 0; index < clips.size(); ++index) {
        const auto& clip = clips[index];
        require(clip.recording_id == master.at("recording_id") && clip.camera_serial == master.at("camera_serial"),
                "crop parent/camera binding mismatch");
        require(clip.clip_index == index && !clip.clip_id.empty() && ids.insert(clip.clip_id).second &&
                paths.insert(clip.metadata_relative_path.generic_string()).second,
                "crop clip membership/order invalid");
        const auto ref = artifact(root, clip.metadata_relative_path);
        std::ifstream input(resolve(root, clip.metadata_relative_path));
        require(line(input, &row), "crop CSV is empty");
        const auto header = fields(row);
        std::set<std::string> column_names(header.begin(), header.end());
        require(column_names.size() == header.size(), "duplicate crop CSV column");
        auto column = [&](const std::string& name) {
            const auto it = std::find(header.begin(), header.end(), name);
            require(it != header.end(), "required crop CSV column missing");
            return static_cast<std::size_t>(it - header.begin());
        };
        const auto rid = column("recording_frame_id"), local = column("crop_video_frame_index");
        const auto session = column("session_crop_video_frame_index"), camera_time = column("timestamp");
        const auto real_time = column("timestamp_sys");
        const uint64_t first = outputs;
        uint64_t local_count = 0;
        while (line(input, &row)) {
            const auto crop = fields(row);
            require(crop.size() == header.size() && next_assigned(), "extra/malformed crop output");
            require(integer(crop[rid]) == integer(source_fields[3]) &&
                    integer(crop[local]) == local_count && integer(crop[session]) == outputs,
                    "crop/master source or local/session index mismatch");
            require(integer(crop[camera_time]) == integer(source_fields[9]) &&
                    integer(crop[real_time]) == integer(source_fields[13]), "crop/master timestamp mismatch");
            correspondence.update(std::to_string(index) + "," + std::to_string(local_count) + "," +
                std::to_string(outputs) + "," + std::to_string(assigned) + "," +
                std::to_string(observations - 1) + "\n");
            ++outputs; ++local_count;
        }
        require(local_count > 0, "empty moving crop clip");
        require(ref == artifact(root, clip.metadata_relative_path), "crop metadata mutated during validation");
        clip_results.push_back({{"clip_id", clip.clip_id}, {"clip_index", index}, {"metadata", ref},
            {"row_count", local_count}, {"first_session_crop_video_frame_index", first},
            {"last_session_crop_video_frame_index", outputs - 1}});
    }
    require(!next_assigned(), "missing crop suffix relative to master recording");
    require(outputs == master.at("source").at("assigned_frame_offers") &&
            observations == master.at("counters").at("written"), "master count/coverage mismatch");
    require(master.at("csv") == artifact(root, master_path), "master metadata mutated during validation");
    return {{"schema_id", "orange.recording.moving_crop_metadata_correspondence"}, {"schema_version", 1},
        {"status", "matched"}, {"recording_id", master.at("recording_id")},
        {"camera_serial", master.at("camera_serial")}, {"producer_instance_id", master.at("producer_instance_id")},
        {"stream_generation", master.at("stream_generation")},
        {"authority", "master_and_crop_metadata_rows_only"}, {"media_finalization_evaluated", false},
        {"master_csv", master.at("csv")}, {"master_record_sha256", "sha256:" + checksum::sha256_hex(master.dump())},
        {"master_record_digest_domain", "nlohmann_sorted_compact_json_utf8_no_lf"},
        {"assigned_frames", assigned}, {"crop_rows", outputs}, {"clips", clip_results},
        {"correspondence_sha256", "sha256:" + correspondence.final_hex()},
        {"correspondence_digest_domain", "ascii_clip_index_local_index_session_index_recording_id_master_observation_index_comma_lf_v1"}};
}
} // namespace orange::recording
