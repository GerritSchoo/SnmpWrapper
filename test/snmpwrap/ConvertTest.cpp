// Unit tests of Convert.hpp / Convert.cpp (internal): Oid <-> Net-SNMP oid arrays, Value <-> Net-SNMP varbinds
// (assign / fromVar) for every type, the ASN.1 tag that goes on the wire, and values snmpwrap cannot represent.
// Only Net-SNMP's data structures are used - no session, no network.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "Convert.hpp"
#include "snmpwrap/Error.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using namespace snmpwrap::detail;

namespace {

/// One Net-SNMP varbind, freed at the end of the test.
class NetVar {
public:
    NetVar() {
        const ::oid name[] = {1, 3, 6, 1, 4, 1, 99999, 1, 0};
        snmp_varlist_add_variable(&list_, name, OID_LENGTH(name), ASN_NULL, nullptr, 0);
    }
    ~NetVar() { snmp_free_varbind(list_); }
    NetVar(const NetVar&) = delete;
    NetVar& operator=(const NetVar&) = delete;
    netsnmp_variable_list* get() { return list_; }

private:
    netsnmp_variable_list* list_ = nullptr;
};

// --- Oid <-> oid arrays ---------------------------------------------------------------------------

TEST(ConvertOid, ToNetOidCopiesEverySubId) {
    const auto net = toNetOid(Oid({1, 3, 6, 4294967295u}));
    ASSERT_EQ(net.size(), 4u);
    EXPECT_EQ(net[3], static_cast<::oid>(4294967295u));
}

TEST(ConvertOid, FromNetOidCopiesEverySubId) {
    const ::oid name[] = {1, 3, 6, 1};
    EXPECT_EQ(fromNetOid(name, 4), Oid({1, 3, 6, 1}));
    EXPECT_EQ(fromNetOid(name, 0), Oid{});
}

TEST(ConvertOid, RoundTrip) {
    const Oid o{1, 3, 6, 1, 4, 1, 99999, 0};
    const auto net = toNetOid(o);
    EXPECT_EQ(fromNetOid(net.data(), net.size()), o);
}

// --- assign + fromVar: every type -----------------------------------------------------------------

struct OnTheWire {
    const char* what;
    Value value;
    u_char asnType;   ///< the ASN.1 tag assign() writes
    Value readBack;   ///< what fromVar() makes of it
};

void PrintTo(const OnTheWire& w, std::ostream* os) { *os << w.what; }

class ConvertValue : public ::testing::TestWithParam<OnTheWire> {};

TEST_P(ConvertValue, AssignWritesTheAsnType) {
    NetVar var;
    assign(var.get(), GetParam().value);
    EXPECT_EQ(var.get()->type, GetParam().asnType);
}

TEST_P(ConvertValue, FromVarReadsItBack) {
    NetVar var;
    assign(var.get(), GetParam().value);
    EXPECT_EQ(fromVar(var.get()), GetParam().readBack);
}

TEST_P(ConvertValue, AssignKeepsTheName) {
    NetVar var;
    assign(var.get(), GetParam().value);
    EXPECT_EQ(fromNetOid(var.get()->name, var.get()->name_length), Oid({1, 3, 6, 1, 4, 1, 99999, 1, 0}));
}

INSTANTIATE_TEST_SUITE_P(
    Convert, ConvertValue,
    ::testing::Values(
        OnTheWire{"integer", Value::integer(-5), ASN_INTEGER, Value::integer(-5)},
        OnTheWire{"integer min", Value::integer(std::numeric_limits<std::int32_t>::min()), ASN_INTEGER,
                  Value::integer(std::numeric_limits<std::int32_t>::min())},
        OnTheWire{"integer max", Value::integer(std::numeric_limits<std::int32_t>::max()), ASN_INTEGER,
                  Value::integer(std::numeric_limits<std::int32_t>::max())},
        OnTheWire{"gauge", Value::gauge(4294967295u), ASN_GAUGE, Value::gauge(4294967295u)},
        OnTheWire{"counter32", Value::counter32(7), ASN_COUNTER, Value::counter32(7)},
        OnTheWire{"timeticks", Value::timeTicks(153), ASN_TIMETICKS, Value::timeTicks(153)},
        OnTheWire{"counter64", Value::counter64(0x1122334455667788ULL), ASN_COUNTER64, Value::counter64(0x1122334455667788ULL)},
        OnTheWire{"counter64 max", Value::counter64(18446744073709551615ULL), ASN_COUNTER64, Value::counter64(18446744073709551615ULL)},
        OnTheWire{"string", Value::string("hello"), ASN_OCTET_STR, Value::string("hello")},
        OnTheWire{"empty string", Value::string(""), ASN_OCTET_STR, Value::string("")},
        OnTheWire{"binary string", Value::string(std::string("\0\xff", 2)), ASN_OCTET_STR, Value::string(std::string("\0\xff", 2))},
        OnTheWire{"opaque", Value::opaque("ab"), ASN_OPAQUE, Value::opaque("ab")},
        // SMIv2 BITS are an OCTET STRING on the wire (RFC 2578 7.1.4): they come back as one
        OnTheWire{"bits", Value::bits(std::string(1, '\x80')), ASN_OCTET_STR, Value::string(std::string(1, '\x80'))},
        OnTheWire{"object id", Value::oid(Oid({1, 3, 6, 1, 4294967295u})), ASN_OBJECT_ID, Value::oid(Oid({1, 3, 6, 1, 4294967295u}))},
        OnTheWire{"ip address", Value::ipAddress(10, 0, 0, 255), ASN_IPADDRESS, Value::ipAddress(10, 0, 0, 255)},
        OnTheWire{"null", Value::null(), ASN_NULL, Value::null()},
        OnTheWire{"noSuchObject", Value::exception(Type::NoSuchObject), SNMP_NOSUCHOBJECT, Value::exception(Type::NoSuchObject)},
        OnTheWire{"noSuchInstance", Value::exception(Type::NoSuchInstance), SNMP_NOSUCHINSTANCE,
                  Value::exception(Type::NoSuchInstance)},
        OnTheWire{"endOfMibView", Value::exception(Type::EndOfMibView), SNMP_ENDOFMIBVIEW, Value::exception(Type::EndOfMibView)}));

TEST(ConvertValue, AssignReplacesAPreviousValue) {
    NetVar var;
    assign(var.get(), Value::string("a long text that needs its own buffer"));
    assign(var.get(), Value::integer(3));
    EXPECT_EQ(fromVar(var.get()), Value::integer(3));
}

// --- fromVar: types from other agents -------------------------------------------------------------

TEST(ConvertFromVar, ObsoleteUInteger32IsAGauge) {
    NetVar var;
    const u_long x = 9;
    snmp_set_var_typed_value(var.get(), ASN_UINTEGER, &x, sizeof x);
    EXPECT_EQ(fromVar(var.get()), Value::gauge(9));
}

TEST(ConvertFromVar, ObsoleteBitStringIsBits) {
    NetVar var;
    const u_char bits[] = {0xa0};
    snmp_set_var_typed_value(var.get(), ASN_BIT_STR, bits, sizeof bits);
    EXPECT_EQ(fromVar(var.get()), Value::bits(std::string("\xa0", 1)));
}

TEST(ConvertFromVar, IpAddressWithoutFourOctetsThrows) {
    NetVar var;
    const u_char three[] = {10, 0, 0};
    snmp_set_var_typed_value(var.get(), ASN_IPADDRESS, three, sizeof three);
    EXPECT_THROW(fromVar(var.get()), Error);
}

TEST(ConvertFromVar, UnsupportedTypeThrows) {
    NetVar var;
    const float f = 1.5f;
    snmp_set_var_typed_value(var.get(), ASN_OPAQUE_FLOAT, &f, sizeof f);
    EXPECT_THROW(fromVar(var.get()), Error);
}

}  // namespace
