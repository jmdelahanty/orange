#pragma once

#include "json.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>

namespace orange::gui::spatial_layout {

enum class CommissioningFinalizationDisposition {
    kPending,
    kPublished,
    kRejected,
    kTimedOut,
    kCancelled,
};

struct CommissioningFinalizationRequest {
    std::string transaction_id;
    std::string operation_id;
    std::string canvas_path;
    std::string expected_canvas_checksum;
    bool accept_commissioning_armed = false;
    std::chrono::milliseconds poll_interval{100};
    std::chrono::milliseconds timeout{15000};
};

struct CommissioningControlReply {
    bool ok = false;
    bool definitive_rejection = false;
    nlohmann::json commissioning = nlohmann::json::object();
    std::string error;
};

struct CommissioningFinalizationResult {
    CommissioningFinalizationDisposition disposition =
        CommissioningFinalizationDisposition::kPending;
    std::string expected_release_id;
    std::string release_id;
    std::string manifest_path;
    std::string manifest_sha256;
    std::string error;
    nlohmann::json commissioning = nlohmann::json::object();
};

using CommissioningFinalizeCallback = std::function<CommissioningControlReply(
    const CommissioningFinalizationRequest&)>;
using CommissioningStatusCallback =
    std::function<CommissioningControlReply(const std::string& phase)>;

std::string expected_commissioning_release_id(
    const std::string& transaction_id);

CommissioningFinalizationResult assess_commissioning_finalization_status(
    const nlohmann::json& commissioning_status,
    const std::string& expected_transaction_id,
    const std::string& expected_release_id,
    bool deadline_expired);

class CommissioningFinalizationWorker {
public:
    CommissioningFinalizationWorker() = default;
    ~CommissioningFinalizationWorker();

    CommissioningFinalizationWorker(
        const CommissioningFinalizationWorker&) = delete;
    CommissioningFinalizationWorker& operator=(
        const CommissioningFinalizationWorker&) = delete;

    bool Start(
        CommissioningFinalizationRequest request,
        CommissioningFinalizeCallback finalize,
        CommissioningStatusCallback status,
        std::string* error_out);
    bool Poll(CommissioningFinalizationResult* result_out);
    bool running() const;
    void Cancel();

private:
    std::atomic<bool> cancel_requested_{false};
    std::future<CommissioningFinalizationResult> future_;
    bool running_ = false;
};

}  // namespace orange::gui::spatial_layout
