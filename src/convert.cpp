#include "convert.hpp"

#include <mutex>

#include "snmpwrap/error.hpp"

namespace snmpwrap::detail {

namespace {
std::mutex g_initMutex;
bool g_initialized = false;
}  // namespace

void initLibrary(const char* appName) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initialized) return;
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PERSIST_STATE, 1);
    init_snmp(appName);
    g_initialized = true;
}

bool libraryInitialized() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    return g_initialized;
}

void shutdownLibrary(const char* appName) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    snmp_shutdown(appName);
    g_initialized = false;
}

std::vector<::oid> toNetOid(const Oid& o) {
    std::vector<::oid> out;
    out.reserve(o.size());
    for (SubId s : o.ids()) out.push_back(static_cast<::oid>(s));
    return out;
}

Oid fromNetOid(const ::oid* name, std::size_t len) {
    std::vector<SubId> ids;
    ids.reserve(len);
    for (std::size_t i = 0; i < len; ++i) ids.push_back(static_cast<SubId>(name[i]));
    return Oid(std::move(ids));
}

void assign(netsnmp_variable_list* var, const Value& v) {
    int rc = 0;
    switch (v.type()) {
        case Type::Integer: {
            long x = v.asInt();
            rc = snmp_set_var_typed_value(var, ASN_INTEGER, &x, sizeof x);
            break;
        }
        case Type::Gauge32:
        case Type::Counter32:
        case Type::TimeTicks: {
            u_long x = v.asUInt();
            const u_char asn = v.type() == Type::Gauge32    ? ASN_GAUGE
                               : v.type() == Type::Counter32 ? ASN_COUNTER
                                                              : ASN_TIMETICKS;
            rc = snmp_set_var_typed_value(var, asn, &x, sizeof x);
            break;
        }
        case Type::Counter64: {
            struct counter64 c;
            const std::uint64_t x = v.asUInt64();
            c.high = static_cast<u_long>(x >> 32);
            c.low = static_cast<u_long>(x & 0xFFFFFFFFu);
            rc = snmp_set_var_typed_value(var, ASN_COUNTER64, &c, sizeof c);
            break;
        }
        case Type::OctetString:
        case Type::Opaque:
        case Type::Bits: {
            const std::string& s = v.asString();
            const u_char asn = v.type() == Type::Opaque ? ASN_OPAQUE : v.type() == Type::Bits ? ASN_BIT_STR : ASN_OCTET_STR;
            rc = snmp_set_var_typed_value(var, asn, s.data(), s.size());
            break;
        }
        case Type::ObjectId: {
            auto o = toNetOid(v.asOid());
            rc = snmp_set_var_typed_value(var, ASN_OBJECT_ID, o.data(), o.size() * sizeof(::oid));
            break;
        }
        case Type::IpAddress: {
            auto ip = v.asIp();
            rc = snmp_set_var_typed_value(var, ASN_IPADDRESS, ip.data(), ip.size());
            break;
        }
        case Type::Null:
            rc = snmp_set_var_typed_value(var, ASN_NULL, nullptr, 0);
            break;
        case Type::NoSuchObject:
            rc = snmp_set_var_typed_value(var, SNMP_NOSUCHOBJECT, nullptr, 0);
            break;
        case Type::NoSuchInstance:
            rc = snmp_set_var_typed_value(var, SNMP_NOSUCHINSTANCE, nullptr, 0);
            break;
        case Type::EndOfMibView:
            rc = snmp_set_var_typed_value(var, SNMP_ENDOFMIBVIEW, nullptr, 0);
            break;
    }
    if (rc != 0) throw Error("snmp_set_var_typed_value failed");
}

Value fromVar(const netsnmp_variable_list* var) {
    switch (var->type) {
        case ASN_INTEGER:
            return Value::integer(static_cast<std::int32_t>(*var->val.integer));
        case ASN_GAUGE:     // == ASN_UNSIGNED (Gauge32 / Unsigned32)
        case ASN_UINTEGER:  // obsolete SMIv1 UInteger32
            return Value::gauge(static_cast<std::uint32_t>(*var->val.integer));
        case ASN_COUNTER:
            return Value::counter32(static_cast<std::uint32_t>(*var->val.integer));
        case ASN_TIMETICKS:
            return Value::timeTicks(static_cast<std::uint32_t>(*var->val.integer));
        case ASN_COUNTER64:
            return Value::counter64((static_cast<std::uint64_t>(var->val.counter64->high) << 32) |
                                    (static_cast<std::uint64_t>(var->val.counter64->low) & 0xFFFFFFFFu));
        case ASN_OCTET_STR:
            return Value::string(std::string(reinterpret_cast<const char*>(var->val.string), var->val_len));
        case ASN_OPAQUE:
            return Value::opaque(std::string(reinterpret_cast<const char*>(var->val.string), var->val_len));
        case ASN_BIT_STR:
            return Value::bits(std::string(reinterpret_cast<const char*>(var->val.string), var->val_len));
        case ASN_OBJECT_ID:
            return Value::oid(fromNetOid(var->val.objid, var->val_len / sizeof(::oid)));
        case ASN_IPADDRESS:
            if (var->val_len != 4) throw Error("malformed IpAddress");
            return Value::ipAddress(var->val.string[0], var->val.string[1], var->val.string[2], var->val.string[3]);
        case ASN_NULL:
            return Value::null();
        case SNMP_NOSUCHOBJECT:
            return Value::exception(Type::NoSuchObject);
        case SNMP_NOSUCHINSTANCE:
            return Value::exception(Type::NoSuchInstance);
        case SNMP_ENDOFMIBVIEW:
            return Value::exception(Type::EndOfMibView);
        default:
            throw Error("unsupported ASN.1 type " + std::to_string(var->type));
    }
}

}  // namespace snmpwrap::detail
