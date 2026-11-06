// Unit tests of Index.hpp / Index.cpp: IndexSpec factories, encodeIndex and decodeIndex for every IndexKind
// (RFC 2578 7.7), malformed indexes and invalid layouts.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/Index.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;

namespace {

// --- IndexSpec ------------------------------------------------------------------------------------

TEST(IndexSpec, FactoriesSetKindAndSize) {
    EXPECT_EQ(IndexSpec::integer().kind, IndexKind::Integer);
    EXPECT_EQ(IndexSpec::unsignedInt().kind, IndexKind::Unsigned);
    EXPECT_EQ(IndexSpec::string().kind, IndexKind::String);
    EXPECT_EQ(IndexSpec::impliedString().kind, IndexKind::ImpliedString);
    EXPECT_EQ(IndexSpec::fixedString(6).kind, IndexKind::FixedString);
    EXPECT_EQ(IndexSpec::fixedString(6).size, 6u);
    EXPECT_EQ(IndexSpec::ipAddress().kind, IndexKind::IpAddress);
    EXPECT_EQ(IndexSpec::objectId().kind, IndexKind::ObjectId);
    EXPECT_EQ(IndexSpec::impliedObjectId().kind, IndexKind::ImpliedObjectId);
    EXPECT_EQ(IndexSpec::integer().size, 0u);
}

// --- encode and decode, one kind at a time --------------------------------------------------------

struct Coded {
    const char* what;
    IndexSpec spec;
    Value value;
    Oid encoded;
};

void PrintTo(const Coded& c, std::ostream* os) { *os << c.what; }

class IndexRoundTrip : public ::testing::TestWithParam<Coded> {};

TEST_P(IndexRoundTrip, Encodes) { EXPECT_EQ(encodeIndex({GetParam().spec}, {GetParam().value}), GetParam().encoded); }

TEST_P(IndexRoundTrip, DecodesBack) {
    const auto dec = decodeIndex({GetParam().spec}, GetParam().encoded);
    ASSERT_TRUE(dec);
    ASSERT_EQ(dec->size(), 1u);
    EXPECT_EQ(dec->front(), GetParam().value);
}

INSTANTIATE_TEST_SUITE_P(
    Index, IndexRoundTrip,
    ::testing::Values(Coded{"integer", IndexSpec::integer(), Value::integer(80), Oid({80})},
                      Coded{"negative integer wraps", IndexSpec::integer(), Value::integer(-1), Oid({4294967295u})},
                      Coded{"unsigned", IndexSpec::unsignedInt(), Value::gauge(4294967295u), Oid({4294967295u})},
                      Coded{"string", IndexSpec::string(), Value::string("ab"), Oid({2, 'a', 'b'})},
                      Coded{"empty string", IndexSpec::string(), Value::string(""), Oid({0})},
                      Coded{"octet 255", IndexSpec::string(), Value::string("\xff"), Oid({1, 255})},
                      Coded{"implied string", IndexSpec::impliedString(), Value::string("web"), Oid({'w', 'e', 'b'})},
                      Coded{"empty implied string", IndexSpec::impliedString(), Value::string(""), Oid{}},
                      Coded{"fixed string", IndexSpec::fixedString(2), Value::string("ab"), Oid({'a', 'b'})},
                      Coded{"ip address", IndexSpec::ipAddress(), Value::ipAddress(10, 0, 0, 1), Oid({10, 0, 0, 1})},
                      Coded{"object id", IndexSpec::objectId(), Value::oid(Oid({1, 3, 6})), Oid({3, 1, 3, 6})},
                      Coded{"implied object id", IndexSpec::impliedObjectId(), Value::oid(Oid({1, 3, 6})), Oid({1, 3, 6})}));

// --- several columns ------------------------------------------------------------------------------

const std::vector<IndexSpec> kConn{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};

TEST(IndexComposite, ColumnsFollowEachOther) {
    const Oid enc = encodeIndex(kConn, {Value::ipAddress(10, 0, 0, 1), Value::integer(80), Value::string("web")});
    EXPECT_EQ(enc, Oid({10, 0, 0, 1, 80, 'w', 'e', 'b'}));
    const auto dec = decodeIndex(kConn, enc);
    ASSERT_TRUE(dec);
    ASSERT_EQ(dec->size(), 3u);
    EXPECT_EQ((*dec)[0], Value::ipAddress(10, 0, 0, 1));
    EXPECT_EQ((*dec)[1], Value::integer(80));
    EXPECT_EQ((*dec)[2], Value::string("web"));
}

TEST(IndexComposite, LengthPrefixedStringIsConsumedExactly) {
    const std::vector<IndexSpec> spec{IndexSpec::string(), IndexSpec::integer()};
    EXPECT_EQ(encodeIndex(spec, {Value::string("ab"), Value::integer(7)}), Oid({2, 'a', 'b', 7}));
    EXPECT_EQ(decodeIndex(spec, Oid({2, 'a', 'b', 7}))->at(1), Value::integer(7));
}

TEST(IndexComposite, NoColumnsIsTheEmptyIndex) {
    EXPECT_EQ(encodeIndex({}, {}), Oid{});
    EXPECT_TRUE(decodeIndex({}, Oid{}).has_value());
    EXPECT_FALSE(decodeIndex({}, Oid({1})));  // trailing sub-id
}

// --- malformed indexes: decodeIndex returns nothing -----------------------------------------------

struct Malformed {
    const char* what;
    std::vector<IndexSpec> specs;
    Oid index;
};

void PrintTo(const Malformed& m, std::ostream* os) { *os << m.what; }

class IndexMalformed : public ::testing::TestWithParam<Malformed> {};

TEST_P(IndexMalformed, IsNotDecoded) { EXPECT_FALSE(decodeIndex(GetParam().specs, GetParam().index)); }

INSTANTIATE_TEST_SUITE_P(
    Index, IndexMalformed,
    ::testing::Values(Malformed{"integer missing", {IndexSpec::integer()}, Oid{}},
                      Malformed{"trailing sub-id", {IndexSpec::integer()}, Oid({1, 2})},
                      Malformed{"string length past the end", {IndexSpec::string()}, Oid({3, 'a', 'b'})},
                      Malformed{"string octet above 255", {IndexSpec::string()}, Oid({1, 256})},
                      Malformed{"string length missing", {IndexSpec::string()}, Oid{}},
                      Malformed{"implied string octet above 255", {IndexSpec::impliedString()}, Oid({300})},
                      Malformed{"fixed string too short", {IndexSpec::fixedString(2)}, Oid({'a'})},
                      Malformed{"ip address too short", {IndexSpec::ipAddress()}, Oid({10, 0, 0})},
                      Malformed{"ip address octet above 255", {IndexSpec::ipAddress()}, Oid({10, 0, 256, 1})},
                      Malformed{"object id length past the end", {IndexSpec::objectId()}, Oid({3, 1, 3})},
                      Malformed{"object id length missing", {IndexSpec::objectId()}, Oid{}},
                      Malformed{"composite too short", kConn, Oid({10, 0, 0})}));

// --- errors in what the caller passes: encodeIndex / decodeIndex throw ----------------------------

TEST(IndexErrors, ValueCountMustMatchTheColumns) {
    EXPECT_THROW(encodeIndex({IndexSpec::integer()}, {}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::integer()}, {Value::integer(1), Value::integer(2)}), Error);
}

TEST(IndexErrors, ValueTypeMustMatchTheKind) {
    EXPECT_THROW(encodeIndex({IndexSpec::integer()}, {Value::string("1")}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::unsignedInt()}, {Value::integer(1)}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::string()}, {Value::integer(1)}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::ipAddress()}, {Value::string("10.0.0.1")}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::objectId()}, {Value::string("1.3")}), Error);
}

TEST(IndexErrors, UnsignedAcceptsGaugeCounterAndTimeTicks) {
    EXPECT_EQ(encodeIndex({IndexSpec::unsignedInt()}, {Value::counter32(6)}), Oid({6}));
    EXPECT_EQ(encodeIndex({IndexSpec::unsignedInt()}, {Value::timeTicks(5)}), Oid({5}));
}

TEST(IndexErrors, FixedStringMustHaveItsSize) {
    EXPECT_THROW(encodeIndex({IndexSpec::fixedString(2)}, {Value::string("abc")}), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::fixedString(2)}, {Value::string("a")}), Error);
}

TEST(IndexErrors, ImpliedMustBeTheLastColumn) {
    EXPECT_THROW(decodeIndex({IndexSpec::impliedString(), IndexSpec::integer()}, Oid({1})), Error);
    EXPECT_THROW(decodeIndex({IndexSpec::impliedObjectId(), IndexSpec::integer()}, Oid({1})), Error);
    EXPECT_THROW(encodeIndex({IndexSpec::impliedString(), IndexSpec::integer()}, {Value::string("a"), Value::integer(1)}), Error);
}

}  // namespace
