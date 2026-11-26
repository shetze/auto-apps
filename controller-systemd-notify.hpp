#pragma once
// Minimal sd_notify(READY=1) sender.  Avoids a new libsystemd build/runtime
// dependency for the native QM resource helper.  Only the owning main process
// sends readiness, and only after its private SOCK_SEQPACKET socket listens.
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace controller_systemd_notify {
inline bool ready() {
    const char *path = std::getenv("NOTIFY_SOCKET");
    if (!path) return true; // Direct/manual invocation outside systemd.
    const std::size_t count = std::strlen(path);
    if (count == 0 || count >= sizeof(sockaddr_un::sun_path)) return false;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path[0] == '@') {
        address.sun_path[0] = '\0'; // systemd abstract notification socket
        std::memcpy(address.sun_path + 1, path + 1, count - 1);
    } else {
        std::memcpy(address.sun_path, path, count + 1);
    }
    const socklen_t address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + count + (path[0] == '@' ? 0 : 1));
    constexpr char message[] = "READY=1\nSTATUS=QM resource socket listening";
    const auto sent = sendto(fd, message, sizeof(message) - 1, MSG_NOSIGNAL,
                             reinterpret_cast<sockaddr *>(&address), address_length);
    close(fd);
    return sent == static_cast<ssize_t>(sizeof(message) - 1);
}
} // namespace controller_systemd_notify
