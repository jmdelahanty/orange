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
    print_out += ", Camera Dropped Frames: " + std::to_string(camera_state->dropped_frames);
    print_out += ", Preprocess Waits: " + std::to_string(preprocess_resource_waits);
    print_out += ", Preprocess Drops: " + std::to_string(preprocess_frames_dropped);
    print_out += ", Encode Failures: " + std::to_string(encode_failures);
    print_out += ", Encode Slow Frames: " + std::to_string(encode_slow_frames);
    float calc_frame_rate = camera_state->frames_recd / time_diff;
    print_out += ", Calculated Frame Rate: " + std::to_string(calc_frame_rate);
    std::cout << print_out << std::endl;
}

void show_ptp_offset(PTPState *ptp_state, CameraEmergent *ecam)
{
    // Show raw offsets.
    for (unsigned int i = 0; i < 5;)
    {
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
}

void start_ptp_sync(PTPState *ptp_state, PTPParams *ptp_params, CameraParams *camera_params, CameraEmergent *ecam, unsigned int delay_in_second)
{
    if (ptp_params->network_sync) {
        uint64_t ptp_counter = sync_fetch_and_add(&ptp_params->ptp_counter, 1);
        printf("%lu\n", ptp_counter);
        std::cout << ptp_params->ptp_global_time << std::endl;
        while(!ptp_params->network_set_start_ptp) {
            usleep(10); // sleep 1ms
        }
        ptp_state->ptp_time = get_current_PTP_time(&ecam->camera);
    } else {
        if (ptp_params->ptp_counter == camera_params->num_cameras - 1)
        {
            ptp_state->ptp_time = get_current_PTP_time(&ecam->camera);
            ptp_params->ptp_global_time = ((unsigned long long)delay_in_second) * 1000000000 + ptp_state->ptp_time;
        }
        uint64_t ptp_counter = sync_fetch_and_add(&ptp_params->ptp_counter, 1);
        printf("%lu\n", ptp_counter);
        while (ptp_params->ptp_counter != camera_params->num_cameras)
        {
            // printf(".");
            // fflush(stdout);
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
}


void grab_frames_after_countdown(PTPState *ptp_state, CameraEmergent *ecam)
{
    printf("Grabbing Frames after countdown...\n");
    ptp_state->ptp_time_countdown = 0;
    // Countdown code
    do
    {
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
}
