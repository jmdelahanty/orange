#pragma once

#include <optional>
#include <string>

// Shared environment-variable parsing helpers for GUI-side configuration.
// Consolidated from identical file-local copies in orange.cpp, gui/autorun.cpp
// and gui/recording_finalizer.cpp.

std::string gui_lower_ascii(std::string text);
std::optional<bool> gui_env_flag_value(const char* name);
bool gui_env_flag_enabled(const char* name, bool default_value = false);
int gui_env_int(const char* name, int default_value, int min_value);
