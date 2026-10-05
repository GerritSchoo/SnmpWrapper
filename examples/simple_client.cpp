// simple_client – a small, self-explaining SNMP client built with snmpwrap.
//
// It talks to simple_agent (through snmpd) and shows the typical client operations:
// GET, walking a table, SET, a rejected SET and reading a non-existing instance.
//
//   simple_client [host[:port]] [community]                     SNMPv2c (default: 127.0.0.1:161 public / private)
//   simple_client [host[:port]] --v3 <user> <auth-passphrase>   SNMPv3 authNoPriv with SHA

#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "snmpwrap/client.hpp"

using namespace snmpwrap;

namespace {

const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999.100");   // the subtree of simple_agent
const Oid kDeviceName = kRoot + Oid{1, 0};
const Oid kUptime = kRoot + Oid{2, 0};
const Oid kThreshold = kRoot + Oid{3, 0};
const Oid kSensorTable = kRoot + Oid{4};

void section(const std::string& title) { std::cout << "\n=== " << title << " ===\n"; }

}  // namespace

int main(int argc, char** argv) {
    // 1) Describe the connection ---------------------------------------------------------------
    SessionConfig reader;                        // used for reading
    reader.peer = argc > 1 ? argv[1] : "127.0.0.1:161";
    reader.timeout = std::chrono::milliseconds(2000);
    SessionConfig writer = reader;               // used for writing (needs write access)

    if (argc > 4 && std::string(argv[2]) == "--v3") {
        reader.version = SessionConfig::Version::V3;
        reader.user = argv[3];
        reader.securityLevel = SessionConfig::SecurityLevel::AuthNoPriv;
        reader.authProtocol = SessionConfig::AuthProtocol::SHA1;
        reader.authPassphrase = argv[4];
        writer = reader;                         // the v3 user may read and write
    } else {
        reader.version = SessionConfig::Version::V2c;
        reader.community = argc > 2 ? argv[2] : "public";
        writer.version = SessionConfig::Version::V2c;
        writer.community = "private";
    }

    try {
        // 2) Open the sessions (RAII: closed automatically at the end of the scope) ----------
        Client client(reader);
        Client admin(writer);

        // 3) GET a few scalars in one request ---------------------------------------------------
        section("GET scalars");
        for (const VarBind& vb : client.get({kDeviceName, kUptime, kThreshold}))
            std::cout << vb.oid.str() << " = " << vb.value.str() << "\n";

        // 4) Walk the sensor table and print it row by row -----------------------------------------
        section("WALK sensorTable");
        // A walk returns the table column by column: <table>.1.<column>.<index>
        std::map<std::uint32_t, std::map<SubId, Value>> rows;          // index -> column -> value
        for (const VarBind& vb : client.walk(kSensorTable)) {
            const Oid cell = kSensorTable.suffixOf(vb.oid);           // 1.<column>.<index>
            rows[cell[2]][cell[1]] = vb.value;
        }
        std::cout << std::left << std::setw(7) << "index" << std::setw(10) << "name" << std::setw(8) << "value"
                  << "enabled\n";
        for (const auto& [index, cols] : rows) {
            std::cout << std::setw(7) << index << std::setw(10) << cols.at(2).asString() << std::setw(8)
                      << (std::to_string(cols.at(3).asInt()) + " C") << (cols.at(4).asInt() == 1 ? "yes" : "no")
                      << "\n";
        }

        // 5) SET values -------------------------------------------------------------------------
        section("SET alarmThreshold = 30 and disable sensor 2 (one atomic request)");
        admin.set({{kThreshold, Value::integer(30)},
                   {kSensorTable + Oid{1, 4, 2}, Value::integer(2)}});
        std::cout << "alarmThreshold is now " << client.get(kThreshold).value.asInt() << "\n";
        std::cout << "sensor 2 enabled:     " << client.get(kSensorTable + Oid{1, 4, 2}).value.asInt() << " (2 = false)\n";

        // 6) A SET the agent rejects ---------------------------------------------------------------
        section("SET alarmThreshold = 150 (out of range)");
        try {
            admin.set(kThreshold, Value::integer(150));
            std::cout << "unexpectedly accepted\n";
        } catch (const ResponseError& e) {
            std::cout << "rejected by the agent: " << e.what() << " (error-status " << static_cast<int>(e.status())
                      << ", varbind " << e.index() << ")\n";
        }
        std::cout << "alarmThreshold is still " << client.get(kThreshold).value.asInt() << "\n";

        section("SET deviceName to a number (wrong type)");
        try {
            admin.set(kDeviceName, Value::integer(5));
        } catch (const ResponseError& e) {
            std::cout << "rejected by the agent: " << e.what() << "\n";
        }

        // 7) Reading something that does not exist -----------------------------------------------
        section("GET sensorName.9 (no such row)");
        const VarBind missing = client.get(kSensorTable + Oid{1, 2, 9});
        if (missing.value.isException())
            std::cout << missing.oid.str() << ": " << toString(missing.value.type()) << "\n";

        // 8) Restore what we changed ---------------------------------------------------------------
        admin.set({{kThreshold, Value::integer(40)}, {kSensorTable + Oid{1, 4, 2}, Value::integer(1)}});
        section("done (changes restored)");
    } catch (const TransportError& e) {
        std::cerr << "no connection: " << e.what() << "\n"
                  << "Is snmpd running with 'master agentx', and is simple_agent connected?\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
