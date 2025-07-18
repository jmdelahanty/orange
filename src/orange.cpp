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
#include <cuda_runtime.h>
#include "NvEncoder/NvCodecUtils.h"
#include "network_base.h"
#include "enet_thread.h"
#include "yolo_worker.h"
#include "global.h"
#include "gpu_video_encoder.h"
#include "opengldisplay.h"
#include "image_writer_worker.h"
#include "yolov8_det.h"
#include "crop_and_encode_worker.h"
#include "dispatcher_worker.h"
#include "preprocess_worker.h"

std::vector<YOLOv8Worker*> yolo_workers; // For managing YOLO workers
ENetPeer* external_data_consumer_peer = nullptr; // Store the peer for YOLO data

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
    int num_cameras = 0;
    int stream_downsample = 1;
    CameraControl *camera_control = new CameraControl{false, false, false, false};

    int evt_buffer_size{100};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, false, false, false, false};
    COpenGLDisplay** openGLDisplayWorkers = nullptr;
    GPUVideoEncoder** gpuVideoEncoders = nullptr;
    CropAndEncodeWorker** cropAndEncodeWorkers = nullptr;
    PreprocessWorker** preprocessWorkers = nullptr;
    DispatcherWorker** dispatcherWorkers = nullptr;
    ImageWriterWorker* image_writer = new ImageWriterWorker("ImageSaverThread");
    image_writer->StartThread();

    std::vector<CameraResources> camera_resources;

    EncoderConfig *encoder_config = new EncoderConfig{
        "h264",
        "p1"
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

        if (ptp_params->network_set_stop_ptp && ptp_params->ptp_stop_reached) {
            ptp_params->network_set_stop_ptp = false;

            for (int i = 0; i < num_cameras; i++) {
                if (cameras_select[i].stream_on) {
                    int camera_width = int(cameras_params[i].width / cameras_select[i].downsample);
                    int camera_height = int(cameras_params[i].height / cameras_select[i].downsample);
                    clear_upload_and_cleanup(tex[i], camera_width, camera_height);
                }
            }
            delete[] tex;

            for (auto &t: camera_threads)
                t.join();

            for (int i = 0; i < num_cameras; i++) {
                camera_threads.pop_back();
            }
            for (int i = 0; i < num_cameras; i++) {
                destroy_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, evt_buffer_size, &cameras_params[i]);
                delete[] ecams[i].evt_frame;
                check_camera_errors(EVT_CameraCloseStream(&ecams[i].camera), cameras_params[i].camera_serial.c_str());
            }

            for (int i = 0; i < num_cameras; i++) {
                ptp_sync_off(&ecams[i].camera, &cameras_params[i]);
            }
            camera_control->sync_camera = false;
            camera_control->record_video = false;

            ptp_params->ptp_global_time = 0;
            ptp_params->ptp_stop_time = 0;
            ptp_params->ptp_counter = 0;
            ptp_params->ptp_stop_counter = 0;
            ptp_params->network_sync = false;
            ptp_params->network_set_start_ptp = false;
            ptp_params->ptp_stop_reached = false;
            ptp_params->ptp_start_reached = false;

            for (int i = 0; i < num_cameras; i++) {
                close_camera(&ecams[i].camera, &cameras_params[i]);
            }

            camera_control->open = false;

            for (int i = 0; i < cam_count; i++) {
                camera_is_selected[i] = 0;
            }
        }

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

                if (ImGui::BeginTable("Camera Control Setting", 8,
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
                    ImGui::Text("YOLO IPC");

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO ENet");
                    ImGui::TableNextColumn();
                    ImGui::Text("Crop & Encode");


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
                        else
                        {
                            // Add an empty cell to keep table alignment correct
                            ImGui::Text("");
                        }

                        // New Checkboxes for IPC and ENet
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##yolo_ipc%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].send_yolo_via_ipc); // Assumes flag name from video_capture.h

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##yolo_enet%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].send_yolo_via_enet); // Assumes flag name from video_capture.h

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##crop_encode%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].crop_and_encode);
                    }
                    ImGui::EndTable();
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
                // ImGui::BeginDisabled();
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
                // ImGui::BeginDisabled();
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

                        if (std::any_of(cameras_select, cameras_select + num_cameras, [](const CameraEachSelect& cs){ return cs.record || cs.crop_and_encode; })) { // Modified this line slightly for correctness
                            encoder_config->folder_name = input_folder + "/" + get_current_date_time();
                            make_folder(encoder_config->folder_name);
                            std::cout << "Recording session folder: " << encoder_config->folder_name << std::endl;
                        }

                        camera_resources.resize(num_cameras);
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
                        }
                        
                        // 1. Initialize worker pointer arrays
                        openGLDisplayWorkers = new COpenGLDisplay*[num_cameras]();
                        gpuVideoEncoders = new GPUVideoEncoder*[num_cameras]();
                        cropAndEncodeWorkers = new CropAndEncodeWorker*[num_cameras]();
                        preprocessWorkers = new PreprocessWorker*[num_cameras]();
                        dispatcherWorkers = new DispatcherWorker*[num_cameras]();
                        yolo_workers.assign(num_cameras, nullptr);

                        tex = new GL_Texture[num_cameras];
                        yolo_workers.assign(num_cameras, nullptr);
                        for(int i = 0; i < num_cameras; ++i) {
                            openGLDisplayWorkers[i] = nullptr;
                            gpuVideoEncoders[i] = nullptr;
                            cropAndEncodeWorkers[i] = nullptr;
                        }
                        cudaSetDevice(display_gpu_id);
                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                int w = (int)(cameras_params[i].width / cameras_select[i].downsample);
                                int h = (int)(cameras_params[i].height / cameras_select[i].downsample);
                                setup_texture(tex[i], w, h);
                            }
                        }

                        // 2. CREATE AND LINK ALL WORKER THREADS (Bottom-up: consumers first)
                        for (int i = 0; i < num_cameras; i++) {
                            // Create Consumers
                            if (cameras_select[i].stream_on) {
                                std::string name = "OpenGLDisplay_Cam_" + cameras_params[i].camera_serial;
                                openGLDisplayWorkers[i] = new COpenGLDisplay(name.c_str(), &cameras_params[i], &cameras_select[i], tex[i].cuda_buffer, &indigo_signal_builder, *camera_resources[i].recycle_queue);
                            }
                            if (cameras_select[i].record) {
                                std::string name = "GPUEncoder_Cam_" + cameras_params[i].camera_serial;
                                bool ready_signal = false; // This signal might be handled differently now or removed
                                gpuVideoEncoders[i] = new GPUVideoEncoder(name.c_str(), &cameras_params[i], encoder_config->encoder_codec, encoder_config->encoder_preset, encoder_config->tuning_info, encoder_config->folder_name, &ready_signal, *camera_resources[i].recycle_queue);
                            }
                            if (cameras_select[i].yolo) {
                                std::string name = "YOLO_Worker_Cam_" + cameras_params[i].camera_serial;
                                cameras_select[i].yolo_model = yolo_model.c_str();
                                yolo_workers[i] = new YOLOv8Worker(name.c_str(), &cameras_params[i], &cameras_select[i], camera_control, *camera_resources[i].recycle_queue);
                            }
                            if (cameras_select[i].crop_and_encode) {
                                std::string name = "CropEncode_Cam_" + cameras_params[i].camera_serial;
                                unsigned char* pbo_ptr = (cameras_select[i].stream_on) ? tex[i].cuda_buffer : nullptr;
                                cropAndEncodeWorkers[i] = new CropAndEncodeWorker(name.c_str(), &cameras_params[i], encoder_config->folder_name, *camera_resources[i].recycle_queue, pbo_ptr);
                            }


                            // Create Dispatcher and link consumers
                            std::string dispatcher_name = "Dispatcher_Cam_" + cameras_params[i].camera_serial;
                            dispatcherWorkers[i] = new DispatcherWorker(
                                dispatcher_name.c_str(),
                                &cameras_select[i],
                                camera_control,
                                openGLDisplayWorkers[i],  // Can be null
                                gpuVideoEncoders[i],      // Can be null
                                yolo_workers[i],          // Can be null
                                *camera_resources[i].recycle_queue
                            );
                            
                            // Create Preprocessor and link to Dispatcher
                            std::string preprocessor_name = "Preprocessor_Cam_" + cameras_params[i].camera_serial;
                            preprocessWorkers[i] = new PreprocessWorker(
                                preprocessor_name.c_str(),
                                &cameras_params[i],
                                *camera_resources[i].recycle_queue
                            );
                            preprocessWorkers[i]->SetNextWorker(dispatcherWorkers[i]);

                            // Link YOLO to its potential consumers
                            if(yolo_workers[i]){
                                yolo_workers[i]->SetDisplayWorker(openGLDisplayWorkers[i]); // Can be null
                                yolo_workers[i]->SetCropAndEncodeWorker(cropAndEncodeWorkers[i]); // Can be null
                            }

                        }

                        // 3. START ALL WORKER THREADS (in pipeline order for clarity)
                        for (int i = 0; i < num_cameras; i++) {
                            if (preprocessWorkers[i]) preprocessWorkers[i]->StartThread();
                            if (dispatcherWorkers[i]) dispatcherWorkers[i]->StartThread();
                            if (yolo_workers[i]) yolo_workers[i]->StartThread();
                            if (openGLDisplayWorkers[i]) openGLDisplayWorkers[i]->StartThread();
                            if (gpuVideoEncoders[i]) gpuVideoEncoders[i]->StartThread();
                            if (cropAndEncodeWorkers[i]) cropAndEncodeWorkers[i]->StartThread();

                        }

                        // PREPARE CAMERAS (no changes here)
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

                        // Start acquisition threads (no changes here)
                        for (int i = 0; i < num_cameras; i++) {
                            camera_threads.emplace_back(
                                &acquire_frames,
                                &ecams[i],
                                &cameras_params[i],
                                &cameras_select[i],
                                camera_control,
                                ptp_params,
                                &indigo_signal_builder,
                                preprocessWorkers[i],
                                image_writer,
                                &camera_resources[i]
                            );
                        }
                    } else {
                        // STOP STREAMING
                        std::cout << "STOPPING STREAMING SESSION..." << std::endl;

                        // 1. Stop the acquisition threads first.
                        // This is the most important step to prevent new frames from entering the pipeline.
                        for (auto &t : camera_threads) {
                            if (t.joinable()) t.join();
                        }
                        camera_threads.clear();
                        std::cout << "Acquisition threads joined." << std::endl;

                        // 2. Signal all worker threads to stop processing NEW data from their input queues.
                        // They will finish processing whatever is currently in their queues.
                        for (int i = 0; i < num_cameras; i++) {
                            if (preprocessWorkers && preprocessWorkers[i]) preprocessWorkers[i]->StopThread();
                            if (dispatcherWorkers && dispatcherWorkers[i]) dispatcherWorkers[i]->StopThread();
                            if (yolo_workers[i]) yolo_workers[i]->StopThread();
                            if (openGLDisplayWorkers && openGLDisplayWorkers[i]) openGLDisplayWorkers[i]->StopThread();
                            if (cropAndEncodeWorkers && cropAndEncodeWorkers[i]) cropAndEncodeWorkers[i]->StopThread();
                            if (gpuVideoEncoders && gpuVideoEncoders[i]) gpuVideoEncoders[i]->StopThread();
                        }
                        std::cout << "All worker threads signaled to stop. Waiting for queues to drain..." << std::endl;

                        // 3. Delete workers in REVERSE pipeline order to ensure the pipeline is drained correctly.
                        // Consumers -> Dispatcher -> Preprocessor
                        for (int i = 0; i < num_cameras; i++) {
                            // Endpoints (consumers) are first.
                            if (openGLDisplayWorkers && openGLDisplayWorkers[i]) {
                                delete openGLDisplayWorkers[i];
                                openGLDisplayWorkers[i] = nullptr;
                            }
                            if (cropAndEncodeWorkers && cropAndEncodeWorkers[i]) {
                                cropAndEncodeWorkers[i]->flush_and_close();
                                delete cropAndEncodeWorkers[i];
                                cropAndEncodeWorkers[i] = nullptr;
                            }
                            if (yolo_workers[i]) {
                                delete yolo_workers[i];
                                yolo_workers[i] = nullptr;
                            }
                            if (gpuVideoEncoders && gpuVideoEncoders[i]) {
                                gpuVideoEncoders[i]->flush_and_close();
                                delete gpuVideoEncoders[i];
                                gpuVideoEncoders[i] = nullptr;
                            }

                            // Now it's safe to delete the dispatcher that fed them.
                            if (dispatcherWorkers && dispatcherWorkers[i]) {
                                delete dispatcherWorkers[i];
                                dispatcherWorkers[i] = nullptr;
                            }

                            // Finally, delete the preprocessor that fed the dispatcher.
                            if (preprocessWorkers && preprocessWorkers[i]) {
                                delete preprocessWorkers[i];
                                preprocessWorkers[i] = nullptr;
                            }
                        }

                        // Clear the worker pointer vectors and arrays
                        yolo_workers.clear();
                        if(openGLDisplayWorkers) { delete[] openGLDisplayWorkers; openGLDisplayWorkers = nullptr; }
                        if(gpuVideoEncoders) { delete[] gpuVideoEncoders; gpuVideoEncoders = nullptr; }
                        if(cropAndEncodeWorkers) { delete[] cropAndEncodeWorkers; cropAndEncodeWorkers = nullptr; }
                        if(dispatcherWorkers) { delete[] dispatcherWorkers; dispatcherWorkers = nullptr; }
                        if(preprocessWorkers) { delete[] preprocessWorkers; preprocessWorkers = nullptr; }
                        std::cout << "Worker threads all cleaned up." << std::endl;

                        // 4. Final resource cleanup (camera buffers, textures, resource pools).
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
                        }
                        if(tex) { delete[] tex; tex = nullptr; }

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
            for (int i = 0; i < num_cameras; i++) {
                if (cameras_select[i].stream_on) {
                    int camera_width = int(cameras_params[i].width / cameras_select[i].downsample);
                    int camera_height = int(cameras_params[i].height / cameras_select[i].downsample);
                    upload_texture_from_pbo(tex[i], camera_width, camera_height);
                }
            }

            if (camera_control->record_video) {
                int64_t start_ns = record_start_time_ns.load();
                std::string g_formatted_elapsed_time;
                if (start_ns > 0) {
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                
                    auto elapsed_sec = std::chrono::seconds((now_ns - start_ns) / 1'000'000'000);
                    g_formatted_elapsed_time = format_elapsed_time(elapsed_sec);
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
                        if (gpuVideoEncoders[i])
                        {
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Encoding FPS: %.1f", gpuVideoEncoders[i]->get_fps());
                        }
                        
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