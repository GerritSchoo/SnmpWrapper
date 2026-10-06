// Tests of the generated nested Data structure and its DataAgent adapter (NESTED-TEST-MIB).
// No snmpd needed: the adapter is bound to a plain snmpwrap::Mib and driven like the agent does.

#include <iostream>
#include <string>
#include <vector>

#include "nested_group_mib.hpp"  // generated from test/mibs/NESTED-GROUP-MIB.txt (types from NESTED-TYPES-MIB)
#include "nested_test_mib.hpp"   // generated from test/mibs/NESTED-TEST-MIB.txt

namespace m = nested_test_mib;
using namespace snmpwrap;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(...)                                                                         \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(__VA_ARGS__)) {                                                              \
            ++g_failures;                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n"; \
        }                                                                                  \
    } while (0)

/// A whole SET like the agent runs it: prepare -> apply -> commit (undo after a failed apply).
std::optional<SetError> trySet(Handler& h, const std::vector<VarBind>& sets) {
    std::unique_ptr<SetTransaction> t;
    try {
        t = h.prepare(sets);
    } catch (const SetError& e) {
        return e;
    }
    try {
        t->apply();
    } catch (const SetError& e) {
        t->undo();
        return e;
    }
    t->commit();
    return std::nullopt;
}

bool failsWith(const std::optional<SetError>& e, ErrorStatus status, std::size_t index) {
    return e && e->status() == status && e->index() == index;
}

bool mentions(const std::vector<std::string>& v, const std::string& what) {
    for (const std::string& s : v)
        if (s.find(what) != std::string::npos) return true;
    return false;
}

Oid scalar(const Oid& object) { return object + SubId{0}; }
Oid cell(const Oid& column, const Oid& index) { return column + index; }

/// A fully valid Data object: every value inside the ranges of the MIB.
m::Data validData() {
    m::Data d;
    d.boatName = "aurora";
    d.navigation.navigationMode = m::NavigationMode::manual;
    d.attitude.extendedRollTable[1] = {124, "gyro-1", 7, 1000};
    d.attitude.extendedRollTable[2] = {-30, "gyro-2", 0, 0};
    d.navigation.extendedCourseTable[1].extendedCourseValue = 900;
    return d;
}

// ---------------------------------------------------------------------------------------------
void testNestedStructure() {
    m::Data data;
    data.boatName = "aurora";
    data.attitude.extendedRollTable[1].extendedRollValue = 124;  // plain index number
    data.attitude.extendedRollTable[1].extendedRollSensor = "gyro-1";
    data.attitude.extendedPitchTable[m::ExtendedPitchEntryIndex{3, "port"}].extendedPitchValue = -5;  // composite index
    data.attitude.extendedPitchTable[{4, "star"}].extendedPitchValue = 6;                               // braced
    data.navigation.navigationMode = m::NavigationMode::automatic;

    CHECK(data.attitude.extendedRollTable.size() == 1);
    CHECK(data.attitude.extendedRollTable.at(m::ExtendedRollEntryIndex{1}).extendedRollSensor == "gyro-1");
    CHECK(data.attitude.extendedRollTable[1].extendedRollValue == 124);
    CHECK(data.attitude.extendedPitchTable.size() == 2);
    CHECK(data.attitude.extendedPitchTable[{3, "port"}].extendedPitchValue == -5);
    CHECK(data.navigation.navigationMode == m::NavigationMode::automatic);
    CHECK(data.navigation.extendedCourseTable.empty());

    // rows are ordered like the OIDs of their indexes, not like the numbers
    data.attitude.extendedRollTable[10];
    data.attitude.extendedRollTable[2];
    std::vector<std::int32_t> order;
    for (const auto& kv : data.attitude.extendedRollTable) order.push_back(kv.first.extendedRollIndex);
    CHECK((order == std::vector<std::int32_t>{1, 2, 10}));
}

void testValidate() {
    CHECK(validData().validate().empty());

    m::Data fresh;  // nothing set: boatName is empty, but SIZE(1..16)
    CHECK(mentions(fresh.validate(), "boatName"));

    m::Data d = validData();
    d.boatName = std::string(17, 'x');
    d.attitude.extendedRollTable[1].extendedRollValue = 5000;
    d.navigation.navigationMode = static_cast<m::NavigationMode>(7);
    d.navigation.extendedCourseTable[2].extendedCourseValue = 4000;
    const auto bad = d.validate();
    CHECK(bad.size() == 4);
    CHECK(mentions(bad, "boatName: length must be in 1..16"));
    CHECK(mentions(bad, "attitude.extendedRollTable[1].extendedRollValue: value must be in -1800..1800"));
    CHECK(mentions(bad, "navigation.navigationMode: not a defined value"));
    CHECK(mentions(bad, "navigation.extendedCourseTable[2].extendedCourseValue"));
}

struct Served {
    m::Data data = validData();
    Mib mib{m::oids::root};
    m::DataAgent adapter{mib, data};
};

void testGet() {
    Served s;
    CHECK(s.mib.get(scalar(m::oids::boatName)) == Value::string("aurora"));
    CHECK(s.mib.get(scalar(m::oids::navigationMode)) == Value::integer(1));
    const Oid roll1 = m::ExtendedRollEntryIndex{1}.toOid();
    CHECK(s.mib.get(cell(m::oids::extendedRollValue, roll1)) == Value::integer(124));
    CHECK(s.mib.get(cell(m::oids::extendedRollSensor, roll1)) == Value::string("gyro-1"));
    CHECK(s.mib.get(cell(m::oids::extendedRollCounter, roll1)) == Value::counter32(7));
    CHECK(s.mib.get(cell(m::oids::extendedRollTimepoint, roll1)) == Value::timeTicks(1000));
    CHECK(!s.mib.get(cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{99}.toOid())));  // no such row
    CHECK(s.mib.missing(cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{99}.toOid())) == Type::NoSuchInstance);

    // a change of the structure is visible at once
    s.data.boatName = "changed";
    CHECK(s.mib.get(scalar(m::oids::boatName)) == Value::string("changed"));
}

void testWalkOrder() {
    Served s;
    s.data.attitude.extendedPitchTable[{2, "b"}].extendedPitchValue = 1;
    s.data.attitude.extendedPitchTable[{2, "a"}].extendedPitchValue = 2;
    s.data.attitude.extendedPitchTable[{1, "z"}].extendedPitchValue = 3;
    s.data.attitude.extendedRollTable[10].extendedRollValue = 10;

    std::vector<Oid> seen;
    Oid cur = m::oids::root;
    while (auto next = s.mib.getNext(cur)) {
        seen.push_back(next->oid);
        cur = next->oid;
    }
    // boatName + roll (2 rows... 3 with row 10) * 4 columns + pitch 3 rows * (value + status) + mode + course
    CHECK(seen.size() == 1 + 3 * 4 + 3 * 2 + 1 + 1);
    for (std::size_t i = 1; i < seen.size(); ++i) CHECK(seen[i - 1] < seen[i]);

    // roll rows 1, 2, 10 within one column come in OID order
    std::vector<std::int32_t> rollValues;
    Oid at = m::oids::extendedRollValue;
    while (auto next = s.mib.getNext(at)) {
        if (!m::oids::extendedRollValue.isPrefixOf(next->oid)) break;
        rollValues.push_back(next->value.asInt());
        at = next->oid;
    }
    CHECK((rollValues == std::vector<std::int32_t>{124, -30, 10}));

    // pitch rows: index (source, channel) -> sorted by source, then by the encoded string
    std::vector<std::string> pitchRows;
    at = m::oids::extendedPitchValue;
    while (auto next = s.mib.getNext(at)) {
        if (!m::oids::extendedPitchValue.isPrefixOf(next->oid)) break;
        const auto idx = m::ExtendedPitchEntryIndex::fromOid(m::oids::extendedPitchValue.suffixOf(next->oid));
        pitchRows.push_back(std::to_string(idx->extendedPitchSource) + idx->extendedPitchChannel);
        at = next->oid;
    }
    CHECK((pitchRows == std::vector<std::string>{"1z", "2a", "2b"}));
}

void testSet() {
    Served s;
    const Oid name = scalar(m::oids::boatName);
    const Oid rollValue = cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{1}.toOid());

    // writable scalar and writable cell change the structure
    CHECK(!trySet(s.mib, {{name, Value::string("neptune")}}));
    CHECK(s.data.boatName == "neptune");
    CHECK(!trySet(s.mib, {{rollValue, Value::integer(-99)}, {scalar(m::oids::navigationMode), Value::integer(2)}}));
    CHECK(s.data.attitude.extendedRollTable[1].extendedRollValue == -99);
    CHECK(s.data.navigation.navigationMode == m::NavigationMode::automatic);

    // read-only objects cannot be set
    const Oid counter = cell(m::oids::extendedRollCounter, m::ExtendedRollEntryIndex{1}.toOid());
    CHECK(failsWith(trySet(s.mib, {{counter, Value::counter32(1)}}), ErrorStatus::NotWritable, 0));
    CHECK(failsWith(trySet(s.mib, {{cell(m::oids::extendedCourseValue, m::ExtendedCourseEntryIndex{1}.toOid()), Value::integer(1)}}),
                    ErrorStatus::NotWritable, 0));
    CHECK(s.data.attitude.extendedRollTable[1].extendedRollCounter == 7);

    // MIB types and limits are checked on SET, the structure stays untouched
    CHECK(failsWith(trySet(s.mib, {{rollValue, Value::integer(5000)}}), ErrorStatus::WrongValue, 0));
    CHECK(failsWith(trySet(s.mib, {{rollValue, Value::string("x")}}), ErrorStatus::WrongType, 0));
    CHECK(failsWith(trySet(s.mib, {{name, Value::string("")}}), ErrorStatus::WrongLength, 0));
    CHECK(failsWith(trySet(s.mib, {{name, Value::string(std::string(17, 'x'))}}), ErrorStatus::WrongLength, 0));
    CHECK(failsWith(trySet(s.mib, {{scalar(m::oids::navigationMode), Value::integer(3)}}), ErrorStatus::WrongValue, 0));
    CHECK(s.data.boatName == "neptune" && s.data.attitude.extendedRollTable[1].extendedRollValue == -99);

    // all or nothing: the invalid second value cancels the valid first one
    const auto e = trySet(s.mib, {{name, Value::string("zephyr")}, {rollValue, Value::integer(9999)}});
    CHECK(failsWith(e, ErrorStatus::WrongValue, 1));
    CHECK(s.data.boatName == "neptune");

    // a cell of a row that does not exist (table without RowStatus)
    CHECK(trySet(s.mib, {{cell(m::oids::extendedRollValue, m::ExtendedRollEntryIndex{50}.toOid()), Value::integer(1)}}));
}

void testRowStatus() {
    Served s;
    const Oid idx = m::ExtendedPitchEntryIndex{2, "bow"}.toOid();
    const Oid value = cell(m::oids::extendedPitchValue, idx), status = cell(m::oids::extendedPitchStatus, idx);

    // createAndGo needs the (writable) value column in the same request
    CHECK(failsWith(trySet(s.mib, {{status, Value::integer(4)}}), ErrorStatus::InconsistentValue, 0));
    CHECK(s.data.attitude.extendedPitchTable.empty());

    CHECK(!trySet(s.mib, {{value, Value::integer(25)}, {status, Value::integer(4)}}));
    CHECK(s.data.attitude.extendedPitchTable.size() == 1);
    const auto& row = s.data.attitude.extendedPitchTable.at(m::ExtendedPitchEntryIndex{2, "bow"});
    CHECK(row.extendedPitchValue == 25 && row.extendedPitchStatus == RowStatus::Active);
    CHECK(s.mib.get(status) == Value::integer(1));

    // the value is out of range -> no row
    CHECK(failsWith(trySet(s.mib, {{cell(m::oids::extendedPitchValue, m::ExtendedPitchEntryIndex{2, "x"}.toOid()), Value::integer(901)},
                                   {cell(m::oids::extendedPitchStatus, m::ExtendedPitchEntryIndex{2, "x"}.toOid()), Value::integer(4)}}),
                    ErrorStatus::WrongValue, 0));
    CHECK(s.data.attitude.extendedPitchTable.size() == 1);

    // destroy removes the row from the structure
    CHECK(!trySet(s.mib, {{status, Value::integer(6)}}));
    CHECK(s.data.attitude.extendedPitchTable.empty());
}

void testHook() {
    Served s;
    std::vector<std::string> events;
    s.adapter.onSet([&](m::Object object, const Oid& index) { events.push_back(std::string(m::toString(object)) + "@" + index.str()); });
    CHECK(!trySet(s.mib, {{scalar(m::oids::boatName), Value::string("hooked")}}));
    CHECK(!trySet(s.mib, {{cell(m::oids::extendedRollSensor, m::ExtendedRollEntryIndex{2}.toOid()), Value::string("g2")}}));
    CHECK((events == std::vector<std::string>{"boatName@", "extendedRollSensor@2"}));

    // a hook that throws SetError refuses the change; the old value comes back
    s.adapter.onSet([](m::Object object, const Oid&) {
        if (object == m::Object::boatName) throw SetError(ErrorStatus::InconsistentValue, "not now");
    });
    const auto e = trySet(s.mib, {{scalar(m::oids::boatName), Value::string("refused")}});
    CHECK(e && e->status() == ErrorStatus::InconsistentValue);
    CHECK(s.data.boatName == "hooked");
}

void testGetHook() {
    Served s;
    int reads = 0;
    s.adapter.onGet([&](m::Object object, const Oid& index) {
        ++reads;
        if (object == m::Object::extendedRollCounter) s.data.attitude.extendedRollTable.at(*m::ExtendedRollEntryIndex::fromOid(index)).extendedRollCounter += 100;
    });
    const Oid counter = cell(m::oids::extendedRollCounter, m::ExtendedRollEntryIndex{1}.toOid());
    CHECK(s.mib.get(counter) == Value::counter32(107));  // 7 + 100: the hook ran before the read
    CHECK(s.mib.get(counter) == Value::counter32(207));
    CHECK(reads == 2);
    s.mib.get(scalar(m::oids::boatName));
    CHECK(reads == 3);
    CHECK(!s.mib.get(cell(m::oids::extendedRollCounter, m::ExtendedRollEntryIndex{99}.toOid())));
    CHECK(reads == 3);  // a missing instance is not read
}

void testRowCompleteness() {
    Served s;
    const Oid idx = m::ExtendedPitchEntryIndex{5, "aft"}.toOid();
    const Oid status = cell(m::oids::extendedPitchStatus, idx), value = cell(m::oids::extendedPitchValue, idx);

    // createAndWait without the required column: the row exists but is not ready
    CHECK(!trySet(s.mib, {{status, Value::integer(5)}}));
    CHECK(s.data.attitude.extendedPitchTable.at(m::ExtendedPitchEntryIndex{5, "aft"}).extendedPitchStatus == RowStatus::NotReady);
    CHECK(failsWith(trySet(s.mib, {{status, Value::integer(1)}}), ErrorStatus::InconsistentValue, 0));  // not active yet

    // supplying the column completes it (notReady -> notInService), then it can be activated
    CHECK(!trySet(s.mib, {{value, Value::integer(12)}}));
    CHECK(s.data.attitude.extendedPitchTable.at(m::ExtendedPitchEntryIndex{5, "aft"}).extendedPitchStatus == RowStatus::NotInService);
    CHECK(!trySet(s.mib, {{status, Value::integer(1)}}));
    CHECK(s.mib.get(status) == Value::integer(1));

    // rows added by the application itself are complete from the start
    s.data.attitude.extendedPitchTable[{6, "app"}].extendedPitchStatus = RowStatus::NotInService;
    CHECK(!trySet(s.mib, {{cell(m::oids::extendedPitchStatus, m::ExtendedPitchEntryIndex{6, "app"}.toOid()), Value::integer(1)}}));
}

void testImportedTypesAndSingleGroup() {
    namespace g = nested_group_mib;
    g::Data d;
    d.attitude.boatRoll = 5000;  // the group survives although every object is inside it
    d.attitude.boatRollSensor = g::SensorKind::compass;  // named numbers of a TEXTUAL-CONVENTION from another MIB
    const auto bad = d.validate();
    CHECK(bad.size() == 1 && mentions(bad, "attitude.boatRoll: value must be in -1800..1800"));  // range of the imported TC

    Mib mib{g::oids::root};
    g::DataAgent adapter{mib, d};
    CHECK(failsWith(trySet(mib, {{g::oids::boatRollSensor + SubId{0}, Value::integer(4)}}), ErrorStatus::WrongValue, 0));
    CHECK(!trySet(mib, {{g::oids::boatRoll + SubId{0}, Value::integer(-900)}}));
    CHECK(d.attitude.boatRoll == -900);
    CHECK(std::string(g::toString(g::SensorKind::gps)) == "gps");
}

void testMessages() {
    Served s;
    std::vector<std::string> log;
    s.adapter.onChanged([&](m::Object o, const Oid& index) { log.push_back(std::string("value ") + m::toString(o) + "@" + index.str()); });
    s.adapter.onOwnboat([&](const m::Data& d) { log.push_back("ownboat " + d.boatName); });
    s.adapter.onNavigation([&](const m::Data::NavigationGroup& g) { log.push_back("navigation " + std::string(m::toString(g.navigationMode))); });
    s.adapter.onAttitude([&](const m::Data::AttitudeGroup& g) { log.push_back("attitude rows=" + std::to_string(g.extendedRollTable.size())); });
    s.adapter.onExtendedRollTableRow([&](const m::ExtendedRollEntryIndex& i, const m::ExtendedRollEntry& r) {
        log.push_back("roll[" + std::to_string(i.extendedRollIndex) + "] " + std::to_string(r.extendedRollValue) + " " + r.extendedRollSensor);
    });
    s.adapter.onExtendedPitchTableRow([&](const m::ExtendedPitchEntryIndex& i, const m::ExtendedPitchEntry& r) {
        log.push_back("pitch[" + i.extendedPitchChannel + "] " + std::to_string(static_cast<int>(r.extendedPitchStatus)));
    });
    const Oid roll1 = m::ExtendedRollEntryIndex{1}.toOid(), roll2 = m::ExtendedRollEntryIndex{2}.toOid();

    // a scalar directly in the module and one in a group, one request: each value, each touched subtree - once
    CHECK(!trySet(s.mib, {{scalar(m::oids::boatName), Value::string("orca")}, {scalar(m::oids::navigationMode), Value::integer(2)}}));
    CHECK((log == std::vector<std::string>{"value boatName@", "value navigationMode@", "navigation automatic", "ownboat orca"}));

    // two cells of row 1 and one of row 2: each row once, with all its new values; the attitude subtree once
    log.clear();
    CHECK(!trySet(s.mib, {{cell(m::oids::extendedRollValue, roll1), Value::integer(11)},
                          {cell(m::oids::extendedRollSensor, roll1), Value::string("g-new")},
                          {cell(m::oids::extendedRollValue, roll2), Value::integer(22)}}));
    CHECK((log == std::vector<std::string>{"value extendedRollValue@1", "value extendedRollSensor@1", "value extendedRollValue@2",
                                           "roll[1] 11 g-new", "roll[2] 22 gyro-2", "attitude rows=2", "ownboat orca"}));

    // nothing for a refused request: an invalid value (before anything is written) ...
    log.clear();
    CHECK(trySet(s.mib, {{scalar(m::oids::boatName), Value::string("x")}, {scalar(m::oids::navigationMode), Value::integer(9)}}));
    // ... or refused by onSet while writing (rolled back)
    s.adapter.onSet([](m::Object o, const Oid&) {
        if (o == m::Object::navigationMode) throw SetError(ErrorStatus::InconsistentValue, "no");
    });
    CHECK(trySet(s.mib, {{scalar(m::oids::boatName), Value::string("y")}, {scalar(m::oids::navigationMode), Value::integer(1)}}));
    CHECK(log.empty());
    CHECK(s.data.boatName == "orca");
    s.adapter.onSet(nullptr);

    // RowStatus: creating a row is a row message; destroying it reports the value and the subtree, but no row
    const Oid idx = m::ExtendedPitchEntryIndex{1, "aft"}.toOid();
    CHECK(!trySet(s.mib, {{cell(m::oids::extendedPitchValue, idx), Value::integer(5)}, {cell(m::oids::extendedPitchStatus, idx), Value::integer(4)}}));
    CHECK((log == std::vector<std::string>{"value extendedPitchStatus@1.3.97.102.116", "pitch[aft] 1", "attitude rows=2", "ownboat orca"}));
    log.clear();
    CHECK(!trySet(s.mib, {{cell(m::oids::extendedPitchStatus, idx), Value::integer(6)}}));
    CHECK((log == std::vector<std::string>{"value extendedPitchStatus@1.3.97.102.116", "attitude rows=2", "ownboat orca"}));
}

void testLock() {
    Served s;
    {
        auto guard = s.adapter.lock();  // holding it does not break reading the data in this thread
        s.data.boatName = "locked";
    }
    CHECK(s.mib.get(scalar(m::oids::boatName)) == Value::string("locked"));
}

}  // namespace

int main() {
    testNestedStructure();
    testValidate();
    testGet();
    testWalkOrder();
    testSet();
    testRowStatus();
    testHook();
    testGetHook();
    testRowCompleteness();
    testImportedTypesAndSingleGroup();
    testMessages();
    testLock();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
