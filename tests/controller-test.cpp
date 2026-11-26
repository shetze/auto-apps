#include "controller-protocol.hpp"
#include "controller-systemd.hpp"
#include "controller-ui-state.hpp"
#include "demo-tuning.hpp"
#include "fast-warning.hpp"
#include <cassert>
#include <iostream>
#include <numeric>

int main() {
    using namespace controller;
    Command set{15, Target::Qm, Knob::MemoryMax, 40, Status::Pending};
    assert(valid(set));
    auto raw = encode(set);
    Command got;
    assert(decode(raw.data(),raw.size(),got));
    assert(got.serial == 15 && got.value == 40 && got.target == Target::Qm);
    set.value = 41; assert(!valid(set));
    set.value = 19; assert(!valid(set));
    set.value = 20; assert(valid(set));
    set.knob = Knob::DemoCpu; assert(!valid(set));
    set.target = Target::Multimedia; set.value = 2000; assert(valid(set));
    set.value = 2001; assert(!valid(set));
    set.target = Target::Sensors; set.value = 300; assert(valid(set));
    set.knob = Knob::DemoMutex; set.value = 2; assert(!valid(set));
    set.value = 1; assert(valid(set));
    raw = encode(set);
    auto short_data = raw; short_data.pop_back();
    assert(!decode(short_data.data(), short_data.size(), got));
    raw[0] = 0;
    assert(!decode(raw.data(),raw.size(),got));
    Snapshot s;
    s.sequence = 128;
    s.tuning_enabled = 1;
    s.last = set;
    s.nodes[0].available = 1;
    s.nodes[0].values[0] = 40;
    s.nodes[0].statuses[0] = Status::Ok;
    auto bytes = encode(s);
    assert(bytes.size() == kSnapshotBytes);
    Snapshot decoded;
    assert(decode_snapshot(bytes.data(), bytes.size(), decoded));
    assert(decoded.sequence == 128 && decoded.tuning_enabled == 1);
    assert(decoded.nodes[0].values[0] == 40);
    assert(decoded.nodes[0].statuses[0] == Status::Ok);
    assert(controller_ui::format(Knob::CpuWeight, 0, Target::Qm) == "idle");
    assert(controller_ui::format(Knob::CpuWeight, 0xfffffffeu, Target::Engine) == "default (100)");
    assert(controller_ui::with_status(Knob::DemoCpu, 0xffffffffu, Target::Engine, Status::Timeout) == "-- (TIMEOUT)");
    bytes.back() = 9;
    assert(decode_snapshot(bytes.data(), bytes.size(), decoded));
    bytes[0] = 3;
    assert(!decode_snapshot(bytes.data(), bytes.size(), decoded));
    assert(controller_systemd::cpu_weight("idle") == 0);
    assert(controller_systemd::cpu_weight("[not set]") == 0xfffffffeu);
    assert(controller_systemd::cpu_weight("50") == 50);
    assert(controller_systemd::cpu_weight("not an int") == 0xffffffffu);
    assert(controller_systemd::is_unit("engine.service"));
    assert(!controller_systemd::is_unit("sshd.service"));
    assert(!controller_systemd::is_unit("../../etc/passwd"));
    assert(controller_systemd::cg_path("/system.slice/qm.service"));
    assert(!controller_systemd::cg_path("/tmp/../../etc"));
    std::uint32_t mask = 0;
    assert(controller_systemd::cpu_mask("0-3,5,7", mask));
    assert(mask == 0xafu);
    assert(controller_systemd::mask_list(mask) == "0,1,2,3,5,7");
    assert(!controller_systemd::cpu_mask("0-64", mask));
    assert(!controller_systemd::cpu_mask("1-0", mask));
    assert(!controller_systemd::cpu_mask("0;rm -rf /", mask));
    auto &rows = controller_ui::rows();
    assert(rows.size() == 24);
    for (auto row : rows) {
        std::uint32_t val = row.target == Target::Qm && row.knob == Knob::MemoryMax
                ? 40 : 0;
        for (int i=0;i<5;++i) {
            val = controller_ui::adjust(row, val, +1);
            Command test{1,row.target,row.knob,val,Status::Pending};
            assert(controller::valid(test));
        }
    }
    const auto picture = fast_warning::artwork();
    assert(picture.size() == 13);
    assert(std::all_of(picture.begin(), picture.end(), [](const std::string &l) {
        return l.size() == 58 && (l[0] == '+' || l[0] == '|') &&
               (l[57] == '+' || l[57] == '|');
    }));
    assert(std::accumulate(picture.begin(),picture.end(),std::string{}).find("VIDEO DISABLED")
           != std::string::npos);
    auto text = std::accumulate(picture.begin(), picture.end(), std::string{});
    assert(text.find("DEAKTIVIERT") == std::string::npos);
    assert(text.find("SICHERHEIT") == std::string::npos);
    demo_tuning::Runtime runtime({400,10,1,0});
    assert(runtime.read().cpu_ms == 400 && runtime.read().ram_mib == 10);
    assert(runtime.apply(Knob::DemoCpu, 120, pthread_self()) == Status::Ok);
    assert(runtime.read().cpu_ms == 120);
    assert(runtime.apply(Knob::DemoCpu, 2001, pthread_self()) == Status::Invalid);
    assert(runtime.apply(Knob::DemoMutex, 0, pthread_self()) == Status::Ok);
    assert(runtime.apply(Knob::DemoRam, 512, pthread_self()) == Status::Ok);
    assert(runtime.apply(Knob::RtPriority, 1, pthread_self()) == Status::Denied);
    assert(runtime.value(Knob::DemoRam) == 512);
    assert(runtime.value(Knob::MemoryMax) == 0xffffffffu);
    std::cout << "controller protocol, safety bounds, UI and tuning: PASS\n";
}
