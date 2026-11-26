#include "controller-systemd-notify.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main() {
    unsetenv("NOTIFY_SOCKET");
    assert(controller_systemd_notify::ready()); // root helper/manual run
    setenv("NOTIFY_SOCKET", "", 1);
    assert(!controller_systemd_notify::ready());
    setenv("NOTIFY_SOCKET", "/does-not-exist/notify.sock", 1);
    assert(!controller_systemd_notify::ready());

    char directory[] = "/tmp/auto-apps-ready-XXXXXX";
    assert(mkdtemp(directory));
    const std::string path = std::string(directory) + "/notify.sock";
    const int listener = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
    setenv("NOTIFY_SOCKET", path.c_str(), 1);
    assert(controller_systemd_notify::ready());
    char message[80]{};
    const ssize_t received = recv(listener, message, sizeof(message) - 1, 0);
    assert(received > 0);
    assert(std::string(message).find("READY=1") == 0);
    close(listener);
    unlink(path.c_str());
    rmdir(directory);

    // The QM systemd manager may use abstract NOTIFY_SOCKET addresses.
    const int abstract_listener = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(abstract_listener >= 0);
    sockaddr_un abstract_address{};
    abstract_address.sun_family = AF_UNIX;
    const std::string abstract_name = "auto-apps-notify-test-" + std::to_string(getpid());
    std::memcpy(abstract_address.sun_path + 1, abstract_name.data(), abstract_name.size());
    const auto abstract_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + abstract_name.size());
    assert(bind(abstract_listener, reinterpret_cast<sockaddr *>(&abstract_address), abstract_length) == 0);
    setenv("NOTIFY_SOCKET", ("@" + abstract_name).c_str(), 1);
    assert(controller_systemd_notify::ready());
    std::memset(message, 0, sizeof message);
    assert(recv(abstract_listener, message, sizeof message - 1, 0) > 0);
    assert(std::string(message).find("READY=1") == 0);
    close(abstract_listener);
    unsetenv("NOTIFY_SOCKET");
    std::puts("systemd readiness (path and abstract sockets): PASS");
}
