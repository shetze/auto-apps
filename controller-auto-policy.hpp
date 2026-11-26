#pragma once
// Closed-loop controller for synthetic Multimedia CPU load ONLY.
// No changes to cgroup CPUWeight, RAM, RT scheduling or Engine behavior.
// All methods are called under the Controller's state mutex. Decisions are
// pending until a matching SOME/IP response is received; GET observations
// establish an initial baseline and verify the effective budget.
#include "demo-health.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace controller_auto {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kUnknown = 0xffffffffu;
constexpr std::uint32_t kMinimumCpuMs = 20;
constexpr unsigned kOverrunSamples = 5;
constexpr auto kReductionCooldown = std::chrono::seconds(60);
constexpr auto kRecoveryWindow = std::chrono::seconds(90);
constexpr auto kRecoveryCooldown = std::chrono::seconds(90);
constexpr auto kSampleGap = std::chrono::milliseconds(2500);

enum class Action : std::uint32_t { None = 0, Reduce = 1, Restore = 2 };
enum class Reason : std::uint32_t {
    None = 0, CpuOverrun = 1, StableHealth = 2, ManualOverride = 3,
    Timeout = 4, Rejected = 5
};
struct Decision { Action action = Action::None; std::uint32_t from = 0; std::uint32_t to = 0; };

class Policy {
public:
    void configured_baseline(std::uint32_t value) {
        if (baseline_ == kUnknown && value > 0 && value <= 2000)
            baseline_ = value;
    }
    void observe(std::uint32_t value) {
        if (value == kUnknown || pending_ != Action::None || manual_pending_) return;
        // A fresh GET can reveal an external/manual change after a timeout.
        // Treat such a change as a new user baseline instead of silently
        // overriding it with our old desired target. Confirmed auto writes
        // update current_ first, so their matching GET does not reset it.
        if (current_ != kUnknown && current_ != value) {
            baseline_ = value;
            last_reason_ = Reason::ManualOverride;
            reset_streaks();
            next_action_ = Clock::now() + kRecoveryWindow;
        }
        current_ = value;
        if (baseline_ == kUnknown) baseline_ = value;
    }
    // An explicit user intervention takes precedence over automatic policy.
    // Its new target becomes the baseline only if the service acknowledges it.
    void manual_submitted(Clock::time_point now) {
        manual_pending_ = true;
        reset_streaks();
        next_action_ = now + kRecoveryWindow;
    }
    void manual_result(bool ok, std::uint32_t value, Clock::time_point now) {
        manual_pending_ = false;
        if (ok) {
            baseline_ = current_ = value;
            last_reason_ = Reason::ManualOverride;
        }
        reset_streaks();
        next_action_ = now + kRecoveryWindow;
    }
    void no_fresh_health() { reset_streaks(); }

    Decision health(const demo_health::Message &data, Clock::time_point now,
                    bool enabled, bool online, bool value_ready) {
        if (pending_ != Action::None || manual_pending_ || !enabled || !online ||
            !value_ready || current_ == kUnknown || baseline_ == kUnknown ||
            !data.cpu_metrics_valid) {
            reset_streaks();
            return {};
        }
        // A gap invalidates 'consecutive samples' and 'stable for 90s'.
        if (last_sample_ != Clock::time_point{} && now - last_sample_ > kSampleGap)
            reset_streaks();
        last_sample_ = now;
        // If almost the entire observation interval was off-CPU, the
        // deadline miss could be mutex blocking, throttling or descheduling.
        // The v1 telemetry cannot reliably attribute that to CPU demand:
        // fail closed rather than automatically altering the workload.
        if (data.cpu_wall_ms != 0 &&
            4ull * data.cpu_off_ms >= 3ull * data.cpu_wall_ms) {
            reset_streaks();
            return {};
        }
        const bool overrun = (data.cpu == demo_health::Level::Degraded ||
                              data.cpu == demo_health::Level::Failed) &&
                              data.cpu_overrun_ms > 0;
        const bool stable = data.cpu == demo_health::Level::Normal &&
                            data.cpu_overrun_ms == 0 &&
                            (!data.ram_metrics_valid || data.ram == demo_health::Level::Normal);
        if (overrun) {
            ++overrun_samples_;
            stable_since_ = Clock::time_point{};
        } else if (stable) {
            overrun_samples_ = 0;
            if (stable_since_ == Clock::time_point{}) stable_since_ = now;
        } else {
            reset_streaks();
            return {}; // e.g. RAM failure, uncertain CPU or stale CPU warning
        }
        if (now < next_action_) return {};
        if (overrun && overrun_samples_ >= kOverrunSamples && current_ > kMinimumCpuMs) {
            const auto target = std::max(kMinimumCpuMs, current_ / 2);
            if (target < current_) return begin(Action::Reduce, target, now);
        }
        if (stable && current_ < baseline_ && stable_since_ != Clock::time_point{} &&
            now - stable_since_ >= kRecoveryWindow) {
            // One bounded step at a time; require a NEW stable window for
            // every subsequent step. Never exceed the last manual baseline.
            const auto target = std::min(baseline_, static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(2ull * current_ + (current_ == 0),
                                            std::numeric_limits<std::uint32_t>::max())));
            if (target > current_) return begin(Action::Restore, target, now);
        }
        return {};
    }
    // Called only for the pending auto SET serial; late responses are ignored
    // by the Controller before this method runs.
    void result(bool ok, std::uint32_t value, Clock::time_point now,
                bool timeout = false) {
        if (pending_ == Action::None) return;
        if (ok && value == pending_target_) {
            current_ = value;
            if (pending_ == Action::Reduce) {
                ++reductions_;
                last_reason_ = Reason::CpuOverrun;
            } else {
                ++restores_;
                last_reason_ = Reason::StableHealth;
            }
            last_from_ = pending_from_;
            last_to_ = value;
        } else {
            last_reason_ = timeout ? Reason::Timeout : Reason::Rejected;
            // Do not assume the value did NOT change on a missing reply.
            current_ = kUnknown;
        }
        next_action_ = now + (pending_ == Action::Reduce ? kReductionCooldown : kRecoveryCooldown);
        pending_ = Action::None;
        reset_streaks();
    }
    std::uint32_t baseline() const { return baseline_; }
    std::uint32_t current() const { return current_; }
    std::uint32_t reductions() const { return reductions_; }
    std::uint32_t restores() const { return restores_; }
    std::uint32_t last_from() const { return last_from_; }
    std::uint32_t last_to() const { return last_to_; }
    Reason last_reason() const { return last_reason_; }
    Action pending() const { return pending_; }
    bool manual_pending() const { return manual_pending_; }
private:
    Decision begin(Action action, std::uint32_t target, Clock::time_point now) {
        Decision plan{action, current_, target};
        pending_ = action;
        pending_from_ = current_;
        pending_target_ = target;
        next_action_ = now + kReductionCooldown;
        reset_streaks();
        return plan;
    }
    void reset_streaks() {
        overrun_samples_ = 0;
        stable_since_ = Clock::time_point{};
        last_sample_ = Clock::time_point{};
    }
    std::uint32_t baseline_ = kUnknown, current_ = kUnknown;
    std::uint32_t last_from_ = kUnknown, last_to_ = kUnknown;
    std::uint32_t pending_from_ = 0, pending_target_ = 0;
    std::uint32_t reductions_ = 0, restores_ = 0;
    unsigned overrun_samples_ = 0;
    bool manual_pending_ = false;
    Action pending_ = Action::None;
    Reason last_reason_ = Reason::None;
    Clock::time_point stable_since_{}, last_sample_{}, next_action_{};
};
inline const char *action_name(Action action) {
    switch (action) {
    case Action::Reduce: return "REDUCE";
    case Action::Restore: return "RESTORE";
    case Action::None: return "NONE";
    }
    return "NONE";
}
inline const char *reason_name(Reason reason) {
    switch (reason) {
    case Reason::None: return "NONE";
    case Reason::CpuOverrun: return "CPU OVERRUN";
    case Reason::StableHealth: return "STABLE HEALTH";
    case Reason::ManualOverride: return "MANUAL SET";
    case Reason::Timeout: return "TIMEOUT";
    case Reason::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}
} // namespace controller_auto
