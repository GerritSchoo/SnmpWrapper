// client_app - an application that reads and changes the data of an SNMP agent (here: agent_app).
//
//   client_app [host[:port]] [community]        defaults: 127.0.0.1:161, private
//
// The whole recipe:
//   1. snmpwrap::Client session(config)       address, version, community / SNMPv3 user
//   2. my_app_mib::Remote remote(session)     nested like the MIB: remote.group.object.get() / .set(v)
//   3. catch TransportError (no answer), ResponseError (agent refused), SetError (MIB check, nothing sent)

#include <iostream>
#include <string>

#include "my_app_mib.hpp"  // generated from MY-APP-MIB by snmpwrap_add_mib()

namespace mib = my_app_mib;

int main(int argc, char** argv) {
    snmpwrap::SessionConfig cfg;
    cfg.peer = argc > 1 ? argv[1] : "127.0.0.1:161";
    cfg.community = argc > 2 ? argv[2] : "private";  // read-write community, needed for the SETs below
    cfg.timeout = std::chrono::milliseconds(2000);
    cfg.retries = 1;

    try {
        snmpwrap::Client session(cfg);
        mib::Remote remote(session);

        // GET
        std::cout << "appName  = " << remote.appName.get() << "\n";
        std::cout << "appLimit = " << remote.appSensors.appLimit.get() << " degrees Celsius\n";

        // a whole table, then one row
        for (const auto& [index, row] : remote.appSensors.appSensorTable.read())
            std::cout << "sensor " << index.appSensorIndex << ": " << row.appSensorName << " " << row.appSensorTemperature
                      << " C, " << mib::toString(row.appSensorMode) << "\n";
        std::cout << "sensor 1 is " << remote.appSensors.appSensorTable[1].appSensorName.get() << "\n";

        // SET (checked against the MIB first)
        remote.appSensors.appLimit.set(35);
        std::cout << "appLimit is now " << remote.appSensors.appLimit.get() << "\n";
        remote.appSensors.appSensorTable[3].appSensorMode.set(mib::AppSensorMode::on);
        std::cout << "sensor 3 is now " << mib::toString(remote.appSensors.appSensorTable[3].appSensorMode.get()) << "\n";
        remote.appSensors.appSensorTable[3].appSensorMode.set(mib::AppSensorMode::off);

        // the three kinds of errors
        try {
            remote.appSensors.appLimit.set(500);  // MIB: 0..100
        } catch (const snmpwrap::SetError& e) {
            std::cout << "rejected before sending: " << e.what() << "\n";
        }
        try {
            remote.appSensors.appLimit.set(5);  // fine for the MIB, but agent_app's own rule says >= 10
        } catch (const snmpwrap::ResponseError& e) {
            std::cout << "agent refused: " << e.what() << "\n";
        }
    } catch (const snmpwrap::TransportError& e) {
        std::cerr << "no answer from " << cfg.peer << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
