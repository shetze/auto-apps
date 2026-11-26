#pragma once
// Small, bounded AF_UNIX SOCK_SEQPACKET protocol between an ipc-labeled SOME/IP container and its native,
// partition-local systemd resource helper.
// Only a fixed 28-byte controller::Command can be sent; never arbitrary shell
// arguments, file paths or unit names. Not exposed to the QM network.
#include "controller-protocol.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <unistd.h>

namespace controller_host_ipc {
constexpr const char *kSocket = "/run/ipc/auto-apps-controller/host.sock";
constexpr const char *kQmDirectory = "/run/auto-apps-qm-agent";
constexpr const char *kQmSocket = "/run/auto-apps-qm-agent/qm.sock";
inline bool request(const controller::Command &in, controller::Command &out,
                    const char *socket_path = kSocket, int timeout_ms = 2500) {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(socket_path) >= sizeof address.sun_path) {
        close(fd); return false;
    }
    std::strcpy(address.sun_path, socket_path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
        close(fd); return false;
    }
    const auto bytes = controller::encode(in);
    if (send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(bytes.size())) {
        close(fd); return false;
    }
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) != 1 || (pfd.revents & POLLIN) == 0) {
        close(fd); return false;
    }
    std::uint8_t reply[64];
    const ssize_t got = recv(fd, reply, sizeof reply, 0);
    close(fd);
    if (got != 28 || !controller::decode(reply, static_cast<std::size_t>(got), out)) return false;
    return out.serial == in.serial && out.target == in.target && out.knob == in.knob;
}
} // namespace controller_host_ipc
