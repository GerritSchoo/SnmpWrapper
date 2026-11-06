#include "NotificationDetail.hpp"

#include <algorithm>

#include "Convert.hpp"
#include "snmpwrap/Error.hpp"

namespace snmpwrap::detail {

namespace {

const Oid kSysUpTime{1, 3, 6, 1, 2, 1, 1, 3, 0};
const Oid kSnmpTrapOid{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};
const Oid kSnmpTraps{1, 3, 6, 1, 6, 3, 1, 1, 5};  // generic v1 traps: snmpTraps.(generic + 1)

}  // namespace

std::optional<Notification> NotificationDecoder::decode(const netsnmp_pdu& pdu, std::string source) const {
    Notification n;
    if (pdu.community) n.community.assign(reinterpret_cast<const char*>(pdu.community), pdu.community_len);
    if (!accepted_.empty() && std::find(accepted_.begin(), accepted_.end(), n.community) == accepted_.end())
        return std::nullopt;  // not accepted: no handler, no acknowledgement
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
            n.version = 2;
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
        if (n.version == 2 && vb.oid == kSysUpTime && vb.value.type() == Type::TimeTicks) {
            n.uptime = vb.value.asUInt();
        } else if (n.version == 2 && vb.oid == kSnmpTrapOid && vb.value.type() == Type::ObjectId) {
            n.trapOid = vb.value.asOid();
        } else {
            n.vars.push_back(std::move(vb));
        }
    }
    return n;
}

}  // namespace snmpwrap::detail
