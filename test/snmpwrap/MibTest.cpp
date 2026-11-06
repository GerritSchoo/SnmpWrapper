// Unit tests of Mib.hpp / Mib.cpp: the IHandler defaults, Mib as a IMibBuilder (definitions, hooks), GET,
// noSuchObject / noSuchInstance, GETNEXT order, SET with validation, atomicity and rollback, RowStatus (RFC 2579),
// definition errors, multi-column indexes and large tables.

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "snmpwrap/Mib.hpp"
#include "snmpwrap/IHandlerMock.hpp"
#include "snmpwrap/IMibBuilderMock.hpp"
#include "TestSupport.hpp"

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

TEST(Mib, GetterThatThrowsDuringGetNextLeaksNothing) {  // run under the asan preset: GCC < 13 leaked the OID here
    Mib m(kRoot);
    m.scalar({1}, ScalarDef{Type::Integer, []() -> Value { throw Error("broken scalar"); }});
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.rows = [] { return std::vector<Oid>{Oid({1})}; };
    t.get = [](const Oid&, SubId) -> Value { throw Error("broken cell"); };
    m.table(2, std::move(t));
    EXPECT_THROW(m.getNext(kRoot), Error);                      // scalar
    EXPECT_THROW(m.getNext(kRoot + Oid({1, 0})), Error);       // table cell
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


// =================================================================================================
// RowStatus (RFC 2579)
// =================================================================================================

/// Table 4: index Integer, column 2 name (required, "commitfail" fails in apply), column 3 value (<= 1000),
/// column 4 RowStatus. A row is complete once it has a name.
class RowStatusTest : public ::testing::Test {
protected:
    struct Row {
        std::string name;
        std::uint32_t value = 0;
        RowStatus status = RowStatus::NotReady;
    };
    static constexpr SubId kName = 2, kValue = 3, kStatus = 4;

    explicit RowStatusTest(bool rejectEditWhileActive = false) {
        TableDef t;
        t.indexes = {IndexSpec::integer()};
        t.columns = {{kName, Type::OctetString, Access::ReadWrite}, {kValue, Type::Gauge32, Access::ReadWrite}};
        t.rows = [this] {
            std::vector<Oid> r;
            for (auto& [k, v] : rows) r.push_back(Oid({k}));
            return r;
        };
        t.get = [this](const Oid& i, SubId c) {
            auto& r = rows.at(i[0]);
            return c == kName ? Value::string(r.name) : Value::gauge(r.value);
        };
        t.set = [this](const Oid& i, SubId c, const Value& v) {
            auto& r = rows.at(i[0]);
            if (c == kName) {
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected");
                r.name = v.asString();
            } else {
                r.value = v.asUInt();
            }
        };
        t.validate = [](const Oid&, SubId c, const Value& v) {
            if (c == kValue && v.asUInt() > 1000) throw SetError(ErrorStatus::WrongValue, "too big");
        };
        RowStatusSpec rs;
        rs.column = kStatus;
        rs.requiredColumns = {kName};
        rs.create = [this](const Oid& i, const std::map<SubId, Value>& cols) {
            Row r;
            if (auto n = cols.find(kName); n != cols.end()) r.name = n->second.asString();
            if (auto v = cols.find(kValue); v != cols.end()) r.value = v->second.asUInt();
            rows[i[0]] = r;
        };
        rs.destroy = [this](const Oid& i) {
            ++destroyCalls;
            rows.erase(i[0]);
        };
        rs.setState = [this](const Oid& i, RowStatus s) { rows.at(i[0]).status = s; };
        rs.state = [this](const Oid& i) { return rows.at(i[0]).status; };
        rs.complete = [this](const Oid& i) { return !rows.at(i[0]).name.empty(); };
        rs.rejectEditWhileActive = rejectEditWhileActive;
        t.rowStatus = rs;
        mib.table(4, std::move(t));
    }

    static Oid cell(SubId col, std::uint32_t idx) { return kRoot + Oid({4, 1, col, idx}); }
    static Value st(RowStatus s) { return Value::integer(static_cast<int>(s)); }
    /// createAndGo of row @p idx with name @p name.
    std::optional<SetError> createRow(std::uint32_t idx, const std::string& name) {
        return trySet(mib, {{cell(kName, idx), Value::string(name)}, {cell(kStatus, idx), st(RowStatus::CreateAndGo)}});
    }

    std::map<std::uint32_t, Row> rows;
    int destroyCalls = 0;
    Mib mib{kRoot};
};

/// The same table with rejectEditWhileActive (RFC 2579 behaviour).
class RowStatusStrictTest : public RowStatusTest {
protected:
    RowStatusStrictTest() : RowStatusTest(true) {}
};

// --- createAndGo --------------------------------------------------------------------------------

TEST_F(RowStatusTest, CreateAndGoNeedsTheRequiredColumns) {
    const auto e = trySet(mib, {{cell(kValue, 5), Value::gauge(1)}, {cell(kStatus, 5), st(RowStatus::CreateAndGo)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::InconsistentValue, 1));
    EXPECT_TRUE(rows.empty());
}

TEST_F(RowStatusTest, CreateAndGoCreatesAnActiveRow) {
    ASSERT_TRUE(Succeeds(createRow(5, "eth5")));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.at(5).name, "eth5");
    EXPECT_EQ(rows.at(5).status, RowStatus::Active);
    EXPECT_EQ(mib.get(cell(kStatus, 5)), st(RowStatus::Active));
    EXPECT_EQ(mib.get(cell(kName, 5)), Value::string("eth5"));
}

TEST_F(RowStatusTest, StatusColumnIsWalkedAfterTheOtherColumns) {
    ASSERT_TRUE(Succeeds(createRow(5, "eth5")));
    EXPECT_EQ(mib.getNext(cell(kValue, 5))->oid, cell(kStatus, 5));
    EXPECT_FALSE(mib.getNext(cell(kStatus, 5)));
}

TEST_F(RowStatusTest, CreateOnAnExistingRowIsInconsistent) {
    ASSERT_TRUE(Succeeds(createRow(5, "eth5")));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 5), st(RowStatus::CreateAndGo)}}), ErrorStatus::InconsistentValue, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 5), st(RowStatus::CreateAndWait)}}), ErrorStatus::InconsistentValue, 0));
}

// --- createAndWait ------------------------------------------------------------------------------

TEST_F(RowStatusTest, CreateAndWaitWithoutRequiredColumnIsNotReady) {
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 7), st(RowStatus::CreateAndWait)}})));
    EXPECT_EQ(rows.at(7).status, RowStatus::NotReady);  // name missing -> incomplete
    EXPECT_EQ(mib.get(cell(kStatus, 7)), st(RowStatus::NotReady));

    // activating an incomplete row fails in the apply phase and changes nothing
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 7), st(RowStatus::Active)}}), ErrorStatus::InconsistentValue, 0));
    EXPECT_EQ(rows.at(7).status, RowStatus::NotReady);
}

TEST_F(RowStatusTest, SupplyingTheRequiredColumnPromotesToNotInService) {
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 7), st(RowStatus::CreateAndWait)}})));
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 7), Value::string("x")}})));
    EXPECT_EQ(rows.at(7).status, RowStatus::NotInService);

    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 7), st(RowStatus::Active)}})));
    EXPECT_EQ(rows.at(7).status, RowStatus::Active);
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 7), st(RowStatus::NotInService)}})));
    EXPECT_EQ(rows.at(7).status, RowStatus::NotInService);
}

TEST_F(RowStatusTest, NameAndActiveInOneRequestActivateANotReadyRow) {
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 8), st(RowStatus::CreateAndWait)}})));
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 8), Value::string("y")}, {cell(kStatus, 8), st(RowStatus::Active)}})));
    EXPECT_EQ(rows.at(8).status, RowStatus::Active);
}

TEST_F(RowStatusTest, CreateAndWaitWithAllColumnsIsCompleteRightAway) {
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 9), Value::string("z")}, {cell(kStatus, 9), st(RowStatus::CreateAndWait)}})));
    EXPECT_EQ(rows.at(9).status, RowStatus::NotInService);
}

// --- destroy ------------------------------------------------------------------------------------

TEST_F(RowStatusTest, DestroyOfANonExistingRowIsANoOp) {
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 99), st(RowStatus::Destroy)}})));
    EXPECT_EQ(destroyCalls, 0);
}

TEST_F(RowStatusTest, DestroyHappensOnlyInCommit) {  // irreversible
    ASSERT_TRUE(Succeeds(createRow(1, "a")));
    auto txn = mib.prepare({{cell(kStatus, 1), st(RowStatus::Destroy)}});
    txn->apply();
    EXPECT_EQ(rows.count(1), 1u);  // still there after apply
    txn->commit();
    EXPECT_EQ(rows.count(1), 0u);
    EXPECT_EQ(destroyCalls, 1);
}

TEST_F(RowStatusTest, DestroyIsSkippedWhenAnotherPartOfTheRequestFails) {
    ASSERT_TRUE(Succeeds(createRow(1, "a")));
    ASSERT_TRUE(Succeeds(createRow(2, "b")));
    const auto e = trySet(mib, {{cell(kStatus, 1), st(RowStatus::Destroy)}, {cell(kName, 2), Value::string("commitfail")}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::CommitFailed, 1));
    EXPECT_EQ(rows.count(1), 1u);
    EXPECT_EQ(rows.count(2), 1u);
}

// --- errors -------------------------------------------------------------------------------------

TEST_F(RowStatusTest, CellOfANonExistingRowWithoutCreateIsInconsistentName) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kName, 3), Value::string("x")}}), ErrorStatus::InconsistentName, 0));
}

TEST_F(RowStatusTest, ActiveOnANonExistingRowIsInconsistentValue) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 3), st(RowStatus::Active)}}), ErrorStatus::InconsistentValue, 0));
}

TEST_F(RowStatusTest, InvalidRowStatusValues) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 3), Value::integer(9)}}), ErrorStatus::WrongValue, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 3), Value::integer(0)}}), ErrorStatus::WrongValue, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 3), st(RowStatus::NotReady)}}), ErrorStatus::WrongValue, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kStatus, 3), Value::string("x")}}), ErrorStatus::WrongType, 0));
}

TEST_F(RowStatusTest, ColumnValidatorAppliesDuringCreation) {
    const auto e = trySet(mib, {{cell(kName, 3), Value::string("x")}, {cell(kValue, 3), Value::gauge(5000)},
                                {cell(kStatus, 3), st(RowStatus::CreateAndGo)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::WrongValue, 1));
    EXPECT_TRUE(rows.empty());
}

TEST_F(RowStatusTest, UnknownColumnAndWrongType) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{kRoot + Oid({4, 1, 9, 3}), Value::integer(1)}}), ErrorStatus::NoCreation, 0));
    ASSERT_TRUE(Succeeds(createRow(3, "x")));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kName, 3), Value::integer(1)}}), ErrorStatus::WrongType, 0));
}

// --- rollback -----------------------------------------------------------------------------------

TEST_F(RowStatusTest, FailedRequestRemovesTheRowItCreated) {
    ASSERT_TRUE(Succeeds(createRow(1, "keep")));
    // create row 5 and modify row 1 in one request; the modification fails in apply -> row 5 must vanish again
    const auto e = trySet(mib, {{cell(kName, 5), Value::string("new")},
                                {cell(kStatus, 5), st(RowStatus::CreateAndGo)},
                                {cell(kValue, 1), Value::gauge(77)},
                                {cell(kName, 1), Value::string("commitfail")}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::CommitFailed, 3));
    EXPECT_EQ(rows.count(5), 0u);
    EXPECT_EQ(rows.at(1).value, 0u);
    EXPECT_EQ(rows.at(1).name, "keep");
}

TEST_F(RowStatusTest, FailedRequestUndoesTheNotReadyPromotion) {
    ASSERT_TRUE(Succeeds(createRow(1, "keep")));
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(kStatus, 6), st(RowStatus::CreateAndWait)}})));
    const auto e = trySet(mib, {{cell(kName, 6), Value::string("six")}, {cell(kName, 1), Value::string("commitfail")}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::CommitFailed, 1));
    EXPECT_TRUE(rows.at(6).name.empty());
    EXPECT_EQ(rows.at(6).status, RowStatus::NotReady);
}

// --- editing active rows ------------------------------------------------------------------------

TEST_F(RowStatusTest, ActiveRowsStayEditableByDefault) {
    ASSERT_TRUE(Succeeds(createRow(1, "a")));
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 1), Value::string("b")}})));
}

TEST_F(RowStatusStrictTest, EditOfAnActiveRowIsRefused) {
    ASSERT_TRUE(Succeeds(createRow(1, "a")));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(kName, 1), Value::string("b")}}), ErrorStatus::InconsistentValue, 0));
    EXPECT_EQ(rows.at(1).name, "a");
}

TEST_F(RowStatusStrictTest, EditTogetherWithNotInServiceIsAllowed) {
    ASSERT_TRUE(Succeeds(createRow(1, "a")));
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 1), Value::string("b")}, {cell(kStatus, 1), st(RowStatus::NotInService)}})));
    EXPECT_EQ(rows.at(1).name, "b");
    EXPECT_EQ(rows.at(1).status, RowStatus::NotInService);
    // a not-in-service row can be edited on its own
    EXPECT_TRUE(Succeeds(trySet(mib, {{cell(kName, 1), Value::string("c")}})));
    EXPECT_EQ(rows.at(1).name, "c");
}

// --- malformed index (Integer + length-prefixed string) -----------------------------------------

TEST(RowStatus, MalformedIndexRefusesCreation) {
    std::map<Oid, int> rows;
    const std::vector<IndexSpec> spec{IndexSpec::integer(), IndexSpec::string()};
    Mib m(kRoot);
    TableDef t;
    t.indexes = spec;
    t.columns = {{2, Type::Integer, Access::ReadWrite}};
    t.rows = [&] {
        std::vector<Oid> r;
        for (auto& [k, v] : rows) r.push_back(k);
        return r;
    };
    t.get = [&](const Oid& i, SubId) { return Value::integer(rows.at(i)); };
    t.set = [&](const Oid& i, SubId, const Value& v) { rows.at(i) = v.asInt(); };
    RowStatusSpec rs;
    rs.column = 3;
    rs.create = [&](const Oid& i, const std::map<SubId, Value>&) { rows[i] = 0; };
    rs.destroy = [&](const Oid& i) { rows.erase(i); };
    rs.setState = [](const Oid&, RowStatus) {};
    rs.state = [](const Oid&) { return RowStatus::Active; };
    t.rowStatus = rs;
    m.table(1, std::move(t));

    const Oid good = encodeIndex(spec, {Value::integer(1), Value::string("ab")});
    EXPECT_TRUE(Succeeds(trySet(m, {{kRoot + Oid({1, 1, 3}) + good, Value::integer(4)}})));
    EXPECT_EQ(rows.count(good), 1u);
    // trailing garbage / truncated string -> not a valid index
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 1, 3, 1, 2, 'a', 'b', 9}), Value::integer(4)}}), ErrorStatus::InconsistentName, 0));
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 1, 3, 1, 5, 'a'}), Value::integer(4)}}), ErrorStatus::InconsistentName, 0));
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 1, 3}), Value::integer(4)}}), ErrorStatus::NoCreation, 0));  // no index at all
    EXPECT_EQ(rows.size(), 1u);
}


// =================================================================================================
// IHandler: the defaults of the interface
// =================================================================================================

/// A IHandler that only implements the pure virtual functions.
class MinimalHandler : public IHandler {
public:
    std::optional<Value> get(const Oid&) override { return std::nullopt; }
    std::optional<VarBind> getNext(const Oid&) override { return std::nullopt; }
};

TEST(HandlerDefaults, MissingIsNoSuchInstance) {
    MinimalHandler h;
    EXPECT_EQ(h.missing(Oid({1, 3})), Type::NoSuchInstance);
}

TEST(HandlerDefaults, PrepareRefusesEverySetAsNotWritable) {
    MinimalHandler h;
    EXPECT_TRUE(FailsWith(trySet(h, {{Oid({1, 3}), Value::integer(1)}}), ErrorStatus::NotWritable, 0));
}

// =================================================================================================
// Mib as a IMibBuilder: root, definitions, request hooks
// =================================================================================================

TEST(IMibBuilder, RootIsTheConstructorArgument) {
    Mib m(kRoot);
    EXPECT_EQ(m.root(), kRoot);
    const IMibBuilder& b = m;  // through the interface
    EXPECT_EQ(b.root(), kRoot);
}

TEST(IMibBuilder, DefinitionsChain) {
    Mib m(kRoot);
    IMibBuilder& b = m;
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.rows = [] { return std::vector<Oid>{Oid({1})}; };
    t.get = [](const Oid&, SubId) { return Value::integer(5); };
    b.scalar(Oid({1}), {Type::Integer, [] { return Value::integer(1); }}).table(Oid({2}), std::move(t));
    EXPECT_EQ(m.get(kRoot + Oid({1, 0})), Value::integer(1));
    EXPECT_EQ(m.get(kRoot + Oid({2, 1, 2, 1})), Value::integer(5));
}

TEST(IMibBuilder, SubIdOverloadsAreObjectsDirectlyBelowTheRoot) {
    Mib m(kRoot);
    m.scalar(3, {Type::Integer, [] { return Value::integer(3); }});
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.rows = [] { return std::vector<Oid>{Oid({1})}; };
    t.get = [](const Oid&, SubId) { return Value::integer(4); };
    m.table(4, std::move(t));
    EXPECT_EQ(m.get(kRoot + Oid({3, 0})), Value::integer(3));
    EXPECT_EQ(m.get(kRoot + Oid({4, 1, 2, 1})), Value::integer(4));
}

TEST_F(MibTest, OnRequestEndReportsACommitOncePerRequest) {
    std::vector<bool> ends;
    mib.onRequestEnd([&](bool committed) { ends.push_back(committed); });
    ASSERT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}, {kRoot + Oid({1, 2, 0}), Value::integer(20)}})));
    EXPECT_EQ(ends, (std::vector<bool>{true}));
}

TEST_F(MibTest, OnRequestEndReportsARollback) {
    std::vector<bool> ends;
    mib.onRequestEnd([&](bool committed) { ends.push_back(committed); });
    EXPECT_TRUE(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}, {kRoot + Oid({2, 1, 2, 1}), Value::string("commitfail")}}));
    EXPECT_EQ(ends, (std::vector<bool>{false}));
}

TEST_F(MibTest, OnRequestEndIsNotCalledForARequestRefusedBeforeWriting) {
    int ends = 0;
    mib.onRequestEnd([&](bool) { ++ends; });
    EXPECT_TRUE(trySet(mib, {{kRoot + Oid({1, 2, 0}), Value::integer(500)}}));  // the validator refuses in prepare
    EXPECT_EQ(ends, 0);
}

TEST_F(MibTest, OnUndoRunsBeforeTheOldValuesComeBack) {
    std::vector<std::string> events;
    mib.onUndo([&] { events.push_back("undo starts, name is " + name); });
    mib.onRequestEnd([&](bool committed) { events.push_back(std::string(committed ? "commit" : "rollback") + ", name is " + name); });
    EXPECT_TRUE(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}, {kRoot + Oid({2, 1, 2, 1}), Value::string("commitfail")}}));
    EXPECT_EQ(events, (std::vector<std::string>{"undo starts, name is x", "rollback, name is n"}));
}

TEST_F(MibTest, HooksThatThrowAreIgnored) {
    mib.onRequestEnd([](bool) { throw std::runtime_error("hook"); });
    mib.onUndo([] { throw std::runtime_error("hook"); });
    EXPECT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}})));
    EXPECT_TRUE(trySet(mib, {{kRoot + Oid({2, 1, 2, 1}), Value::string("commitfail")}}));
    EXPECT_EQ(name, "x");
}

TEST_F(MibTest, AnEmptyHookIsNone) {
    mib.onRequestEnd(nullptr);
    mib.onUndo(nullptr);
    EXPECT_TRUE(Succeeds(trySet(mib, {{kRoot + Oid({1, 1, 0}), Value::string("x")}})));
}

// =================================================================================================
// more SET and table rules
// =================================================================================================

TEST(MibSet, ValidatorKeepsTheIndexItNamesItself) {
    Mib m(kRoot);
    int v = 0;
    m.scalar({1}, {Type::Integer, [&] { return Value::integer(v); }, [&](const Value& x) { v = x.asInt(); },
                   [](const Value&) { throw SetError(ErrorStatus::WrongValue, "no", 7); }});
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 0}), Value::integer(1)}}), ErrorStatus::WrongValue, 7));
}

TEST(MibSet, ValidatorThatThrowsSomethingElseIsGenErr) {
    Mib m(kRoot);
    m.scalar({1}, {Type::Integer, [] { return Value::integer(0); }, [](const Value&) {},
                   [](const Value&) { throw std::runtime_error("unexpected"); }});
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 0}), Value::integer(1)}}), ErrorStatus::GenErr, 0));
}

TEST(MibSet, BitsAcceptAnOctetStringAndStoreBits) {  // a manager's BITS arrive as OCTET STRING
    Mib m(kRoot);
    Value stored;
    m.scalar({1}, {Type::Bits, [&] { return Value::bits(""); }, [&](const Value& x) { stored = x; }});
    ASSERT_TRUE(Succeeds(trySet(m, {{kRoot + Oid({1, 0}), Value::string(std::string(1, '\x80'))}})));
    EXPECT_EQ(stored, Value::bits(std::string(1, '\x80')));
}

TEST(MibSet, TableWithoutSetIsReadOnly) {
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadWrite}};  // writable by access, but no set callback
    t.rows = [] { return std::vector<Oid>{Oid({1})}; };
    t.get = [](const Oid&, SubId) { return Value::integer(1); };
    m.table(1, std::move(t));
    EXPECT_TRUE(FailsWith(trySet(m, {{kRoot + Oid({1, 1, 2, 1}), Value::integer(2)}}), ErrorStatus::NotWritable, 0));
}

TEST(MibTable, NextRowThatDoesNotIncreaseIsAnError) {  // otherwise GETNEXT would loop forever
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.hasRow = [](const Oid&) { return true; };
    t.nextRow = [](const Oid*) { return std::optional<Oid>(Oid({1})); };  // always row 1
    t.get = [](const Oid&, SubId) { return Value::integer(1); };
    m.table(1, std::move(t));
    EXPECT_EQ(m.getNext(kRoot)->oid, kRoot + Oid({1, 1, 2, 1}));  // the first row is fine
    EXPECT_THROW(m.getNext(kRoot + Oid({1, 1, 2, 1})), Error);
}

TEST(MibTable, WithoutIndexLayoutEveryNonEmptyIndexIsARow) {
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.rows = [] { return std::vector<Oid>{Oid({7, 7, 7})}; };
    t.get = [](const Oid& i, SubId) { return Value::integer(static_cast<int>(i.size())); };
    m.table(1, std::move(t));
    EXPECT_EQ(m.get(kRoot + Oid({1, 1, 2, 7, 7, 7})), Value::integer(3));
}

TEST(MibDefinitions, RowStatusNeedsAllItsCallbacks) {
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadWrite}};
    t.rows = [] { return std::vector<Oid>{}; };
    t.get = [](const Oid&, SubId) { return Value::integer(1); };
    RowStatusSpec rs;
    rs.column = 3;
    t.rowStatus = rs;  // create / destroy / setState / state missing
    EXPECT_THROW(m.table(1, t), Error);
}

TEST(MibDefinitions, ColumnIdsMustBePositiveAndUnique) {
    Mib m(kRoot);
    TableDef t;
    t.rows = [] { return std::vector<Oid>{}; };
    t.get = [](const Oid&, SubId) { return Value::integer(1); };
    t.columns = {{0, Type::Integer, Access::ReadOnly}};
    EXPECT_THROW(m.table(1, t), Error);
    t.columns = {{2, Type::Integer, Access::ReadOnly}, {2, Type::Gauge32, Access::ReadOnly}};
    EXPECT_THROW(m.table(1, t), Error);
}

TEST(MibDefinitions, ObjectsMustNotOverlapEvenAcrossKinds) {
    Mib m(kRoot);
    m.scalar({1}, {Type::Integer, [] { return Value::integer(1); }});
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}};
    t.rows = [] { return std::vector<Oid>{}; };
    t.get = [](const Oid&, SubId) { return Value::integer(1); };
    EXPECT_THROW(m.table(Oid({1, 5}), t), Error);  // below the scalar
    EXPECT_THROW(m.scalar(Oid{}, {Type::Integer, [] { return Value::integer(1); }}), Error);  // empty relative OID
}

}  // namespace
