#include "recording_master_crop_coverage.h"
#include "recording_master_journal.h"
#include "recording_media_decode.h"
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

namespace {
fs::path media_relative(const fs::path& root, const json& path) {
    const fs::path value = path.get<std::string>();
    const auto relative = value.is_absolute() ? value.lexically_relative(root) : value;
    (void)resolve(root, relative); // no traversal, aliases, missing files or non-files
    return relative;
}
void returned_crop_proof(const json& summary, uint64_t count) {
    const auto& proof = summary.at("frame_identity_proof");
    require(proof.at("schema_id") == "orange.external_recorder.frame_identity_proof" &&
        number(proof.at("schema_version")) == 2 && proof.at("status") == "passed" &&
        proof.at("canonical_field") == "recording_frame_id" &&
        proof.at("scope") == "recording_session_and_camera_stream" &&
        proof.at("row_granularity") == "one_encoded_video_frame" &&
        proof.at("continuity_policy") == "encoded_subset" &&
        number(proof.at("source_frames_skipped_by_policy")) == 0 &&
        number(proof.at("source_frames_dropped")) == 0, "crop returned-identity proof invalid");
    const auto& binding = proof.at("video_binding");
    require(binding.at("method") == "nvenc_input_timestamp_to_output_timestamp_registry" &&
        binding.at("metadata_write_event") == "completed_gop_after_returned_identity_match" &&
        binding.at("verification_rule_id") == "orange.external_recorder.frame_identity.v2" &&
        binding.at("verified") == true && binding.at("first_packet_write_error_code").is_null(),
        "crop returned-identity binding invalid");
    for (const auto* key : {"submitted_frame_identities", "returned_identity_matches", "encoded_video_frames",
            "metadata_rows", "packet_submissions_accepted", "packet_write_attempts", "packets_written"})
        require(number(binding.at(key)) == count, "crop returned identity/packet count mismatch");
    for (const auto* key : {"identity_mismatches", "outstanding_submitted_identities", "packet_submissions_rejected", "packet_write_failures"})
        require(number(binding.at(key)) == 0, "crop returned identity/packet failure");
    const auto& merged = summary.at("merged_output");
    require(merged.at("coordinator_enabled") == true && number(merged.at("pending_gops")) == 0 &&
            merged.at("failed") == false, "crop GOP coordinator did not finish");
}
void container_complete(const json& sidecar, uint64_t count, uint64_t bytes) {
    require(sidecar.at("schema_id") == "orange.video_container_finalization" && number(sidecar.at("schema_version")) == 2 &&
            sidecar.at("status") == "complete" && sidecar.at("terminal") == true, "crop container finalization incomplete");
    const auto& packets = sidecar.at("packet_writes");
    for (const auto* key : {"submissions_accepted", "write_attempts", "packets_written"})
        require(number(packets.at(key)) == count, "crop mux packet count mismatch");
    for (const auto* key : {"submissions_rejected", "write_failures"})
        require(number(packets.at(key)) == 0, "crop mux packet failure");
    require(packets.at("complete") == true && packets.at("writer_error_latched") == false &&
            packets.at("muxer_flush_attempted") == true && packets.at("muxer_flush_succeeded") == true &&
            packets.at("first_write_error_code").is_null() && packets.at("muxer_flush_error_code").is_null(),
            "crop packet writer did not finish");
    const auto& container = sidecar.at("container");
    for (const auto* key : {"header_written", "trailer_attempted", "trailer_written", "output_close_attempted", "output_closed", "finalized"})
        require(container.at(key) == true, "crop container close/trailer incomplete");
    require(number(container.at("file_size_bytes")) == bytes && container.at("trailer_error_code").is_null() &&
            container.at("output_close_error_code").is_null() && container.at("file_size_error").is_null(),
            "crop finalized container size/error mismatch");
}
}

json FinalizeMovingCropMedia(const fs::path& recording_root, const std::string& serial, int width, int height) {
    ScopedFsuid fsuid_guard;
    const auto root = fs::canonical(recording_root);
    const auto prefix = stem(serial);
    const auto master = document(root, prefix + "_master_frames_v1.json");
    const auto metadata_ref = RequireMovingCropMetadataReceipt(root, master);
    const auto metadata = document(root, metadata_ref.at("relative_path").get<std::string>());
    const auto summary_ref = metadata.at("recorder_summary");
    const auto summary = document(root, summary_ref.at("relative_path").get<std::string>());
    const auto count = number(master.at("source").at("assigned_frame_offers"));
    returned_crop_proof(summary, count);
    const bool rolling = summary.at("rolling_output").at("enabled") == true;
    require(width > 0 && height > 0 && width <= 8192 && height <= 8192, "invalid crop media raster");
    require(summary.at("codec") == "hevc" && summary.at("tuning") == "lossless", "moving crop codec/profile mismatch");
    json videos = json::array();
    std::set<std::string> distinct;
    std::ifstream crops(resolve(root, prefix + "_crop_meta.csv"));
    std::string row;
    require(line(crops, &row), "session crop header missing");
    const auto crop_header = fields(row);
    auto column = [](const std::vector<std::string>& header, const char* name) {
        const auto it = std::find(header.begin(), header.end(), name);
        require(it != header.end() && std::count(header.begin(), header.end(), name) == 1, "recorder/crop column missing or duplicated");
        return static_cast<std::size_t>(it - header.begin());
    };
    for (std::size_t i = 0; i < metadata.at("partitions").size(); ++i) {
        const auto& partition = metadata.at("partitions").at(i);
        const auto& output = rolling ? summary.at("rolling_output").at("clips").at(i) : summary.at("merged_output");
        const auto video_path = media_relative(root, output.at("mp4"));
        const auto recorder_path = media_relative(root, output.at("metadata"));
        const auto sidecar_path = fs::path(video_path.string() + ".finalization.json");
        require(distinct.insert(video_path.string()).second && distinct.insert(recorder_path.string()).second,
                "crop media output paths reused");
        const auto video_ref = artifact(root, video_path), recorder_ref = artifact(root, recorder_path),
                   sidecar_ref = artifact(root, sidecar_path);
        const auto n = number(partition.at("frame_count"));
        container_complete(document(root, sidecar_path), n, number(video_ref.at("size_bytes")));
        std::ifstream recorder(resolve(root, recorder_path));
        require(line(recorder, &row), "recorder metadata header missing");
        const auto header = fields(row);
        for (uint64_t frame = 0; frame < n; ++frame) {
            require(line(recorder, &row), "missing recorder metadata row");
            const auto actual = fields(row);
            require(line(crops, &row), "missing session crop row");
            const auto expected = fields(row);
            require(actual.size() == header.size() && expected.size() == crop_header.size(), "malformed crop/recorder metadata row");
            for (const auto& names : {std::pair{"recording_frame_id", "recording_frame_id"},
                    std::pair{"frame_id", "recording_frame_id"}, std::pair{"local_frame_id", "local_frame_id"},
                    std::pair{"timestamp", "timestamp"}, std::pair{"timestamp_sys", "timestamp_sys"}})
                require(integer(actual.at(column(header, names.first))) == integer(expected.at(column(crop_header, names.second))),
                        "encoded crop identity/timestamp differs from master-bound metadata");
        }
        require(!line(recorder, &row), "extra recorder metadata row");
        RequireDecodedCropMedia(resolve(root, video_path), width, height, n);
        require(video_ref == artifact(root, video_path) && recorder_ref == artifact(root, recorder_path) &&
                sidecar_ref == artifact(root, sidecar_path), "crop media changed during validation");
        videos.push_back({{"clip_index", i}, {"clip_id", partition.at("clip_id")}, {"frame_count", n},
            {"first_recording_frame_id", partition.at("first_recording_frame_id")},
            {"last_recording_frame_id", partition.at("last_recording_frame_id")},
            {"video", video_ref}, {"recorder_metadata", recorder_ref}, {"container_finalization", sidecar_ref},
            {"crop_metadata", metadata.at("metadata_correspondence").at("clips").at(i).at("metadata")}});
    }
    require(!line(crops, &row), "extra session crop row");
    require(metadata_ref == RequireMovingCropMetadataReceipt(root, master), "crop metadata changed during media validation");
    json receipt = {{"schema_id", "orange.recording.moving_crop_media_completion"}, {"schema_version", 1},
        {"status", "complete"}, {"recording_id", master.at("recording_id")}, {"camera_serial", serial},
        {"validation_profile", "returned_identity_v2_mux_and_full_hevc_decode_v1"},
        {"metadata_completion", metadata_ref}, {"width", width}, {"height", height},
        {"frame_count", count}, {"mode", rolling ? "rolling_clips" : "single_clip"}, {"videos", videos}};
    Publication publication(root / (prefix + "_moving_crop_media_v1.json"));
    publication.Write(receipt.dump() + "\n"); publication.Finish();
    return receipt;
}

json RequireMovingCropMediaReceipt(const fs::path& recording_root, const json& master) {
    const auto root = fs::canonical(recording_root);
    const auto path = stem(master.at("camera_serial").get<std::string>()) + "_moving_crop_media_v1.json";
    const auto ref = artifact(root, path), receipt = document(root, path);
    closed(receipt, {"schema_id", "schema_version", "status", "recording_id", "camera_serial", "validation_profile",
        "metadata_completion", "width", "height", "frame_count", "mode", "videos"});
    require(receipt.at("schema_id") == "orange.recording.moving_crop_media_completion" && number(receipt.at("schema_version")) == 1 &&
        receipt.at("status") == "complete" && receipt.at("recording_id") == master.at("recording_id") &&
        receipt.at("camera_serial") == master.at("camera_serial") &&
        receipt.at("validation_profile") == "returned_identity_v2_mux_and_full_hevc_decode_v1" &&
        receipt.at("metadata_completion") == RequireMovingCropMetadataReceipt(root, master) &&
        receipt.at("frame_count") == master.at("source").at("assigned_frame_offers"), "crop media receipt identity mismatch");
    const auto metadata = document(root, receipt.at("metadata_completion").at("relative_path").get<std::string>());
    const auto summary = document(root, metadata.at("recorder_summary").at("relative_path").get<std::string>());
    const bool rolling = summary.at("rolling_output").at("enabled") == true;
    require(number(receipt.at("width")) > 0 && number(receipt.at("width")) <= 8192 &&
        number(receipt.at("height")) > 0 && number(receipt.at("height")) <= 8192 &&
        receipt.at("mode") == (rolling ? "rolling_clips" : "single_clip"), "crop media raster/mode invalid");
    returned_crop_proof(summary, number(receipt.at("frame_count")));
    require(receipt.at("videos").is_array() && receipt.at("videos").size() == metadata.at("partitions").size(), "crop media partition mismatch");
    for (std::size_t i = 0; i < receipt.at("videos").size(); ++i) {
        const auto& video = receipt.at("videos").at(i);
        closed(video, {"clip_index", "clip_id", "frame_count", "first_recording_frame_id", "last_recording_frame_id",
            "video", "recorder_metadata", "container_finalization", "crop_metadata"});
        for (const auto* key : {"clip_index", "clip_id", "frame_count", "first_recording_frame_id", "last_recording_frame_id"})
            require(video.at(key) == metadata.at("partitions").at(i).at(key), "crop media partition identity mismatch");
        const auto& output = rolling ? summary.at("rolling_output").at("clips").at(i) : summary.at("merged_output");
        const auto video_path = media_relative(root, output.at("mp4"));
        require(video.at("video").at("relative_path") == video_path.generic_string() &&
            video.at("recorder_metadata").at("relative_path") == media_relative(root, output.at("metadata")).generic_string() &&
            video.at("container_finalization").at("relative_path") == video_path.generic_string() + ".finalization.json" &&
            video.at("crop_metadata") == metadata.at("metadata_correspondence").at("clips").at(i).at("metadata"),
            "crop media receipt differs from recorder/metadata partition");
        for (const auto* key : {"video", "recorder_metadata", "container_finalization", "crop_metadata"}) {
            const auto& bound = video.at(key);
            require(bound == artifact(root, bound.at("relative_path").get<std::string>()), "finalized crop media changed");
        }
        RequireCropMediaContainerRaster(resolve(root, video_path), number(receipt.at("width")), number(receipt.at("height")));
    }
    require(ref == artifact(root, path), "crop media receipt changed while reading");
    return ref;
}

void ApplyRequiredMovingCropMediaGate(const fs::path& recording_root, json* parent) {
    if (!fs::exists(recording_root)) return;
    const auto root = fs::canonical(recording_root);
    const fs::path path = fs::exists(root / "recording_snapshot_start.json") ? "recording_snapshot_start.json" : "recording_snapshot.json";
    if (!fs::exists(root / path)) return;
    const auto snapshot = document(root, path);
    if (!snapshot.contains("session") || !snapshot.at("session").contains("moving_crop_encoded_media")) return;
    json result = {{"schema_version", 1}, {"required", true}, {"profile", "returned_identity_v2_mux_and_full_hevc_decode_v1"},
        {"status", "pending"}, {"cameras", json::array()}};
    const auto status = parent->value("status", std::string());
    if (status == "completed" || status == "failed" || status == "incomplete" || status == "interrupted") {
        try {
            const auto& marker = snapshot.at("session").at("moving_crop_encoded_media");
            closed(marker, {"schema_version", "required", "profile"});
            require(number(marker.at("schema_version")) == 1 && marker.at("required") == true &&
                marker.at("profile") == "returned_identity_v2_mux_and_full_hevc_decode_v1", "invalid required encoded crop media profile");
            const auto& cameras = snapshot.at("session").at("master_frame_journal").at("cameras");
            require(cameras.is_array() && !cameras.empty(), "required crop media master cameras absent");
            std::set<std::string> serials;
            for (const auto& camera : cameras) {
                const auto serial = camera.at("camera_serial").get<std::string>();
                require(serials.insert(serial).second, "duplicate encoded crop camera");
                const auto master = document(root, stem(serial) + "_master_frames_v1.json");
                require(master.at("status") == "complete" && master.at("recording_id") == parent->at("session_id") &&
                    master.at("recording_id") == camera.at("recording_id") &&
                    master.at("producer_instance_id") == camera.at("producer_instance_id") &&
                    master.at("stream_generation") == camera.at("stream_generation"), "encoded crop parent/master identity mismatch");
                const auto ref = RequireMovingCropMediaReceipt(root, master);
                result["cameras"].push_back({{"camera_serial", serial}, {"receipt", ref}});
            }
            result["status"] = "complete";
        } catch (const std::exception& ex) {
            result["status"] = "failed"; result["reason"] = ex.what();
            if (status == "completed") (*parent)["status"] = "failed";
        }
    }
    (*parent)["moving_crop_encoded_media"] = result;
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
