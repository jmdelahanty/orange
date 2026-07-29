#include "gui/per_camera_stream_runtime.h"

#include <iostream>
#include <type_traits>

static_assert(
    std::is_default_constructible_v<orange::gui::PerCameraStreamRuntime>);
static_assert(
    std::is_destructible_v<orange::gui::PerCameraStreamRuntime>);
static_assert(
    std::is_nothrow_destructible_v<orange::gui::PerCameraStreamRuntime>);
static_assert(
    !std::is_copy_constructible_v<orange::gui::PerCameraStreamRuntime>);
static_assert(
    !std::is_copy_assignable_v<orange::gui::PerCameraStreamRuntime>);
static_assert(
    std::is_nothrow_move_constructible_v<
        orange::gui::PerCameraStreamRuntime>);
static_assert(
    std::is_nothrow_move_assignable_v<orange::gui::PerCameraStreamRuntime>);

int main()
{
    std::cout << "per_camera_stream_runtime_contract_tests: PASS\n";
    return 0;
}
