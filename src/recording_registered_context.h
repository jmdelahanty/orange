#pragma once

#include "json.hpp"
#include "session/registered_scene_context_capture_declaration.h"
#include "spatial_roi_session_authority_store.h"
#include <map>
#include <memory>
#include <vector>

namespace orange::recording {

struct RegisteredContextConfig {
    bool enabled = false;
    int timeout_ms = 10000;
    std::vector<int> worker_cpu_ids;
    session::spatial_roi::RegisteredSceneContextCaptureDeclaration declaration;
    static RegisteredContextConfig Parse(const nlohmann::json& value);
    nlohmann::json ToJson() const;
};

// Immutable prearm bindings. No Arena, region or crop ID is invented for the
// full-camera context. The master acquisition owner supplies producer identity.
struct RegisteredContextCamera {
    std::string serial, producer_instance_id;
    uint64_t camera_id = 0, stream_generation = 0;
    int width = 0, height = 0;
};
struct RegisteredContextFrame {
    std::string camera_serial;
    int width = 0, height = 0;
    uint64_t local_frame_id = 0, camera_frame_id = 0, recording_frame_id = 0;
    uint64_t camera_timestamp_ns = 0, timestamp_sys_ns = 0;
    std::vector<unsigned char> mono8;
};

// All disk work is control-plane, before recording arm. Owns a descriptor-bound
// recording root; uses the shared ROI branch's generic authority store unchanged.
class RegisteredContextSet {
public:
    void Prepare(const RegisteredContextConfig& config,
                 const std::filesystem::path& root,
                 const std::vector<RegisteredContextCamera>& cameras,
                 const nlohmann::json& resolved_geometry_contract);
    nlohmann::json StartEvidence() const;
    void Accept(const RegisteredContextFrame& frame);
    bool Complete() const;
private:
    RegisteredContextConfig config_;
    std::string recording_id_;
    std::unique_ptr<session::spatial_roi::SpatialRoiSessionAuthorityStore> store_;
    session::spatial_roi::SpatialRoiSessionAuthorityReceipt geometry_;
    nlohmann::json bindings_ = nlohmann::json::array();
    std::map<std::string, bool> captured_;
};

// Separate from existing geometry/identity v1 contracts. Required context
// failures cannot be hidden by a subsequently refreshed mutable snapshot.
void ApplyRequiredRegisteredContextGate(const std::filesystem::path& root,
                                      nlohmann::json* parent);
} // namespace orange::recording
