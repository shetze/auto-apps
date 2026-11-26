// AutoSD root-partition controller: read-only service-health monitoring,
// bounded opt-in tuning, and conservative QM-only demo load mitigation.
// NEVER accepts a filesystem path, service name or arbitrary systemctl args.
// IMPORTANT: SOME/IP app IDs are not an authentication mechanism. Tuning must
// only be enabled on an isolated, trusted demo network; this is not a safety
// controller or a substitute for certified FFI mechanisms.
#include "services.hpp"
#include "demo-health.hpp"
#include "controller-systemd.hpp"
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
const char *unit_name(controller::Target t) {
    switch (t) {
    case controller::Target::Qm: return "qm.service";
    case controller::Target::Engine: return "engine.service";
    case controller::Target::Multimedia: return "multimedia.service";
    case controller::Target::Sensors: return "sensors.service";
    }
    return "";
}
class Controller {
public:
    Controller()
      : app_(vsomeip::runtime::get()->create_application("Controller")),
        enabled_(std::getenv("AUTO_APPS_CONTROLLER_TUNING") &&
                 std::string(std::getenv("AUTO_APPS_CONTROLLER_TUNING")) == "1") {
        state_.tuning_enabled = enabled_ ? 1u : 0u;
        state_.last.status = controller::Status::Unknown;
    }
    bool init() {
        if (!app_ || !app_->init()) return false;
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
                        if (!available) state_.nodes[i].health = 4;
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
                    agent_online_ = value;
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
        // Never adjust the real engine's CPU quota automatically. Only QM
        // multimedia synthetic load is eligible for the conservative policy.
        if (index == 2) {
            const bool degraded = data.cpu == demo_health::Level::Failed ||
                                  data.cpu == demo_health::Level::Degraded;
            degraded_streak_ = degraded ? degraded_streak_ + 1u : 0u;
            const auto now = Clock::now();
            if (enabled_ && degraded_streak_ >= 5 &&
                now >= next_auto_ &&
                node.values[controller::index(controller::Knob::DemoCpu)] != 0xffffffffu &&
                node.values[controller::index(controller::Knob::DemoCpu)] >= 20) {
                controller::Command command;
                command.serial = 0x80000000u | (++auto_serial_ & 0x7fffffffu);
                command.target = controller::Target::Multimedia;
                command.knob = controller::Knob::DemoCpu;
                command.value = node.values[controller::index(command.knob)] / 2u;
                command.status = controller::Status::Pending;
                queued_.push_back(command);
                next_auto_ = now + std::chrono::seconds(60);
                degraded_streak_ = 0;
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
        if (request->get_client() != 0x1002u || !enabled_)
            cmd.status = controller::Status::Denied;
        else if (cmd.status != controller::Status::Pending ||
                 !controller::valid(cmd))
            cmd.status = controller::Status::Invalid;
        else {
            cmd.status = controller::Status::Pending;
            std::lock_guard<std::mutex> lock(mutex_);
            if (queued_.size() >= 16)
                cmd.status = controller::Status::Failed;
            else { queued_.push_back(cmd); cv_.notify_all(); }
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
        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<unsigned>(cmd.target);
        if (cmd.status == controller::Status::Ok && index < state_.nodes.size())
            state_.nodes[index].values[controller::index(cmd.knob)] = cmd.value;
        if (cmd.serial != 0) {
            // Ignore out-of-order completions of obsolete commands.
            if ((cmd.serial & 0x80000000u) != 0 && cmd.status == controller::Status::Ok)
                ++state_.automatic;
            if (state_.last.serial == cmd.serial)
                state_.last = cmd;
        }
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
        if (cmd.target == controller::Target::Multimedia &&
             (cmd.knob == controller::Knob::MemoryMax ||
              cmd.knob == controller::Knob::CpuWeight ||
              cmd.knob == controller::Knob::Affinity)) {
            bool agent;
            { std::lock_guard<std::mutex> lock(mutex_); agent = agent_online_; }
            if (agent) { emit(cmd); return; }
            cmd.status = controller::Status::Unavailable;
        } else if (cmd.knob == controller::Knob::MemoryMax ||
                   cmd.knob == controller::Knob::CpuWeight ||
                   cmd.knob == controller::Knob::Affinity) {
            cmd.status = controller_systemd::apply(unit_name(cmd.target), cmd,
                                                  cmd.target == controller::Target::Qm);
            if (cmd.status == controller::Status::Ok) {
                controller::Node fresh;
                if (controller_systemd::inspect(unit_name(cmd.target), fresh,
                            cmd.target == controller::Target::Qm)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto &node = state_.nodes[static_cast<unsigned>(cmd.target)];
                    for (unsigned i=0; i<3; ++i) node.values[i] = fresh.values[i];
                    if (cmd.target == controller::Target::Qm)
                        node.available = fresh.available;
                }
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
            if (state_.last.serial == cmd.serial)
                state_.last = cmd;
        }
    }
    void poll_systemd() {
        for (auto t : {controller::Target::Qm, controller::Target::Engine,
                       controller::Target::Sensors}) {
            controller::Node fresh;
            const bool success = controller_systemd::inspect(unit_name(t), fresh,
                                                          t == controller::Target::Qm);
            std::lock_guard<std::mutex> lock(mutex_);
            auto &node = state_.nodes[static_cast<unsigned>(t)];
            if (success) {
                for (unsigned k=0; k<3; ++k) node.values[k] = fresh.values[k];
                if (t == controller::Target::Qm) {
                    node.available = fresh.available;
                    node.health = fresh.available ? 1u : 4u;
                    node.age_ms = 0;
                }
            } else if (t == controller::Target::Qm) {
                node.available = 0;
                node.health = 4;
                node.age_ms = 0xffffffffu;
            }
        }
        for (unsigned target=1; target<4; ++target) {
            for (unsigned key=4; key<=7; ++key) {
                controller::Command cmd;
                cmd.target = static_cast<controller::Target>(target);
                cmd.knob = static_cast<controller::Knob>(key);
                cmd.status = controller::Status::Unknown;
                bool online;
                { std::lock_guard<std::mutex> lock(mutex_);
                  online = state_.nodes[target].available != 0; }
                if (online) emit(cmd);
            }
        }
        bool agent;
        { std::lock_guard<std::mutex> lock(mutex_); agent = agent_online_; }
        if (agent) for (unsigned key=1; key<=3; ++key) {
            controller::Command cmd;
            cmd.target = controller::Target::Multimedia;
            cmd.knob = static_cast<controller::Knob>(key);
            cmd.status = controller::Status::Unknown;
            emit(cmd);
        }
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
                if (age > 4000 || !node.available) node.health = 4;
            }
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
    Clock::time_point next_auto_{};
    unsigned auto_serial_ = 0;
    unsigned degraded_streak_ = 0;
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
