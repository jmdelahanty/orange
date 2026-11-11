// tools/camera_frame_id_tests.cpp
//
// Gap counting across the camera's 1..65535 frame-id wrap
// (src/camera_frame_id.h). The oracle walks a simulated camera stream frame
// by frame, so it does not share the modular arithmetic under test.

#include "camera_frame_id.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_EQ(actual, expected, msg)                                              \
    do {                                                                              \
        const auto a_ = (actual);                                                     \
        const auto e_ = (expected);                                                   \
        if (a_ != e_) {                                                               \
            ++g_failures;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s: got %llu, expected %llu\n",         \
                         __FILE__, __LINE__, msg, (unsigned long long)a_,             \
                         (unsigned long long)e_);                                     \
        }                                                                             \
    } while (0)

using orange::camera_frame_id::MissingBetween;

uint16_t NextId(uint16_t id)
{
    return id == 65535 ? 1 : static_cast<uint16_t>(id + 1);
}

// Counts like acquire_frames: previous id kept across the wrap, 0 never stored.
uint64_t CountStream(const std::vector<uint16_t>& received)
{
    uint16_t prev = 0;
    uint64_t missing = 0;
    for (uint16_t id : received) {
        missing += MissingBetween(prev, id);
        if (id != 0) prev = id;
    }
    return missing;
}

void TestEdges()
{
    EXPECT_EQ(MissingBetween(0, 7), 0u, "no previous frame");
    EXPECT_EQ(MissingBetween(7, 0), 0u, "invalid id 0");
    EXPECT_EQ(MissingBetween(100, 101), 0u, "consecutive");
    EXPECT_EQ(MissingBetween(23289, 23291), 1u, "one lost (rig 2026-11-10, Cam2010096)");
    EXPECT_EQ(MissingBetween(65534, 65535), 0u, "up to the last id");
    EXPECT_EQ(MissingBetween(65535, 1), 0u, "wrap 65535 -> 1");
    EXPECT_EQ(MissingBetween(65534, 1), 1u, "65535 lost");
    EXPECT_EQ(MissingBetween(65535, 2), 1u, "1 lost right after the wrap");
    EXPECT_EQ(MissingBetween(65535, 4), 3u, "1..3 lost right after the wrap");
    EXPECT_EQ(MissingBetween(65533, 2), 3u, "65534, 65535, 1 lost");
    EXPECT_EQ(MissingBetween(500, 500), 0u, "repeated id");
    EXPECT_EQ(MissingBetween(40000, 1), 25535u, "camera restart counts as a gap");
}

// Every single loss position around the wrap, against the frame-by-frame oracle.
void TestSimulatedWrap()
{
    std::vector<uint16_t> ids;
    uint16_t id = 65500;
    for (int i = 0; i < 80; ++i) {
        ids.push_back(id);
        id = NextId(id);
    }
    for (size_t lost = 1; lost + 1 < ids.size(); ++lost) {
        for (size_t run = 1; run <= 3 && lost + run < ids.size(); ++run) {
            std::vector<uint16_t> received;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i < lost || i >= lost + run) received.push_back(ids[i]);
            }
            EXPECT_EQ(CountStream(received), run, "simulated loss around the wrap");
        }
    }
    EXPECT_EQ(CountStream(ids), 0u, "no loss across the wrap");
}

}  // namespace

int main()
{
    TestEdges();
    TestSimulatedWrap();
    if (g_failures != 0) {
        std::fprintf(stderr, "camera_frame_id_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("camera_frame_id_tests: PASS\n");
    return 0;
}
