// The generated nested Data structure and its DataAgent adapter (NESTED-TEST-MIB, NESTED-GROUP-MIB,
// BITS from SNMPWRAPPER-TEST-MIB). No snmpd: the adapter is bound to a plain snmpwrap::Mib and driven like
// the agent does (prepare -> apply -> commit / undo).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "generated/nested_data.hpp"
#include "nested_group_mib.hpp"      // generated from test/mibs/NESTED-GROUP-MIB.txt (types from NESTED-TYPES-MIB)
#include "nested_test_mib.hpp"       // generated from test/mibs/NESTED-TEST-MIB.txt
#include "snmpwrapper_test_mib.hpp"  // BITS
#include "support/test_support.hpp"

namespace m = nested_test_mib;
using namespace snmpwrap;
using namespace snmpwrap::test;

namespace {

// --- the Data structure alone -------------------------------------------------------------------

TEST(Data, IsNestedLikeTheMib) {
    m::Data data;
    data.boatName = "aurora";
    data.attitude.extendedRollTable[1].extendedRollValue = 124;  // plain index number
    data.attitude.extendedRollTable[1].extendedRollSensor = "gyro-1";
    data.attitude.extendedPitchTable[m::ExtendedPitchEntryIndex{3, "port"}].extendedPitchValue = -5;  // composite index
    data.attitude.extendedPitchTable[{4, "star"}].extendedPitchValue = 6;                               // braced
    data.navigation.navigationMode = m::NavigationMode::automatic;

    EXPECT_EQ(data.attitude.extendedRollTable.size(), 1u);
    EXPECT_EQ(data.attitude.extendedRollTable.at(m::ExtendedRollEntryIndex{1}).extendedRollSensor, "gyro-1");
    EXPECT_EQ(data.attitude.extendedRollTable[1].extendedRollValue, 124);
    EXPECT_EQ(data.attitude.extendedPitchTable.size(), 2u);
    EXPECT_EQ((data.attitude.extendedPitchTable[{3, "port"}].extendedPitchValue), -5);
    EXPECT_EQ(data.navigation.navigationMode, m::NavigationMode::automatic);
    EXPECT_TRUE(data.navigation.extendedCourseTable.empty());
}

TEST(Data, RowsAreOrderedLikeTheOidsOfTheirIndexes) {
    m::Data data;
    data.attitude.extendedRollTable[1];
    data.attitude.extendedRollTable[10];
    data.attitude.extendedRollTable[2];
    std::vector<std::int32_t> order;
    for (const auto& kv : data.attitude.extendedRollTable) order.push_back(kv.first.extendedRollIndex);
    EXPECT_EQ(order, (std::vector<std::int32_t>{1, 2, 10}));
}

TEST(Data, ValidateAcceptsValidData) { EXPECT_TRUE(validData().validate().empty()); }

TEST(Data, ValidateReportsEveryViolationWithItsPath) {
    m::Data fresh;  // nothing set: boatName is empty, but SIZE(1..16)
    EXPECT_TRUE(mentions(fresh.validate(), "boatName"));

    m::Data d = validData();
    d.boatName = std::string(17, 'x');
    d.attitude.extendedRollTable[1].extendedRollValue = 5000;
    d.navigation.navigationMode = static_cast<m::NavigationMode>(7);
    d.navigation.extendedCourseTable[2].extendedCourseValue = 4000;
    const auto bad = d.validate();
    EXPECT_EQ(bad.size(), 4u);
    EXPECT_TRUE(mentions(bad, "boatName: length must be in 1..16"));
    EXPECT_TRUE(mentions(bad, "attitude.extendedRollTable[1].extendedRollValue: value must be in -1800..1800"));
    EXPECT_TRUE(mentions(bad, "navigation.navigationMode: not a defined value"));
    EXPECT_TRUE(mentions(bad, "navigation.extendedCourseTable[2].extendedCourseValue"));
}

// --- served through a Mib -----------------------------------------------------------------------

/// validData() served by a DataAgent bound to a plain Mib.
class DataAgentTest : public ::testing::Test {
protected:
    static Oid rollCell(const Oid& column, std::int32_t row) { return cell(column, m::ExtendedRollEntryIndex{row}.toOid()); }
    static Oid pitchCell(const Oid& column, std::int32_t source, const std::string& channel) {
        return cell(column, m::ExtendedPitchEntryIndex{source, channel}.toOid());
    }

    m::Data data = validData();
    Mib mib{m::oids::root};
    m::DataAgent adapter{mib, data};
};

TEST_F(DataAgentTest, GetReadsTheStructure) {
    EXPECT_EQ(mib.get(scalar(m::oids::boatName)), Value::string("aurora"));
    EXPECT_EQ(mib.get(scalar(m::oids::navigationMode)), Value::integer(1));
    EXPECT_EQ(mib.get(rollCell(m::oids::extendedRollValue, 1)), Value::integer(124));
    EXPECT_EQ(mib.get(rollCell(m::oids::extendedRollSensor, 1)), Value::string("gyro-1"));
    EXPECT_EQ(mib.get(rollCell(m::oids::extendedRollCounter, 1)), Value::counter32(7));
    EXPECT_EQ(mib.get(rollCell(m::oids::extendedRollTimepoint, 1)), Value::timeTicks(1000));
    EXPECT_FALSE(mib.get(rollCell(m::oids::extendedRollValue, 99)));  // no such row
    EXPECT_EQ(mib.missing(rollCell(m::oids::extendedRollValue, 99)), Type::NoSuchInstance);
}

TEST_F(DataAgentTest, ChangesOfTheStructureAreVisibleAtOnce) {
    data.boatName = "changed";
    EXPECT_EQ(mib.get(scalar(m::oids::boatName)), Value::string("changed"));
}

TEST_F(DataAgentTest, WalkIsInOidOrder) {
    data.attitude.extendedPitchTable[{2, "b"}].extendedPitchValue = 1;
    data.attitude.extendedPitchTable[{2, "a"}].extendedPitchValue = 2;
    data.attitude.extendedPitchTable[{1, "z"}].extendedPitchValue = 3;
    data.attitude.extendedRollTable[10].extendedRollValue = 10;

    std::vector<Oid> seen;
    Oid cur = m::oids::root;
    while (auto next = mib.getNext(cur)) {
        seen.push_back(next->oid);
        cur = next->oid;
    }
    // boatName + roll 3 rows * 4 columns + pitch 3 rows * (value + status) + mode + course
    EXPECT_EQ(seen.size(), 1u + 3 * 4 + 3 * 2 + 1 + 1);
    for (std::size_t i = 1; i < seen.size(); ++i) EXPECT_LT(seen[i - 1], seen[i]);

    // roll rows 1, 2, 10 within one column come in OID order
    std::vector<std::int32_t> rollValues;
    Oid at = m::oids::extendedRollValue;
    while (auto next = mib.getNext(at)) {
        if (!m::oids::extendedRollValue.isPrefixOf(next->oid)) break;
        rollValues.push_back(next->value.asInt());
        at = next->oid;
    }
    EXPECT_EQ(rollValues, (std::vector<std::int32_t>{124, -30, 10}));

    // pitch rows: index (source, channel) -> sorted by source, then by the encoded string
    std::vector<std::string> pitchRows;
    at = m::oids::extendedPitchValue;
    while (auto next = mib.getNext(at)) {
        if (!m::oids::extendedPitchValue.isPrefixOf(next->oid)) break;
        const auto idx = m::ExtendedPitchEntryIndex::fromOid(m::oids::extendedPitchValue.suffixOf(next->oid));
        pitchRows.push_back(std::to_string(idx->extendedPitchSource) + idx->extendedPitchChannel);
        at = next->oid;
    }
    EXPECT_EQ(pitchRows, (std::vector<std::string>{"1z", "2a", "2b"}));
}

TEST_F(DataAgentTest, SetWritesScalarsAndCells) {
    EXPECT_TRUE(Succeeds(trySet(mib, {{scalar(m::oids::boatName), Value::string("neptune")}})));
    EXPECT_EQ(data.boatName, "neptune");
    EXPECT_TRUE(Succeeds(trySet(mib, {{rollCell(m::oids::extendedRollValue, 1), Value::integer(-99)},
                                      {scalar(m::oids::navigationMode), Value::integer(2)}})));
    EXPECT_EQ(data.attitude.extendedRollTable[1].extendedRollValue, -99);
    EXPECT_EQ(data.navigation.navigationMode, m::NavigationMode::automatic);
}

TEST_F(DataAgentTest, ReadOnlyObjectsCannotBeSet) {
    EXPECT_TRUE(FailsWith(trySet(mib, {{rollCell(m::oids::extendedRollCounter, 1), Value::counter32(1)}}), ErrorStatus::NotWritable, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{cell(m::oids::extendedCourseValue, m::ExtendedCourseEntryIndex{1}.toOid()), Value::integer(1)}}),
                          ErrorStatus::NotWritable, 0));
    EXPECT_EQ(data.attitude.extendedRollTable[1].extendedRollCounter, 7u);
}

TEST_F(DataAgentTest, SetIsCheckedAgainstTheMib) {  // types and limits; the structure stays untouched
    const Oid name = scalar(m::oids::boatName), rollValue = rollCell(m::oids::extendedRollValue, 1);
    EXPECT_TRUE(FailsWith(trySet(mib, {{rollValue, Value::integer(5000)}}), ErrorStatus::WrongValue, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{rollValue, Value::string("x")}}), ErrorStatus::WrongType, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{name, Value::string("")}}), ErrorStatus::WrongLength, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{name, Value::string(std::string(17, 'x'))}}), ErrorStatus::WrongLength, 0));
    EXPECT_TRUE(FailsWith(trySet(mib, {{scalar(m::oids::navigationMode), Value::integer(3)}}), ErrorStatus::WrongValue, 0));
    EXPECT_EQ(data.boatName, "aurora");
    EXPECT_EQ(data.attitude.extendedRollTable[1].extendedRollValue, 124);
}

TEST_F(DataAgentTest, SetIsAllOrNothing) {  // the invalid second value cancels the valid first one
    const auto e = trySet(mib, {{scalar(m::oids::boatName), Value::string("zephyr")},
                                {rollCell(m::oids::extendedRollValue, 1), Value::integer(9999)}});
    EXPECT_TRUE(FailsWith(e, ErrorStatus::WrongValue, 1));
    EXPECT_EQ(data.boatName, "aurora");
}

TEST_F(DataAgentTest, CellOfAMissingRowWithoutRowStatusIsRefused) {
    EXPECT_TRUE(trySet(mib, {{rollCell(m::oids::extendedRollValue, 50), Value::integer(1)}}));
}

TEST_F(DataAgentTest, RowStatusCreatesAndDestroysRows) {
    const Oid value = pitchCell(m::oids::extendedPitchValue, 2, "bow"), status = pitchCell(m::oids::extendedPitchStatus, 2, "bow");

    // createAndGo needs the (writable) value column in the same request
    EXPECT_TRUE(FailsWith(trySet(mib, {{status, Value::integer(4)}}), ErrorStatus::InconsistentValue, 0));
    EXPECT_TRUE(data.attitude.extendedPitchTable.empty());

    ASSERT_TRUE(Succeeds(trySet(mib, {{value, Value::integer(25)}, {status, Value::integer(4)}})));
    ASSERT_EQ(data.attitude.extendedPitchTable.size(), 1u);
    const auto& row = data.attitude.extendedPitchTable.at(m::ExtendedPitchEntryIndex{2, "bow"});
    EXPECT_EQ(row.extendedPitchValue, 25);
    EXPECT_EQ(row.extendedPitchStatus, RowStatus::Active);
    EXPECT_EQ(mib.get(status), Value::integer(1));

    // the value is out of range -> no row
    EXPECT_TRUE(FailsWith(trySet(mib, {{pitchCell(m::oids::extendedPitchValue, 2, "x"), Value::integer(901)},
                                       {pitchCell(m::oids::extendedPitchStatus, 2, "x"), Value::integer(4)}}),
                          ErrorStatus::WrongValue, 0));
    EXPECT_EQ(data.attitude.extendedPitchTable.size(), 1u);

    // destroy removes the row from the structure
    EXPECT_TRUE(Succeeds(trySet(mib, {{status, Value::integer(6)}})));
    EXPECT_TRUE(data.attitude.extendedPitchTable.empty());
}

TEST_F(DataAgentTest, RowCompleteness) {
    const Oid status = pitchCell(m::oids::extendedPitchStatus, 5, "aft"), value = pitchCell(m::oids::extendedPitchValue, 5, "aft");
    auto state = [&] { return data.attitude.extendedPitchTable.at(m::ExtendedPitchEntryIndex{5, "aft"}).extendedPitchStatus; };

    // createAndWait without the required column: the row exists but is not ready
    ASSERT_TRUE(Succeeds(trySet(mib, {{status, Value::integer(5)}})));
    EXPECT_EQ(state(), RowStatus::NotReady);
    EXPECT_TRUE(FailsWith(trySet(mib, {{status, Value::integer(1)}}), ErrorStatus::InconsistentValue, 0));  // not active yet

    // the column supplied by a rolled-back request does not count (a later row of the request is refused)
    const Oid other = m::ExtendedPitchEntryIndex{9, "x"}.toOid();
    adapter.onSet([&](m::Object object, const Oid& index) {
        if (object == m::Object::extendedPitchStatus && index == other) throw SetError(ErrorStatus::InconsistentValue, "no");
    });
    EXPECT_TRUE(trySet(mib, {{value, Value::integer(12)},
                             {cell(m::oids::extendedPitchValue, other), Value::integer(1)},
                             {cell(m::oids::extendedPitchStatus, other), Value::integer(4)}}));
    adapter.onSet(nullptr);
    EXPECT_EQ(state(), RowStatus::NotReady);
    EXPECT_TRUE(FailsWith(trySet(mib, {{status, Value::integer(1)}}), ErrorStatus::InconsistentValue, 0));  // still incomplete

    // supplying the column completes it (notReady -> notInService), then it can be activated
    EXPECT_TRUE(Succeeds(trySet(mib, {{value, Value::integer(12)}})));
    EXPECT_EQ(state(), RowStatus::NotInService);
    EXPECT_TRUE(Succeeds(trySet(mib, {{status, Value::integer(1)}})));
    EXPECT_EQ(mib.get(status), Value::integer(1));
}

TEST_F(DataAgentTest, RowsAddedByTheApplicationAreComplete) {
    data.attitude.extendedPitchTable[{6, "app"}].extendedPitchStatus = RowStatus::NotInService;
    EXPECT_TRUE(Succeeds(trySet(mib, {{pitchCell(m::oids::extendedPitchStatus, 6, "app"), Value::integer(1)}})));
}

// --- hooks --------------------------------------------------------------------------------------

TEST_F(DataAgentTest, OnSetSeesEveryChange) {
    std::vector<std::string> events;
    adapter.onSet([&](m::Object object, const Oid& index) { events.push_back(std::string(m::toString(object)) + "@" + index.str()); });
    EXPECT_TRUE(Succeeds(trySet(mib, {{scalar(m::oids::boatName), Value::string("hooked")}})));
    EXPECT_TRUE(Succeeds(trySet(mib, {{rollCell(m::oids::extendedRollSensor, 2), Value::string("g2")}})));
    EXPECT_EQ(events, (std::vector<std::string>{"boatName@", "extendedRollSensor@2"}));
}

TEST_F(DataAgentTest, OnSetCanRefuseAChange) {  // the old value comes back
    adapter.onSet([](m::Object object, const Oid&) {
        if (object == m::Object::boatName) throw SetError(ErrorStatus::InconsistentValue, "not now");
    });
    const auto e = trySet(mib, {{scalar(m::oids::boatName), Value::string("refused")}});
    ASSERT_TRUE(e);
    EXPECT_EQ(e->status(), ErrorStatus::InconsistentValue);
    EXPECT_EQ(data.boatName, "aurora");
}

TEST_F(DataAgentTest, RollbackBypassesTheHooks) {
    const Oid name = scalar(m::oids::boatName);
    const Oid sensor = rollCell(m::oids::extendedRollSensor, 1);
    const Oid rowA = m::ExtendedPitchEntryIndex{1, "a"}.toOid(), rowB = m::ExtendedPitchEntryIndex{2, "b"}.toOid();
    std::vector<std::string> events;
    // refuses every sensor change, row B, and would refuse putting boatName back to "aurora"
    adapter.onSet([&](m::Object object, const Oid& index) {
        events.push_back(std::string(m::toString(object)) + "@" + index.str());
        if (object == m::Object::extendedRollSensor) throw SetError(ErrorStatus::InconsistentValue, "no");
        if (object == m::Object::boatName && data.boatName == "aurora") throw SetError(ErrorStatus::InconsistentValue, "no");
        if (object == m::Object::extendedPitchStatus && index == rowB) throw SetError(ErrorStatus::InconsistentValue, "no");
    });
    int changed = 0;
    adapter.onChanged([&](m::Object, const Oid&) { ++changed; });

    // boatName is applied first, the sensor fails -> rollback restores boatName although the hook would refuse it
    EXPECT_TRUE(FailsWith(trySet(mib, {{name, Value::string("neptune")}, {sensor, Value::string("g")}}), ErrorStatus::InconsistentValue, 1));
    EXPECT_EQ(data.boatName, "aurora");
    EXPECT_EQ(data.attitude.extendedRollTable[1].extendedRollSensor, "gyro-1");
    EXPECT_EQ(events, (std::vector<std::string>{"boatName@", "extendedRollSensor@1"}));  // not called for the rollback
    EXPECT_EQ(changed, 0);

    // row A is created, row B is refused -> A is destroyed again without reporting a destroy (rows run in index order)
    events.clear();
    EXPECT_TRUE(trySet(mib, {{cell(m::oids::extendedPitchValue, rowA), Value::integer(1)},
                             {cell(m::oids::extendedPitchStatus, rowA), Value::integer(4)},
                             {cell(m::oids::extendedPitchValue, rowB), Value::integer(2)},
                             {cell(m::oids::extendedPitchStatus, rowB), Value::integer(4)}}));
    EXPECT_TRUE(data.attitude.extendedPitchTable.empty());
    EXPECT_EQ(events, (std::vector<std::string>{"extendedPitchStatus@" + rowA.str(), "extendedPitchStatus@" + rowB.str()}));

    // the next request is a normal one again
    events.clear();
    EXPECT_TRUE(Succeeds(trySet(mib, {{name, Value::string("zephyr")}})));
    EXPECT_EQ(data.boatName, "zephyr");
    EXPECT_EQ(changed, 1);
    EXPECT_EQ(events, (std::vector<std::string>{"boatName@"}));
}

TEST_F(DataAgentTest, OnGetRunsBeforeEveryRead) {
    int reads = 0;
    adapter.onGet([&](m::Object object, const Oid& index) {
        ++reads;
        if (object == m::Object::extendedRollCounter)
            data.attitude.extendedRollTable.at(*m::ExtendedRollEntryIndex::fromOid(index)).extendedRollCounter += 100;
    });
    const Oid counter = rollCell(m::oids::extendedRollCounter, 1);
    EXPECT_EQ(mib.get(counter), Value::counter32(107));  // 7 + 100: the hook ran before the read
    EXPECT_EQ(mib.get(counter), Value::counter32(207));
    EXPECT_EQ(reads, 2);
    mib.get(scalar(m::oids::boatName));
    EXPECT_EQ(reads, 3);
    EXPECT_FALSE(mib.get(rollCell(m::oids::extendedRollCounter, 99)));
    EXPECT_EQ(reads, 3);  // a missing instance is not read
}

TEST_F(DataAgentTest, LockDoesNotBlockReadsFromThisThread) {
    {
        auto guard = adapter.lock();
        data.boatName = "locked";
    }
    EXPECT_EQ(mib.get(scalar(m::oids::boatName)), Value::string("locked"));
}

// --- messages after the commit ------------------------------------------------------------------

/// Logs every message of the adapter: values, rows and subtrees.
class DataAgentMessagesTest : public DataAgentTest {
protected:
    DataAgentMessagesTest() {
        adapter.onChanged([this](m::Object o, const Oid& index) { log.push_back(std::string("value ") + m::toString(o) + "@" + index.str()); });
        adapter.onOwnboat([this](const m::Data& d) { log.push_back("ownboat " + d.boatName); });
        adapter.onNavigation([this](const m::Data::NavigationGroup& g) { log.push_back("navigation " + std::string(m::toString(g.navigationMode))); });
        adapter.onAttitude([this](const m::Data::AttitudeGroup& g) { log.push_back("attitude rows=" + std::to_string(g.extendedRollTable.size())); });
        adapter.onExtendedRollTableRow([this](const m::ExtendedRollEntryIndex& i, const m::ExtendedRollEntry& r) {
            log.push_back("roll[" + std::to_string(i.extendedRollIndex) + "] " + std::to_string(r.extendedRollValue) + " " + r.extendedRollSensor);
        });
        adapter.onExtendedPitchTableRow([this](const m::ExtendedPitchEntryIndex& i, const m::ExtendedPitchEntry& r) {
            log.push_back("pitch[" + i.extendedPitchChannel + "] " + std::to_string(static_cast<int>(r.extendedPitchStatus)));
        });
    }

    std::vector<std::string> log;
};

TEST_F(DataAgentMessagesTest, EachValueAndEachTouchedSubtreeOnce) {
    // a scalar directly in the module and one in a group, one request
    ASSERT_TRUE(Succeeds(trySet(mib, {{scalar(m::oids::boatName), Value::string("orca")}, {scalar(m::oids::navigationMode), Value::integer(2)}})));
    EXPECT_EQ(log, (std::vector<std::string>{"value boatName@", "value navigationMode@", "navigation automatic", "ownboat orca"}));
}

TEST_F(DataAgentMessagesTest, EachRowOnceWithAllItsNewValues) {
    // two cells of row 1 and one of row 2; the attitude subtree once
    ASSERT_TRUE(Succeeds(trySet(mib, {{rollCell(m::oids::extendedRollValue, 1), Value::integer(11)},
                                      {rollCell(m::oids::extendedRollSensor, 1), Value::string("g-new")},
                                      {rollCell(m::oids::extendedRollValue, 2), Value::integer(22)}})));
    EXPECT_EQ(log, (std::vector<std::string>{"value extendedRollValue@1", "value extendedRollSensor@1", "value extendedRollValue@2",
                                             "roll[1] 11 g-new", "roll[2] 22 gyro-2", "attitude rows=2", "ownboat aurora"}));
}

TEST_F(DataAgentMessagesTest, NothingForARefusedRequest) {
    // an invalid value (before anything is written) ...
    EXPECT_TRUE(trySet(mib, {{scalar(m::oids::boatName), Value::string("x")}, {scalar(m::oids::navigationMode), Value::integer(9)}}));
    // ... or refused by onSet while writing (rolled back)
    adapter.onSet([](m::Object o, const Oid&) {
        if (o == m::Object::navigationMode) throw SetError(ErrorStatus::InconsistentValue, "no");
    });
    EXPECT_TRUE(trySet(mib, {{scalar(m::oids::boatName), Value::string("y")}, {scalar(m::oids::navigationMode), Value::integer(1)}}));
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(data.boatName, "aurora");
}

TEST_F(DataAgentMessagesTest, RowStatusCreateIsARowMessageDestroyIsNot) {
    const Oid idx = m::ExtendedPitchEntryIndex{1, "aft"}.toOid();
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(m::oids::extendedPitchValue, idx), Value::integer(5)},
                                      {cell(m::oids::extendedPitchStatus, idx), Value::integer(4)}})));
    EXPECT_EQ(log, (std::vector<std::string>{"value extendedPitchStatus@1.3.97.102.116", "pitch[aft] 1", "attitude rows=2", "ownboat aurora"}));
    log.clear();
    ASSERT_TRUE(Succeeds(trySet(mib, {{cell(m::oids::extendedPitchStatus, idx), Value::integer(6)}})));
    EXPECT_EQ(log, (std::vector<std::string>{"value extendedPitchStatus@1.3.97.102.116", "attitude rows=2", "ownboat aurora"}));
}

// --- other MIBs ---------------------------------------------------------------------------------

TEST(DataAgentBits, ManagersBitsArriveAsOctetString) {
    namespace t = snmpwrapper_test_mib;
    t::Data data;
    data.swtScalars.swtFlags = std::string(1, '\x80');
    Mib mib{t::oids::root};
    t::DataAgent adapter{mib, data};
    const Oid flags = scalar(t::oids::swtFlags);

    EXPECT_EQ(mib.get(flags), Value::bits(std::string(1, '\x80')));  // served with the declared type
    // a manager's BITS arrive as OCTET STRING (no own tag on the wire) and are accepted
    EXPECT_TRUE(Succeeds(trySet(mib, {{flags, Value::string(std::string(1, '\x60'))}})));
    EXPECT_EQ(data.swtScalars.swtFlags, std::string(1, '\x60'));
    EXPECT_TRUE(Succeeds(trySet(mib, {{flags, Value::bits(std::string(1, '\x20'))}})));
    EXPECT_EQ(data.swtScalars.swtFlags, std::string(1, '\x20'));
    EXPECT_TRUE(FailsWith(trySet(mib, {{flags, Value::integer(1)}}), ErrorStatus::WrongType, 0));
}

TEST(DataAgentImportedTypes, SingleGroupWithTypesFromAnotherMib) {
    namespace g = nested_group_mib;
    g::Data d;
    d.attitude.boatRoll = 5000;                          // the group survives although every object is inside it
    d.attitude.boatRollSensor = g::SensorKind::compass;  // named numbers of a TEXTUAL-CONVENTION from another MIB
    const auto bad = d.validate();
    EXPECT_EQ(bad.size(), 1u);
    EXPECT_TRUE(mentions(bad, "attitude.boatRoll: value must be in -1800..1800"));  // range of the imported TC

    Mib mib{g::oids::root};
    g::DataAgent adapter{mib, d};
    EXPECT_TRUE(FailsWith(trySet(mib, {{g::oids::boatRollSensor + SubId{0}, Value::integer(4)}}), ErrorStatus::WrongValue, 0));
    EXPECT_TRUE(Succeeds(trySet(mib, {{g::oids::boatRoll + SubId{0}, Value::integer(-900)}})));
    EXPECT_EQ(d.attitude.boatRoll, -900);
    EXPECT_STREQ(g::toString(g::SensorKind::gps), "gps");
}

}  // namespace
