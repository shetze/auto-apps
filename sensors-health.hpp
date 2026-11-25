#pragma once

#include "demo-health.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

// Diagnostic samples for the *actual sensors worker*, rather than inventing a
// synthetic CPU load. Memory is the frame/payload buffer footprint, NOT RSS.
namespace sensors_health {

inline std::uint32_t to_u32(double value) {
    if (!(value > 0))
        return 0;
    if (value >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(std::lround(value));
}

class Tracker {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    Tracker(Time start, std::uint64_t cpu_ns)
        : last_time_(start), last_cpu_ns_(cpu_ns) {}

    void frame_finished(std::chrono::nanoseconds duration,
                        std::chrono::nanoseconds period) {
        if (duration <= period)
            return;
        ++deadline_misses_;
        ++new_misses_;
        peak_overrun_ms_ = std::max(peak_overrun_ms_,
                std::chrono::duration<double, std::milli>(duration - period).count());
    }

    demo_health::Message sample(Time now, std::uint64_t thread_cpu_ns,
                                std::uint64_t current_frame_buffer_bytes) {
        const double wall_ms = std::max(0.0,
                std::chrono::duration<double, std::milli>(now - last_time_).count());
        const double cpu_ms = thread_cpu_ns >= last_cpu_ns_
                ? static_cast<double>(thread_cpu_ns - last_cpu_ns_) / 1000000.0 : 0.0;
        demo_health::Message result;
        result.sequence = ++sequence_;
        result.cpu = new_misses_ ? demo_health::Level::Degraded : demo_health::Level::Normal;
        result.ram = demo_health::Level::Normal;
        result.cpu_metrics_valid = wall_ms >= 1.0;
        result.ram_metrics_valid = true;
        result.cpu_share_centi_percent = to_u32(wall_ms > 0
                ? std::min(100.0, cpu_ms * 100.0 / wall_ms) * 100.0 : 0.0);
        result.cpu_wall_ms = to_u32(wall_ms);
        result.cpu_off_ms = to_u32(std::max(0.0, wall_ms - cpu_ms));
        result.cpu_overrun_ms = to_u32(peak_overrun_ms_);
        result.cpu_deadline_misses = deadline_misses_;
        result.ram_requested_bytes = current_frame_buffer_bytes;
        result.ram_allocated_bytes = current_frame_buffer_bytes;
        last_time_ = now;
        last_cpu_ns_ = thread_cpu_ns;
        new_misses_ = 0;
        peak_overrun_ms_ = 0;
        return result;
    }

private:
    Time last_time_{};
    std::uint64_t last_cpu_ns_ = 0;
    std::uint32_t sequence_ = 0;
    std::uint32_t deadline_misses_ = 0;
    std::uint32_t new_misses_ = 0;
    double peak_overrun_ms_ = 0;
};

} // namespace sensors_health
