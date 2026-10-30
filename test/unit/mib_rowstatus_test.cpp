// Mib with RowStatus (RFC 2579): createAndGo / createAndWait, notReady -> notInService, destroy in COMMIT,
// error statuses, rollback of row creation and state changes, rejectEditWhileActive, malformed indexes.

#include <gtest/gtest.h>

#include <map>
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

}  // namespace
