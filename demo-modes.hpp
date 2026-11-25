#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Pure (no SOME/IP, no UI) rules for the five engine phases, CPU demo
// duty cycle and the automatic radio-volume changes.
namespace demo_modes {

enum class EngineMode : std::uint32_t { Drive = 0, Reverse = 1, Fast = 2, Parking = 3 };

inline bool decode_mode(std::uint32_t raw, EngineMode &mode) {
    if (raw > static_cast<std::uint32_t>(EngineMode::Parking))
        return false;
    mode = static_cast<EngineMode>(raw);
    return true;
}

inline const char *name(EngineMode mode) {
    switch (mode) {
    case EngineMode::Drive: return "Drive";
    case EngineMode::Reverse: return "Reverse";
    case EngineMode::Fast: return "Fast";
    case EngineMode::Parking: return "Parking";
    }
    return "Unknown";
}

struct Phase {
    EngineMode mode;
    std::chrono::seconds duration;
};

// One complete cycle is exactly 60 seconds, starting in Reverse.
// The two Drive phases intentionally have different durations.
constexpr std::size_t kPhaseCount = 5;
constexpr std::chrono::seconds kCycleDuration{60};
inline Phase phase(std::size_t index) {
    switch (index % kPhaseCount) {
    case 0: return {EngineMode::Reverse, std::chrono::seconds(10)};
    case 1: return {EngineMode::Drive, std::chrono::seconds(20)};
    case 2: return {EngineMode::Fast, std::chrono::seconds(10)};
    case 3: return {EngineMode::Drive, std::chrono::seconds(15)};
    default: return {EngineMode::Parking, std::chrono::seconds(5)};
    }
}

// Monotonic, anchored deadlines: neither CPU work nor RAM allocation extends
// a phase. A hold requested before Parking takes effect *at* Parking; disabling
// it while parked starts a fresh Reverse 10s -> Drive 20s -> ... cycle.
class ParkingCycle {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    explicit ParkingCycle(Time started) { reset(started); }

    void reset(Time started) {
        phase_index_ = 0;
        deadline_ = started + phase(0).duration;
        held_ = false;
        ++generation_;
    }

    bool advance(Time now) {
        if (held_)
            return false;
        bool changed = false;
        // Catch up from delayed callbacks without adding their delay to the
        // 60-second period. Each iteration advances a positive-duration phase.
        while (now >= deadline_) {
            phase_index_ = (phase_index_ + 1) % kPhaseCount;
            ++generation_;
            changed = true;
            if (phase_index_ == kPhaseCount - 1 && hold_requested_) {
                held_ = true;
                deadline_ = Time::max();
                break;
            }
            deadline_ += phase(phase_index_).duration;
        }
        return changed;
    }

    bool set_parking_hold(bool requested, Time now) {
        if (requested == hold_requested_)
            return false;
        hold_requested_ = requested;
        if (!requested && held_) {
            held_ = false;
            phase_index_ = 0; // immediate Reverse when released
            deadline_ = now + phase(0).duration;
            ++generation_;
        } else if (requested && phase_index_ == kPhaseCount - 1) {
            // The vehicle has already reached Parking: hold immediately.
            held_ = true;
            deadline_ = Time::max();
        }
        return true;
    }

    EngineMode mode() const { return phase(phase_index_).mode; }
    std::size_t phase_index() const { return phase_index_; }
    Time deadline() const { return deadline_; }
    bool parking_hold_requested() const { return hold_requested_; }
    bool parked() const { return held_; }
    std::uint64_t generation() const { return generation_; }

private:
    std::size_t phase_index_ = 0;
    Time deadline_{};
    bool hold_requested_ = false;
    bool held_ = false;
    std::uint64_t generation_ = 0;
};

// Keep one common wall-clock period for half/full load. Halving the number
// of busy milliseconds without pacing would still saturate a CPU core.
inline std::chrono::microseconds cpu_budget(unsigned peak_ms, bool full) {
    return std::chrono::microseconds(static_cast<std::int64_t>(peak_ms) *
                                     (full ? 1000 : 500));
}
inline std::chrono::microseconds cpu_period(unsigned peak_ms) {
    return std::chrono::microseconds(static_cast<std::int64_t>(peak_ms) * 1000 + 2000);
}

class VolumePolicy {
public:
    explicit VolumePolicy(unsigned starting_volume = 50)
        : base_(std::min(100u, starting_volume)) {}

    void set_mode(EngineMode mode) {
        if (mode == mode_)
            return;
        mode_ = mode;
        // Reverse temporarily lowers the radio. The base volume remains
        // intact unless the user manually adjusts it during Reverse.
        reverse_auto_ = mode == EngineMode::Reverse;
    }

    unsigned change(int delta) {
        if (reverse_auto_) {
            base_ = 30;
            reverse_auto_ = false;
        }
        const int adjusted = static_cast<int>(base_) + delta;
        base_ = static_cast<unsigned>(std::max(0, std::min(100, adjusted)));
        return current();
    }

    unsigned current() const {
        if (reverse_auto_)
            return 30;
        return std::min(100u, base_ + (mode_ == EngineMode::Fast ? 10u : 0u));
    }
    unsigned base() const { return base_; }
    EngineMode mode() const { return mode_; }

private:
    unsigned base_;
    EngineMode mode_ = EngineMode::Drive;
    bool reverse_auto_ = false;
};

// Video is allowed only outside Fast, and camera is allowed only in Reverse.
inline bool fast_warning(EngineMode mode, bool video_view_open) {
    return mode == EngineMode::Fast && video_view_open;
}
inline bool media_load(bool local_video_playing, bool camera_frame_visible,
                       EngineMode mode) {
    return mode != EngineMode::Fast && (local_video_playing || camera_frame_visible);
}

} // namespace demo_modes
