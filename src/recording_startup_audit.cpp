#include "recording_startup_audit.h"
#include "timing/clock.h"
#include "fsuid_guard.h"
#include "timing/log_writer.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>

namespace orange {

namespace {

uint64_t steady_ns()
{
    return orange::timing::monotonic_now_ns();
}

uint64_t realtime_ns()
{
    return orange::timing::realtime_now_ns();
}

std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

constexpr size_t kMaxRecords = 200000;

}  // namespace

struct RecordingStartupAudit::Impl {
    struct Record {
        uint64_t t_steady_ns;
        std::string camera;
        const char* milestone;
        std::string detail;
        uint64_t frame_id;
    };
    std::mutex mutex;
    bool armed = false;
    bool written = false;
    std::string session_id;
    std::string folder;
    uint64_t session_steady_ns = 0;
    uint64_t session_realtime_ns = 0;
    std::atomic<uint64_t> enabled_steady_ns{0};
    std::vector<Record> records;
    std::atomic<uint64_t> overflow{0};
};

RecordingStartupAudit& RecordingStartupAudit::Instance()
{
    static RecordingStartupAudit instance;
    return instance;
}

RecordingStartupAudit::Impl* RecordingStartupAudit::impl()
{
    static Impl the_impl;
    return &the_impl;
}

double RecordingStartupAudit::WindowSeconds()
{
    static const double seconds = [] {
        const char* env = std::getenv("ORANGE_RECORDING_STARTUP_AUDIT_WINDOW_S");
        const double parsed = (env && *env) ? std::atof(env) : 5.0;
        return parsed > 0.0 ? parsed : 5.0;
    }();
    return seconds;
}

void RecordingStartupAudit::BeginSession(const std::string& session_id, const std::string& recording_folder)
{
    EndSession();
    Impl* p = impl();
    std::lock_guard<std::mutex> lock(p->mutex);
    p->armed = true;
    p->written = false;
    p->session_id = session_id;
    p->folder = recording_folder;
    p->session_steady_ns = steady_ns();
    p->session_realtime_ns = realtime_ns();
    p->enabled_steady_ns.store(0, std::memory_order_relaxed);
    p->records.clear();
    p->records.reserve(4096);
    p->overflow.store(0, std::memory_order_relaxed);
    p->records.push_back(Impl::Record{p->session_steady_ns, std::string(), "session_begin",
                                      "folder=" + recording_folder, 0});
}

void RecordingStartupAudit::Mark(const std::string& camera, const char* milestone,
                                 const std::string& detail, uint64_t frame_id)
{
    Impl* p = impl();
    const uint64_t now = steady_ns();
    std::lock_guard<std::mutex> lock(p->mutex);
    if (!p->armed) {
        return;
    }
    if (p->records.size() >= kMaxRecords) {
        p->overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p->records.push_back(Impl::Record{now, camera, milestone, detail, frame_id});
}

void RecordingStartupAudit::MarkRecordingEnabled(const std::string& detail)
{
    Impl* p = impl();
    p->enabled_steady_ns.store(steady_ns(), std::memory_order_release);
    Mark(std::string(), "recording_enabled", detail, 0);
}

bool RecordingStartupAudit::InWindow() const
{
    Impl* p = const_cast<RecordingStartupAudit*>(this)->impl();
    const uint64_t enabled = p->enabled_steady_ns.load(std::memory_order_acquire);
    if (enabled == 0) {
        return p->armed;  // before recording was enabled: still startup
    }
    const uint64_t now = steady_ns();
    return now >= enabled && (now - enabled) <= static_cast<uint64_t>(WindowSeconds() * 1e9);
}

uint64_t RecordingStartupAudit::overflow() const
{
    return const_cast<RecordingStartupAudit*>(this)->impl()->overflow.load(std::memory_order_relaxed);
}

// Writes recording_startup_audit.jsonl and _summary.json (log writer thread).
template <typename RecordT>
void WriteStartupAuditFiles(const std::vector<RecordT>& records, const std::string& folder,
                            const std::string& session_id, uint64_t session_steady,
                            uint64_t session_realtime, uint64_t enabled, uint64_t overflow)
{
    ScopedFsuid fsuid_guard;  // both files belong to the sudo invoker
    const std::string path = folder + "/recording_startup_audit.jsonl";
    std::ofstream out(path);
    if (!out) {
        std::cerr << "[startup_audit] cannot write " << path << std::endl;
        return;
    }
    out << "{\"record\":\"session\",\"session_id\":\"" << json_escape(session_id)
        << "\",\"session_steady_ns\":" << session_steady
        << ",\"session_realtime_ns\":" << session_realtime
        << ",\"recording_enabled_steady_ns\":" << enabled
        << ",\"window_s\":" << RecordingStartupAudit::WindowSeconds()
        << ",\"records\":" << records.size() << ",\"overflow\":" << overflow << "}\n";
    std::map<std::string, int> counts;
    for (const auto& r : records) {
        const double rel_ms = enabled ? (static_cast<double>(r.t_steady_ns) - static_cast<double>(enabled)) / 1e6
                                      : (static_cast<double>(r.t_steady_ns) - static_cast<double>(session_steady)) / 1e6;
        out << "{\"t_steady_ns\":" << r.t_steady_ns
            << ",\"t_rel_enabled_ms\":" << rel_ms
            << ",\"camera\":\"" << json_escape(r.camera)
            << "\",\"milestone\":\"" << r.milestone
            << "\",\"frame_id\":" << r.frame_id
            << ",\"detail\":\"" << json_escape(r.detail) << "\"}\n";
        counts[r.milestone]++;
    }
    out.close();
    std::ofstream summary(folder + "/recording_startup_audit_summary.json");
    summary << "{\n  \"schema_id\": \"orange.recording_startup_audit\",\n  \"schema_version\": 1,\n"
            << "  \"session_id\": \"" << json_escape(session_id) << "\",\n"
            << "  \"records\": " << records.size() << ",\n  \"overflow\": " << overflow << ",\n"
            << "  \"window_s\": " << RecordingStartupAudit::WindowSeconds() << ",\n  \"milestone_counts\": {";
    bool first = true;
    for (const auto& [k, v] : counts) {
        summary << (first ? "" : ",") << "\n    \"" << k << "\": " << v;
        first = false;
    }
    summary << "\n  },\n  \"jsonl\": \"" << json_escape(path) << "\"\n}\n";
    std::cout << "[startup_audit] wrote " << records.size() << " records (overflow " << overflow
              << ") to " << path << std::endl;
}

void RecordingStartupAudit::EndSession()
{
    Impl* p = impl();
    std::vector<Impl::Record> records;
    std::string folder, session_id;
    uint64_t session_steady = 0, session_realtime = 0, enabled = 0, overflow = 0;
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (!p->armed || p->written) {
            return;
        }
        p->written = true;
        p->armed = false;
        records = p->records;
        folder = p->folder;
        session_id = p->session_id;
        session_steady = p->session_steady_ns;
        session_realtime = p->session_realtime_ns;
        enabled = p->enabled_steady_ns.load(std::memory_order_relaxed);
        overflow = p->overflow.load(std::memory_order_relaxed);
    }
    if (folder.empty()) {
        return;
    }
    // Written by the process log writer thread (the caller may be the GUI or
    // a session thread); finalize's LogWriter::FlushAll waits for it.
    static const auto jobs = orange::timing::LogChannel<uint8_t>::Create(
        {"recording_startup_audit", 2}, [](const uint8_t&, std::string&) {});
    jobs->Post([records = std::move(records), folder, session_id, session_steady, session_realtime,
                enabled, overflow]() {
        WriteStartupAuditFiles(records, folder, session_id, session_steady, session_realtime,
                               enabled, overflow);
    });
}

}  // namespace orange
