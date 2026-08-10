#include "gui/spatial_layout/commissioning_finalization.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace spatial = orange::gui::spatial_layout;

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

nlohmann::json CompatibleStatus(const std::string& release_id)
{
    return {
        {"state", "compatible"},
        {"compatible", true},
        {"release_id", release_id},
        {"manifest_path", "/commissioning/commissioning.json"},
        {"manifest_sha256", "sha256:manifest"},
        {"last_finalize", {
            {"state", "committed"},
            {"release_id", release_id},
        }},
    };
}

void TestExactActiveReleasePublishes()
{
    const auto result = spatial::assess_commissioning_finalization_status(
        CompatibleStatus("commissioning_txn"), "txn", "commissioning_txn",
        false);
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kPublished,
            "exact compatible release should publish");
    Require(result.manifest_sha256 == "sha256:manifest",
            "published proof should retain the manifest checksum");
}

void TestOldRejectionDoesNotTerminateNewRequest()
{
    const nlohmann::json status = {
        {"state", "invalid"},
        {"compatible", false},
        {"last_finalize", {
            {"state", "rejected"},
            {"transaction_id", "older"},
            {"error", "old_failure"},
        }},
    };
    const auto result = spatial::assess_commissioning_finalization_status(
        status, "current", "commissioning_current", false);
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kPending,
            "an old rejection must not reject the current transaction");
}

void TestMatchingRejectionTerminates()
{
    const nlohmann::json status = {
        {"state", "invalid"},
        {"compatible", false},
        {"last_finalize", {
            {"state", "rejected"},
            {"transaction_id", "current"},
            {"error", "commissioning_canvas_compare_and_swap_conflict"},
        }},
    };
    const auto result = spatial::assess_commissioning_finalization_status(
        status, "current", "commissioning_current", false);
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kRejected,
            "matching rejection should terminate");
    Require(result.error ==
                "commissioning_canvas_compare_and_swap_conflict",
            "matching rejection reason should be preserved");
}

void TestWorkerConfirmsWithoutBlockingCaller()
{
    spatial::CommissioningFinalizationWorker worker;
    spatial::CommissioningFinalizationRequest request;
    request.transaction_id = "txn";
    request.operation_id = "op";
    request.canvas_path = "/canvas.json";
    request.expected_canvas_checksum = "sha256:canvas";
    request.accept_commissioning_armed = true;
    request.poll_interval = std::chrono::milliseconds(1);
    request.timeout = std::chrono::milliseconds(100);
    int status_calls = 0;
    std::string error;
    const bool started = worker.Start(
        request,
        [](const auto&) {
            spatial::CommissioningControlReply reply;
            reply.ok = true;
            return reply;
        },
        [&status_calls](const std::string&) {
            spatial::CommissioningControlReply reply;
            reply.ok = true;
            ++status_calls;
            if (status_calls >= 2) {
                reply.commissioning = CompatibleStatus("commissioning_txn");
            } else {
                reply.commissioning = {
                    {"state", "invalid"}, {"compatible", false}};
            }
            return reply;
        },
        &error);
    Require(started, "worker should start: " + error);
    Require(worker.running(), "worker should report an active background task");
    spatial::CommissioningFinalizationResult result;
    for (int attempt = 0; attempt < 100 && !worker.Poll(&result); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kPublished,
            "worker should confirm the exact published release");
    Require(status_calls >= 2, "worker should poll until compatibility catches up");
}

void TestWorkerReconcilesLostFinalizeAcknowledgement()
{
    spatial::CommissioningFinalizationWorker worker;
    spatial::CommissioningFinalizationRequest request;
    request.transaction_id = "txn-lost-ack";
    request.operation_id = "op-lost-ack";
    request.canvas_path = "/canvas.json";
    request.expected_canvas_checksum = "sha256:canvas";
    request.accept_commissioning_armed = true;
    request.poll_interval = std::chrono::milliseconds(1);
    request.timeout = std::chrono::milliseconds(100);
    std::string error;
    const bool started = worker.Start(
        request,
        [](const auto&) {
            spatial::CommissioningControlReply reply;
            reply.ok = false;
            reply.error = "no response from Citrus local-control socket";
            return reply;
        },
        [](const std::string&) {
            spatial::CommissioningControlReply reply;
            reply.ok = true;
            reply.commissioning =
                CompatibleStatus("commissioning_txn-lost-ack");
            return reply;
        },
        &error);
    Require(started, "lost-ack reconciliation worker should start: " + error);
    spatial::CommissioningFinalizationResult result;
    for (int attempt = 0; attempt < 100 && !worker.Poll(&result); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kPublished,
            "exact active release should reconcile a lost submit acknowledgement");
}

void TestWorkerReportsDefinitiveRequestRejectionImmediately()
{
    spatial::CommissioningFinalizationWorker worker;
    spatial::CommissioningFinalizationRequest request;
    request.transaction_id = "txn-rejected";
    request.operation_id = "op-rejected";
    request.canvas_path = "/canvas.json";
    request.expected_canvas_checksum = "sha256:canvas";
    request.accept_commissioning_armed = true;
    request.poll_interval = std::chrono::milliseconds(1);
    request.timeout = std::chrono::milliseconds(100);
    std::string error;
    const bool started = worker.Start(
        request,
        [](const auto&) {
            spatial::CommissioningControlReply reply;
            reply.ok = false;
            reply.definitive_rejection = true;
            reply.error = "commissioning_canvas_compare_and_swap_conflict";
            return reply;
        },
        [](const std::string&) {
            throw std::runtime_error(
                "status callback must not run after definitive rejection");
            return spatial::CommissioningControlReply{};
        },
        &error);
    Require(started, "definitive-rejection worker should start: " + error);
    spatial::CommissioningFinalizationResult result;
    for (int attempt = 0; attempt < 100 && !worker.Poll(&result); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(result.disposition ==
                spatial::CommissioningFinalizationDisposition::kRejected,
            "definitive request rejection should terminate immediately");
    Require(result.error ==
                "commissioning_canvas_compare_and_swap_conflict",
            "definitive request rejection should preserve its reason");
}

}  // namespace

int main()
{
    try {
        TestExactActiveReleasePublishes();
        TestOldRejectionDoesNotTerminateNewRequest();
        TestMatchingRejectionTerminates();
        TestWorkerConfirmsWithoutBlockingCaller();
        TestWorkerReconcilesLostFinalizeAcknowledgement();
        TestWorkerReportsDefinitiveRequestRejectionImmediately();
        std::cout << "commissioning finalization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "commissioning finalization tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
