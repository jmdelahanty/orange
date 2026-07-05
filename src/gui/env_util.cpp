#include "env_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

std::string gui_lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::optional<bool> gui_env_flag_value(const char* name)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return std::nullopt;
    }
    const std::string value = gui_lower_ascii(raw);
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    std::cerr << "[GUI][autorun] Ignoring invalid " << name << "='"
              << raw << "'" << std::endl;
    return std::nullopt;
}

bool gui_env_flag_enabled(const char* name, const bool default_value)
{
    if (const std::optional<bool> value = gui_env_flag_value(name)) {
        return *value;
    }
    return default_value;
}

int gui_env_int(const char* name, const int default_value, const int min_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::cerr << "[GUI][autorun] Ignoring invalid " << name << "='"
                  << raw << "'" << std::endl;
        return default_value;
    }
    if (parsed < min_value) {
        std::cerr << "[GUI][autorun] Raising " << name << "=" << parsed
                  << " to minimum " << min_value << std::endl;
        return min_value;
    }
    return static_cast<int>(parsed);
}
