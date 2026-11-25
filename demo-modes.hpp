#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Pure (no SOME/IP, no UI) rules for the three engine phases, CPU demo
// duty cycle and the automatic radio-volume changes.
namespace demo_modes {

enum class EngineMode : std::uint32_t { Drive = 0, Reverse = 1, Fast = 2 };

inline bool decode_mode(std::uint32_t raw, EngineMode &mode) {
    if (raw > static_cast<std::uint32_t>(EngineMode::Fast))
        return false;
    mode = static_cast<EngineMode>(raw);
    return true;
}

inline const char *name(EngineMode mode) {
    switch (mode) {
    case EngineMode::Drive: return "Drive";
    case EngineMode::Reverse: return "Reverse";
    case EngineMode::Fast: return "Fast";
    }
    return "Unknown";
}

struct Phase {
    EngineMode mode;
    std::chrono::seconds duration;
};

// Return to Drive before entering Reverse: no direct Fast/Reverse transition.
constexpr std::size_t kPhaseCount = 4;
inline Phase phase(std::size_t index) {
    switch (index % kPhaseCount) {
    case 0: return {EngineMode::Drive, std::chrono::seconds(20)};
    case 1: return {EngineMode::Fast, std::chrono::seconds(10)};
    case 2: return {EngineMode::Drive, std::chrono::seconds(10)};
    default: return {EngineMode::Reverse, std::chrono::seconds(10)};
    }
}

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
