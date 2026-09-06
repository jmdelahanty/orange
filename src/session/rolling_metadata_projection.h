#pragma once

#include "json.hpp"
#include "gui/spatial_layout/sha256.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace orange::session {
// Opt-in finalization-only compatibility adapter. Produces one metadata CSV
// accepted by the existing acquisition_index_mapping v1; never renumbers frames
// or creates timestamps. The sibling provenance binds each original clip CSV.
// A non-dense or unfinished recording remains ineligible for this v1 profile.
inline nlohmann::json project_dense_rolling_metadata(
    const nlohmann::json& manifest, const std::filesystem::path& manifest_path)
{
    namespace fs = std::filesystem;
    namespace checksum = orange::gui::spatial_layout::checksum;
    using json = nlohmann::json;
    auto fail = [](const std::string& reason) { throw std::runtime_error(reason); };
    if (manifest.value("mode", "") != "rolling_clips" ||
        manifest.value("status", "") != "completed" ||
        !manifest.at("recording").value("drain_completed", false))
        fail("rolling_projection_recording_not_closed");
    if (!manifest.at("session_id").is_string() || manifest.at("session_id").get<std::string>().empty())
        fail("rolling_projection_missing_recording_identity");
    const auto root = fs::canonical(manifest_path.parent_path());
    auto relative = [&](const std::string& value) {
        fs::path p(value);
        if (p.empty()) fail("rolling_projection_missing_path");
        auto resolved = fs::canonical(p.is_absolute() ? p : root / p);
        auto rel = resolved.lexically_relative(root);
        if (rel.empty() || rel.is_absolute()) fail("rolling_projection_path_outside_recording");
        for (const auto& component : rel)
            if (component == "..") fail("rolling_projection_path_outside_recording");
        return rel;
    };
    auto fields = [](const std::string& line) {
        std::vector<std::string> parts;
        size_t begin = 0;
        for (;;) {
            const auto end = line.find(',', begin);
            parts.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return parts;
    };
    auto integer = [&](const std::string& s) {
        uint64_t n = 0;
        const auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
        if (s.empty() || parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
            fail("rolling_projection_noninteger_metadata");
        return n;
    };
    const auto& clips = manifest.at("clips");
    const auto& cameras = manifest.at("cameras");
    if (!clips.is_array() || clips.empty() || !cameras.is_array() || cameras.empty())
        fail("rolling_projection_missing_streams");
    std::set<std::string> clip_ids, camera_ids;
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto& clip = clips[i];
        const std::string id = clip.at("clip_id").get<std::string>();
        if (id.empty() || !clip_ids.insert(id).second || clip.at("clip_index") != i ||
            clip.value("session_id", "") != manifest.at("session_id") ||
            clip.value("status", "") != "completed" || !clip.value("drain_completed", false) ||
            clip.value("final_clip", false) != (i + 1 == clips.size()) ||
            clip.at("rollover").value("pending_next_clip", false))
            fail("rolling_projection_invalid_clip_order_or_closure");
    }
    json projected = manifest;
    json provenance = {{"schema_id", "orange.recording.rolling_metadata_projection"},
        {"schema_version", 1}, {"status", "finalized"},
        {"recording_id", manifest.at("session_id")},
        {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
        {"policy", "dense_parent_ids_preserved_clip_csv_rows_in_order_v1"},
        {"source_order", "encoded_metadata_row_order_with_declared_packet_parity"},
        {"camera_streams", json::object()}};

    for (const auto& serial_json : cameras) {
        const auto serial = serial_json.get<std::string>();
        if (serial.empty() || !camera_ids.insert(serial).second)
            fail("rolling_projection_duplicate_camera");
        std::string temporary = (root / ".rolling_projection_XXXXXX").string();
        const int fd = ::mkstemp(temporary.data());
        if (fd < 0) fail("rolling_projection_staging_failed");
        ::close(fd);
        struct Cleanup {
            fs::path path;
            ~Cleanup() { std::error_code ec; fs::remove(path, ec); }
        } cleanup{temporary};
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) fail("rolling_projection_staging_failed");
        checksum::StreamingSha256 output_hash;
        uint64_t output_bytes = 0, total = 0;
        auto emit = [&](const std::string& line) {
            output << line;
            output_hash.update(line);
            output_bytes += line.size();
        };
        std::string expected_header;
        json sources = json::array();
        std::set<std::string> source_paths;
        for (const auto& clip : clips) {
            const auto artifact = clip.at("camera_artifacts").at(serial);
            const auto clip_directory = relative(clip.at("directory").get<std::string>());
            const auto meta_rel = relative(artifact.at("metadata").get<std::string>());
            const auto video_rel = relative(artifact.at("video").get<std::string>());
            for (const auto& path : {meta_rel, video_rel}) {
                const auto within = path.lexically_relative(clip_directory);
                if (within.empty() || within.is_absolute()) fail("rolling_projection_clip_path_mismatch");
                for (const auto& component : within)
                    if (component == "..") fail("rolling_projection_clip_path_mismatch");
            }
            if (!source_paths.insert(meta_rel.generic_string()).second)
                fail("rolling_projection_duplicate_source");
            const auto source_path = root / meta_rel;
            const auto size_before = fs::file_size(source_path);
            const auto time_before = fs::last_write_time(source_path);
            std::ifstream input(source_path, std::ios::binary);
            checksum::StreamingSha256 source_hash;
            uint64_t bytes = 0;
            auto read_line = [&](std::string& line) {
                if (!std::getline(input, line)) return false;
                if (input.eof() || line.size() > 16384)
                    fail("rolling_projection_partial_or_oversized_row");
                line += '\n';
                source_hash.update(line);
                bytes += line.size();
                return true;
            };
            std::string header;
            if (!read_line(header)) fail("rolling_projection_missing_header");
            // Preserve bytes, including CRLF. No repair of malformed source rows.
            auto parse_line = [&](std::string line) {
                if (!line.empty() && line.back() == '\n') line.pop_back();
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return fields(line);
            };
            const auto columns = parse_line(header);
            std::set<std::string> unique_columns(columns.begin(), columns.end());
            if (unique_columns.size() != columns.size() || unique_columns.count(""))
                fail("rolling_projection_invalid_header");
            auto column = [&](const char* name) {
                for (size_t i = 0; i < columns.size(); ++i) if (columns[i] == name) return i;
                fail("rolling_projection_missing_identity_or_clock_column");
                return size_t{0};
            };
            const auto alias = column("frame_id"), id = column("recording_frame_id");
            const auto camera_time = column("timestamp"), host_time = column("timestamp_sys");
            if (expected_header.empty()) { expected_header = header; emit(header); }
            else if (header != expected_header) fail("rolling_projection_header_mismatch");
            const uint64_t first = total + 1;
            uint64_t count = 0;
            std::string line;
            while (read_line(line)) {
                const auto values = parse_line(line);
                if (values.size() != columns.size()) fail("rolling_projection_malformed_row");
                const auto frame = integer(values[id]);
                if (total >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
                    frame != total + 1 || integer(values[alias]) != frame)
                    fail("rolling_projection_nondense_or_mispaired_identity");
                (void)integer(values[camera_time]);
                (void)integer(values[host_time]);
                emit(line);
                ++total;
                ++count;
            }
            if (!input.eof() || input.bad() || bytes != size_before ||
                fs::file_size(source_path) != size_before || fs::last_write_time(source_path) != time_before)
                fail("rolling_projection_source_changed_or_io_failure");
            if (count == 0 || artifact.at("frame_count") != count ||
                artifact.at("first_recording_frame_id") != first ||
                artifact.at("last_recording_frame_id") != total ||
                artifact.at("packet_count") != count ||
                artifact.value("packet_count_source", "").empty() ||
                artifact.value("packet_count_source", "unavailable") == "unavailable")
                fail("rolling_projection_clip_identity_or_packet_parity_mismatch");
            sources.push_back({{"clip_id", clip.at("clip_id")}, {"clip_index", clip.at("clip_index")},
                {"metadata_relative_path", meta_rel.generic_string()},
                {"metadata_size_bytes", bytes}, {"metadata_sha256", "sha256:" + source_hash.final_hex()},
                {"video_relative_path", video_rel.generic_string()},
                {"first_recording_frame_id", first}, {"last_recording_frame_id", total},
                {"first_parent_output_index", first - 1}, {"first_clip_output_index", 0},
                {"frame_count", count}, {"packet_count_source", artifact.at("packet_count_source")}});
        }
        output.flush();
        if (!output) fail("rolling_projection_write_failed");
        output.close();
        if (!output) fail("rolling_projection_close_failed");
        const std::string digest = "sha256:" + output_hash.final_hex();
        const std::string name = "rolling_metadata_" + digest.substr(7) + ".csv";
        std::error_code link_error;
        fs::create_hard_link(temporary, root / name, link_error);
        if (link_error) {
            std::string existing_hash, error;
            if (link_error != std::errc::file_exists || fs::is_symlink(root / name) ||
                !checksum::file_sha256(root / name, &existing_hash, &error) || existing_hash != digest)
                fail("rolling_projection_publish_conflict");
        }
        provenance["camera_streams"][serial] = {{"sources", sources},
            {"projection", {{"relative_path", name}, {"size_bytes", output_bytes}, {"sha256", digest}}}};
        // This is a metadata aggregate, not a fictitious parent video stream.
        projected["camera_artifacts"][serial] = {{"metadata", name},
            {"frame_count", total}, {"first_recording_frame_id", 1},
            {"last_recording_frame_id", total}, {"recording_frame_id_gaps", 0}};
    }
    projected["rolling_metadata_projection"] = provenance;
    projected["rolling_metadata_projection_sha256"] = "sha256:" + checksum::sha256_hex(provenance.dump());
    return projected;
}
}  // namespace orange::session
