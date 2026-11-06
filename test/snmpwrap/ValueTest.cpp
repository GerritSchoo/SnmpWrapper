// Unit tests of Value.hpp / Value.cpp: toString(Type), the Value factories and accessors, equality, str(),
// isException(), fits().

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/Value.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;

namespace {

// --- toString(Type) -------------------------------------------------------------------------------

struct TypeName {
    Type type;
    const char* name;
};

class ValueTypeName : public ::testing::TestWithParam<TypeName> {};

TEST_P(ValueTypeName, IsTheEnumeratorName) { EXPECT_STREQ(toString(GetParam().type), GetParam().name); }

INSTANTIATE_TEST_SUITE_P(Value, ValueTypeName,
                         ::testing::Values(TypeName{Type::Null, "Null"}, TypeName{Type::Integer, "Integer"},
                                           TypeName{Type::OctetString, "OctetString"}, TypeName{Type::ObjectId, "ObjectId"},
                                           TypeName{Type::IpAddress, "IpAddress"}, TypeName{Type::Counter32, "Counter32"},
                                           TypeName{Type::Gauge32, "Gauge32"}, TypeName{Type::TimeTicks, "TimeTicks"},
                                           TypeName{Type::Counter64, "Counter64"}, TypeName{Type::Opaque, "Opaque"},
                                           TypeName{Type::Bits, "Bits"}, TypeName{Type::NoSuchObject, "NoSuchObject"},
                                           TypeName{Type::NoSuchInstance, "NoSuchInstance"},
                                           TypeName{Type::EndOfMibView, "EndOfMibView"}));

// --- every factory: type, text, exception marker --------------------------------------------------

struct Made {
    Value value;
    Type type;
    std::string str;
    bool exception;
};

class ValueFactory : public ::testing::TestWithParam<Made> {};

TEST_P(ValueFactory, HasItsType) { EXPECT_EQ(GetParam().value.type(), GetParam().type); }
TEST_P(ValueFactory, StrNamesTypeAndValue) { EXPECT_EQ(GetParam().value.str(), GetParam().str); }
TEST_P(ValueFactory, IsExceptionOnlyForTheMarkers) { EXPECT_EQ(GetParam().value.isException(), GetParam().exception); }
TEST_P(ValueFactory, FitsItsOwnType) { EXPECT_TRUE(GetParam().value.fits(GetParam().type)); }
TEST_P(ValueFactory, EqualsACopy) {
    const Value copy = GetParam().value;
    EXPECT_EQ(copy, GetParam().value);
}

INSTANTIATE_TEST_SUITE_P(
    Value, ValueFactory,
    ::testing::Values(Made{Value(), Type::Null, "Null", false},
                      Made{Value::null(), Type::Null, "Null", false},
                      Made{Value::integer(-5), Type::Integer, "Integer: -5", false},
                      Made{Value::gauge(7), Type::Gauge32, "Gauge32: 7", false},
                      Made{Value::counter32(8), Type::Counter32, "Counter32: 8", false},
                      Made{Value::timeTicks(9), Type::TimeTicks, "TimeTicks: 9", false},
                      Made{Value::counter64(18446744073709551615ULL), Type::Counter64, "Counter64: 18446744073709551615", false},
                      Made{Value::string("abc"), Type::OctetString, "OctetString: \"abc\"", false},
                      Made{Value::string(""), Type::OctetString, "OctetString: \"\"", false},
                      Made{Value::string(std::string("a\x01", 2)), Type::OctetString, "OctetString (hex): 61 01", false},
                      Made{Value::opaque("ab"), Type::Opaque, "Opaque (hex): 61 62", false},  // never shown as text
                      Made{Value::bits(std::string("\xA0", 1)), Type::Bits, "Bits (hex): A0", false},
                      Made{Value::oid(Oid({1, 3, 6})), Type::ObjectId, "ObjectId: 1.3.6", false},
                      Made{Value::ipAddress(10, 0, 0, 255), Type::IpAddress, "IpAddress: 10.0.0.255", false},
                      Made{Value::exception(Type::NoSuchObject), Type::NoSuchObject, "NoSuchObject", true},
                      Made{Value::exception(Type::NoSuchInstance), Type::NoSuchInstance, "NoSuchInstance", true},
                      Made{Value::exception(Type::EndOfMibView), Type::EndOfMibView, "EndOfMibView", true}));

// --- accessors ------------------------------------------------------------------------------------

TEST(ValueAsInt, ReturnsTheInteger) {
    EXPECT_EQ(Value::integer(std::numeric_limits<std::int32_t>::min()).asInt(), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(Value::integer(std::numeric_limits<std::int32_t>::max()).asInt(), std::numeric_limits<std::int32_t>::max());
}

TEST(ValueAsInt, OfAnotherTypeThrows) {
    EXPECT_THROW(Value::gauge(1).asInt(), Error);
    EXPECT_THROW(Value::string("1").asInt(), Error);
    EXPECT_THROW(Value::null().asInt(), Error);
}

TEST(ValueAsUInt, ReturnsGaugeCounterAndTimeTicks) {
    EXPECT_EQ(Value::gauge(4294967295u).asUInt(), 4294967295u);
    EXPECT_EQ(Value::counter32(2).asUInt(), 2u);
    EXPECT_EQ(Value::timeTicks(3).asUInt(), 3u);
}

TEST(ValueAsUInt, AcceptsANonNegativeInteger) { EXPECT_EQ(Value::integer(3).asUInt(), 3u); }

TEST(ValueAsUInt, RefusesANegativeInteger) { EXPECT_THROW(Value::integer(-1).asUInt(), Error); }

TEST(ValueAsUInt, OfAnotherTypeThrows) {
    EXPECT_THROW(Value::counter64(1).asUInt(), Error);  // may not fit into 32 bits
    EXPECT_THROW(Value::string("1").asUInt(), Error);
}

TEST(ValueAsUInt64, ReturnsCounter64AndEveryUnsigned) {
    EXPECT_EQ(Value::counter64(0x1122334455667788ULL).asUInt64(), 0x1122334455667788ULL);
    EXPECT_EQ(Value::gauge(7).asUInt64(), 7u);
    EXPECT_EQ(Value::integer(5).asUInt64(), 5u);
}

TEST(ValueAsUInt64, OfAnotherTypeThrows) {
    EXPECT_THROW(Value::integer(-1).asUInt64(), Error);
    EXPECT_THROW(Value::oid(Oid({1})).asUInt64(), Error);
}

TEST(ValueAsString, ReturnsOctetStringOpaqueAndBits) {
    EXPECT_EQ(Value::string("abc").asString(), "abc");
    EXPECT_EQ(Value::opaque("ab").asString(), "ab");
    EXPECT_EQ(Value::bits(std::string("\xA0", 1)).asString(), std::string("\xA0", 1));
    EXPECT_EQ(Value::string(std::string("\0x", 2)).asString().size(), 2u);  // binary safe
}

TEST(ValueAsString, OfAnotherTypeThrows) { EXPECT_THROW(Value::integer(1).asString(), Error); }

TEST(ValueAsOid, ReturnsTheOid) { EXPECT_EQ(Value::oid(Oid({1, 3})).asOid(), Oid({1, 3})); }

TEST(ValueAsOid, OfAnotherTypeThrows) { EXPECT_THROW(Value::string("1.3").asOid(), Error); }

TEST(ValueAsIp, ReturnsTheFourOctets) {
    const auto ip = Value::ipAddress(192, 168, 1, 2).asIp();
    EXPECT_EQ(ip[0], 192);
    EXPECT_EQ(ip[3], 2);
}

TEST(ValueAsIp, OfAnotherTypeThrows) { EXPECT_THROW(Value::gauge(1).asIp(), Error); }

// --- equality -------------------------------------------------------------------------------------

TEST(ValueEquality, ComparesTypeAndData) {
    EXPECT_EQ(Value::integer(1), Value::integer(1));
    EXPECT_NE(Value::integer(1), Value::integer(2));
    EXPECT_NE(Value::integer(1), Value::gauge(1));        // same number, other type
    EXPECT_NE(Value::opaque("x"), Value::string("x"));    // same bytes, other type
    EXPECT_NE(Value::bits("x"), Value::opaque("x"));
    EXPECT_NE(Value::exception(Type::NoSuchObject), Value::exception(Type::NoSuchInstance));
}

// --- fits -----------------------------------------------------------------------------------------

TEST(ValueFits, BitsAcceptAnOctetString) {  // BITS have no tag of their own on the wire
    EXPECT_TRUE(Value::string("x").fits(Type::Bits));
    EXPECT_TRUE(Value::bits("x").fits(Type::Bits));
}

TEST(ValueFits, NothingElseIsInterchangeable) {
    EXPECT_FALSE(Value::bits("x").fits(Type::OctetString));
    EXPECT_FALSE(Value::opaque("x").fits(Type::Bits));
    EXPECT_FALSE(Value::opaque("x").fits(Type::OctetString));
    EXPECT_FALSE(Value::integer(1).fits(Type::Gauge32));
    EXPECT_FALSE(Value::gauge(1).fits(Type::Counter32));
    EXPECT_FALSE(Value::exception(Type::NoSuchInstance).fits(Type::Integer));
}

// --- VarBind --------------------------------------------------------------------------------------

TEST(VarBind, HoldsOidAndValue) {
    const VarBind vb{Oid({1, 3}), Value::integer(4)};
    EXPECT_EQ(vb.oid, Oid({1, 3}));
    EXPECT_EQ(vb.value, Value::integer(4));
}

}  // namespace
