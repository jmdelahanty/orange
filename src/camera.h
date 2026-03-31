#ifndef ORANGE_CAMERA
#define ORANGE_CAMERA

#ifndef  EMERGENT_SDK
#include <EmergentCameraAPIs.h>
#include <emergentframe.h>
#include <EvtParamAttribute.h>
#include <gigevisiondeviceinfo.h>
#endif

#include <emergentcameradef.h>
#include <emergentgigevisiondef.h>
#include <EvtParamAttribute.h>
#include <unistd.h>
#include <cstdint>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <numeric>

struct CameraGpioNodeConfig {
    std::string name;
    std::string type = "enum";
    std::string value_string;
    bool value_bool = false;
    uint32_t value_uint = 0;
};

struct CameraParams{
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate;
    unsigned int gain;
    unsigned int exposure;
    unsigned int iris;
    unsigned int focus;
    std::string pixel_format;
    std::string color_temp;
    int gpu_id;
    int camera_id;
    std::string camera_name;
    std::string camera_serial;
    int num_cameras;
    bool gpu_direct;
    bool focus_uart_bootstrap = false;
    bool need_reorder;
    std::string config_schema_id;
    int config_schema_version = 0;
    std::string device_model;
    std::string camera_scan_type = "unknown";
    std::string gpio_connector_variant = "unknown";
    std::string gpio_recipe;
    std::string sync_mode = "free_run";
    bool trigger_enabled = false;
    std::string trigger_selector = "AcquisitionStart";
    std::string trigger_source = "Software";
    std::string trigger_activation = "RisingEdge";
    std::string ptp_mode;
    std::vector<CameraGpioNodeConfig> gpio_nodes;
    unsigned int gain_max; 
    unsigned int gain_min;
    unsigned int gain_inc;
    unsigned int exposure_max; 
    unsigned int exposure_min;
    unsigned int exposure_inc;
    unsigned int iris_max; 
    unsigned int iris_min;
    unsigned int iris_inc;    
    unsigned int frame_rate_max; 
    unsigned int frame_rate_min;
    unsigned int frame_rate_inc;
    unsigned int width_max; 
    unsigned int width_min;
    unsigned int width_inc;
    unsigned int height_max; 
    unsigned int height_min;
    unsigned int height_inc;
    unsigned int offsetx;
    unsigned int offsety;
    unsigned int offsetx_max; 
    unsigned int offsetx_min;
    unsigned int offsetx_inc;
    unsigned int offsety_max; 
    unsigned int offsety_min;
    unsigned int offsety_inc;
    unsigned int focus_max; 
    unsigned int focus_min;
    unsigned int focus_inc;
    bool color;
    int sens_temp;
    int sens_temp_max; 
    int sens_temp_min;
    std::string config_path;
}; 


std::string get_evt_error_string(EVT_ERROR error);

#define check_camera_errors(err, camera_serial) __check_camera_errors(err, camera_serial, __FILE__, __LINE__)

inline void __check_camera_errors(EVT_ERROR err, const char *camera_serial, const char *file, const int line) {
  if (EVT_SUCCESS != err) {
    const std::string error_string = get_evt_error_string(err);
    const std::string message =
        std::string(camera_serial) +
        " checkCameraErrors() Driver API error = " +
        std::to_string(static_cast<int>(err)) +
        " \"" + error_string + "\" from file <" +
        file + ">, line " + std::to_string(line) + ".";
    fprintf(stderr, "%s\n", message.c_str());
    throw std::runtime_error(message);
  }
}


struct CameraEmergent{
    Emergent::CEmergentCamera camera;
    Emergent::CEmergentFrame* evt_frame;
    Emergent::CEmergentFrame frame_recv;
    Emergent::CEmergentFrame frame_reorder;
};

struct PTPParams{
    unsigned long long ptp_global_time; 
    unsigned long long ptp_stop_time;
    uint64_t ptp_counter;
    uint64_t ptp_stop_counter;
    bool network_sync = false;
    bool ptp_start_reached = false;
    bool ptp_stop_reached = false;
    bool network_set_stop_ptp = false;
    bool network_set_start_ptp = false;
};

void print_camera_device_struct(GigEVisionDeviceInfo* device_info, int camera_idx);
void configure_factory_defaults(Emergent::CEmergentCamera* camera, CameraParams *camera_params);
void close_camera(Emergent::CEmergentCamera* camera, CameraParams *camera_params);
void open_camera_with_params(Emergent::CEmergentCamera* camera, GigEVisionDeviceInfo* device_info, CameraParams* camera_params);
void update_camera_params(Emergent::CEmergentCamera *camera, GigEVisionDeviceInfo *device_info, CameraParams *camera_params);
void allocate_frame_buffer(
    Emergent::CEmergentCamera* camera,
    Emergent::CEmergentFrame* evt_frame,
    CameraParams* camera_params,
    int buffer_size,
    int buffer_mode = EVT_FRAME_BUFFER_ZERO_COPY);
void set_frame_buffer(Emergent::CEmergentFrame* evt_frame, CameraParams* camera_params);
void destroy_frame_buffer(Emergent::CEmergentCamera* camera, Emergent::CEmergentFrame* evt_frame, int buffer_size, CameraParams *camera_params);
void ptp_camera_sync(Emergent::CEmergentCamera* camera, CameraParams *camera_params);
void ptp_sync_off(Emergent::CEmergentCamera *camera, CameraParams *camera_params);
bool camera_sync_mode_uses_ptp(const CameraParams* camera_params);
bool build_gpio_recipe_preview_nodes(const CameraParams* camera_params,
                                     std::vector<CameraGpioNodeConfig>* nodes_out,
                                     std::string* error_out);
void quick_print_camera(GigEVisionDeviceInfo* device_info, int camera_idx);
unsigned long long get_current_PTP_time(Emergent::CEmergentCamera* camera);
void test_gpo_manual_toggle(Emergent::CEmergentCamera* camera);
void change_camera_ip_persistent(GigEVisionDeviceInfo* device_info, Emergent::CEmergentCamera* camera, const char* new_ip, CameraParams *camera_params);
void update_color_temperature(Emergent::CEmergentCamera *camera, std::string color_string, CameraParams *camera_params);
void update_gain_value(Emergent::CEmergentCamera* camera, int gain_val, CameraParams* camera_params);
void update_exposure_value(Emergent::CEmergentCamera* camera, int exposure_val, CameraParams* camera_params);
void update_exposure_framerate_value(Emergent::CEmergentCamera *camera, int exposure_val, int* frame_rate_val, CameraParams *camera_params);
void update_frame_rate_value(Emergent::CEmergentCamera* camera, int frame_rate_val, CameraParams* camera_params);
void update_width_value(Emergent::CEmergentCamera* camera, int width_val, CameraParams* camera_params);
void update_height_value(Emergent::CEmergentCamera* camera, int height_val, CameraParams* camera_params);
void update_offsetX_value(Emergent::CEmergentCamera* camera, int OFFSET_X_VAL, CameraParams* camera_params);
void update_offsetY_value(Emergent::CEmergentCamera* camera, int OFFSET_Y_VAL, CameraParams* camera_params);
void update_focus_value(Emergent::CEmergentCamera* camera, int focus_value, CameraParams* camera_params);
void update_iris_value(Emergent::CEmergentCamera* camera, int iris_value, CameraParams* camera_params);
int scan_cameras(int max_cameras, GigEVisionDeviceInfo *device_info);
void allocate_frame_reorder_buffer(Emergent::CEmergentCamera* camera, Emergent::CEmergentFrame* frame_reorder, CameraParams* camera_params);
void camera_open_stream(Emergent::CEmergentCamera* camera, CameraParams *camera_params);
void sort_cameras_ip(GigEVisionDeviceInfo *device_info, GigEVisionDeviceInfo *sorted_device_info, int cam_count);
void get_senstemp_value(Emergent::CEmergentCamera *camera, CameraParams *camera_params);
#endif
