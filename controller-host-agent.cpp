// Native root-partition helper for the SOME/IP controller container.
// SECURITY: A fixed socket mounted ONLY into the Controller container, a
// root-peer check, and a hard-coded unit/knob allowlist. Does not accept
// arbitrary systemctl commands and never acts as a SOME/IP participant.
#include "controller-host-ipc.hpp"
#include "controller-systemd.hpp"
#include "controller-agent-scope.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace {
std::atomic<bool> stopping{false};
void signal_handler(int) { stopping.store(true); }
using controller_agent_scope::Scope;
controller::Command handle(controller::Command command, bool tuning, Scope scope) {
    const char *unit = controller_agent_scope::unit_name(command.target, scope);
    if (!unit) { command.status = controller::Status::Unsupported; return command; }
    const unsigned key = controller::index(command.knob);
    if (key >= 3) { command.status = controller::Status::Unsupported; return command; }
    if (command.status == controller::Status::Unknown) {
        controller::Node node;
        if (controller_systemd::inspect(unit, node,
                                         command.target == controller::Target::Qm) &&
            node.values[key] != 0xffffffffu) {
            command.value = node.values[key];
            command.status = controller::Status::Ok;
        } else command.status = controller::Status::Unavailable;
    } else if (command.status == controller::Status::Pending) {
        if (!tuning) command.status = controller::Status::Denied;
        else if (!controller::valid(command)) command.status = controller::Status::Invalid;
        else {
            command.status = controller_systemd::apply(unit, command,
                                              command.target == controller::Target::Qm);
            if (command.status == controller::Status::Ok) {
                controller::Node actual;
                if (controller_systemd::inspect(unit, actual,
                        command.target == controller::Target::Qm) &&
                    actual.values[key] != 0xffffffffu)
                    command.value = actual.values[key];
                else command.status = controller::Status::Unavailable;
            }
        }
    } else command.status = controller::Status::Invalid;
    return command;
}
void serve(int peer, bool tuning, Scope scope) {
    // A peer must be host-root (the rootful controller container) and
    // obtain access through the deliberately private socket mount.
    struct ucred credentials{};
    socklen_t length = sizeof credentials;
    if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
        length != sizeof credentials || credentials.uid != 0) {
        std::cerr << "HOST-AGENT: rejected non-root socket peer\n";
        return;
    }
    pollfd pfd{peer, POLLIN, 0};
    if (poll(&pfd, 1, 3000) != 1 || !(pfd.revents & POLLIN)) return;
    std::uint8_t raw[64]{};
    const ssize_t length_read = recv(peer, raw, sizeof raw, 0);
    controller::Command command;
    if (length_read != 28 ||
        !controller::decode(raw, static_cast<std::size_t>(length_read), command))
        return;
    command = handle(command, tuning, scope);
    const auto reply = controller::encode(command);
    if (send(peer, reply.data(), reply.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(reply.size()))
        std::cerr << "HOST-AGENT: response could not be delivered\n";
}
}
int main() {
    const bool tuning = std::getenv("AUTO_APPS_HOST_AGENT_TUNING") &&
        std::strcmp(std::getenv("AUTO_APPS_HOST_AGENT_TUNING"), "1") == 0;
    const char *scope_env = std::getenv("AUTO_APPS_AGENT_SCOPE");
    if (scope_env && std::strcmp(scope_env, "qm") != 0 &&
        std::strcmp(scope_env, "root") != 0) {
        std::cerr << "RESOURCE-AGENT: invalid scope\n";
        return 1;
    }
    const Scope scope = scope_env && std::strcmp(scope_env, "qm") == 0
                          ? Scope::Qm : Scope::Root;
    const char *socket_path = scope == Scope::Qm
        ? controller_host_ipc::kQmSocket : controller_host_ipc::kSocket;
    const char *directory_path = scope == Scope::Qm
        ? "/run/ipc/auto-apps-qm-agent" : "/run/ipc/auto-apps-controller";
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("HOST-AGENT: socket"); return 1; }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, socket_path);
    // Socket parent is created by the respective partition tmpfiles service.
    // Never follow an attacker-created path or replace a live agent.
    struct stat directory{};
    if (lstat(directory_path, &directory) != 0 ||
        !S_ISDIR(directory.st_mode) || directory.st_uid != 0 ||
        (directory.st_mode & 0022) != 0) {
        std::cerr << "HOST-AGENT: insecure or missing socket directory\n";
        close(fd); return 1;
    }
    struct stat existing{};
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != 0) {
            std::cerr << "HOST-AGENT: refusing to replace foreign path\n";
            close(fd); return 1;
        }
        const int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (probe < 0) { close(fd); return 1; }
        const int result = connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof address);
        const int error = errno;
        close(probe);
        if (result == 0 || error != ECONNREFUSED ||
            unlink(socket_path) != 0) {
            std::cerr << "HOST-AGENT: live or inaccessible previous socket\n";
            close(fd); return 1;
        }
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
        std::perror("HOST-AGENT: bind"); close(fd); return 1;
    }
    chmod(socket_path, 0600);
    if (listen(fd, 8) != 0) {
        std::perror("HOST-AGENT: listen"); close(fd);
        unlink(socket_path); return 1;
    }
    std::cout << "RESOURCE-AGENT: scope=" << (scope == Scope::Qm ? "QM" : "Root")
              << " socket=" << socket_path << " resource tuning " <<
        (tuning ? "ENABLED" : "LOCKED") << std::endl;
    while (!stopping.load()) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0 || !(pfd.revents & POLLIN)) continue;
        const int peer = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) continue;
        serve(peer, tuning, scope);
        close(peer);
    }
    close(fd);
    unlink(socket_path);
    return 0;
}
