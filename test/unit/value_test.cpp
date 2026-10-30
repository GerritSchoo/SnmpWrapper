// Value (value.hpp): typed accessors, comparison, text form and fits().

#include <gtest/gtest.h>

#include <string>

#include "snmpwrap/value.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;

TEST(Value, TypedAccessors) {
    EXPECT_EQ(Value::integer(-5).asInt(), -5);
    EXPECT_EQ(Value::gauge(7).asUInt(), 7u);
    EXPECT_EQ(Value::counter64(0x1122334455667788ULL).asUInt64(), 0x1122334455667788ULL);
    EXPECT_EQ(Value::integer(3).asUInt(), 3u);
    EXPECT_EQ(Value::string("abc").asString(), "abc");
}

TEST(Value, AccessorOfTheWrongTypeThrows) {
    EXPECT_THROW(Value::integer(-1).asUInt(), Error);
    EXPECT_THROW(Value::string("x").asInt(), Error);
}

TEST(Value, EqualityIncludesTheType) {
    EXPECT_NE(Value::integer(1), Value::gauge(1));
    EXPECT_EQ(Value::integer(1), Value::integer(1));
}

TEST(Value, ExceptionMarkers) {
    EXPECT_TRUE(Value::exception(Type::NoSuchInstance).isException());
    EXPECT_FALSE(Value::null().isException());
}

TEST(Value, OpaqueAndBitsKeepTheirTypeButShareTheStringAccessor) {
    EXPECT_EQ(Value::opaque("ab").type(), Type::Opaque);
    EXPECT_EQ(Value::opaque("ab").asString(), "ab");
    EXPECT_EQ(Value::bits(std::string("\xA0", 1)).type(), Type::Bits);
    EXPECT_NE(Value::opaque("x"), Value::string("x"));
    EXPECT_NE(Value::bits("x"), Value::opaque("x"));
    EXPECT_STREQ(toString(Type::Opaque), "Opaque");
    EXPECT_STREQ(toString(Type::Bits), "Bits");
    EXPECT_EQ(Value::bits(std::string("\xA0", 1)).str(), "Bits (hex): A0");
}

TEST(Value, FitsAcceptsBitsAsOctetStringOnly) {  // BITS arrive as OCTET STRING, nothing else is interchangeable
    EXPECT_TRUE(Value::string("x").fits(Type::Bits));
    EXPECT_TRUE(Value::bits("x").fits(Type::Bits));
    EXPECT_TRUE(Value::string("x").fits(Type::OctetString));
    EXPECT_FALSE(Value::bits("x").fits(Type::OctetString));
    EXPECT_FALSE(Value::opaque("x").fits(Type::Bits));
    EXPECT_FALSE(Value::integer(1).fits(Type::Gauge32));
}
