#pragma once

// One SHA-256 and one canonical-JSON digest for every Orange output.
//
// Two textual forms exist in published outputs and are kept apart by name: sha256_hex() is the
// bare 64-character hex string, sha256_token() is "sha256:<hex>". Which one an output writes is
// part of its schema; new outputs use tokens.

#include "gui/spatial_layout/sha256.h"
#include "json.hpp"

#include <filesystem>
#include <string>

namespace orange::outputs {

inline constexpr const char* kSha256TokenPrefix = "sha256:";

// Identifier recorded next to canonical digests (canonicalization fields in published outputs).
inline constexpr const char* kCanonicalJsonId = "canonical_json_utf8_sort_keys_compact_v1";

inline std::string sha256_hex(const std::string& bytes) {
    return orange::gui::spatial_layout::checksum::sha256_hex(bytes);
}

inline std::string sha256_token(const std::string& bytes) {
    return kSha256TokenPrefix + sha256_hex(bytes);
}

// Streams the whole file. Returns false (with error) if it cannot be read.
// (The underlying spatial_layout::checksum::file_sha256 returns the token form.)
inline bool file_sha256_token(const std::filesystem::path& path, std::string* token, std::string* error = nullptr) {
    return orange::gui::spatial_layout::checksum::file_sha256(path, token, error);
}

inline bool file_sha256_hex(const std::filesystem::path& path, std::string* hex, std::string* error = nullptr) {
    std::string token;
    if (!file_sha256_token(path, &token, error)) {
        return false;
    }
    *hex = token.substr(std::char_traits<char>::length(kSha256TokenPrefix));
    return true;
}

// 64 lowercase hex digits.
inline bool is_sha256_hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

// "sha256:" followed by 64 lowercase hex digits.
inline bool is_sha256_token(const std::string& value) {
    return value.size() == 71 && value.compare(0, 7, kSha256TokenPrefix) == 0 &&
           is_sha256_hex(value.substr(7));
}

// Canonical JSON text (kCanonicalJsonId): compact, keys sorted (nlohmann objects are ordered),
// UTF-8 without escaping, invalid UTF-8 rejected (throws nlohmann::json::type_error).
inline std::string canonical_json_text(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

inline std::string canonical_json_sha256_token(const nlohmann::json& value) {
    return sha256_token(canonical_json_text(value));
}

}  // namespace orange::outputs
