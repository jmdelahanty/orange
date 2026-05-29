#include "project.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
    }
  }
})json");

    const AppStorageConfig config = load_from_path(config_path);
    require(config.default_detect_engine == "/tmp/detect.engine", "detect engine should load");
    require(config.gui_recording_sink_mode == "external_ipc", "full-frame sink mode should load");
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
        {"real_crop_sink_aliases_in_process", &test_real_crop_sink_aliases_in_process},
        {"invalid_crop_sink_fails", &test_invalid_crop_sink_fails},
        {"invalid_crop_queue_depth_fails", &test_invalid_crop_queue_depth_fails},
        {"invalid_crop_recorder_gpu_fails", &test_invalid_crop_recorder_gpu_fails},
        {"invalid_crop_recorder_gpu_map_fails", &test_invalid_crop_recorder_gpu_map_fails},
        {"invalid_stream_downsample_fails", &test_invalid_stream_downsample_fails},
        {"invalid_speed_graph_type_fails", &test_invalid_speed_graph_type_fails},
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
