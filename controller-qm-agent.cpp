// Runs as a QM ipc-labeled SOME/IP container. It has NO systemd host bus.
// A native QM resource helper owns the local systemd manager, and only a
// fixed Multimedia unit/knob allowlist is reachable over a private AF_UNIX
// socket. Do not run this process as a native QM systemd IPC application.
#include "services.hpp"
#include "vsomeip-identity.hpp"
#include "controller-host-ipc.hpp"
#include <vsomeip/vsomeip.hpp>
#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace {
std::atomic<bool> stopping{false};
void signal_handler(int) { stopping.store(true); }
class Agent {
public:
    Agent() : app_(vsomeip::runtime::get()->create_application("Controller QM Agent")) {}
    bool init() {
        if (!app_ || !vsomeip_identity::init(app_, CONTROLLER_AGENT_SERVICE_ID)) return false;
        app_->register_state_handler([this](vsomeip::state_type_e state) {
            if (state == vsomeip::state_type_e::ST_REGISTERED) {
                std::cerr << "QM IPC: registered as SOME/IP client 0x1005\n";
                app_->offer_service(CONTROLLER_AGENT_SERVICE_ID, CONTROLLER_AGENT_INSTANCE_ID);
            }
        });
        app_->register_message_handler(CONTROLLER_AGENT_SERVICE_ID,
                CONTROLLER_AGENT_INSTANCE_ID, CONTROLLER_AGENT_APPLY_METHOD_ID,
                [this](const std::shared_ptr<vsomeip::message> &request) {
                    auto payload = request->get_payload();
                    controller::Command cmd;
                    if (!payload || !controller::decode(payload->get_data(),
                            payload->get_length(), cmd) ||
                            cmd.target != controller::Target::Multimedia) return;
                    if (request->get_client() != CONTROLLER_SERVICE_ID) {
                        vsomeip_identity::denied("QM AGENT", request->get_client(), CONTROLLER_SERVICE_ID);
                        cmd.status = controller::Status::Denied;
                    } else if (cmd.knob != controller::Knob::MemoryMax &&
                        cmd.knob != controller::Knob::CpuWeight &&
                        cmd.knob != controller::Knob::Affinity) {
                        cmd.status = controller::Status::Unsupported;
                    } else if (cmd.status == controller::Status::Unknown ||
                               cmd.status == controller::Status::Pending) {
                        controller::Command reply;
                        if (controller_host_ipc::request(cmd, reply,
                                controller_host_ipc::kQmSocket, 3000)) {
                            cmd = reply;
                        } else {
                            cmd.status = controller::Status::Unavailable;
                            std::cerr << "QM IPC: native resource helper unavailable at "
                                      << controller_host_ipc::kQmSocket << '\n';
                        }
                    } else cmd.status = controller::Status::Invalid;
                    auto response = vsomeip::runtime::get()->create_response(request);
                    auto bytes = vsomeip::runtime::get()->create_payload();
                    bytes->set_data(controller::encode(cmd));
                    response->set_payload(bytes);
                    app_->send(response);
                });
        return true;
    }
    void start() { app_->start(); }
    void stop() { app_->stop(); }
private:
    std::shared_ptr<vsomeip::application> app_;
};
} // namespace
int main() {
    Agent agent;
    if (!agent.init()) return 1;
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    std::thread signal_watcher([&] {
        while (!stopping.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        agent.stop();
    });
    agent.start();
    stopping.store(true);
    signal_watcher.join();
    return 0;
}
