#include "session/acquisition_index_authority.h"

#include "gui/spatial_layout/sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

using json = nlohmann::json;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string semantic_sha256(const json& value)
{
    return "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(
        value.dump(-1, ' ', false, json::error_handler_t::strict));
}

std::string artifact_sha256(const std::string& bytes)
{
    return "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path manifest_path;
    std::filesystem::path metadata_path;
    std::string metadata_bytes;
    json manifest;

    Fixture()
    {
        root = std::filesystem::temp_directory_path() /
            ("orange_acquisition_authority_" +
             std::to_string(static_cast<long long>(::getpid())));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "external_recorder");
        manifest_path = root / "recording_session.json";
        metadata_path = root / "external_recorder" / "Cam2010096_external_meta.csv";
        metadata_bytes =
            "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id\n"
            "1,1001,2001,1,91\n"
            "2,1002,2002,2,92\n"
            "3,1003,2003,3,93\n";
        write_metadata(metadata_bytes);

        json frame_identity = {
            {"schema_id", "orange.recording.frame_identity"},
            {"schema_version", 1},
            {"status", "finalized"},
            {"canonical_field", "recording_frame_id"},
            {"scope", "recording_session_and_camera_stream"},
            {"assignment_event", "orange_acquisition_recording_frame_sequence"},
            {"legacy_aliases", {{"frame_id", "recording_frame_id"}}},
            {"timestamp_fields", {"timestamp", "timestamp_sys"}},
            {"continuity_policy", "encoded_subset"},
            {"recording_frame_id_gaps_allowed", true},
            {"camera_streams", {
                {"2010096", {
                    {"camera_serial", "2010096"},
                    {"backend", "external_ipc"},
                    {"verification_status", "passed"},
                }},
            }},
            {"verification", {{"required", true}, {"result", "passed"}}},
        };
        json stream = {
            {"camera_serial", "2010096"},
            {"coverage", {
                {"first_recording_frame_id", 1},
                {"last_recording_frame_id", 3},
                {"total_acquisitions", 3},
                {"metadata_row_count", 3},
                {"gap_count", 0},
                {"gap_policy", "none"},
            }},
            {"source_metadata_artifact", {
                {"relative_path", "external_recorder/Cam2010096_external_meta.csv"},
                {"media_type", "text/csv"},
                {"sha256", artifact_sha256(metadata_bytes)},
            }},
            {"producer_identity", {
                {"field", "recording_frame_id"},
                {"dtype", "uint64"},
                {"index_base", 1},
                {"scope", "recording_session_and_camera_stream"},
                {"assignment_event", "orange_acquisition_recording_frame_sequence"},
            }},
            {"destination_identity", {
                {"field", "source_acquisition_frame_index"},
                {"dtype", "int64"},
                {"index_base", 0},
            }},
            {"conversion", {
                {"method", "subtract_constant_v1"},
                {"expression", "source_acquisition_frame_index = recording_frame_id - 1"},
                {"constant", 1},
            }},
        };
        json mapping = {
            {"schema_id", "orange.recording.acquisition_index_mapping"},
            {"schema_version", 1},
            {"status", "finalized"},
            {"recording_id", "recording_123"},
            {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
            {"frame_identity_contract_ref", "#/frame_identity_contract"},
            {"frame_identity_contract_sha256", semantic_sha256(frame_identity)},
            {"camera_streams", {{"2010096", stream}}},
        };
        manifest = {
            {"schema_id", "orange.recording_session"},
            {"schema_version", 1},
            {"session_id", "recording_123"},
            {"status", "completed"},
            {"cameras", {"2010096"}},
            {"frame_identity_contract", frame_identity},
            {"acquisition_index_mapping", mapping},
            {"acquisition_index_mapping_sha256", semantic_sha256(mapping)},
        };
    }

    ~Fixture()
    {
        std::filesystem::remove_all(root);
    }

    void write_metadata(const std::string& bytes)
    {
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "could not write metadata fixture");
        output << bytes;
        require(static_cast<bool>(output), "metadata fixture write failed");
    }

    void reseal_mapping()
    {
        manifest["acquisition_index_mapping_sha256"] =
            semantic_sha256(manifest.at("acquisition_index_mapping"));
    }

    void reseal_identity_and_mapping()
    {
        manifest["acquisition_index_mapping"]["frame_identity_contract_sha256"] =
            semantic_sha256(manifest.at("frame_identity_contract"));
        reseal_mapping();
    }
};

void test_resolves_dense_authority_and_converts_ids()
{
    Fixture fixture;
    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    require(
        orange::session::resolve_acquisition_index_authority(
            fixture.manifest,
            fixture.manifest_path,
            "2010096",
            &authority,
            &error),
        "valid authority should resolve: " + error);
    require(authority.recording_id == "recording_123" &&
                authority.camera_serial == "2010096" &&
                authority.total_acquisitions == 3 &&
                authority.source_metadata_path ==
                    std::filesystem::weakly_canonical(fixture.metadata_path),
            "resolved authority fields are incomplete");

    std::int64_t index = -1;
    require(
        authority.recording_frame_id_to_source_acquisition_index(1, &index, &error) &&
            index == 0,
        "recording frame 1 should map to acquisition index 0");
    require(
        authority.recording_frame_id_to_source_acquisition_index(3, &index, &error) &&
            index == 2,
        "recording frame 3 should map to acquisition index 2");
    require(
        !authority.recording_frame_id_to_source_acquisition_index(0, &index, &error) &&
            !authority.recording_frame_id_to_source_acquisition_index(4, &index, &error),
        "out-of-domain recording frame ids must fail closed");
}

void test_rejects_stale_semantic_and_artifact_digests()
{
    Fixture fixture;
    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    fixture.manifest["acquisition_index_mapping_sha256"] =
        "sha256:" + std::string(64, '0');
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest, fixture.manifest_path, "2010096", &authority, &error),
        "stale mapping digest must fail");

    fixture.reseal_mapping();
    fixture.write_metadata(fixture.metadata_bytes + "4,1004,2004,4,94\n");
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest, fixture.manifest_path, "2010096", &authority, &error),
        "post-seal metadata mutation must fail");
}

void test_rejects_alias_mismatch_even_when_resealed()
{
    Fixture fixture;
    const std::string mismatched =
        "frame_id,timestamp,timestamp_sys,recording_frame_id,local_frame_id\n"
        "1,1001,2001,1,91\n"
        "2,1002,2002,3,92\n"
        "3,1003,2003,3,93\n";
    fixture.write_metadata(mismatched);
    fixture.manifest["acquisition_index_mapping"]["camera_streams"]["2010096"]
        ["source_metadata_artifact"]["sha256"] = artifact_sha256(mismatched);
    fixture.reseal_mapping();

    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest, fixture.manifest_path, "2010096", &authority, &error) &&
            error.find("aliases") != std::string::npos,
        "a checksum-valid frame_id/recording_frame_id contradiction must fail");
}

void test_rejects_unlisted_camera_and_open_records()
{
    Fixture fixture;
    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest, fixture.manifest_path, "2010095", &authority, &error),
        "an unlisted camera must fail");

    fixture.manifest["acquisition_index_mapping"]["unexpected"] = true;
    fixture.reseal_mapping();
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest, fixture.manifest_path, "2010096", &authority, &error),
        "an open-ended mapping record must fail");
}

void test_rejects_unverified_external_frame_authority()
{
    Fixture fixture;
    fixture.manifest["frame_identity_contract"]["camera_streams"]["2010096"]
        ["verification_status"] = "legacy_unproven";
    fixture.manifest["frame_identity_contract"]["verification"]["result"] =
        "legacy_unproven";
    fixture.reseal_identity_and_mapping();

    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    require(
        !orange::session::resolve_acquisition_index_authority(
            fixture.manifest,
            fixture.manifest_path,
            "2010096",
            &authority,
            &error) &&
            error.find("not verified") != std::string::npos,
        "a legacy external row sequence must not substitute for returned-frame proof");
}

orange::session::AcquisitionIndexAuthority resolve_fixture_authority(Fixture* fixture)
{
    orange::session::AcquisitionIndexAuthority authority;
    std::string error;
    require(
        fixture && orange::session::resolve_acquisition_index_authority(
            fixture->manifest,
            fixture->manifest_path,
            "2010096",
            &authority,
            &error),
        "fixture authority should resolve: " + error);
    return authority;
}

void test_builds_palette_exact_source_mapping_record()
{
    Fixture fixture;
    const orange::session::AcquisitionIndexAuthority authority =
        resolve_fixture_authority(&fixture);
    orange::session::PaletteSourceAcquisitionMapping mapping;
    std::string error;
    require(
        orange::session::build_palette_source_acquisition_mapping(
            authority,
            {1, 3},
            std::string(64, 'a'),
            std::string(64, 'b'),
            &mapping,
            &error),
        "Palette source mapping should build: " + error);
    require(
        mapping.source_acquisition_frame_index == std::vector<std::int64_t>({0, 2}),
        "Shaman-v2 recording IDs should become exact zero-based indices");
    require(
        mapping.array_content_sha256 ==
            "e7d8446107d69a20fc8f0ece8ee1296874ef37635c1c5ed504c78922741c340b",
        "C++ array digest must match Palette/NumPy canonical bytes");
    require(
        mapping.mapping_record_sha256 ==
            "4d763b38cd74a6996e5f1a3a55306438c75f35307a5feb81758e38289c63e069",
        "C++ record digest must match Palette canonical JSON");
    require(
        mapping.mapping_record.at("schema_id") ==
            "citrus.stimulus_source_acquisition_mapping" &&
        mapping.mapping_record.at("array_dtype") == "<i8" &&
        mapping.mapping_record.at("array_shape") == json::array({2}) &&
        mapping.mapping_record.at("source_total_frames") == 3,
        "Palette source mapping record does not match closed schema v1");
}

void test_palette_source_mapping_rejects_unsealed_or_out_of_range_inputs()
{
    Fixture fixture;
    orange::session::AcquisitionIndexAuthority authority =
        resolve_fixture_authority(&fixture);
    orange::session::PaletteSourceAcquisitionMapping mapping;
    std::string error;
    require(
        !orange::session::build_palette_source_acquisition_mapping(
            authority,
            {},
            std::string(64, 'a'),
            std::string(64, 'b'),
            &mapping,
            &error),
        "empty canonical chaser rowsets must be omitted rather than mapped");
    require(
        !orange::session::build_palette_source_acquisition_mapping(
            authority,
            {1, 4},
            std::string(64, 'a'),
            std::string(64, 'b'),
            &mapping,
            &error),
        "out-of-domain Shaman-v2 identities must fail");
    require(
        !orange::session::build_palette_source_acquisition_mapping(
            authority,
            {1},
            "sha256:" + std::string(64, 'a'),
            std::string(64, 'b'),
            &mapping,
            &error),
        "Palette digests must use the exact bare digest representation");

    authority.acquisition_mapping_sha256.clear();
    require(
        !orange::session::build_palette_source_acquisition_mapping(
            authority,
            {1},
            std::string(64, 'a'),
            std::string(64, 'b'),
            &mapping,
            &error),
        "an unsealed Orange authority must not generate Palette records");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase tests[] = {
        {"resolves_dense_authority_and_converts_ids",
         test_resolves_dense_authority_and_converts_ids},
        {"rejects_stale_semantic_and_artifact_digests",
         test_rejects_stale_semantic_and_artifact_digests},
        {"rejects_alias_mismatch_even_when_resealed",
         test_rejects_alias_mismatch_even_when_resealed},
        {"rejects_unlisted_camera_and_open_records",
         test_rejects_unlisted_camera_and_open_records},
        {"rejects_unverified_external_frame_authority",
         test_rejects_unverified_external_frame_authority},
        {"builds_palette_exact_source_mapping_record",
         test_builds_palette_exact_source_mapping_record},
        {"palette_source_mapping_rejects_unsealed_or_out_of_range_inputs",
         test_palette_source_mapping_rejects_unsealed_or_out_of_range_inputs},
    };
    int failures = 0;
    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
