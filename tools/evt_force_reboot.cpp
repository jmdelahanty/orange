// tools/evt_force_reboot.cpp
//
// Reboot an Emergent camera without an open control session.
//
// EVT_ForceReboot is a discovery-style GVCP broadcast (or unicast when the
// camera IP is given), like the ForceIP the acquisition code already uses, so
// it works even when the camera is refusing EVT_CameraOpen with
// "GVCP ACK error" because a killed process still holds its control
// privilege. That is the case the PDU cannot reach for cameras not wired to
// it (docs/detect_latency_review_2026_09_03.md, 2026-09-04 note).
//
// Usage:
//   evt_force_reboot <serial-or-mac> [camera-ip]
//
// The serial is the decimal camera serial (e.g. 2010093); a MAC is accepted
// in the SDK's dash-separated form (e0-55-97-1e-ab-ed). With no IP the
// command is broadcast on every interface; with an IP it is sent unicast.
// After a reboot the camera takes a few seconds to answer discovery again;
// verify with the stream smoke before starting a run.

#include <EmergentCameraAPIs.h>
#include <emergenterrors.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void usage(const char* argv0)
{
    std::fprintf(stderr,
                 "Usage: %s <serial-or-mac> [camera-ip]\n"
                 "  Sends EVT_ForceReboot to the camera. No control session is needed.\n"
                 "  Example: %s 2010093 192.168.110.2\n",
                 argv0, argv0);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3 ||
        std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 2;
    }
    const std::string target = argv[1];
    const char* camera_ip = argc == 3 ? argv[2] : nullptr;

    std::printf("evt_force_reboot: sending force reboot to %s (%s)\n",
                target.c_str(),
                camera_ip ? camera_ip : "broadcast");
    const EVT_ERROR status = Emergent::EVT_ForceReboot(target.c_str(), camera_ip);
    if (status != EVT_SUCCESS) {
        std::fprintf(stderr,
                     "evt_force_reboot: EVT_ForceReboot failed with EVT_ERROR %d\n",
                     static_cast<int>(status));
        return 1;
    }
    std::printf("evt_force_reboot: reboot command sent; allow a few seconds, then verify with\n"
                "  sudo -n /usr/local/bin/orange-evt-stream-smoke --config-dir <config> --serial %s --frames 5\n",
                target.c_str());
    return 0;
}
