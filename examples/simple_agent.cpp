// simple_agent – a small, self-explaining SNMP agent built with snmpwrap.
//
// It simulates a device with a few temperature sensors and publishes this data below
// 1.3.6.1.4.1.99999.100 ("demo" subtree, 99999 is a placeholder enterprise number):
//
//   .1.0          deviceName       OCTET STRING  read-write (max. 32 characters)
//   .2.0          deviceUptime     TimeTicks     read-only
//   .3.0          alarmThreshold   Integer       read-write (0..100 °C)
//   .4            sensorTable      INDEX { sensorIndex }
//     .4.1.2.<i>    sensorName     OCTET STRING  read-only
//     .4.1.3.<i>    sensorValue    Integer       read-only  (°C, changes every second)
//     .4.1.4.<i>    sensorEnabled  Integer       read-write (1 = true, 2 = false)
//   .0.1          sensorAlarm      notification  sent when an enabled sensor exceeds alarmThreshold
//
// Needs a running snmpd with "master agentx" (see examples/README.md).
//
//   simple_agent [agentx-socket]          default: tcp:127.0.0.1:705

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <string>

#include "snmpwrap/agent.hpp"

using namespace snmpwrap;

namespace {

// ---------------------------------------------------------------------------------------------
// The application data. In a real program this is your existing data model.
// All snmpwrap callbacks run in the thread that calls agent.poll(), which is the only thread
// in this program, so no mutex is needed here.
// ---------------------------------------------------------------------------------------------
struct Sensor {
    std::string name;
    int value;     // °C
    bool enabled;
};

struct Device {
    std::string name = "demo-device";
    int alarmThreshold = 40;
    std::map<std::uint32_t, Sensor> sensors{
        {1, {"cpu", 35, true}},
        {2, {"board", 30, true}},
        {3, {"psu", 28, false}},
    };
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

std::atomic<bool> g_stop{false};  // set by the signal handler, checked by the main loop

void onSignal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
    Device device;

    AgentConfig config;
    config.name = "simple-agent";
    if (argc > 1) config.agentxSocket = argv[1];

    try {
        // 1) Create the agent (connects to snmpd later, in start()/poll()).
        Agent agent(config);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // 2) Register our subtree. All OIDs below are relative to this root.
        const Oid root = Oid::parse("1.3.6.1.4.1.99999.100");
        Mib& mib = agent.addMib(root);

        // 3) Scalars ---------------------------------------------------------------------------

        // deviceName: read-write string with a length check
        mib.scalar(1, {
            Type::OctetString,
            [&] { return Value::string(device.name); },                    // GET
            [&](const Value& v) { device.name = v.asString(); },           // SET
            [](const Value& v) {                                           // validation before SET
                if (v.asString().empty() || v.asString().size() > 32)
                    throw SetError(ErrorStatus::WrongLength, "deviceName must have 1..32 characters");
            }});

        // deviceUptime: read-only, computed on every request (TimeTicks = 1/100 s)
        mib.scalar(2, {
            Type::TimeTicks,
            [&] {
                auto age = std::chrono::steady_clock::now() - device.started;
                auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(age).count() / 10;
                return Value::timeTicks(static_cast<std::uint32_t>(ticks));
            }});

        // alarmThreshold: read-write integer with a range check
        mib.scalar(3, {
            Type::Integer,
            [&] { return Value::integer(device.alarmThreshold); },
            [&](const Value& v) { device.alarmThreshold = v.asInt(); },
            [](const Value& v) {
                if (v.asInt() < 0 || v.asInt() > 100)
                    throw SetError(ErrorStatus::WrongValue, "alarmThreshold must be 0..100");
            }});

        // 4) Table -----------------------------------------------------------------------------
        TableDef sensors;
        sensors.indexes = {IndexSpec::integer()};                     // INDEX { sensorIndex }
        sensors.columns = {
            {2, Type::OctetString, Access::ReadOnly},                  // sensorName
            {3, Type::Integer, Access::ReadOnly},                      // sensorValue
            {4, Type::Integer, Access::ReadWrite},                     // sensorEnabled (TruthValue)
        };
        sensors.rows = [&] {                                           // which rows exist?
            std::vector<Oid> rows;
            for (const auto& [index, sensor] : device.sensors) rows.push_back(indexInt(index));
            return rows;
        };
        sensors.get = [&](const Oid& index, SubId column) {           // value of one cell
            const Sensor& s = device.sensors.at(index[0]);
            switch (column) {
                case 2: return Value::string(s.name);
                case 3: return Value::integer(s.value);
                default: return Value::integer(s.enabled ? 1 : 2);
            }
        };
        sensors.validate = [](const Oid&, SubId, const Value& v) {    // only sensorEnabled is writable
            if (v.asInt() != 1 && v.asInt() != 2)
                throw SetError(ErrorStatus::WrongValue, "sensorEnabled is true(1) or false(2)");
        };
        sensors.set = [&](const Oid& index, SubId, const Value& v) {
            device.sensors.at(index[0]).enabled = (v.asInt() == 1);
            std::cout << "sensor " << index[0] << (v.asInt() == 1 ? " enabled" : " disabled") << " by SNMP" << std::endl;
        };
        mib.table(4, std::move(sensors));

        // 5) Main loop: serve SNMP requests and simulate the sensors ---------------------------
        std::cout << "simple_agent: serving " << root.str() << " via " << config.agentxSocket
                  << " (Ctrl+C to stop)" << std::endl;

        auto nextUpdate = std::chrono::steady_clock::now();
        int tick = 0;
        while (!g_stop && agent.poll()) {                              // handles requests, returns at least once per second
            if (std::chrono::steady_clock::now() < nextUpdate) continue;
            nextUpdate += std::chrono::seconds(1);
            ++tick;

            // simulate: values move up and down a little
            for (auto& [index, s] : device.sensors) {
                int delta = ((tick + static_cast<int>(index)) % 4 < 2) ? 1 : -1;
                s.value += delta;
            }

            // send a notification for every enabled sensor above the threshold (at most every 10 s)
            if (tick % 10 == 0) {
                for (const auto& [index, s] : device.sensors) {
                    if (!s.enabled || s.value <= device.alarmThreshold) continue;
                    agent.sendTrap(root + Oid{0, 1},                                       // sensorAlarm
                                   {{root + Oid{4, 1, 2, index}, Value::string(s.name)},   // sensorName.<i>
                                    {root + Oid{4, 1, 3, index}, Value::integer(s.value)}});
                    std::cout << "alarm sent: " << s.name << " = " << s.value << " C" << std::endl;
                }
            }
        }
        std::cout << "simple_agent: stopped" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "simple_agent: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
