// client_app - an application that reads and changes the data of an SNMP agent (here: agent_app).
//
//   client_app [host[:port]] [community]        defaults: 127.0.0.1:161, private
//
// Structure of an application with a client:
//   1. SessionConfig (address, version, community / SNMPv3 user)  ->  snmpwrap::Client
//   2. the generated my_app_mib::Client on top of it: typed methods, MIB range checks before sending
//   3. catch the three error kinds: TransportError (no answer), ResponseError (agent refused), SetError (local MIB check)

#include <iostream>
#include <string>

#include "my_app_mib.hpp"  // generated from MY-APP-MIB by snmpwrap_add_mib()

int main(int argc, char** argv) {
    snmpwrap::SessionConfig cfg;
    cfg.peer = argc > 1 ? argv[1] : "127.0.0.1:161";
    cfg.community = argc > 2 ? argv[2] : "private";  // needs a read-write community for the SET below
    cfg.timeout = std::chrono::milliseconds(2000);
    cfg.retries = 1;

    try {
        snmpwrap::Client session(cfg);
        my_app_mib::Client app(session);  // typed access to every object of MY-APP-MIB

        // GET: one typed method per object
        std::cout << "appName        = " << app.appName() << "\n";
        std::cout << "appTemperature = " << app.appTemperature() << " degrees Celsius\n";
        std::cout << "appLimit       = " << app.appLimit() << " degrees Celsius\n";

        // SET: the MIB is checked locally first (appLimit is Integer32 (0..100))
        app.setAppLimit(35);
        std::cout << "appLimit is now " << app.appLimit() << "\n";
        try {
            app.setAppLimit(500);
        } catch (const snmpwrap::SetError& e) {
            std::cout << "rejected before sending: " << e.what() << "\n";
        }
        try {
            app.setAppLimit(5);  // fine for the MIB, but the agent's own rule says >= 10
        } catch (const snmpwrap::ResponseError& e) {
            std::cout << "agent refused: " << e.what() << " (status " << static_cast<int>(e.status()) << ")\n";
        }

        // plain OIDs and walks work on the same session
        std::cout << "\nwalk of " << my_app_mib::oids::root.str() << ":\n";
        for (const snmpwrap::VarBind& vb : session.walk(my_app_mib::oids::root))
            std::cout << "  " << vb.oid.str() << " = " << vb.value.str() << "\n";
    } catch (const snmpwrap::TransportError& e) {
        std::cerr << "no answer from " << cfg.peer << ": " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
