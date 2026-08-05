#include "external_recorder_frame_metadata.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_writer_tracks_rows_and_gaps()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "orange_external_recorder_frame_metadata_tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "Cam2010096_external_meta.csv";

    orange::external_recorder::FrameMetadataCsvWriter writer;
    std::string error;
    expect(writer.Open(path.string(), &error), "writer opens: " + error);
    expect(writer.Write({1, 11, 0, 0, 1770000000000000037ULL,
                         1770000000000000000ULL, 5, 6, 0, 128}, &error),
           "first row writes: " + error);
    expect(writer.Write({3, 13, 0, 2, 1770000000066666703ULL,
                         1770000000066666666ULL, 5, 6, 0, 128}, &error),
           "second row writes: " + error);
    expect(!writer.Write({3, 13, 0, 2, 1, 1, 5, 6, 0, 128}, &error),
           "duplicate recording frame id is rejected");
    expect(writer.Close(&error), "writer closes: " + error);

    const auto& summary = writer.summary();
    expect(summary.rows_written == 2, "summary row count is exact");
    expect(summary.first_recording_frame_id == 1, "summary first frame id");
    expect(summary.last_recording_frame_id == 3, "summary last frame id");
    expect(summary.recording_frame_id_gaps == 1, "summary gap count");
    expect(summary.zero_camera_timestamp_rows == 0, "camera timestamps are populated");
    expect(summary.zero_system_timestamp_rows == 0, "system timestamps are populated");
    expect(
        orange::external_recorder::ValidateAuthoritativeFrameMetadata(
            summary, 2, 2, &error),
        "terminal validation permits an intentional recording-frame gap: " + error);

    std::ifstream input(path);
    std::string header;
    std::getline(input, header);
    expect(header.rfind("frame_id,timestamp,timestamp_sys", 0) == 0,
           "CSV preserves the legacy Cam*_meta.csv columns first");
    expect(header.find("recording_frame_id") != std::string::npos,
           "CSV exposes recording frame identity");
    expect(header.find("timestamp,timestamp_sys") != std::string::npos,
           "CSV exposes both timestamp clocks");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_terminal_validation_requires_packet_and_clock_parity()
{
    orange::external_recorder::FrameMetadataSummary summary;
    summary.path = "/tmp/Cam2010096_external_meta.csv";
    summary.rows_written = 2;
    summary.first_recording_frame_id = 1;
    summary.last_recording_frame_id = 2;

    std::string error;
    expect(
        orange::external_recorder::ValidateAuthoritativeFrameMetadata(
            summary, 2, 2, &error),
        "clean summary passes terminal validation: " + error);
    expect(
        !orange::external_recorder::ValidateAuthoritativeFrameMetadata(
            summary, 2, 1, &error),
        "packet/frame mismatch fails terminal validation");

    summary.zero_camera_timestamp_rows = 1;
    expect(
        !orange::external_recorder::ValidateAuthoritativeFrameMetadata(
            summary, 2, 2, &error),
        "zero camera timestamp fails terminal validation");
    summary.zero_camera_timestamp_rows = 0;
    summary.zero_system_timestamp_rows = 1;
    expect(
        !orange::external_recorder::ValidateAuthoritativeFrameMetadata(
            summary, 2, 2, &error),
        "zero system timestamp fails terminal validation");
}

void test_path_derivation()
{
    expect(
        orange::external_recorder::DeriveFrameMetadataPath(
            "/tmp/session/Cam2010096_external.mp4") ==
            "/tmp/session/Cam2010096_external_meta.csv",
        "metadata path derives from the authoritative MP4 stem");
}

}  // namespace

int main()
{
    test_writer_tracks_rows_and_gaps();
    test_terminal_validation_requires_packet_and_clock_parity();
    test_path_derivation();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "external recorder frame metadata tests passed\n";
    return 0;
}
