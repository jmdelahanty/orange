#include "session/acquisition_index_authority.h"

#include "gui/spatial_layout/sha256.h"

#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace orange::session {

namespace {

using json = nlohmann::json;

constexpr const char* kMappingSchemaId =
    "orange.recording.acquisition_index_mapping";
constexpr int kMappingSchemaVersion = 1;
constexpr const char* kFrameIdentitySchemaId =
    "orange.recording.frame_identity";
constexpr int kFrameIdentitySchemaVersion = 1;
constexpr const char* kCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::string canonical_json_sha256(const json& value)
{
    return "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(
        value.dump(-1, ' ', false, json::error_handler_t::strict));
}

bool has_exact_fields(const json& value, const std::set<std::string>& expected)
{
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (expected.count(item.key()) == 0) {
            return false;
        }
    }
    return true;
}

bool get_uint64(const json& value, const char* field, std::uint64_t* out)
{
    if (!out || !value.contains(field)) {
        return false;
    }
    const json& item = value.at(field);
    if (item.is_number_unsigned()) {
        *out = item.get<std::uint64_t>();
        return true;
    }
    if (item.is_number_integer()) {
        const std::int64_t signed_value = item.get<std::int64_t>();
        if (signed_value >= 0) {
            *out = static_cast<std::uint64_t>(signed_value);
            return true;
        }
    }
    return false;
}

bool is_sha256(const std::string& value)
{
    if (value.size() != 71 || value.rfind("sha256:", 0) != 0) {
        return false;
    }
    for (std::size_t index = 7; index < value.size(); ++index) {
        const char ch = value[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_bare_sha256(const std::string& value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

void append_little_endian_int64(std::string* bytes, const std::int64_t value)
{
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (unsigned int offset = 0; offset < 64; offset += 8) {
        bytes->push_back(static_cast<char>((bits >> offset) & 0xffu));
    }
}

std::string palette_int64_array_sha256(const std::vector<std::int64_t>& values)
{
    const json header = {
        {"canonicalization", "numpy_dtype_shape_c_order_bytes_v1"},
        {"dtype", "<i8"},
        {"shape", {values.size()}},
    };
    std::string bytes = header.dump(
        -1, ' ', false, json::error_handler_t::strict);
    bytes.push_back('\0');
    bytes.reserve(bytes.size() + values.size() * sizeof(std::int64_t));
    for (const std::int64_t value : values) {
        append_little_endian_int64(&bytes, value);
    }
    return orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

bool is_safe_recording_relative_path(const std::filesystem::path& value)
{
    if (value.empty() || value.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& component : value) {
        if (component == ".." || component == ".") {
            return false;
        }
    }
    return true;
}

bool camera_is_listed_once(const json& manifest, const std::string& camera_serial)
{
    const auto cameras = manifest.find("cameras");
    if (cameras == manifest.end() || !cameras->is_array()) {
        return false;
    }
    std::size_t matches = 0;
    for (const json& camera : *cameras) {
        if (camera.is_string() && camera.get<std::string>() == camera_serial) {
            ++matches;
        }
    }
    return matches == 1;
}

std::vector<std::string> split_csv_row(const std::string& row)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= row.size()) {
        const std::size_t comma = row.find(',', begin);
        if (comma == std::string::npos) {
            fields.push_back(row.substr(begin));
            break;
        }
        fields.push_back(row.substr(begin, comma - begin));
        begin = comma + 1;
    }
    return fields;
}

bool parse_strict_positive_uint64(const std::string& text, std::uint64_t* value_out)
{
    if (!value_out || text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        return false;
    }
    *value_out = value;
    return true;
}

bool validate_metadata_identity_rows(
    const std::string& bytes,
    const std::uint64_t expected_rows,
    std::string* error_out)
{
    std::istringstream input(bytes);
    std::string header_text;
    if (!std::getline(input, header_text)) {
        return fail(error_out, "acquisition metadata CSV is missing its header");
    }
    if (!header_text.empty() && header_text.back() == '\r') {
        header_text.pop_back();
    }
    const std::vector<std::string> header = split_csv_row(header_text);
    std::size_t frame_id_column = header.size();
    std::size_t recording_frame_id_column = header.size();
    std::set<std::string> unique_fields;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (header[index].empty() || !unique_fields.insert(header[index]).second) {
            return fail(error_out, "acquisition metadata CSV header is invalid");
        }
        if (header[index] == "frame_id") {
            frame_id_column = index;
        } else if (header[index] == "recording_frame_id") {
            recording_frame_id_column = index;
        }
    }
    if (frame_id_column == header.size() ||
        recording_frame_id_column == header.size()) {
        return fail(
            error_out,
            "acquisition metadata CSV lacks frame_id or recording_frame_id");
    }

    std::uint64_t row_count = 0;
    std::string row_text;
    while (std::getline(input, row_text)) {
        if (!row_text.empty() && row_text.back() == '\r') {
            row_text.pop_back();
        }
        if (row_text.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_csv_row(row_text);
        if (fields.size() != header.size()) {
            return fail(error_out, "acquisition metadata CSV row width is invalid");
        }
        std::uint64_t frame_id = 0;
        std::uint64_t recording_frame_id = 0;
        if (!parse_strict_positive_uint64(fields[frame_id_column], &frame_id) ||
            !parse_strict_positive_uint64(
                fields[recording_frame_id_column], &recording_frame_id)) {
            return fail(error_out, "acquisition metadata CSV frame identity is invalid");
        }
        ++row_count;
        if (frame_id != recording_frame_id || recording_frame_id != row_count) {
            return fail(
                error_out,
                "acquisition metadata CSV frame aliases are not identical and dense");
        }
    }
    if (row_count != expected_rows) {
        return fail(error_out, "acquisition metadata CSV row count is inconsistent");
    }
    return true;
}

}  // namespace

bool AcquisitionIndexAuthority::recording_frame_id_to_source_acquisition_index(
    const std::uint64_t recording_frame_id,
    std::int64_t* acquisition_index_out,
    std::string* error_out) const
{
    if (!acquisition_index_out) {
        return fail(error_out, "acquisition index output is null");
    }
    if (total_acquisitions == 0 ||
        first_recording_frame_id != 1 ||
        last_recording_frame_id != total_acquisitions) {
        return fail(error_out, "acquisition index authority is not a dense sealed domain");
    }
    if (recording_frame_id == 0 || recording_frame_id > total_acquisitions) {
        return fail(
            error_out,
            "recording_frame_id lies outside the sealed recording domain");
    }
    const std::uint64_t zero_based = recording_frame_id - 1;
    if (zero_based >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(error_out, "source acquisition index exceeds signed int64");
    }
    *acquisition_index_out = static_cast<std::int64_t>(zero_based);
    return true;
}

bool resolve_acquisition_index_authority(
    const json& manifest,
    const std::filesystem::path& manifest_path,
    const std::string& camera_serial,
    AcquisitionIndexAuthority* authority_out,
    std::string* error_out)
{
    if (!authority_out) {
        return fail(error_out, "acquisition authority output is null");
    }
    *authority_out = {};
    if (!manifest.is_object()) {
        return fail(error_out, "recording session manifest must be an object");
    }
    if (manifest.value("schema_id", std::string()) != "orange.recording_session" ||
        manifest.value("schema_version", 0) != 1 ||
        manifest.value("status", std::string()) != "completed") {
        return fail(error_out, "recording session is not a completed Orange v1 manifest");
    }
    const std::string recording_id = manifest.value("session_id", std::string());
    if (recording_id.empty()) {
        return fail(error_out, "recording session is missing session_id");
    }
    if (camera_serial.empty() || !camera_is_listed_once(manifest, camera_serial)) {
        return fail(error_out, "camera is not listed exactly once in the recording session");
    }

    const json frame_identity = manifest.value(
        "frame_identity_contract", json::object());
    if (!frame_identity.is_object() ||
        frame_identity.value("schema_id", std::string()) != kFrameIdentitySchemaId ||
        frame_identity.value("schema_version", 0) != kFrameIdentitySchemaVersion ||
        frame_identity.value("status", std::string()) != "finalized" ||
        frame_identity.value("canonical_field", std::string()) !=
            "recording_frame_id" ||
        frame_identity.value("scope", std::string()) !=
            "recording_session_and_camera_stream" ||
        frame_identity.value("assignment_event", std::string()) !=
            "orange_acquisition_recording_frame_sequence" ||
        frame_identity.value("legacy_aliases", json::object()) !=
            json{{"frame_id", "recording_frame_id"}}) {
        return fail(error_out, "recording frame identity contract is not finalized");
    }
    const json identity_streams = frame_identity.value("camera_streams", json::object());
    if (!identity_streams.is_object() ||
        !identity_streams.contains(camera_serial) ||
        !identity_streams.at(camera_serial).is_object() ||
        identity_streams.at(camera_serial).value("camera_serial", std::string()) !=
            camera_serial) {
        return fail(error_out, "recording frame identity does not bind the selected camera");
    }
    const json& identity_stream = identity_streams.at(camera_serial);
    const std::string identity_backend =
        identity_stream.value("backend", std::string());
    const std::string verification_status =
        identity_stream.value("verification_status", std::string());
    const bool verified_external =
        identity_backend == "external_ipc" && verification_status == "passed";
    const bool declared_in_process =
        identity_backend == "in_process" &&
        verification_status == "producer_declared";
    if (!verified_external && !declared_in_process) {
        return fail(
            error_out,
            "recording frame identity camera authority is not verified");
    }

    const json mapping = manifest.value("acquisition_index_mapping", json::object());
    const std::set<std::string> mapping_fields = {
        "schema_id",
        "schema_version",
        "status",
        "recording_id",
        "canonicalization",
        "frame_identity_contract_ref",
        "frame_identity_contract_sha256",
        "camera_streams",
    };
    if (!has_exact_fields(mapping, mapping_fields) ||
        mapping.value("schema_id", std::string()) != kMappingSchemaId ||
        mapping.value("schema_version", 0) != kMappingSchemaVersion ||
        mapping.value("status", std::string()) != "finalized" ||
        mapping.value("recording_id", std::string()) != recording_id ||
        mapping.value("canonicalization", std::string()) != kCanonicalization ||
        mapping.value("frame_identity_contract_ref", std::string()) !=
            "#/frame_identity_contract") {
        return fail(error_out, "acquisition index mapping is not the closed finalized v1 record");
    }

    const std::string frame_identity_digest =
        mapping.value("frame_identity_contract_sha256", std::string());
    if (!is_sha256(frame_identity_digest) ||
        frame_identity_digest != canonical_json_sha256(frame_identity)) {
        return fail(error_out, "acquisition mapping frame-identity digest is invalid");
    }
    const std::string mapping_digest =
        manifest.value("acquisition_index_mapping_sha256", std::string());
    if (!is_sha256(mapping_digest) || mapping_digest != canonical_json_sha256(mapping)) {
        return fail(error_out, "acquisition index mapping digest is invalid");
    }

    const json streams = mapping.value("camera_streams", json::object());
    if (!streams.is_object() || !streams.contains(camera_serial) ||
        !streams.at(camera_serial).is_object()) {
        return fail(error_out, "acquisition mapping has no selected camera stream");
    }
    const json& stream = streams.at(camera_serial);
    const std::set<std::string> stream_fields = {
        "camera_serial",
        "coverage",
        "source_metadata_artifact",
        "producer_identity",
        "destination_identity",
        "conversion",
    };
    if (!has_exact_fields(stream, stream_fields) ||
        stream.value("camera_serial", std::string()) != camera_serial) {
        return fail(error_out, "selected acquisition stream is not the closed v1 record");
    }

    const json expected_producer_identity = {
        {"field", "recording_frame_id"},
        {"dtype", "uint64"},
        {"index_base", 1},
        {"scope", "recording_session_and_camera_stream"},
        {"assignment_event", "orange_acquisition_recording_frame_sequence"},
    };
    const json expected_destination_identity = {
        {"field", "source_acquisition_frame_index"},
        {"dtype", "int64"},
        {"index_base", 0},
    };
    const json expected_conversion = {
        {"method", "subtract_constant_v1"},
        {"expression", "source_acquisition_frame_index = recording_frame_id - 1"},
        {"constant", 1},
    };
    if (stream.at("producer_identity") != expected_producer_identity ||
        stream.at("destination_identity") != expected_destination_identity ||
        stream.at("conversion") != expected_conversion) {
        return fail(error_out, "selected acquisition stream identity conversion is invalid");
    }

    const json& coverage = stream.at("coverage");
    const std::set<std::string> coverage_fields = {
        "first_recording_frame_id",
        "last_recording_frame_id",
        "total_acquisitions",
        "metadata_row_count",
        "gap_count",
        "gap_policy",
    };
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t total = 0;
    std::uint64_t rows = 0;
    std::uint64_t gaps = 0;
    if (!has_exact_fields(coverage, coverage_fields) ||
        !get_uint64(coverage, "first_recording_frame_id", &first) ||
        !get_uint64(coverage, "last_recording_frame_id", &last) ||
        !get_uint64(coverage, "total_acquisitions", &total) ||
        !get_uint64(coverage, "metadata_row_count", &rows) ||
        !get_uint64(coverage, "gap_count", &gaps) ||
        coverage.value("gap_policy", std::string()) != "none" ||
        first != 1 || total == 0 || last != total || rows != total || gaps != 0 ||
        total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(error_out, "selected acquisition stream is not dense and gap-free");
    }

    const json& source_artifact = stream.at("source_metadata_artifact");
    const std::set<std::string> artifact_fields = {
        "relative_path", "media_type", "sha256"};
    const std::string relative_text =
        source_artifact.value("relative_path", std::string());
    const std::string artifact_digest =
        source_artifact.value("sha256", std::string());
    const std::filesystem::path relative_path(relative_text);
    if (!has_exact_fields(source_artifact, artifact_fields) ||
        source_artifact.value("media_type", std::string()) != "text/csv" ||
        !is_safe_recording_relative_path(relative_path) ||
        !is_sha256(artifact_digest)) {
        return fail(error_out, "selected acquisition metadata artifact record is invalid");
    }

    std::error_code path_error;
    const std::filesystem::path manifest_folder =
        std::filesystem::weakly_canonical(manifest_path.parent_path(), path_error);
    if (path_error) {
        return fail(error_out, "could not canonicalize recording session folder");
    }
    const std::filesystem::path source_path =
        std::filesystem::weakly_canonical(manifest_folder / relative_path, path_error);
    if (path_error || source_path.lexically_relative(manifest_folder).empty()) {
        return fail(error_out, "could not resolve acquisition metadata artifact");
    }
    const std::filesystem::path resolved_relative =
        source_path.lexically_relative(manifest_folder);
    if (resolved_relative.is_absolute()) {
        return fail(error_out, "acquisition metadata artifact escapes recording folder");
    }
    for (const std::filesystem::path& component : resolved_relative) {
        if (component == "..") {
            return fail(error_out, "acquisition metadata artifact escapes recording folder");
        }
    }
    std::string source_bytes;
    std::string source_error;
    if (!orange::gui::spatial_layout::checksum::read_file(
            source_path, &source_bytes, &source_error)) {
        return fail(error_out, "could not read acquisition metadata artifact");
    }
    if ("sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(source_bytes) !=
        artifact_digest) {
        return fail(error_out, "acquisition metadata artifact checksum is stale");
    }
    std::string metadata_identity_error;
    if (!validate_metadata_identity_rows(
            source_bytes, total, &metadata_identity_error)) {
        return fail(error_out, metadata_identity_error);
    }

    authority_out->recording_id = recording_id;
    authority_out->camera_serial = camera_serial;
    authority_out->total_acquisitions = total;
    authority_out->first_recording_frame_id = first;
    authority_out->last_recording_frame_id = last;
    authority_out->acquisition_mapping_sha256 = mapping_digest;
    authority_out->frame_identity_contract_sha256 = frame_identity_digest;
    authority_out->source_metadata_relative_path = relative_text;
    authority_out->source_metadata_path = source_path;
    authority_out->source_metadata_sha256 = artifact_digest;
    return true;
}

bool build_palette_source_acquisition_mapping(
    const AcquisitionIndexAuthority& authority,
    const std::vector<std::uint64_t>& recording_frame_ids,
    const std::string& source_row_identity_sha256,
    const std::string& source_row_identity_contract_sha256,
    PaletteSourceAcquisitionMapping* mapping_out,
    std::string* error_out)
{
    if (!mapping_out) {
        return fail(error_out, "Palette source acquisition mapping output is null");
    }
    *mapping_out = {};
    if (authority.recording_id.empty() || authority.camera_serial.empty() ||
        authority.total_acquisitions == 0 ||
        authority.first_recording_frame_id != 1 ||
        authority.last_recording_frame_id != authority.total_acquisitions ||
        !is_sha256(authority.acquisition_mapping_sha256) ||
        !is_sha256(authority.frame_identity_contract_sha256) ||
        !is_sha256(authority.source_metadata_sha256)) {
        return fail(error_out, "acquisition index authority is incomplete or unsealed");
    }
    if (recording_frame_ids.empty()) {
        return fail(error_out, "canonical chaser rowset must not be empty");
    }
    if (!is_bare_sha256(source_row_identity_sha256) ||
        !is_bare_sha256(source_row_identity_contract_sha256)) {
        return fail(error_out, "Palette row identity digests must be bare lowercase SHA-256");
    }

    std::vector<std::int64_t> indices;
    indices.reserve(recording_frame_ids.size());
    for (const std::uint64_t recording_frame_id : recording_frame_ids) {
        std::int64_t index = -1;
        std::string conversion_error;
        if (!authority.recording_frame_id_to_source_acquisition_index(
                recording_frame_id, &index, &conversion_error)) {
            return fail(
                error_out,
                "could not map Shaman-v2 recording_frame_id: " + conversion_error);
        }
        indices.push_back(index);
    }

    const std::string array_digest = palette_int64_array_sha256(indices);
    json record = {
        {"schema_id", "citrus.stimulus_source_acquisition_mapping"},
        {"schema_version", 1},
        {"mapping_method", "explicit_per_stimulus_state_v1"},
        {"source_rowset_ref", "/tracking_data/chaser_states"},
        {"source_row_identity_ref", "/tracking_data/stimulus_state_key"},
        {"source_row_identity_sha256", source_row_identity_sha256},
        {"source_row_identity_contract_sha256",
         source_row_identity_contract_sha256},
        {"acquisition_recording_id", authority.recording_id},
        {"acquisition_camera_id", authority.camera_serial},
        {"source_total_frames", authority.total_acquisitions},
        {"target_domain", "acquisition_frame_index"},
        {"array_ref", "/tracking_data/source_acquisition_frame_index"},
        {"array_dtype", "<i8"},
        {"array_shape", {indices.size()}},
        {"array_content_sha256", array_digest},
        {"canonicalization", "canonical_json_sort_keys_v1"},
    };

    mapping_out->source_acquisition_frame_index = std::move(indices);
    mapping_out->array_content_sha256 = array_digest;
    mapping_out->mapping_record = std::move(record);
    // Palette record digests are bare lowercase SHA-256 rather than the
    // prefixed Orange manifest representation.
    mapping_out->mapping_record_sha256 =
        orange::gui::spatial_layout::checksum::sha256_hex(
            mapping_out->mapping_record.dump(
                -1, ' ', false, json::error_handler_t::strict));
    return true;
}

}  // namespace orange::session
