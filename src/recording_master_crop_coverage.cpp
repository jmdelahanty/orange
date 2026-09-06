#include "recording_master_crop_coverage.h"
#include "recording_master_journal.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/sha256.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

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

uint64_t number(const json& value) {
    require(value.is_number_unsigned() ||
            (value.is_number_integer() && value.get<int64_t>() >= 0), "nonnegative integer required");
    return value.get<uint64_t>();
}
void closed(const json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size(), "moving crop record has unknown/missing fields");
    for (const auto* key : keys) require(value.contains(key), "moving crop record field missing");
}
json document(const fs::path& root, const fs::path& relative) {
    const auto path = resolve(root, relative);
    require(fs::file_size(path) <= 16 * 1024 * 1024, "metadata document exceeds size limit");
    std::ifstream in(path);
    json result;
    in >> result;
    in >> std::ws;
    require(in.eof(), "metadata document trailing data");
    return result;
}
std::string stem(const std::string& serial) {
    require(!serial.empty() && serial.size() <= 128 &&
            serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") == std::string::npos,
            "invalid moving crop camera serial");
    return "Cam" + serial;
}

// Bounded streaming output, same create-once publication contract as the master
// journal. Partial projections may remain after a failed later clip; without the
// final receipt they are not a completed product. Never overwrite on retry.
class Publication {
public:
    explicit Publication(const fs::path& destination) : destination_(destination) {
        require(!fs::exists(destination) && !fs::is_symlink(destination), "crop evidence already exists");
        staging_ = (destination.parent_path() / ".moving_crop_XXXXXX").string();
        fd_ = ::mkstemp(staging_.data());
        require(fd_ >= 0, "cannot stage moving crop evidence");
    }
    ~Publication() {
        if (fd_ >= 0) ::close(fd_);
        if (published_ && !finished_) ::unlink(destination_.c_str());
        if (!staging_.empty()) ::unlink(staging_.c_str());
    }
    Publication(const Publication&) = delete;
    Publication& operator=(const Publication&) = delete;
    void Write(const std::string& bytes) { io_.Write(fd_, bytes); }
    void Finish() {
        io_.Sync(fd_);
        const int closing = fd_;
        fd_ = -1;
        io_.Close(closing);
        require(::link(staging_.c_str(), destination_.c_str()) == 0, "cannot publish moving crop evidence");
        published_ = true;
        const int directory = ::open(destination_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(directory >= 0, "cannot open moving crop evidence directory");
        const int synced = ::fsync(directory);
        const int closed = ::close(directory);
        require(synced == 0 && closed == 0, "cannot sync moving crop evidence directory");
        finished_ = true;
    }
private:
    fs::path destination_;
    std::string staging_;
    int fd_ = -1;
    bool published_ = false, finished_ = false;
    MasterJournalIo io_;
};

// The existing detector event log is ordered per camera and recording. Compare
// every terminal event to a crop row, including original counters and exact ns.
// No hash or successful blank output can turn timeout/failed into zero detections.
json detection_coverage(const fs::path& root, const json& master,
                        const fs::path& crop_path, const fs::path& event_path) {
    const auto crop_ref = artifact(root, crop_path), event_ref = artifact(root, event_path);
    std::ifstream crops(resolve(root, crop_path)), events(resolve(root, event_path));
    std::string row, event_row;
    require(line(crops, &row), "empty crop metadata");
    const auto header = fields(row);
    auto column = [&](const char* name) {
        const auto it = std::find(header.begin(), header.end(), name);
        require(it != header.end(), "crop detection evidence column missing");
        return static_cast<std::size_t>(it - header.begin());
    };
    const auto rid = column("recording_frame_id"), local = column("local_frame_id");
    const auto camera = column("camera_frame_id"), timestamp = column("timestamp"), host = column("timestamp_sys");
    const auto detected = column("has_detection"), blank = column("blank_frame"), state = column("crop_state");
    uint64_t count = 0, blanks = 0, last_sequence = 0;
    while (line(crops, &row)) {
        const auto crop = fields(row);
        require(crop.size() == header.size(), "malformed crop detection row");
        require(line(events, &event_row), "missing detector terminal event for crop");
        const auto event = json::parse(event_row);
        require(event.at("schema_id") == "orange.yolo_event" && number(event.at("schema_version")) == 1 &&
                event.at("event_kind") == "yolo_result" && event.at("recording_id") == master.at("recording_id") &&
                event.at("camera_serial") == master.at("camera_serial"), "detector event identity mismatch");
        const auto sequence = number(event.at("event_sequence"));
        require(sequence == last_sequence + 1, "detector event sequence gap/duplicate");
        last_sequence = sequence;
        const auto& frame = event.at("frame");
        const auto& times = event.at("timestamps");
        require(frame.at("record_active") == true &&
                number(frame.at("recording_frame_id")) == integer(crop[rid]) &&
                number(frame.at("local_frame_id")) == integer(crop[local]) &&
                number(frame.at("camera_frame_id")) == integer(crop[camera]) &&
                number(times.at("camera_timestamp")) == integer(crop[timestamp]) &&
                number(times.at("timestamp_sys_ns")) == integer(crop[host]), "detector/crop source correspondence mismatch");
        const auto& yolo = event.at("yolo");
        require(yolo.at("synthetic_runtime_detection") == false && yolo.at("production_detection_valid") == true &&
                yolo.at("detection_source") == "model" &&
                yolo.at("coordinate_space") == "source_frame_pixels" &&
                yolo.at("model_id").is_string() && !yolo.at("model_id").get<std::string>().empty(),
                "detector evidence is not production model output");
        require(!yolo.contains("error") || yolo.at("error") == "", "detector terminal error");
        require(event.at("detections").is_array() &&
                number(yolo.at("detection_count")) == event.at("detections").size(), "detector count mismatch");
        const bool empty = event.at("detections").empty();
        require(yolo.at("status") == (empty ? "zero_detections" : "detections"), "unsuccessful detector terminal state");
        require(integer(crop[detected]) == (empty ? 0 : 1) && integer(crop[blank]) == (empty ? 1 : 0) &&
                crop[state] == (empty ? "blank_no_detection" : "detected_crop"), "crop blank/detection state contradicts detector");
        ++count;
        blanks += empty;
    }
    require(!line(events, &event_row), "extra detector terminal event");
    require(count > 0 && count == number(master.at("source").at("assigned_frame_offers")), "detector/master frame count mismatch");
    require(crop_ref == artifact(root, crop_path) && event_ref == artifact(root, event_path), "detector/crop evidence changed");
    return {{"assigned_frames", count}, {"successful_detection_frames", count - blanks},
            {"successful_zero_detection_frames", blanks}};
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
        const bool original_ids = column_names.count("local_frame_id") && column_names.count("camera_frame_id");
        const auto source_local = original_ids ? column("local_frame_id") : 0;
        const auto source_camera = original_ids ? column("camera_frame_id") : 0;
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
            if (original_ids) require(integer(source_fields[4]) == 1 && integer(source_fields[6]) == 1 &&
                    integer(crop[source_local]) == integer(source_fields[5]) &&
                    integer(crop[source_camera]) == integer(source_fields[7]), "crop/master original frame ID mismatch");
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

json FinalizeMovingCropMetadata(const fs::path& recording_root, const std::string& serial,
                               const fs::path& summary_path, bool stopped_normally) {
    ScopedFsuid fsuid_guard;
    require(stopped_normally, "moving crop source/recorder shutdown incomplete");
    const auto root = fs::canonical(recording_root);
    const auto prefix = stem(serial);
    const fs::path master_path = prefix + "_master_frames_v1.json";
    const fs::path crop_path = prefix + "_crop_meta.csv", events_path = prefix + "_yolo_events.jsonl";
    const auto master_ref = artifact(root, master_path), summary_ref = artifact(root, summary_path);
    const auto crop_ref = artifact(root, crop_path), events_ref = artifact(root, events_path);
    const auto master = document(root, master_path), summary = document(root, summary_path);
    const auto recording_id = master.at("recording_id").get<std::string>();
    require(master.at("camera_serial") == serial && recording_id == root.filename().string(), "moving crop parent/camera mismatch");
    require(summary.at("schema_id") == "orange.external_recorder.summary" && number(summary.at("schema_version")) == 2 &&
            summary.at("session_id") == recording_id && summary.at("stream_id") == serial + "_crop" &&
            summary.at("stream_kind") == "crop" && summary.at("output_kind") == "crop",
            "moving crop recorder summary identity mismatch");
    const auto count = number(master.at("source").at("assigned_frame_offers"));
    require(count > 0 && count < UINT64_MAX && number(summary.at("frames_received")) == count &&
            number(summary.at("frames_encoded")) == count && summary.at("worker_failed") == false,
            "external crop recorder/master count or worker failure");
    for (const auto* key : {"encode_skipped", "encode_dropped"})
        require(number(summary.at(key)) == 0, "external crop recorder skipped/dropped frames");
    // Validate the original session-global rows before producing clip projections.
    CheckMovingCropMetadataCoverage(root, master, {{recording_id, serial, 0, "session", crop_path}});
    const auto detection = detection_coverage(root, master, crop_path, events_path);

    std::vector<MovingCropClipMetadata> clips;
    json partitions = json::array();
    const auto& rolling = summary.at("rolling_output");
    require(rolling.at("enabled").is_boolean(), "recorder rolling enabled must be boolean");
    if (rolling.at("enabled") == true) {
        const auto& source_clips = rolling.at("clips");
        require(source_clips.is_array() && !source_clips.empty() && source_clips.size() <= 4096,
                "invalid rolling crop clip collection");
        uint64_t expected = 1;
        std::set<std::string> ids;
        // Recorder and metadata collection indices are zero-based. Preserve the
        // recorder's clip_id exactly, independently of its display spelling.
        for (std::size_t i = 0; i < source_clips.size(); ++i) {
            const auto& clip = source_clips.at(i);
            const auto first = number(clip.at("first_recording_frame_id"));
            const auto last = number(clip.at("last_recording_frame_id"));
            const auto n = number(clip.at("frame_count"));
            const auto id = clip.at("clip_id").get<std::string>();
            require(number(clip.at("clip_index")) == i && !id.empty() && ids.insert(id).second &&
                    first == expected && last >= first && last <= count && n == last - first + 1 &&
                    number(clip.at("packets_written")) == n && clip.at("failed") == false,
                    "rolling crop membership/range/count mismatch");
            expected = last + 1;
            const fs::path path = prefix + "_crop_clip_" + std::to_string(i) + "_meta_v1.csv";
            clips.push_back({recording_id, serial, i, id, path});
            partitions.push_back({{"clip_index", i}, {"recorder_clip_index", i}, {"clip_id", id},
                {"first_recording_frame_id", first}, {"last_recording_frame_id", last}, {"frame_count", n}});
        }
        require(expected - 1 == count, "rolling crop collection misses master suffix");
        std::ifstream input(resolve(root, crop_path));
        std::string row;
        require(line(input, &row), "crop projection header missing");
        const auto header_text = row;
        const auto header = fields(row);
        const auto at = std::find(header.begin(), header.end(), "crop_video_frame_index");
        require(at != header.end(), "crop projection local index missing");
        const auto local = static_cast<std::size_t>(at - header.begin());
        for (std::size_t i = 0; i < clips.size(); ++i) {
            Publication output(root / clips[i].metadata_relative_path);
            output.Write(header_text + "\n");
            const auto n = number(partitions.at(i).at("frame_count"));
            for (uint64_t index = 0; index < n; ++index) {
                require(line(input, &row), "crop projection missing row");
                auto values = fields(row);
                require(values.size() == header.size(), "crop projection malformed row");
                values[local] = std::to_string(index);
                std::string projected;
                for (std::size_t c = 0; c < values.size(); ++c) {
                    if (c) projected += ',';
                    projected += values[c];
                }
                output.Write(projected + "\n");
            }
            output.Finish();
        }
        require(!line(input, &row), "crop projection extra row");
    } else {
        clips.push_back({recording_id, serial, 0, "single", crop_path});
        partitions.push_back({{"clip_index", 0}, {"recorder_clip_index", nullptr}, {"clip_id", "single"},
            {"first_recording_frame_id", 1}, {"last_recording_frame_id", count}, {"frame_count", count}});
    }
    const auto correspondence = CheckMovingCropMetadataCoverage(root, master, clips);
    require(master_ref == artifact(root, master_path) && summary_ref == artifact(root, summary_path) &&
            crop_ref == artifact(root, crop_path) && events_ref == artifact(root, events_path),
            "moving crop source evidence mutated during finalization");
    json receipt = {{"schema_id", "orange.recording.moving_crop_metadata_completion"}, {"schema_version", 1},
        {"status", "matched"}, {"authority", "master_crop_and_detector_metadata_only"},
        {"media_finalization_evaluated", false}, {"recording_id", recording_id}, {"camera_serial", serial},
        {"master_descriptor", master_ref}, {"recorder_summary", summary_ref}, {"session_crop_metadata", crop_ref},
        {"detector_events", events_ref}, {"partitions", partitions},
        {"detection_coverage", detection}, {"metadata_correspondence", correspondence}};
    Publication output(root / (prefix + "_moving_crop_metadata_v1.json"));
    output.Write(receipt.dump() + "\n");
    output.Finish();
    return receipt;
}

json RequireMovingCropMetadataReceipt(const fs::path& recording_root, const json& master) {
    const auto root = fs::canonical(recording_root);
    const auto prefix = stem(master.at("camera_serial").get<std::string>());
    const fs::path path = prefix + "_moving_crop_metadata_v1.json";
    const auto ref = artifact(root, path);
    const auto receipt = document(root, path);
    closed(receipt, {"schema_id", "schema_version", "status", "authority", "media_finalization_evaluated",
        "recording_id", "camera_serial", "master_descriptor", "recorder_summary", "session_crop_metadata",
        "detector_events", "partitions", "detection_coverage", "metadata_correspondence"});
    require(receipt.at("schema_id") == "orange.recording.moving_crop_metadata_completion" &&
            number(receipt.at("schema_version")) == 1 && receipt.at("status") == "matched" &&
            receipt.at("authority") == "master_crop_and_detector_metadata_only" &&
            receipt.at("media_finalization_evaluated") == false &&
            receipt.at("recording_id") == master.at("recording_id") &&
            receipt.at("camera_serial") == master.at("camera_serial"), "moving crop completion receipt identity mismatch");
    for (const auto* key : {"master_descriptor", "recorder_summary", "session_crop_metadata", "detector_events"}) {
        const auto& bound = receipt.at(key);
        require(bound == artifact(root, bound.at("relative_path").get<std::string>()), "moving crop receipt artifact changed");
    }
    require(receipt.at("master_descriptor") == artifact(root, prefix + "_master_frames_v1.json"),
            "moving crop receipt binds different master");
    const auto& coverage = receipt.at("metadata_correspondence");
    require(coverage.at("master_csv") == artifact(root, coverage.at("master_csv").at("relative_path").get<std::string>()),
            "moving crop master CSV changed");
    require(coverage.at("status") == "matched" && coverage.at("recording_id") == master.at("recording_id") &&
            coverage.at("camera_serial") == master.at("camera_serial") &&
            coverage.at("producer_instance_id") == master.at("producer_instance_id") &&
            coverage.at("stream_generation") == master.at("stream_generation") && coverage.at("master_csv") == master.at("csv") &&
            coverage.at("assigned_frames") == master.at("source").at("assigned_frame_offers") &&
            coverage.at("crop_rows") == coverage.at("assigned_frames") &&
            receipt.at("detection_coverage").at("assigned_frames") == coverage.at("assigned_frames"),
            "moving crop receipt/master correspondence mismatch");
    require(coverage.at("clips").is_array() && !coverage.at("clips").empty() &&
            coverage.at("clips").size() == receipt.at("partitions").size(), "moving crop receipt clip collection mismatch");
    const auto& detection = receipt.at("detection_coverage");
    closed(detection, {"assigned_frames", "successful_detection_frames", "successful_zero_detection_frames"});
    const auto total = number(coverage.at("assigned_frames"));
    const auto blanks = number(detection.at("successful_zero_detection_frames"));
    require(blanks <= total && number(detection.at("successful_detection_frames")) == total - blanks,
            "moving crop detection totals inconsistent");
    uint64_t frames = 0;
    std::size_t index = 0;
    std::set<std::string> clip_ids;
    for (const auto& clip : coverage.at("clips")) {
        const auto& bound = clip.at("metadata");
        require(bound == artifact(root, bound.at("relative_path").get<std::string>()), "moving crop clip metadata changed");
        const auto& partition = receipt.at("partitions").at(index);
        closed(partition, {"clip_index", "recorder_clip_index", "clip_id", "first_recording_frame_id", "last_recording_frame_id", "frame_count"});
        const auto n = number(clip.at("row_count"));
        require(n > 0 && frames < total && n <= total - frames &&
                number(clip.at("clip_index")) == index && number(partition.at("clip_index")) == index &&
                clip.at("clip_id") == partition.at("clip_id") && clip_ids.insert(clip.at("clip_id").get<std::string>()).second &&
                number(partition.at("frame_count")) == n &&
                number(partition.at("first_recording_frame_id")) == frames + 1 &&
                number(partition.at("last_recording_frame_id")) == frames + n &&
                number(clip.at("first_session_crop_video_frame_index")) == frames &&
                number(clip.at("last_session_crop_video_frame_index")) == frames + n - 1,
                "moving crop receipt clip partition mismatch");
        require(partition.at("recorder_clip_index").is_null()
                    ? (coverage.at("clips").size() == 1 && clip.at("clip_id") == "single")
                    : number(partition.at("recorder_clip_index")) == index,
                "moving crop recorder index mismatch");
        frames += n;
        ++index;
    }
    require(frames == total, "moving crop receipt partition coverage incomplete");
    require(ref == artifact(root, path), "moving crop receipt changed during parent finalization");
    return ref;
}

void ApplyRequiredMovingCropMetadataGate(const fs::path& recording_root, json* parent) {
    if (!fs::exists(recording_root)) return;
    const auto root = fs::canonical(recording_root);
    const fs::path snapshot_path = fs::exists(root / "recording_snapshot_start.json")
        ? "recording_snapshot_start.json" : "recording_snapshot.json";
    if (!fs::exists(root / snapshot_path)) return;
    const auto snapshot = document(root, snapshot_path);
    if (!snapshot.contains("session") || !snapshot.at("session").contains("moving_crop_master_coverage")) return;
    const auto& marker = snapshot.at("session").at("moving_crop_master_coverage");
    json result = {{"schema_version", 1}, {"required", true}, {"profile", "external_moving_crop_full_rate_v1"},
                   {"status", "pending"}, {"cameras", json::array()}};
    const auto status = parent->value("status", std::string());
    if (status == "completed" || status == "failed" || status == "interrupted" || status == "incomplete") {
        try {
            closed(marker, {"schema_version", "required", "profile"});
            require(number(marker.at("schema_version")) == 1 && marker.at("required") == true &&
                    marker.at("profile") == "external_moving_crop_full_rate_v1", "unsupported required moving crop profile");
            const auto& cameras = snapshot.at("session").at("master_frame_journal").at("cameras");
            require(cameras.is_array() && !cameras.empty(), "moving crop master camera set missing");
            std::set<std::string> seen;
            for (const auto& camera : cameras) {
                const auto serial = camera.at("camera_serial").get<std::string>();
                require(seen.insert(serial).second, "duplicate moving crop camera");
                const auto master = document(root, stem(serial) + "_master_frames_v1.json");
                require(master.at("status") == "complete" && master.at("recording_id") == parent->at("session_id") &&
                        master.at("recording_id") == camera.at("recording_id") &&
                        master.at("producer_instance_id") == camera.at("producer_instance_id") &&
                        master.at("stream_generation") == camera.at("stream_generation"), "moving crop master/start identity mismatch");
                // Resolve throwing validation before constructing json_ref
                // initializer temporaries (vendored JSON exception path).
                const auto receipt_ref = RequireMovingCropMetadataReceipt(root, master);
                result["cameras"].push_back({{"camera_serial", serial}, {"receipt", receipt_ref}});
            }
            result["status"] = "matched";
        } catch (const std::exception& ex) {
            result["status"] = "failed";
            result["reason"] = ex.what();
            if (status == "completed") (*parent)["status"] = "failed";
        }
    }
    (*parent)["moving_crop_master_coverage"] = std::move(result);
}
} // namespace orange::recording
