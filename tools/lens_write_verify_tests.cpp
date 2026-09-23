#include "lens_write_verify.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;
#define EXPECT(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Simulated mount: a move takes `move_ms`; while moving, busy=true and current
// holds the old value; when done, current becomes target. A write arriving
// while busy is dropped (target unchanged).
struct FakeMount {
    double clock_ms = 0.0;
    double busy_until_ms = -1.0;
    unsigned int current = 24;
    unsigned int pending = 24;
    bool busy_node = true;
    bool current_node = true;

    bool busy() const { return clock_ms < busy_until_ms; }
    void settle_if_done() { if (!busy() && current != pending) current = pending; }
    void write(unsigned int target, double move_ms) {
        if (busy()) return;  // dropped, exactly like the hardware
        pending = target;
        busy_until_ms = clock_ms + move_ms;
    }
    // A move of the other axis (e.g. focus): occupies the mount, leaves this register alone.
    void other_axis_move(double move_ms) {
        if (busy()) return;
        busy_until_ms = clock_ms + move_ms;
    }
    orange::lens::LensPollers pollers() {
        orange::lens::LensPollers p;
        p.read_busy = [this](bool* b) { settle_if_done(); if (!busy_node) return false; *b = busy(); return true; };
        p.read_current = [this](unsigned int* c) { settle_if_done(); if (!current_node) return false; *c = current; return true; };
        p.now_ms = [this]() { return clock_ms; };
        p.sleep_poll_interval = [this]() { clock_ms += 5.0; };
        return p;
    }
};

void test_idle_immediately()
{
    FakeMount m;
    auto r = orange::lens::wait_lens_idle(m.pollers(), 2000.0);
    EXPECT(r.idle);
    EXPECT(r.busy_readable);
    EXPECT(r.polls == 1);
    EXPECT(r.waited_ms == 0.0);
}

void test_wait_idle_clears_after_move()
{
    FakeMount m;
    m.write(8, 60.0);
    auto r = orange::lens::wait_lens_idle(m.pollers(), 2000.0);
    EXPECT(r.idle);
    EXPECT(r.waited_ms >= 60.0 && r.waited_ms <= 70.0);
}

void test_wait_idle_timeout()
{
    FakeMount m;
    m.write(8, 5000.0);
    auto r = orange::lens::wait_lens_idle(m.pollers(), 100.0);
    EXPECT(!r.idle);
    EXPECT(r.waited_ms >= 100.0);
}

void test_unreadable_busy_counts_as_idle()
{
    FakeMount m;
    m.busy_node = false;
    m.write(8, 60.0);
    auto r = orange::lens::wait_lens_idle(m.pollers(), 2000.0);
    EXPECT(r.idle);
    EXPECT(!r.busy_readable);
}

void test_settle_tracks_current()
{
    FakeMount m;
    m.write(8, 52.0);
    auto r = orange::lens::wait_lens_settled(m.pollers(), 8, 1500.0);
    EXPECT(r.settled);
    EXPECT(r.busy_seen);
    EXPECT(r.current == 8);
    EXPECT(r.settle_ms >= 52.0 && r.settle_ms <= 60.0);
}

void test_dropped_write_is_detected()
{
    FakeMount m;
    m.other_axis_move(118.0);  // focus move in flight
    m.write(8, 52.0);          // iris write dropped by the busy mount
    auto r = orange::lens::wait_lens_settled(m.pollers(), 8, 300.0);
    EXPECT(!r.settled);
    EXPECT(r.current == 24);
    EXPECT(r.settle_ms >= 300.0);
}

void test_no_current_node_reports_unverifiable()
{
    FakeMount m;
    m.current_node = false;
    m.write(8, 52.0);
    auto r = orange::lens::wait_lens_settled(m.pollers(), 8, 1500.0);
    EXPECT(!r.settled);
    EXPECT(!r.current_readable);
    EXPECT(r.polls == 1);
}

void test_already_at_target_settles_at_once()
{
    FakeMount m;
    auto r = orange::lens::wait_lens_settled(m.pollers(), 24, 1500.0);
    EXPECT(r.settled);
    EXPECT(!r.busy_seen);
    EXPECT(r.polls == 1);
}

void test_focus_tolerance_accepts_one_count_off()
{
    FakeMount m;
    m.current = 3169;  // encoder landed one count past the target
    m.pending = 3169;
    auto exact = orange::lens::wait_lens_settled(m.pollers(), 3168, 50.0, 0);
    EXPECT(!exact.settled);
    auto tol = orange::lens::wait_lens_settled(m.pollers(), 3168, 50.0, 2);
    EXPECT(tol.settled);
    EXPECT(tol.current == 3169);
}

}  // namespace

int main()
{
    test_focus_tolerance_accepts_one_count_off();
    test_idle_immediately();
    test_wait_idle_clears_after_move();
    test_wait_idle_timeout();
    test_unreadable_busy_counts_as_idle();
    test_settle_tracks_current();
    test_dropped_write_is_detected();
    test_no_current_node_reports_unverifiable();
    test_already_at_target_settles_at_once();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("lens_write_verify_tests: all passed\n");
    return 0;
}
