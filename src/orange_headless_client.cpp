#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include "network_base.h"
#include "thread.h"
#include "types.h"
#include <cstring>
#include "video_capture.h"
#include "NvEncoder/NvCodecUtils.h"
#include "project.h"
#include "video_capture.h"
#include "fetch_generated.h"
#include "acquire_frames.h"
#include "modern_recording_pipeline.h"
#include <signal.h>

#define evt_buffer_size 100
#define max_cameras 20

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

struct HeadlessEncoderSettings {
    std::string codec = "h264";
    std::string preset = "p1";
    std::string tuning = "ll";
    std::string rate_control_mode = "vbr";
    int quality_value = 20;
    int gop_length = 0;
};

std::vector<std::string> split_headless_encoder_setup(const std::string& setup)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : setup) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == ';' || ch == '|') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

HeadlessEncoderSettings parse_headless_encoder_setup(const std::string& setup)
{
    HeadlessEncoderSettings settings;
    const std::vector<std::string> tokens = split_headless_encoder_setup(setup);

    int positional_index = 0;
    auto assign_positional = [&](const std::string& token) {
        switch (positional_index++) {
            case 0: settings.codec = token; break;
            case 1: settings.preset = token; break;
            case 2: settings.tuning = token; break;
            case 3: settings.rate_control_mode = token; break;
            case 4: settings.quality_value = std::atoi(token.c_str()); break;
            case 5: settings.gop_length = std::atoi(token.c_str()); break;
            default: break;
        }
    };

    for (const std::string& token : tokens) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            assign_positional(token);
            continue;
        }

        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (value.empty()) {
            continue;
        }

        if (key == "codec") {
            settings.codec = value;
        } else if (key == "preset") {
            settings.preset = value;
        } else if (key == "tuning" || key == "tune") {
            settings.tuning = value;
        } else if (key == "rc" || key == "rate_control" || key == "rate_control_mode") {
            settings.rate_control_mode = value;
        } else if (key == "quality" || key == "cq" || key == "qp") {
            settings.quality_value = std::atoi(value.c_str());
        } else if (key == "gop" || key == "gop_length") {
            settings.gop_length = std::atoi(value.c_str());
        }
    }

    if (settings.codec.empty()) {
        settings.codec = "h264";
    }
    if (settings.preset.empty()) {
        settings.preset = "p1";
    }
    if (settings.tuning.empty()) {
        settings.tuning = "ll";
    }
    if (settings.rate_control_mode.empty()) {
        settings.rate_control_mode = "vbr";
    }
    if (settings.quality_value < 1) {
        settings.quality_value = 20;
    }
    if (settings.gop_length < 0) {
        settings.gop_length = 0;
    }

    return settings;
}

RecordingOutputConfig build_native_recording_output_config(const CameraParams& camera_params)
{
    RecordingOutputConfig output;
    output.mode = "factor";
    output.downsample_factor = 1;
    output.requested_width = static_cast<int>(camera_params.width);
    output.requested_height = static_cast<int>(camera_params.height);
    output.resolved_width = static_cast<int>(camera_params.width);
    output.resolved_height = static_cast<int>(camera_params.height);
    output.resize_enabled = false;
    return output;
}

bool prepare_headless_recording_artifacts(const std::string& record_folder,
                                          CameraControl* camera_control,
                                          CameraParams* cameras_params,
                                          int num_cameras,
                                          PTPParams* ptp_params)
{
    if (record_folder.empty()) {
        std::cerr << "Headless recording folder is empty." << std::endl;
        return false;
    }

    if (mkdir(record_folder.c_str(), 0777) == -1 && errno != EEXIST) {
        std::cerr << "Error : " << std::strerror(errno) << std::endl;
        return false;
    }

    const std::filesystem::path recording_path(record_folder);
    const std::string recording_id = recording_path.filename().string();
    const std::filesystem::path base_path = recording_path.parent_path().empty()
        ? recording_path
        : recording_path.parent_path();

    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->recording_folder = record_folder;
    }

    if (!write_recording_snapshot(
            record_folder,
            recording_id,
            cameras_params,
            num_cameras,
            base_path.string(),
            camera_control->sync_camera,
            ptp_params)) {
        std::cerr << "Failed to write headless recording snapshot for " << record_folder << std::endl;
        return false;
    }

    if (!initialize_ptp_sync_summary(
            record_folder,
            recording_id,
            num_cameras,
            camera_control->sync_camera,
            ptp_params)) {
        std::cerr << "Failed to initialize headless PTP summary for " << record_folder << std::endl;
        return false;
    }

    std::cout << "Recorded video saves to : " << record_folder << std::endl;
    return true;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control)
{
    camera_control->record_video = false;
    camera_control->recording_draining = true;
    camera_control->stop_record = true;

    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (camera_control->active_recorders.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
        usleep(1000);
    }

    if (camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
        std::cerr << "Headless recording drain timed out with "
                  << camera_control->active_recorders.load(std::memory_order_relaxed)
                  << " active recorder(s)." << std::endl;
    }

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->request_stop();
    }

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->shutdown();
        pipeline.reset();
    }

    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    camera_control->recording_folder.clear();
}

} // namespace

void quit_process(bool error = false, const std::string &reason = "")
{
    enet_deinitialize();
    // Show console reason before exit
    if (error)
    {
        std::cout << reason << std::endl;
        system("PAUSE");
        exit(-1);
    }
}

bool open_cameras(CameraParams *cameras_params, CameraEmergent *ecams, CameraEachSelect *cameras_select, GigEVisionDeviceInfo *device_info, int num_cameras, std::string config_folder)
{
    std::vector<std::string> camera_config_files;
    update_camera_configs(camera_config_files, config_folder);

    for (int i = 0; i < num_cameras; i++)
    {
        set_camera_params(&cameras_params[i], &device_info[i], camera_config_files, i, num_cameras);
        open_camera_with_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id], &cameras_params[i]);
    }
    return true;
}


bool start_camera_thread(std::vector<std::thread> &camera_threads,
    std::vector<CameraResources>& camera_resources,
    std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
    CameraParams *cameras_params, CameraEmergent *ecams, CameraControl *camera_control, CameraEachSelect *cameras_select,
    GigEVisionDeviceInfo *device_info, int num_cameras, PTPParams *ptp_params, std::string record_folder, std::string encoder_basic_setup)
{
    std::cout << "start camera sthread..." << std::endl;
    try {
        allocate_camera_frame_buffers(ecams, cameras_params, evt_buffer_size, num_cameras);
    } catch (...) {
        std::cout << "Failed to start thread..." << std::endl;
        return false;
    }

    camera_control->record_video = true;
    camera_control->subscribe = true;
    int ptp_camera_count = 0;
    for (int i = 0; i < num_cameras; i++) {
        if (camera_sync_mode_uses_ptp(&cameras_params[i])) {
            ptp_camera_count++;
        }
    }
    if (ptp_camera_count != 0 && ptp_camera_count != num_cameras) {
        std::cerr << "Headless mode does not support mixed ptp_gate and non-PTP camera sync modes in one run." << std::endl;
        return false;
    }
    const bool use_ptp_sync = (ptp_camera_count == num_cameras && num_cameras > 0);
    camera_control->sync_camera = use_ptp_sync;
    camera_control->recording_draining = false;
    camera_control->stop_record = false;

    if (!prepare_headless_recording_artifacts(
            record_folder,
            camera_control,
            cameras_params,
            num_cameras,
            ptp_params)) {
        return false;
    }

    const HeadlessEncoderSettings encoder_settings = parse_headless_encoder_setup(encoder_basic_setup);
    std::cout << "Headless encoder config: codec=" << encoder_settings.codec
              << " preset=" << encoder_settings.preset
              << " tuning=" << encoder_settings.tuning
              << " rc=" << encoder_settings.rate_control_mode
              << " quality=" << encoder_settings.quality_value
              << " gop=" << encoder_settings.gop_length
              << std::endl;

    size_t max_frame_size_bytes = 0;
    for (int i = 0; i < num_cameras; ++i) {
        const size_t current_size =
            static_cast<size_t>(cameras_params[i].width) * static_cast<size_t>(cameras_params[i].height);
        if (current_size > max_frame_size_bytes) {
            max_frame_size_bytes = current_size;
        }
    }

    camera_resources.clear();
    camera_resources.resize(num_cameras);
    recording_pipelines.clear();
    recording_pipelines.resize(num_cameras);

    try {
        for (int i = 0; i < num_cameras; ++i) {
            camera_resources[i].initialize(cameras_params[i].gpu_id, max_frame_size_bytes);
            cameras_select[i].stream_on = false;
            cameras_select[i].record = true;
            cameras_select[i].yolo = false;
            cameras_select[i].crop_and_encode = false;
            cameras_select[i].send_frame_ipc = false;

            recording_pipelines[i] = std::make_unique<ModernRecordingPipeline>(
                &cameras_params[i],
                build_native_recording_output_config(cameras_params[i]),
                encoder_settings.codec,
                encoder_settings.preset,
                encoder_settings.tuning,
                encoder_settings.rate_control_mode,
                encoder_settings.quality_value,
                encoder_settings.gop_length,
                record_folder,
                *camera_resources[i].recycle_queue,
                camera_control);
            recording_pipelines[i]->start();
        }
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize headless recording pipelines: " << ex.what() << std::endl;
        drain_and_shutdown_recording(recording_pipelines, camera_control);
        for (auto& resources : camera_resources) {
            resources.cleanup();
        }
        camera_resources.clear();
        return false;
    }

    if (use_ptp_sync) {
        for (int i = 0; i < num_cameras; i++)
        {
            ptp_camera_sync(&ecams[i].camera, &cameras_params[i]);
        }
        std::cout << "Headless sync mode: ptp_gate" << std::endl;
    } else {
        std::cout << "Headless sync mode: free_run / explicit trigger" << std::endl;
    }

    for (int i = 0; i < num_cameras; i++)
    {
        camera_threads.push_back(std::thread(
            &acquire_frames,
            &ecams[i],
            &cameras_params[i],
            &cameras_select[i],
            camera_control,
            ptp_params,
            nullptr,
            nullptr,
            recording_pipelines[i] ? recording_pipelines[i]->preprocess_worker() : nullptr,
            nullptr,
            nullptr,
            &camera_resources[i],
            nullptr));
    }

    // wait for all camera ready
    if (use_ptp_sync) {
        while(ptp_params->ptp_counter!=num_cameras) {
            usleep(10);
        }
    }

    return true;
}

bool quit_server = false;


static void interruptHandler(const int signal)
{
    enet_deinitialize();
    printf("\nQuit Orange.\n");
    quit_server = true;
}

struct ManagerContext
{
    FetchGame::ManagerState state;
    bool quit;
};

struct RecordingContext {
    std::string record_folder;
    std::string encoder_basic_setup;
};

void create_camera_manager(int* cam_count, ManagerContext* manager_context, GigEVisionDeviceInfo* unsorted_device_info, GigEVisionDeviceInfo* device_info, std::string* config_folder, RecordingContext* recording_setup, PTPParams *ptp_params) 
{
    // TODO: selecting cameras 
    CameraEmergent *ecams;
    CameraParams *cameras_params;
    std::vector<std::thread> camera_threads;
    std::vector<CameraResources> camera_resources;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    CameraEachSelect *cameras_select;
    CameraControl *camera_control = new CameraControl;

    manager_context->state = FetchGame::ManagerState_IDLE;
    while(!manager_context->quit) {
        switch (manager_context->state) {
            case FetchGame::ManagerState_CONNECT:
                *cam_count = scan_cameras(max_cameras, unsorted_device_info);
                std::cout << *cam_count << std::endl;
                sort_cameras_ip(unsorted_device_info, device_info, *cam_count);
                manager_context->state = FetchGame::ManagerState_CONNECTED;
                break;
            case FetchGame::ManagerState_OPENCAMERA:
                ecams = new CameraEmergent[*cam_count];
                cameras_params = new CameraParams[*cam_count];
                cameras_select = new CameraEachSelect[*cam_count];
                if (open_cameras(cameras_params, ecams, cameras_select, device_info, *cam_count, *config_folder)) 
                {
                    manager_context->state = FetchGame::ManagerState_CAMERAOPENED;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_STARTCAMTHREAD:
                if (start_camera_thread(camera_threads, camera_resources, recording_pipelines, cameras_params, ecams, camera_control, cameras_select, device_info, *cam_count, ptp_params, recording_setup->record_folder, recording_setup->encoder_basic_setup))
                {
                    manager_context->state = FetchGame::ManagerState_THREADREADY;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_ERROR:
                quit_server = true;
                break;
        }

        if (ptp_params->network_set_stop_ptp && ptp_params->ptp_stop_reached) {
            ptp_params->network_set_stop_ptp = false;
            for (auto &t : camera_threads)
                t.join();
            
            for (int i = 0; i < *cam_count; i++)
            {
                camera_threads.pop_back();
            }

            drain_and_shutdown_recording(recording_pipelines, camera_control);
            recording_pipelines.clear();

            if (camera_control->sync_camera) {
                for (int i = 0; i < *cam_count; i++)
                {
                    ptp_sync_off(&ecams[i].camera, &cameras_params[i]);
                }
            }
            ptp_params->ptp_global_time = 0;
            ptp_params->ptp_stop_time = 0;
            ptp_params->ptp_counter = 0;
            ptp_params->ptp_stop_counter = 0;
            ptp_params->network_sync = true;
            ptp_params->network_set_start_ptp = false;
            ptp_params->ptp_stop_reached = false;
            ptp_params->ptp_start_reached = false;
            camera_control->sync_camera = false;

            for (int i = 0; i < *cam_count; i++)
            {
                destroy_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, evt_buffer_size, &cameras_params[i]);
                delete[] ecams[i].evt_frame;
                check_camera_errors(EVT_CameraCloseStream(&ecams[i].camera), cameras_params[i].camera_serial.c_str());
                close_camera(&ecams[i].camera, &cameras_params[i]);
                camera_resources[i].cleanup();
            }
            camera_resources.clear();
            delete[] ecams;
            delete[] cameras_params;
            delete[] cameras_select;
            manager_context->state = FetchGame::ManagerState_RECORDSTOPPED;
        }
        usleep(1000);
    }
}


int main(int argc, char *argv[])
{
    if (enet_initialize() != 0)
    {
        quit_process(true, "ENET failed to initialize!");
    }

    signal(SIGINT, interruptHandler);

    EnetContext client;
    if (enet_initialize(&client, 3333, 1))
    {
        printf("Network Initialized!\n");
    }

    f32 last_time = tick();
    f32 current_time = tick();

    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);
    std::cout << "available no of cameras: " << cam_count << std::endl;

    flatbuffers::FlatBufferBuilder* fb_builder = new flatbuffers::FlatBufferBuilder(1024);
    std::string config_folder;
    RecordingContext recording_setup;
    ManagerContext manager_context;
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, true, false, false, false};

    std::thread* manager_thread = new std::thread(&create_camera_manager, &cam_count, &manager_context, unsorted_device_info, device_info, &config_folder, &recording_setup, ptp_params);
    
    while (!quit_server)
    {
        current_time = tick();
        // Handle All Incoming Packets and Send any enqued packets, does this need to be on another thread?
        service_network(&client, current_time - last_time, [&](const ENetEvent &evnt)
        {
            switch (evnt.type) 
            {
                //New connection request or an existing peer accepted our connection request
                case ENET_EVENT_TYPE_CONNECT:
                    {
                        if (manager_context.state == FetchGame::ManagerState_IDLE) {
                            printf("Network: Successfully connected! Rescaning cameras. \n");
                            manager_context.state = FetchGame::ManagerState_CONNECT; // rescan number of cams
                        } else {
                            printf("Network: Successfully connected! \n");
                            client_send_bringup_message(&client, fb_builder, evnt.peer, cam_count, manager_context.state);
                        }
                    }
                    break;
                //Server has sent us a new packet
                case ENET_EVENT_TYPE_RECEIVE:
                    {
                        printf ("\n A packet of length %u was received from %s on channel %u.\n",
                            evnt.packet -> dataLength,
                            evnt.peer -> data,
                            evnt.channelID);

                        uint8_t* buffer_pointer = evnt.packet->data;
                        auto server_control = FetchGame::GetServer(buffer_pointer);
                        auto server_signal = server_control->control();

                        if (server_signal == FetchGame::ServerControl_OPENCAMERA) {
                            config_folder = server_control->config_folder()->c_str();
                            manager_context.state = FetchGame::ManagerState_OPENCAMERA;
                        }
                        else if (server_signal == FetchGame::ServerControl_STARTTHREAD)
                        {
                            recording_setup.record_folder = server_control->record_folder()->c_str();
                            recording_setup.encoder_basic_setup = server_control->encoder_setup()->c_str();
                            manager_context.state = FetchGame::ManagerState_STARTCAMTHREAD;
                        } else if (server_signal == FetchGame::ServerControl_QUIT) {
                            printf("Exit \n");
                            quit_server = true;
                        } else if (server_signal == FetchGame::ServerControl_STARTRECORDING) {
                            ptp_params->ptp_global_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_global_time << std::endl;
                            ptp_params->network_set_start_ptp = true;
                            manager_context.state = FetchGame::ManagerState_WAITSTOP;
                            client_send_state_update_message(&client, fb_builder, evnt.peer, manager_context.state);
                        } else if (server_signal == FetchGame::ServerControl_STOPRECORDING) {
                            // stop recording
                            printf("stop signal\n");
                            std::cout << server_control->ptp_global_time() << std::endl;
                            ptp_params->ptp_stop_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_stop_time << std::endl;
                            ptp_params->network_set_stop_ptp = true;
                        }
                        enet_packet_destroy(evnt.packet);
                    }
                    break;

                //Server has disconnected
                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("Network: Server has disconnected!\n");
                    break;
            } });

        // coordinate with other thread
        if (manager_context.state == FetchGame::ManagerState_CONNECTED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_bringup_message(&client, fb_builder, &client.m_pNetwork->peers[0], cam_count, manager_context.state);
        }
        if (manager_context.state == FetchGame::ManagerState_CAMERAOPENED) {
            manager_context.state = FetchGame::ManagerState_WAITTHREAD;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_THREADREADY)
        {
            manager_context.state = FetchGame::ManagerState_WAITSTART;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_RECORDSTOPPED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        }
    
        usleep(1000);
        last_time = current_time;
    }

    manager_context.quit = true;
    manager_thread->join();

    // Disconnect
    enet_peer_disconnect(&client.m_pNetwork->peers[0], 0);
    uint8_t disconnected = false;
    /* Allow up to 3 seconds for the disconnect to succeed
     * and drop any packets received packets.
     */
    ENetEvent evnt;
    while (enet_host_service(client.m_pNetwork, &evnt, 3000) > 0)
    {
        switch (evnt.type)
        {
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(evnt.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            puts("Disconnection succeeded.");
            disconnected = true;
            break;
        }
    }
    

    // Drop connection, since disconnection didn't successed
    if (!disconnected)
    {
        enet_peer_reset(&client.m_pNetwork->peers[0]);
    }
    enet_host_destroy(client.m_pNetwork);
    enet_deinitialize();
    return 0;
}
