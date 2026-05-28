#include "session/crop_rolling_sidecars.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace orange::session {

namespace {

void set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
}

std::string first_csv_field(const std::string& line)
{
    const size_t comma = line.find(',');
    return comma == std::string::npos ? line : line.substr(0, comma);
}

bool parse_u64_field(const std::string& value, uint64_t* parsed)
{
    if (value.empty() || !parsed) {
        return false;
    }
    uint64_t out = 0;
    for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (out > (std::numeric_limits<uint64_t>::max() - digit) / 10ULL) {
            return false;
        }
        out = out * 10ULL + digit;
    }
    *parsed = out;
    return true;
}

bool ranges_overlap(const RecordingFrameCsvRange& lhs,
                    const RecordingFrameCsvRange& rhs)
{
    return lhs.first_recording_frame_id <= rhs.last_recording_frame_id &&
           rhs.first_recording_frame_id <= lhs.last_recording_frame_id;
}

bool validate_ranges(const std::vector<RecordingFrameCsvRange>& ranges,
                     std::string* error_out)
{
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (range.first_recording_frame_id == 0 ||
            range.last_recording_frame_id == 0 ||
            range.first_recording_frame_id > range.last_recording_frame_id) {
            set_error(
                error_out,
                "invalid recording_frame_id range at index " + std::to_string(i));
            return false;
        }
        if (range.output_path.empty()) {
            set_error(error_out, "empty output path for range index " + std::to_string(i));
            return false;
        }
        for (size_t j = i + 1; j < ranges.size(); ++j) {
            if (ranges_overlap(range, ranges[j])) {
                set_error(
                    error_out,
                    "overlapping recording_frame_id ranges at indexes " +
                        std::to_string(i) + " and " + std::to_string(j));
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool split_recording_frame_csv_by_ranges(
    const std::string& input_path,
    std::vector<RecordingFrameCsvRange>* ranges,
    std::string* error_out)
{
    if (input_path.empty()) {
        set_error(error_out, "input CSV path is empty");
        return false;
    }
    if (!ranges) {
        set_error(error_out, "range output list is null");
        return false;
    }
    if (!validate_ranges(*ranges, error_out)) {
        return false;
    }

    std::ifstream input(input_path);
    if (!input) {
        set_error(error_out, "failed to open input CSV: " + input_path);
        return false;
    }

    std::string header;
    if (!std::getline(input, header)) {
        set_error(error_out, "input CSV is empty: " + input_path);
        return false;
    }
    if (first_csv_field(header) != "recording_frame_id") {
        set_error(
            error_out,
            "input CSV first column must be recording_frame_id: " + input_path);
        return false;
    }

    std::vector<std::ofstream> outputs;
    outputs.reserve(ranges->size());
    for (auto& range : *ranges) {
        range.rows_written = 0;
        const std::filesystem::path output_path(range.output_path);
        const std::filesystem::path parent = output_path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                set_error(
                    error_out,
                    "failed to create output CSV parent " + parent.string() +
                        ": " + ec.message());
                return false;
            }
        }
        outputs.emplace_back(range.output_path, std::ios::out | std::ios::trunc);
        if (!outputs.back()) {
            set_error(error_out, "failed to open output CSV: " + range.output_path);
            return false;
        }
        outputs.back() << header << '\n';
    }

    std::string line;
    uint64_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        uint64_t recording_frame_id = 0;
        if (!parse_u64_field(first_csv_field(line), &recording_frame_id)) {
            set_error(
                error_out,
                "invalid recording_frame_id at " + input_path + ":" +
                    std::to_string(line_number));
            return false;
        }
        for (size_t i = 0; i < ranges->size(); ++i) {
            auto& range = (*ranges)[i];
            if (recording_frame_id >= range.first_recording_frame_id &&
                recording_frame_id <= range.last_recording_frame_id) {
                outputs[i] << line << '\n';
                range.rows_written++;
                break;
            }
        }
    }

    for (size_t i = 0; i < outputs.size(); ++i) {
        outputs[i].flush();
        if (!outputs[i]) {
            set_error(error_out, "failed while writing output CSV: " + (*ranges)[i].output_path);
            return false;
        }
    }
    return true;
}

}  // namespace orange::session
