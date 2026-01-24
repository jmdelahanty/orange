// src/orange.cpp

#include "video_capture.h"
#include <iostream>
#include "camera.h"
#include "imgui.h"
#include "implot.h"
#include <ImGuiFileDialog.h>
#include "project.h"
#include "gui.h"
#include <sys/stat.h>
#include <cuda.h>
#include <unordered_map>
#include <cuda_runtime.h>
#include "NvEncoder/NvCodecUtils.h"
#include "network_base.h"
#include "enet_thread.h"
#include "yolo_worker.h"
#include "global.h"
#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "opengldisplay.h"
#include "image_writer_worker.h"
#include "yolov8_det.h"
#include "crop_and_encode_worker.h"
#include "frame_ipc_manager.h"

std::vector<YOLOv8Worker*> yolo_workers; // For managing YOLO workers
ENetPeer* external_data_consumer_peer = nullptr; // Store the peer for YOLO data
std::vector<SpeedTrackingData> speed_tracking_data;


void RenderSpeedGraph(int camera_id, YOLOv8Worker* yolo_worker, SpeedTrackingData& speed_data) {
    if (!yolo_worker) return;
    
    // Get current tracked objects from YOLO worker
    auto tracked_objects = yolo_worker->getTrackedObjects();
    
    // Update speed data WITH CM/S SPEEDS
    static float app_time = 0.0f;
    app_time += ImGui::GetIO().DeltaTime;
    
    for (const auto& obj : tracked_objects) {
        // USE CALIBRATED SPEED IN CM/S
        speed_data.AddSpeedData(obj.track_id, obj.current_speed_physical_units, app_time);
    }
    
    // Clean up old tracks
    speed_data.ClearOldTracks(app_time);
    
    // Render the speed graph
    ImGui::Separator();
    ImGui::Text("Object Speed Tracking");
    
    if (ImGui::CollapsingHeader("Speed Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float history = 10.0f;
        ImGui::SliderFloat("Time Window", &history, 2.0f, 30.0f, "%.1f s");
        
        // Make graph size responsive
        ImVec2 available = ImGui::GetContentRegionAvail();
        available.y -= 80;
        ImVec2 graph_size = ImVec2(-1, std::max(250.0f, available.y));
        
        // CHANGE PLOT TITLE AND Y-AXIS LABEL
        if (ImPlot::BeginPlot("Speed (cm/sec)", graph_size)) {
            // Setup axes
            ImPlot::SetupAxes("Time (s)", "Speed (cm/s)");
            ImPlot::SetupAxisLimits(ImAxis_X1, app_time - history, app_time, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, speed_data.max_speed_seen);
            
            // Plot each track
            for (const auto& [track_id, buffer] : speed_data.track_buffers) {
                if (buffer.Data.size() > 1) {
                    ImPlot::SetNextLineStyle(speed_data.track_colors.at(track_id), 2.0f);
                    std::string label = "Track " + std::to_string(track_id);
                    ImPlot::PlotLine(label.c_str(), 
                                   &buffer.Data[0].x, &buffer.Data[0].y, 
                                   buffer.Data.size(), 0, buffer.Offset, 
                                   2 * sizeof(float));
                }
            }
            
            ImPlot::EndPlot();
        }
        
        // CHANGE CURRENT SPEEDS DISPLAY TO CM/S
        if (!tracked_objects.empty()) {
            ImGui::Text("Current Speeds:");
            for (const auto& obj : tracked_objects) {
                ImVec4 color = speed_data.track_colors[obj.track_id];
                ImGui::TextColored(color, "Track %d: %.2f cm/s", 
                                 obj.track_id, obj.current_speed_physical_units);
            }
        } else {
            ImGui::TextDisabled("No objects being tracked");
        }
    }
}


simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

#define display_gpu_id 0

int main(int argc, char **args) {

    // Initialize the YOLOv8 plugins
    YOLOv8::initialize_plugins();

    gx_context *window = (gx_context *) malloc(sizeof(gx_context));
    *window = (gx_context){
        .swap_interval = 1,
        .width = 1920,
        .height = 1080,
        .render_target_title = (char *) "Orange",
        .glsl_version = (char *) malloc(100)
    };

    render_initialize_target(window);

    int max_cameras = 20;
    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);

    std::filesystem::path cwd = std::filesystem::current_path();
    std::string delimiter = "/";
    std::vector<std::string> tokenized_path = string_split(cwd, delimiter);
    std::string orange_root_dir_str = "/home/" + tokenized_path[2] + "/orange_data";
    prepare_application_folders(orange_root_dir_str);
    std::string input_folder = orange_root_dir_str + "/exp/unsorted";

    std::string yolo_model_folder = orange_root_dir_str + "/detect";
    std::string yolo_model = yolo_model_folder + "/fish_jinyao.engine";
    
    bool camera_is_selected[cam_count]{0};
    CameraParams *cameras_params;
    CameraEachSelect *cameras_select;
    CameraEmergent *ecams;
    std::vector<std::thread> camera_threads;
    GL_Texture *tex;
    GL_Texture* crop_tex;
    int num_cameras = 0;
    int stream_downsample = 1;
    CameraControl *camera_control = new CameraControl{false, false, false, false};

    int evt_buffer_size{100};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, false, false, false, false};
    COpenGLDisplay** openGLDisplayWorkers = nullptr;
    EncoderPreprocessWorker** encoderPreprocessWorkers = nullptr;
    EncoderHwWorker** encoderHWWorkers = nullptr;
    CropAndEncodeWorker** cropAndEncodeWorkers = nullptr;
    ImageWriterWorker* image_writer = new ImageWriterWorker("ImageSaverThread");
    image_writer->StartThread();

    std::vector<CameraResources> camera_resources;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;

    EncoderConfig *encoder_config = new EncoderConfig{
        "h264",
        "p1",
        "ll"
    };
    std::vector<std::string> camera_config_files;

    ScrollingBuffer *realtime_plot_data;
    bool show_realtime_plot = false;
    bool ptp_stream_sync = false;

    flatbuffers::FlatBufferBuilder *fb_builder = new flatbuffers::FlatBufferBuilder(1024);

    EnetContext server;
    if (enet_initialize(&server, 3333, 5)) {
        printf("Server Initiated\n");
    }
    ConnectedServer my_servers[2];
    intialize_servers(my_servers);

    INDIGOSignalBuilder indigo_signal_builder{};
    indigo_signal_builder = {
        .builder = fb_builder,
        .server = &server,
        .indigo_connection = nullptr
    };

    std::vector<std::string> network_config_folders;
    std::string network_start_folder_name = orange_root_dir_str + "/config/network";
    for (const auto &entry: std::filesystem::directory_iterator(network_start_folder_name)) {
        network_config_folders.push_back(entry.path().string());
    }
    int network_config_select = 0;

    std::vector<std::string> local_config_folders;
    std::string local_start_folder_name = orange_root_dir_str + "/config/local";
    for (const auto &entry: std::filesystem::directory_iterator(local_start_folder_name)) {
        local_config_folders.push_back(entry.path().string());
    }
    std::string picture_save_folder = orange_root_dir_str + "/pictures/" + get_current_date();
    std::string calib_save_folder = orange_root_dir_str + "/exp/calibration/" + get_current_date();

    int local_config_select = 0;
    bool select_all_cameras = false;
    char *temp_string = (char *) malloc(64);
    *temp_string = '\0';
    bool save_image_all_ready = true;
    bool quite_enet = false;

    std::thread enet_thread = std::thread(&create_enet_thread, &server, my_servers, &indigo_signal_builder,
                                          &quite_enet);
    std::vector<std::string> color_temps = { "CT_Off", "CT_2800K", "CT_3000K", "CT_4000K", "CT_5000K", "CT_6500K", "CT_Custom"};

    while (!glfwWindowShouldClose(window->render_target)) {
        create_new_frame();
        
        if (ImGui::Begin("Orange", nullptr, ImGuiWindowFlags_MenuBar)) {
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                       ImGui::GetIO().Framerate);

            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            if (ImGui::BeginTable("Cameras", 3,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_Borders)) {
                for (int i = 0; i < cam_count; i++) {
                    sprintf(temp_string, "%d", i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Selectable(temp_string, &camera_is_selected[i], ImGuiSelectableFlags_SpanAllColumns);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].serialNumber);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].currentIp);
                }
                ImGui::EndTable();
            }

            if (ImGui::Button(select_all_cameras ? "Clear all" : "Select all")) {
                select_all_cameras = !select_all_cameras;
                if (select_all_cameras) {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = true;
                    }
                } else {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = false;
                    }
                }
            }

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe) {
                // ImGui::BeginDisabled();
            }

            ImGui::Separator();
            ImGui::Spacing();

            // selection for yolo model
            if (ImGui::Button("Select YOLO")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = yolo_model_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseYOLOFile", "Choose File", ".engine", config);
            }
            ImGui::SameLine();
            ImGui::Text("%s", yolo_model.c_str());

            if (camera_control->subscribe) {
                // ImGui::EndDisabled();
            }

            if (camera_control->record_video) {
                // ImGui::BeginDisabled();
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.0f, 0.7f, 1.0f));
            if (ImGui::Button("Save to")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = input_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseRecordingDir", "Choose a Directory", nullptr, config);
            }
            ImGui::PopStyleColor(1); 
            ImGui::SameLine();
            ImGui::Text("%s", input_folder.c_str()); 

            {
                const char *items[] = {"h264", "hevc"};
                static int codec_current = 0;
                if (ImGui::Combo("codec", &codec_current, items, IM_ARRAYSIZE(items))) {
                    encoder_config->encoder_codec = items[codec_current];
                }
            }
            {
                const char *items[] = {"p1", "p3", "p5", "p7"};
                static int preset_current = 0;
                if (ImGui::Combo("preset", &preset_current, items, IM_ARRAYSIZE(items))) {
                    encoder_config->encoder_preset = items[preset_current];
                }
            }
            { // Scoped for clarity
                const char *items[] = {"hq", "ll", "ull", "lossless"};
                static int tuning_current = 1; // Default to "ll" (low latency)
                if (ImGui::Combo("tuning", &tuning_current, items, IM_ARRAYSIZE(items))) {
                    encoder_config->tuning_info = items[tuning_current];
                }
                // Ensure default is set on first run
                if (encoder_config->tuning_info.empty()) {
                    encoder_config->tuning_info = "ll";
                }
            }
            {
                const char *items[] = {"1", "2", "4", "8", "16"};
                static const int item_numbers[] = {1, 2, 4, 8, 16};
                static int downsample_current = 0;
                if(ImGui::Combo("downsample streaming", &downsample_current, items, IM_ARRAYSIZE(items))) {
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].downsample = item_numbers[downsample_current];
                    }
                }
            }

            int fps_temp = streaming_target_fps.load(); // get the current atomic value

            if (ImGui::InputInt("streaming fps", &fps_temp)) {
                // Clamp if necessary
                if (fps_temp < 1) fps_temp = 1;
                if (fps_temp > 240) fps_temp = 240;
                streaming_target_fps.store(fps_temp); // write it back safely
            }
            
   
            if (camera_control->record_video) {
                // ImGui::EndDisabled();
            }

            if (camera_control->open) {
                if (camera_control->record_video) {
                    // ImGui::BeginDisabled();
                }

                ImGui::Checkbox("Show camera temperature", &show_realtime_plot);

                set_camera_properties(ecams, cameras_params, num_cameras, color_temps);

                if (camera_control->record_video) {
                    // ImGui::EndDisabled();
                }

                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                bool stream_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].stream_on) {
                        stream_all_cameras = false;
                        break;
                    }
                }

                bool record_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].record) {
                        record_all_cameras = false;
                        break;
                    }
                }

                if (ImGui::BeginTable("Camera Control Setting", 7,
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_Borders)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Name");
                    ImGui::TableNextColumn();
                    ImGui::Text("Serial");
                    ImGui::TableNextColumn();
                    ImGui::Text("Stream "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##stream", &stream_all_cameras))
                    {
                        if (stream_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("Record "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##record", &record_all_cameras))
                    {
                        if (record_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO "); ImGui::SameLine();

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO Debug");

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO ENet");


                    for (int i = 0; i < num_cameras; i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_serial.c_str());
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_stream%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].stream_on);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_record%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].record);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_yolo%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].yolo);

                        ImGui::TableNextColumn();
                        if (cameras_select[i].yolo)
                        {
                            // Button is always visible and clickable if YOLO is selected.
                            sprintf(temp_string, "Dump Input##yolo_debug%d", i);
                            if (ImGui::Button(temp_string))
                            {
                                // We still check if the worker is ready before calling the function
                                // to prevent a crash, but the button is never grayed out.
                                if (camera_control->subscribe && i < yolo_workers.size() && yolo_workers[i])
                                {
                                    yolo_workers[i]->DumpNextFrame();
                                }
                                else
                                {
                                    std::cout << "Warning: Cannot dump frame. YOLO worker is not ready for camera "
                                              << cameras_params[i].camera_serial << std::endl;
                                }
                            }
                        }

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##yolo_enet%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].send_yolo_via_enet);
                    }
                    ImGui::EndTable();
                }

                // NEW: Add Frame IPC status section after the table
                if (camera_control->subscribe) {
                    bool any_frame_ipc_active = false;
                    for (int i = 0; i < num_cameras; i++) {
                        if (cameras_select[i].send_frame_ipc) {
                            any_frame_ipc_active = true;
                            break;
                        }
                    }
                    
                    if (any_frame_ipc_active) {
                        ImGui::Separator();
                        ImGui::Text("Frame IPC Status:");
                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].send_frame_ipc) {
                                ImGui::Text("  %s: /shm_cam_%u", 
                                           cameras_params[i].camera_serial.c_str(), 
                                           cameras_params[i].camera_id);
                            }
                        }
                        ImGui::TextDisabled("  Run './dummy_reader' to monitor");
                    }
                }

                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }

                if (camera_control->subscribe == true) {
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button("Picture save to")) {
                        make_folder(picture_save_folder);
                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].pictures_counter = 0;
                        }
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 1;
                        config.path = picture_save_folder;
                        ImGuiFileDialog::Instance()->OpenDialog("ChoosePictureDir", "Choose a Directory", nullptr,
                                                                config);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", picture_save_folder.c_str());
                    static int current_picture_format = 0;
                    const char* picture_format_items[] = { "jpg", "tiff", "png"};
                    ImGui::Combo("Picture format", &current_picture_format, picture_format_items, IM_ARRAYSIZE(picture_format_items));
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].frame_save_format = std::string(picture_format_items[current_picture_format]);
                    }

                    if (ImGui::TreeNode("Save pictures from capturing")) {
                        const int cols = 5;
                        for (int i = 0; i < num_cameras; ++i) {     
                            std::string label = cameras_params[i].camera_name + ": " + std::to_string(cameras_select[i].pictures_counter) + "##calibration_save";
                            if (ImGui::Selectable(label.c_str(), &cameras_select[i].selected_to_save,
                                                    ImGuiSelectableFlags_None,
                                                    ImVec2(150, 50))) {
                            }
    
                            // Keep items on the same line until end of row
                            if ((i + 1) % cols != 0)
                                ImGui::SameLine();
                        }
    
                        ImGui::NewLine();
    
                        ImGui::SeparatorText("Debug Info");
                        ImGui::Text("Window Focused: %d, Window Hovered: %d", ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow), ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow));
                        ImGui::Text("camera_control->subscribe = %s", camera_control->subscribe ? "true" : "false");
                        ImGui::Text("save_image_all_ready = %s", save_image_all_ready ? "true" : "false");
                        ImGui::Separator();
    
                        if (ImGui::Button("Save selected")) {
                            std::cout << "[GUI] 'Save selected' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                if (cameras_select[i].selected_to_save) {
                                    cameras_select[i].frame_save_state = State_Write_New_Frame;
                                    std::cout << "[GUI]   - Flagging camera " << cameras_params[i].camera_serial << " to save frame." << std::endl;
                                }
                            }
                        }
                        ImGui::SameLine();
    
                        if (ImGui::Button("Save pictures all")) {
                            std::cout << "[GUI] 'Save pictures all' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            std::cout << "[GUI]   - Flagging ALL cameras to save frame." << std::endl;
                        }
    

                        if (calib_state == CalibSavePictures) {
                            std::cout << "[GUI] Calibration state is 'CalibSavePictures', triggering next pose." << std::endl;
                            send_indigo_message(indigo_signal_builder.server, indigo_signal_builder.builder, indigo_signal_builder.indigo_connection, FetchGame::SignalType_CalibrationNextPose);
                            calib_state = CalibNextPose;
                        }
                        
                        if (calib_state == CalibPoseReached) {
                            std::cout << "[GUI] Calibration state is 'CalibPoseReached', triggering frame save for all cameras." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            calib_state = CalibSavePictures;
                        }

                        if (ImGui::Button("Calib save images with counter")) {
                            std::cout << "[GUI] 'Calib save images with counter' button clicked." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                        } 
                        
                        ImGui::TreePop();
                    }
                }
            }
        }
        ImGui::End();

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseYOLOFile")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                yolo_model = ImGuiFileDialog::Instance()->GetFilePathName();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseRecordingDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                input_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGuiFileDialog::Instance()->Display("ChoosePictureDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                picture_save_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGui::Begin("Local")) {
            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            for (size_t i = 0; i < local_config_folders.size(); i++) {
                std::vector<std::string> folder_token = string_split(local_config_folders[i], "/");
                sprintf(temp_string, "%s", folder_token.back().c_str());
                ImGui::RadioButton(temp_string, &local_config_select, i);
                ImGui::SameLine();
            }
            ImGui::RadioButton("Null", &local_config_select, local_config_folders.size());

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(camera_control->open ? "Close Camera" : "Open camera")) {
                if (!camera_control->open) {
                    if (local_config_select < local_config_folders.size()) {
                        update_camera_configs(camera_config_files, local_config_folders[local_config_select]);
                        select_cameras_have_configs(camera_config_files, device_info, camera_is_selected, cam_count);
                    }

                    num_cameras = 0;
                    for (int i = 0; i < cam_count; i++) {
                        if (camera_is_selected[i]) {
                            num_cameras++;
                        }
                    }
                    if (num_cameras > 0) {
                        camera_control->open = true;
                        cameras_params = new CameraParams[num_cameras];
                        cameras_select = new CameraEachSelect[num_cameras];

                        std::vector<int> selected_cameras;
                        for (int i = 0; i < cam_count; i++) {
                            if (camera_is_selected[i]) {
                                selected_cameras.push_back(i);
                            }
                        }

                        std::vector<bool> skip_setting_params;
                        skip_setting_params.resize(num_cameras);
                        for (int i = 0; i < num_cameras; i++) {
                            if (!set_camera_params(&cameras_params[i], &device_info[selected_cameras[i]],
                                                   camera_config_files, selected_cameras[i], num_cameras)) {
                                skip_setting_params[i] = true;
                                cameras_params[i].camera_id = selected_cameras[i];
                                cameras_params[i].num_cameras = num_cameras;
                            } else {
                                skip_setting_params[i] = false;

                            }
                        }


                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].stream_on = false;
                            if (cameras_params[i].camera_name == "ceiling_center") {
                                cameras_select[i].stream_on = true;
                                cameras_select[i].yolo = false;
                            }

                            if (cameras_params[i].camera_name == "shelter") {
                                cameras_select[i].stream_on = true;
                            }

                        }

                        ecams = new CameraEmergent[num_cameras];
                        for (int i = 0; i < num_cameras; i++) {
                            if (!skip_setting_params[i]) {
                                open_camera_with_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i]);
                            } else {
                                update_camera_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i]);
                            }

                        }
                        realtime_plot_data = new ScrollingBuffer[num_cameras];

                    }
                } else {
                    camera_control->open = false;
                    for (int i = 0; i < num_cameras; i++) {
                        close_camera(&ecams[i].camera, &cameras_params[i]);
                    }
                    delete[] cameras_params;
                    delete[] ecams;
                }
            }
            if (camera_control->subscribe) {
                ImGui::EndDisabled();
            }

            if (!camera_control->record_video && camera_control->open) {
                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }
                ImGui::Checkbox("PTP Stream Sync", &ptp_stream_sync);
                ImGui::SameLine();
                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }
                if (ImGui::Button(camera_control->subscribe ? "Stop streaming" : "Start streaming")) {
                    (camera_control->subscribe) = !(camera_control->subscribe);

                    if (camera_control->subscribe) {
                        // START STREAMING
                        std::cout << "STARTING STREAMING SESSION..." << std::endl;

                        if (std::any_of(cameras_select, cameras_select + num_cameras, [](const CameraEachSelect& cs){ return cs.record || cs.crop_and_encode; })) {
                            // Store the base folder for recordings, not the final timestamped one.
                            encoder_config->folder_name = input_folder;
                        }

                        // This part remains the same
                        camera_resources.resize(num_cameras);
                        frame_ipc_managers.clear();
                        frame_ipc_managers.resize(num_cameras);
                        size_t max_frame_size_bytes = 0;
                        for (int i = 0; i < num_cameras; ++i) {
                            size_t current_size = (size_t)cameras_params[i].width * (size_t)cameras_params[i].height;
                            if (current_size > max_frame_size_bytes) {
                                max_frame_size_bytes = current_size;
                            }
                        }
                        for (int i = 0; i < num_cameras; ++i) {
                            std::cout << "Initializing resources for camera " << i << " on GPU " << cameras_params[i].gpu_id << std::endl;
                            camera_resources[i].initialize(cameras_params[i].gpu_id, max_frame_size_bytes);
                            if (cameras_select[i].send_frame_ipc) {
                                frame_ipc_managers[i] = std::make_unique<FrameIPCManager>(&cameras_params[i]);
                                if (!frame_ipc_managers[i]->isEnabled()) {
                                    frame_ipc_managers[i].reset();
                                }
                            }
                        }
                        // Create worker thread objects and GPU textures
                        openGLDisplayWorkers = new COpenGLDisplay*[num_cameras]();
                        encoderPreprocessWorkers = new EncoderPreprocessWorker*[num_cameras]();
                        encoderHWWorkers = new EncoderHwWorker*[num_cameras]();
                        cropAndEncodeWorkers = new CropAndEncodeWorker*[num_cameras]();
                        tex = new GL_Texture[num_cameras];
                        crop_tex = new GL_Texture[num_cameras];
                        yolo_workers.assign(num_cameras, nullptr);

                        // Initialize all worker pointers to nullptr
                        for(int i = 0; i < num_cameras; ++i) {
                            openGLDisplayWorkers[i] = nullptr;
                            encoderPreprocessWorkers[i] = nullptr;
                            encoderHWWorkers[i] = nullptr;
                            cropAndEncodeWorkers[i] = nullptr;
                        }
                        cudaSetDevice(display_gpu_id);

                        // Allocate main textures for each camera's OpenGL display
                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                int w = (int)(cameras_params[i].width / cameras_select[i].downsample);
                                int h = (int)(cameras_params[i].height / cameras_select[i].downsample);
                                setup_texture(tex[i], w, h);
                            }
                        }

                        // Setup cropped textures for each crop/encode worker
                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].crop_and_encode) {
                                // The crop view has a fixed size of 256x256
                                setup_texture(crop_tex[i], 256, 256);
                            }
                        }

                        // CREATE AND LINK ALL WORKER THREADS
                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                std::string name = "OpenGLDisplay_Cam_" + cameras_params[i].camera_serial;
                                openGLDisplayWorkers[i] = new COpenGLDisplay(name.c_str(), &cameras_params[i], &cameras_select[i], tex[i].cuda_buffer, &indigo_signal_builder, *camera_resources[i].recycle_queue);
                            }
                            if (cameras_select[i].yolo) {
                                std::string name = "YOLO_Worker_Cam_" + cameras_params[i].camera_serial;
                                cameras_select[i].yolo_model = yolo_model.c_str();
                                yolo_workers[i] = new YOLOv8Worker(
                                    name.c_str(),
                                    &cameras_params[i],
                                    &cameras_select[i],
                                    camera_control,
                                    *camera_resources[i].recycle_queue
                                );
                                if (openGLDisplayWorkers[i]) {
                                    yolo_workers[i]->SetDisplayWorker(openGLDisplayWorkers[i]);
                                }
                            }
                            if (cameras_select[i].record) {
                                // The encoder pitch is required by the preprocess worker to prepare the frame correctly.
                                // We can get it by creating a temporary encoder instance.

                                // Get the current CUDA context for this thread
                                CUcontext cuContext;
                                ck(cuCtxGetCurrent(&cuContext));

                                // Now, use the local cuContext variable
                                NvEncoderCuda temp_enc(cuContext, cameras_params[i].width, cameras_params[i].height, NV_ENC_BUFFER_FORMAT_NV12);

                                // --- START: ADDED INITIALIZATION ---
                                // We must initialize the encoder to get valid parameters from it.
                                NV_ENC_INITIALIZE_PARAMS initializeParams = {NV_ENC_INITIALIZE_PARAMS_VER};
                                NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
                                initializeParams.encodeConfig = &encodeConfig;

                                // Use placeholder settings; we only need the pitch which is determined by resolution and format.
                                temp_enc.CreateDefaultEncoderParams(&initializeParams, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
                                temp_enc.CreateEncoder(&initializeParams);
                                // --- END: ADDED INITIALIZATION ---

                                const NvEncInputFrame* temp_frame = temp_enc.GetNextInputFrame();
                                int encoder_pitch = temp_frame->pitch;
                                temp_enc.DestroyEncoder();

                                std::string preprocess_name = "Preprocess_Cam_" + cameras_params[i].camera_serial;
                                encoderPreprocessWorkers[i] = new EncoderPreprocessWorker(
                                    preprocess_name.c_str(),
                                    &cameras_params[i],
                                    encoder_pitch,
                                    *camera_resources[i].recycle_queue,
                                    camera_control
                                );

                                std::string hw_encoder_name = "HW_Encoder_Cam_" + cameras_params[i].camera_serial;
                                encoderHWWorkers[i] = new EncoderHwWorker(
                                    hw_encoder_name.c_str(),
                                    &cameras_params[i],
                                    encoder_config->encoder_codec,
                                    encoder_config->encoder_preset,
                                    encoder_config->tuning_info,
                                    encoder_config->folder_name,
                                    encoderPreprocessWorkers[i], // Link to the preprocess worker
                                    camera_control
                                );
                                
                                // CRITICAL STEP: Link the two stages together
                                encoderPreprocessWorkers[i]->SetHwWorker(encoderHWWorkers[i]);
                            }

                            if (cameras_select[i].crop_and_encode) {
                                std::string name = "CropEncode_Cam_" + cameras_params[i].camera_serial;
                                cropAndEncodeWorkers[i] = new CropAndEncodeWorker(
                                    name.c_str(),
                                    &cameras_params[i],
                                    encoder_config->folder_name,
                                    *camera_resources[i].recycle_queue,
                                    crop_tex[i].cuda_buffer,
                                    camera_control
                                );
                                // Immediately link it to the YOLO worker if it exists
                                if (yolo_workers[i]) {
                                    yolo_workers[i]->SetCropAndEncodeWorker(cropAndEncodeWorkers[i]);
                                }
                            }
                        }

                        // START ALL WORKER THREADS
                        for (int i = 0; i < num_cameras; i++) {
                            if (openGLDisplayWorkers[i]) {
                                openGLDisplayWorkers[i]->SetMaxQueueSize(240); 
                                openGLDisplayWorkers[i]->StartThread();
                            }
                            if (yolo_workers[i]) {
                                yolo_workers[i]->SetMaxQueueSize(240);
                                yolo_workers[i]->StartThread();
                            }
                            if (encoderPreprocessWorkers[i]) {
                                encoderPreprocessWorkers[i]->SetMaxQueueSize(240);
                                encoderPreprocessWorkers[i]->StartThread();
                            }
                            if (encoderHWWorkers[i]) {
                                encoderHWWorkers[i]->SetMaxQueueSize(240);
                                encoderHWWorkers[i]->StartThread();
                            }
                            if (cropAndEncodeWorkers[i]) {
                                cropAndEncodeWorkers[i]->SetMaxQueueSize(240);
                                cropAndEncodeWorkers[i]->StartThread();
                            }
                        }

                        // PREPARE CAMERAS
                        for (int i = 0; i < num_cameras; i++) {
                            camera_open_stream(&ecams[i].camera, &cameras_params[i]);
                            ecams[i].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
                            allocate_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, &cameras_params[i], evt_buffer_size);
                        }
                        if (ptp_stream_sync) {
                            for (int i = 0; i < num_cameras; i++) {
                                ptp_camera_sync(&ecams[i].camera, &cameras_params[i]);
                            }
                            camera_control->sync_camera = true;
                        }

                        // Start acquisition threads
                        for (int i = 0; i < num_cameras; i++) {
                            camera_threads.emplace_back(
                                &acquire_frames,
                                &ecams[i],
                                &cameras_params[i],
                                &cameras_select[i],
                                camera_control,
                                ptp_params,
                                &indigo_signal_builder,
                                openGLDisplayWorkers[i],
                                encoderPreprocessWorkers[i],
                                yolo_workers[i],
                                image_writer,
                                &camera_resources[i],
                                frame_ipc_managers[i].get()
                            );
                        }
                    } else {
                        // STOP STREAMING
                        std::cout << "STOPPING STREAMING SESSION..." << std::endl;

                        // 1. Stop the acquisition threads first.
                        // This prevents new frames from entering the pipeline.
                        for (auto &t : camera_threads) {
                            if (t.joinable()) t.join();
                        }
                        camera_threads.clear();
                        std::cout << "Acquisition threads joined." << std::endl;

                        // RESET PTP STATE
                        if (ptp_stream_sync) {
                            ptp_params->ptp_global_time = 0;
                            ptp_params->ptp_stop_time = 0;
                            ptp_params->ptp_counter = 0;
                            ptp_params->ptp_stop_counter = 0;
                            ptp_params->network_sync = false;
                            ptp_params->network_set_start_ptp = false;
                            ptp_params->ptp_stop_reached = false;
                            ptp_params->ptp_start_reached = false;
                            camera_control->sync_camera = false; // Also reset this flag
                            std::cout << "PTPParams state has been reset for the next run." << std::endl;
                        }

                        // 2. Signal all worker threads to stop processing NEW data from their queues.
                        // They will finish processing whatever is currently in their queue.
                        for (int i = 0; i < num_cameras; i++) {
                            if (yolo_workers[i]) yolo_workers[i]->StopThread();
                            if (openGLDisplayWorkers[i]) openGLDisplayWorkers[i]->StopThread();
                            if (cropAndEncodeWorkers[i]) cropAndEncodeWorkers[i]->StopThread();
                            if (encoderHWWorkers[i]) encoderHWWorkers[i]->StopThread();
                            if (encoderPreprocessWorkers[i]) encoderPreprocessWorkers[i]->StopThread();
                        }
                        std::cout << "All worker threads signaled to stop. Waiting for queues to drain..." << std::endl;

                        // 3. Join and delete workers in REVERSE pipeline order to ensure the pipeline is drained.
                        for (int i = 0; i < num_cameras; i++) {
                            // Endpoints are first.
                            if (openGLDisplayWorkers[i]) {
                                delete openGLDisplayWorkers[i];
                                openGLDisplayWorkers[i] = nullptr;
                            }

                            if (cropAndEncodeWorkers[i]) {
                                std::cout << "Flushing final packets for crop encoder " << cameras_params[i].camera_serial << "..." << std::endl;
                                cropAndEncodeWorkers[i]->flush_and_close();
                                delete cropAndEncodeWorkers[i];
                                cropAndEncodeWorkers[i] = nullptr;
                            }
                            
                            // Now the hardware encoder, which is fed by the preprocessor.
                            if (encoderHWWorkers[i]) {
                                std::cout << "Flushing final packets for main HW encoder " << cameras_params[i].camera_serial << "..." << std::endl;
                                encoderHWWorkers[i]->flush_and_close(); // Ensure it has a flush_and_close method
                                delete encoderHWWorkers[i];
                                encoderHWWorkers[i] = nullptr;
                            }

                            // The YOLO worker can be deleted now.
                            if (yolo_workers[i]) {
                                delete yolo_workers[i];
                                yolo_workers[i] = nullptr;
                            }

                            // Finally, the preprocess worker which is at the start of the encoding pipeline.
                            if (encoderPreprocessWorkers[i]) {
                                delete encoderPreprocessWorkers[i];
                                encoderPreprocessWorkers[i] = nullptr;
                            }
                        }
                        
                        // Clear the worker pointer vectors
                        yolo_workers.clear();
                        if(openGLDisplayWorkers) { delete[] openGLDisplayWorkers; openGLDisplayWorkers = nullptr; }
                        if(encoderPreprocessWorkers) { delete[] encoderPreprocessWorkers; encoderPreprocessWorkers = nullptr; }
                        if(encoderHWWorkers) { delete[] encoderHWWorkers; encoderHWWorkers = nullptr; }
                        if(cropAndEncodeWorkers) { delete[] cropAndEncodeWorkers; cropAndEncodeWorkers = nullptr; }
                        std::cout << "Worker threads all cleaned up." << std::endl;
                        frame_ipc_managers.clear();

                        // 4. Final resource cleanup
                        for (int i = 0; i < num_cameras; i++) {
                            destroy_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, evt_buffer_size, &cameras_params[i]);
                            delete[] ecams[i].evt_frame;
                            ecams[i].evt_frame = nullptr;
                            check_camera_errors(EVT_CameraCloseStream(&ecams[i].camera), cameras_params[i].camera_serial.c_str());
                        }

                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                int w = int(cameras_params[i].width / cameras_select[i].downsample);
                                int h = int(cameras_params[i].height / cameras_select[i].downsample);
                                clear_upload_and_cleanup(tex[i], w, h);
                            }
                            // Add this block to clean up the crop textures
                            if (cameras_select[i].crop_and_encode) {
                                clear_upload_and_cleanup(crop_tex[i], 256, 256);
                            }
                        }

                        if(tex) delete[] tex;
                        tex = nullptr;
                        if(crop_tex) delete[] crop_tex;
                        crop_tex = nullptr;

                        for(int i = 0; i < num_cameras; ++i) {
                            camera_resources[i].cleanup();
                        }
                        camera_resources.clear();
                        std::cout << "Cleaned up all per-camera resources." << std::endl;
                    }
                }
            }

            if (camera_control->stop_record) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.5f, 0, 0, 1.0f});
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0.5f, 0, 1.0f});
            }

            if (camera_control->open) {

                if (!camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                if (ImGui::Button(camera_control->record_video ? ICON_FK_PAUSE : ICON_FK_PLAY)) {
                    camera_control->record_video = !camera_control->record_video;
                
                    if (camera_control->record_video) {
                        // START RECORDING
                        try_start_timer();
                        std::cout << "Recording toggled ON." << std::endl;
                    } else {
                        // STOP RECORDING
                        try_stop_timer();
                        std::cout << "Recording toggled OFF. Encoders will stop receiving frames." << std::endl;
                    }
                }
                
                if (!camera_control->subscribe) {
                    // ImGui::EndDisabled(); // This can be uncommented if you want to disable the button
                }
            }

            ImGui::PopStyleColor(1);
        }
        ImGui::End();


        if (camera_control->subscribe) {
            // Upload the texture data from the PBOs to the GPU textures
            for (int i = 0; i < num_cameras; i++) {
                if (cameras_select[i].stream_on) {
                    int camera_width = int(cameras_params[i].width / cameras_select[i].downsample);
                    int camera_height = int(cameras_params[i].height / cameras_select[i].downsample);
                    upload_texture_from_pbo(tex[i], camera_width, camera_height);
                }
                if (cameras_select[i].crop_and_encode) {
                    upload_texture_from_pbo(crop_tex[i], 256, 256);
                }
            }
            // Draw main camera views
            if (camera_control->record_video) {
                int64_t start_ns = record_start_time_ns.load();
                std::string g_formatted_elapsed_time;
                if (start_ns > 0) {
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                
                    auto elapsed_sec = std::chrono::seconds((now_ns - start_ns) / 1'000'000'000);
                    g_formatted_elapsed_time = format_elapsed_time(elapsed_sec);
                }
                // Resize speed tracking data
                if (speed_tracking_data.size() != num_cameras) {
                        speed_tracking_data.resize(num_cameras);
                }

                for (int i = 0; i < num_cameras; i++) {
                    
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());
                        
                        if (start_ns > 0) {
                            ImGui::TextColored(ImVec4{0.0, 1.0f, 0, 1.0f}, "Elapsed Time: %s", g_formatted_elapsed_time.c_str());
                        } else {
                            ImGui::TextColored(ImVec4{1.0, 1.0f, 0, 1.0f}, "Recording starting...");
                        }ImGui::SameLine();
                        if (yolo_workers[i])
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
                        ImGui::SameLine();
                        ImGui::Text("Streaming FPS: %.1f", streaming_fps.load());    
                        
                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
            
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        avail_size.y *= 0.5f;
    
                        static ImVec2 bmin(0, 0);
                        static ImVec2 uv0(0, 0);
                        static ImVec2 uv1(1, 1);
                        static ImVec4 tint(1, 1, 1, 1);
    
                        // ImGui::Image((void*)(intptr_t)texture[i], avail_size);
                        ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks |
                                                    ImPlotAxisFlags_NoGridLines;
                        if (ImPlot::BeginPlot("##no_plot_name", avail_size, ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            
                            // Calculate the correct display dimensions based on the downsample factor.
                            const float display_width = static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                            const float display_height = static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);

                            // Set the plot axes to match the dimensions of the image being displayed.
                            ImPlot::SetupAxesLimits(0, display_width, 0, display_height);
                            ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags); 
                            ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags);

                            // Tell ImPlot to draw the image using these correct dimensions.
                            ImPlot::PlotImage("##no_image_name", (void *) (intptr_t) tex[i].texture, ImVec2(0, 0),
                                              ImVec2(display_width, display_height));
                                              

                            ImPlot::EndPlot();
                        }

                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            RenderSpeedGraph(i, yolo_workers[i], speed_tracking_data[i]);
                        }
                        ImGui::End();
                    }
                }
            } else {
                for (int i = 0; i < num_cameras; i++) {
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());
                        ImGui::TextColored(ImVec4{1.0, 0.0f, 0, 1.0f}, "NOT RECORDING, ");
                        ImGui::SameLine();
                        ImGui::Text("Streaming FPS: %.1f", streaming_fps.load());    
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();

                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
    
                        static ImVec2 bmin(0, 0);
                        static ImVec2 uv0(0, 0);
                        static ImVec2 uv1(1, 1);
                        static ImVec4 tint(1, 1, 1, 1);
    
                        // ImGui::Image((void*)(intptr_t)texture[i], avail_size);
                        ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks |
                                                    ImPlotAxisFlags_NoGridLines;
                        if (ImPlot::BeginPlot("##no_plot_name", avail_size, ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            ImPlot::SetupAxesLimits(0, cameras_params[i].width, 0, cameras_params[i].height);
                            ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags); // X-axis
                            ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags); // Y-axis
                            ImPlot::PlotImage("##no_image_name", (void *) (intptr_t) tex[i].texture, ImVec2(0, 0),
                                                ImVec2(cameras_params[i].width, cameras_params[i].height));
                        
                            ImPlot::EndPlot();
                            
                        }
                        ImGui::End();
                    }
                }
            }
            for (int i = 0; i < num_cameras; i++) {
                    // Check if the crop and encode feature is enabled for this camera
                    if (cameras_select[i].crop_and_encode) {
                        // Create a unique name for the new window
                        std::string window_name = cameras_params[i].camera_name + " Crop";
                        ImGui::Begin(window_name.c_str());

                        // Use ImPlot to display the texture, just like the main view
                        if (ImPlot::BeginPlot("##crop_plot", ImGui::GetContentRegionAvail(), ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            ImPlot::PlotImage("##crop_image", (void*)(intptr_t)crop_tex[i].texture, ImVec2(0, 0), ImVec2(256, 256));
                            ImPlot::EndPlot();
                        }
                        ImGui::End();
                    }
                }
        }

        if (camera_control->open && show_realtime_plot) {
            ImGui::Begin("Realtime Plots"); {
                static float t = 0;
                t += ImGui::GetIO().DeltaTime;
                for (int i = 0; i < num_cameras; i++) {
                    get_senstemp_value(&ecams[i].camera, &cameras_params[i]);
                    realtime_plot_data[i].AddPoint(t, cameras_params[i].sens_temp);
                }

                static float history = 10.0f;
                ImGui::SliderFloat("History", &history, 1, 30, "%.1f s");

                static ImPlotAxisFlags flags = ImPlotAxisFlags_NoTickMarks;
                ImVec2 avail_size = ImGui::GetContentRegionAvail();

                if (ImPlot::BeginPlot("Camera Sensor Temperature", avail_size)) {
                    ImPlot::SetupAxes(nullptr, nullptr, flags, flags);
                    ImPlot::SetupAxisLimits(ImAxis_X1, t - history, t, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 30, 90);
                    ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);

                    for (int i = 0; i < num_cameras; i++) {
                        std::string line_name = std::string(cameras_params[i].camera_serial);
                        ImPlot::PlotLine(line_name.c_str(), &realtime_plot_data[i].Data[0].x,
                                         &realtime_plot_data[i].Data[0].y, realtime_plot_data[i].Data.size(), 0,
                                         realtime_plot_data[i].Offset, 2 * sizeof(float));
                    }
                    ImPlot::EndPlot();
                }
                ImGui::End();
            }
        }

        render_a_frame(window);
    }

    if (camera_control->open) {
        for (int i = 0; i < num_cameras; i++) {
            close_camera(&ecams[i].camera, &cameras_params[i]);
        }
        delete[] cameras_params;
        delete[] ecams;
    }

    std::cout << "GUI closed, initiating cleanup..." << std::endl;

    // 1. Signal the ENet thread to stop
    quite_enet = true;

    // 2. Join the ENet thread before exiting
    if (enet_thread.joinable()) {
        std::cout << "Waiting for ENet thread to finish..." << std::endl;
        enet_thread.join();
        std::cout << "ENet thread joined successfully." << std::endl;
    }

    // 3. Cleanup any remaining resources
    gx_cleanup(window);

    // 4. Free allocated memory
    free(window->glsl_version);
    free(window);

    std::cout << "Cleanup completed, exiting..." << std::endl;
}
