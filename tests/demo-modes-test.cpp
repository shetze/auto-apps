#include "demo-modes.hpp"
#include "mm-client-state.hpp"
#include "mm-demo-health.hpp"
#include "sensors-client-state.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

using namespace std::literals::chrono_literals;

int main() {
    using demo_modes::EngineMode;
    EngineMode decoded = EngineMode::Drive;
    assert(demo_modes::decode_mode(0, decoded) && decoded == EngineMode::Drive);
    assert(demo_modes::decode_mode(1, decoded) && decoded == EngineMode::Reverse);
    assert(demo_modes::decode_mode(2, decoded) && decoded == EngineMode::Fast);
    assert(demo_modes::decode_mode(3, decoded) && decoded == EngineMode::Parking);
    assert(!demo_modes::decode_mode(4, decoded));
    assert(!demo_modes::decode_mode(0xffffffff, decoded));
    assert(demo_modes::phase(0).mode == EngineMode::Reverse);
    assert(demo_modes::phase(1).mode == EngineMode::Drive);
    assert(demo_modes::phase(2).mode == EngineMode::Fast);
    assert(demo_modes::phase(3).mode == EngineMode::Drive);
    assert(demo_modes::phase(4).mode == EngineMode::Parking);
    assert(demo_modes::phase(5).mode == EngineMode::Reverse);
    assert(demo_modes::phase(0).duration == 10s);
    assert(demo_modes::phase(1).duration == 20s);
    assert(demo_modes::phase(2).duration == 10s);
    assert(demo_modes::phase(3).duration == 15s);
    assert(demo_modes::phase(4).duration == 5s);
    assert(demo_modes::kCycleDuration == 60s);

    const auto clock_origin = std::chrono::steady_clock::time_point{} + 100s;
    demo_modes::ParkingCycle cycle(clock_origin);
    assert(cycle.mode() == EngineMode::Reverse);
    assert(!cycle.advance(clock_origin + 9s));
    assert(cycle.advance(clock_origin + 10s) && cycle.mode() == EngineMode::Drive);
    assert(cycle.advance(clock_origin + 30s) && cycle.mode() == EngineMode::Fast);
    assert(cycle.advance(clock_origin + 40s) && cycle.mode() == EngineMode::Drive);
    assert(cycle.advance(clock_origin + 55s) && cycle.mode() == EngineMode::Parking);
    assert(cycle.advance(clock_origin + 60s) && cycle.mode() == EngineMode::Reverse);
    assert(cycle.advance(clock_origin + 120s) && cycle.mode() == EngineMode::Reverse);
    assert(cycle.deadline() == clock_origin + 130s); // no timing drift

    demo_modes::ParkingCycle held(clock_origin);
    assert(held.set_parking_hold(true, clock_origin + 2s));
    assert(!held.set_parking_hold(true, clock_origin + 3s)); // idempotent
    assert(held.advance(clock_origin + 55s));
    assert(held.mode() == EngineMode::Parking && held.parked());
    assert(!held.advance(clock_origin + 999s));
    assert(held.mode() == EngineMode::Parking);
    assert(held.set_parking_hold(false, clock_origin + 1000s));
    assert(!held.parked() && held.mode() == EngineMode::Reverse);
    assert(held.deadline() == clock_origin + 1010s);
    assert(held.advance(clock_origin + 1010s) && held.mode() == EngineMode::Drive);
    assert(held.set_parking_hold(true, clock_origin + 1011s));
    assert(held.set_parking_hold(false, clock_origin + 1012s));
    assert(held.advance(clock_origin + 1065s) && held.mode() == EngineMode::Reverse);
    demo_modes::ParkingCycle during_parking(clock_origin);
    assert(during_parking.advance(clock_origin + 56s));
    assert(during_parking.set_parking_hold(true, clock_origin + 56s));
    assert(during_parking.parked());
    assert(during_parking.set_parking_hold(false, clock_origin + 58s));
    assert(during_parking.mode() == EngineMode::Reverse);

    assert(demo_modes::cpu_budget(400, true) == 400ms);
    assert(demo_modes::cpu_budget(400, false) == 200ms);
    assert(demo_modes::cpu_budget(1, false) == 500us);
    assert(demo_modes::cpu_period(400) == 402ms);
    assert(demo_modes::cpu_budget(400, true) / demo_modes::cpu_budget(400, false) == 2);

    demo_modes::VolumePolicy volume;
    assert(volume.current() == 50);
    volume.set_mode(EngineMode::Fast);
    assert(volume.current() == 60);
    volume.set_mode(EngineMode::Fast); // field heartbeat must not compound boosts
    assert(volume.current() == 60);
    assert(volume.change(10) == 70);
    assert(volume.base() == 60);
    volume.set_mode(EngineMode::Drive);
    assert(volume.current() == 60);
    volume.set_mode(EngineMode::Reverse);
    assert(volume.current() == 30);
    volume.set_mode(EngineMode::Drive);
    assert(volume.current() == 60);
    volume.set_mode(EngineMode::Reverse);
    assert(volume.current() == 30);
    assert(volume.change(10) == 40);
    volume.set_mode(EngineMode::Drive);
    assert(volume.current() == 40);
    volume.set_mode(EngineMode::Fast);
    assert(volume.current() == 50);
    volume.set_mode(EngineMode::Drive);
    assert(volume.current() == 40);
    demo_modes::VolumePolicy cap(95);
    cap.set_mode(EngineMode::Fast);
    assert(cap.current() == 100);
    cap.set_mode(EngineMode::Drive);
    assert(cap.current() == 95);
    cap.change(-200);
    assert(cap.current() == 0);

    assert(demo_modes::fast_warning(EngineMode::Fast, true));
    assert(!demo_modes::fast_warning(EngineMode::Fast, false));
    assert(!demo_modes::fast_warning(EngineMode::Drive, true));
    assert(demo_modes::media_load(true, false, EngineMode::Drive));
    assert(demo_modes::media_load(false, true, EngineMode::Reverse));
    assert(!demo_modes::media_load(false, false, EngineMode::Drive));
    assert(!demo_modes::media_load(true, false, EngineMode::Fast));

    mm_ui::ClientState client;
    client.engine_availability(true);
    client.reverse_event(true);
    assert(client.direction == mm_ui::Direction::Reverse);
    const auto now = std::chrono::steady_clock::now();
    client.mode_event(EngineMode::Fast, now);
    assert(client.direction == mm_ui::Direction::Fast);
    client.reverse_event(false);  // old field must not erase Fast
    assert(client.direction == mm_ui::Direction::Fast);
    assert(client.fast_or_last_known_fast());
    client.refresh_engine_mode(now + 3s);
    assert(client.direction == mm_ui::Direction::Fast);
    client.refresh_engine_mode(now + 5s);
    assert(client.direction == mm_ui::Direction::Fast);
    assert(client.mode_stale);
    assert(client.fast_or_last_known_fast()); // stale Fast must not re-enable video
    client.mode_event(EngineMode::Drive, now + 6s);
    assert(client.direction == mm_ui::Direction::Forward);
    assert(!client.mode_stale);
    client.parking_hold_event(true);
    assert(client.parking_hold_seen && client.parking_hold_enabled);
    client.mode_event(EngineMode::Parking, now + 7s);
    assert(client.direction == mm_ui::Direction::Parking);
    assert(!client.fast_or_last_known_fast());
    client.parking_hold_event(false);
    assert(!client.parking_hold_enabled);
    assert(!client.fast_or_last_known_fast());
    client.mode_event(EngineMode::Reverse, now + 7s);
    assert(client.direction == mm_ui::Direction::Reverse);
    client.engine_availability(false);
    assert(client.direction == mm_ui::Direction::Unknown);
    assert(!client.parking_hold_seen);

    // The telemetry CPU fraction reflects the paced cycle, while overrun
    // compares only busy work with the active half/full work deadline.
    mm_demo::HealthTracker tracker(400, 0);
    const auto full = tracker.record_cpu(400ms, 400'000'000ULL, now, 400.0, 402ms);
    const auto half = tracker.record_cpu(200ms, 200'000'000ULL, now, 200.0, 402ms);
    assert(full.cpu_share_percent > 95);
    assert(half.cpu_share_percent > 45 && half.cpu_share_percent < 55);
    assert(full.overrun_ms == 0 && half.overrun_ms == 0);
    std::cout << "60-second parking cycle, hold/release, CPU load, UI and volume passed\n";
}
