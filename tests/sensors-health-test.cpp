#include "sensors-health.hpp"
#include "demo-health.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;
int main() {
    const auto t = std::chrono::steady_clock::time_point{} + 10s;
    sensors_health::Tracker tracker(t, 1000000000ULL);
    const auto first = tracker.sample(t + 1s, 1050000000ULL, 6200);
    assert(first.cpu_metrics_valid && first.ram_metrics_valid);
    assert(first.cpu_share_centi_percent == 500);
    assert(first.cpu_wall_ms == 1000);
    assert(first.cpu_off_ms == 950);
    assert(first.cpu_deadline_misses == 0);
    assert(first.cpu == demo_health::Level::Normal);
    assert(first.ram_allocated_bytes == 6200);
    tracker.frame_finished(120ms, 100ms);
    tracker.frame_finished(130ms, 100ms);
    auto second = tracker.sample(t + 2s, 1250000000ULL, 8200);
    assert(second.sequence == first.sequence + 1);
    assert(second.cpu_share_centi_percent == 2000);
    assert(second.cpu_overrun_ms == 30);
    assert(second.cpu_deadline_misses == 2);
    assert(second.cpu == demo_health::Level::Degraded);
    demo_health::Message decoded;
    const auto wire = demo_health::encode(second);
    assert(demo_health::decode(wire.data(), wire.size(), decoded));
    assert(decoded.cpu_deadline_misses == 2);
    assert(decoded.ram_allocated_bytes == 8200);
    const auto third = tracker.sample(t + 3s, 1250000000ULL, 0);
    assert(third.cpu == demo_health::Level::Normal);
    assert(third.cpu_deadline_misses == 2);
    assert(third.cpu_overrun_ms == 0);
    assert(third.ram_allocated_bytes == 0);
    std::cout << "sensors real-worker health sampling passed\n";
}
