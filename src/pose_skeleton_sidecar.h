// src/pose_skeleton_sidecar.h
//
// Reader for Palette's pose skeleton sidecar (`palette.pose_model_skeleton`
// v1): the ordered keypoint labels, edges and kpt_shape that a pose model
// was trained with, shipped next to the ONNX/engine. Orange adopts it per
// the deployment contract: exact ordered labels and edges for decoding and
// overlays, K checked against the real engine output, the file's SHA-256
// as the skeleton identity, unsupported versions rejected.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orange::pose {

struct PoseSkeletonSidecar {
    std::string path;
    std::string schema_id;
    int schema_version = 0;
    std::string skeleton_id;                    // e.g. "pose_schema:traditional_v1"
    std::vector<std::string> labels;            // training order, index = keypoint index
    std::vector<std::pair<int, int>> edges;     // zero-based keypoint index pairs
    int kpt_k = 0;                              // kpt_shape[0]
    int kpt_d = 0;                              // kpt_shape[1] (model shape, 3 = x,y,conf)
    std::string source_run_id;
    std::string source_onnx_sha256;
    std::string raw_text;                       // the exact file bytes (hash-verifiable)
    std::string file_sha256;                    // lowercase hex of the raw file bytes
    uint64_t file_sha256_prefix64 = 0;          // first 8 bytes, big-endian: the IPC skeleton hash
};

// Loads and validates the sidecar. Returns false and fills `error` on any
// rejection (missing file, wrong schema/version, non-contiguous node ids,
// edge out of range, kpt_shape inconsistent with the nodes).
bool load_pose_skeleton_sidecar(const std::string& path, PoseSkeletonSidecar* out, std::string* error);

// Validates already-parsed JSON text (for tests and for callers that have
// the bytes); `file_sha256` fields are computed over `bytes`.
bool parse_pose_skeleton_sidecar(const std::string& bytes, const std::string& path,
                                 PoseSkeletonSidecar* out, std::string* error);

// SHA-256 (lowercase hex) of a file's raw bytes; empty on failure.
std::string file_sha256_hex(const std::string& path);

// First 8 bytes of a hex SHA-256 as a big-endian uint64 (0 if malformed).
uint64_t sha256_prefix64(const std::string& hex);

}  // namespace orange::pose
