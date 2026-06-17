#include "project.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name))
    {
        if (const char* value = std::getenv(name_.c_str())) {
            had_original_ = true;
            original_ = value;
        }
    }

    ~ScopedEnv()
    {
        if (had_original_) {
            setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void Set(const std::string& value)
    {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void Unset()
    {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    bool had_original_ = false;
    std::string original_;
};

std::filesystem::path make_temp_dir()
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("orange_app_storage_config_tests_" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("failed to open " + path.string());
    }
    out << text;
}

nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open " + path.string());
    }
    nlohmann::json value;
    in >> value;
    return value;
}

AppStorageConfig load_from_path(const std::filesystem::path& path, std::string* error_out = nullptr)
{
    ScopedEnv app_config("ORANGE_APP_CONFIG_PATH");
    ScopedEnv gui_app_config("ORANGE_GUI_APP_CONFIG_PATH");
    app_config.Set(path.string());
    gui_app_config.Unset();

    AppStorageConfig config;
    std::string error;
    const bool ok = load_app_storage_config("/tmp/orange_data_for_app_config_tests", &config, &error);
    if (error_out) {
        *error_out = error;
    }
    if (!ok) {
        throw std::runtime_error(error.empty() ? "load_app_storage_config failed" : error);
    }
    return config;
}

void require_load_fails(const std::filesystem::path& path, const std::string& expected_error)
{
    ScopedEnv app_config("ORANGE_APP_CONFIG_PATH");
    ScopedEnv gui_app_config("ORANGE_GUI_APP_CONFIG_PATH");
    app_config.Set(path.string());
    gui_app_config.Unset();

    AppStorageConfig config;
    std::string error;
    const bool ok = load_app_storage_config("/tmp/orange_data_for_app_config_tests", &config, &error);
    require(!ok, "app config load should fail");
    require(
        error.find(expected_error) != std::string::npos,
        "error should contain '" + expected_error + "', got: " + error);
}

void test_missing_config_uses_defaults()
{
    const std::filesystem::path root = make_temp_dir();
    const AppStorageConfig config = load_from_path(root / "missing.json");

    require(config.gui_recording_sink_mode == "real", "default full-frame sink mode");
    require(
        !config.gui_recording_sink_mode_configured,
        "default full-frame sink mode should not count as app-configured");
    require(config.gui_recording_record_for_seconds == 0, "default record_for_seconds");
    require(config.gui_recording_clip_seconds == 0, "default clip_seconds");
    require(config.gui_crop_recording_sink_mode == "in_process", "default crop sink mode");
    require(config.gui_crop_external_encode_queue_depth == -1, "default crop queue unset");
    require(config.gui_crop_external_recorder_gpu_id == -1, "default crop recorder GPU unset");
    require(
        config.gui_crop_external_recorder_gpu_ids_by_serial.empty(),
        "default per-camera crop recorder GPU map unset");
    require(config.gui_crop_frame_pool_size == -1, "default crop frame pool unset");
    require(config.gui_ptp_register_read_decimate == 1, "default PTP decimate");
    require(config.gui_stream_downsample == -1, "default stream downsample unset");
    require(!config.gui_show_speed_graphs, "default speed graphs disabled");
    require(
        !config.gui_local_control_recording_start_enabled,
        "default local-control recording start disabled");
    require(
        !config.gui_local_control_recording_stop_enabled,
        "default local-control recording stop disabled");
    require(
        !config.gui_local_control_citrus_completion_stop_enabled,
        "default Citrus completion stop disabled");
    require(
        !config.gui_local_control_exit_after_finalize,
        "default local-control exit-after-finalize disabled");
    require(
        config.gui_local_control_drain_timeout_seconds == -1,
        "default local-control drain timeout unset");

    std::filesystem::remove_all(root);
}

void test_loads_gui_and_crop_defaults()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "models": {
    "default_detect_engine": "/tmp/detect.engine"
  },
  "recording": {
    "sink_mode": "external_ipc",
    "recording_control": {
      "record_for_seconds": 6,
      "clip_seconds": 2
    },
    "crop": {
      "sink_mode": "external_ipc",
      "frame_pool_size": 256,
      "external_ipc": {
        "encode_queue_depth": 128,
        "recorder_gpu_id": 4,
        "recorder_gpu_ids_by_serial": {
          "2010095": 8,
          "2010096": 6
        }
      }
    },
    "ptp_register_read_decimate": 100
  },
  "gui": {
    "stream": {
      "downsample": 4
    },
    "display": {
      "profile": "citrus_safe"
    },
    "telemetry": {
      "show_speed_graphs": true
    },
    "local_control": {
      "recording_start_enabled": true,
      "recording_stop_enabled": false,
      "citrus_completion_stop_enabled": true,
      "exit_after_finalize": false,
      "drain_timeout_seconds": 75
    }
  }
})json");

    const AppStorageConfig config = load_from_path(config_path);
    require(config.default_detect_engine == "/tmp/detect.engine", "detect engine should load");
    require(config.gui_recording_sink_mode == "external_ipc", "full-frame sink mode should load");
    require(config.gui_recording_sink_mode_configured, "full-frame sink mode should be app-configured");
    require(config.gui_recording_record_for_seconds == 6, "record_for_seconds should load");
    require(config.gui_recording_clip_seconds == 2, "clip_seconds should load");
    require(config.gui_crop_recording_sink_mode == "external_ipc", "crop sink mode should load");
    require(config.gui_crop_external_encode_queue_depth == 128, "crop queue depth should load");
    require(config.gui_crop_external_recorder_gpu_id == 4, "crop recorder GPU should load");
    require(
        config.gui_crop_external_recorder_gpu_ids_by_serial.size() == 2,
        "per-camera crop recorder GPU map should load");
    require(
        config.gui_crop_external_recorder_gpu_ids_by_serial.at("2010095") == 8,
        "per-camera crop recorder GPU should load for 2010095");
    require(
        config.gui_crop_external_recorder_gpu_ids_by_serial.at("2010096") == 6,
        "per-camera crop recorder GPU should load for 2010096");
    require(config.gui_crop_frame_pool_size == 256, "crop frame pool should load");
    require(config.gui_ptp_register_read_decimate == 100, "PTP decimate should load");
    require(config.gui_stream_downsample == 4, "GUI stream downsample should load");
    require(config.gui_display_profile == "citrus_safe", "display profile should normalize");
    require(config.gui_display_preview_max_fps == 10, "citrus-safe preview cap should apply");
    require(config.gui_swap_interval == 1, "citrus-safe swap interval should apply");
    require(config.gui_frame_max_fps == 30, "citrus-safe frame cap should apply");
    require(config.gui_show_speed_graphs, "speed graph flag should load");
    require(config.gui_local_control_recording_start_enabled, "local-control start should load");
    require(
        !config.gui_local_control_recording_stop_enabled,
        "explicit local-control stop false should load");
    require(
        config.gui_local_control_citrus_completion_stop_enabled,
        "Citrus completion stop should load");
    require(
        !config.gui_local_control_exit_after_finalize,
        "exit-after-finalize false should load");
    require(
        config.gui_local_control_drain_timeout_seconds == 75,
        "local-control drain timeout should load");

    std::filesystem::remove_all(root);
}

void test_loads_manual_gui_citrus_completion_profile()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "gui": {
    "local_control": {
      "recording_start_enabled": false,
      "recording_stop_enabled": false,
      "citrus_completion_stop_enabled": true,
      "exit_after_finalize": false,
      "drain_timeout_seconds": 60
    }
  }
})json");

    const AppStorageConfig config = load_from_path(config_path);
    require(
        !config.gui_local_control_recording_start_enabled,
        "manual GUI profile should keep socket start disabled");
    require(
        !config.gui_local_control_recording_stop_enabled,
        "manual GUI profile should keep generic socket stop disabled");
    require(
        config.gui_local_control_citrus_completion_stop_enabled,
        "manual GUI profile should accept Citrus completion stop");
    require(
        !config.gui_local_control_exit_after_finalize,
        "manual GUI profile should keep GUI open after finalization");
    require(
        config.gui_local_control_drain_timeout_seconds == 60,
        "manual GUI profile should load bounded local-control drain timeout");

    std::filesystem::remove_all(root);
}

void test_real_crop_sink_aliases_in_process()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "sink_mode": "real"
    }
  }
})json");

    const AppStorageConfig config = load_from_path(config_path);
    require(config.gui_crop_recording_sink_mode == "in_process", "real should alias in_process");

    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "sink_mode": "inprocess"
    }
  }
})json");

    const AppStorageConfig inprocess_config = load_from_path(config_path);
    require(
        inprocess_config.gui_crop_recording_sink_mode == "in_process",
        "inprocess should alias in_process");

    std::filesystem::remove_all(root);
}

void test_empty_app_sink_mode_is_not_configured()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "sink_mode": ""
  }
})json");

    const AppStorageConfig config = load_from_path(config_path);
    require(config.gui_recording_sink_mode == "real", "empty app sink should fall back to real");
    require(
        !config.gui_recording_sink_mode_configured,
        "empty app sink should allow camera preferred_sink_mode to resolve the session");

    std::filesystem::remove_all(root);
}

void test_camera_config_scan_ignores_editor_and_temp_files()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_dir = root / "Fred";
    std::filesystem::create_directories(config_dir);
    write_text(config_dir / "2012632.json", R"json({"device_serial_number":"2012632"})json");
    write_text(config_dir / ".2012632.json.swp", "not json");
    write_text(config_dir / ".hidden.json", R"json({"device_serial_number":"hidden"})json");
    write_text(config_dir / "2012632.json.tmp", "not json");
    write_text(config_dir / "notes.txt", "not json");
    std::filesystem::create_directories(config_dir / "nested.json");

    std::vector<std::string> camera_config_files;
    update_camera_configs(camera_config_files, config_dir.string());

    require(camera_config_files.size() == 1, "scanner should only include visible regular .json files");
    require(
        std::filesystem::path(camera_config_files[0]).filename() == "2012632.json",
        "scanner should include the Fred camera config");

    std::filesystem::remove_all(root);
}

void test_camera_config_loads_lens_control_flag()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "2012632.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.camera.config",
  "schema_version": 4,
  "device_serial_number": "2012632",
  "camera_scan_type": "area_scan",
  "gpio_connector_variant": "area_scan_12_pin",
  "gpio_recipe": "",
  "name": "Cam2012632",
  "width": 2464,
  "height": 2064,
  "frame_rate": 250,
  "gain": 256,
  "exposure": 300,
  "pixel_format": "BayerRG8",
  "color_temp": "CT_Off",
  "source_gpu_id": 0,
  "gpu_direct": true,
  "focus_uart_bootstrap": false,
  "lens_control_enabled": false,
  "optics": {
    "schema_id": "orange.camera.optics",
    "schema_version": 1,
    "lens": {
      "present": false,
      "focus_control": "none",
      "iris_control": "none",
      "notes": "Color camera has no controllable lens attached"
    },
    "filter_stack": [
      {
        "id": "hoya_r72_67mm",
        "manufacturer": "HOYA / Kenko Tokina Co., Ltd.",
        "model": "Creative Filter Infrared R72",
        "label": "67",
        "type": "infrared_longpass_filter",
        "thread_size": "67 mm",
        "state": "installed",
        "runtime_role": "normal_experiment_filter",
        "cutoff_wavelength_nm": 720.0
      }
    ]
  },
  "color": true,
  "focus": 0,
  "iris": 0,
  "recording": {
    "preferred_sink_mode": "external_ipc"
  },
  "sync_mode": "free_run",
  "trigger": {
    "enabled": false,
    "selector": "AcquisitionStart",
    "source": "Software",
    "activation": "RisingEdge"
  },
  "ptp": {
    "enabled": false
  },
  "gpio": {
    "nodes": []
  }
})json");

    CameraParams params{};
    load_camera_json_config_files(config_path.string(), &params, 0, 1);
    require(!params.lens_control_enabled, "lens_control_enabled=false should load");
    require(params.optics.lens.configured, "optics.lens should load");
    require(!params.optics.lens.present, "optics.lens.present=false should load");
    require(params.optics.lens.focus_control == "none", "lens focus_control should load");
    require(params.optics.filter_stack.size() == 1, "optics filter stack should load");
    require(params.optics.filter_stack.front().id == "hoya_r72_67mm", "filter id should load");
    require(
        params.optics.filter_stack.front().state == "installed",
        "runtime filter state should load");
    require(
        params.recording.preferred_sink_mode == "external_ipc",
        "recording.preferred_sink_mode should load");

    std::filesystem::remove_all(root);
}

void test_camera_config_lens_control_defaults_enabled()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "2012632.json";
    write_text(
        config_path,
        R"json({
  "name": "Cam2012632",
  "width": 2464,
  "height": 2064,
  "frame_rate": 250,
  "gain": 256,
  "exposure": 300,
  "pixel_format": "BayerRG8",
  "color_temp": "CT_Off",
  "source_gpu_id": 0,
  "gpu_direct": true,
  "color": true,
  "focus": 0,
  "iris": 0
})json");

    CameraParams params{};
    load_camera_json_config_files(config_path.string(), &params, 0, 1);
    require(params.lens_control_enabled, "legacy camera config should default lens control on");

    std::filesystem::remove_all(root);
}

void test_recording_snapshot_includes_camera_coordinate_frame()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path recording_folder = root / "recording";
    const std::filesystem::path config_path = root / "2010096.json";
    std::filesystem::create_directories(recording_folder);
    write_text(config_path, R"json({"device_serial_number":"2010096"})json");

    CameraParams params{};
    params.width = 4512;
    params.height = 4512;
    params.frame_rate = 100;
    params.gpu_id = 5;
    params.configured_gpu_id = 5;
    params.camera_id = 0;
    params.camera_serial = "2010096";
    params.camera_name = "Cam2010096";
    params.pixel_format = "Mono8";
    params.gpu_direct = true;
    params.config_path = config_path.string();

    require(
        write_recording_snapshot(
            recording_folder.string(),
            "coord_frame_test",
            &params,
            1,
            root.string(),
            false),
        "recording snapshot should write");

    const nlohmann::json snapshot = read_json(recording_folder / "recording_snapshot.json");
    const nlohmann::json& frame =
        snapshot.at("camera_runtime").at("2010096").at("coordinate_frame");

    require(frame.value("schema_version", 0) == 1, "coordinate frame schema version");
    require(
        frame.value("coordinate_space", "") == "camera_native_pixels",
        "coordinate frame names camera_native_pixels");
    require(frame.value("units", "") == "pixels", "coordinate frame units");
    require(frame.at("origin").value("name", "") == "top_left_pixel", "coordinate origin");
    require(
        frame.at("axes").at("x").value("positive_direction", "") == "right",
        "x axis direction");
    require(
        frame.at("axes").at("y").value("positive_direction", "") == "down",
        "y axis direction");
    require(frame.value("point_order", "") == "xy", "point order");
    require(frame.at("pixel_indexing").value("index_base", -1) == 0, "zero-based pixel indexing");
    require(
        frame.at("pixel_indexing").value("valid_x_index_max", 0) == 4511,
        "x index max follows width");
    require(
        frame.at("pixel_indexing").value("valid_y_index_max", 0) == 4511,
        "y index max follows height");
    require(frame.at("extent").value("width_px", 0) == 4512, "extent width");
    require(frame.at("extent").value("height_px", 0) == 4512, "extent height");
    require(
        frame.at("extent").value("x_max_exclusive_px", 0) == 4512,
        "exclusive x extent");
    require(
        frame.at("extent").value("y_max_exclusive_px", 0) == 4512,
        "exclusive y extent");

    std::filesystem::remove_all(root);
}

void test_invalid_crop_sink_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "sink_mode": "banana"
    }
  }
})json");

    require_load_fails(config_path, "recording.crop.sink_mode must be real, in_process, or external_ipc");
    std::filesystem::remove_all(root);
}

void test_invalid_crop_queue_depth_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "external_ipc": {
        "encode_queue_depth": 0
      }
    }
  }
})json");

    require_load_fails(config_path, "recording.crop.external_ipc.encode_queue_depth must be in [1,4096]");
    std::filesystem::remove_all(root);
}

void test_invalid_crop_recorder_gpu_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "external_ipc": {
        "recorder_gpu_id": 256
      }
    }
  }
})json");

    require_load_fails(config_path, "recording.crop.external_ipc.recorder_gpu_id must be in [0,255]");
    std::filesystem::remove_all(root);
}

void test_invalid_crop_recorder_gpu_map_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "recording": {
    "crop": {
      "external_ipc": {
        "recorder_gpu_ids_by_serial": {
          "2010095": "gpu8"
        }
      }
    }
  }
})json");

    require_load_fails(
        config_path,
        "recording.crop.external_ipc.recorder_gpu_ids_by_serial.2010095 must be an integer");
    std::filesystem::remove_all(root);
}

void test_invalid_stream_downsample_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "gui": {
    "stream": {
      "downsample": 3
    }
  }
})json");

    require_load_fails(config_path, "gui.stream.downsample must be one of 1, 2, 4, 8, or 16");
    std::filesystem::remove_all(root);
}

void test_invalid_speed_graph_type_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "gui": {
    "telemetry": {
      "show_speed_graphs": 1
    }
  }
})json");

    require_load_fails(config_path, "gui.telemetry.show_speed_graphs must be a boolean");
    std::filesystem::remove_all(root);
}

void test_invalid_local_control_field_fails()
{
    const std::filesystem::path root = make_temp_dir();
    const std::filesystem::path config_path = root / "default.json";
    write_text(
        config_path,
        R"json({
  "schema_id": "orange.app.config",
  "schema_version": 1,
  "gui": {
    "local_control": {
      "citrus_completion_stop_enabled": "yes"
    }
  }
})json");

    require_load_fails(
        config_path,
        "gui.local_control.citrus_completion_stop_enabled must be a boolean");
    std::filesystem::remove_all(root);
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"missing_config_uses_defaults", &test_missing_config_uses_defaults},
        {"loads_gui_and_crop_defaults", &test_loads_gui_and_crop_defaults},
        {"loads_manual_gui_citrus_completion_profile",
         &test_loads_manual_gui_citrus_completion_profile},
        {"real_crop_sink_aliases_in_process", &test_real_crop_sink_aliases_in_process},
        {"empty_app_sink_mode_is_not_configured",
         &test_empty_app_sink_mode_is_not_configured},
        {"camera_config_scan_ignores_editor_and_temp_files",
         &test_camera_config_scan_ignores_editor_and_temp_files},
        {"camera_config_loads_lens_control_flag", &test_camera_config_loads_lens_control_flag},
        {"camera_config_lens_control_defaults_enabled",
         &test_camera_config_lens_control_defaults_enabled},
        {"recording_snapshot_includes_camera_coordinate_frame",
         &test_recording_snapshot_includes_camera_coordinate_frame},
        {"invalid_crop_sink_fails", &test_invalid_crop_sink_fails},
        {"invalid_crop_queue_depth_fails", &test_invalid_crop_queue_depth_fails},
        {"invalid_crop_recorder_gpu_fails", &test_invalid_crop_recorder_gpu_fails},
        {"invalid_crop_recorder_gpu_map_fails", &test_invalid_crop_recorder_gpu_map_fails},
        {"invalid_stream_downsample_fails", &test_invalid_stream_downsample_fails},
        {"invalid_speed_graph_type_fails", &test_invalid_speed_graph_type_fails},
        {"invalid_local_control_field_fails", &test_invalid_local_control_field_fails},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All app storage config tests passed.\n";
    return EXIT_SUCCESS;
}
