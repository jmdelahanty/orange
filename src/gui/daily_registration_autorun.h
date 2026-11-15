// Unattended daily projection registration (docs/daily_registration_autorun_plan.md).
//
// Everything mechanical runs on its own: Citrus canvas import, the begin
// transaction, the rim capture, the known-radius rim fit, the candidate, the
// automatic geometry QC, accept and select. The one recorded human judgement is a
// single confirmation sent over local control (`confirm_daily_registration`,
// step "rims_and_acceptance"): "these rims follow the inner rim; accept and select
// the resulting translation if Orange's automatic geometry QC passes". It is
// recorded with the socket peer (SO_PEERCRED). The rim observation v3 that follows
// is operator-confirmed because a person did confirm it; nothing sets an operator
// flag without that request.
#pragma once

#include "json.hpp"

#include <deque>
#include <mutex>
#include <string>

namespace orange::gui {

inline constexpr const char* kDailyRegistrationConfirmMethod = "confirm_daily_registration";
inline constexpr const char* kDailyRegistrationConfirmStep = "rims_and_acceptance";
inline constexpr const char* kDailyRegistrationAutorunPolicy = "orange_daily_registration_autorun_rim_only_v1";

struct DailyRegistrationAutorunConfig {
    bool enabled = false;
    std::string citrus_config_path;        // Citrus canvas (shadow.json)
    std::string fish_plane_scale_path;     // orange.calibration.fish_plane_scale v1
    std::string declared_by;               // who declares dishes, water and IR filters ready
    std::string nir_filters;               // "in" (production) or "out" (e.g. a filters-off
                                           // accuracy session); required, recorded as declared
    std::string result_json_path;
    double begin_timeout_seconds = 180.0;
    double confirm_timeout_seconds = 1800.0;
    double workflow_timeout_seconds = 900.0;  // after the confirmation
    bool exit_after_completion = true;
    std::string error;                     // a configuration error refuses the autorun
};

// ORANGE_GUI_DAILY_REGISTRATION_AUTORUN=1 and its _CITRUS_CONFIG_PATH,
// _FISH_PLANE_SCALE_PATH, _DECLARED_BY, _NIR_FILTERS, _RESULT_JSON, _CONFIRM_TIMEOUT_SECONDS,
// _EXIT_AFTER_COMPLETION keys.
DailyRegistrationAutorunConfig resolve_daily_registration_autorun_config();

// Identity pattern shared with the start declaration's declared_by.
bool daily_registration_identity_valid(const std::string& identity);

// The confirmation as it arrived over local control.
struct DailyRegistrationConfirmation {
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string received_at_utc;
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json peer = nullptr;
};

// Hands confirmations from the local-control drain to the autorun (both on the GUI
// thread today; the mutex keeps it safe if that changes) and publishes the autorun
// state for status.local_control.daily_registration_autorun.
class DailyRegistrationAutorunMailbox {
public:
    void Post(DailyRegistrationConfirmation confirmation);
    bool Take(DailyRegistrationConfirmation* out);
    void SetStatus(nlohmann::json status);
    nlohmann::json StatusJson() const;

private:
    mutable std::mutex mutex_;
    std::deque<DailyRegistrationConfirmation> queue_;
    nlohmann::json status_ = nullptr;
};

DailyRegistrationAutorunMailbox& GuiDailyRegistrationAutorun();

}  // namespace orange::gui
