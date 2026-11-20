#include "snmpwrap/Client.hpp"

#include <cstdlib>
#include <memory>
#include <string>

#include "ClientDetail.hpp"
#include "Convert.hpp"

namespace snmpwrap {

namespace {

struct PduDeleter {
    void operator()(netsnmp_pdu* p) const { snmp_free_pdu(p); }
};
using PduPtr = std::unique_ptr<netsnmp_pdu, PduDeleter>;

/// Net-SNMP hands out error texts as malloc'ed memory that the caller must free.
std::string takeErrorText(char* msg) {
    std::unique_ptr<char, void (*)(void*)> owner(msg, std::free);
    return msg ? std::string(msg) : std::string("unknown error");
}

}  // namespace

struct Client::Impl {
    SessionConfig config;
    void* handle = nullptr;  // from snmp_sess_open()

    ~Impl() {
        if (handle) snmp_sess_close(handle);
    }

    std::vector<VarBind> request(int pduType, const std::vector<VarBind>& in, long nonRepeaters = 0,
                                 long maxRepetitions = 0) {
        netsnmp_pdu* pdu = snmp_pdu_create(pduType);
        if (!pdu) throw Error("snmp_pdu_create failed");
        PduPtr guard(pdu);  // released below once ownership passes to snmp_sess_synch_response
        if (pduType == SNMP_MSG_GETBULK) {
            pdu->non_repeaters = nonRepeaters;
            pdu->max_repetitions = maxRepetitions;
        }
        for (const auto& vb : in) {
            auto name = detail::toNetOid(vb.oid);
            netsnmp_variable_list* var = snmp_pdu_add_variable(pdu, name.data(), name.size(), ASN_NULL, nullptr, 0);
            if (!var) throw Error("snmp_pdu_add_variable failed");
            if (pduType == SNMP_MSG_SET) detail::assign(var, vb.value);
        }

        netsnmp_pdu* response = nullptr;
        guard.release();  // snmp_sess_synch_response consumes the request PDU in every case
        const int status = snmp_sess_synch_response(handle, pdu, &response);
        PduPtr resp(response);

        if (status == STAT_TIMEOUT) {
            const bool v3 = config.version == SessionConfig::Version::V3;
            throw TransportError("SNMP request to " + config.peer + " timed out (" + std::to_string(config.timeout.count()) +
                                 " ms, " + std::to_string(config.retries + 1) + " tries) - is the agent running and reachable "
                                 "(address, port, firewall)? " +
                                 (v3 ? std::string("SNMPv3: is the user known to the agent?")
                                     : std::string("v1 / v2c: agents silently drop a wrong community (\"") + config.community +
                                           "\" - for SET a read-write community is needed)"));
        }
        if (status != STAT_SUCCESS) {
            char* msg = nullptr;
            snmp_sess_error(handle, nullptr, nullptr, &msg);
            const std::string text = takeErrorText(msg);
            throw TransportError("SNMP request to " + config.peer + " failed: " + text);
        }
        return detail::ResponseReader(pduType, in.size(), config.peer).read(*resp);
    }
};

Client::Client(const SessionConfig& cfg) : impl_(std::make_unique<Impl>()) {
    const detail::SessionSetup setup(cfg);  // checks the settings; the session points into it until snmp_sess_open
    detail::initLibrary("snmpwrap-client");
    impl_->config = cfg;

    netsnmp_session sess;
    snmp_sess_init(&sess);
    setup.configure(sess);

    {
        detail::ClientAddrScope source(cfg.localAddress);
        impl_->handle = snmp_sess_open(&sess);
    }
    if (!impl_->handle) {
        char* msg = nullptr;
        snmp_error(&sess, nullptr, nullptr, &msg);
        const std::string text = takeErrorText(msg);
        throw TransportError("cannot open SNMP session to " + cfg.peer + ": " + text +
                             (cfg.localAddress.empty() ? std::string()
                                                       : " (localAddress " + cfg.localAddress +
                                                             ": an address of this host, port free?)"));
    }
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

std::vector<VarBind> Client::get(const std::vector<Oid>& oids) {
    std::vector<VarBind> in;
    for (const auto& o : oids) in.push_back({o, Value::null()});
    return impl_->request(SNMP_MSG_GET, in);
}

VarBind Client::getNext(const Oid& oid) { return impl_->request(SNMP_MSG_GETNEXT, {{oid, Value::null()}}).front(); }

std::vector<VarBind> Client::getBulk(const std::vector<Oid>& oids, int nonRepeaters, int maxRepetitions) {
    if (impl_->config.version == SessionConfig::Version::V1) throw Error("GETBULK is not available in SNMPv1");
    std::vector<VarBind> in;
    for (const auto& o : oids) in.push_back({o, Value::null()});
    return impl_->request(SNMP_MSG_GETBULK, in, nonRepeaters, maxRepetitions);
}

void Client::set(const std::vector<VarBind>& varbinds) { impl_->request(SNMP_MSG_SET, varbinds); }

std::string Client::peer() const { return impl_->config.peer; }

void Client::walk(const Oid& root, const std::function<bool(const VarBind&)>& callback) {
    const bool v1 = impl_->config.version == SessionConfig::Version::V1;
    const detail::Walker walker(root, v1, impl_->config.maxRepetitions, [&](const Oid& after, int maxRepetitions) {
        return v1 ? impl_->request(SNMP_MSG_GETNEXT, {{after, Value::null()}})
                  : impl_->request(SNMP_MSG_GETBULK, {{after, Value::null()}}, 0, maxRepetitions);
    });
    walker.walk(callback);
}


}  // namespace snmpwrap
