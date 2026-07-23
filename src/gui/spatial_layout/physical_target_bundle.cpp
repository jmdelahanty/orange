#include "gui/spatial_layout/physical_target_bundle.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/session_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

constexpr const char* kDefaultPhysicalTargetId =
    "acrylic_hole_target_78mm_pitch5_margin3_v002";
constexpr const char* kDefaultPhysicalTargetJsonFilename =
    "acrylic_hole_target_78mm_pitch5_margin3_v002.json";
constexpr const char* kCameraOnlyPhysicalTargetCalibrationPurpose =
    "camera_only_physical_target_calibration";
constexpr const char* kDryPhysicalTargetHeightParallaxDiagnosticPurpose =
    "dry_physical_target_height_parallax_diagnostic";
constexpr const char* kProjectedSurfaceScaleCalibrationPurpose =
    "projected_surface_scale_calibration";

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

uint32_t rotr(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(const std::vector<unsigned char>& data)
{
    uint32_t h0 = 0x6a09e667u;
    uint32_t h1 = 0xbb67ae85u;
    uint32_t h2 = 0x3c6ef372u;
    uint32_t h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu;
    uint32_t h5 = 0x9b05688cu;
    uint32_t h6 = 0x1f83d9abu;
    uint32_t h7 = 0x5be0cd19u;

    std::vector<unsigned char> padded = data;
    const uint64_t bit_length = static_cast<uint64_t>(padded.size()) * 8u;
    padded.push_back(0x80u);
    while ((padded.size() % 64u) != 56u) {
        padded.push_back(0u);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<unsigned char>((bit_length >> shift) & 0xffu));
    }

    for (size_t chunk = 0; chunk < padded.size(); chunk += 64u) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t offset = chunk + i * 4u;
            w[i] = (static_cast<uint32_t>(padded[offset]) << 24) |
                   (static_cast<uint32_t>(padded[offset + 1]) << 16) |
                   (static_cast<uint32_t>(padded[offset + 2]) << 8) |
                   static_cast<uint32_t>(padded[offset + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        uint32_t f = h5;
        uint32_t g = h6;
        uint32_t h = h7;

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::nouppercase;
    for (uint32_t value : {h0, h1, h2, h3, h4, h5, h6, h7}) {
        oss << std::setw(8) << value;
    }
    return oss.str();
}

bool read_binary_file(
    const std::filesystem::path& path,
    std::vector<unsigned char>* data_out,
    std::string* error_out)
{
    if (data_out == nullptr) {
        if (error_out) {
            *error_out = "Null binary-file destination.";
        }
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        if (error_out) {
            *error_out = "Failed to open physical target file: " + path.generic_string();
        }
        return false;
    }
    data_out->assign(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error_out) {
            *error_out = "Failed while reading physical target file: " + path.generic_string();
        }
        return false;
    }
    return true;
}

bool file_sha256(
    const std::filesystem::path& path,
    std::string* sha256_out,
    std::string* error_out)
{
    if (sha256_out == nullptr) {
        if (error_out) {
            *error_out = "Null SHA-256 destination.";
        }
        return false;
    }
    std::vector<unsigned char> data;
    if (!read_binary_file(path, &data, error_out)) {
        return false;
    }
    *sha256_out = sha256_hex(data);
    return true;
}

std::vector<std::string> split_csv_line(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    for (char ch : line) {
        if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else if (ch != '\r') {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

bool parse_finite_double(const std::string& text, double* value_out)
{
    if (value_out == nullptr || text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    *value_out = value;
    return true;
}

bool validate_coordinate_csv(
    const std::filesystem::path& csv_path,
    int expected_point_count,
    std::string* error_out)
{
    std::ifstream in(csv_path, std::ios::in);
    if (!in.is_open()) {
        if (error_out) {
            *error_out = "Failed to open physical target coordinate CSV: " +
                         csv_path.generic_string();
        }
        return false;
    }

    std::string line;
    if (!std::getline(in, line)) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV is empty: " +
                         csv_path.generic_string();
        }
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const std::string expected_header =
        "point_id,x_mm,y_mm,marker_role,nominal_diameter_mm,include_in_fit";
    if (line != expected_header) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV header mismatch. Expected " +
                         expected_header + ", got " + line;
        }
        return false;
    }

    int row_count = 0;
    int fit_count = 0;
    std::set<std::string> ids;
    std::map<std::string, int> roles;
    bool has_c = false;
    bool has_xplus = false;
    double c_x = 0.0;
    double c_y = 0.0;
    double xplus_x = 0.0;
    double xplus_y = 0.0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_csv_line(line);
        if (fields.size() != 6u) {
            if (error_out) {
                *error_out = "Physical target coordinate CSV row has " +
                             std::to_string(fields.size()) +
                             " columns instead of 6: " + line;
            }
            return false;
        }
        const std::string& point_id = fields[0];
        if (point_id.empty() || ids.count(point_id) != 0u) {
            if (error_out) {
                *error_out = point_id.empty()
                                 ? "Physical target coordinate CSV has an empty point_id."
                                 : "Physical target coordinate CSV has duplicate point_id: " + point_id;
            }
            return false;
        }
        double x = 0.0;
        double y = 0.0;
        if (!parse_finite_double(fields[1], &x) ||
            !parse_finite_double(fields[2], &y)) {
            if (error_out) {
                *error_out = "Physical target coordinate CSV has non-finite x_mm/y_mm at " +
                             point_id + ".";
            }
            return false;
        }
        ids.insert(point_id);
        roles[fields[3]] += 1;
        if (fields[5] == "true") {
            ++fit_count;
        }
        if (point_id == "C") {
            has_c = true;
            c_x = x;
            c_y = y;
        } else if (point_id == "XPLUS") {
            has_xplus = true;
            xplus_x = x;
            xplus_y = y;
        }
        ++row_count;
    }

    if (expected_point_count > 0 && row_count != expected_point_count) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV point count mismatch: expected " +
                         std::to_string(expected_point_count) + ", got " +
                         std::to_string(row_count) + ".";
        }
        return false;
    }
    if (fit_count <= 0) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV has no include_in_fit=true rows.";
        }
        return false;
    }
    for (const char* role : {"origin", "x_orientation", "asymmetry"}) {
        if (roles[role] <= 0) {
            if (error_out) {
                *error_out = std::string("Physical target coordinate CSV is missing marker role: ") +
                             role;
            }
            return false;
        }
    }
    if (!has_c || !has_xplus) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV must contain C and XPLUS fiducials.";
        }
        return false;
    }
    const double dx = xplus_x - c_x;
    const double dy = xplus_y - c_y;
    const double span = std::sqrt(dx * dx + dy * dy);
    if (std::abs(span - 25.0) > 1e-6) {
        if (error_out) {
            std::ostringstream oss;
            oss << "Physical target C->XPLUS span in CSV is " << span
                << " mm, expected 25.000 mm.";
            *error_out = oss.str();
        }
        return false;
    }
    return true;
}

std::string json_string(const nlohmann::json& value, const char* key)
{
    return value.is_object() && value.contains(key) && value.at(key).is_string()
               ? value.at(key).get<std::string>()
               : std::string();
}

bool copy_bundle_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string* error_out)
{
    try {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(
            source,
            destination,
            std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "Failed to copy physical target bundle file from " +
                         source.generic_string() + " to " +
                         destination.generic_string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

std::filesystem::path relative_or_fallback(
    const std::filesystem::path& path,
    const std::filesystem::path& base,
    const std::filesystem::path& fallback)
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, base, error);
    if (error || relative.empty()) {
        return fallback;
    }
    return relative;
}

nlohmann::json default_coordinate_scale_policy()
{
    return {
        {"csv_coordinates_are_source_of_truth", true},
        {"do_not_rescale_from_outer_diameter", true},
        {"do_not_rescale_from_svg_export_transform", true},
        {"fiducial_span_check", {
            {"point_a", "C"},
            {"point_b", "XPLUS"},
            {"nominal_distance_mm", 25.0},
            {"measured_distance_mm", 25.0},
            {"measurement_method", "calipers"},
            {"measurement_uncertainty_mm", 0.5},
            {"apply_scale_correction", false}
        }}
    };
}

nlohmann::json fabrication_with_default_observations(nlohmann::json fabrication)
{
    if (!fabrication.is_object()) {
        fabrication = nlohmann::json::object();
    }
    if (!fabrication.contains("measured_outer_diameter_mm")) {
        fabrication["measured_outer_diameter_mm"] = 77.0;
    }
    if (!fabrication.contains("measured_outer_diameter_method")) {
        fabrication["measured_outer_diameter_method"] = "calipers";
    }
    if (!fabrication.contains("measured_C_to_XPLUS_mm")) {
        fabrication["measured_C_to_XPLUS_mm"] = 25.0;
    }
    if (!fabrication.contains("measured_C_to_XPLUS_method")) {
        fabrication["measured_C_to_XPLUS_method"] = "calipers";
    }
    if (!fabrication.contains("measured_C_to_XPLUS_uncertainty_mm")) {
        fabrication["measured_C_to_XPLUS_uncertainty_mm"] = 0.5;
    }
    if (!fabrication.contains("coordinate_scale_correction_applied")) {
        fabrication["coordinate_scale_correction_applied"] = false;
    }
    return fabrication;
}

bool expected_hash_matches(
    const std::string& label,
    const std::string& expected,
    const std::string& actual,
    std::string* error_out)
{
    if (expected.empty() || expected == actual) {
        return true;
    }
    if (error_out) {
        *error_out = label + " SHA-256 mismatch: JSON records " + expected +
                     ", actual file is " + actual + ".";
    }
    return false;
}

}  // namespace

std::filesystem::path default_physical_calibration_target_json_path()
{
    if (const char* env = std::getenv("ORANGE_PHYSICAL_TARGET_JSON")) {
        if (std::strlen(env) > 0) {
            return std::filesystem::path(env);
        }
    }

    const std::filesystem::path root(
        "/home/jeremy/citrus/targets/physical_calibration_targets");
    const std::filesystem::path expected =
        root / kDefaultPhysicalTargetId / kDefaultPhysicalTargetJsonFilename;
    if (std::filesystem::exists(expected)) {
        return expected;
    }

    const std::filesystem::path legacy_typo =
        root / "acrylic_hole_target_788mm_pitch5_margin3_v002" /
        kDefaultPhysicalTargetJsonFilename;
    return legacy_typo;
}

bool compute_file_sha256(
    const std::filesystem::path& path,
    std::string* sha256_out,
    std::string* error_out)
{
    return file_sha256(path, sha256_out, error_out);
}

bool materialize_physical_target_bundle_for_request(
    orange::calibration::CalibrationImageSetRequest* request,
    const std::filesystem::path& artifact_dir,
    std::string* error_out)
{
    if (request == nullptr) {
        if (error_out) {
            *error_out = "Null calibration image-set request for physical target bundle.";
        }
        return false;
    }
    if (request->purpose != kCameraOnlyPhysicalTargetCalibrationPurpose &&
        request->purpose != kDryPhysicalTargetHeightParallaxDiagnosticPurpose &&
        request->purpose != kProjectedSurfaceScaleCalibrationPurpose) {
        return true;
    }
    if (artifact_dir.empty()) {
        if (error_out) {
            *error_out = "Cannot materialize physical target bundle without artifact directory.";
        }
        return false;
    }

    const std::filesystem::path target_json_path =
        default_physical_calibration_target_json_path();
    nlohmann::json target_json;
    if (!read_json_file(target_json_path, &target_json, error_out)) {
        return false;
    }
    const std::filesystem::path source_dir = target_json_path.parent_path();
    const std::string target_id =
        target_json.value("physical_target_id", std::string(kDefaultPhysicalTargetId));
    if (target_id != request->target_id) {
        if (error_out) {
            *error_out = "Physical target ID mismatch: workflow requested " +
                         request->target_id + ", bundle contains " + target_id + ".";
        }
        return false;
    }

    const nlohmann::json source_files =
        target_json.value("source_files", nlohmann::json::object());
    const std::string csv_filename = json_string(source_files, "coordinate_csv_path");
    const std::string svg_filename = json_string(source_files, "svg_path");
    const std::filesystem::path csv_path =
        csv_filename.empty() ? std::filesystem::path() : source_dir / csv_filename;
    const std::filesystem::path svg_path =
        svg_filename.empty() ? std::filesystem::path() : source_dir / svg_filename;
    std::filesystem::path png_path;
    const std::string configured_png =
        json_string(source_files, "reference_png_path").empty()
            ? json_string(source_files, "png_path")
            : json_string(source_files, "reference_png_path");
    if (!configured_png.empty()) {
        png_path = source_dir / configured_png;
    } else if (!svg_path.empty()) {
        png_path = svg_path;
        png_path.replace_extension(".png");
    }

    if (csv_path.empty() || !std::filesystem::is_regular_file(csv_path)) {
        if (error_out) {
            *error_out = "Physical target coordinate CSV is missing: " +
                         csv_path.generic_string();
        }
        return false;
    }
    if (svg_path.empty() || !std::filesystem::is_regular_file(svg_path)) {
        if (error_out) {
            *error_out = "Physical target SVG is missing: " + svg_path.generic_string();
        }
        return false;
    }

    std::string json_sha256;
    std::string csv_sha256;
    std::string svg_sha256;
    std::string png_sha256;
    if (!file_sha256(target_json_path, &json_sha256, error_out) ||
        !file_sha256(csv_path, &csv_sha256, error_out) ||
        !file_sha256(svg_path, &svg_sha256, error_out)) {
        return false;
    }
    if (!expected_hash_matches(
            "Physical target coordinate CSV",
            json_string(source_files, "coordinate_csv_sha256"),
            csv_sha256,
            error_out) ||
        !expected_hash_matches(
            "Physical target SVG",
            json_string(source_files, "svg_sha256"),
            svg_sha256,
            error_out)) {
        return false;
    }
    if (!png_path.empty() && std::filesystem::exists(png_path)) {
        if (!file_sha256(png_path, &png_sha256, error_out)) {
            return false;
        }
        const std::string expected_png_sha =
            json_string(source_files, "reference_png_sha256").empty()
                ? json_string(source_files, "png_sha256")
                : json_string(source_files, "reference_png_sha256");
        if (!expected_hash_matches(
                "Physical target reference PNG",
                expected_png_sha,
                png_sha256,
                error_out)) {
            return false;
        }
    }

    const int expected_points =
        target_json.value("point_summary", nlohmann::json::object())
            .value("num_points_total", 0);
    if (!validate_coordinate_csv(csv_path, expected_points, error_out)) {
        return false;
    }

    const std::filesystem::path relative_bundle_dir =
        std::filesystem::path("targets") / sanitize_artifact_component(target_id);
    const std::filesystem::path bundle_dir = artifact_dir / relative_bundle_dir;
    const std::filesystem::path copied_json = bundle_dir / target_json_path.filename();
    const std::filesystem::path copied_csv = bundle_dir / csv_path.filename();
    const std::filesystem::path copied_svg = bundle_dir / svg_path.filename();
    if (!copy_bundle_file(target_json_path, copied_json, error_out) ||
        !copy_bundle_file(csv_path, copied_csv, error_out) ||
        !copy_bundle_file(svg_path, copied_svg, error_out)) {
        return false;
    }
    std::filesystem::path copied_png;
    if (!png_sha256.empty()) {
        copied_png = bundle_dir / png_path.filename();
        if (!copy_bundle_file(png_path, copied_png, error_out)) {
            return false;
        }
    }

    nlohmann::json physical_target = request->physical_target.is_object()
                                         ? request->physical_target
                                         : nlohmann::json::object();
    physical_target["physical_target_id"] = target_id;
    physical_target["target_id"] = target_id;
    physical_target["target_type"] =
        target_json.value("target_type", "opaque_acrylic_hole_mask");
    physical_target["target_design"] = request->target_design;
    physical_target["source"] = "citrus_physical_calibration_target_bundle";
    physical_target["source_json_path"] = target_json_path.generic_string();
    physical_target["source_json_sha256"] = json_sha256;
    physical_target["session_bundle_dir"] = relative_bundle_dir.generic_string();
    physical_target["session_json_path"] =
        relative_or_fallback(copied_json, artifact_dir, relative_bundle_dir / copied_json.filename())
            .generic_string();
    physical_target["coordinate_csv_path"] =
        relative_or_fallback(copied_csv, artifact_dir, relative_bundle_dir / copied_csv.filename())
            .generic_string();
    physical_target["coordinate_csv_sha256"] = csv_sha256;
    physical_target["svg_path"] =
        relative_or_fallback(copied_svg, artifact_dir, relative_bundle_dir / copied_svg.filename())
            .generic_string();
    physical_target["svg_sha256"] = svg_sha256;
    if (!png_sha256.empty()) {
        physical_target["reference_png_path"] =
            relative_or_fallback(copied_png, artifact_dir, relative_bundle_dir / copied_png.filename())
                .generic_string();
        physical_target["reference_png_sha256"] = png_sha256;
    }
    physical_target["instance_tracking"] = "untracked_equivalent_set";
    physical_target["physical_target_instance_id"] = nullptr;
    physical_target["equivalent_instance_count"] = 4;
    physical_target["equivalence_basis"] =
        "operator measured all four fabricated instances with calipers as 77 mm outside diameter, 5 mm pitch, and 25 mm C-to-XPLUS span";
    physical_target["coordinate_frame"] =
        target_json.value("coordinate_frame", nlohmann::json::object());
    physical_target["geometry"] =
        target_json.value("geometry", nlohmann::json::object());
    physical_target["fabrication"] =
        fabrication_with_default_observations(
            target_json.value("fabrication", nlohmann::json::object()));
    physical_target["coordinate_scale_policy"] =
        target_json.contains("coordinate_scale_policy")
            ? target_json["coordinate_scale_policy"]
            : default_coordinate_scale_policy();
    physical_target["point_summary"] =
        target_json.value("point_summary", nlohmann::json::object());
    physical_target["coordinate_table_authority"] =
        "session-local copied CSV; CSV coordinates are the source of truth";
    physical_target["coordinate_target_kind"] = "physical_hole_centers";
    physical_target["projected_pattern_used_as_coordinate_target"] = false;
    physical_target["capture_condition_policy"] = {
        {"wet_or_dry", request->wet_or_dry},
        {"water_path_included", request->wet_or_dry == "wet"},
        {"runtime_correction_ready",
         request->purpose == kProjectedSurfaceScaleCalibrationPurpose},
        {"current_use", request->purpose},
        {"analysis_readiness",
         request->purpose == kProjectedSurfaceScaleCalibrationPurpose
             ? "orange_correspondence_fit_then_citrus_authoritative_refit"
             : "raw_capture_and_provenance_only_until_fiducial_detection_is_reliable"}
    };
    if (request->purpose == kProjectedSurfaceScaleCalibrationPurpose) {
        physical_target["plane_contract"] = {
            {"target_plane", "projected_surface"},
            {"illuminated_hole_center_plane_z_mm", 0.0},
            {"camera_facing_target_surface_z_mm", 3.0},
            {"occluding_target_thickness_mm", 3.0},
            {"coordinate_interpretation",
             "transmitted-light hole centers locate the projected surface; the acrylic top face is not z=0"},
        };
    }
    if (request->purpose == kDryPhysicalTargetHeightParallaxDiagnosticPurpose) {
        physical_target["capture_condition_policy"]["diagnostic_only"] = true;
        physical_target["capture_condition_policy"]["not_runtime_recording_condition_map"] = true;
        physical_target["capture_condition_policy"]["fiducial_detection_status"] =
            "operator_reported_unreliable";
        physical_target["capture_condition_policy"]["water_state_interpretation"] =
            "dry camera/lens height-dependence diagnostic; no water-surface refraction included";
    }

    request->physical_target = std::move(physical_target);
    return true;
}

}  // namespace orange::gui::spatial_layout
