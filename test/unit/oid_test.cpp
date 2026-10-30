// Oid (oid.hpp): parsing, order, prefixes and the index helpers indexString / indexIp.

#include <gtest/gtest.h>

#include "snmpwrap/index.hpp"
#include "snmpwrap/oid.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;

TEST(Oid, ParsesDottedText) {
    EXPECT_EQ(Oid::parse("1.3.6.1").str(), "1.3.6.1");
    EXPECT_EQ(Oid::parse(".1.3.6.1"), Oid({1, 3, 6, 1}));
    EXPECT_EQ(Oid::parse("1.4294967295")[1], 4294967295u);
}

TEST(Oid, RejectsMalformedText) {
    EXPECT_THROW(Oid::parse(""), Error);
    EXPECT_THROW(Oid::parse("1..3"), Error);
    EXPECT_THROW(Oid::parse("1.3."), Error);
    EXPECT_THROW(Oid::parse("1.a"), Error);
    EXPECT_THROW(Oid::parse("1.4294967296"), Error);
}

TEST(Oid, OrdersBySubIdNotByText) {  // GETNEXT order is lexicographic by sub-id, not by string
    EXPECT_LT(Oid::parse("1.3.6.1.2"), Oid::parse("1.3.6.1.10"));
    EXPECT_LT(Oid::parse("1.3.6"), Oid::parse("1.3.6.0"));
}

TEST(Oid, PrefixAndSuffix) {
    EXPECT_TRUE(Oid::parse("1.3.6.1").isPrefixOf(Oid::parse("1.3.6.1.4")));
    EXPECT_TRUE(Oid::parse("1.3.6.1").isPrefixOf(Oid::parse("1.3.6.1")));
    EXPECT_FALSE(Oid::parse("1.3.6.2").isPrefixOf(Oid::parse("1.3.6.1.4")));
    EXPECT_EQ(Oid::parse("1.3").suffixOf(Oid::parse("1.3.6.1")), Oid({6, 1}));
}

TEST(Oid, IndexHelpers) {
    EXPECT_EQ(indexString("ab"), Oid({2, 'a', 'b'}));
    EXPECT_EQ(indexString("ab", true), Oid({'a', 'b'}));
    EXPECT_EQ(indexIp(10, 0, 0, 1), Oid({10, 0, 0, 1}));
}
