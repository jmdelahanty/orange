#include "spatial_roi_registered_scene_context.h"

#include "gui/spatial_layout/sha256.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using orange::session::spatial_roi::RegisteredSceneContextDescriptor;
using orange::session::spatial_roi::RegisteredSceneContextPublication;
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
        std::string pattern = "/tmp/orange_registered_scene_context_XXXXXX";
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

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

RegisteredSceneContextDescriptor descriptor_fixture()
{
    RegisteredSceneContextDescriptor descriptor;
    descriptor.recording_id = "recording-20260902T120000Z";
    descriptor.session_id = "session-1";
    descriptor.recording_identity_token = "sha256:" + std::string(64, 'a');
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 3;
    descriptor.camera_serial = "2010096";
    descriptor.source_camera_stream_id = "2010096";
    descriptor.stream_epoch_id = "stream_epoch_1";
    descriptor.camera_configuration_sha256 = "sha256:" + std::string(64, 'b');
    descriptor.source_frame = {91, 91, 7001, 0, 123456789, 987654321};
    descriptor.native_raster = {4, 3, 4};
    descriptor.pixel_format = "Mono8";
    descriptor.layout = {"layout_1", "sha256:" + std::string(64, 'c')};
    descriptor.materialization = {
        "materialization_1", "sha256:" + std::string(64, 'd')};
    descriptor.registration = {
        "registration_1", "sha256:" + std::string(64, 'e')};
    descriptor.invariants = {
        true,
        "accepted_for_experiment",
        true,
        "unknown",
        true,
        true,
        true};
    descriptor.artifact.relative_path = "registered_scene_context_2010096.mono8";
    return descriptor;
}

std::unique_ptr<SpatialRoiSessionAuthorityStore> open_authority(
    const fs::path& root)
{
    std::unique_ptr<SpatialRoiSessionAuthorityStore> authority;
    std::string error;
    require(SpatialRoiSessionAuthorityStore::OpenExisting(
                root,
                {"registered_scene_context_2010096.mono8",
                 "registered_scene_context_2010096.json"},
                &authority,
                &error),
            "open authority failed: " + error);
    return authority;
}

void test_descriptor_round_trip_and_closed_schema()
{
    RegisteredSceneContextDescriptor source = descriptor_fixture();
    const std::string bytes("abcdefghijkl", 12);
    source.artifact.size_bytes = bytes.size();
    source.artifact.sha256 =
        "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(bytes);

    std::string error;
    require(orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                source, &error),
            "valid descriptor rejected: " + error);
    const nlohmann::json wire =
        orange::session::spatial_roi::registered_scene_context_descriptor_to_json(source);
    RegisteredSceneContextDescriptor parsed;
    require(orange::session::spatial_roi::registered_scene_context_descriptor_from_json(
                wire, &parsed, &error),
            "descriptor parse failed: " + error);
    require(parsed.source_frame.camera_frame_id == 7001 &&
                parsed.native_raster.stride_bytes == 4 &&
                parsed.artifact.sha256 == source.artifact.sha256,
            "descriptor fields did not survive round trip");
    require(orange::session::spatial_roi::registered_scene_context_descriptor_to_json(
                parsed) == wire,
            "descriptor JSON round trip changed bytes");

    nlohmann::json unknown = wire;
    unknown["future_field"] = true;
    require(!orange::session::spatial_roi::registered_scene_context_descriptor_from_json(
                unknown, &parsed, &error),
            "unknown top-level field was accepted");
    unknown = wire;
    unknown["capture_role"] = "recording_start_context";
    require(!orange::session::spatial_roi::registered_scene_context_descriptor_from_json(
                unknown, &parsed, &error),
            "wrong capture role was accepted");

}

void test_invariants_and_receipt_validation()
{
    RegisteredSceneContextDescriptor descriptor = descriptor_fixture();
    descriptor.artifact.size_bytes = 12;
    descriptor.artifact.sha256 = "sha256:" + std::string(64, 'f');
    std::string error;
    require(orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "fixture descriptor should validate: " + error);

    descriptor.camera_id = 0;
    descriptor.invariants.subject_presence = "present";
    require(orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "camera index zero or present subject was rejected: " + error);

    descriptor.invariants.registration_authority_status =
        "diagnostic_not_physical_acceptance";
    descriptor.invariants.daily_registration_accepted = false;
    require(orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "truthful diagnostic registration status was rejected: " + error);
    descriptor.invariants.daily_registration_accepted = true;
    require(!orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "diagnostic authority was allowed to claim daily acceptance");
    descriptor.invariants.registration_authority_status =
        "accepted_for_experiment";
    descriptor.invariants.daily_registration_accepted = true;

    descriptor.invariants.rig_fixed = false;
    require(!orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "moving rig invariant was accepted");
    descriptor = descriptor_fixture();
    descriptor.artifact.size_bytes = 12;
    descriptor.artifact.sha256 = "sha256:" + std::string(64, 'f');
    descriptor.invariants.subject_presence = "not_declared";
    require(!orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "unknown subject-presence enum was accepted");
    descriptor.invariants.subject_presence = "unknown";
    descriptor.pixel_format = "mono8";
    require(!orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "lowercase pixel format was accepted");
    descriptor = descriptor_fixture();
    descriptor.artifact.size_bytes = 11;
    descriptor.artifact.sha256 = "sha256:" + std::string(64, 'f');
    require(!orange::session::spatial_roi::validate_registered_scene_context_descriptor(
                descriptor, &error),
            "receipt size not matching raster was accepted");
}

void test_descriptor_authoritative_publish_and_readback()
{
    TempTree tree;
    const fs::path recording_root = tree.path() / "recording";
    require(fs::create_directory(recording_root), "failed to create root");
    auto authority = open_authority(recording_root);
    const std::string bytes("abcdefghijkl", 12);
    RegisteredSceneContextDescriptor input = descriptor_fixture();
    RegisteredSceneContextPublication publication;
    std::string error;
    require(orange::session::spatial_roi::publish_registered_scene_context(
                *authority,
                "registered_scene_context_2010096.json",
                input,
                bytes,
                &publication,
                &error),
            "context publication failed: " + error);
    require(publication.descriptor.artifact.size_bytes == bytes.size() &&
                publication.descriptor.artifact.sha256 ==
                    "sha256:" +
                        orange::gui::spatial_layout::checksum::sha256_hex(bytes),
            "published descriptor did not retain exact image receipt");
    require(publication.descriptor_receipt.relative_path ==
                "registered_scene_context_2010096.json",
            "descriptor receipt path is wrong");

    std::string read_back;
    require(orange::session::spatial_roi::read_registered_scene_context_bytes(
                *authority, publication.descriptor, &read_back, &error),
            "context readback failed: " + error);
    require(read_back == bytes, "context readback changed exact Mono8 bytes");

    RegisteredSceneContextPublication retry;
    require(orange::session::spatial_roi::publish_registered_scene_context(
                *authority,
                "registered_scene_context_2010096.json",
                input,
                bytes,
                &retry,
                &error),
            "identical context publication retry failed: " + error);
    require(retry.descriptor_receipt == publication.descriptor_receipt,
            "identical context retry changed descriptor receipt");

    require(!orange::session::spatial_roi::publish_registered_scene_context(
                *authority,
                "registered_scene_context_2010096.json",
                input,
                std::string("changedbytes", 12),
                &retry,
                &error),
            "changed image bytes were accepted on retry");
}

}  // namespace

int main()
{
    try {
        test_descriptor_round_trip_and_closed_schema();
        test_invariants_and_receipt_validation();
        test_descriptor_authoritative_publish_and_readback();
    } catch (const std::exception& exception) {
        std::cerr << "registered scene context tests failed: " << exception.what()
                  << '\n';
        return 1;
    }
    std::cout << "registered scene context tests passed\n";
    return 0;
}
