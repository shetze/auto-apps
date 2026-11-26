#include "controller-host-ipc.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main() {
    char pattern[] = "/tmp/auto-apps-host-ipc-XXXXXX";
    char *directory = mkdtemp(pattern);
    assert(directory);
    const std::string socket_path = std::string(directory) + "/agent.sock";
    const int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    assert(listener >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    assert(socket_path.size() < sizeof addr.sun_path);
    std::strcpy(addr.sun_path, socket_path.c_str());
    assert(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
    assert(listen(listener, 1) == 0);
    std::thread agent([&] {
        const int client = accept(listener, nullptr, nullptr);
        assert(client >= 0);
        std::uint8_t bytes[28];
        assert(recv(client, bytes, sizeof bytes, 0) == 28);
        controller::Command cmd;
        assert(controller::decode(bytes, sizeof bytes, cmd));
        assert(cmd.target == controller::Target::Qm);
        cmd.value = 40;
        cmd.status = controller::Status::Ok;
        const auto response = controller::encode(cmd);
        assert(send(client, response.data(), response.size(), 0) == 28);
        close(client);
    });
    controller::Command request;
    request.serial = 123;
    request.target = controller::Target::Qm;
    request.knob = controller::Knob::MemoryMax;
    request.status = controller::Status::Unknown;
    controller::Command response;
    assert(controller_host_ipc::request(request, response, socket_path.c_str()));
    assert(response.serial == 123 && response.value == 40 &&
           response.status == controller::Status::Ok);
    agent.join();
    close(listener);
    unlink(socket_path.c_str());
    rmdir(directory);
    assert(!controller_host_ipc::request(request, response, socket_path.c_str(), 100));
    std::cout << "host-agent unix seqpacket request/response: PASS\n";
}
