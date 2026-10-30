// Index codec (index.hpp): encodeIndex / decodeIndex for every IndexKind.

#include <gtest/gtest.h>

#include <vector>

#include "snmpwrap/index.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;

namespace {
const std::vector<IndexSpec> kMulti{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
const std::vector<IndexSpec> kStr{IndexSpec::string(), IndexSpec::integer()};
}  // namespace

TEST(Index, CompositeRoundTrip) {
    const Oid enc = encodeIndex(kMulti, {Value::ipAddress(10, 0, 0, 1), Value::integer(80), Value::string("web")});
    EXPECT_EQ(enc, Oid({10, 0, 0, 1, 80, 'w', 'e', 'b'}));
    const auto dec = decodeIndex(kMulti, enc);
    ASSERT_TRUE(dec);
    ASSERT_EQ(dec->size(), 3u);
    EXPECT_EQ((*dec)[0], Value::ipAddress(10, 0, 0, 1));
    EXPECT_EQ((*dec)[1], Value::integer(80));
    EXPECT_EQ((*dec)[2], Value::string("web"));
}

TEST(Index, ImpliedStringMayBeEmpty) {  // the empty tail is still a complete index
    EXPECT_EQ(decodeIndex(kMulti, Oid({10, 0, 0, 1, 80}))->at(2), Value::string(""));
}

TEST(Index, MalformedCompositeIsRejected) {
    EXPECT_FALSE(decodeIndex(kMulti, Oid({10, 0, 0})));               // too short
    EXPECT_FALSE(decodeIndex(kMulti, Oid({10, 0, 256, 1, 80})));      // octet out of range
    EXPECT_FALSE(decodeIndex(kMulti, Oid({10, 0, 0, 1, 80, 300})));   // string octet out of range
}

TEST(Index, LengthPrefixedStringIsConsumedExactly) {
    EXPECT_EQ(encodeIndex(kStr, {Value::string("ab"), Value::integer(7)}), Oid({2, 'a', 'b', 7}));
    EXPECT_EQ(decodeIndex(kStr, Oid({2, 'a', 'b', 7}))->at(0), Value::string("ab"));
    EXPECT_EQ(decodeIndex(kStr, Oid({0, 7}))->at(0), Value::string(""));  // empty string
    EXPECT_FALSE(decodeIndex(kStr, Oid({3, 'a', 'b', 7})));                // length runs past the end
    EXPECT_FALSE(decodeIndex(kStr, Oid({2, 'a', 'b', 7, 9})));             // trailing sub-id
    EXPECT_FALSE(decodeIndex(kStr, Oid({2, 'a', 'b'})));                   // integer missing
    EXPECT_FALSE(decodeIndex(kStr, Oid({})));
}

TEST(Index, UnsignedAcceptsGaugeCounterAndTimeTicks) {
    EXPECT_EQ(encodeIndex({IndexSpec::unsignedInt()}, {Value::timeTicks(5)}), Oid({5}));
    EXPECT_EQ(encodeIndex({IndexSpec::unsignedInt()}, {Value::counter32(6)}), Oid({6}));
    EXPECT_THROW(encodeIndex({IndexSpec::unsignedInt()}, {Value::integer(1)}), Error);
}

TEST(Index, LargestOctetIs255) {
    EXPECT_TRUE(decodeIndex({IndexSpec::string()}, Oid({1, 255})).has_value());
    EXPECT_FALSE(decodeIndex({IndexSpec::string()}, Oid({1, 256})));
}

TEST(Index, FixedSizeString) {
    EXPECT_EQ(decodeIndex({IndexSpec::fixedString(2)}, Oid({'a', 'b'}))->at(0), Value::string("ab"));
    EXPECT_FALSE(decodeIndex({IndexSpec::fixedString(2)}, Oid({'a'})));
    EXPECT_THROW(encodeIndex({IndexSpec::fixedString(2)}, {Value::string("abc")}), Error);
}

TEST(Index, ObjectIdIsLengthPrefixed) {
    const std::vector<IndexSpec> oidSpec{IndexSpec::objectId(), IndexSpec::integer()};
    EXPECT_EQ(encodeIndex(oidSpec, {Value::oid(Oid({1, 3, 6})), Value::integer(1)}), Oid({3, 1, 3, 6, 1}));
    EXPECT_EQ(decodeIndex(oidSpec, Oid({3, 1, 3, 6, 1}))->at(0), Value::oid(Oid({1, 3, 6})));
}

TEST(Index, NegativeAndUnsignedIntegers) {
    EXPECT_EQ(decodeIndex({IndexSpec::integer()}, Oid({4294967295u}))->at(0), Value::integer(-1));
    EXPECT_EQ(encodeIndex({IndexSpec::integer()}, {Value::integer(-1)}), Oid({4294967295u}));
    EXPECT_EQ(decodeIndex({IndexSpec::unsignedInt()}, Oid({4294967295u}))->at(0), Value::gauge(4294967295u));
}

TEST(Index, InvalidLayoutsThrow) {
    EXPECT_THROW(decodeIndex({IndexSpec::impliedString(), IndexSpec::integer()}, Oid({1})), Error);  // IMPLIED not last
    EXPECT_THROW(encodeIndex({IndexSpec::integer()}, {}), Error);                                     // count mismatch
}
