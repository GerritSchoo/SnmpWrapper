#include "snmpwrap/client.hpp"

#include <cstdlib>
#include <memory>

#include "convert.hpp"

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

// SHA-2 (RFC 7860) and AES192/256 (Blumenthal draft) protocol OIDs. Defined here because Net-SNMP only
// exports its own copies when built with OpenSSL / --enable-blumenthal-aes; if the library lacks the
// algorithm, key derivation or session opening fails and is reported as an Error.
const ::oid kSHA224[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 4};
const ::oid kSHA256[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 5};
const ::oid kSHA384[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 6};
const ::oid kSHA512[] = {1, 3, 6, 1, 6, 3, 10, 1, 1, 7};
const ::oid kAES192[] = {1, 3, 6, 1, 4, 1, 14832, 1, 3};
const ::oid kAES256[] = {1, 3, 6, 1, 4, 1, 14832, 1, 4};

void setAuth(netsnmp_session& sess, const ::oid* proto, std::size_t len) {
    sess.securityAuthProto = const_cast<::oid*>(proto);
    sess.securityAuthProtoLen = len;
}

void setPriv(netsnmp_session& sess, const ::oid* proto, std::size_t len) {
    sess.securityPrivProto = const_cast<::oid*>(proto);
    sess.securityPrivProtoLen = len;
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

        if (status == STAT_TIMEOUT) throw TransportError("SNMP request to " + config.peer + " timed out");
        if (status != STAT_SUCCESS) {
            char* msg = nullptr;
            snmp_sess_error(handle, nullptr, nullptr, &msg);
            const std::string text = takeErrorText(msg);
            throw TransportError("SNMP request to " + config.peer + " failed: " + text);
        }
        if (resp->errstat != SNMP_ERR_NOERROR)
            throw ResponseError(static_cast<ErrorStatus>(resp->errstat), static_cast<int>(resp->errindex),
                                std::string("agent returned error: ") + snmp_errstring(static_cast<int>(resp->errstat)));

        std::vector<VarBind> out;
        for (auto* v = resp->variables; v; v = v->next_variable)
            out.push_back({detail::fromNetOid(v->name, v->name_length), detail::fromVar(v)});
        return out;
    }
};

Client::Client(const SessionConfig& cfg) : impl_(std::make_unique<Impl>()) {
    detail::initLibrary("snmpwrap-client");
    impl_->config = cfg;

    netsnmp_session sess;
    snmp_sess_init(&sess);
    sess.peername = const_cast<char*>(cfg.peer.c_str());  // copied by snmp_sess_open
    sess.timeout = static_cast<long>(cfg.timeout.count()) * 1000;  // microseconds
    sess.retries = cfg.retries;

    using V = SessionConfig::Version;
    if (cfg.version == V::V3) {
        using L = SessionConfig::SecurityLevel;
        sess.version = SNMP_VERSION_3;
        sess.securityName = const_cast<char*>(cfg.user.c_str());
        sess.securityNameLen = cfg.user.size();
        if (!cfg.contextName.empty()) {
            sess.contextName = const_cast<char*>(cfg.contextName.c_str());  // copied by snmp_sess_open
            sess.contextNameLen = cfg.contextName.size();
        }
        sess.securityLevel = cfg.securityLevel == L::NoAuthNoPriv   ? SNMP_SEC_LEVEL_NOAUTH
                             : cfg.securityLevel == L::AuthNoPriv ? SNMP_SEC_LEVEL_AUTHNOPRIV
                                                                    : SNMP_SEC_LEVEL_AUTHPRIV;
        if (cfg.securityLevel != L::NoAuthNoPriv) {
            using A = SessionConfig::AuthProtocol;
            switch (cfg.authProtocol) {
                case A::MD5: setAuth(sess, usmHMACMD5AuthProtocol, OID_LENGTH(usmHMACMD5AuthProtocol)); break;
                case A::SHA1: setAuth(sess, usmHMACSHA1AuthProtocol, OID_LENGTH(usmHMACSHA1AuthProtocol)); break;
                case A::SHA224: setAuth(sess, kSHA224, OID_LENGTH(kSHA224)); break;
                case A::SHA256: setAuth(sess, kSHA256, OID_LENGTH(kSHA256)); break;
                case A::SHA384: setAuth(sess, kSHA384, OID_LENGTH(kSHA384)); break;
                case A::SHA512: setAuth(sess, kSHA512, OID_LENGTH(kSHA512)); break;
            }
            sess.securityAuthKeyLen = USM_AUTH_KU_LEN;
            if (generate_Ku(sess.securityAuthProto, static_cast<u_int>(sess.securityAuthProtoLen),
                            reinterpret_cast<const u_char*>(cfg.authPassphrase.data()), cfg.authPassphrase.size(),
                            sess.securityAuthKey, &sess.securityAuthKeyLen) != SNMPERR_SUCCESS)
                throw Error("cannot derive SNMPv3 authentication key (algorithm unsupported by this Net-SNMP build?)");
        }
        if (cfg.securityLevel == L::AuthPriv) {
            using P = SessionConfig::PrivProtocol;
            switch (cfg.privProtocol) {
                case P::DES: setPriv(sess, usmDESPrivProtocol, OID_LENGTH(usmDESPrivProtocol)); break;
                case P::AES128: setPriv(sess, usmAESPrivProtocol, OID_LENGTH(usmAESPrivProtocol)); break;
                case P::AES192: setPriv(sess, kAES192, OID_LENGTH(kAES192)); break;
                case P::AES256: setPriv(sess, kAES256, OID_LENGTH(kAES256)); break;
            }
            sess.securityPrivKeyLen = USM_PRIV_KU_LEN;
            if (generate_Ku(sess.securityAuthProto, static_cast<u_int>(sess.securityAuthProtoLen),
                            reinterpret_cast<const u_char*>(cfg.privPassphrase.data()), cfg.privPassphrase.size(),
                            sess.securityPrivKey, &sess.securityPrivKeyLen) != SNMPERR_SUCCESS)
                throw Error("cannot derive SNMPv3 privacy key (algorithm unsupported by this Net-SNMP build?)");
        }
    } else {
        sess.version = cfg.version == V::V1 ? SNMP_VERSION_1 : SNMP_VERSION_2c;
        sess.community = reinterpret_cast<u_char*>(const_cast<char*>(cfg.community.c_str()));
        sess.community_len = cfg.community.size();
    }

    impl_->handle = snmp_sess_open(&sess);
    if (!impl_->handle) {
        char* msg = nullptr;
        snmp_error(&sess, nullptr, nullptr, &msg);
        const std::string text = takeErrorText(msg);
        throw TransportError("cannot open SNMP session to " + cfg.peer + ": " + text);
    }
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

VarBind Client::get(const Oid& oid) { return get(std::vector<Oid>{oid}).front(); }

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

void Client::set(const Oid& oid, const Value& value) { set(std::vector<VarBind>{{oid, value}}); }

void Client::set(const std::vector<VarBind>& varbinds) { impl_->request(SNMP_MSG_SET, varbinds); }

void Client::walk(const Oid& root, const std::function<bool(const VarBind&)>& callback) {
    const bool v1 = impl_->config.version == SessionConfig::Version::V1;
    Oid cur = root;
    for (;;) {
        std::vector<VarBind> batch;
        try {
            batch = v1 ? impl_->request(SNMP_MSG_GETNEXT, {{cur, Value::null()}})
                       : impl_->request(SNMP_MSG_GETBULK, {{cur, Value::null()}}, 0, 20);
        } catch (const ResponseError& e) {
            if (v1 && e.status() == ErrorStatus::NoSuchName) return;  // v1 signals end of MIB this way
            throw;
        }
        if (batch.empty()) return;
        for (auto& vb : batch) {
            if (vb.value.type() == Type::EndOfMibView || !root.isPrefixOf(vb.oid)) return;
            if (vb.oid <= cur) throw Error("agent returned non-increasing OID " + vb.oid.str() + " during walk");
            cur = vb.oid;
            if (!callback(vb)) return;
        }
    }
}

std::vector<VarBind> Client::walk(const Oid& root) {
    std::vector<VarBind> out;
    walk(root, [&](const VarBind& vb) {
        out.push_back(vb);
        return true;
    });
    return out;
}

}  // namespace snmpwrap
