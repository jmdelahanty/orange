#include "gui/spatial_layout/commissioning_finalization.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <thread>

namespace orange::gui::spatial_layout {
namespace {

std::string SafeComponent(std::string value)
{
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '-' && ch != '_' && ch != '.') {
            ch = '_';
        }
    }
    return value.empty() ? "unknown" : value;
}

bool ExactActiveRelease(
    const nlohmann::json& status,
    const std::string& expected_release_id)
{
    return status.is_object() &&
        status.value("compatible", false) &&
        status.value("state", "") == "compatible" &&
        status.value("release_id", "") == expected_release_id &&
        !status.value("manifest_path", "").empty() &&
        !status.value("manifest_sha256", "").empty();
}

}  // namespace

std::string expected_commissioning_release_id(
    const std::string& transaction_id)
{
    return "commissioning_" + SafeComponent(transaction_id);
}

CommissioningFinalizationResult assess_commissioning_finalization_status(
    const nlohmann::json& commissioning_status,
    const std::string& expected_transaction_id,
    const std::string& expected_release_id,
    bool deadline_expired)
{
    CommissioningFinalizationResult result;
    result.expected_release_id = expected_release_id;
    if (!commissioning_status.is_object()) {
        if (deadline_expired) {
            result.disposition = CommissioningFinalizationDisposition::kTimedOut;
            result.error = "commissioning_status_unavailable_at_deadline";
        }
        return result;
    }
    result.commissioning = commissioning_status;

    // Exact compatible active authority is sufficient reconciliation even if
    // Orange missed the original mutation acknowledgement or retried it.
    if (ExactActiveRelease(commissioning_status, expected_release_id)) {
        result.disposition = CommissioningFinalizationDisposition::kPublished;
        result.release_id = commissioning_status.value("release_id", "");
        result.manifest_path = commissioning_status.value("manifest_path", "");
        result.manifest_sha256 =
            commissioning_status.value("manifest_sha256", "");
        return result;
    }

    const auto last_finalize = commissioning_status.value(
        "last_finalize", nlohmann::json::object());
    const std::string last_state = last_finalize.value("state", "not_requested");
    if (last_state == "committed" &&
        last_finalize.value("release_id", "") == expected_release_id) {
        // The pointer was committed but the runtime compatibility snapshot has
        // not caught up yet. Keep polling rather than claiming publication.
    } else if (last_state == "rejected" &&
               last_finalize.value("transaction_id", "") ==
                   expected_transaction_id) {
        result.disposition = CommissioningFinalizationDisposition::kRejected;
        result.error = last_finalize.value(
            "error", "commissioning_finalization_rejected");
        return result;
    }

    if (deadline_expired) {
        result.disposition = CommissioningFinalizationDisposition::kTimedOut;
        result.error = "commissioning_finalization_confirmation_timed_out";
    }
    return result;
}

CommissioningFinalizationWorker::~CommissioningFinalizationWorker()
{
    Cancel();
    if (future_.valid()) future_.wait();
}

bool CommissioningFinalizationWorker::Start(
    CommissioningFinalizationRequest request,
    CommissioningFinalizeCallback finalize,
    CommissioningStatusCallback status,
    std::string* error_out)
{
    if (running_) {
        if (error_out) *error_out = "commissioning_finalization_already_running";
        return false;
    }
    if (request.transaction_id.empty() || request.operation_id.empty() ||
        request.canvas_path.empty() ||
        request.expected_canvas_checksum.empty() || !finalize || !status) {
        if (error_out) *error_out = "commissioning_finalization_input_invalid";
        return false;
    }
    if (!request.accept_commissioning_armed) {
        if (error_out) *error_out = "accept_commissioning_not_armed";
        return false;
    }
    if (request.poll_interval < std::chrono::milliseconds(1)) {
        request.poll_interval = std::chrono::milliseconds(1);
    }
    if (request.timeout < request.poll_interval) {
        if (error_out) *error_out = "commissioning_finalization_timeout_invalid";
        return false;
    }

    cancel_requested_.store(false, std::memory_order_release);
    running_ = true;
    future_ = std::async(
        std::launch::async,
        [this, request = std::move(request),
         finalize = std::move(finalize), status = std::move(status)]() mutable {
            CommissioningFinalizationResult result;
            result.expected_release_id =
                expected_commissioning_release_id(request.transaction_id);
            const auto submitted = finalize(request);
            const std::string submission_error = submitted.error;
            if (!submitted.ok && submitted.definitive_rejection) {
                result.disposition =
                    CommissioningFinalizationDisposition::kRejected;
                result.error = submission_error.empty()
                    ? "commissioning_finalize_request_rejected"
                    : submission_error;
                result.commissioning = submitted.commissioning;
                return result;
            }

            const auto deadline =
                std::chrono::steady_clock::now() + request.timeout;
            while (!cancel_requested_.load(std::memory_order_acquire)) {
                const bool deadline_expired =
                    std::chrono::steady_clock::now() >= deadline;
                const auto queried = status(
                    "commissioning-finalization-confirmation");
                if (queried.ok) {
                    result = assess_commissioning_finalization_status(
                        queried.commissioning, request.transaction_id,
                        result.expected_release_id,
                        deadline_expired);
                    if (result.disposition !=
                        CommissioningFinalizationDisposition::kPending) {
                        return result;
                    }
                } else if (deadline_expired) {
                    result.disposition =
                        CommissioningFinalizationDisposition::kTimedOut;
                    result.error = !submission_error.empty()
                        ? submission_error
                        : (queried.error.empty()
                            ? "commissioning_status_unavailable_at_deadline"
                            : queried.error);
                    return result;
                }
                std::this_thread::sleep_for(request.poll_interval);
            }
            result.disposition =
                CommissioningFinalizationDisposition::kCancelled;
            result.error = "commissioning_finalization_cancelled";
            return result;
        });
    return true;
}

bool CommissioningFinalizationWorker::Poll(
    CommissioningFinalizationResult* result_out)
{
    if (!running_ || !future_.valid() ||
        future_.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
        return false;
    }
    CommissioningFinalizationResult result;
    try {
        result = future_.get();
    } catch (const std::exception& error) {
        result.disposition = CommissioningFinalizationDisposition::kRejected;
        result.error = std::string("commissioning_finalization_worker_failed:") +
            error.what();
    } catch (...) {
        result.disposition = CommissioningFinalizationDisposition::kRejected;
        result.error = "commissioning_finalization_worker_failed:unknown";
    }
    running_ = false;
    if (result_out) *result_out = std::move(result);
    return true;
}

bool CommissioningFinalizationWorker::running() const
{
    return running_;
}

void CommissioningFinalizationWorker::Cancel()
{
    cancel_requested_.store(true, std::memory_order_release);
}

}  // namespace orange::gui::spatial_layout
