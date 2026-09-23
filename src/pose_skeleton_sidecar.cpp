// src/pose_skeleton_sidecar.cpp — see pose_skeleton_sidecar.h.
#include "pose_skeleton_sidecar.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "gui/spatial_layout/sha256.h"

namespace orange::pose {

namespace {
bool read_file(const std::string& path, std::string* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}
}  // namespace

std::string file_sha256_hex(const std::string& path)
{
    std::string bytes;
    if (!read_file(path, &bytes)) {
        return {};
    }
    return orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

uint64_t sha256_prefix64(const std::string& hex)
{
    if (hex.size() < 16) {
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = hex[i];
        int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return 0;
        value = (value << 4) | static_cast<uint64_t>(nibble);
    }
    return value;
}

bool parse_pose_skeleton_sidecar(const std::string& bytes, const std::string& path,
                                 PoseSkeletonSidecar* out, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = "pose skeleton sidecar " + path + ": " + why;
        return false;
    };
    if (!out) {
        return fail("no output");
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(bytes);
    } catch (const std::exception& e) {
        return fail(std::string("invalid JSON: ") + e.what());
    }
    if (!doc.is_object()) {
        return fail("top level is not an object");
    }
    PoseSkeletonSidecar s;
    s.path = path;
    s.schema_id = doc.value("schema_id", std::string());
    if (s.schema_id != "palette.pose_model_skeleton") {
        return fail("unsupported schema_id '" + s.schema_id + "' (expected palette.pose_model_skeleton)");
    }
    if (!doc.contains("schema_version") || !doc["schema_version"].is_number_integer()) {
        return fail("schema_version missing");
    }
    s.schema_version = doc["schema_version"].get<int>();
    if (s.schema_version != 1) {
        return fail("unsupported schema_version " + std::to_string(s.schema_version) + " (expected 1)");
    }
    s.skeleton_id = doc.value("skeleton_id", std::string());
    if (s.skeleton_id.empty()) {
        return fail("skeleton_id missing");
    }
    if (!doc.contains("nodes") || !doc["nodes"].is_array() || doc["nodes"].empty()) {
        return fail("nodes missing or empty");
    }
    const auto& nodes = doc["nodes"];
    s.labels.assign(nodes.size(), std::string());
    for (const auto& node : nodes) {
        if (!node.is_object() || !node.contains("id") || !node["id"].is_number_integer() ||
            !node.contains("name") || !node["name"].is_string()) {
            return fail("each node needs an integer id and a string name");
        }
        const int id = node["id"].get<int>();
        const std::string name = node["name"].get<std::string>();
        if (id < 0 || id >= static_cast<int>(nodes.size())) {
            return fail("node id " + std::to_string(id) + " outside 0.." + std::to_string(nodes.size() - 1));
        }
        if (!s.labels[id].empty()) {
            return fail("duplicate node id " + std::to_string(id));
        }
        if (name.empty()) {
            return fail("node " + std::to_string(id) + " has an empty name");
        }
        s.labels[id] = name;
    }
    if (!doc.contains("edges") || !doc["edges"].is_array()) {
        return fail("edges missing");
    }
    for (const auto& edge : doc["edges"]) {
        if (!edge.is_array() || edge.size() != 2 || !edge[0].is_number_integer() || !edge[1].is_number_integer()) {
            return fail("each edge must be a pair of integers");
        }
        const int a = edge[0].get<int>();
        const int b = edge[1].get<int>();
        if (a < 0 || b < 0 || a >= static_cast<int>(s.labels.size()) || b >= static_cast<int>(s.labels.size()) || a == b) {
            return fail("edge [" + std::to_string(a) + "," + std::to_string(b) + "] outside the nodes");
        }
        s.edges.emplace_back(a, b);
    }
    if (!doc.contains("kpt_shape") || !doc["kpt_shape"].is_array() || doc["kpt_shape"].size() != 2 ||
        !doc["kpt_shape"][0].is_number_integer() || !doc["kpt_shape"][1].is_number_integer()) {
        return fail("kpt_shape must be [K, D]");
    }
    s.kpt_k = doc["kpt_shape"][0].get<int>();
    s.kpt_d = doc["kpt_shape"][1].get<int>();
    if (s.kpt_k != static_cast<int>(s.labels.size())) {
        return fail("kpt_shape K=" + std::to_string(s.kpt_k) + " does not match " + std::to_string(s.labels.size()) + " nodes");
    }
    if (s.kpt_d != 3) {
        return fail("kpt_shape D=" + std::to_string(s.kpt_d) + " is not the model shape [K,3] (x, y, confidence)");
    }
    if (doc.contains("source") && doc["source"].is_object()) {
        s.source_run_id = doc["source"].value("run_id", std::string());
        s.source_onnx_sha256 = doc["source"].value("onnx_sha256", std::string());
    }
    s.raw_text = bytes;
    s.file_sha256 = orange::gui::spatial_layout::checksum::sha256_hex(bytes);
    s.file_sha256_prefix64 = sha256_prefix64(s.file_sha256);
    *out = std::move(s);
    return true;
}

bool load_pose_skeleton_sidecar(const std::string& path, PoseSkeletonSidecar* out, std::string* error)
{
    std::string bytes;
    if (!read_file(path, &bytes)) {
        if (error) *error = "pose skeleton sidecar " + path + ": cannot read file";
        return false;
    }
    return parse_pose_skeleton_sidecar(bytes, path, out, error);
}

}  // namespace orange::pose
