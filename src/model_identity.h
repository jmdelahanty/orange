#pragma once

// Model identity for the recording snapshot (realtime-products slice, item 2).
//
// Palette trains the models and exports the ONNX; Orange builds every TensorRT
// engine it runs (detect and pose) on the rig, and its build scripts
// (scripts/write_tensorrt_engine_manifest.py, write_pose_tensorrt_engine_manifest.py)
// write `<stem>.manifest.json` beside the engine (`foo.engine` ->
// `foo.manifest.json`, schema_id orange.tensorrt_engine_manifest v1, published in
// docs/schemas) with the engine's SHA-256, the source ONNX, weights and Palette
// manifest SHA-256s, the build id and precision. Palette's own ONNX manifests are
// named there by path and digest only; Orange does not parse them. This module
// hashes the engine bytes that actually ran, reads that manifest through the
// generated strict reader and reports both, so `models.<serial>.detect/.pose.runtime`
// carries the full identity chain (weights -> onnx -> engine) and whether the
// bytes on disk still match the manifest's declaration. A manifest that does not
// match the published schema leaves the fields empty and says why in
// manifest_error.
//
// Resolution is cached per engine path (the hashes take tens of ms for the
// 6-9 MB engines) and is only ever called from the record-start path, never
// from an acquisition, YOLO or pose thread.

#include <cstdint>
#include <string>

#include "json.hpp"

namespace orange::model_identity {

struct ModelIdentity {
    std::string engine_path;
    bool engine_present = false;
    std::uint64_t engine_bytes = 0;
    std::string engine_sha256;             // computed from the file bytes; empty when unreadable

    std::string manifest_path;             // <stem>.manifest.json beside the engine
    bool manifest_present = false;
    std::string manifest_sha256;           // file bytes of the manifest
    std::string manifest_error;            // parse / schema error, empty when ok
    std::string manifest_schema_id;
    int manifest_schema_version = 0;
    std::string manifest_status;           // candidate / active / ...
    std::string task;                      // detect / pose (pose manifests only)
    std::string run_id;                    // palette_run_id
    std::string set_id;
    std::string build_id;
    std::string precision;
    std::string target_hardware_class;
    std::string created_at_utc;
    std::string declared_engine_sha256;    // manifest engine.sha256
    bool declared_engine_sha256_matches = false;
    std::string onnx_sha256;               // source.onnx.sha256
    std::string onnx_path;
    std::string weights_sha256;            // source.weights.sha256
    std::string weights_path;
    std::string canonical_manifest_path;   // source.canonical_manifest (pose)
    std::string canonical_manifest_sha256;
    std::string onnx_manifest_path;        // source.onnx_manifest (detect)
    std::string onnx_manifest_sha256;
};

// Hash the engine and read its manifest. An empty path returns an empty
// identity (engine_present false, no manifest). Cached per path.
ModelIdentity resolve_model_identity(const std::string& engine_path);

// Same, bypassing the cache (tests).
ModelIdentity resolve_model_identity_uncached(const std::string& engine_path);

// The `engine_manifest` object written into the snapshot runtime block; null
// JSON when the engine path is empty.
nlohmann::json engine_manifest_json(const ModelIdentity& identity);

}  // namespace orange::model_identity
