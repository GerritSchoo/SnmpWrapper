// Unit tests for MibModel: loading MIB files with Net-SNMP's parser and the derived information.

#include <iostream>
#include <string>

#include "snmpwrap/mib_model.hpp"

using namespace snmpwrap;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(...)                                                                                  \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        if (!(__VA_ARGS__)) {                                                                       \
            ++g_failures;                                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n";      \
        }                                                                                           \
    } while (0)

#define CHECK_THROWS(expr, ExcType)                                                                 \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        bool thrown = false;                                                                        \
        try { expr; } catch (const ExcType&) { thrown = true; }                                     \
        if (!thrown) {                                                                              \
            ++g_failures;                                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected " #ExcType " from " #expr "\n"; \
        }                                                                                           \
    } while (0)

const std::string kMibs = SNMPWRAP_SOURCE_DIR "/mibs";
const std::string kTestMibs = SNMPWRAP_SOURCE_DIR "/test/mibs";
const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999");

void testExampleMib(const MibModel& m) {
    CHECK(m.modules().size() == 2 && m.modules()[0] == "SNMPWRAPPER-TEST-MIB");

    // scalars
    const MibNode& limit = m.node("swtLimit");
    CHECK(limit.oid == kRoot + Oid{1, 4});
    CHECK(limit.module == "SNMPWRAPPER-TEST-MIB");
    CHECK(limit.kind == MibNodeKind::Scalar);
    CHECK(limit.type == Type::Integer);
    CHECK(limit.access == MibAccess::ReadWrite);
    CHECK(limit.ranges.size() == 1 && limit.ranges[0].low == 1 && limit.ranges[0].high == 100);
    CHECK(limit.description.find("rejected with wrongValue") != std::string::npos);
    CHECK(&m.node("SNMPWRAPPER-TEST-MIB::swtLimit") == &limit);

    const MibNode& name = m.node("swtName");
    CHECK(name.type == Type::OctetString && name.textualConvention == "DisplayString");
    CHECK(name.ranges.size() == 1 && name.ranges[0].low == 0 && name.ranges[0].high == 64);
    CHECK(m.node("swtCounter").type == Type::Counter32 && m.node("swtCounter").access == MibAccess::ReadOnly);
    CHECK(m.node("swtUptime").type == Type::TimeTicks);
    CHECK(m.node("swtBigCounter").type == Type::Counter64);
    CHECK(m.node("swtScalars").kind == MibNodeKind::Other);
    CHECK(m.node("snmpWrapperTestMIB").oid == kRoot);

    // tables
    const MibNode& table = m.node("swtTable");
    CHECK(table.kind == MibNodeKind::Table && table.access == MibAccess::NotAccessible);
    CHECK(m.entryOf(table).name == "swtEntry" && m.entryOf(table).kind == MibNodeKind::Entry);
    auto cols = m.columns(table);
    CHECK(cols.size() == 4);  // incl. the not-accessible index column
    CHECK(cols.size() == 4 && cols[0]->name == "swtIndex" && cols[0]->access == MibAccess::NotAccessible);
    CHECK(cols.size() == 4 && cols[3]->name == "swtEntryStatus" && cols[3]->enums.size() == 3);
    CHECK(m.node("swtEntryStatus").enums.size() == 3 && m.node("swtEntryStatus").enums[1].label == "up");
    CHECK(m.indexSpecs(table).size() == 1 && m.indexSpecs(table)[0].kind == IndexKind::Integer);

    // RowStatus table
    const MibNode& rs = m.node("swtRowStatus");
    CHECK(rs.kind == MibNodeKind::Column && rs.textualConvention == "RowStatus" && rs.access == MibAccess::ReadCreate);
    CHECK(rs.enums.size() == 6 && rs.enums[3].label == "createAndGo" && rs.enums[3].value == 4);
    CHECK(m.node("swtRowValue").defaultValue == "0");
    CHECK(m.node("swtRowName").defaultValue.empty());

    // composite index with IMPLIED string
    auto conn = m.indexSpecs(m.node("swtConnTable"));
    CHECK(conn.size() == 3);
    CHECK(conn.size() == 3 && conn[0].kind == IndexKind::IpAddress && conn[1].kind == IndexKind::Integer &&
          conn[2].kind == IndexKind::ImpliedString);
    CHECK(m.entryOf(m.node("swtConnTable")).index.size() == 3 && m.entryOf(m.node("swtConnTable")).index[2].implied);
    CHECK(m.indexSpecs(m.node("swtBigTable"))[0].kind == IndexKind::Unsigned);

    // notification
    const MibNode& alarm = m.node("swtAlarm");
    CHECK(alarm.kind == MibNodeKind::Notification && alarm.oid == kRoot + Oid{3, 0, 1});
    CHECK(alarm.objects.size() == 2 && alarm.objects[0] == "swtName" && alarm.objects[1] == "swtLimit");

    // standard MIBs are available too (imported / Net-SNMP defaults)
    CHECK(m.find("sysDescr") && m.node("sysDescr").oid == Oid::parse("1.3.6.1.2.1.1.1"));
    CHECK(m.find("noSuchThing") == nullptr);
    CHECK_THROWS(m.node("noSuchThing"), Error);
    CHECK_THROWS(m.indexSpecs(limit), Error);

    // module listing
    auto objs = m.objects("SNMPWRAPPER-TEST-MIB");
    CHECK(!objs.empty() && objs.front()->name == "snmpWrapperTestMIB");
    bool sorted = true;
    for (std::size_t i = 1; i < objs.size(); ++i) sorted = sorted && objs[i - 1]->oid < objs[i]->oid;
    CHECK(sorted);
}

void testModelTestMib(const MibModel& m) {
    const MibNode& port = m.node("swmPort");
    CHECK(port.type == Type::Gauge32 && port.units == "port" && port.defaultValue == "161");
    CHECK(port.ranges.size() == 2);
    CHECK(port.ranges.size() == 2 && port.ranges[1].low == 4000000000LL && port.ranges[1].high == 4294967295LL);
    CHECK(m.node("swmTemperature").ranges.size() == 1 && m.node("swmTemperature").ranges[0].low == -40);
    CHECK(m.node("swmEnabled").textualConvention == "TruthValue" && m.node("swmEnabled").enums.size() == 2);

    auto keys = m.indexSpecs(m.node("swmKeyTable"));
    CHECK(keys.size() == 2);
    CHECK(keys.size() == 2 && keys[0].kind == IndexKind::FixedString && keys[0].size == 6);
    CHECK(keys.size() == 2 && keys[1].kind == IndexKind::ImpliedObjectId);

    // AUGMENTS: index comes from swtRowEntry of the other module
    const MibNode& ext = m.entryOf(m.node("swmRowExtTable"));
    CHECK(ext.augments == "swtRowEntry");
    CHECK(ext.index.size() == 1 && ext.index[0].name == "swtRowIndex");
    CHECK(m.indexSpecs(m.node("swmRowExtTable")).size() == 1);

    CHECK(m.node("swmPortChanged").objects.size() == 2);
}

void testValidate(const MibModel& m) {
    const MibNode& limit = m.node("swtLimit");
    m.validate(limit, Value::integer(1));
    m.validate(limit, Value::integer(100));
    auto status = [&](const MibNode& n, const Value& v) {
        try {
            m.validate(n, v);
        } catch (const SetError& e) {
            return e.status();
        }
        return ErrorStatus::NoError;
    };
    CHECK(status(limit, Value::integer(0)) == ErrorStatus::WrongValue);
    CHECK(status(limit, Value::integer(101)) == ErrorStatus::WrongValue);
    CHECK(status(limit, Value::gauge(5)) == ErrorStatus::WrongType);
    CHECK(status(m.node("swtName"), Value::string(std::string(64, 'x'))) == ErrorStatus::NoError);
    CHECK(status(m.node("swtName"), Value::string(std::string(65, 'x'))) == ErrorStatus::WrongLength);
    CHECK(status(m.node("swtEntryStatus"), Value::integer(2)) == ErrorStatus::NoError);
    CHECK(status(m.node("swtEntryStatus"), Value::integer(4)) == ErrorStatus::WrongValue);  // not an enum value
    const MibNode& port = m.node("swmPort");
    CHECK(status(port, Value::gauge(161)) == ErrorStatus::NoError);
    CHECK(status(port, Value::gauge(70000)) == ErrorStatus::WrongValue);       // between the ranges
    CHECK(status(port, Value::gauge(4294967295u)) == ErrorStatus::NoError);   // upper range, > 2^31
    CHECK(status(m.node("swmTemperature"), Value::integer(-41)) == ErrorStatus::WrongValue);
    CHECK(status(m.node("swmTemperature"), Value::integer(-40)) == ErrorStatus::NoError);
}

void testResolveFormatParse(const MibModel& m) {
    CHECK(m.resolve("swtLimit.0") == kRoot + Oid{1, 4, 0});
    CHECK(m.resolve("SNMPWRAPPER-TEST-MIB::swtRowName.5") == kRoot + Oid{4, 1, 2, 5});
    CHECK(m.resolve("1.3.6.1.2.1.1.1.0") == Oid::parse("1.3.6.1.2.1.1.1.0"));
    CHECK(m.resolve(".1.3.6") == Oid{1, 3, 6});
    CHECK(m.resolve("swtConnState.10.0.0.1.80.'web'") == kRoot + Oid{5, 1, 4, 10, 0, 0, 1, 80, 'w', 'e', 'b'});
    CHECK(m.resolve("swtConnState.10.0.0.1.80.\"web\"") == kRoot + Oid{5, 1, 4, 10, 0, 0, 1, 80, 3, 'w', 'e', 'b'});
    CHECK(m.resolve("  sysDescr.0 ") == Oid::parse("1.3.6.1.2.1.1.1.0"));
    CHECK_THROWS(m.resolve("nope.0"), Error);
    CHECK_THROWS(m.resolve("swtLimit.0."), Error);
    CHECK_THROWS(m.resolve("swtLimit.x"), Error);
    CHECK_THROWS(m.resolve("swtConnState.'web"), Error);
    CHECK_THROWS(m.resolve(""), Error);

    CHECK(m.nodeFor(kRoot + Oid{4, 1, 4, 5})->name == "swtRowStatus");
    CHECK(m.findByOid(kRoot + Oid{4, 1, 4})->name == "swtRowStatus");
    CHECK(m.findByOid(kRoot + Oid{4, 1, 4, 5}) == nullptr);

    CHECK(m.format({kRoot + Oid{4, 1, 4, 5}, Value::integer(1)}) == "swtRowStatus.5 = active(1)");
    CHECK(m.format({kRoot + Oid{1, 1, 0}, Value::string("hi")}) == "swtName.0 = \"hi\"");
    CHECK(m.format({kRoot + Oid{1, 4, 0}, Value::integer(7)}) == "swtLimit.0 = 7");
    CHECK(m.format({kRoot + Oid{1, 5, 0}, Value::timeTicks(153)}) == "swtUptime.0 = (153) 0:00:01.53");
    CHECK(m.format({kRoot + Oid{1, 5, 0}, Value::timeTicks(8640000u + 360000u)}) == "swtUptime.0 = (9000000) 1 day, 1:00:00.00");
    CHECK(m.format({m.oid("swmTemperature") + SubId{0}, Value::integer(21)}) == "swmTemperature.0 = 21 degrees Celsius");
    CHECK(m.format({kRoot + Oid{4, 1, 2, 9}, Value::exception(Type::NoSuchInstance)}) == "swtRowName.9 = NoSuchInstance");
    CHECK(m.format({Oid{9, 9, 9}, Value::integer(1)}) == "9.9.9 = 1");

    CHECK(m.parseValue("swtLimit", "75") == Value::integer(75));
    CHECK(m.parseValue("swtRowStatus.5", "createAndGo") == Value::integer(4));
    CHECK(m.parseValue("swtRowStatus.5", "destroy(6)") == Value::integer(6));
    CHECK(m.parseValue("swtRowStatus.5", "4") == Value::integer(4));
    CHECK(m.parseValue("swtName", "\"quoted\"") == Value::string("quoted"));
    // SMI hex / binary notation (as used in DEFVAL)
    CHECK(m.parseValue("swtName", "'4142'H") == Value::string("AB"));
    CHECK(m.parseValue("swtName", "'00ff'h") == Value::string(std::string("\x00\xff", 2)));
    CHECK(m.parseValue("swtName", "''H") == Value::string(""));
    CHECK(m.parseValue("swtName", "'0100000101000010'B") == Value::string("AB"));
    CHECK_THROWS(m.parseValue("swtName", "'414'H"), Error);   // half octet
    CHECK_THROWS(m.parseValue("swtName", "'41G2'H"), Error);  // not hex
    CHECK(m.parseValue("swmTemperature", "-12") == Value::integer(-12));
    CHECK(m.parseValue("swmPort", "4294967295") == Value::gauge(4294967295u));
    CHECK(m.parseValue("swtBigCounter", "18446744073709551615") == Value::counter64(18446744073709551615ULL));
    CHECK_THROWS(m.parseValue("swtLimit", "500"), SetError);    // range
    CHECK_THROWS(m.parseValue("swtLimit", "abc"), Error);       // no number
    CHECK_THROWS(m.parseValue("swtLimit", "99999999999"), Error);
    CHECK_THROWS(m.parseValue("swtRowStatus", "sleeping"), Error);
    CHECK_THROWS(m.parseValue("swmPort", "-1"), Error);
    CHECK_THROWS(m.parseValue("swtTable", "1"), Error);         // not a scalar / column
}

void testBrokenMibs() {
    try {
        MibModel::load({kTestMibs + "/broken/BROKEN-SYNTAX-MIB.txt"});
        CHECK(!"syntax error not reported");
    } catch (const Error& e) {
        std::string what = e.what();
        CHECK(what.find("loading MIB failed") != std::string::npos);
        CHECK(what.find("BROKEN-SYNTAX-MIB.txt") != std::string::npos);  // Net-SNMP names the file and line
    }
    try {
        MibModel::load({kTestMibs + "/broken/BROKEN-IMPORT-MIB.txt"});
        CHECK(!"missing import not reported");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("NO-SUCH-MODULE-MIB") != std::string::npos);
    }
    CHECK_THROWS(MibModel::load({kTestMibs + "/does-not-exist.txt"}), Error);
    CHECK_THROWS(MibModel::loadModules({"NO-SUCH-MODULE-MIB"}), Error);
}

}  // namespace

int main() {
    try {
        // the model-test MIB imports from SNMPWRAPPER-TEST-MIB, found through the extra MIB directory
        MibModel m = MibModel::load({kMibs + "/SNMPWRAPPER-TEST-MIB.txt", kTestMibs + "/SNMPWRAPPER-MODEL-TEST-MIB.txt"},
                                    {kMibs});
        testExampleMib(m);
        testModelTestMib(m);
        testValidate(m);
        testResolveFormatParse(m);
        // standard module by name
        MibModel ifm = MibModel::loadModules({"IF-MIB"});
        CHECK(ifm.node("ifDescr").oid == Oid::parse("1.3.6.1.2.1.2.2.1.2"));
        CHECK(ifm.indexSpecs(ifm.node("ifTable"))[0].kind == IndexKind::Integer);
        testBrokenMibs();
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << "\n";
        return 1;
    }
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
