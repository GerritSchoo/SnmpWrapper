// agent_app - an application that publishes its data via SNMP.
//
//   agent_app [agentx-socket]        default: tcp:127.0.0.1:705  (the "master agentx" address of snmpd)
//
// The whole recipe:
//   1. fill the generated my_app_mib::Data - plain C++, nested like the MIB
//   2. my_app_mib::DataAgent serves it; onSet tells you what a manager changed
//   3. run agent.poll() in a loop; other threads touch the data only under adapter.lock()

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "my_app_mib.hpp"  // generated from MY-APP-MIB by snmpwrap_add_mib()

namespace mib = my_app_mib;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_stop{false};  // set by Ctrl+C / SIGTERM, checked by the main loop
void onSignal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    snmpwrap::AgentConfig config;
    config.name = "agent_app";
    if (argc > 1) config.agentxSocket = argv[1];

    try {
        snmpwrap::Agent agent(config);  // only one Agent per process
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // --- 1. the data of the application, nested like the MIB -----------------------------------
        mib::Data data;
        data.appName = "my-app";
        data.appSensors.appLimit = 30;
        data.appSensors.appSensorTable[1] = {"cpu", 20, mib::AppSensorMode::on};
        data.appSensors.appSensorTable[2] = {"board", 20, mib::AppSensorMode::on};
        data.appSensors.appSensorTable[3] = {"psu", 20, mib::AppSensorMode::off};
        for (const std::string& problem : data.validate()) std::cerr << "invalid value: " << problem << "\n";

        // --- 2. serve it -----------------------------------------------------------------------------
        mib::DataAgent adapter(agent, data);
        adapter.onSet([&](mib::Object object, const snmpwrap::Oid& index) {  // a manager changed a value
            switch (object) {
                case mib::Object::appLimit:                                   // a setting (manager -> agent)
                    if (data.appSensors.appLimit < 10)                        // an extra rule on top of the MIB's 0..100
                        throw snmpwrap::SetError(snmpwrap::ErrorStatus::WrongValue, "limit below 10 is not allowed");
                    break;
                case mib::Object::appSensorMode:                              // a command for one row
                    std::cout << "sensor " << mib::AppSensorEntryIndex::fromOid(index)->appSensorIndex << " switched "
                              << mib::toString(data.appSensors.appSensorTable.at(*mib::AppSensorEntryIndex::fromOid(index)).appSensorMode)
                              << std::endl;
                    break;
                default:
                    break;
            }
            std::cout << mib::toString(object) << (index.empty() ? "" : "." + index.str()) << " was changed by a manager" << std::endl;
        });

        // --- your own thread: new measurements; it touches `data` only under the lock ---------------------
        std::atomic<bool> running{true};
        std::vector<mib::AppSensorEntryIndex> alarms;  // sensors that just became too warm (guarded by the lock)
        std::thread worker([&] {
            int tick = 0;
            std::map<std::int32_t, bool> hot;
            while (running) {
                std::this_thread::sleep_for(1s);
                auto guard = adapter.lock();
                ++tick;
                for (auto& [index, row] : data.appSensors.appSensorTable) {
                    row.appSensorTemperature = 20 + (tick + index.appSensorIndex) % 15;
                    const bool tooWarm = row.appSensorMode == mib::AppSensorMode::on && row.appSensorTemperature > data.appSensors.appLimit;
                    if (tooWarm && !hot[index.appSensorIndex]) alarms.push_back(index);
                    hot[index.appSensorIndex] = tooWarm;
                }
            }
        });

        std::cout << "agent_app: serving " << mib::oids::root.str() << " via " << config.agentxSocket << " (Ctrl+C to stop)" << std::endl;

        // --- 3. the agent loop: answers requests, returns at least once per second ----------------------
        while (!g_stop && agent.poll()) {
            std::vector<std::pair<mib::AppSensorEntryIndex, mib::AppSensorEntry>> toSend;
            {
                auto guard = adapter.lock();
                for (const auto& index : alarms) toSend.emplace_back(index, data.appSensors.appSensorTable.at(index));
                alarms.clear();
            }
            for (const auto& [index, row] : toSend) {
                mib::sendAppLimitExceeded(agent, row.appSensorName, row.appSensorTemperature, index);  // notification
                std::cout << "trap sent: " << row.appSensorName << " = " << row.appSensorTemperature << " C" << std::endl;
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
