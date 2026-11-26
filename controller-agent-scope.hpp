#pragma once
// Different systemd managers, different allowlists. Scope is a trusted
// systemd unit setting, NEVER a value from the network command payload.
#include "controller-protocol.hpp"

namespace controller_agent_scope {
enum class Scope { Root, Qm };
inline const char *unit_name(controller::Target target, Scope scope) {
    if (scope == Scope::Qm)
        return target == controller::Target::Multimedia ? "multimedia.service" : nullptr;
    switch (target) {
    case controller::Target::Qm: return "qm.service";
    case controller::Target::Engine: return "engine.service";
    case controller::Target::Sensors: return "sensors.service";
    case controller::Target::Multimedia: return nullptr;
    }
    return nullptr;
}
inline bool allowed(const controller::Command &cmd, Scope scope) {
    return unit_name(cmd.target, scope) != nullptr &&
           controller::index(cmd.knob) < 3;
}
} // namespace controller_agent_scope
