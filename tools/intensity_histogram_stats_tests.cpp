#include "gui/intensity_histogram_stats.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
#define EXPECT(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
}

int main()
{
    using orange::gui::IntensityHistogramStats;
    // Uniform: every value once per frame, 2 frames.
    uint64_t counts[256];
    for (int v = 0; v < 256; ++v) counts[v] = 2;
    IntensityHistogramStats s;
    orange::gui::compute_intensity_histogram_stats(counts, 512.0, &s);
    EXPECT(near(s.fraction[0], 1.0 / 256.0));
    EXPECT(near(s.mean, 127.5));
    EXPECT(s.p50 == 127.0 || s.p50 == 128.0);
    EXPECT(s.p1 == 2.0);
    EXPECT(s.p99 == 253.0);
    EXPECT(near(s.clip_fraction, 6.0 / 256.0));
    EXPECT(near(s.dark_fraction, 8.0 / 256.0));

    // Saturated dish: 85% at 255, 15% at 30 (the 12:26 fault signature).
    for (int v = 0; v < 256; ++v) counts[v] = 0;
    counts[255] = 850; counts[30] = 150;
    orange::gui::compute_intensity_histogram_stats(counts, 1000.0, &s);
    EXPECT(near(s.clip_fraction, 0.85));
    EXPECT(s.p50 == 255.0);
    EXPECT(s.p1 == 30.0);
    EXPECT(near(s.mean, 0.85 * 255 + 0.15 * 30));

    // Empty total: everything zero, no division.
    orange::gui::compute_intensity_histogram_stats(counts, 0.0, &s);
    EXPECT(s.mean == 0.0 && s.p99 == 0.0);

    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("intensity_histogram_stats_tests: all passed\n");
    return 0;
}
