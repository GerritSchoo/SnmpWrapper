// Mib (mib.hpp): GET, noSuchObject / noSuchInstance, GETNEXT order, SET with validation, atomicity and
// rollback, definition errors, multi-column indexes and large tables.

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "snmpwrap/mib.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;
using snmpwrap::test::FailsWith;
using snmpwrap::test::Succeeds;
using snmpwrap::test::trySet;

namespace {

const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999");

/// Three scalars (1.1 writable string, 1.2 writable Integer <= 100, 1.3 read-only Counter32) and table 2
/// without RowStatus: column 2 writable string ("commitfail" fails in the apply phase), column 3 read-only Gauge32.
class MibTest : public ::testing::Test {
protected:
    MibTest() {
        mib.scalar({1, 1}, {Type::OctetString, [this] { return Value::string(name); },
                            [this](const Value& v) { name = v.asString(); }});
        mib.scalar({1, 2}, {Type::Integer, [this] { return Value::integer(limit); },
                            [this](const Value& v) { limit = v.asInt(); },
                            [](const Value& v) {
                                if (v.asInt() > 100) throw SetError(ErrorStatus::WrongValue);
                            }});
        mib.scalar({1, 3}, {Type::Counter32, [] { return Value::counter32(1); }});  // read-only

        TableDef t;
        t.columns = {{2, Type::OctetString, Access::ReadWrite}, {3, Type::Gauge32, Access::ReadOnly}};
        t.rows = [this] {
            std::vector<Oid> out;
            for (auto& [i, r] : rows) out.push_back(indexInt(i));
            return out;
        };
        t.get = [this](const Oid& idx, SubId col) {
            auto& r = rows.at(idx[0]);
            return col == 2 ? Value::string(r.first) : Value::gauge(r.second);
        };
        t.set = [this](const Oid& idx, SubId, const Value& v) {
            if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected");
            rows.at(idx[0]).first = v.asString();
        };
        mib.table(2, std::move(t));
    }

    std::string name = "n";
    int limit = 10;
    std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> rows{{1, {"a", 10}}, {2, {"b", 20}}, {10, {"c", 30}}};
    Mib mib{kRoot};
};

// --- GET ----------------------------------------------------------------------------------------

TEST_F(MibTest, GetReturnsScalarsAndCells) {
    EXPECT_EQ(mib.get(kRoot + Oid({1, 1, 0})), Value::string("n"));
    EXPECT_EQ(mib.get(kRoot + Oid({1, 2, 0})), Value::integer(10));
    EXPECT_EQ(mib.get(kRoot + Oid({2, 1, 2, 1})), Value::string("a"));
    EXPECT_EQ(mib.get(kRoot + Oid({2, 1, 3, 10})), Value::gauge(30));
}

TEST_F(MibTest, GetOfNonInstancesIsEmpty) {
    EXPECT_FALSE(mib.get(kRoot + Oid({1, 1})));        // no .0 instance suffix
    EXPECT_FALSE(mib.get(kRoot + Oid({1, 9, 0})));     // unknown object
    EXPECT_FALSE(mib.get(kRoot + Oid({2, 1, 2, 3})));  // row 3 does not exist
    EXPECT_FALSE(mib.get(kRoot + Oid({2, 1, 9, 1})));  // column 9 does not exist
    EXPECT_FALSE(mib.get(Oid::parse("1.3.6.1.2.1.1.1.0")));
}

TEST_F(MibTest, MissingIsNoSuchInstanceWhenTheObjectExists) {
    EXPECT_EQ(mib.missing(kRoot + Oid({1, 1, 5})), Type::NoSuchInstance);      // scalar with wrong suffix
    EXPECT_EQ(mib.missing(kRoot + Oid({1, 1})), Type::NoSuchInstance);         // the scalar object itself
    EXPECT_EQ(mib.missing(kRoot + Oid({2, 1, 2, 99})), Type::NoSuchInstance);  // column exists, row does not
    EXPECT_EQ(mib.missing(kRoot + Oid({2, 1, 2})), Type::NoSuchInstance);      // column without index
}

TEST_F(MibTest, MissingIsNoSuchObjectOtherwise) {
    EXPECT_EQ(mib.missing(kRoot + Oid({1, 9, 0})), Type::NoSuchObject);
    EXPECT_EQ(mib.missing(kRoot + Oid({2, 1, 9, 1})), Type::NoSuchObject);  // unknown column
    EXPECT_EQ(mib.missing(kRoot + Oid({2, 2, 2, 1})), Type::NoSuchObject);  // not the entry (.1)
    EXPECT_EQ(mib.missing(kRoot + Oid({7})), Type::NoSuchObject);
    EXPECT_EQ(mib.missing(kRoot), Type::NoSuchObject);
    EXPECT_EQ(mib.missing(Oid::parse("1.3.6.1.2.1")), Type::NoSuchObject);
}

// --- GETNEXT ------------------------------------------------------------------------------------

TEST_F(MibTest, WalkIsStrictlyIncreasingAndComplete) {
    // from before the root: 3 scalars + 3 rows * 2 columns
    std::vector<Oid> seen;
    Oid cur = Oid::parse("1.3.6");
    while (auto nx = mib.getNext(cur)) {
        EXPECT_GT(nx->oid, cur);
        seen.push_back(nx->oid);
        cur = nx->oid;
    }
    ASSERT_EQ(seen.size(), 9u);
    EXPECT_EQ(seen.front(), kRoot + Oid({1, 1, 0}));
    EXPECT_EQ(seen.back(), kRoot + Oid({2, 1, 3, 10}));
    for (const Oid& o : seen) EXPECT_TRUE(mib.get(o).has_value()) << o.str() << " was walked but is not gettable";
}

TEST_F(MibTest, GetNextFromPositionsInsideTheTree) {
    EXPECT_EQ(mib.getNext(kRoot)->oid, kRoot + Oid({1, 1, 0}));                       // exactly the root
    EXPECT_EQ(mib.getNext(kRoot + Oid({1}))->oid, kRoot + Oid({1, 1, 0}));              // inside, before first scalar
    EXPECT_EQ(mib.getNext(kRoot + Oid({1, 1}))->oid, kRoot + Oid({1, 1, 0}));           // the object OID itself
    EXPECT_EQ(mib.getNext(kRoot + Oid({1, 1, 0}))->oid, kRoot + Oid({1, 2, 0}));
    EXPECT_EQ(mib.getNext(kRoot + Oid({1, 1, 0, 5}))->oid, kRoot + Oid({1, 2, 0}));     // past the instance
    EXPECT_EQ(mib.getNext(kRoot + Oid({1, 3, 0}))->oid, kRoot + Oid({2, 1, 2, 1}));     // scalar -> table
    EXPECT_EQ(mib.getNext(kRoot + Oid({2}))->oid, kRoot + Oid({2, 1, 2, 1}));
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1}))->oid, kRoot + Oid({2, 1, 2, 1}));
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 2}))->oid, kRoot + Oid({2, 1, 2, 1}));
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 1, 5}))->oid, kRoot + Oid({2, 1, 2, 1}));  // column 1 does not exist
}

TEST_F(MibTest, GetNextInsideATable) {
    // index 10 sorts after index 2 (numeric, not textual)
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 2, 2}))->oid, kRoot + Oid({2, 1, 2, 10}));
    // from the last row of a column to the first row of the next column
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 2, 10}))->oid, kRoot + Oid({2, 1, 3, 1}));
    // from a non-existing instance between existing ones
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 2, 5}))->oid, kRoot + Oid({2, 1, 2, 10}));
    EXPECT_EQ(mib.getNext(kRoot + Oid({2, 1, 2, 1, 7}))->oid, kRoot + Oid({2, 1, 2, 2}));  // longer than any index
}

TEST_F(MibTest, GetNextAtTheEndAndOutsideTheSubtree) {
    EXPECT_FALSE(mib.getNext(kRoot + Oid({2, 1, 3, 10})));
    EXPECT_FALSE(mib.getNext(kRoot + Oid({2, 1, 4})));  // column 4 does not exist, nothing after
    EXPECT_FALSE(mib.getNext(kRoot + Oid({9})));
    EXPECT_FALSE(mib.getNext(Oid::parse("1.3.6.1.4.1.100000")));
    EXPECT_EQ(mib.getNext(Oid::parse("1.3.6.1.2.1.1.1.0"))->oid, kRoot + Oid({1, 1, 0}));  // before the root
}

// --- SET ----------------------------------------------------------------------------------------

TEST_F(MibTest, SetWritesScalarsAndCells) {
    EXPECT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}})));
    EXPECT_EQ(name, "x");
    EXPECT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({2, 1, 2, 2}), Value::string("renamed")}})));
    EXPECT_EQ(rows.at(2).first, "renamed");
}

TEST_F(MibTest, SetIsRejectedWithTheMatchingStatus) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::integer(1)}}), ErrorStatus::WrongType, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({1, 2, 0}), Value::integer(101)}}), ErrorStatus::WrongValue, 0));  // validator
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({1, 3, 0}), Value::counter32(1)}}), ErrorStatus::NotWritable, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({2, 1, 3, 1}), Value::gauge(1)}}), ErrorStatus::NotWritable, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({2, 1, 2, 99}), Value::string("z")}}), ErrorStatus::NoCreation, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({9, 9, 0}), Value::integer(1)}}), ErrorStatus::NoCreation, 0));
}

TEST_F(MibTest, InvalidSecondVarbindStopsTheWholeRequest) {  // validated before anything is applied
    const auto e = trySet(mib, {{kRoot + Oid({1, 2, 0}), Value::integer(60)}, {kRoot + Oid({1, 1, 0}), Value::integer(5)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::WrongType, 1));
    EXPECT_EQ(limit, 10);
}

TEST_F(MibTest, ValidatorFailureOnTheThirdVarbind) {
    const auto e = trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("q")},
                                {kRoot + Oid({2, 1, 2, 1}), Value::string("w")},
                                {kRoot + Oid({1, 2, 0}), Value::integer(500)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::WrongValue, 2));
    EXPECT_EQ(name, "n");
    EXPECT_EQ(rows.at(1).first, "a");
}

TEST_F(MibTest, FailureInTheApplyPhaseRollsEverythingBack) {
    auto e = trySet(mib, {{kRoot + Oid({1, 2, 0}), Value::integer(60)},
                          {kRoot + Oid({1, 1, 0}), Value::string("changed")},
                          {kRoot + Oid({2, 1, 2, 1}), Value::string("commitfail")}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::CommitFailed, 2));  // the index points at the culprit
    EXPECT_EQ(limit, 10);
    EXPECT_EQ(name, "n");
    EXPECT_EQ(rows.at(1).first, "a");

    // a failed apply of the very first op leaves nothing behind either
    e = trySet(mib, {{kRoot + Oid({2, 1, 2, 1}), Value::string("commitfail")}, {kRoot + Oid({1, 2, 0}), Value::integer(70)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::CommitFailed, 0));
    EXPECT_EQ(limit, 10);

    // the same PDU without the bad value succeeds completely
    EXPECT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({1, 2, 0}), Value::integer(60)}, {kRoot + Oid({2, 1, 2, 1}), Value::string("ok")}})));
    EXPECT_EQ(limit, 60);
    EXPECT_EQ(rows.at(1).first, "ok");
}

TEST(Mib, OtherExceptionsInTheSetterBecomeCommitFailed) {
    Mib m(kRoot);
    int boom = 0;
    m.scalar({1}, {Type::Integer, [&] { return Value::integer(boom); }, [&](const Value&) { throw std::runtime_error("disk"); },
                   [](const Value&) {}});
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 0}), Value::integer(1)}}), ErrorStatus::CommitFailed, 0));
}

// --- definitions --------------------------------------------------------------------------------

TEST(Mib, OverlappingOrIncompleteScalarsAreRefused) {
    Mib m(kRoot);
    ScalarDef s{Type::Integer, [] { return Value::integer(1); }};
    m.scalar({1, 1}, s);
    EXPECT_THROW(m.scalar({1, 1}, s), Error);         // duplicate
    EXPECT_THROW(m.scalar({1, 1, 5}, s), Error);      // below an existing scalar
    EXPECT_THROW(m.scalar({1}, s), Error);            // above an existing scalar
    EXPECT_THROW(m.scalar({7}, ScalarDef{}), Error);  // getter missing
}

TEST(Mib, TablesNeedRowsOrNextRowWithHasRow) {
    Mib m(kRoot);
    EXPECT_THROW(m.table(5, TableDef{}), Error);  // no callbacks / columns
    TableDef noRows;
    noRows.columns = {{2, Type::Integer, Access::ReadOnly}};
    noRows.get = [](const Oid&, SubId) { return Value::integer(1); };
    EXPECT_THROW(m.table(6, noRows), Error);  // neither rows nor nextRow+hasRow
    noRows.nextRow = [](const Oid*) { return std::optional<Oid>(); };
    EXPECT_THROW(m.table(6, noRows), Error);  // nextRow alone is not enough (no hasRow)
    noRows.hasRow = [](const Oid&) { return false; };
    EXPECT_NO_THROW(m.table(6, noRows));

    TableDef badIdx = noRows;
    badIdx.indexes = {IndexSpec::impliedString(), IndexSpec::integer()};
    EXPECT_THROW(m.table(8, badIdx), Error);  // IMPLIED not last
}

TEST(Mib, GetterReturningTheWrongTypeIsAnError) {  // a programming error, reported as Error
    Mib m(kRoot);
    m.scalar({2, 1}, ScalarDef{Type::Integer, [] { return Value::string("oops"); }});
    EXPECT_THROW(m.get(kRoot + Oid({2, 1, 0})), Error);
}

// --- multi-column index (ipAddress, integer, IMPLIED string) ------------------------------------

class MibMultiIndexTest : public ::testing::Test {
protected:
    MibMultiIndexTest() {
        state[key(1, 80, "web")] = 1;
        state[key(1, 80, "web2")] = 2;
        state[key(1, 443, "tls")] = 3;
        state[key(2, 22, "ssh")] = 4;
        state[key(1, 80, "")] = 5;  // empty implied string

        TableDef t;
        t.indexes = spec;
        t.columns = {{5, Type::Integer, Access::ReadWrite}};
        t.rows = [this] {
            std::vector<Oid> r;
            for (auto& [k, v] : state) r.push_back(k);
            return r;
        };
        t.get = [this](const Oid& i, SubId) { return Value::integer(state.at(i)); };
        t.set = [this](const Oid& i, SubId, const Value& v) { state.at(i) = v.asInt(); };
        state[Oid({9, 9})] = 99;  // a malformed row that the application (wrongly) reports must be ignored
        m.table(3, std::move(t));
    }

    Oid key(std::uint8_t d, int port, const char* tag) const {
        return encodeIndex(spec, {Value::ipAddress(10, 0, 0, d), Value::integer(port), Value::string(tag)});
    }
    Oid cellOf(const Oid& index) const { return kRoot + Oid({3, 1, 5}) + index; }

    const std::vector<IndexSpec> spec{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
    std::map<Oid, std::int32_t> state;
    Mib m{kRoot};
};

TEST_F(MibMultiIndexTest, Get) {
    EXPECT_EQ(m.get(cellOf(key(1, 80, "web"))), Value::integer(1));
    EXPECT_EQ(m.get(cellOf(key(1, 80, ""))), Value::integer(5));
    EXPECT_FALSE(m.get(cellOf(key(1, 81, "web"))));
    EXPECT_FALSE(m.get(kRoot + Oid({3, 1, 5, 9, 9})));  // malformed index is not an instance
    EXPECT_EQ(m.missing(kRoot + Oid({3, 1, 5, 9, 9})), Type::NoSuchInstance);
}

TEST_F(MibMultiIndexTest, WalkIsInOidOrderAndSkipsMalformedRows) {
    std::vector<Oid> order;
    Oid cur = kRoot;
    while (auto nx = m.getNext(cur)) {
        order.push_back(nx->oid);
        cur = nx->oid;
    }
    // 10.0.0.1:80 "" < "web" < "web2", then :443, then 10.0.0.2
    const std::vector<Oid> expected{cellOf(key(1, 80, "")), cellOf(key(1, 80, "web")), cellOf(key(1, 80, "web2")),
                                    cellOf(key(1, 443, "tls")), cellOf(key(2, 22, "ssh"))};
    EXPECT_EQ(order, expected);
}

TEST_F(MibMultiIndexTest, Set) {
    EXPECT_TRUE(Succeeds(trySet(m, {{cellOf(key(2, 22, "ssh")), Value::integer(40)}})));
    EXPECT_EQ(state.at(key(2, 22, "ssh")), 40);
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({3, 1, 5, 9, 9}), Value::integer(1)}}), ErrorStatus::NoCreation, 0));
}

// --- large tables -------------------------------------------------------------------------------

TEST(Mib, LargeTableWithNextRowNeverEnumeratesAllRows) {
    constexpr std::uint32_t N = 100000;
    std::map<std::uint32_t, std::uint32_t> data;
    for (std::uint32_t i = 1; i <= N; ++i) data[i * 2] = i;  // even indexes only

    int rowsCalls = 0;
    Mib m(kRoot);
    TableDef t;
    t.indexes = {IndexSpec::unsignedInt()};
    t.columns = {{2, Type::Gauge32, Access::ReadOnly}, {3, Type::Gauge32, Access::ReadOnly}};
    t.rows = [&] {
        ++rowsCalls;
        std::vector<Oid> r;
        for (auto& [k, v] : data) r.push_back(Oid({k}));
        return r;
    };
    t.hasRow = [&](const Oid& i) { return i.size() == 1 && data.count(i[0]) > 0; };
    t.nextRow = [&](const Oid* after) -> std::optional<Oid> {
        auto it = after ? data.upper_bound((*after)[0]) : data.begin();  // single-sub-id indexes only here
        if (after && after->size() != 1) it = after->empty() ? data.begin() : data.upper_bound((*after)[0]);
        if (it == data.end()) return std::nullopt;
        return Oid({it->first});
    };
    t.get = [&](const Oid& i, SubId c) { return Value::gauge(data.at(i[0]) + c); };
    m.table(1, std::move(t));

    std::size_t count = 0;
    Oid cur = kRoot;
    bool ascending = true;
    while (auto nx = m.getNext(cur)) {
        ascending = ascending && nx->oid > cur;
        cur = nx->oid;
        ++count;
    }
    EXPECT_TRUE(ascending);
    EXPECT_EQ(count, 2u * N);
    EXPECT_EQ(rowsCalls, 0);  // never enumerated the whole table

    EXPECT_EQ(m.getNext(kRoot + Oid({1, 1, 2, 4}))->oid, kRoot + Oid({1, 1, 2, 6}));
    EXPECT_EQ(m.getNext(kRoot + Oid({1, 1, 2, 5}))->oid, kRoot + Oid({1, 1, 2, 6}));      // between rows
    EXPECT_EQ(m.getNext(kRoot + Oid({1, 1, 2, 2 * N}))->oid, kRoot + Oid({1, 1, 3, 2}));  // column boundary
    EXPECT_FALSE(m.getNext(kRoot + Oid({1, 1, 3, 2 * N})));
    EXPECT_EQ(m.get(kRoot + Oid({1, 1, 3, 200})), Value::gauge(103));
    EXPECT_FALSE(m.get(kRoot + Oid({1, 1, 3, 201})));
    EXPECT_EQ(rowsCalls, 0);  // get() used hasRow as well
}

TEST(Mib, FallbackLoadsRowsAtMostOncePerGetNext) {
    std::map<std::uint32_t, int> data;
    for (std::uint32_t i = 1; i <= 500; ++i) data[i] = static_cast<int>(i);
    int rowsCalls = 0;
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}, {3, Type::Integer, Access::ReadOnly}, {4, Type::Integer, Access::ReadOnly}};
    t.rows = [&] {
        ++rowsCalls;
        std::vector<Oid> r;
        for (auto it = data.rbegin(); it != data.rend(); ++it) r.push_back(Oid({it->first}));  // deliberately unsorted
        return r;
    };
    t.get = [&](const Oid& i, SubId c) { return Value::integer(data.at(i[0]) + static_cast<int>(c)); };
    m.table(1, std::move(t));

    Oid cur = kRoot;
    std::size_t count = 0;
    bool ok = true;
    for (;;) {
        const int before = rowsCalls;
        auto nx = m.getNext(cur);
        ok = ok && rowsCalls - before <= 1;  // incl. column transitions
        if (!nx) break;
        ok = ok && nx->oid > cur;
        cur = nx->oid;
        ++count;
    }
    EXPECT_TRUE(ok);
    EXPECT_EQ(count, 1500u);
}

}  // namespace
