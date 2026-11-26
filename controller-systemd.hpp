#pragma once
// systemd-only cgroup management; strict unit whitelist and bounded values.
// No shell invocation, no arbitrary paths or unit names from SOME/IP payloads.
#include "controller-protocol.hpp"
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>

namespace controller_systemd {
struct Result { bool ok = false; std::string output; };
inline Result run(const std::vector<std::string> &args) {
    int pipes[2];
    if (pipe(pipes) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        std::vector<char *> av;
        av.push_back(const_cast<char *>("systemctl"));
        for (const auto &s : args) av.push_back(const_cast<char *>(s.c_str()));
        av.push_back(nullptr);
        execv("/usr/bin/systemctl", av.data());
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return {}; }
    fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL, 0) | O_NONBLOCK);
    Result result;
    int status = -1;
    bool exited = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[512];
        ssize_t n;
        while ((n = read(pipes[0], buffer, sizeof buffer)) > 0 && result.output.size() < 8192)
            result.output.append(buffer, static_cast<std::size_t>(n));
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    char tail[512]; ssize_t n;
    while ((n = read(pipes[0], tail, sizeof tail)) > 0 && result.output.size() < 8192)
        result.output.append(tail, static_cast<std::size_t>(n));
    close(pipes[0]);
    result.ok = exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}
inline bool is_unit(const std::string &unit) {
    return unit == "qm.service" || unit == "engine.service" ||
           unit == "multimedia.service" || unit == "sensors.service";
}
inline std::string line_value(const std::string &text, const std::string &key) {
    std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size() + 1, key + "=") == 0)
            return line.substr(key.size() + 1);
    }
    return {};
}
inline bool parse_uint(const std::string &s, std::uint64_t &v) {
    if (s.empty() || s.size() > 20) return false;
    for (char ch : s) if (ch < '0' || ch > '9') return false;
    try { v = std::stoull(s); return true; } catch (...) { return false; }
}
inline std::uint64_t host_total_memory() {
    std::ifstream in("/proc/meminfo");
    std::string tag, unit; std::uint64_t kb = 0;
    if (in >> tag >> kb >> unit && tag == "MemTotal:") return kb * 1024ULL;
    return 0;
}
// Deliberately limited to a 32-core demonstration UI. Values beyond that
// cannot be represented by the wire mask and are reported as unknown.
inline bool cpu_mask(const std::string &list, std::uint32_t &mask) {
    mask = 0;
    if (list.empty()) return false;
    std::istringstream in(list); std::string part;
    while (std::getline(in, part, ',')) {
        const auto dash = part.find('-');
        std::uint64_t a = 0, b = 0;
        if (!parse_uint(part.substr(0, dash), a)) return false;
        if (dash == std::string::npos) b = a;
        else if (!parse_uint(part.substr(dash + 1), b)) return false;
        if (a > b || b >= 32) return false;
        for (auto i = a; i <= b; ++i) mask |= std::uint32_t{1} << i;
    }
    return mask != 0;
}
inline std::string mask_list(std::uint32_t mask) {
    std::string s;
    for (unsigned i = 0; i < 32; ++i) {
        if (!(mask & (1u << i))) continue;
        if (!s.empty()) s += ',';
        s += std::to_string(i);
    }
    return s;
}
inline bool cg_path(const std::string &cg) {
    return !cg.empty() && cg[0] == '/' && cg.find("..") == std::string::npos &&
           cg.find('\n') == std::string::npos;
}
inline std::string file_text(const std::string &path) {
    std::ifstream in(path);
    std::string val;
    std::getline(in, val);
    return val;
}
inline bool inspect(const std::string &unit, controller::Node &node,
                    bool qm_partition = false) {
    if (!is_unit(unit)) return false;
    const auto result = run({"show", "--property=MemoryMax", "--property=CPUWeight",
                             "--property=ControlGroup", "--property=ActiveState", unit});
    if (!result.ok) return false;
    node.available = line_value(result.output, "ActiveState") == "active" ? 1u : 0u;
    std::uint64_t val;
    if (parse_uint(line_value(result.output, "MemoryMax"), val) && val < UINT64_MAX) {
        if (qm_partition) {
            const auto host = host_total_memory();
            if (host) node.values[0] = static_cast<std::uint32_t>(std::min<std::uint64_t>(100,
                    (val / 1024ULL * 100ULL + (host / 1024ULL) / 2ULL) / (host / 1024ULL)));
        } else node.values[0] = static_cast<std::uint32_t>(std::min<std::uint64_t>(0xfffffffeULL,
                val / (1024ULL * 1024ULL)));
    } else node.values[0] = 0; // no cap (infinity)
    if (parse_uint(line_value(result.output, "CPUWeight"), val))
        node.values[1] = static_cast<std::uint32_t>(val);
    const auto group = line_value(result.output, "ControlGroup");
    if (cg_path(group)) {
        const auto cpu_list = file_text("/sys/fs/cgroup" + group + "/cpuset.cpus.effective");
        std::uint32_t mask = 0;
        if (cpu_mask(cpu_list, mask)) node.values[2] = mask;
    }
    return true;
}
inline bool safe_memory_limit(const std::string &unit, std::uint64_t limit) {
    // Avoid an immediate memory.max OOM by checking actual cgroup usage,
    // leaving 128 MiB of slack. A concurrent allocation is still possible.
    const auto result = run({"show", "--property=ControlGroup", unit});
    if (!result.ok) return false;
    const auto group = line_value(result.output, "ControlGroup");
    if (!cg_path(group)) return false;
    std::uint64_t current = 0;
    if (!parse_uint(file_text("/sys/fs/cgroup" + group + "/memory.current"), current))
        return false;
    return limit > current && limit - current >= 128ULL * 1024ULL * 1024ULL;
}

inline controller::Status apply(const std::string &unit, const controller::Command &c,
                                bool qm_partition = false) {
    if (!is_unit(unit) || !controller::valid(c)) return controller::Status::Invalid;
    std::string property;
    if (c.knob == controller::Knob::MemoryMax) {
        if (qm_partition) {
            auto total = host_total_memory();
            if (!total) return controller::Status::Unavailable;
            const auto cap = total / 100u * c.value;
            if (!safe_memory_limit(unit, cap)) return controller::Status::Denied;
            property = "MemoryMax=" + std::to_string(cap);
        } else {
            const auto cap = std::uint64_t(c.value) * 1024ULL * 1024ULL;
            if (cap && !safe_memory_limit(unit, cap)) return controller::Status::Denied;
            property = c.value ? "MemoryMax=" + std::to_string(cap)
                               : "MemoryMax=infinity";
        }
    } else if (c.knob == controller::Knob::CpuWeight) {
        property = "CPUWeight=" + std::to_string(c.value);
    } else if (c.knob == controller::Knob::Affinity) {
        // Cannot grant CPUs outside the unit's parent cpuset, even if the
        // request comes from the QM side. Inherited mask=0 clears override.
        if (c.value) {
            controller::Node node;
            if (!inspect(unit, node, qm_partition)) return controller::Status::Unavailable;
            // only allow subsets of current effective mask for this demo.
            if (node.values[2] == 0xffffffffu || (c.value & ~node.values[2]))
                return controller::Status::Denied;
        }
        property = "AllowedCPUs=" + mask_list(c.value);
    } else return controller::Status::Unsupported;
    const auto result = run({"set-property", "--runtime", unit, property});
    return result.ok ? controller::Status::Ok : controller::Status::Failed;
}
} // namespace controller_systemd
