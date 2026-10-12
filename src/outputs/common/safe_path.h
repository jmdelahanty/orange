#pragma once

// One rule for paths stored in outputs and one for "is this path inside that root".

#include <filesystem>
#include <string>

namespace orange::outputs {

// A relative path as stored in an output (e.g. "external_recorder/Cam1_meta.csv"): non-empty,
// not absolute, no empty, "." or ".." components, and already in normal form, so one file has
// exactly one spelling ("a//b", "a/./b", "a/" and "." are refused).
inline bool is_safe_relative_path(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return path.lexically_normal().generic_string() == value;
}

// The path with symlinks resolved as far as it exists (weakly_canonical). If that fails (e.g. a
// permission error on a parent), the absolute, lexically normal path.
inline std::filesystem::path resolved_path(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path value = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return value;
    }
    value = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : value.lexically_normal();
}

// True when `child`, after resolving symlinks, is `root` or lies below it (component-wise, so
// "/data/rec10" is not inside "/data/rec1").
inline bool is_inside(const std::filesystem::path& child, const std::filesystem::path& root) {
    const std::filesystem::path resolved_child = resolved_path(child);
    const std::filesystem::path resolved_root = resolved_path(root);
    auto child_it = resolved_child.begin();
    for (auto root_it = resolved_root.begin(); root_it != resolved_root.end(); ++root_it, ++child_it) {
        if (root_it->empty() && std::next(root_it) == resolved_root.end()) {
            break;  // trailing separator of the root
        }
        if (child_it == resolved_child.end() || *child_it != *root_it) {
            return false;
        }
    }
    return true;
}

}  // namespace orange::outputs
