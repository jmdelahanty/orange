// Rounded output numbers serialize as their shortest decimal text, under the host
// build's fast math (src/outputs/common/decimal_round.h). This target is compiled
// with -Ofast -ffast-math, as orange and orange_client are: the defect it guards
// against (std::round(v * 10^d) / 10^d printing as 180.96200000000002) appears
// only there.
//
// Checks the TEXT nlohmann writes, for:
//  - the values Palette found in the 2026-11-15 rig recordings (means 180.962,
//    223.825, 170.289, 179.825, 228.015; fraction 0.23449868);
//  - sweeps at 3, 4 and 8 decimals against the decimal formed from integers;
//  - every rounding path: outputs::round_decimals, the generated writers'
//    detail::round_to, event_log_format::round_to, and image_levels'
//    StatisticsToJson (session.image_levels_check).
#include "event_log_format.h"
#include "image_levels.h"
#include "orange_outputs/orange_image_levels_v1.h"
#include "outputs/common/decimal_round.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

namespace {
int failures = 0;
int checks = 0;
void check(bool ok, const std::string& what)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "[FAIL] " << what << "\n";
    }
}

// The shortest decimal of units / 10^decimals as nlohmann writes a double
// ("181.0" for an integer value), built from integers only.
std::string expected_text(std::int64_t units, int decimals)
{
    const bool negative = units < 0;
    std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-units) : static_cast<std::uint64_t>(units);
    std::uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    std::string fraction = std::to_string(magnitude % scale);
    fraction = std::string(static_cast<std::size_t>(decimals) - fraction.size(), '0') + fraction;
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    if (fraction.empty()) fraction = "0";
    return std::string(negative && magnitude != 0 ? "-" : "") + std::to_string(magnitude / scale) + "." + fraction;
}

std::string dumped(double value) { return nlohmann::json(value).dump(); }

// Each path is called as production calls it: inlined, with the decimals a
// compile-time constant. (Through a function pointer the compiler cannot see
// the constant 10^d and keeps the division, which hides the defect.)
struct Helper {
    template <int D> static double round(double v) { return orange::outputs::round_decimals(v, D); }
    static constexpr const char* name = "outputs::round_decimals";
};
struct Generated {
    template <int D> static double round(double v) { return orange::outputs::gen::image_levels_v1::detail::round_to(v, D); }
    static constexpr const char* name = "generated detail::round_to";
};
struct EventLog {
    template <int D> static double round(double v) { return event_log_format::round_to(v, D); }
    static constexpr const char* name = "event_log_format::round_to";
};

template <class Path>
void known_values()
{
    // volatile: the raw values come from runtime arithmetic, as means do.
    volatile double count = 1000.0;
    const struct { double raw; const char* text; } three[] = {
        {180962.0 / count, "180.962"}, {223825.0 / count, "223.825"}, {170289.0 / count, "170.289"},
        {179825.0 / count, "179.825"}, {228015.0 / count, "228.015"}, {180.96200000000002, "180.962"},
        {13634729540.0 / 1e8, "136.347"}, {0.0, "0.0"}, {255.0, "255.0"},
    };
    for (const auto& c : three) {
        const std::string text = dumped(Path::template round<3>(c.raw));
        check(text == c.text, std::string(Path::name) + ": " + std::to_string(c.raw) + " at 3 decimals writes " + text +
                                  ", want " + c.text);
    }
    const struct { double raw; const char* text; } eight[] = {
        {0.2344986812, "0.23449868"}, {0.23449868000000001, "0.23449868"}, {0.00000001, "1e-08"},
    };
    for (const auto& c : eight) {
        // nlohmann writes 1e-08 for 0.00000001: its shortest form of the same double.
        const std::string text = dumped(Path::template round<8>(c.raw));
        check(text == c.text, std::string(Path::name) + ": " + std::to_string(c.raw) + " at 8 decimals writes " + text +
                                  ", want " + c.text);
    }
}

template <class Path, int D>
void sweep(std::int64_t limit, std::mt19937_64& rng)
{
    std::uniform_int_distribution<std::int64_t> units_of(0, limit);
    std::uniform_real_distribution<double> jitter(-0.4, 0.4);
    volatile double scale = 1.0;  // runtime, so the raw values carry ordinary rounding noise
    for (int i = 0; i < D; ++i) scale = scale * 10.0;
    int bad = 0;
    std::string first;
    for (int n = 0; n < 200000; ++n) {
        const std::int64_t units = units_of(rng);
        const double raw = (static_cast<double>(units) + jitter(rng)) / scale;  // rounds to units
        const std::string want = expected_text(units, D);
        const std::string got = dumped(Path::template round<D>(raw));
        // As text, or where nlohmann chose exponent form, as the same value.
        const bool ok = got == want || std::strtod(got.c_str(), nullptr) == std::strtod(want.c_str(), nullptr);
        if (!ok && bad++ == 0) first = got + " for " + want;
    }
    check(bad == 0, std::string(Path::name) + ": " + std::to_string(bad) + " of 200000 values at " + std::to_string(D) +
                        " decimals not their shortest decimal (first: " + first + ")");
}

template <class Path>
void all_of_path(std::mt19937_64& rng)
{
    known_values<Path>();
    sweep<Path, 3>(300000000, rng);  // coordinates, means, latencies
    sweep<Path, 4>(10000, rng);      // confidences
    sweep<Path, 8>(100000000, rng);  // fractions
}

void statistics_text()
{
    // 1e8 pixels: 23449868 at 255, the rest at 100. Mean 136.3472954 -> 136.347,
    // saturated 0.23449868 (the fraction Palette found 1 ulp off).
    std::uint64_t histogram[256] = {};
    histogram[255] = 23449868;
    histogram[100] = 100000000 - 23449868;
    const auto stats = orange::image_levels::StatisticsFromHistogram(histogram);
    const std::string text = orange::image_levels::StatisticsToJson(stats).dump(2);
    check(text.find("\"mean\": 136.347,") != std::string::npos, "image_levels mean text: " + text);
    check(text.find("\"saturated_fraction\": 0.23449868,") != std::string::npos,
          "image_levels saturated_fraction text: " + text);
    // 962 pixels at 181 and 38 at 180: mean 180.962 exactly.
    std::uint64_t small[256] = {};
    small[181] = 962;
    small[180] = 38;
    const std::string small_text =
        orange::image_levels::StatisticsToJson(orange::image_levels::StatisticsFromHistogram(small)).dump();
    check(small_text.find("\"mean\":180.962,") != std::string::npos, "image_levels mean 180.962 text: " + small_text);
    // The generated writer on the same statistics (the empty-background descriptor path).
    orange::outputs::gen::image_levels_v1::OrangeImageLevelsV1 levels;
    levels.pixel_count = 1000;
    levels.mean = 180962.0 / static_cast<double>(levels.pixel_count);
    levels.saturated_fraction = 0.2344986812;
    levels.zero_fraction = 0.0;
    const std::string gen_text = orange::outputs::gen::image_levels_v1::to_json(levels).dump();
    check(gen_text.find("\"mean\":180.962") != std::string::npos &&
              gen_text.find("\"saturated_fraction\":0.23449868") != std::string::npos,
          "generated image_levels writer text: " + gen_text);
}
}  // namespace

int main()
{
    std::mt19937_64 rng(20261115);
    all_of_path<Helper>(rng);
    all_of_path<Generated>(rng);
    all_of_path<EventLog>(rng);
    statistics_text();
    std::cout << checks << " checks, " << failures << " failures\n"
              << (failures == 0 ? "decimal_rounding_tests: PASS\n" : "decimal_rounding_tests: FAIL\n");
    return failures == 0 ? 0 : 1;
}
