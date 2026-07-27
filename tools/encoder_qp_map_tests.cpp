#include "encoder_qp_map.h"

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

void test_off_builds_no_map()
{
    orange::encoding::QpMapPolicy policy;
    orange::encoding::QpDeltaMap map;
    std::string error;
    require(orange::encoding::build_circle_qp_delta_map(
                policy, 4512, 4512, 32, &map, &error),
            "off policy should build: " + error);
    require(!map.enabled(), "off policy must not allocate a map");
}

void test_full_frame_grid_and_regions()
{
    orange::encoding::QpMapPolicy policy;
    policy.mode = "static-dish-prior";
    policy.circle = {2256.0, 2256.0, 2000.0};
    policy.halo_px = 64.0;
    policy.inside_delta_qp = -2;
    policy.halo_delta_qp = 0;
    policy.outside_delta_qp = 2;

    orange::encoding::QpDeltaMap map;
    std::string error;
    require(orange::encoding::build_circle_qp_delta_map(
                policy, 4512, 4512, 32, &map, &error),
            "dish policy should build: " + error);
    require(map.enabled(), "dish policy should enable a map");
    require(map.grid_width == 141 && map.grid_height == 141,
            "4512 HEVC grid must be 141x141");
    require(map.size_bytes() == 141U * 141U,
            "QP delta map must contain one signed byte per CTB");
    require(map.inside_block_count > 0, "inside region must be populated");
    require(map.halo_block_count > 0, "halo region must be populated");
    require(map.outside_block_count > 0, "outside region must be populated");
    require(map.inside_block_count + map.halo_block_count + map.outside_block_count ==
                map.values.size(),
            "region counts must cover the grid");
    require(map.values[70U * 141U + 70U] == -2,
            "center CTB must receive inside priority");
    require(map.values.front() == 2,
            "corner CTB must receive outside penalty");
    require(map.checksum.rfind("fnv1a64:", 0) == 0,
            "generated map must carry a deterministic checksum");
}

void test_intersecting_block_is_protected()
{
    orange::encoding::QpMapPolicy policy;
    policy.mode = orange::encoding::kQpMapModeStaticDishPrior;
    policy.circle = {32.0, 32.0, 1.0};
    policy.halo_px = 0.0;
    policy.inside_delta_qp = -3;
    policy.halo_delta_qp = 0;
    policy.outside_delta_qp = 3;

    orange::encoding::QpDeltaMap map;
    std::string error;
    require(orange::encoding::build_circle_qp_delta_map(
                policy, 64, 64, 32, &map, &error),
            "boundary policy should build: " + error);
    require(map.inside_block_count == 4,
            "all four CTBs touching the protected circle must be prioritized");
}

void test_invalid_delta_order_is_rejected()
{
    orange::encoding::QpMapPolicy policy;
    policy.mode = orange::encoding::kQpMapModeStaticDishPrior;
    policy.circle = {100.0, 100.0, 50.0};
    policy.inside_delta_qp = 2;
    policy.halo_delta_qp = 0;
    policy.outside_delta_qp = -2;
    std::string error;
    require(!orange::encoding::validate_qp_map_policy(policy, &error),
            "inverted QP priority must be rejected");
    require(!error.empty(), "invalid policy should explain the failure");
}

}  // namespace

int main()
{
    try {
        test_off_builds_no_map();
        test_full_frame_grid_and_regions();
        test_intersecting_block_is_protected();
        test_invalid_delta_order_is_rejected();
    } catch (const std::exception& ex) {
        std::cerr << "encoder_qp_map_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "encoder_qp_map_tests passed" << std::endl;
    return 0;
}
