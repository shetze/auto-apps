#pragma once
// Auto-apps demo controller protocol. All fields are network-order u32.
// Fixed-size, versioned and bounded so SOME/IP callbacks never interpret text
// as commands or filesystem paths.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace controller {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kMagic = 0x41544331u; // ATC1
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kNodeCount = 4;
enum class Target : std::uint32_t { Qm = 0, Engine = 1, Multimedia = 2, Sensors = 3 };
enum class Knob : std::uint32_t {
    MemoryMax = 1, CpuWeight = 2, Affinity = 3,
    DemoCpu = 4, DemoRam = 5, DemoMutex = 6, RtPriority = 7
};
enum class Status : std::uint32_t {
    Ok = 0, Invalid = 1, Unavailable = 2, Denied = 3, Failed = 4,
    Pending = 5, Unsupported = 6, Unknown = 7
};
// value: QM MemoryMax is a percentage (never >40); per-service MemoryMax
// and DemoRam are MiB; Affinity is CPU mask bits 0..31, 0 means inherited.
struct Command {
    std::uint32_t serial = 0;
    Target target = Target::Qm;
    Knob knob = Knob::CpuWeight;
    std::uint32_t value = 0;
    Status status = Status::Ok;
};
inline void put(Bytes &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline std::uint32_t get(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
inline Bytes encode(const Command &c) {
    Bytes b;
    put(b, kMagic); put(b, kVersion); put(b, c.serial);
    put(b, static_cast<std::uint32_t>(c.target));
    put(b, static_cast<std::uint32_t>(c.knob));
    put(b, c.value); put(b, static_cast<std::uint32_t>(c.status));
    return b;
}
inline bool decode(const std::uint8_t *p, std::size_t n, Command &c) {
    if (!p || n != 28 || get(p) != kMagic || get(p + 4) != kVersion ||
        get(p + 12) > 3 || get(p + 16) < 1 || get(p + 16) > 7 ||
        get(p + 24) > 7) return false;
    c.serial = get(p + 8); c.target = static_cast<Target>(get(p + 12));
    c.knob = static_cast<Knob>(get(p + 16)); c.value = get(p + 20);
    c.status = static_cast<Status>(get(p + 24));
    return true;
}
inline bool valid(const Command &c) {
    const auto t = c.target;
    const auto k = c.knob;
    const auto v = c.value;
    switch (k) {
    case Knob::MemoryMax:
        return t == Target::Qm ? v >= 20 && v <= 40 : v == 0 || (v >= 64 && v <= 4096);
    case Knob::CpuWeight: return v >= 1 && v <= 10000 && (t != Target::Qm || v <= 100);
    case Knob::Affinity: return true; // checked against available cpuset on host
    case Knob::DemoCpu: return t != Target::Qm && v <= (t == Target::Sensors ? 300u : 2000u);
    case Knob::DemoRam: return t != Target::Qm && v <= 1024;
    case Knob::DemoMutex: return t != Target::Qm && v <= 1;
    case Knob::RtPriority: return t != Target::Qm && v <= 5;
    }
    return false;
}
struct Node {
    std::uint32_t available = 0; // service availability / QM unit accessible
    std::uint32_t health = 0; // 0 unknown, 1 normal, 2 degraded, 3 failed, 4 stale
    std::uint32_t age_ms = 0xffffffffu;
    std::array<std::uint32_t, 7> values{{0xffffffffu, 0xffffffffu, 0xffffffffu,
                                         0xffffffffu, 0xffffffffu, 0xffffffffu,
                                         0xffffffffu}};
};
struct Snapshot {
    std::uint32_t sequence = 0;
    std::array<Node, kNodeCount> nodes{};
    Command last{};
    std::uint32_t automatic = 0; // successful auto intervention count
    std::uint32_t tuning_enabled = 0;
};
constexpr std::size_t kSnapshotBytes = 8 + kNodeCount * 40 + 28 + 8;
inline Bytes encode(const Snapshot &s) {
    Bytes b; b.reserve(kSnapshotBytes);
    put(b, kMagic); put(b, s.sequence);
    for (const auto &n : s.nodes) {
        put(b, n.available); put(b, n.health); put(b, n.age_ms);
        for (auto v : n.values) put(b, v);
    }
    const auto last = encode(s.last);
    b.insert(b.end(), last.begin(), last.end());
    put(b, s.automatic); put(b, s.tuning_enabled);
    return b;
}
inline bool decode_snapshot(const std::uint8_t *p, std::size_t size, Snapshot &s) {
    if (!p || size != kSnapshotBytes || get(p) != kMagic) return false;
    Snapshot parsed;
    parsed.sequence = get(p + 4);
    std::size_t pos = 8;
    for (auto &n : parsed.nodes) {
        n.available = get(p + pos); pos += 4;
        n.health = get(p + pos); pos += 4;
        n.age_ms = get(p + pos); pos += 4;
        if (n.available > 1 || n.health > 4) return false;
        for (auto &v : n.values) { v = get(p + pos); pos += 4; }
    }
    if (!decode(p + pos, 28, parsed.last)) return false;
    pos += 28;
    parsed.automatic = get(p + pos); pos += 4;
    parsed.tuning_enabled = get(p + pos);
    s = parsed; return true;
}
inline std::size_t index(Knob k) { return static_cast<std::size_t>(k) - 1u; }
inline const char *target_name(Target t) {
    switch (t) {
    case Target::Qm: return "QM partition";
    case Target::Engine: return "Engine";
    case Target::Multimedia: return "Multimedia";
    case Target::Sensors: return "Sensors";
    }
    return "Unknown";
}
inline const char *knob_name(Knob k) {
    switch (k) {
    case Knob::MemoryMax: return "Memory max";
    case Knob::CpuWeight: return "CPU weight";
    case Knob::Affinity: return "CPU affinity";
    case Knob::DemoCpu: return "Demo CPU ms";
    case Knob::DemoRam: return "Demo RAM MiB";
    case Knob::DemoMutex: return "Demo mutex";
    case Knob::RtPriority: return "RT priority";
    }
    return "Unknown";
}
inline const char *status_name(Status s) {
    switch (s) {
    case Status::Ok: return "APPLIED";
    case Status::Invalid: return "INVALID";
    case Status::Unavailable: return "OFFLINE";
    case Status::Denied: return "DENIED";
    case Status::Failed: return "FAILED";
    case Status::Pending: return "PENDING";
    case Status::Unsupported: return "UNSUPPORTED";
    case Status::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}
} // namespace controller
