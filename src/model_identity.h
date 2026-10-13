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
#include <optional>
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
    // Palette's canonical ONNX manifest named above, checked when it is named: its bytes must
    // match canonical_manifest_sha256, and it must be valid against the Palette schema version it
    // declares (contracts/palette_canonical_onnx_model_manifest_v{1,2}.schema.json, generated
    // readers). Empty when valid or not named. Not written to the snapshot; a non-empty value
    // refuses the record start (canonical_manifest_refusal).
    std::string canonical_manifest_error;
    std::string onnx_manifest_path;        // source.onnx_manifest (detect)
    std::string onnx_manifest_sha256;
};

// Hash the engine and read its manifest. An empty path returns an empty
// identity (engine_present false, no manifest). Cached per path.
ModelIdentity resolve_model_identity(const std::string& engine_path);

// Why a recording must not start with this engine, or empty: the canonical manifest its engine
// manifest names is unreadable, has changed, or is not valid against its declared schema.
std::string canonical_manifest_refusal(const ModelIdentity& identity);

// Same, bypassing the cache (tests).
ModelIdentity resolve_model_identity_uncached(const std::string& engine_path);

// The snapshot runtime block's `engine_manifest` (models.<serial>.detect/.pose),
// filled into the generated struct of orange.recording_detect_model.v1 or
// orange.recording_pose_model.v2 (both declare the same members); nullopt (JSON
// null) when the engine path is empty.
template <class Block>
std::optional<Block> engine_manifest_block(const ModelIdentity& identity)
{
    if (identity.engine_path.empty()) {
        return std::nullopt;
    }
    Block block;
    block.path = identity.manifest_path;
    block.present = identity.manifest_present;
    block.sha256 = identity.manifest_sha256;
    block.error = identity.manifest_error;
    block.schema_id = identity.manifest_schema_id;
    block.schema_version = static_cast<std::uint64_t>(identity.manifest_schema_version);
    block.status = identity.manifest_status;
    block.task = identity.task;
    block.run_id = identity.run_id;
    block.set_id = identity.set_id;
    block.build_id = identity.build_id;
    block.precision = identity.precision;
    block.target_hardware_class = identity.target_hardware_class;
    block.created_at_utc = identity.created_at_utc;
    block.engine_sha256_declared = identity.declared_engine_sha256;
    block.engine_sha256_matches = identity.declared_engine_sha256_matches;
    block.onnx.path = identity.onnx_path;
    block.onnx.sha256 = identity.onnx_sha256;
    block.weights.path = identity.weights_path;
    block.weights.sha256 = identity.weights_sha256;
    block.canonical_manifest.path = identity.canonical_manifest_path;
    block.canonical_manifest.sha256 = identity.canonical_manifest_sha256;
    block.onnx_manifest.path = identity.onnx_manifest_path;
    block.onnx_manifest.sha256 = identity.onnx_manifest_sha256;
    return block;
}

}  // namespace orange::model_identity
