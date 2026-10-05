// mib_agent – the demo device of simple_agent.cpp, but built FROM THE MIB:
// mibs/SNMPWRAPPER-DEMO-MIB.txt is the interface description, CMake runs snmpwrap-mibgen on it
// (snmpwrap_add_mib in examples/CMakeLists.txt) and this program only implements the generated
// interface snmpwrapper_demo_mib::Instrumentation. No OIDs, types or ranges appear here; if the MIB
// changes, the compiler points at every place that has to follow.
//
//   mib_agent [agentx-socket]          default: tcp:127.0.0.1:705

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <string>

#include "snmpwrapper_demo_mib.hpp"  // generated from SNMPWRAPPER-DEMO-MIB

using namespace snmpwrapper_demo_mib;

namespace {

std::atomic<bool> g_stop{false};  // set by the signal handler, checked by the main loop
void onSignal(int) { g_stop = true; }

/// The application: a device with three temperature sensors.
class DemoDevice : public Instrumentation {
public:
    // --- scalars (one method per MIB object; setters only for read-write objects) ---------------
    std::string deviceName() override { return name_; }
    void setDeviceName(const std::string& v) override { name_ = v; }  // SIZE (1..32) already checked
    std::uint32_t deviceUptime() override {
        auto age = std::chrono::steady_clock::now() - started_;
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count() / 10);
    }
    std::int32_t alarmThreshold() override { return threshold_; }
    void setAlarmThreshold(std::int32_t v) override { threshold_ = v; }  // range 0..100 already checked

    // --- sensorTable (typed index SensorEntryIndex, TruthValue enum from SNMPv2-TC) --------------
    std::vector<SensorEntryIndex> sensorTableRows() override {
        std::vector<SensorEntryIndex> rows;
        for (const auto& [index, s] : sensors_) rows.push_back({index});
        return rows;
    }
    std::string sensorName(const SensorEntryIndex& i) override { return sensors_.at(i.sensorIndex).name; }
    std::int32_t sensorValue(const SensorEntryIndex& i) override { return sensors_.at(i.sensorIndex).value; }
    TruthValue sensorEnabled(const SensorEntryIndex& i) override {
        return sensors_.at(i.sensorIndex).enabled ? TruthValue::true_ : TruthValue::false_;
    }
    void setSensorEnabled(const SensorEntryIndex& i, TruthValue v) override {
        sensors_.at(i.sensorIndex).enabled = (v == TruthValue::true_);
        std::cout << "sensor " << i.sensorIndex << " " << toString(v) << " by SNMP" << std::endl;
    }

    /// Simulation step (once per second); sends sensorAlarm for warm, enabled sensors every 10 s.
    void tick(snmpwrap::Agent& agent) {
        ++ticks_;
        for (auto& [index, s] : sensors_) s.value += ((ticks_ + static_cast<int>(index)) % 4 < 2) ? 1 : -1;
        if (ticks_ % 10 != 0) return;
        for (const auto& [index, s] : sensors_) {
            if (!s.enabled || s.value <= threshold_) continue;
            sendSensorAlarm(agent, s.name, s.value, SensorEntryIndex{index});  // generated, typed
            std::cout << "alarm sent: " << s.name << " = " << s.value << " C" << std::endl;
        }
    }

private:
    struct Sensor {
        std::string name;
        std::int32_t value;
        bool enabled;
    };
    std::string name_ = "demo-device";
    std::int32_t threshold_ = 40;
    std::map<std::int32_t, Sensor> sensors_{{1, {"cpu", 35, true}}, {2, {"board", 30, true}}, {3, {"psu", 28, false}}};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    int ticks_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    snmpwrap::AgentConfig config;
    config.name = "mib-agent";
    if (argc > 1) config.agentxSocket = argv[1];

    try {
        snmpwrap::Agent agent(config);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        DemoDevice device;
        registerMib(agent, device);  // generated: registers every object of SNMPWRAPPER-DEMO-MIB

        std::cout << "mib_agent: serving SNMPWRAPPER-DEMO-MIB (" << oids::root.str() << ") via " << config.agentxSocket
                  << " (Ctrl+C to stop)" << std::endl;
        auto next = std::chrono::steady_clock::now();
        while (!g_stop && agent.poll()) {
            if (std::chrono::steady_clock::now() < next) continue;
            next += std::chrono::seconds(1);
            device.tick(agent);
        }
        std::cout << "mib_agent: stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "mib_agent: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
