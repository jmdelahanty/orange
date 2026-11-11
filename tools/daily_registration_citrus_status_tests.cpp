// tools/daily_registration_citrus_status_tests.cpp
//
// The guided daily registration's reading of Citrus's daily_registration
// status (src/gui/spatial_layout/daily_registration_citrus_status.h): a begin
// Citrus applied, one it refused (status shapes from Citrus
// DailyRegistration::Controller::Begin, which keeps the previous
// operation_id on a refusal), a silent Citrus, and the not-active abort case
// from 2026-11-10.

#include "gui/spatial_layout/daily_registration_citrus_status.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                                       \
    } while (0)

using namespace orange::gui::daily_registration;
using nlohmann::json;

const std::string kTxn = "dailyreg_2026_11_10T20_00_00Z";
const std::string kOp = "begin_000042";

json Status(bool active, const char* transition, const char* operation_id, const char* error,
            const char* transaction_id = nullptr)
{
    json s = {{"active", active}, {"transition", transition}};
    s["operation_id"] = operation_id ? json(operation_id) : json(nullptr);
    s["error"] = error ? json(error) : json(nullptr);
    s["transaction_id"] = transaction_id ? json(transaction_id) : json(nullptr);
    return s;
}

void TestBegin()
{
    const char* stale = "compatible_commissioning_release_required:commissioning_rig_configuration_stale";
    // Still showing the base-only selection that preceded the request.
    auto r = ClassifyBeginWait(Status(false, "base_only_selected", "restore_base_only_000041", nullptr),
                               kTxn, kOp, "base_only_selected", "", 0.2);
    EXPECT(r.outcome == BeginWait::kWaiting, "not applied yet: wait");

    r = ClassifyBeginWait(Status(true, "begun", kOp.c_str(), nullptr, kTxn.c_str()), kTxn, kOp,
                          "base_only_selected", "", 0.3);
    EXPECT(r.outcome == BeginWait::kBegun, "begun with our transaction and operation");

    r = ClassifyBeginWait(Status(true, "begun", "begin_000001", nullptr, kTxn.c_str()), kTxn, kOp,
                          "base_only_selected", "", 0.3);
    EXPECT(r.outcome == BeginWait::kWaiting, "begun with another operation id is not ours");

    // 2026-11-10: refused, previous operation id kept.
    r = ClassifyBeginWait(Status(false, "begin_rejected", "restore_base_only_000041", stale), kTxn, kOp,
                          "base_only_selected", "", 0.3);
    EXPECT(r.outcome == BeginWait::kRefused, "fresh refusal is ours at once");
    EXPECT(r.reason == std::string("citrus_daily_registration_begin_rejected:") + stale,
           "refusal carries Citrus's error");

    // The same refusal was already showing before the request: a leftover
    // until Citrus has had time to apply ours.
    r = ClassifyBeginWait(Status(false, "begin_rejected", "begin_000040", stale), kTxn, kOp,
                          "begin_rejected", stale, 1.0);
    EXPECT(r.outcome == BeginWait::kWaiting, "identical leftover refusal: wait");
    r = ClassifyBeginWait(Status(false, "begin_rejected", "begin_000040", stale), kTxn, kOp,
                          "begin_rejected", stale, kBeginRefusalSettleSeconds);
    EXPECT(r.outcome == BeginWait::kRefused, "identical refusal after the settle time is ours");
    r = ClassifyBeginWait(Status(false, "begin_rejected", "begin_000040", "experiment_active_or_armed:arena_2"),
                          kTxn, kOp, "begin_rejected", stale, 0.4);
    EXPECT(r.outcome == BeginWait::kRefused, "a different refusal error is fresh");

    r = ClassifyBeginWait(Status(false, "begin_rejected", nullptr, nullptr), kTxn, kOp, "idle", "", 0.1);
    EXPECT(r.reason == "citrus_daily_registration_begin_rejected:no_error_given", "refusal without error");

    r = ClassifyBeginWait(Status(false, "base_only_selected", "x", nullptr), kTxn, kOp, "base_only_selected",
                          "", kBeginTimeoutSeconds);
    EXPECT(r.outcome == BeginWait::kTimedOut, "no answer: time out");
    EXPECT(r.reason == "citrus_daily_registration_begin_unacknowledged:transition=base_only_selected",
           "timeout names the last transition");
    r = ClassifyBeginWait(json::object(), kTxn, kOp, "", "", kBeginTimeoutSeconds + 1);
    EXPECT(r.reason == "citrus_daily_registration_begin_unacknowledged:transition=none", "empty status");
}

void TestAbortNotActive()
{
    EXPECT(CitrusHasNoActiveTransaction(Status(false, "begin_rejected", "x", "e"), kTxn), "inactive");
    EXPECT(CitrusHasNoActiveTransaction(Status(true, "begun", "x", nullptr, "dailyreg_other"), kTxn),
           "another transaction is active");
    EXPECT(!CitrusHasNoActiveTransaction(Status(true, "begun", "x", nullptr, kTxn.c_str()), kTxn),
           "ours is active");
    EXPECT(!CitrusHasNoActiveTransaction(json{{"transaction_id", kTxn}}, kTxn),
           "missing active flag with our id: assume active");
    EXPECT(std::string(kAbortTransactionNotActive) == "daily_registration_transaction_not_active",
           "Citrus's error text");
}

}  // namespace

int main()
{
    TestBegin();
    TestAbortNotActive();
    if (g_failures) {
        std::fprintf(stderr, "daily_registration_citrus_status_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("daily_registration_citrus_status_tests: PASS\n");
    return 0;
}
