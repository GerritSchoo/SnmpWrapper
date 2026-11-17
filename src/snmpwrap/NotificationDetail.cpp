#include "NotificationDetail.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include "Convert.hpp"
#include "Usm.hpp"
#include "snmpwrap/Error.hpp"

namespace snmpwrap::detail {

namespace {

const Oid kSysUpTime{1, 3, 6, 1, 2, 1, 1, 3, 0};
const Oid kSnmpTrapOid{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};
const Oid kSnmpTraps{1, 3, 6, 1, 6, 3, 1, 1, 5};  // generic v1 traps: snmpTraps.(generic + 1)

SecurityLevel levelOf(int netsnmpLevel) {
    switch (netsnmpLevel) {
        case SNMP_SEC_LEVEL_AUTHPRIV: return SecurityLevel::AuthPriv;
        case SNMP_SEC_LEVEL_AUTHNOPRIV: return SecurityLevel::AuthNoPriv;
        default: return SecurityLevel::NoAuthNoPriv;
    }
}

/// Collects what Net-SNMP logs at LOG_ERR and above while it lives (createUser reports its errors only that way).
class ErrorLog {
public:
    ErrorLog() {
        handler_ = netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_ERR);
        snmp_register_callback(SNMP_CALLBACK_LIBRARY, SNMP_CALLBACK_LOGGING, &ErrorLog::collect, this);
    }
    ~ErrorLog() {
        snmp_unregister_callback(SNMP_CALLBACK_LIBRARY, SNMP_CALLBACK_LOGGING, &ErrorLog::collect, this, 1);
        if (handler_) netsnmp_remove_loghandler(handler_);
    }
    ErrorLog(const ErrorLog&) = delete;
    ErrorLog& operator=(const ErrorLog&) = delete;
    const std::string& text() const { return text_; }

private:
    static int collect(int, int, void* serverarg, void* clientarg) {
        const auto* msg = static_cast<const snmp_log_message*>(serverarg);
        auto* self = static_cast<ErrorLog*>(clientarg);
        if (msg && msg->msg && msg->priority <= LOG_ERR) {
            std::string line(msg->msg);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) self->text_ += (self->text_.empty() ? "" : "; ") + line;
        }
        return 0;
    }
    netsnmp_log_handler* handler_ = nullptr;
    std::string text_;
};

/// A master key (Ku) of @p passphrase for createUser's "-m <hex>": no quoting problems with any passphrase.
std::string masterKeyHex(AuthProtocol auth, const std::string& passphrase, const std::string& what) {
    if (passphrase.size() < 8) throw Error(what + " passphrase must have at least 8 characters");
    const ProtocolOid proto = authProtocolOid(auth);
    u_char key[SNMP_MAXBUF_SMALL];
    size_t keyLen = sizeof key;
    if (generate_Ku(proto.oid, static_cast<u_int>(proto.length), reinterpret_cast<const u_char*>(passphrase.data()),
                    passphrase.size(), key, &keyLen) != SNMPERR_SUCCESS)
        throw Error("cannot derive the " + what + " key (" + authProtocolName(auth) +
                    " not supported by this Net-SNMP build?)");
    std::string hex;
    char byte[3];
    for (size_t i = 0; i < keyLen; ++i) {
        std::snprintf(byte, sizeof byte, "%02x", key[i]);
        hex += byte;
    }
    return hex;
}

}  // namespace

std::optional<Notification> NotificationDecoder::decode(const netsnmp_pdu& pdu, std::string source) const {
    Notification n;
    if (pdu.version == SNMP_VERSION_3) {
        // Net-SNMP has checked user and keys already; what is left: is it one of ours, at its level?
        if (pdu.securityName) n.user.assign(pdu.securityName, pdu.securityNameLen);
        n.securityLevel = levelOf(pdu.securityLevel);
        const auto user = users_.find(n.user);
        if (user == users_.end() || n.securityLevel < user->second) return std::nullopt;  // no handler, no acknowledgement
    } else {
        if (onlyV3_) return std::nullopt;
        if (pdu.community) n.community.assign(reinterpret_cast<const char*>(pdu.community), pdu.community_len);
        if (!accepted_.empty() && std::find(accepted_.begin(), accepted_.end(), n.community) == accepted_.end())
            return std::nullopt;  // not accepted: no handler, no acknowledgement
    }
    n.source = std::move(source);

    switch (pdu.command) {
        case SNMP_MSG_TRAP: {  // SNMPv1: enterprise / generic / specific -> snmpTrapOID (RFC 3584 3.1)
            n.version = 1;
            n.uptime = static_cast<std::uint32_t>(pdu.time);
            if (pdu.trap_type != SNMP_TRAP_ENTERPRISESPECIFIC) {
                n.trapOid = kSnmpTraps + static_cast<SubId>(pdu.trap_type + 1);
            } else {
                n.trapOid = fromNetOid(pdu.enterprise, pdu.enterprise_length) + SubId{0} + static_cast<SubId>(pdu.specific_type);
            }
            break;
        }
        case SNMP_MSG_TRAP2:
        case SNMP_MSG_INFORM:
            n.version = pdu.version == SNMP_VERSION_3 ? 3 : 2;
            n.inform = pdu.command == SNMP_MSG_INFORM;
            break;
        default:
            return std::nullopt;  // not a notification
    }

    for (const netsnmp_variable_list* v = pdu.variables; v; v = v->next_variable) {
        VarBind vb{fromNetOid(v->name, v->name_length), Value()};
        try {
            vb.value = fromVar(v);
        } catch (const Error&) {
            continue;  // a type snmpwrap cannot represent
        }
        const bool v2Header = n.version != 1;  // v2c and v3 notifications start with sysUpTime.0, snmpTrapOID.0
        if (v2Header && vb.oid == kSysUpTime && vb.value.type() == Type::TimeTicks) {
            n.uptime = vb.value.asUInt();
        } else if (v2Header && vb.oid == kSnmpTrapOid && vb.value.type() == Type::ObjectId) {
            n.trapOid = vb.value.asOid();
        } else {
            n.vars.push_back(std::move(vb));
        }
    }
    return n;
}

void createUsmUser(const NotificationUser& user) {
    const auto blank = [](unsigned char c) { return std::isspace(c) || c == '"' || c == '\''; };
    if (user.name.empty() || std::any_of(user.name.begin(), user.name.end(), blank))
        throw Error("SNMPv3 user: the name must not be empty or contain blanks or quotes");
    const std::string who = "SNMPv3 user " + user.name + ": ";

    std::string line;
    if (!user.engineId.empty()) {
        std::string hex = user.engineId;
        if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex.erase(0, 2);
        if (hex.empty() || hex.size() % 2 != 0 || hex.size() > 64 ||
            !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); }))
            throw Error(who + "engineId must be hex digits (5 to 32 bytes), e.g. 0x80001f8803...");
        line += "-e 0x" + hex + " ";
    }
    line += user.name;
    try {
        if (user.securityLevel != SecurityLevel::NoAuthNoPriv)
            line += std::string(" ") + authProtocolName(user.authProtocol) + " -m " +
                    masterKeyHex(user.authProtocol, user.authPassphrase, "authentication");
        if (user.securityLevel == SecurityLevel::AuthPriv)  // privacy keys are derived with the auth hash
            line += std::string(" ") + privProtocolName(user.privProtocol) + " -m " +
                    masterKeyHex(user.authProtocol, user.privPassphrase, "privacy");
    } catch (const Error& e) {
        throw Error(who + e.what());
    }

    ErrorLog log;
    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    usm_parse_create_usmUser("createUser", buf.data());
    if (!log.text().empty()) throw Error(who + log.text());
}

}  // namespace snmpwrap::detail
