// Pure statistics for the Intensity Histogram panel (src/opengldisplay.cpp
// accumulates the counts on the display worker; this turns them into the
// averaged fractions and summary numbers shown in the GUI and logged).
#pragma once

#include <cstdint>

namespace orange::gui {

struct IntensityHistogramStats {
    double fraction[256] = {};  // fraction of pixels per intensity value
    double mean = 0.0;
    double p1 = 0.0;
    double p50 = 0.0;
    double p99 = 0.0;
    double clip_fraction = 0.0;  // pixels >= 250
    double dark_fraction = 0.0;  // pixels < 8
};

// counts[v] = number of pixels with value v summed over all accumulated
// frames; total = pixels per frame * frames. Percentiles are the smallest
// value at which the cumulative fraction reaches the level.
inline void compute_intensity_histogram_stats(const uint64_t counts[256], double total, IntensityHistogramStats* out)
{
    if (!out) return;
    *out = IntensityHistogramStats{};
    if (total <= 0.0) return;
    double cum = 0.0;
    double sum = 0.0;
    bool got_p1 = false, got_p50 = false, got_p99 = false;
    for (int v = 0; v < 256; ++v) {
        const double f = static_cast<double>(counts[v]) / total;
        out->fraction[v] = f;
        sum += f * v;
        cum += f;
        if (!got_p1 && cum >= 0.01) { out->p1 = v; got_p1 = true; }
        if (!got_p50 && cum >= 0.50) { out->p50 = v; got_p50 = true; }
        if (!got_p99 && cum >= 0.99) { out->p99 = v; got_p99 = true; }
        if (v >= 250) out->clip_fraction += f;
        if (v < 8) out->dark_fraction += f;
    }
    out->mean = sum;
}

}  // namespace orange::gui
