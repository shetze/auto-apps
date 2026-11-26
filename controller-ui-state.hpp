#pragma once
#include "controller-protocol.hpp"
#include "controller-auto-policy.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace controller_ui {
struct Row { controller::Target target; controller::Knob knob; int step; };
inline const std::vector<Row> &rows() {
    static const std::vector<Row> items = [] {
        std::vector<Row> r;
        for (auto t : {controller::Target::Qm, controller::Target::Engine,
                       controller::Target::Multimedia, controller::Target::Sensors}) {
            r.push_back({t, controller::Knob::MemoryMax, t == controller::Target::Qm ? 5 : 128});
            r.push_back({t, controller::Knob::CpuWeight, t == controller::Target::Qm ? 10 : 100});
            r.push_back({t, controller::Knob::Affinity, 1});
            if (t != controller::Target::Qm) {
                r.push_back({t, controller::Knob::DemoCpu, 50});
                r.push_back({t, controller::Knob::DemoRam, 64});
                r.push_back({t, controller::Knob::DemoMutex, 1});
                r.push_back({t, controller::Knob::RtPriority, 1});
            }
        }
        return r;
    }();
    return items;
}
inline std::uint32_t current(const controller::Snapshot &s, Row row) {
    return s.nodes[static_cast<std::size_t>(row.target)].values[controller::index(row.knob)];
}
inline std::uint32_t adjust(Row row, std::uint32_t value, int dir) {
    if (value == 0xffffffffu) value = 0;
    if (row.knob == controller::Knob::CpuWeight && value == 0xfffffffeu) value = 100;
    if (row.knob == controller::Knob::DemoMutex) return value ? 0u : 1u;
    if (row.knob == controller::Knob::Affinity) {
        // Bit patterns: inherited => CPU0 => CPUs0-1 => 0-3 => 0-7 => 0-15.
        const std::uint32_t presets[] = {0, 1, 3, 15, 255, 65535};
        auto it = std::find(std::begin(presets), std::end(presets), value);
        if (it == std::end(presets)) return 0;
        auto index = static_cast<int>(it - std::begin(presets));
        index = std::max(0, std::min(5, index + dir));
        return presets[index];
    }
    auto signed_value = std::int64_t(value) + dir * row.step;
    if (row.knob == controller::Knob::MemoryMax) {
        if (row.target == controller::Target::Qm)
            return static_cast<std::uint32_t>(std::max<std::int64_t>(20,
                            std::min<std::int64_t>(40, signed_value)));
        if (dir > 0 && value == 0) signed_value = 128;
        return static_cast<std::uint32_t>(std::max<std::int64_t>(0,
                            std::min<std::int64_t>(4096, signed_value)));
    }
    if (row.knob == controller::Knob::CpuWeight) {
        // 0 means idle and sits immediately below 1. Respect a larger UI
        // step above 1 but never skip the boundary on the way down.
        if (dir > 0 && value == 0) return 1;
        if (dir < 0 && value <= static_cast<std::uint32_t>(row.step))
            return value == 0 ? 0u : (value == 1 ? 0u : 1u);
        return static_cast<std::uint32_t>(std::max<std::int64_t>(0,
                   std::min<std::int64_t>(row.target == controller::Target::Qm ? 100 : 10000,
                                          signed_value)));
    }
    const auto maxv = row.knob == controller::Knob::DemoCpu ?
                (row.target == controller::Target::Sensors ? 300 : 2000) :
            row.knob == controller::Knob::DemoRam ? 1024 : 5;
    return static_cast<std::uint32_t>(std::max<std::int64_t>(0,
                    std::min<std::int64_t>(maxv, signed_value)));
}
inline std::string format(controller::Knob knob, std::uint32_t value,
                          controller::Target target) {
    if (value == 0xffffffffu) return "--";
    if (knob == controller::Knob::CpuWeight) {
        if (value == 0) return "idle";
        if (value == 0xfffffffeu) return "default (100)";
    }
    if (knob == controller::Knob::MemoryMax) {
        if (target == controller::Target::Qm) return std::to_string(value) + "%";
        return value ? std::to_string(value) + " MiB" : "unlimited";
    }
    if (knob == controller::Knob::Affinity) {
        if (!value) return "inherited";
        std::ostringstream o; o << "0x" << std::hex << std::uppercase << value;
        return o.str();
    }
    if (knob == controller::Knob::DemoMutex) return value ? "HELD" : "FREE";
    if (knob == controller::Knob::RtPriority) return value ?
            "FIFO " + std::to_string(value) : "SCHED_OTHER";
    return std::to_string(value);
}
} // namespace controller_ui

namespace controller_ui {
inline std::string with_status(controller::Knob knob, std::uint32_t value,
                               controller::Target target, controller::Status status) {
    if (target == controller::Target::Qm && controller::index(knob) >= 3) return "n/a";
    if (status == controller::Status::Ok) return format(knob, value, target);
    if (status == controller::Status::Pending) return "-- (PENDING)";
    if (status == controller::Status::Unknown) return "-- (UNKNOWN)";
    if (status == controller::Status::Unsupported) return "-- (N/A)";
    return "-- (" + std::string(controller::status_name(status)) + ")";
}
} // namespace controller_ui
