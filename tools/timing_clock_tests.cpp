// Pins src/timing/clock.h: string formats identical to the helpers it replaced,
// same-base differences, and the clock each helper reads.
#include "timing/clock.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}
void expect_eq(const std::string& got, const std::string& want, const std::string& what)
{
    expect(got == want, what + ": got '" + got + "', want '" + want + "'");
}

// The pre-2026-10-11 get_current_utc_timestamp() body (with gmtime_r).
std::string legacy_utc(std::time_t seconds)
{
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}
}  // namespace

int main()
{
    using namespace orange::timing;
    const RealtimeNs t{1760140800123456789ULL};  // 2025-10-11T00:00:00.123456789Z

    expect_eq(utc_iso8601(t), "2025-10-11T00:00:00Z", "utc_iso8601 seconds");
    expect_eq(utc_iso8601(t, 3), "2025-10-11T00:00:00.123Z", "utc_iso8601 ms");
    expect_eq(utc_iso8601(t, 6), "2025-10-11T00:00:00.123456Z", "utc_iso8601 us");
    expect_eq(utc_iso8601(t, 9), "2025-10-11T00:00:00.123456789Z", "utc_iso8601 ns");
    expect_eq(utc_iso8601(t, 12), "2025-10-11T00:00:00.123456789Z", "utc_iso8601 clamps digits");
    expect_eq(utc_iso8601(RealtimeNs{1760140800000000000ULL - 1}), "2025-10-10T23:59:59Z",
              "utc_iso8601 truncates, never rounds up");
    for (std::time_t s : {std::time_t{0}, std::time_t{951782400}, std::time_t{1767225599}}) {
        expect_eq(utc_iso8601(RealtimeNs{static_cast<uint64_t>(s) * 1000000000ULL}), legacy_utc(s),
                  "utc_iso8601 matches the legacy format");
    }

    setenv("TZ", "UTC", 1);
    tzset();
    expect_eq(local_time_string(t, "%Y_%m_%d_%H_%M_%S"), "2025_10_11_00_00_00",
              "local_time_string folder format");
    expect_eq(local_time_string(t, "%Y_%m_%d"), "2025_10_11", "local_time_string date format");

    expect(between(MonotonicNs{100}, MonotonicNs{350}).value == 250, "between monotonic");
    expect(between(CameraNs{500}, CameraNs{200}).value == -300, "between camera, negative");
    expect(between(RealtimeNs{0}, RealtimeNs{1500000}).ms() == 1.5, "DurationNs::ms");

    // Same clocks as the code used before: steady_clock and system_clock.
    const uint64_t steady_before = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t mono = monotonic_now_ns();
    const uint64_t steady_after = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    expect(steady_before <= mono && mono <= steady_after, "monotonic_now_ns reads steady_clock");

    timespec rt{};
    clock_gettime(CLOCK_REALTIME, &rt);
    const uint64_t rt_ns = static_cast<uint64_t>(rt.tv_sec) * 1000000000ULL + rt.tv_nsec;
    const uint64_t real = realtime_now_ns();
    expect(real >= rt_ns && real - rt_ns < 1000000000ULL, "realtime_now_ns reads CLOCK_REALTIME");

    timespec mt{};
    clock_gettime(CLOCK_MONOTONIC, &mt);
    const uint64_t mt_ns = static_cast<uint64_t>(mt.tv_sec) * 1000000000ULL + mt.tv_nsec;
    const uint64_t mono2 = monotonic_now_ns();
    expect(mono2 >= mt_ns && mono2 - mt_ns < 1000000000ULL, "steady_clock is CLOCK_MONOTONIC");

    const ClockAnchor a = anchor_now();
    expect(a.realtime.value > 0 && a.monotonic.value > 0 && a.uncertainty_ns < 1000000ULL,
           "anchor_now pairs both clocks tightly");

    if (failures) {
        std::cerr << failures << " timing clock test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "timing clock tests passed" << std::endl;
    return 0;
}
