// agent_app - an application that publishes its data via SNMP.
//
//   agent_app [agentx-socket]        default: tcp:127.0.0.1:705  (the "master agentx" address of snmpd)
//
// Structure of an application with an agent:
//   1. your data + a mutex (worker threads and SNMP callbacks both touch it)
//   2. a class implementing the generated my_app_mib::Instrumentation (one method per MIB object)
//   3. Agent -> registerMib -> loop with poll(); send traps from this loop

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "my_app_mib.hpp"  // generated from MY-APP-MIB by snmpwrap_add_mib()

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};  // set by the signal handler, checked by the main loop (no pointer to the Agent needed)
void onSignal(int) { g_stop = true; }

/// Your application. The SNMP callbacks below are called by the Agent from the thread that runs poll().
class MyApp : public my_app_mib::Instrumentation {
public:
    // --- read: called for every GET / GETNEXT / walk ---------------------------------------------
    std::string appName() override { std::lock_guard<std::mutex> l(m_); return name_; }
    std::int32_t appTemperature() override { std::lock_guard<std::mutex> l(m_); return temperature_; }
    std::int32_t appLimit() override { std::lock_guard<std::mutex> l(m_); return limit_; }

    // --- write: called on SNMP SET, after the MIB checks (SIZE, range) passed -> your "value changed" event
    void setAppName(const std::string& v) override {
        std::lock_guard<std::mutex> l(m_);
        name_ = v;
        std::cout << "appName was set to \"" << name_ << "\" by a manager" << std::endl;
    }
    void setAppLimit(std::int32_t v) override {
        std::lock_guard<std::mutex> l(m_);
        limit_ = v;
        std::cout << "appLimit was set to " << limit_ << " by a manager" << std::endl;
    }
    // optional extra rule on top of the MIB's range: throw snmpwrap::SetError to reject
    void validateAppLimit(std::int32_t v) override {
        if (v < 10) throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10 is not allowed");
    }

    // --- called by your own code (any thread) --------------------------------------------------------
    void measure() {  // pretend a sensor delivers a new value
        std::lock_guard<std::mutex> l(m_);
        temperature_ = 20 + (++ticks_ % 15);
        if (temperature_ > limit_) alarmPending_ = true;
    }
    /// True once per alarm; the SNMP thread uses it to send the trap.
    bool takeAlarm(std::int32_t& temperature) {
        std::lock_guard<std::mutex> l(m_);
        temperature = temperature_;
        return std::exchange(alarmPending_, false);
    }

private:
    std::mutex m_;
    std::string name_ = "my-app";
    std::int32_t temperature_ = 20;
    std::int32_t limit_ = 30;
    int ticks_ = 0;
    bool alarmPending_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    snmpwrap::AgentConfig config;
    config.name = "agent_app";
    if (argc > 1) config.agentxSocket = argv[1];

    try {
        snmpwrap::Agent agent(config);  // only one Agent per process
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        MyApp app;
        my_app_mib::registerMib(agent, app);  // registers every object of MY-APP-MIB with the master agent

        // your own thread: updates the data, never calls snmpwrap
        std::atomic<bool> running{true};
        std::thread worker([&] {
            while (running) {
                std::this_thread::sleep_for(1s);
                app.measure();
            }
        });

        std::cout << "agent_app: serving " << my_app_mib::oids::root.str() << " via " << config.agentxSocket
                  << " (Ctrl+C to stop)" << std::endl;

        while (!g_stop && agent.poll()) {  // handles SNMP requests; returns at least once per second
            std::int32_t temperature = 0;
            if (app.takeAlarm(temperature)) {
                my_app_mib::sendAppLimitExceeded(agent, temperature);  // generated, typed notification
                std::cout << "trap sent: temperature " << temperature << std::endl;
            }
        }

        running = false;
        worker.join();
        std::cout << "agent_app: stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "agent_app: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
