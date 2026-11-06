#include "ClientDetail.hpp"

#include "Convert.hpp"

namespace snmpwrap::detail {

namespace {

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

// --- SessionSetup ---------------------------------------------------------------------------------

SessionSetup::SessionSetup(const SessionConfig& config) : cfg_(config) {
    if (!cfg_.localAddress.empty() && cfg_.peer.rfind("tcp", 0) == 0)
        throw Error("SessionConfig::localAddress works with UDP peers only, not with " + cfg_.peer);
}

void SessionSetup::configure(netsnmp_session& sess) const {
    sess.peername = const_cast<char*>(cfg_.peer.c_str());  // copied by snmp_sess_open
    sess.timeout = static_cast<long>(cfg_.timeout.count()) * 1000;  // microseconds
    sess.retries = cfg_.retries;
    using V = SessionConfig::Version;
    if (cfg_.version == V::V3) {
        using L = SessionConfig::SecurityLevel;
        sess.version = SNMP_VERSION_3;
        sess.securityName = const_cast<char*>(cfg_.user.c_str());
        sess.securityNameLen = cfg_.user.size();
        if (!cfg_.contextName.empty()) {
            sess.contextName = const_cast<char*>(cfg_.contextName.c_str());  // copied by snmp_sess_open
            sess.contextNameLen = cfg_.contextName.size();
        }
        sess.securityLevel = cfg_.securityLevel == L::NoAuthNoPriv   ? SNMP_SEC_LEVEL_NOAUTH
                             : cfg_.securityLevel == L::AuthNoPriv ? SNMP_SEC_LEVEL_AUTHNOPRIV
                                                                    : SNMP_SEC_LEVEL_AUTHPRIV;
        if (cfg_.securityLevel != L::NoAuthNoPriv) {
            using A = SessionConfig::AuthProtocol;
            switch (cfg_.authProtocol) {
                case A::MD5: setAuth(sess, usmHMACMD5AuthProtocol, OID_LENGTH(usmHMACMD5AuthProtocol)); break;
                case A::SHA1: setAuth(sess, usmHMACSHA1AuthProtocol, OID_LENGTH(usmHMACSHA1AuthProtocol)); break;
                case A::SHA224: setAuth(sess, kSHA224, OID_LENGTH(kSHA224)); break;
                case A::SHA256: setAuth(sess, kSHA256, OID_LENGTH(kSHA256)); break;
                case A::SHA384: setAuth(sess, kSHA384, OID_LENGTH(kSHA384)); break;
                case A::SHA512: setAuth(sess, kSHA512, OID_LENGTH(kSHA512)); break;
            }
            sess.securityAuthKeyLen = USM_AUTH_KU_LEN;
            if (generate_Ku(sess.securityAuthProto, static_cast<u_int>(sess.securityAuthProtoLen),
                            reinterpret_cast<const u_char*>(cfg_.authPassphrase.data()), cfg_.authPassphrase.size(),
                            sess.securityAuthKey, &sess.securityAuthKeyLen) != SNMPERR_SUCCESS)
                throw Error("cannot derive SNMPv3 authentication key (algorithm unsupported by this Net-SNMP build?)");
        }
        if (cfg_.securityLevel == L::AuthPriv) {
            using P = SessionConfig::PrivProtocol;
            switch (cfg_.privProtocol) {
                case P::DES: setPriv(sess, usmDESPrivProtocol, OID_LENGTH(usmDESPrivProtocol)); break;
                case P::AES128: setPriv(sess, usmAESPrivProtocol, OID_LENGTH(usmAESPrivProtocol)); break;
                case P::AES192: setPriv(sess, kAES192, OID_LENGTH(kAES192)); break;
                case P::AES256: setPriv(sess, kAES256, OID_LENGTH(kAES256)); break;
            }
            sess.securityPrivKeyLen = USM_PRIV_KU_LEN;
            if (generate_Ku(sess.securityAuthProto, static_cast<u_int>(sess.securityAuthProtoLen),
                            reinterpret_cast<const u_char*>(cfg_.privPassphrase.data()), cfg_.privPassphrase.size(),
                            sess.securityPrivKey, &sess.securityPrivKeyLen) != SNMPERR_SUCCESS)
                throw Error("cannot derive SNMPv3 privacy key (algorithm unsupported by this Net-SNMP build?)");
        }
    } else {
        sess.version = cfg_.version == V::V1 ? SNMP_VERSION_1 : SNMP_VERSION_2c;
        sess.community = reinterpret_cast<u_char*>(const_cast<char*>(cfg_.community.c_str()));
        sess.community_len = cfg_.community.size();
    }

}

// --- ClientAddrScope ------------------------------------------------------------------------------

bool ClientAddrScope::namesPort(const std::string& address) {
    if (!address.empty() && address.front() == '[') return address.find("]:") != std::string::npos;
    return address.find(':') != std::string::npos && address.find(':') == address.rfind(':');
}

// --- ResponseReader -------------------------------------------------------------------------------

ResponseReader::ResponseReader(int requestType, std::size_t requested, std::string peer)
    : type_(requestType), requested_(requested), peer_(std::move(peer)) {}

std::vector<VarBind> ResponseReader::read(const netsnmp_pdu& resp) const {
    if (resp.errstat != SNMP_ERR_NOERROR)
        throw ResponseError(static_cast<ErrorStatus>(resp.errstat), static_cast<int>(resp.errindex),
                            std::string("agent returned error: ") + snmp_errstring(static_cast<int>(resp.errstat)));
    std::vector<VarBind> out;
    for (auto* v = resp.variables; v; v = v->next_variable) {
        Value value = fromVar(v);  // first: GCC < 13 leaks the OID if a later aggregate initializer throws
        out.push_back({fromNetOid(v->name, v->name_length), std::move(value)});
    }
    // GET / GETNEXT / SET answer with exactly the varbinds of the request (RFC 3416 4.2); callers rely on it
    if (type_ != SNMP_MSG_GETBULK && out.size() != requested_)
        throw Error("agent " + peer_ + " answered " + std::to_string(out.size()) + " varbinds to a request with " +
                    std::to_string(requested_));
    return out;
}

// --- Walker ---------------------------------------------------------------------------------------

Walker::Walker(Oid root, bool v1, Fetch fetch) : root_(std::move(root)), v1_(v1), fetch_(std::move(fetch)) {}

void Walker::walk(const std::function<bool(const VarBind&)>& callback) const {
    Oid cur = root_;
    for (;;) {
        std::vector<VarBind> batch;
        try {
            batch = fetch_(cur);
        } catch (const ResponseError& e) {
            if (v1_ && e.status() == ErrorStatus::NoSuchName) return;  // v1 signals end of MIB this way
            throw;
        }
        if (batch.empty()) return;
        for (auto& vb : batch) {
            if (vb.value.type() == Type::EndOfMibView || !root_.isPrefixOf(vb.oid)) return;
            if (vb.oid <= cur) throw Error("agent returned non-increasing OID " + vb.oid.str() + " during walk");
            cur = vb.oid;
            if (!callback(vb)) return;
        }
    }
}

}  // namespace snmpwrap::detail
