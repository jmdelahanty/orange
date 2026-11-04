#include "video_capture.h"
#include <chrono>
#include "global.h"

void report_statistics(CameraParams *camera_params,
                       CameraState *camera_state,
                       double time_diff,
                       uint64_t preprocess_resource_waits,
                       uint64_t preprocess_frames_dropped,
                       uint64_t encode_failures,
                       uint64_t encode_slow_frames)
{
    std::string print_out;
    print_out += "\n" + camera_params->camera_serial;
    print_out += ", Frame count: " + std::to_string(camera_state->frame_count);
    print_out += ", Frame received: " + std::to_string(camera_state->frames_recd);
    print_out += ", Camera Frame-ID Gaps: " + std::to_string(camera_state->dropped_frames);
    print_out += ", GetFrame Errors: " + std::to_string(camera_state->get_frame_errors);
    print_out += ", Last GetFrame Error: " + std::to_string(camera_state->last_get_frame_error_code);
    print_out += ", Preprocess Waits: " + std::to_string(preprocess_resource_waits);
    print_out += ", Preprocess Drops: " + std::to_string(preprocess_frames_dropped);
    print_out += ", Encode Failures: " + std::to_string(encode_failures);
    print_out += ", Encode Slow Frames: " + std::to_string(encode_slow_frames);
    float calc_frame_rate = camera_state->frames_recd / time_diff;
    print_out += ", Calculated Frame Rate: " + std::to_string(calc_frame_rate);
    std::cout << print_out << std::endl;
}

namespace {

// Limits for the PTP start. With the host stack running, the camera's offset
// changes five times within about 0.2 s and every camera reaches the barrier
// within about 2 s of the first.
constexpr auto kPtpOffsetLockTimeout = std::chrono::seconds(10);
constexpr auto kPtpStartBarrierTimeout = std::chrono::seconds(30);
constexpr auto kPtpCountdownTimeout = std::chrono::seconds(30);

bool ptp_keep_running(const bool *keep_running)
{
    return !keep_running || __atomic_load_n(keep_running, __ATOMIC_ACQUIRE);
}

bool ptp_start_aborted(const PTPParams *ptp_params)
{
    return __atomic_load_n(&ptp_params->ptp_start_aborted, __ATOMIC_ACQUIRE);
}

std::string camera_ptp_status(CameraEmergent *ecam)
{
    char status[100] = {};
    unsigned long size = 0;
    if (EVT_CameraGetEnumParam(&ecam->camera, "PtpStatus", status, sizeof(status), &size) != EVT_SUCCESS) {
        return "unreadable";
    }
    return status;
}

void set_ptp_error(std::string *error_out, const std::string &message)
{
    if (error_out && error_out->empty()) {
        *error_out = message;
    }
}

}  // namespace

bool show_ptp_offset(PTPState *ptp_state, CameraEmergent *ecam,
                     const bool *keep_running, std::string *error_out)
{
    // Five changes of the camera's PTP offset show it follows a master. The
    // offset stays constant without one (no ptp4l on the host).
    const auto deadline = std::chrono::steady_clock::now() + kPtpOffsetLockTimeout;
    for (unsigned int i = 0; i < 5;)
    {
        if (!ptp_keep_running(keep_running)) {
            set_ptp_error(error_out, "stream stopped while waiting for the camera's PTP lock");
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            set_ptp_error(error_out,
                "camera shows no PTP lock after " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kPtpOffsetLockTimeout).count()) +
                " s (PtpOffset unchanged, PtpStatus " + camera_ptp_status(ecam) +
                "). Is the host PTP stack running? Check: systemctl is-active ptp4l phc2sys");
            return false;
        }
        EVT_CameraGetInt32Param(&ecam->camera, "PtpOffset", &ptp_state->ptp_offset);
        if (ptp_state->ptp_offset != ptp_state->ptp_offset_prev)
        {
            ptp_state->ptp_offset_sum += ptp_state->ptp_offset;
            i++;
            // printf("Offset %d: %d\n", i, ptp_offset);
        }
        ptp_state->ptp_offset_prev = ptp_state->ptp_offset;
    }
    printf("Offset Average: %d\n", ptp_state->ptp_offset_sum / 5);
    return true;
}

bool start_ptp_sync(PTPState *ptp_state, PTPParams *ptp_params, CameraParams *camera_params,
                    CameraEmergent *ecam, unsigned int delay_in_second,
                    const bool *keep_running, std::string *error_out)
{
    if (ptp_params->network_sync) {
        uint64_t ptp_counter = sync_fetch_and_add(&ptp_params->ptp_counter, 1);
        printf("%lu\n", ptp_counter);
        std::cout << ptp_params->ptp_global_time << std::endl;
        // The network controller decides when to start: no time limit, but
        // the wait ends on stop or on another camera's abort.
        while(!ptp_params->network_set_start_ptp) {
            if (!ptp_keep_running(keep_running) || ptp_start_aborted(ptp_params)) {
                set_ptp_error(error_out, ptp_start_aborted(ptp_params)
                    ? "another camera aborted the PTP start"
                    : "stream stopped while waiting for the network PTP start");
                return false;
            }
            usleep(10); // sleep 1ms
        }
        ptp_state->ptp_time = get_current_PTP_time(&ecam->camera);
    } else {
        if (ptp_params->ptp_counter == static_cast<uint64_t>(camera_params->num_cameras - 1))
        {
            ptp_state->ptp_time = get_current_PTP_time(&ecam->camera);
            ptp_params->ptp_global_time = ((unsigned long long)delay_in_second) * 1000000000 + ptp_state->ptp_time;
        }
        uint64_t ptp_counter = sync_fetch_and_add(&ptp_params->ptp_counter, 1);
        printf("%lu\n", ptp_counter);
        const auto deadline = std::chrono::steady_clock::now() + kPtpStartBarrierTimeout;
        while (ptp_params->ptp_counter != static_cast<uint64_t>(camera_params->num_cameras))
        {
            if (ptp_start_aborted(ptp_params)) {
                set_ptp_error(error_out, "another camera aborted the PTP start");
                return false;
            }
            if (!ptp_keep_running(keep_running)) {
                set_ptp_error(error_out, "stream stopped at the PTP start barrier");
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                set_ptp_error(error_out,
                    "PTP start barrier: " + std::to_string(ptp_params->ptp_counter) + " of " +
                    std::to_string(camera_params->num_cameras) + " cameras arrived within " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kPtpStartBarrierTimeout).count()) +
                    " s");
                return false;
            }
            usleep(10);
        }
    }

    const unsigned long long base_gate_time_ns = ptp_params->ptp_global_time;
    const unsigned long long ptp_time_plus_delta_to_start =
        base_gate_time_ns + camera_params->ptp_gate_offset_ns;
    ptp_state->ptp_time_plus_delta_to_start_low = (unsigned int)(ptp_time_plus_delta_to_start & 0xFFFFFFFF);
    ptp_state->ptp_time_plus_delta_to_start_high = (unsigned int)(ptp_time_plus_delta_to_start >> 32);
    EVT_CameraSetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeHigh", ptp_state->ptp_time_plus_delta_to_start_high);
    EVT_CameraSetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeLow", ptp_state->ptp_time_plus_delta_to_start_low);
    ptp_state->ptp_time_plus_delta_to_start_uint = ptp_time_plus_delta_to_start;
    ptp_state->ptp_time_plus_delta_to_start = ptp_time_plus_delta_to_start;
    unsigned int gate_time_high_readback = 0;
    unsigned int gate_time_low_readback = 0;
    const bool gate_high_ok =
        EVT_CameraGetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeHigh", &gate_time_high_readback) == EVT_SUCCESS;
    const bool gate_low_ok =
        EVT_CameraGetUInt32Param(&ecam->camera, "PtpAcquisitionGateTimeLow", &gate_time_low_readback) == EVT_SUCCESS;
    const unsigned long long gate_time_readback =
        (static_cast<unsigned long long>(gate_time_high_readback) << 32) |
        static_cast<unsigned long long>(gate_time_low_readback);
    printf("PTP Gate programming serial=%s base_gate_ns=%llu effective_gate_ns=%llu offset_ns=%llu readback_gate_ns=%s%llu\n",
           camera_params->camera_serial.c_str(),
           base_gate_time_ns,
           ptp_time_plus_delta_to_start,
           camera_params->ptp_gate_offset_ns,
           (gate_high_ok && gate_low_ok) ? "" : "unavailable:",
           gate_time_readback);
    return true;
}


bool grab_frames_after_countdown(PTPState *ptp_state, CameraEmergent *ecam,
                                 const bool *keep_running, std::string *error_out)
{
    printf("Grabbing Frames after countdown...\n");
    ptp_state->ptp_time_countdown = 0;
    const auto deadline = std::chrono::steady_clock::now() + kPtpCountdownTimeout;
    // Countdown code
    do
    {
        if (!ptp_keep_running(keep_running)) {
            set_ptp_error(error_out, "stream stopped during the PTP gate countdown");
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            set_ptp_error(error_out, "camera PTP time did not reach the gate time within " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kPtpCountdownTimeout).count()) + " s");
            return false;
        }
        EVT_CameraExecuteCommand(&ecam->camera, "GevTimestampControlLatch");
        EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueHigh", &ptp_state->ptp_time_high);
        EVT_CameraGetUInt32Param(&ecam->camera, "GevTimestampValueLow", &ptp_state->ptp_time_low);
        ptp_state->ptp_time = (((unsigned long long)(ptp_state->ptp_time_high)) << 32) | ((unsigned long long)(ptp_state->ptp_time_low));

        if (ptp_state->ptp_time > ptp_state->ptp_time_countdown)
        {
            printf("%llu\n", (ptp_state->ptp_time_plus_delta_to_start - ptp_state->ptp_time) / 1000000000);
            ptp_state->ptp_time_countdown = ptp_state->ptp_time + 1000000000; // 1s
        }

    } while (ptp_state->ptp_time <= ptp_state->ptp_time_plus_delta_to_start);
    // Countdown done.
    try_start_timer();
    return true;
}
