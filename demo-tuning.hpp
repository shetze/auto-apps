#pragma once
#include "controller-protocol.hpp"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>

// Thread-safe runtime settings for synthetic work only. No process cgroup
// or global scheduler configuration is changed here.
namespace demo_tuning {
inline std::uint32_t from_env(const char *key, std::uint32_t fallback, std::uint32_t max) {
    const char *s = std::getenv(key);
    if (!s || !*s || *s == '-' || *s == '+') return fallback;
    errno = 0; char *end = nullptr;
    const auto n = std::strtoul(s, &end, 10);
    return errno || end == s || *end || n > max ? fallback : static_cast<std::uint32_t>(n);
}
struct Values {
    std::uint32_t cpu_ms = 0;
    std::uint32_t ram_mib = 0;
    std::uint32_t mutex = 0;
    std::uint32_t rt_priority = 0;
};
class Runtime {
public:
    explicit Runtime(Values initial)
      : cpu_ms_(initial.cpu_ms), ram_mib_(initial.ram_mib),
        mutex_(initial.mutex), rt_priority_(initial.rt_priority) {}
    Values read() const {
        return {cpu_ms_.load(), ram_mib_.load(), mutex_.load(), rt_priority_.load()};
    }
    controller::Status apply(controller::Knob knob, std::uint32_t value,
                             pthread_t worker, bool allow_rt = false) {
        switch (knob) {
        case controller::Knob::DemoCpu:
            if (value > 2000) return controller::Status::Invalid;
            cpu_ms_.store(value); return controller::Status::Ok;
        case controller::Knob::DemoRam:
            if (value > 1024) return controller::Status::Invalid;
            ram_mib_.store(value); return controller::Status::Ok;
        case controller::Knob::DemoMutex:
            if (value > 1) return controller::Status::Invalid;
            mutex_.store(value); return controller::Status::Ok;
        case controller::Knob::RtPriority: {
            if (value > 5) return controller::Status::Invalid;
            if (value && !allow_rt) return controller::Status::Denied;
            sched_param param{};
            param.sched_priority = static_cast<int>(value);
            if (pthread_setschedparam(worker, value ? SCHED_FIFO : SCHED_OTHER, &param) != 0)
                return controller::Status::Denied;
            rt_priority_.store(value);
            return controller::Status::Ok;
        }
        default: return controller::Status::Unsupported;
        }
    }
    std::uint32_t value(controller::Knob knob) const {
        const auto v = read();
        switch (knob) {
        case controller::Knob::DemoCpu: return v.cpu_ms;
        case controller::Knob::DemoRam: return v.ram_mib;
        case controller::Knob::DemoMutex: return v.mutex;
        case controller::Knob::RtPriority: return v.rt_priority;
        default: return 0xffffffffu;
        }
    }
private:
    std::atomic<std::uint32_t> cpu_ms_, ram_mib_, mutex_, rt_priority_;
};
inline std::uint32_t to_mib(std::uint64_t bytes) {
    return static_cast<std::uint32_t>(bytes / (1024ULL * 1024ULL));
}
} // namespace demo_tuning
