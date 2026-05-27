#include "crop_preview_cadence.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_first_detection_updates()
{
    CropPreviewCadence cadence(15);
    const auto decision = cadence.ShouldUpdate(true, false, 1000);
    require(decision.offered, "first detection should be offered");
    require(decision.update, "first detection should update preview");
    require(!decision.skipped_by_cadence, "first detection should not be skipped");
}

void test_repeated_detection_respects_interval()
{
    CropPreviewCadence cadence(10);
    auto first = cadence.ShouldUpdate(true, false, 1'000'000'000ull);
    require(first.update, "first detection should update");
    cadence.MarkUpdated(false, 1'000'000'000ull);

    const auto early = cadence.ShouldUpdate(true, false, 1'050'000'000ull);
    require(early.offered, "early detection should be offered");
    require(!early.update, "early detection should be skipped before interval");
    require(early.skipped_by_cadence, "early detection should count as cadence skip");

    const auto ready = cadence.ShouldUpdate(true, false, 1'100'000'000ull);
    require(ready.update, "detection should update after interval");
}

void test_unlimited_updates_every_offer()
{
    CropPreviewCadence cadence(0);
    auto first = cadence.ShouldUpdate(true, false, 10);
    require(first.update, "unlimited first detection should update");
    cadence.MarkUpdated(false, 10);
    const auto second = cadence.ShouldUpdate(true, false, 11);
    require(second.update, "unlimited second detection should update");
}

void test_negative_fps_is_unlimited()
{
    CropPreviewCadence cadence(-1);
    require(cadence.MaxFps() == 0, "negative max FPS should sanitize to unlimited");
    auto first = cadence.ShouldUpdate(true, false, 10);
    require(first.update, "negative/unlimited first detection should update");
    cadence.MarkUpdated(false, 10);
    const auto second = cadence.ShouldUpdate(true, false, 11);
    require(second.update, "negative/unlimited second detection should update");
}

void test_large_fps_is_clamped()
{
    CropPreviewCadence cadence(999999);
    require(
        cadence.MaxFps() == CameraCropPipelineConfig::kMaxPreviewMaxFps,
        "large max FPS should clamp to configured maximum");
}

void test_initial_blank_is_not_repeatedly_cleared()
{
    CropPreviewCadence cadence(15);
    const auto blank = cadence.ShouldUpdate(true, true, 10);
    require(blank.offered, "blank should be offered");
    require(!blank.update, "initial blank should not clear an already blank preview");
    require(blank.skipped_by_cadence, "initial blank should count as skipped");
}

void test_detection_to_blank_transition_clears_once()
{
    CropPreviewCadence cadence(15);
    auto detection = cadence.ShouldUpdate(true, false, 10);
    require(detection.update, "detection should update");
    cadence.MarkUpdated(false, 10);

    auto blank = cadence.ShouldUpdate(true, true, 20);
    require(blank.update, "detection-to-blank transition should clear preview");
    cadence.MarkUpdated(true, 20);

    const auto repeated_blank = cadence.ShouldUpdate(true, true, 30);
    require(!repeated_blank.update, "repeated blank should not clear again");
}

void test_hidden_preview_is_not_offered()
{
    CropPreviewCadence cadence(15);
    cadence.SetDisplayEnabled(false);
    const auto hidden = cadence.ShouldUpdate(true, false, 10);
    require(!hidden.offered, "hidden preview should not be offered");
    require(!hidden.update, "hidden preview should not update");
    require(!hidden.skipped_by_cadence, "hidden preview should not count as cadence skip");
}

void test_reenable_forces_next_update()
{
    CropPreviewCadence cadence(15);
    auto first = cadence.ShouldUpdate(true, false, 10);
    require(first.update, "first detection should update");
    cadence.MarkUpdated(false, 10);

    cadence.SetDisplayEnabled(false);
    cadence.SetDisplayEnabled(true);
    const auto forced = cadence.ShouldUpdate(true, false, 11);
    require(forced.offered, "reenabled preview should be offered");
    require(forced.update, "reenabled preview should force the next update");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"first_detection_updates", &test_first_detection_updates},
        {"repeated_detection_respects_interval", &test_repeated_detection_respects_interval},
        {"unlimited_updates_every_offer", &test_unlimited_updates_every_offer},
        {"negative_fps_is_unlimited", &test_negative_fps_is_unlimited},
        {"large_fps_is_clamped", &test_large_fps_is_clamped},
        {"initial_blank_is_not_repeatedly_cleared", &test_initial_blank_is_not_repeatedly_cleared},
        {"detection_to_blank_transition_clears_once", &test_detection_to_blank_transition_clears_once},
        {"hidden_preview_is_not_offered", &test_hidden_preview_is_not_offered},
        {"reenable_forces_next_update", &test_reenable_forces_next_update},
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

    std::cout << "All crop preview cadence tests passed.\n";
    return EXIT_SUCCESS;
}
