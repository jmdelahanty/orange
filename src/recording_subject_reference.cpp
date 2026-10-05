#include "recording_subject_reference.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace orange::recording {
namespace {

using json = nlohmann::json;

void require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

// ---------------------------------------------------------------- HTTP GET --

struct ParsedUrl {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

bool parse_http_url(const std::string& url, ParsedUrl* out, std::string* error)
{
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        *error = "only http:// URLs are supported: " + url;
        return false;
    }
    std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out->path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out->host = authority.substr(0, colon);
        out->port = authority.substr(colon + 1);
    } else {
        out->host = authority;
    }
    if (out->host.empty()) {
        *error = "URL has no host: " + url;
        return false;
    }
    return true;
}

long long ms_left(const std::chrono::steady_clock::time_point& deadline)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
}

bool wait_fd(int fd, short events, const std::chrono::steady_clock::time_point& deadline)
{
    const long long left = ms_left(deadline);
    if (left <= 0) return false;
    pollfd p{};
    p.fd = fd;
    p.events = events;
    const int rc = ::poll(&p, 1, static_cast<int>(std::min<long long>(left, 1 << 30)));
    return rc > 0 && (p.revents & (events | POLLERR | POLLHUP)) != 0;
}

// Decode a chunked body (RFC 7230 4.1); returns false if malformed.
bool dechunk(const std::string& in, std::string* out)
{
    std::size_t pos = 0;
    while (pos < in.size()) {
        const auto line_end = in.find("\r\n", pos);
        if (line_end == std::string::npos) return false;
        const std::string size_hex = in.substr(pos, line_end - pos);
        std::size_t semi = size_hex.find(';');
        const std::string digits = semi == std::string::npos ? size_hex : size_hex.substr(0, semi);
        std::size_t chunk = 0;
        try { chunk = std::stoul(digits, nullptr, 16); } catch (...) { return false; }
        pos = line_end + 2;
        if (chunk == 0) return true;
        if (pos + chunk > in.size()) return false;
        out->append(in, pos, chunk);
        pos += chunk + 2;  // trailing CRLF
    }
    return true;
}

}  // namespace

HttpGetResult HttpGet(const std::string& url, const int timeout_ms)
{
    HttpGetResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 1));
    ParsedUrl parsed;
    if (!parse_http_url(url, &parsed, &result.error)) return result;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int gai = ::getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &addresses);
    if (gai != 0 || addresses == nullptr) {
        result.error = "resolve failed for " + parsed.host + ": " + ::gai_strerror(gai);
        return result;
    }
    int fd = -1;
    std::string connect_error = "no address connected";
    for (addrinfo* ai = addresses; ai != nullptr && fd < 0; ai = ai->ai_next) {
        const int candidate = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (candidate < 0) continue;
        int rc = ::connect(candidate, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            connect_error = std::string("connect failed: ") + std::strerror(errno);
            ::close(candidate);
            continue;
        }
        if (rc != 0) {
            if (!wait_fd(candidate, POLLOUT, deadline)) {
                connect_error = "connect timed out";
                ::close(candidate);
                continue;
            }
            int so_error = 0;
            socklen_t len = sizeof so_error;
            if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
                connect_error = std::string("connect failed: ") + std::strerror(so_error ? so_error : errno);
                ::close(candidate);
                continue;
            }
        }
        fd = candidate;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) {
        result.error = connect_error;
        return result;
    }

    const std::string request =
        "GET " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host +
        (parsed.port == "80" ? "" : ":" + parsed.port) +
        "\r\nAccept: application/json\r\nUser-Agent: orange-subject-reference/1\r\nConnection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        if (!wait_fd(fd, POLLOUT, deadline)) { result.error = "send timed out"; ::close(fd); return result; }
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            result.error = std::string("send failed: ") + std::strerror(errno);
            ::close(fd);
            return result;
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buffer[16384];
    const std::size_t max_bytes = 4u << 20;
    while (true) {
        if (!wait_fd(fd, POLLIN, deadline)) { result.error = "receive timed out"; ::close(fd); return result; }
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            result.error = std::string("receive failed: ") + std::strerror(errno);
            ::close(fd);
            return result;
        }
        if (n == 0) break;
        raw.append(buffer, static_cast<std::size_t>(n));
        if (raw.size() > max_bytes) { result.error = "response exceeds 4 MiB"; ::close(fd); return result; }
        // Stop early when Content-Length is satisfied (servers honour
        // Connection: close, but do not depend on it).
        const auto header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            std::string headers_lower = raw.substr(0, header_end);
            std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);
            const auto cl = headers_lower.find("content-length:");
            if (cl != std::string::npos) {
                try {
                    const std::size_t length = std::stoul(headers_lower.substr(cl + 15));
                    if (raw.size() - (header_end + 4) >= length) break;
                } catch (...) {}
            }
        }
    }
    ::close(fd);

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
        result.error = "malformed HTTP response";
        return result;
    }
    const std::string status_line = raw.substr(0, raw.find("\r\n"));
    const auto first_space = status_line.find(' ');
    if (first_space == std::string::npos || status_line.size() < first_space + 4) {
        result.error = "malformed HTTP status line";
        return result;
    }
    try { result.status = std::stoi(status_line.substr(first_space + 1, 3)); }
    catch (...) { result.error = "malformed HTTP status code"; return result; }
    std::string headers_lower = raw.substr(0, header_end);
    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);
    const std::string body = raw.substr(header_end + 4);
    if (headers_lower.find("transfer-encoding: chunked") != std::string::npos) {
        if (!dechunk(body, &result.body)) { result.error = "malformed chunked body"; return result; }
    } else {
        result.body = body;
    }
    result.transport_ok = true;
    return result;
}

// ------------------------------------------------------------- configs -----

ZebrobotLookupConfig ZebrobotLookupConfig::Parse(const json& config)
{
    ZebrobotLookupConfig c;
    require(config.is_object(), "zebrobot config must be an object");
    for (auto it = config.begin(); it != config.end(); ++it) {
        require(it.key() == "base_url" || it.key() == "timeout_ms", "unknown zebrobot config field: " + it.key());
    }
    if (config.contains("base_url")) {
        require(config.at("base_url").is_string(), "zebrobot.base_url must be a string");
        c.base_url = config.at("base_url").get<std::string>();
        while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();
        require(c.base_url.empty() || c.base_url.rfind("http://", 0) == 0,
                "zebrobot.base_url must start with http:// (TLS is not supported by Orange's minimal client)");
    }
    if (config.contains("timeout_ms")) {
        require(config.at("timeout_ms").is_number_integer() && config.at("timeout_ms").get<int>() >= 100 &&
                    config.at("timeout_ms").get<int>() <= 10000,
                "zebrobot.timeout_ms must be an integer in [100, 10000]");
        c.timeout_ms = config.at("timeout_ms").get<int>();
    }
    return c;
}

json ZebrobotLookupConfig::ToJson() const
{
    return {{"base_url", base_url}, {"timeout_ms", timeout_ms}};
}

namespace {
SubjectDeclaration parse_declaration(const json& entry, const std::string& what)
{
    require(entry.is_object(), what + " must be an object");
    for (auto it = entry.begin(); it != entry.end(); ++it) {
        require(it.key() == "dish_id" || it.key() == "subject_count", "unknown " + what + " field: " + it.key());
    }
    SubjectDeclaration d;
    if (entry.contains("dish_id")) {
        require(entry.at("dish_id").is_string(), what + " dish_id must be a string (omit the key for no dish)");
        const std::string id = entry.at("dish_id").get<std::string>();
        require(!id.empty(), what + " dish_id must not be empty (omit the key instead)");
        require(id.size() <= 256, what + " dish_id exceeds 256 bytes");
        for (unsigned char ch : id) {
            require(ch > 0x20 && ch != 0x7f && ch != '/' && ch != '?' && ch != '#',
                    what + " dish_id contains whitespace, control or URL-delimiter characters");
        }
        d.dish_id = id;
    }
    if (entry.contains("subject_count")) {
        require(entry.at("subject_count").is_number_integer() && !entry.at("subject_count").is_boolean() &&
                    entry.at("subject_count").get<long long>() >= 1 && entry.at("subject_count").get<long long>() <= 100000,
                what + " subject_count must be an integer >= 1 (omit the key when not declared)");
        d.subject_count = entry.at("subject_count").get<int>();
    }
    require(!d.empty(), what + " must declare a dish_id and/or a subject_count (omit the entry instead)");
    return d;
}
json declaration_json(const SubjectDeclaration& d)
{
    json j = json::object();
    if (d.dish_id) j["dish_id"] = *d.dish_id;
    if (d.subject_count) j["subject_count"] = *d.subject_count;
    return j;
}
}  // namespace

SubjectReferencesConfig SubjectReferencesConfig::Parse(const json& config)
{
    SubjectReferencesConfig c;
    require(config.is_object(), "subject_references config must be an object");
    for (auto it = config.begin(); it != config.end(); ++it) {
        require(it.key() == "schema_version" || it.key() == "default" || it.key() == "cameras",
                "unknown subject_references config field: " + it.key());
    }
    require(config.contains("schema_version") && config.at("schema_version").is_number_integer() &&
                config.at("schema_version") == 1,
            "subject_references config schema_version must be 1");
    if (config.contains("default")) {
        require(!config.at("default").is_null(), "subject_references default must be an object or absent, not null");
        c.default_declaration = parse_declaration(config.at("default"), "subject_references default");
    }
    if (config.contains("cameras")) {
        require(config.at("cameras").is_object(), "subject_references cameras must be an object keyed by serial");
        for (auto it = config.at("cameras").begin(); it != config.at("cameras").end(); ++it) {
            require(!it.key().empty(), "subject_references cameras has an empty serial key");
            c.cameras[it.key()] = parse_declaration(it.value(), "subject_references camera " + it.key());
        }
    }
    return c;
}

json SubjectReferencesConfig::ToJson() const
{
    json j = {{"schema_version", 1}};
    if (default_declaration) j["default"] = declaration_json(*default_declaration);
    if (!cameras.empty()) {
        json cams = json::object();
        for (const auto& [serial, d] : cameras) cams[serial] = declaration_json(d);
        j["cameras"] = cams;
    }
    return j;
}

std::map<std::string, SubjectDeclaration> SubjectReferencesConfig::Resolve(
    const std::vector<std::string>& recording_serials) const
{
    std::map<std::string, SubjectDeclaration> out;
    for (const auto& serial : recording_serials) {
        SubjectDeclaration d;
        if (default_declaration) d = *default_declaration;
        const auto it = cameras.find(serial);
        if (it != cameras.end()) {  // field-wise override
            if (it->second.dish_id) d.dish_id = it->second.dish_id;
            if (it->second.subject_count) d.subject_count = it->second.subject_count;
        }
        out[serial] = d;
    }
    return out;
}

// ------------------------------------------------------------- building ----

namespace {

json transport_error(const std::string& message)
{
    return {{"kind", "transport"}, {"detail_error", nullptr}, {"message", message}};
}

json http_error(const HttpGetResult& r)
{
    json detail = nullptr;
    const json body = json::parse(r.body, nullptr, false);
    if (body.is_object() && body.contains("detail") && body.at("detail").is_object() &&
        body.at("detail").contains("error") && body.at("detail").at("error").is_string()) {
        detail = body.at("detail").at("error");
    }
    return {{"kind", "http"}, {"detail_error", detail},
            {"message", "HTTP " + std::to_string(r.status) + (detail.is_string() ? ": " + detail.get<std::string>() : "")}};
}

// Copy a served scalar as served; missing or wrong-typed is a contract failure
// reported as an http error, never substituted.
bool copy_string(const json& src, const char* key, json* dst, std::string* missing)
{
    if (!src.contains(key) || !src.at(key).is_string()) { *missing = key; return false; }
    (*dst)[key] = src.at(key);
    return true;
}
bool copy_int(const json& src, const char* key, json* dst, std::string* missing)
{
    if (!src.contains(key) || !src.at(key).is_number_integer()) { *missing = key; return false; }
    (*dst)[key] = src.at(key);
    return true;
}

}  // namespace

json NotCollectedSubjectReference(const std::string& reason, const std::optional<int>& subject_count)
{
    require(!reason.empty(), "not_collected needs a reason");
    require(!subject_count || *subject_count >= 1, "subject_count must be >= 1 when declared");
    return {{"schema_id", kSubjectReferenceSchemaId}, {"schema_version", kSubjectReferenceSchemaVersion},
            {"status", "not_collected"}, {"reason", reason}, {"zebrobot", nullptr}, {"dish", nullptr},
            {"dish_fish", json::array()}, {"dish_fish_lookup", nullptr},
            {"subject_count", subject_count ? json(*subject_count) : json(nullptr)}};
}

json BuildSubjectReference(const SubjectDeclaration& declaration,
                           const ZebrobotLookupConfig& zebrobot,
                           const std::string& queried_at_utc,
                           const HttpGetFn& http_get)
{
    require(!declaration.subject_count || *declaration.subject_count >= 1, "subject_count must be >= 1 when declared");
    const json subject_count = declaration.subject_count ? json(*declaration.subject_count) : json(nullptr);
    if (!declaration.dish_id) return NotCollectedSubjectReference("no_dish_declared", declaration.subject_count);
    const std::string& dish_id_value = *declaration.dish_id;
    const std::optional<std::string> dish_id = dish_id_value;
    const std::string endpoint = "/dishes/" + *dish_id + "/citrus-snapshot";
    const std::string fish_endpoint = "/dishes/" + *dish_id + "/fish";
    json entry = {{"schema_id", kSubjectReferenceSchemaId}, {"schema_version", kSubjectReferenceSchemaVersion},
                  {"status", "lookup_failed"}, {"reason", ""}, {"subject_count", subject_count},
                  {"zebrobot", {{"base_url", zebrobot.base_url}, {"endpoint", endpoint}, {"fish_endpoint", fish_endpoint},
                                {"api_schema_version", kZebrobotApiSchemaVersion}, {"queried_at_utc", queried_at_utc},
                                {"http_status", nullptr}, {"error", nullptr}}},
                  {"dish", nullptr}, {"dish_fish", json::array()},
                  {"dish_fish_lookup", {{"status", "not_attempted"}, {"http_status", nullptr}, {"error", nullptr}}}};
    auto fail = [&](const json& error, const std::string& reason) {
        entry["zebrobot"]["error"] = error;
        entry["reason"] = reason;
        return entry;
    };
    if (!zebrobot.configured()) {
        return fail(transport_error("zebrobot base_url is not configured"), "zebrobot_not_configured");
    }

    // Dish identity.
    const HttpGetResult dish = http_get(zebrobot.base_url + endpoint, zebrobot.timeout_ms);
    if (!dish.transport_ok) return fail(transport_error(dish.error), "dish_lookup_transport_failure");
    entry["zebrobot"]["http_status"] = dish.status;
    if (dish.status != 200) return fail(http_error(dish), "dish_lookup_http_" + std::to_string(dish.status));
    const json body = json::parse(dish.body, nullptr, false);
    if (!body.is_object()) return fail({{"kind", "http"}, {"detail_error", nullptr}, {"message", "dish response is not a JSON object"}},
                                       "dish_lookup_invalid_body");
    if (!body.contains("schema_version") || body.at("schema_version") != kZebrobotApiSchemaVersion) {
        return fail({{"kind", "http"}, {"detail_error", nullptr},
                     {"message", "dish response schema_version is not " + std::to_string(kZebrobotApiSchemaVersion)}},
                    "dish_lookup_api_schema_mismatch");
    }
    json dish_json = json::object();
    std::string missing;
    if (!copy_string(body, "dish_id", &dish_json, &missing) || !copy_string(body, "dish_uuid", &dish_json, &missing) ||
        !copy_int(body, "revision", &dish_json, &missing) || !copy_string(body, "updated_at", &dish_json, &missing)) {
        return fail({{"kind", "http"}, {"detail_error", nullptr}, {"message", "dish response lacks " + missing}},
                    "dish_lookup_missing_" + missing);
    }
    if (dish_json.at("dish_id") != *dish_id) {
        return fail({{"kind", "http"}, {"detail_error", nullptr},
                     {"message", "dish response dish_id differs from the declared dish_id"}},
                    "dish_lookup_identity_mismatch");
    }
    entry["dish"] = dish_json;
    entry["status"] = "collected";

    // Fish subjects registered to the dish (as served; Orange selects nothing).
    const HttpGetResult fish = http_get(zebrobot.base_url + fish_endpoint, zebrobot.timeout_ms);
    json& fish_lookup = entry["dish_fish_lookup"];
    if (!fish.transport_ok) {
        fish_lookup = {{"status", "failed"}, {"http_status", nullptr}, {"error", transport_error(fish.error)}};
        return entry;
    }
    fish_lookup["http_status"] = fish.status;
    if (fish.status != 200) {
        fish_lookup = {{"status", "failed"}, {"http_status", fish.status}, {"error", http_error(fish)}};
        return entry;
    }
    const json fish_body = json::parse(fish.body, nullptr, false);
    if (!fish_body.is_object() || !fish_body.contains("items") || !fish_body.at("items").is_array()) {
        fish_lookup = {{"status", "failed"}, {"http_status", fish.status},
                       {"error", {{"kind", "http"}, {"detail_error", nullptr}, {"message", "fish response lacks an items array"}}}};
        return entry;
    }
    json dish_fish = json::array();
    for (const auto& item : fish_body.at("items")) {
        json row = json::object();
        if (!item.is_object() || !copy_string(item, "fish_id", &row, &missing) || !copy_int(item, "revision", &row, &missing) ||
            !copy_string(item, "updated_at", &row, &missing)) {
            fish_lookup = {{"status", "failed"}, {"http_status", fish.status},
                           {"error", {{"kind", "http"}, {"detail_error", nullptr}, {"message", "fish item lacks " + missing}}}};
            return entry;
        }
        dish_fish.push_back(row);
    }
    entry["dish_fish"] = dish_fish;
    fish_lookup = {{"status", "complete"}, {"http_status", fish.status}, {"error", nullptr}};
    return entry;
}

json BuildSubjectReferences(const std::map<std::string, SubjectDeclaration>& declarations,
                            const ZebrobotLookupConfig& zebrobot,
                            const std::string& queried_at_utc,
                            const HttpGetFn& http_get)
{
    json block = json::object();
    for (const auto& [serial, declaration] : declarations) {
        require(!serial.empty(), "subject references: empty camera serial");
        block[serial] = BuildSubjectReference(declaration, zebrobot, queried_at_utc, http_get);
    }
    ValidateEmittedSubjectReferences(block);
    return block;
}

// ---------------------------------------------------------- validation -----

namespace {

void require_keys(const json& j, const std::set<std::string>& keys, const std::string& what)
{
    require(j.is_object(), what + " must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) require(keys.count(it.key()) != 0, "unknown " + what + " field: " + it.key());
    for (const auto& k : keys) require(j.contains(k), what + " is missing " + k);
}

void validate_error(const json& e, const std::string& what)
{
    require_keys(e, {"kind", "detail_error", "message"}, what);
    require(e.at("kind") == "transport" || e.at("kind") == "http", what + " kind must be transport or http");
    require(e.at("detail_error").is_null() || e.at("detail_error").is_string(), what + " detail_error must be null or a string");
    require(e.at("message").is_string() && !e.at("message").get<std::string>().empty(), what + " needs a message");
}

void validate_entry(const std::string& serial, const json& e)
{
    const std::string what = "subject_references[" + serial + "]";
    require_keys(e, {"schema_id", "schema_version", "status", "reason", "zebrobot", "dish", "dish_fish", "dish_fish_lookup",
                     "subject_count"}, what);
    require(e.at("subject_count").is_null() ||
                (e.at("subject_count").is_number_integer() && !e.at("subject_count").is_boolean() &&
                 e.at("subject_count").get<long long>() >= 1),
            what + " subject_count must be null or an integer >= 1");
    require(e.at("schema_id") == kSubjectReferenceSchemaId, what + " schema_id");
    require(e.at("schema_version").is_number_integer() && e.at("schema_version") == kSubjectReferenceSchemaVersion,
            what + " schema_version must be 1");
    const std::string status = e.at("status").is_string() ? e.at("status").get<std::string>() : "";
    require(status == "collected" || status == "not_collected" || status == "lookup_failed", what + " status is invalid");
    require(e.at("reason").is_string(), what + " reason must be a string");
    const std::string reason = e.at("reason").get<std::string>();
    require(e.at("dish_fish").is_array(), what + " dish_fish must be an array");
    for (const auto& f : e.at("dish_fish")) {
        require_keys(f, {"fish_id", "revision", "updated_at"}, what + " dish_fish item");
        require(f.at("fish_id").is_string() && f.at("revision").is_number_integer() && f.at("updated_at").is_string(),
                what + " dish_fish item types");
    }
    if (status == "not_collected") {
        require(!reason.empty(), what + " not_collected needs a reason");
        require(e.at("zebrobot").is_null() && e.at("dish").is_null() && e.at("dish_fish").empty() && e.at("dish_fish_lookup").is_null(),
                what + " not_collected must carry no zebrobot, dish, dish_fish or dish_fish_lookup");
        return;
    }
    const json& z = e.at("zebrobot");
    require_keys(z, {"base_url", "endpoint", "fish_endpoint", "api_schema_version", "queried_at_utc", "http_status", "error"},
                 what + " zebrobot");
    require(z.at("base_url").is_string() && z.at("endpoint").is_string() && z.at("fish_endpoint").is_string() &&
                z.at("queried_at_utc").is_string(),
            what + " zebrobot string fields");
    require(z.at("api_schema_version").is_number_integer() && z.at("api_schema_version") == kZebrobotApiSchemaVersion,
            what + " zebrobot api_schema_version must be 2");
    require(z.at("http_status").is_null() || z.at("http_status").is_number_integer(), what + " zebrobot http_status");
    const json& fl = e.at("dish_fish_lookup");
    require_keys(fl, {"status", "http_status", "error"}, what + " dish_fish_lookup");
    require(fl.at("status") == "complete" || fl.at("status") == "failed" || fl.at("status") == "not_attempted",
            what + " dish_fish_lookup status");
    require(fl.at("http_status").is_null() || fl.at("http_status").is_number_integer(), what + " dish_fish_lookup http_status");
    if (fl.at("status") == "failed") validate_error(fl.at("error"), what + " dish_fish_lookup error");
    else require(fl.at("error").is_null(), what + " dish_fish_lookup error must be null unless failed");
    if (fl.at("status") != "complete") require(e.at("dish_fish").empty(), what + " dish_fish must be empty unless the fish lookup completed");
    if (status == "collected") {
        require(reason.empty(), what + " collected must have an empty reason");
        require(z.at("error").is_null(), what + " collected must have no zebrobot error");
        require(z.at("http_status") == 200, what + " collected requires http_status 200");
        require_keys(e.at("dish"), {"dish_id", "dish_uuid", "revision", "updated_at"}, what + " dish");
        const json& d = e.at("dish");
        require(d.at("dish_id").is_string() && d.at("dish_uuid").is_string() && d.at("revision").is_number_integer() &&
                    d.at("updated_at").is_string(),
                what + " dish field types");
        require(!d.at("dish_id").get<std::string>().empty() && !d.at("dish_uuid").get<std::string>().empty(),
                what + " dish identifiers must not be empty");
    } else {  // lookup_failed
        require(!reason.empty(), what + " lookup_failed needs a reason");
        validate_error(z.at("error"), what + " zebrobot error");
        require(e.at("dish").is_null(), what + " lookup_failed must carry no dish");
        require(e.at("dish_fish").empty() && fl.at("status") == "not_attempted", what + " lookup_failed must not carry fish results");
    }
}

json read_json_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    require(static_cast<bool>(in), "cannot read " + path.string());
    return json::parse(in);
}

}  // namespace

void ValidateEmittedSubjectReferences(const json& block)
{
    require(block.is_object() && !block.empty(), "subject_references must be a non-empty object keyed by camera serial");
    for (auto it = block.begin(); it != block.end(); ++it) {
        require(!it.key().empty(), "subject_references has an empty camera serial key");
        validate_entry(it.key(), it.value());
    }
}

std::optional<json> ReadFrozenSubjectReferences(const std::string& recording_folder, std::string* error_out)
{
    try {
        const std::filesystem::path root(recording_folder);
        const auto sealed = root / "recording_snapshot_start.json";
        const auto mutable_path = root / "recording_snapshot.json";
        const auto path = std::filesystem::exists(sealed) ? sealed : mutable_path;
        if (!std::filesystem::exists(path)) return std::nullopt;
        const json snapshot = read_json_file(path);
        if (!snapshot.is_object() || !snapshot.contains("session") || !snapshot.at("session").is_object() ||
            !snapshot.at("session").contains("subject_references")) {
            return std::nullopt;
        }
        const json block = snapshot.at("session").at("subject_references");
        ValidateEmittedSubjectReferences(block);
        return block;
    } catch (const std::exception& ex) {
        if (error_out) *error_out = std::string("frozen subject_references: ") + ex.what();
        return std::nullopt;
    }
}

void ApplySubjectReferencesGate(const std::string& recording_folder, json* manifest)
{
    require(manifest != nullptr && manifest->is_object(), "subject references gate needs a manifest object");
    if (!manifest->contains("cameras") || !manifest->at("cameras").is_array()) return;  // clip manifests
    std::string error;
    const auto frozen = ReadFrozenSubjectReferences(recording_folder, &error);
    require(error.empty(), error);
    if (!frozen) {
        require(!manifest->contains("subject_references"),
                "manifest carries subject_references but the recording start snapshot froze none");
        return;
    }
    std::set<std::string> cameras;
    for (const auto& serial : manifest->at("cameras")) {
        require(serial.is_string(), "manifest cameras must be serial strings");
        cameras.insert(serial.get<std::string>());
    }
    std::set<std::string> frozen_serials;
    for (auto it = frozen->begin(); it != frozen->end(); ++it) frozen_serials.insert(it.key());
    require(cameras == frozen_serials, "subject_references membership does not equal the manifest camera set");
    if (manifest->contains("subject_references")) {
        require(manifest->at("subject_references") == *frozen,
                "manifest subject_references differs from the block frozen in the recording start snapshot");
    }
    (*manifest)["subject_references"] = *frozen;
}

}  // namespace orange::recording
