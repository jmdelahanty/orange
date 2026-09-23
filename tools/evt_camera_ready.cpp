// tools/evt_camera_ready.cpp
//
// Post-power-cycle readiness check for EVT cameras with Emergent EF mounts.
// Waits until every expected serial answers discovery, opens each camera
// without streaming, and reports lens-mount state (LensMountPresent,
// LensPresent, LensBusy, LensName, mount firmware), the commanded and
// mount-reported lens positions (Iris/IrisCurrent, Focus/FocusCurrent), and
// the sensor registers that decide exposure (Exposure, Gain, AutoGain,
// LUTEnable, Offset). Optionally re-initializes the lens (Iris=0, Focus=0)
// and applies configured focus/iris from a camera config folder, verifying
// through the *Current nodes. Writes a JSON report; exit code 0 only when
// every expected camera is present and every check passed.
//
// Usage:
//   evt_camera_ready --serials 2010093,2010094,2010095,2010096 \
//       [--wait-seconds 90] [--config-dir <dir>] [--init-lens] [--apply-lens]
//       [--json <path>]
//
// No streaming is opened, so no root privileges are needed.
#include <EmergentCameraAPIs.h>
#include <gigevisiondeviceinfo.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Emergent;

namespace {

double now_ms()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Options {
    std::vector<std::string> serials;
    double wait_seconds = 90.0;
    std::string config_dir;
    bool init_lens = false;
    bool apply_lens = false;
    std::string json_path;
};

void usage(const char* argv0)
{
    std::fprintf(stderr,
        "Usage: %s --serials <csv> [--wait-seconds <s>] [--config-dir <dir>] [--init-lens] [--apply-lens] [--json <path>]\n"
        "  --serials      Expected camera serials (comma separated).\n"
        "  --wait-seconds How long to wait for all serials to answer discovery (default 90).\n"
        "  --config-dir   Camera config folder with <serial>.json (focus/iris targets).\n"
        "  --init-lens    Send Iris=0 and Focus=0 (mechanism re-init) before checks.\n"
        "  --apply-lens   Apply configured focus/iris with LensBusy gating and *Current verification.\n"
        "  --json         Write the report to this path.\n", argv0);
}

std::vector<std::string> split_csv(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool read_u32(CEmergentCamera* c, const char* n, unsigned int* v) { return EVT_CameraGetUInt32Param(c, n, v) == EVT_SUCCESS; }
bool read_bool(CEmergentCamera* c, const char* n, bool* v) { return EVT_CameraGetBoolParam(c, n, v) == EVT_SUCCESS; }
std::string read_str(CEmergentCamera* c, const char* n)
{
    char buf[128] = {0};
    unsigned long vs = 0;
    if (EVT_CameraGetStringParam(c, n, buf, sizeof buf, &vs) != EVT_SUCCESS) return "";
    return std::string(buf);
}

// Minimal JSON reader for "focus": N / "iris": N in a camera config file.
bool read_json_int(const std::string& path, const char* key, int* out)
{
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    const std::string s = ss.str();
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    char* end = nullptr;
    const long v = std::strtol(s.c_str() + p, &end, 10);
    if (end == s.c_str() + p) return false;
    *out = static_cast<int>(v);
    return true;
}

struct WriteResult { bool ok = false; unsigned int current = 0; double settle_ms = 0; double call_ms = 0; bool busy_timeout = false; };

WriteResult write_verified(CEmergentCamera* c, const char* node, const char* current_node, unsigned int target, unsigned int tol, double timeout_ms)
{
    WriteResult r;
    // Wait for the mount to be idle (writes issued while LensBusy are dropped).
    const double t_idle = now_ms();
    for (;;) {
        bool busy = false;
        if (!read_bool(c, "LensBusy", &busy) || !busy) break;
        if (now_ms() - t_idle > 3000.0) { r.busy_timeout = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double t0 = now_ms();
    if (EVT_CameraSetUInt32Param(c, node, target) != EVT_SUCCESS) return r;
    r.call_ms = now_ms() - t0;
    for (;;) {
        bool busy = false;
        read_bool(c, "LensBusy", &busy);
        unsigned int cur = 0;
        if (!read_u32(c, current_node, &cur)) return r;
        r.current = cur;
        r.settle_ms = now_ms() - t0;
        const unsigned int diff = cur > target ? cur - target : target - cur;
        if (!busy && diff <= tol) { r.ok = true; return r; }
        if (r.settle_ms > timeout_ms) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", flag); std::exit(2); }
            return argv[++i];
        };
        if (a == "--serials") o.serials = split_csv(next("--serials"));
        else if (a == "--wait-seconds") o.wait_seconds = std::atof(next("--wait-seconds"));
        else if (a == "--config-dir") o.config_dir = next("--config-dir");
        else if (a == "--init-lens") o.init_lens = true;
        else if (a == "--apply-lens") o.apply_lens = true;
        else if (a == "--json") o.json_path = next("--json");
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (o.serials.empty()) { usage(argv[0]); return 2; }
    if (o.apply_lens && o.config_dir.empty()) { std::fprintf(stderr, "--apply-lens needs --config-dir\n"); return 2; }

    // 1. Discovery: wait for every expected serial.
    std::printf("evt_camera_ready: SDK %s, waiting up to %.0f s for %zu camera(s)\n", EVT_SDKVersion(), o.wait_seconds, o.serials.size());
    std::vector<GigEVisionDeviceInfo> found;
    const double t_wait = now_ms();
    double discovered_after_s = -1.0;
    for (;;) {
        std::vector<GigEVisionDeviceInfo> d(32);
        unsigned int n = 0, sz = 32;
        found.clear();
        if (EVT_ListDevices(d.data(), &sz, &n) == EVT_SUCCESS) {
            for (const auto& s : o.serials)
                for (unsigned i = 0; i < n; ++i)
                    if (s == d[i].serialNumber) found.push_back(d[i]);
        }
        if (found.size() == o.serials.size()) { discovered_after_s = (now_ms() - t_wait) / 1000.0; break; }
        if ((now_ms() - t_wait) / 1000.0 > o.wait_seconds) break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    std::printf("discovery: %zu/%zu present after %.1f s\n", found.size(), o.serials.size(), (now_ms() - t_wait) / 1000.0);

    // 2. Per-camera checks.
    bool all_ok = found.size() == o.serials.size();
    std::ostringstream js;
    js << "{\n  \"schema_id\": \"orange.evt_camera_ready\", \"schema_version\": 1,\n"
       << "  \"expected\": " << o.serials.size() << ", \"present\": " << found.size()
       << ", \"discovered_after_s\": " << discovered_after_s << ",\n  \"cameras\": [\n";
    bool first = true;
    for (const auto& dev : found) {
        CEmergentCamera cam;
        const std::string serial = dev.serialNumber;
        if (!first) js << ",\n";
        first = false;
        if (EVT_CameraOpen(&cam, &dev) != EVT_SUCCESS) {
            std::printf("== %s OPEN FAILED (GVCP refused; camera may need a reboot)\n", serial.c_str());
            js << "    {\"serial\": \"" << serial << "\", \"open\": false, \"ok\": false}";
            all_ok = false;
            continue;
        }
        bool mount = false, lens = false, busy = false, autogain = false, lut = false;
        read_bool(&cam, "LensMountPresent", &mount);
        read_bool(&cam, "LensPresent", &lens);
        read_bool(&cam, "LensBusy", &busy);
        read_bool(&cam, "AutoGain", &autogain);
        read_bool(&cam, "LUTEnable", &lut);
        unsigned int iris = 0, iris_cur = 0, focus = 0, focus_cur = 0, exposure = 0, gain = 0, offset = 0;
        const bool feedback = read_u32(&cam, "IrisCurrent", &iris_cur) && read_u32(&cam, "FocusCurrent", &focus_cur);
        read_u32(&cam, "Iris", &iris); read_u32(&cam, "Focus", &focus);
        read_u32(&cam, "Exposure", &exposure); read_u32(&cam, "Gain", &gain); read_u32(&cam, "Offset", &offset);
        const std::string lens_name = read_str(&cam, "LensName");
        const std::string mount_fw = read_str(&cam, "LensMountFirmwareVersion");

        std::printf("== %s ip=%s mount=%d lens=%d busy=%d lens_name=\"%s\" mount_fw=%s\n", serial.c_str(), dev.currentIp, (int)mount, (int)lens, (int)busy, lens_name.c_str(), mount_fw.c_str());
        std::printf("   Iris=%u IrisCurrent=%u Focus=%u FocusCurrent=%u feedback=%s | Exposure=%u Gain=%u AutoGain=%d LUTEnable=%d Offset=%u\n",
                    iris, iris_cur, focus, focus_cur, feedback ? "yes" : "no", exposure, gain, (int)autogain, (int)lut, offset);

        bool cam_ok = mount && lens && feedback;
        std::string init_note = "skipped", apply_note = "skipped";
        if (o.init_lens && cam_ok) {
            const WriteResult ri = write_verified(&cam, "Iris", "IrisCurrent", 0, 0, 6000.0);
            const WriteResult rf = write_verified(&cam, "Focus", "FocusCurrent", 0, 2, 8000.0);
            std::printf("   init: Iris=0 %s (call %.0f ms, IrisCurrent=%u) Focus=0 %s (call %.0f ms, FocusCurrent=%u)\n",
                        ri.ok ? "ok" : "FAILED", ri.call_ms, ri.current, rf.ok ? "ok" : "FAILED", rf.call_ms, rf.current);
            init_note = (ri.ok && rf.ok) ? "ok" : "failed";
            cam_ok = cam_ok && ri.ok && rf.ok;
        }
        int cfg_focus = -1, cfg_iris = -1;
        if (!o.config_dir.empty()) {
            const std::string path = o.config_dir + "/" + serial + ".json";
            read_json_int(path, "focus", &cfg_focus);
            read_json_int(path, "iris", &cfg_iris);
        }
        if (o.apply_lens && cam_ok) {
            if (cfg_focus < 0 || cfg_iris < 0) {
                std::printf("   apply: no focus/iris in %s/%s.json\n", o.config_dir.c_str(), serial.c_str());
                apply_note = "no_config";
                cam_ok = false;
            } else {
                const WriteResult rf = write_verified(&cam, "Focus", "FocusCurrent", (unsigned)cfg_focus, 2, 8000.0);
                const WriteResult ri = write_verified(&cam, "Iris", "IrisCurrent", (unsigned)cfg_iris, 0, 6000.0);
                std::printf("   apply: Focus=%d %s (FocusCurrent=%u, %.0f ms) Iris=%d %s (IrisCurrent=%u, %.0f ms)\n",
                            cfg_focus, rf.ok ? "ok" : "FAILED", rf.current, rf.settle_ms, cfg_iris, ri.ok ? "ok" : "FAILED", ri.current, ri.settle_ms);
                apply_note = (rf.ok && ri.ok) ? "ok" : "failed";
                cam_ok = cam_ok && rf.ok && ri.ok;
                read_u32(&cam, "IrisCurrent", &iris_cur); read_u32(&cam, "FocusCurrent", &focus_cur);
                read_u32(&cam, "Iris", &iris); read_u32(&cam, "Focus", &focus);
            }
        } else if (!o.apply_lens && cfg_focus >= 0 && cfg_iris >= 0 && feedback) {
            // Report-only: does the mount already sit at the configured values?
            const unsigned fd = focus_cur > (unsigned)cfg_focus ? focus_cur - cfg_focus : cfg_focus - focus_cur;
            const bool at_cfg = iris_cur == (unsigned)cfg_iris && fd <= 2;
            std::printf("   config: focus=%d iris=%d -> mount %s\n", cfg_focus, cfg_iris, at_cfg ? "matches" : "DIFFERS");
            apply_note = at_cfg ? "matches_config" : "differs_from_config";
            cam_ok = cam_ok && at_cfg;
        }
        std::printf("   result: %s\n", cam_ok ? "OK" : "FAIL");
        all_ok = all_ok && cam_ok;
        js << "    {\"serial\": \"" << serial << "\", \"ip\": \"" << dev.currentIp << "\", \"open\": true"
           << ", \"lens_mount_present\": " << (mount ? "true" : "false") << ", \"lens_present\": " << (lens ? "true" : "false")
           << ", \"lens_busy\": " << (busy ? "true" : "false") << ", \"lens_name\": \"" << lens_name << "\", \"mount_firmware\": \"" << mount_fw << "\""
           << ", \"feedback_available\": " << (feedback ? "true" : "false")
           << ", \"iris\": " << iris << ", \"iris_current\": " << iris_cur << ", \"focus\": " << focus << ", \"focus_current\": " << focus_cur
           << ", \"exposure\": " << exposure << ", \"gain\": " << gain << ", \"auto_gain\": " << (autogain ? "true" : "false")
           << ", \"lut_enable\": " << (lut ? "true" : "false") << ", \"offset\": " << offset
           << ", \"config_focus\": " << cfg_focus << ", \"config_iris\": " << cfg_iris
           << ", \"init\": \"" << init_note << "\", \"apply\": \"" << apply_note << "\", \"ok\": " << (cam_ok ? "true" : "false") << "}";
        EVT_CameraClose(&cam);
    }
    for (const auto& s : o.serials) {
        bool present = false;
        for (const auto& d : found) if (s == d.serialNumber) present = true;
        if (!present) {
            if (!first) js << ",\n";
            first = false;
            js << "    {\"serial\": \"" << s << "\", \"open\": false, \"present\": false, \"ok\": false}";
            std::printf("== %s NOT DISCOVERED\n", s.c_str());
        }
    }
    js << "\n  ],\n  \"ok\": " << (all_ok ? "true" : "false") << "\n}\n";
    if (!o.json_path.empty()) {
        std::ofstream out(o.json_path);
        out << js.str();
        std::printf("report: %s\n", o.json_path.c_str());
    }
    std::printf("evt_camera_ready: %s\n", all_ok ? "ALL OK" : "PROBLEMS FOUND");
    return all_ok ? 0 : 1;
}
