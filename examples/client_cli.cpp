// Small SNMP client built on snmpwrap::Client (v1, v2c and v3).
//
//   client_cli [options] <host[:port]> get <oid>...
//   client_cli [options] <host[:port]> getnext <oid>
//   client_cli [options] <host[:port]> getbulk <oid>
//   client_cli [options] <host[:port]> walk <oid>
//   client_cli [options] <host[:port]> set <oid> <type> <value> [<oid> <type> <value> ...]    (one atomic request)
//
// types: i Integer, u Gauge32, c Counter32, t TimeTicks, s OctetString, o ObjectId, a IpAddress,
//        = take the type from the MIB (needs -m), e.g.  set swtRowStatus.5 = createAndGo
//
// options:
//   -v 1|2c|3         protocol version (default 2c)
//   -c <community>    v1 / v2c community (default public)
//   -u <user>         v3 user name
//   -l noAuthNoPriv|authNoPriv|authPriv   v3 security level (default noAuthNoPriv)
//   -a MD5|SHA|SHA-224|SHA-256|SHA-384|SHA-512  v3 authentication protocol (default SHA)     -A <passphrase>
//   -x DES|AES|AES-192|AES-256  v3 privacy protocol (default AES)            -X <passphrase>
//   -n <context>      v3 context name
//   -t <ms>           timeout per try (default 3000)              -r <retries> (default 1)
//   -m <mib-file>     load a MIB (repeatable): names instead of OIDs, readable output, "=" values
//   -M <dir>          additional directory for imported MIB modules

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "snmpwrap/client.hpp"
#include "snmpwrap/mib_model.hpp"

using namespace snmpwrap;

namespace {

Value parseValue(const std::string& type, const std::string& text) {
    if (type == "i") return Value::integer(std::stoi(text));
    if (type == "u") return Value::gauge(static_cast<std::uint32_t>(std::stoul(text)));
    if (type == "c") return Value::counter32(static_cast<std::uint32_t>(std::stoul(text)));
    if (type == "t") return Value::timeTicks(static_cast<std::uint32_t>(std::stoul(text)));
    if (type == "s") return Value::string(text);
    if (type == "o") return Value::oid(Oid::parse(text));
    if (type == "a") {
        const Oid o = Oid::parse(text);
        if (o.size() != 4) throw Error("bad IpAddress: " + text);
        return Value::ipAddress(static_cast<std::uint8_t>(o[0]), static_cast<std::uint8_t>(o[1]),
                                static_cast<std::uint8_t>(o[2]), static_cast<std::uint8_t>(o[3]));
    }
    throw Error("unknown value type '" + type + "'");
}

std::optional<MibModel> g_model;  // set with -m

Oid toOid(const std::string& text) { return g_model ? g_model->resolve(text) : Oid::parse(text); }

void print(const VarBind& vb) {
    if (g_model)
        std::cout << g_model->format(vb) << "\n";
    else
        std::cout << vb.oid.str() << " = " << vb.value.str() << "\n";
}

int usage() {
    std::cerr << "usage: client_cli [-v 1|2c|3] [-c community] [-u user -l level -a MD5|SHA|SHA-256.. -A pass -x DES|AES|AES-256 -X pass -n ctx]\n"
                 "                  [-t ms] [-r retries] [-m mib-file]... [-M mib-dir]...\n"
                 "                  <host[:port]> get|getnext|getbulk|walk|set <oid|name> [type|= value ...]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    SessionConfig cfg;
    std::vector<std::string> args, mibFiles, mibDirs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "-v") {
            const std::string v = next();
            if (v == "1") cfg.version = SessionConfig::Version::V1;
            else if (v == "2c") cfg.version = SessionConfig::Version::V2c;
            else if (v == "3") cfg.version = SessionConfig::Version::V3;
            else return usage();
        } else if (a == "-c") cfg.community = next();
        else if (a == "-u") cfg.user = next();
        else if (a == "-l") {
            const std::string l = next();
            if (l == "noAuthNoPriv") cfg.securityLevel = SessionConfig::SecurityLevel::NoAuthNoPriv;
            else if (l == "authNoPriv") cfg.securityLevel = SessionConfig::SecurityLevel::AuthNoPriv;
            else if (l == "authPriv") cfg.securityLevel = SessionConfig::SecurityLevel::AuthPriv;
            else return usage();
        } else if (a == "-a") {
            const std::string p = next();
            if (p == "MD5") cfg.authProtocol = SessionConfig::AuthProtocol::MD5;
            else if (p == "SHA") cfg.authProtocol = SessionConfig::AuthProtocol::SHA1;
            else if (p == "SHA-224") cfg.authProtocol = SessionConfig::AuthProtocol::SHA224;
            else if (p == "SHA-256") cfg.authProtocol = SessionConfig::AuthProtocol::SHA256;
            else if (p == "SHA-384") cfg.authProtocol = SessionConfig::AuthProtocol::SHA384;
            else if (p == "SHA-512") cfg.authProtocol = SessionConfig::AuthProtocol::SHA512;
            else return usage();
        } else if (a == "-A") cfg.authPassphrase = next();
        else if (a == "-x") {
            const std::string p = next();
            if (p == "DES") cfg.privProtocol = SessionConfig::PrivProtocol::DES;
            else if (p == "AES" || p == "AES-128") cfg.privProtocol = SessionConfig::PrivProtocol::AES128;
            else if (p == "AES-192") cfg.privProtocol = SessionConfig::PrivProtocol::AES192;
            else if (p == "AES-256") cfg.privProtocol = SessionConfig::PrivProtocol::AES256;
            else return usage();
        } else if (a == "-X") cfg.privPassphrase = next();
        else if (a == "-n") cfg.contextName = next();
        else if (a == "-t") cfg.timeout = std::chrono::milliseconds(std::stol(next()));
        else if (a == "-r") cfg.retries = std::stoi(next());
        else if (a == "-m") mibFiles.push_back(next());
        else if (a == "-M") mibDirs.push_back(next());
        else args.push_back(a);
    }
    if (args.size() < 3) return usage();

    try {
        if (!mibFiles.empty()) g_model = MibModel::load(mibFiles, mibDirs);
        cfg.peer = args[0];
        Client client(cfg);
        const std::string& cmd = args[1];

        if (cmd == "get") {
            std::vector<Oid> oids;
            for (std::size_t i = 2; i < args.size(); ++i) oids.push_back(toOid(args[i]));
            for (const auto& vb : client.get(oids)) print(vb);
        } else if (cmd == "getnext") {
            print(client.getNext(toOid(args[2])));
        } else if (cmd == "getbulk") {
            for (const auto& vb : client.getBulk({toOid(args[2])}, 0, 5)) print(vb);
        } else if (cmd == "walk") {
            client.walk(toOid(args[2]), [&](const VarBind& vb) { print(vb); return true; });
        } else if (cmd == "set" && args.size() >= 5 && (args.size() - 2) % 3 == 0) {
            std::vector<VarBind> sets;
            for (std::size_t i = 2; i + 2 < args.size(); i += 3) {
                if (args[i + 1] == "=") {  // type and MIB check from the MIB
                    if (!g_model) throw Error("value type '=' needs a MIB (-m)");
                    sets.push_back({toOid(args[i]), g_model->parseValue(args[i], args[i + 2])});
                } else {
                    sets.push_back({toOid(args[i]), parseValue(args[i + 1], args[i + 2])});
                }
            }
            client.set(sets);
            std::cout << "OK\n";
        } else {
            return usage();
        }
    } catch (const ResponseError& e) {
        std::cerr << "agent error " << static_cast<int>(e.status()) << " (varbind " << e.index() << "): " << e.what() << "\n";
        return 1;
    } catch (const TransportError& e) {
        std::cerr << "transport error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
