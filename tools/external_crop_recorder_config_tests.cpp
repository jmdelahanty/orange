#include "session/external_crop_recorder_config.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
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

void default_uses_analytics_gpu()
{
    ScopedEnv global("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID");
    ScopedEnv per_camera("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095");
    global.Unset();
    per_camera.Unset();

    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 5,
        "default crop recorder GPU should match analytics GPU");
    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", -1) == 0,
        "negative analytics GPU should fall back to GPU 0");
}

void global_override_applies()
{
    ScopedEnv global("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID");
    ScopedEnv per_camera("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095");
    global.Set("8");
    per_camera.Unset();

    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 8,
        "global crop recorder GPU override should apply");
}

void per_camera_override_wins()
{
    ScopedEnv global("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID");
    ScopedEnv per_camera("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095");
    global.Set("8");
    per_camera.Set("6");

    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 6,
        "per-camera crop recorder GPU override should win over global override");
}

void invalid_override_falls_back()
{
    ScopedEnv global("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID");
    ScopedEnv per_camera("ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_2010095");
    global.Set("not-a-gpu");
    per_camera.Unset();

    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 5,
        "invalid global override should fall back to analytics GPU");

    global.Set("8");
    per_camera.Set("-1");
    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 8,
        "invalid per-camera override should be ignored so the global override can apply");

    global.Unset();
    require(
        orange::session::resolve_external_crop_recorder_gpu_id_from_env("2010095", 5) == 5,
        "invalid per-camera override without global override should fall back to analytics GPU");
}

void require_separate_gpu_flag_parses()
{
    ScopedEnv require_separate("ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU");
    require_separate.Unset();
    require(
        !orange::session::external_crop_recorder_require_separate_gpu_from_env(),
        "unset separate-GPU flag should be false");

    require_separate.Set("1");
    require(
        orange::session::external_crop_recorder_require_separate_gpu_from_env(),
        "separate-GPU flag should accept 1");

    require_separate.Set("true");
    require(
        orange::session::external_crop_recorder_require_separate_gpu_from_env(),
        "separate-GPU flag should accept true");

    require_separate.Set("off");
    require(
        !orange::session::external_crop_recorder_require_separate_gpu_from_env(),
        "separate-GPU flag should accept off");

    require_separate.Set("not-a-bool");
    require(
        !orange::session::external_crop_recorder_require_separate_gpu_from_env(),
        "invalid separate-GPU flag should be treated as false");
}

void same_gpu_guard_uses_separate_gpu_flag()
{
    ScopedEnv require_separate("ORANGE_CROP_EXTERNAL_REQUIRE_SEPARATE_GPU");
    require_separate.Unset();
    require(
        !orange::session::external_crop_recorder_same_gpu_disallowed(5, 5),
        "same-GPU placement should be allowed when the guard is unset");

    require_separate.Set("1");
    require(
        orange::session::external_crop_recorder_same_gpu_disallowed(5, 5),
        "same-GPU placement should be rejected when the guard is enabled");
    require(
        !orange::session::external_crop_recorder_same_gpu_disallowed(5, 6),
        "different recorder GPU should pass when the guard is enabled");
    require(
        !orange::session::external_crop_recorder_same_gpu_disallowed(-1, -1),
        "unknown analytics GPU should not trip the same-GPU guard");
}

}  // namespace

int main()
{
    default_uses_analytics_gpu();
    global_override_applies();
    per_camera_override_wins();
    invalid_override_falls_back();
    require_separate_gpu_flag_parses();
    same_gpu_guard_uses_separate_gpu_flag();
    return 0;
}
