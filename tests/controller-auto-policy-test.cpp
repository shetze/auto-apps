#include "controller-auto-policy.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace controller_auto;
using demo_health::Level;
using Seconds = std::chrono::seconds;

static demo_health::Message sample(Level cpu, Level ram, unsigned overrun) {
    demo_health::Message d;
    d.cpu = cpu; d.ram = ram; d.cpu_metrics_valid = true;
    d.ram_metrics_valid = true; d.cpu_overrun_ms = overrun;
    return d;
}

int main() {
    Policy restored_after_controller_restart;
    restored_after_controller_restart.configured_baseline(150);
    restored_after_controller_restart.observe(18);
    assert(restored_after_controller_restart.baseline() == 150 &&
           restored_after_controller_restart.current() == 18);
    Policy p;
    const auto start = Clock::time_point{Seconds(1000)};
    p.observe(150);
    assert(p.baseline() == 150 && p.current() == 150);
    for (int i=0; i<4; ++i)
        assert(p.health(sample(Level::Degraded, Level::Normal, 20),
                        start+Seconds(i), true, true, true).action == Action::None);
    auto d = p.health(sample(Level::Degraded, Level::Normal, 25),
                      start+Seconds(4), true, true, true);
    assert(d.action == Action::Reduce && d.from == 150 && d.to == 75);
    assert(p.pending() == Action::Reduce);
    assert(p.health(sample(Level::Degraded, Level::Normal, 25),
                    start+Seconds(5), true, true, true).action == Action::None);
    p.result(true, 75, start+Seconds(5));
    assert(p.reductions() == 1 && p.baseline() == 150 && p.current() == 75);
    // CPU-only errors aren't a RAM response; RAM failure must not trigger
    // either mitigation or recovery.
    for (int i=6; i<110; ++i)
        assert(p.health(sample(Level::Normal, Level::Failed, 0),
                        start+Seconds(i), true, true, true).action == Action::None);
    assert(p.restores() == 0);
    for (int i=110; i<200; ++i)
        assert(p.health(sample(Level::Normal, Level::Normal, 0),
                        start+Seconds(i), true, true, true).action == Action::None);
    d = p.health(sample(Level::Normal, Level::Normal, 0),
                 start+Seconds(200), true, true, true);
    assert(d.action == Action::Restore && d.from == 75 && d.to == 150);
    p.result(true, 150, start+Seconds(201));
    assert(p.restores() == 1 && p.current() == p.baseline());
    for (int i=201; i<320; ++i)
        assert(p.health(sample(Level::Normal, Level::Normal, 0),
                        start+Seconds(i), true, true, true).action == Action::None);
    // Explicit user changes supersede recovery and establish a new baseline.
    p.manual_submitted(start+Seconds(321));
    assert(p.health(sample(Level::Degraded, Level::Normal, 15),
                    start+Seconds(321), true, true, true).action == Action::None);
    p.manual_result(true, 300, start+Seconds(322));
    assert(p.baseline() == 300 && p.current() == 300);
    // Suppress automatic interventions when telemetry isn't current or
    // when a CPU warning has no measured deadline miss.
    for (int i=330; i<425; ++i) {
        assert(p.health(sample(Level::Degraded, Level::Normal, 0),
                        start+Seconds(i), true, true, true).action == Action::None);
    }
    for (int i=430; i<435; ++i)
        assert(p.health(sample(Level::Degraded, Level::Normal, 30),
                        start+Seconds(i), true, false, true).action == Action::None);
    for (int i=450; i<455; ++i)
        d = p.health(sample(Level::Degraded, Level::Normal, 30),
                     start+Seconds(i), true, true, true);
    assert(d.action == Action::Reduce && d.to == 150);
    p.result(false, 0, start+Seconds(456), true);
    assert(p.current() == kUnknown && p.last_reason() == Reason::Timeout);
    assert(p.health(sample(Level::Degraded, Level::Normal, 30),
                    start+Seconds(457), true, true, true).action == Action::None);
    p.observe(300); // re-read after lost response
    assert(p.current() == 300 && p.baseline() == 300);
    p.observe(225); // a service-side manual edit becomes the new baseline
    assert(p.current() == 225 && p.baseline() == 225);
    // CPU warnings dominated by off-CPU time must not be attributed to CPU
    // load when mutex blocking or throttling could explain them.
    Policy blocked;
    blocked.observe(160);
    auto blocked_data = sample(Level::Degraded, Level::Normal, 40);
    blocked_data.cpu_wall_ms = 1000;
    blocked_data.cpu_off_ms = 850;
    for (int i=0; i<10; ++i)
        assert(blocked.health(blocked_data, start+Seconds(i), true, true, true).action == Action::None);
    // Consecutive samples cannot be accumulated across a telemetry gap.
    Policy gap;
    gap.observe(160);
    for (int i=0; i<4; ++i)
        assert(gap.health(sample(Level::Degraded, Level::Normal, 35),
                    start+Seconds(i), true, true, true).action == Action::None);
    assert(gap.health(sample(Level::Degraded, Level::Normal, 35),
                    start+Seconds(12), true, true, true).action == Action::None);
    std::cout << "auto CPU mitigation / baseline / recovery / manual override: PASS\n";
}
