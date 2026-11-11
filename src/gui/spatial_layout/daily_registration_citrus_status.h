#pragma once

// Decisions the guided daily registration takes from Citrus's
// daily_registration status, kept free of the socket client so they can be
// tested on recorded status shapes.
//
// Citrus queues begin_daily_registration and applies it on its GUI thread
// (src/main.cpp, DailyRegistration::Controller::Begin). A refusal there sets
// transition "begin_rejected" and the error but keeps the previous
// operation_id, so it cannot be matched to the request by id. 2026-11-10: a
// refused begin (compatible_commissioning_release_required:
// commissioning_rig_configuration_stale) left Orange at "Acquiring the Citrus
// daily-registration lease" forever.

#include "json.hpp"

#include <string>

namespace orange::gui::daily_registration {

// A begin_rejected that matches the status seen before the request may be a
// leftover; it counts as this request's refusal once Citrus had this long to
// apply the request (it applies commands within a GUI frame).
constexpr double kBeginRefusalSettleSeconds = 5.0;
// No begun and no refusal within this long: stop waiting.
constexpr double kBeginTimeoutSeconds = 20.0;

enum class BeginWait { kWaiting, kBegun, kRefused, kTimedOut };

struct BeginWaitResult {
    BeginWait outcome = BeginWait::kWaiting;
    std::string reason;
};

inline std::string StatusString(const nlohmann::json& value, const char* key)
{
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline BeginWaitResult ClassifyBeginWait(
    const nlohmann::json& daily,
    const std::string& transaction_id,
    const std::string& operation_id,
    const std::string& prior_transition,
    const std::string& prior_error,
    double seconds_since_request)
{
    const bool active = daily.value("active", false);
    const std::string transition = StatusString(daily, "transition");
    const std::string error = StatusString(daily, "error");
    if (active && transition == "begun" &&
        StatusString(daily, "transaction_id") == transaction_id &&
        StatusString(daily, "operation_id") == operation_id) {
        return {BeginWait::kBegun, {}};
    }
    if (!active && transition == "begin_rejected") {
        const bool fresh = prior_transition != "begin_rejected" || error != prior_error;
        if (fresh || seconds_since_request >= kBeginRefusalSettleSeconds) {
            return {BeginWait::kRefused,
                    "citrus_daily_registration_begin_rejected:" +
                        (error.empty() ? std::string("no_error_given") : error)};
        }
    }
    if (seconds_since_request >= kBeginTimeoutSeconds) {
        return {BeginWait::kTimedOut,
                "citrus_daily_registration_begin_unacknowledged:transition=" +
                    (transition.empty() ? std::string("none") : transition) +
                    (error.empty() ? std::string() : ",error=" + error)};
    }
    return {BeginWait::kWaiting, {}};
}

// Citrus refused an abort with daily_registration_transaction_not_active. The
// transaction is over on the Citrus side when its status shows no active
// transaction, or an active one that is not ours.
inline bool CitrusHasNoActiveTransaction(const nlohmann::json& daily, const std::string& transaction_id)
{
    return !daily.value("active", true) || StatusString(daily, "transaction_id") != transaction_id;
}

constexpr const char* kAbortTransactionNotActive = "daily_registration_transaction_not_active";

}  // namespace orange::gui::daily_registration
