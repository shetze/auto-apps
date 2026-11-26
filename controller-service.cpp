// AutoSD root-partition controller: read-only service-health monitoring,
// bounded opt-in tuning, and conservative QM-only demo load mitigation.
// NEVER accepts a filesystem path, service name or arbitrary systemctl args.
// IMPORTANT: SOME/IP app IDs are not an authentication mechanism. Tuning must
// only be enabled on an isolated, trusted demo network; this is not a safety
// controller or a substitute for certified FFI mechanisms.
#include "services.hpp"
#include "vsomeip-identity.hpp"
#include "demo-health.hpp"
#include "controller-auto-policy.hpp"
#include "demo-tuning.hpp"
#include "controller-host-ipc.hpp"
#include <vsomeip/vsomeip.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> signal_stop{false};
void on_signal(int) { signal_stop.store(true); }
constexpr std::array<vsomeip::service_t, 4> ids{{0, ENGINE_SERVICE_ID,
                                                  RADIO_SERVICE_ID, SENSORS_SERVICE_ID}};
constexpr std::array<vsomeip::event_t, 4> health_events{{0, ENGINE_HEALTH_EVENT_ID,
                                                         RADIO_HEALTH_EVENT_ID,
                                                         SENSORS_HEALTH_EVENT_ID}};
constexpr std::array<vsomeip::eventgroup_t, 4> health_groups{{0, ENGINE_HEALTH_EVENTGROUP_ID,
                                                               RADIO_HEALTH_EVENTGROUP_ID,
                                                               SENSORS_HEALTH_EVENTGROUP_ID}};
class Controller {
public:
    Controller()
      : app_(vsomeip::runtime::get()->create_application("Controller")),
        enabled_(std::getenv("AUTO_APPS_CONTROLLER_TUNING") &&
                 std::string(std::getenv("AUTO_APPS_CONTROLLER_TUNING")) == "1") {
        state_.tuning_enabled = enabled_ ? 1u : 0u;
        state_.last.status = controller::Status::Unknown;
        auto_policy_.configured_baseline(demo_tuning::from_env(
                "AUTO_APPS_MM_CPU_BASELINE_MS", 0, 2000));
    }
    bool init() {
        if (!app_ || !vsomeip_identity::init(app_, CONTROLLER_SERVICE_ID)) return false;
        app_->register_state_handler([this](vsomeip::state_type_e s) {
            if (s == vsomeip::state_type_e::ST_REGISTERED) {
                app_->offer_service(CONTROLLER_SERVICE_ID, CONTROLLER_INSTANCE_ID);
                std::lock_guard<std::mutex> lock(mutex_);
                registered_ = true;
                cv_.notify_all();
            }
        });
        app_->register_message_handler(CONTROLLER_SERVICE_ID, CONTROLLER_INSTANCE_ID,
                CONTROLLER_SET_METHOD_ID,
                [this](const std::shared_ptr<vsomeip::message> &request) {
                    handle_client_request(request);
                });
        app_->offer_event(CONTROLLER_SERVICE_ID, CONTROLLER_INSTANCE_ID,
                CONTROLLER_STATUS_EVENT_ID,
                std::set<vsomeip::eventgroup_t>{CONTROLLER_EVENTGROUP_ID},
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        for (unsigned i=1; i<4; ++i) {
            app_->register_availability_handler(ids[i], 1,
                    [this,i](vsomeip::service_t, vsomeip::instance_t, bool available) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        state_.nodes[i].available = available ? 1u : 0u;
                        if (!available) {
                    state_.nodes[i].health = 4;
                    for (unsigned k=3; k<7; ++k) {
                        state_.nodes[i].statuses[k] = controller::Status::Unavailable;
                        state_.nodes[i].values[k] = 0xffffffffu;
                        pending_serial_[i][k] = 0;
                    }
                }
                        cv_.notify_all();
                    });
            app_->register_message_handler(ids[i], 1, health_events[i],
                    [this,i](const std::shared_ptr<vsomeip::message> &msg) {
                        receive_health(i, msg);
                    });
            app_->request_event(ids[i], 1, health_events[i],
                    std::set<vsomeip::eventgroup_t>{health_groups[i]},
                    vsomeip::event_type_e::ET_FIELD);
            app_->subscribe(ids[i], 1, health_groups[i]);
            app_->register_message_handler(ids[i], 1, DEMO_TUNE_METHOD_ID,
                    [this](const std::shared_ptr<vsomeip::message> &msg) {
                        receive_reply(msg);
                    });
            app_->request_service(ids[i], 1);
        }
        app_->register_availability_handler(CONTROLLER_AGENT_SERVICE_ID, 1,
                [this](vsomeip::service_t, vsomeip::instance_t, bool value) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (agent_online_ != value)
                        std::cerr << "CONTROLLER: QM agent SOME/IP service "
                                  << (value ? "AVAILABLE" : "UNAVAILABLE") << '\n';
                    agent_online_ = value;
                    if (!value) {
                        for (unsigned k=0; k<3; ++k) {
                            state_.nodes[2].statuses[k] = controller::Status::Unavailable;
                            state_.nodes[2].values[k] = 0xffffffffu;
                            pending_serial_[2][k] = 0;
                        }
                    }
                    cv_.notify_all();
                });
        app_->register_message_handler(CONTROLLER_AGENT_SERVICE_ID, 1,
                CONTROLLER_AGENT_APPLY_METHOD_ID,
                [this](const std::shared_ptr<vsomeip::message> &msg) {
                    receive_reply(msg);
                });
        app_->request_service(CONTROLLER_AGENT_SERVICE_ID, 1);
        return true;
    }
    void start() {
        // Keep the 1Hz status heartbeat independent of blocking systemctl
        // calls. A slow systemd manager must not make the UI appear stale.
        std::thread worker([this] { loop(); });
        std::thread observer([this] { observe_systemd(); });
        app_->start();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        worker.join();
        observer.join();
    }
    void stop() { app_->stop(); }
private:
    void receive_health(unsigned index, const std::shared_ptr<vsomeip::message> &msg) {
        auto p = msg->get_payload();
        demo_health::Message data;
        if (!p || !demo_health::decode(p->get_data(), p->get_length(), data)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto &node = state_.nodes[index];
        node.health = std::max(std::uint32_t(data.cpu), std::uint32_t(data.ram));
        last_health_[index] = Clock::now();
        node.age_ms = 0;
        // CPU deadline misses, not RAM pressure or a held demo mutex, trigger
        // synthetic QM load mitigation. The policy records the initial budget
        // and restores it only after stable, fresh telemetry.
        if (index == 2) {
            const auto now = Clock::now();
            const auto key = controller::index(controller::Knob::DemoCpu);
            const auto decision = auto_policy_.health(data, now, enabled_,
                    node.available != 0,
                    node.statuses[key] == controller::Status::Ok);
            if (decision.action != controller_auto::Action::None) {
                controller::Command command;
                command.serial = 0x80000000u | (++auto_serial_ & 0x3fffffffu);
                command.target = controller::Target::Multimedia;
                command.knob = controller::Knob::DemoCpu;
                command.value = decision.to;
                command.status = controller::Status::Pending;
                pending_auto_serial_ = command.serial;
                pending_auto_at_ = now;
                queued_.push_back(command);
                std::cerr << "CONTROLLER AUTO: proposed "
                          << controller_auto::action_name(decision.action)
                          << " " << decision.from << " -> " << decision.to
                          << " CPU ms because "
                          << (decision.action == controller_auto::Action::Reduce
                              ? "measured CPU deadline overruns" : "90s stable CPU/RAM health")
                          << ", sample=" << data.sequence << '\n';
                cv_.notify_all();
            }
        }
    }
    void handle_client_request(const std::shared_ptr<vsomeip::message> &request) {
        controller::Command cmd;
        auto p = request->get_payload();
        if (!p || !controller::decode(p->get_data(), p->get_length(), cmd)) return;
        // Check routing-manager application ID in addition to explicit opt-in;
        // vsomeip client IDs are a demo guard, NOT cryptographic auth.
        if (request->get_client() != MULTIMEDIA_CLIENT_ID || !enabled_) {
            if (request->get_client() != MULTIMEDIA_CLIENT_ID)
                vsomeip_identity::denied("CONTROLLER", request->get_client(), MULTIMEDIA_CLIENT_ID);
            cmd.status = controller::Status::Denied;
        }
        else if (cmd.status != controller::Status::Pending ||
                 !controller::valid(cmd))
            cmd.status = controller::Status::Invalid;
        else {
            cmd.status = controller::Status::Pending;
            std::lock_guard<std::mutex> lock(mutex_);
            if (queued_.size() >= 16 ||
                (cmd.target == controller::Target::Multimedia &&
                 cmd.knob == controller::Knob::DemoCpu &&
                 (auto_policy_.pending() != controller_auto::Action::None ||
                  manual_cpu_serial_ != 0))) {
                cmd.status = controller::Status::Failed;
            } else {
                if (cmd.target == controller::Target::Multimedia &&
                    cmd.knob == controller::Knob::DemoCpu) {
                    auto_policy_.manual_submitted(Clock::now());
                    manual_cpu_serial_ = cmd.serial;
                }
                queued_.push_back(cmd); cv_.notify_all();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.last = cmd;
            if (cmd.status == controller::Status::Pending) pending_at_ = Clock::now();
        }
        auto reply = vsomeip::runtime::get()->create_response(request);
        auto bytes = vsomeip::runtime::get()->create_payload();
        bytes->set_data(controller::encode(cmd));
        reply->set_payload(bytes);
        app_->send(reply);
    }
    void receive_reply(const std::shared_ptr<vsomeip::message> &msg) {
        controller::Command cmd;
        auto p = msg->get_payload();
        if (!p || !controller::decode(p->get_data(), p->get_length(), cmd)) return;
        const auto target = static_cast<unsigned>(cmd.target);
        if (target == 0 || target >= state_.nodes.size()) return;
        const bool qm_cgroup = target == 2 && controller::index(cmd.knob) < 3;
        const vsomeip::service_t expected = qm_cgroup ? CONTROLLER_AGENT_SERVICE_ID : ids[target];
        if (msg->get_service() != expected) return;
        const unsigned key = controller::index(cmd.knob);
        std::lock_guard<std::mutex> lock(mutex_);
        if ((cmd.serial & 0xc0000000u) == 0x40000000u) { // GET serial
            if (pending_serial_[target][key] != cmd.serial) return;
            pending_serial_[target][key] = 0;
            state_.nodes[target].statuses[key] = cmd.status;
            state_.nodes[target].values[key] = cmd.status == controller::Status::Ok
                                                ? cmd.value : 0xffffffffu;
            if (target == 2 && key == controller::index(controller::Knob::DemoCpu) &&
                cmd.status == controller::Status::Ok)
                auto_policy_.observe(cmd.value);
            if (cmd.status != controller::Status::Ok)
                std::cerr << "CONTROLLER: GET " << controller::target_name(cmd.target)
                          << "/" << controller::knob_name(cmd.knob) << " => "
                          << controller::status_name(cmd.status) << '\n';
            return;
        }
        if (cmd.target == controller::Target::Multimedia &&
            cmd.knob == controller::Knob::DemoCpu &&
            pending_auto_serial_ == cmd.serial && pending_auto_serial_ != 0) {
            const auto kind = auto_policy_.pending();
            const auto prior = auto_policy_.current();
            const bool ok = cmd.status == controller::Status::Ok;
            auto_policy_.result(ok, cmd.value, Clock::now());
            pending_auto_serial_ = 0;
            pending_serial_[target][key] = 0; // discard a pre-SET GET
            state_.nodes[target].statuses[key] = ok ? controller::Status::Ok : cmd.status;
            state_.nodes[target].values[key] = ok ? cmd.value : 0xffffffffu;
            std::cerr << "CONTROLLER AUTO: " << controller_auto::action_name(kind)
                      << " " << prior << " -> " << cmd.value << " CPU ms "
                      << (ok ? "APPLIED" : controller::status_name(cmd.status))
                      << " (confirm next GET)\n";
            cv_.notify_all();
            return;
        }
        // Late or unrelated SET responses must not overwrite a newer value.
        if (cmd.serial == 0 || cmd.serial != state_.last.serial) return;
        if (cmd.target == controller::Target::Multimedia &&
            cmd.knob == controller::Knob::DemoCpu &&
            manual_cpu_serial_ == cmd.serial && manual_cpu_serial_ != 0) {
            auto_policy_.manual_result(cmd.status == controller::Status::Ok,
                                       cmd.value, Clock::now());
            manual_cpu_serial_ = 0;
            pending_serial_[target][key] = 0;
            std::cerr << "CONTROLLER AUTO: manual CPU budget " << cmd.value
                      << " ms => " << controller::status_name(cmd.status)
                      << "; baseline " << auto_policy_.baseline() << " ms\n";
        }
        if (cmd.status == controller::Status::Ok) {
            state_.nodes[target].values[key] = cmd.value;
            state_.nodes[target].statuses[key] = controller::Status::Ok;
        }
        if (cmd.serial != 0 && state_.last.serial == cmd.serial)
            state_.last = cmd;
    }
    void emit(const controller::Command &command) {
        vsomeip::service_t service = command.target == controller::Target::Multimedia &&
                (command.knob == controller::Knob::MemoryMax ||
                 command.knob == controller::Knob::CpuWeight ||
                 command.knob == controller::Knob::Affinity)
                    ? CONTROLLER_AGENT_SERVICE_ID
                    : ids[static_cast<unsigned>(command.target)];
        vsomeip::method_t method = service == CONTROLLER_AGENT_SERVICE_ID
                ? CONTROLLER_AGENT_APPLY_METHOD_ID : DEMO_TUNE_METHOD_ID;
        if (!service) return;
        auto req = vsomeip::runtime::get()->create_request();
        req->set_service(service);
        req->set_instance(1);
        req->set_method(method);
        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(controller::encode(command));
        req->set_payload(payload);
        app_->send(req);
    }
    void process(controller::Command cmd) {
        const auto key = controller::index(cmd.knob);
        if (cmd.target == controller::Target::Multimedia && key < 3) {
            bool agent;
            { std::lock_guard<std::mutex> lock(mutex_); agent = agent_online_; }
            if (agent) { emit(cmd); return; }
            cmd.status = controller::Status::Unavailable;
        } else if (key < 3) {
            // No host systemd bus is mounted in the ipc_t container.
            // The native host agent alone is permitted to read / mutate units.
            controller::Command result;
            cmd.status = controller_host_ipc::request(cmd, result)
                       ? result.status : controller::Status::Unavailable;
            if (cmd.status == controller::Status::Ok) {
                cmd.value = result.value; // report the actually applied value
                std::lock_guard<std::mutex> lock(mutex_);
                auto &node = state_.nodes[static_cast<unsigned>(cmd.target)];
                node.values[key] = cmd.value;
                node.statuses[key] = controller::Status::Ok;
            }
        } else {
            const auto index = static_cast<unsigned>(cmd.target);
            bool online;
            { std::lock_guard<std::mutex> lock(mutex_); online = state_.nodes[index].available != 0; }
            if (online) { emit(cmd); return; }
            cmd.status = controller::Status::Unavailable;
        }
        if (cmd.serial != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_auto_serial_ == cmd.serial) {
                auto_policy_.result(false, 0, Clock::now());
                pending_auto_serial_ = 0;
                auto &node = state_.nodes[static_cast<unsigned>(controller::Target::Multimedia)];
                node.statuses[controller::index(controller::Knob::DemoCpu)] = cmd.status;
                node.values[controller::index(controller::Knob::DemoCpu)] = 0xffffffffu;
            }
            if (manual_cpu_serial_ == cmd.serial) {
                auto_policy_.manual_result(false, 0, Clock::now());
                manual_cpu_serial_ = 0;
            }
            if (state_.last.serial == cmd.serial)
                state_.last = cmd;
        }
    }
    void poll_root_resources() {
        for (const auto t : {controller::Target::Qm, controller::Target::Engine,
                             controller::Target::Sensors}) {
            bool any_ok = false;
            for (unsigned key=1; key<=3; ++key) {
                controller::Command query;
                query.target = t;
                query.knob = static_cast<controller::Knob>(key);
                query.status = controller::Status::Unknown;
                controller::Command reply;
                const bool connected = controller_host_ipc::request(query, reply);
                const auto status = connected ? reply.status : controller::Status::Unavailable;
                auto &node = state_.nodes[static_cast<unsigned>(t)];
                std::lock_guard<std::mutex> lock(mutex_);
                node.values[key-1] = status == controller::Status::Ok ? reply.value : 0xffffffffu;
                node.statuses[key-1] = status;
                any_ok |= status == controller::Status::Ok;
            }
            if (t == controller::Target::Qm) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto &node = state_.nodes[0];
                node.available = any_ok;
                node.health = any_ok ? 1u : 4u;
                node.age_ms = any_ok ? 0u : 0xffffffffu;
            }
        }
    }
    void poll_remote_values() {
        // Real SOME/IP GET requests. Give each request a serial and deadline;
        // a missing or negative reply becomes visible instead of permanent --.
        for (unsigned target=1; target<4; ++target) {
            for (unsigned key=4; key<=7; ++key)
                poll_one(static_cast<controller::Target>(target),
                         static_cast<controller::Knob>(key));
        }
        for (unsigned key=1; key<=3; ++key)
            poll_one(controller::Target::Multimedia,
                     static_cast<controller::Knob>(key));
    }
    void poll_one(controller::Target target, controller::Knob knob) {
        const unsigned idx = static_cast<unsigned>(target), key = controller::index(knob);
        controller::Command cmd;
        cmd.target = target;
        cmd.knob = knob;
        cmd.status = controller::Status::Unknown;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool online = key < 3 ? agent_online_ : state_.nodes[idx].available != 0;
            if (!online) {
                state_.nodes[idx].values[key] = 0xffffffffu;
                state_.nodes[idx].statuses[key] = controller::Status::Unavailable;
                pending_serial_[idx][key] = 0;
                return;
            }
            if (pending_serial_[idx][key] != 0) return;
            // Do not issue a GET that can race with an in-flight CPU SET.
            if (idx == 2 && key == controller::index(controller::Knob::DemoCpu) &&
                (pending_auto_serial_ || manual_cpu_serial_)) return;
            // 0x40000000 disambiguates probe responses from writes (0x8...).
            cmd.serial = 0x40000000u | (++next_probe_serial_ & 0x3fffffffu);
            pending_serial_[idx][key] = cmd.serial;
            poll_pending_at_[idx][key] = Clock::now();
            state_.nodes[idx].statuses[key] = controller::Status::Pending;
        }
        emit(cmd);
    }
    void poll_systemd() {
        poll_root_resources();
        poll_remote_values();
    }
    void observe_systemd() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return registered_ || !running_; });
        while (running_) {
            lock.unlock();
            poll_systemd();
            lock.lock();
            cv_.wait_for(lock, std::chrono::seconds(5), [&] {
                return !running_;
            });
        }
    }
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return registered_ || !running_; });
        if (!running_) return;
        while (running_) {
            const auto now = Clock::now();
            if (!queued_.empty()) {
                const auto cmd = queued_.front(); queued_.pop_front();
                lock.unlock();
                process(cmd);
                lock.lock();
                continue;
            }
            if (state_.last.status == controller::Status::Pending &&
                now - pending_at_ > std::chrono::seconds(4))
                state_.last.status = controller::Status::Unavailable;
            for (unsigned i=1; i<4; ++i) {
                auto &node = state_.nodes[i];
                if (last_health_[i] == Clock::time_point{}) continue;
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_health_[i]).count();
                node.age_ms = static_cast<std::uint32_t>(std::min<std::int64_t>(age, 0xfffffffeLL));
                if (age > 4000 || !node.available) {
                    node.health = 4;
                    if (i == 2) auto_policy_.no_fresh_health();
                }
            }
            if (pending_auto_serial_ &&
                now - pending_auto_at_ >= std::chrono::seconds(4)) {
                std::cerr << "CONTROLLER AUTO: "
                          << controller_auto::action_name(auto_policy_.pending())
                          << " response TIMEOUT; suppressing further action until a fresh GET\n";
                pending_auto_serial_ = 0;
                auto_policy_.result(false, 0, now, true);
                auto &cpu = state_.nodes[2];
                cpu.values[controller::index(controller::Knob::DemoCpu)] = 0xffffffffu;
                cpu.statuses[controller::index(controller::Knob::DemoCpu)] = controller::Status::Unknown;
                pending_serial_[2][controller::index(controller::Knob::DemoCpu)] = 0;
            }
            if (manual_cpu_serial_ && state_.last.serial == manual_cpu_serial_ &&
                now - pending_at_ >= std::chrono::seconds(4)) {
                auto_policy_.manual_result(false, 0, now);
                manual_cpu_serial_ = 0;
            }
            for (unsigned t=1; t<4; ++t) for (unsigned k=0; k<7; ++k) {
                if (pending_serial_[t][k] &&
                    now - poll_pending_at_[t][k] >= std::chrono::seconds(4)) {
                    pending_serial_[t][k] = 0;
                    state_.nodes[t].values[k] = 0xffffffffu;
                    state_.nodes[t].statuses[k] = controller::Status::Timeout;
                    std::cerr << "CONTROLLER: GET "
                              << controller::target_name(static_cast<controller::Target>(t))
                              << "/" << controller::knob_name(static_cast<controller::Knob>(k+1))
                              << " => TIMEOUT\n";
                }
            }
            state_.automatic = auto_policy_.reductions();
            state_.auto_restores = auto_policy_.restores();
            state_.auto_baseline = auto_policy_.baseline();
            state_.auto_current = auto_policy_.current();
            state_.auto_pending = static_cast<std::uint32_t>(auto_policy_.pending());
            state_.auto_last_reason = static_cast<std::uint32_t>(auto_policy_.last_reason());
            state_.auto_last_from = auto_policy_.last_from();
            state_.auto_last_to = auto_policy_.last_to();
            state_.sequence++;
            auto data = controller::encode(state_);
            lock.unlock();
            auto payload = vsomeip::runtime::get()->create_payload();
            payload->set_data(data);
            app_->notify(CONTROLLER_SERVICE_ID, CONTROLLER_INSTANCE_ID,
                         CONTROLLER_STATUS_EVENT_ID, payload, true);
            lock.lock();
            cv_.wait_for(lock, std::chrono::seconds(1), [&] {
                return !running_ || !queued_.empty();
            });
        }
    }
    std::shared_ptr<vsomeip::application> app_;
    const bool enabled_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool registered_ = false, running_ = true, agent_online_ = false;
    controller::Snapshot state_;
    std::array<Clock::time_point,4> last_health_{};
    std::deque<controller::Command> queued_;
    Clock::time_point pending_at_{};
    controller_auto::Policy auto_policy_;
    std::uint32_t pending_auto_serial_ = 0;
    std::uint32_t manual_cpu_serial_ = 0;
    Clock::time_point pending_auto_at_{};
    unsigned auto_serial_ = 0;
    unsigned next_probe_serial_ = 0;
    std::array<std::array<std::uint32_t,7>,4> pending_serial_{};
    std::array<std::array<Clock::time_point,7>,4> poll_pending_at_{};
};
} // namespace
int main() {
    Controller controller;
    if (!controller.init()) {
        std::cerr << "CONTROLLER: unable to initialize SOME/IP\n";
        return 1;
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::thread signal_watcher([&] {
        while (!signal_stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        controller.stop();
    });
    controller.start();
    signal_stop.store(true);
    signal_watcher.join();
    return 0;
}
