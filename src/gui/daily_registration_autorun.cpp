#include "gui/daily_registration_autorun.h"

#include "gui/env_util.h"

#include <cmath>
#include <cstdlib>
#include <regex>

namespace orange::gui {
namespace {

std::string env_string(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

bool env_seconds(const char* name, double* value_out, std::string* error_out)
{
    const std::string text = env_string(name);
    if (text.empty()) return true;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value) || value <= 0.0) {
        *error_out = std::string(name) + " must be a positive number of seconds";
        return false;
    }
    *value_out = value;
    return true;
}

}  // namespace

bool daily_registration_identity_valid(const std::string& identity)
{
    static const std::regex pattern("^[a-z][a-z0-9_-]{0,31}$");
    return std::regex_match(identity, pattern);
}

DailyRegistrationAutorunConfig resolve_daily_registration_autorun_config()
{
    DailyRegistrationAutorunConfig config;
    config.enabled = gui_env_flag_enabled("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN");
    if (!config.enabled) return config;
    config.citrus_config_path = env_string("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CITRUS_CONFIG_PATH");
    config.fish_plane_scale_path = env_string("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_FISH_PLANE_SCALE_PATH");
    config.declared_by = env_string("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_DECLARED_BY");
    config.result_json_path = env_string("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_RESULT_JSON");
    config.exit_after_completion =
        gui_env_flag_enabled("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_EXIT_AFTER_COMPLETION", true);
    std::string error;
    if (!env_seconds("ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CONFIRM_TIMEOUT_SECONDS",
                     &config.confirm_timeout_seconds, &error)) {
        config.error = error;
    } else if (config.citrus_config_path.empty()) {
        config.error = "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_CITRUS_CONFIG_PATH is required";
    } else if (config.fish_plane_scale_path.empty()) {
        config.error = "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_FISH_PLANE_SCALE_PATH is required";
    } else if (!daily_registration_identity_valid(config.declared_by)) {
        config.error = "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_DECLARED_BY must match ^[a-z][a-z0-9_-]{0,31}$";
    } else if (config.result_json_path.empty()) {
        config.error = "ORANGE_GUI_DAILY_REGISTRATION_AUTORUN_RESULT_JSON is required";
    }
    return config;
}

void DailyRegistrationAutorunMailbox::Post(DailyRegistrationConfirmation confirmation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(confirmation));
}

bool DailyRegistrationAutorunMailbox::Take(DailyRegistrationConfirmation* out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    *out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void DailyRegistrationAutorunMailbox::SetStatus(nlohmann::json status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(status);
}

nlohmann::json DailyRegistrationAutorunMailbox::StatusJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

DailyRegistrationAutorunMailbox& GuiDailyRegistrationAutorun()
{
    static DailyRegistrationAutorunMailbox mailbox;
    return mailbox;
}

}  // namespace orange::gui
