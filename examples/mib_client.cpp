// mib_client – a client that uses the MIB in two ways:
//   1. the generated typed client (snmpwrapper_demo_mib::Client): methods with C++ types, MIB checks
//      before sending, tables as std::map<Index, Row>;
//   2. the MIB loaded at run time (snmpwrap::MibModel): names instead of OIDs and readable output
//      for a generic walk (enum labels, UNITS, TimeTicks).
// Plain OIDs keep working everywhere (the last section shows the same data with snmpwrap::Client only).
//
//   mib_client [host[:port]] [mib-file]       defaults: 127.0.0.1:161, mibs/SNMPWRAPPER-DEMO-MIB.txt

#include <iomanip>
#include <iostream>
#include <string>

#include "snmpwrap/mib_model.hpp"
#include "snmpwrapper_demo_mib.hpp"  // generated from SNMPWRAPPER-DEMO-MIB

namespace demo = snmpwrapper_demo_mib;

namespace {
void section(const std::string& title) { std::cout << "\n=== " << title << " ===\n"; }
}  // namespace

int main(int argc, char** argv) {
    const std::string peer = argc > 1 ? argv[1] : "127.0.0.1:161";
    const std::string mibFile = argc > 2 ? argv[2] : SNMPWRAP_SOURCE_DIR "/mibs/SNMPWRAPPER-DEMO-MIB.txt";

    try {
        // the MIB at run time (only needed for part 2); load it before the first Client
        const snmpwrap::MibModel model = snmpwrap::MibModel::load({mibFile});

        snmpwrap::SessionConfig cfg;
        cfg.peer = peer;
        cfg.community = "private";
        snmpwrap::Client session(cfg);
        demo::Client device(session);  // generated, typed

        // --- 1. generated typed client --------------------------------------------------------
        section("typed access (generated client)");
        std::cout << "deviceName     = " << device.deviceName() << "\n";
        std::cout << "alarmThreshold = " << device.alarmThreshold() << " degrees Celsius\n";

        std::cout << "\n" << std::left << std::setw(7) << "index" << std::setw(8) << "name" << std::setw(8) << "value"
                  << "enabled\n";
        for (const auto& [index, row] : device.sensorTable())  // std::map<SensorEntryIndex, SensorEntry>
            std::cout << std::setw(7) << index.sensorIndex << std::setw(8) << row.sensorName << std::setw(8)
                      << (std::to_string(row.sensorValue) + " C") << demo::toString(row.sensorEnabled) << "\n";

        section("typed SET");
        device.setAlarmThreshold(30);
        std::cout << "alarmThreshold is now " << device.alarmThreshold() << "\n";
        try {
            device.setAlarmThreshold(150);  // MIB: Integer32 (0..100) -> rejected before anything is sent
        } catch (const snmpwrap::SetError& e) {
            std::cout << "rejected locally by the MIB check: " << e.what() << "\n";
        }
        device.setSensorEnabled({3}, demo::TruthValue::true_);
        std::cout << "sensor 3 enabled: " << demo::toString(device.sensorTable().at({3}).sensorEnabled) << "\n";
        device.setSensorEnabled({3}, demo::TruthValue::false_);
        device.setAlarmThreshold(40);

        // --- 2. MIB at run time: names and readable output -------------------------------------------
        section("generic walk, formatted with the MIB");
        session.walk(model.oid("snmpWrapperDemoMIB"), [&](const snmpwrap::VarBind& vb) {
            std::cout << model.format(vb) << "\n";  // e.g. "sensorEnabled.1 = true(1)", "sensorValue.1 = 35 degrees Celsius"
            return true;
        });

        section("names instead of OIDs");
        const snmpwrap::Oid oid = model.resolve("sensorName.2");
        std::cout << "sensorName.2 -> " << oid.str() << " = " << model.formatValue(model.nodeFor(oid), session.get(oid).value) << "\n";
        const snmpwrap::Value v = model.parseValue("alarmThreshold", "35");  // type + range from the MIB
        session.set(model.resolve("alarmThreshold.0"), v);
        std::cout << "set alarmThreshold.0 = 35 -> " << model.format(session.get(model.resolve("alarmThreshold.0"))) << "\n";
        session.set(model.resolve("alarmThreshold.0"), model.parseValue("alarmThreshold", "40"));

        // --- plain OIDs still work ----------------------------------------------------------------------
        section("the same with plain OIDs");
        std::cout << session.get(snmpwrap::Oid::parse("1.3.6.1.4.1.99999.100.1.0")).value.str() << "\n";
    } catch (const snmpwrap::TransportError& e) {
        std::cerr << "no connection: " << e.what() << "\nIs snmpd running with 'master agentx' and mib_agent connected?\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
