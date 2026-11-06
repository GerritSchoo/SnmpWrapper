// Unit tests of Oid.hpp / Oid.cpp: Oid (construction, parse, str, access, append, operators, prefix / suffix)
// and the index helpers indexInt, indexString, indexIp.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/Oid.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;

namespace {

// --- construction ---------------------------------------------------------------------------------

TEST(OidConstruction, DefaultIsEmpty) {
    const Oid o;
    EXPECT_TRUE(o.empty());
    EXPECT_EQ(o.size(), 0u);
    EXPECT_EQ(o.str(), "");
}

TEST(OidConstruction, FromInitializerList) {
    const Oid o{1, 3, 6};
    EXPECT_EQ(o.size(), 3u);
    EXPECT_EQ(o.ids(), (std::vector<SubId>{1, 3, 6}));
}

TEST(OidConstruction, FromVector) { EXPECT_EQ(Oid(std::vector<SubId>{1, 3}), Oid({1, 3})); }

// --- parse ----------------------------------------------------------------------------------------

struct ParseCase {
    const char* text;
    std::vector<SubId> ids;
};

class OidParseValid : public ::testing::TestWithParam<ParseCase> {};

TEST_P(OidParseValid, GivesTheSubIds) { EXPECT_EQ(Oid::parse(GetParam().text).ids(), GetParam().ids); }

INSTANTIATE_TEST_SUITE_P(Oid, OidParseValid,
                         ::testing::Values(ParseCase{"1.3.6.1", {1, 3, 6, 1}},
                                           ParseCase{".1.3.6.1", {1, 3, 6, 1}},  // leading dot as in Net-SNMP tools
                                           ParseCase{"0", {0}},
                                           ParseCase{"1.4294967295", {1, 4294967295u}},  // largest sub-id
                                           ParseCase{"1.0.0", {1, 0, 0}}));

class OidParseInvalid : public ::testing::TestWithParam<const char*> {};

TEST_P(OidParseInvalid, Throws) { EXPECT_THROW(Oid::parse(GetParam()), Error); }

INSTANTIATE_TEST_SUITE_P(Oid, OidParseInvalid,
                         ::testing::Values("", ".", "1..3", "1.3.", "1.a", "1.-3", " 1.3", "1.4294967296", "1.3 "));

// --- str ------------------------------------------------------------------------------------------

TEST(OidStr, DottedWithoutLeadingDot) { EXPECT_EQ(Oid({1, 3, 6, 1}).str(), "1.3.6.1"); }

TEST(OidStr, RoundTripsThroughParse) {
    const Oid o{1, 3, 6, 1, 4, 1, 4294967295u, 0};
    EXPECT_EQ(Oid::parse(o.str()), o);
}

// --- access ---------------------------------------------------------------------------------------

TEST(OidAccess, IndexOperatorAndIds) {
    const Oid o{7, 8, 9};
    EXPECT_EQ(o[0], 7u);
    EXPECT_EQ(o[2], 9u);
    EXPECT_EQ(o.ids().back(), 9u);
}

// --- append / operator+ ---------------------------------------------------------------------------

TEST(OidAppend, SubIdAndOidAreChainable) {
    Oid o{1};
    o.append(3).append(Oid({6, 1}));
    EXPECT_EQ(o, Oid({1, 3, 6, 1}));
}

TEST(OidAppend, PlusLeavesTheOperandsUnchanged) {
    const Oid a{1, 3};
    EXPECT_EQ(a + SubId{6}, Oid({1, 3, 6}));
    EXPECT_EQ(a + Oid({6, 1}), Oid({1, 3, 6, 1}));
    EXPECT_EQ(a, Oid({1, 3}));
}

TEST(OidAppend, EmptyOidIsNeutral) {
    EXPECT_EQ(Oid({1, 3}) + Oid{}, Oid({1, 3}));
    EXPECT_EQ(Oid{} + Oid({1, 3}), Oid({1, 3}));
}

// --- comparison: lexicographic by sub-id = SNMP order ---------------------------------------------

TEST(OidCompare, OrderIsBySubIdNotByText) {
    EXPECT_LT(Oid::parse("1.3.6.1.2"), Oid::parse("1.3.6.1.10"));  // as text "10" < "2"
}

TEST(OidCompare, APrefixSortsFirst) {
    EXPECT_LT(Oid({1, 3, 6}), Oid({1, 3, 6, 0}));
    EXPECT_LT(Oid{}, Oid({0}));
}

TEST(OidCompare, AllSixOperators) {
    const Oid a{1, 2}, b{1, 3};
    EXPECT_TRUE(a == a);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(a <= a && a <= b);
    EXPECT_TRUE(b >= b && b >= a);
    EXPECT_FALSE(b < a);
    EXPECT_FALSE(a > b);
}

// --- isPrefixOf / suffixOf ------------------------------------------------------------------------

TEST(OidPrefix, IsPrefixOf) {
    EXPECT_TRUE(Oid({1, 3, 6, 1}).isPrefixOf(Oid({1, 3, 6, 1, 4})));
    EXPECT_TRUE(Oid({1, 3, 6, 1}).isPrefixOf(Oid({1, 3, 6, 1})));  // an OID is a prefix of itself
    EXPECT_TRUE(Oid{}.isPrefixOf(Oid({1})));                       // the empty OID is a prefix of everything
    EXPECT_FALSE(Oid({1, 3, 6, 2}).isPrefixOf(Oid({1, 3, 6, 1, 4})));
    EXPECT_FALSE(Oid({1, 3, 6, 1, 4}).isPrefixOf(Oid({1, 3, 6, 1})));  // longer is never a prefix
}

TEST(OidSuffix, TheRestAfterThePrefix) {
    EXPECT_EQ(Oid({1, 3}).suffixOf(Oid({1, 3, 6, 1})), Oid({6, 1}));
    EXPECT_EQ(Oid({1, 3}).suffixOf(Oid({1, 3})), Oid{});
    EXPECT_EQ(Oid{}.suffixOf(Oid({1, 3})), Oid({1, 3}));
}

TEST(OidSuffix, OfAnOidThatDoesNotStartWithItThrows) {
    EXPECT_THROW(Oid({1, 4}).suffixOf(Oid({1, 3, 6})), Error);
    EXPECT_THROW(Oid({1, 3, 6}).suffixOf(Oid({1, 3})), Error);  // longer than the other one
}

// --- index helpers --------------------------------------------------------------------------------

TEST(OidIndexHelpers, IndexInt) { EXPECT_EQ(indexInt(4294967295u), Oid({4294967295u})); }

TEST(OidIndexHelpers, IndexStringIsLengthPrefixed) {
    EXPECT_EQ(indexString("ab"), Oid({2, 'a', 'b'}));
    EXPECT_EQ(indexString(""), Oid({0}));
}

TEST(OidIndexHelpers, IndexStringImpliedHasNoLength) {
    EXPECT_EQ(indexString("ab", true), Oid({'a', 'b'}));
    EXPECT_EQ(indexString("", true), Oid{});
}

TEST(OidIndexHelpers, IndexStringBytesAboveAsciiAreUnsigned) { EXPECT_EQ(indexString("\xff", true), Oid({255})); }

TEST(OidIndexHelpers, IndexIp) { EXPECT_EQ(indexIp(10, 0, 0, 255), Oid({10, 0, 0, 255})); }

}  // namespace
