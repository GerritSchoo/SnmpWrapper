// MibModel (mib_model.hpp): loading MIB files with Net-SNMP's parser and the derived information,
// validate(), resolve(), format(), parseValue() and the diagnostics of broken MIBs.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "snmpwrap/mib_model.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;

namespace {

const std::string kTestMibs = SNMPWRAP_SOURCE_DIR "/test/mibs";
const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999");

/// SNMPWRAPPER-TEST-MIB + SNMPWRAPPER-MODEL-TEST-MIB, loaded once for all tests of the suite.
class MibModelTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // the model-test MIB imports from SNMPWRAPPER-TEST-MIB, found through the extra MIB directory
        model_ = std::make_unique<MibModel>(MibModel::load(
            {kTestMibs + "/SNMPWRAPPER-TEST-MIB.txt", kTestMibs + "/SNMPWRAPPER-MODEL-TEST-MIB.txt"}, {kTestMibs}));
    }
    static void TearDownTestSuite() { model_.reset(); }

    /// Status of validate(): NoError if the value is accepted.
    static ErrorStatus status(const MibNode& n, const Value& v) {
        try {
            model_->validate(n, v);
        } catch (const SetError& e) {
            return e.status();
        }
        return ErrorStatus::NoError;
    }

    const MibModel& m = *model_;
    static std::unique_ptr<MibModel> model_;
};

std::unique_ptr<MibModel> MibModelTest::model_;

// --- SNMPWRAPPER-TEST-MIB -----------------------------------------------------------------------

TEST_F(MibModelTest, Scalars) {
    const MibNode& limit = m.node("swtLimit");
    EXPECT_EQ(limit.oid, kRoot + Oid({1, 4}));
    EXPECT_EQ(limit.module, "SNMPWRAPPER-TEST-MIB");
    EXPECT_EQ(limit.kind, MibNodeKind::Scalar);
    EXPECT_EQ(limit.type, Type::Integer);
    EXPECT_EQ(limit.access, MibAccess::ReadWrite);
    ASSERT_EQ(limit.ranges.size(), 1u);
    EXPECT_EQ(limit.ranges[0].low, 1);
    EXPECT_EQ(limit.ranges[0].high, 100);
    EXPECT_NE(limit.description.find("rejected with wrongValue"), std::string::npos);
    EXPECT_EQ(&m.node("SNMPWRAPPER-TEST-MIB::swtLimit"), &limit);

    const MibNode& name = m.node("swtName");
    EXPECT_EQ(name.type, Type::OctetString);
    EXPECT_EQ(name.textualConvention, "DisplayString");
    ASSERT_EQ(name.ranges.size(), 1u);
    EXPECT_EQ(name.ranges[0].low, 0);
    EXPECT_EQ(name.ranges[0].high, 64);
    EXPECT_EQ(m.node("swtCounter").type, Type::Counter32);
    EXPECT_EQ(m.node("swtCounter").access, MibAccess::ReadOnly);
    EXPECT_EQ(m.node("swtUptime").type, Type::TimeTicks);
    EXPECT_EQ(m.node("swtBigCounter").type, Type::Counter64);
    EXPECT_EQ(m.node("swtScalars").kind, MibNodeKind::Other);
    EXPECT_EQ(m.node("snmpWrapperTestMIB").oid, kRoot);
}

TEST_F(MibModelTest, Tables) {
    const MibNode& table = m.node("swtTable");
    EXPECT_EQ(table.kind, MibNodeKind::Table);
    EXPECT_EQ(table.access, MibAccess::NotAccessible);
    EXPECT_EQ(m.entryOf(table).name, "swtEntry");
    EXPECT_EQ(m.entryOf(table).kind, MibNodeKind::Entry);
    const auto cols = m.columns(table);
    ASSERT_EQ(cols.size(), 4u);  // incl. the not-accessible index column
    EXPECT_EQ(cols[0]->name, "swtIndex");
    EXPECT_EQ(cols[0]->access, MibAccess::NotAccessible);
    EXPECT_EQ(cols[3]->name, "swtEntryStatus");
    EXPECT_EQ(cols[3]->enums.size(), 3u);
    ASSERT_EQ(m.node("swtEntryStatus").enums.size(), 3u);
    EXPECT_EQ(m.node("swtEntryStatus").enums[1].label, "up");
    ASSERT_EQ(m.indexSpecs(table).size(), 1u);
    EXPECT_EQ(m.indexSpecs(table)[0].kind, IndexKind::Integer);
}

TEST_F(MibModelTest, RowStatusColumnAndDefaults) {
    const MibNode& rs = m.node("swtRowStatus");
    EXPECT_EQ(rs.kind, MibNodeKind::Column);
    EXPECT_EQ(rs.textualConvention, "RowStatus");
    EXPECT_EQ(rs.access, MibAccess::ReadCreate);
    ASSERT_EQ(rs.enums.size(), 6u);
    EXPECT_EQ(rs.enums[3].label, "createAndGo");
    EXPECT_EQ(rs.enums[3].value, 4);
    EXPECT_EQ(m.node("swtRowValue").defaultValue, "0");
    EXPECT_TRUE(m.node("swtRowName").defaultValue.empty());
}

TEST_F(MibModelTest, CompositeIndexWithImpliedString) {
    const auto conn = m.indexSpecs(m.node("swtConnTable"));
    ASSERT_EQ(conn.size(), 3u);
    EXPECT_EQ(conn[0].kind, IndexKind::IpAddress);
    EXPECT_EQ(conn[1].kind, IndexKind::Integer);
    EXPECT_EQ(conn[2].kind, IndexKind::ImpliedString);
    ASSERT_EQ(m.entryOf(m.node("swtConnTable")).index.size(), 3u);
    EXPECT_TRUE(m.entryOf(m.node("swtConnTable")).index[2].implied);
    EXPECT_EQ(m.indexSpecs(m.node("swtBigTable"))[0].kind, IndexKind::Unsigned);
}

TEST_F(MibModelTest, Notification) {
    const MibNode& alarm = m.node("swtAlarm");
    EXPECT_EQ(alarm.kind, MibNodeKind::Notification);
    EXPECT_EQ(alarm.oid, kRoot + Oid({3, 0, 1}));
    ASSERT_EQ(alarm.objects.size(), 2u);
    EXPECT_EQ(alarm.objects[0], "swtName");
    EXPECT_EQ(alarm.objects[1], "swtLimit");
}

TEST_F(MibModelTest, LookupOfStandardAndUnknownNames) {
    // standard MIBs are available too (imported / Net-SNMP defaults)
    ASSERT_NE(m.find("sysDescr"), nullptr);
    EXPECT_EQ(m.node("sysDescr").oid, Oid::parse("1.3.6.1.2.1.1.1"));
    EXPECT_EQ(m.find("noSuchThing"), nullptr);
    EXPECT_THROW(m.node("noSuchThing"), Error);
    EXPECT_THROW(m.indexSpecs(m.node("swtLimit")), Error);  // not a table
}

TEST_F(MibModelTest, ModuleObjectsAreInOidOrder) {
    const auto objs = m.objects("SNMPWRAPPER-TEST-MIB");
    ASSERT_FALSE(objs.empty());
    EXPECT_EQ(objs.front()->name, "snmpWrapperTestMIB");
    for (std::size_t i = 1; i < objs.size(); ++i) EXPECT_LT(objs[i - 1]->oid, objs[i]->oid) << objs[i]->name;
}

// --- SNMPWRAPPER-MODEL-TEST-MIB -----------------------------------------------------------------

TEST_F(MibModelTest, RangesUnitsAndTextualConventions) {
    const MibNode& port = m.node("swmPort");
    EXPECT_EQ(port.type, Type::Gauge32);
    EXPECT_EQ(port.units, "port");
    EXPECT_EQ(port.defaultValue, "161");
    ASSERT_EQ(port.ranges.size(), 2u);
    EXPECT_EQ(port.ranges[1].low, 4000000000LL);
    EXPECT_EQ(port.ranges[1].high, 4294967295LL);
    ASSERT_EQ(m.node("swmTemperature").ranges.size(), 1u);
    EXPECT_EQ(m.node("swmTemperature").ranges[0].low, -40);
    EXPECT_EQ(m.node("swmEnabled").textualConvention, "TruthValue");
    EXPECT_EQ(m.node("swmEnabled").enums.size(), 2u);
}

TEST_F(MibModelTest, FixedStringAndImpliedObjectIdIndex) {
    const auto keys = m.indexSpecs(m.node("swmKeyTable"));
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].kind, IndexKind::FixedString);
    EXPECT_EQ(keys[0].size, 6u);
    EXPECT_EQ(keys[1].kind, IndexKind::ImpliedObjectId);
}

TEST_F(MibModelTest, AugmentsTakesTheIndexOfTheOtherModule) {
    const MibNode& ext = m.entryOf(m.node("swmRowExtTable"));
    EXPECT_EQ(ext.augments, "swtRowEntry");
    ASSERT_EQ(ext.index.size(), 1u);
    EXPECT_EQ(ext.index[0].name, "swtRowIndex");
    EXPECT_EQ(m.indexSpecs(m.node("swmRowExtTable")).size(), 1u);
    EXPECT_EQ(m.node("swmPortChanged").objects.size(), 2u);
}

// --- validate -----------------------------------------------------------------------------------

TEST_F(MibModelTest, ValidateRangeTypeLengthAndEnum) {
    const MibNode& limit = m.node("swtLimit");
    EXPECT_NO_THROW(m.validate(limit, Value::integer(1)));
    EXPECT_NO_THROW(m.validate(limit, Value::integer(100)));
    EXPECT_EQ(status(limit, Value::integer(0)), ErrorStatus::WrongValue);
    EXPECT_EQ(status(limit, Value::integer(101)), ErrorStatus::WrongValue);
    EXPECT_EQ(status(limit, Value::gauge(5)), ErrorStatus::WrongType);
    EXPECT_EQ(status(m.node("swtName"), Value::string(std::string(64, 'x'))), ErrorStatus::NoError);
    EXPECT_EQ(status(m.node("swtName"), Value::string(std::string(65, 'x'))), ErrorStatus::WrongLength);
    EXPECT_EQ(status(m.node("swtEntryStatus"), Value::integer(2)), ErrorStatus::NoError);
    EXPECT_EQ(status(m.node("swtEntryStatus"), Value::integer(4)), ErrorStatus::WrongValue);  // not an enum value
}

TEST_F(MibModelTest, ValidateSeveralRangesAndNegativeBounds) {
    const MibNode& port = m.node("swmPort");
    EXPECT_EQ(status(port, Value::gauge(161)), ErrorStatus::NoError);
    EXPECT_EQ(status(port, Value::gauge(70000)), ErrorStatus::WrongValue);     // between the ranges
    EXPECT_EQ(status(port, Value::gauge(4294967295u)), ErrorStatus::NoError);  // upper range, > 2^31
    EXPECT_EQ(status(m.node("swmTemperature"), Value::integer(-41)), ErrorStatus::WrongValue);
    EXPECT_EQ(status(m.node("swmTemperature"), Value::integer(-40)), ErrorStatus::NoError);
}

// --- resolve / format / parseValue --------------------------------------------------------------

TEST_F(MibModelTest, ResolveNamesAndIndexes) {
    EXPECT_EQ(m.resolve("swtLimit.0"), kRoot + Oid({1, 4, 0}));
    EXPECT_EQ(m.resolve("SNMPWRAPPER-TEST-MIB::swtRowName.5"), kRoot + Oid({4, 1, 2, 5}));
    EXPECT_EQ(m.resolve("1.3.6.1.2.1.1.1.0"), Oid::parse("1.3.6.1.2.1.1.1.0"));
    EXPECT_EQ(m.resolve(".1.3.6"), (Oid({1, 3, 6})));
    EXPECT_EQ(m.resolve("swtConnState.10.0.0.1.80.'web'"), kRoot + Oid({5, 1, 4, 10, 0, 0, 1, 80, 'w', 'e', 'b'}));
    EXPECT_EQ(m.resolve("swtConnState.10.0.0.1.80.\"web\""), kRoot + Oid({5, 1, 4, 10, 0, 0, 1, 80, 3, 'w', 'e', 'b'}));
    EXPECT_EQ(m.resolve("  sysDescr.0 "), Oid::parse("1.3.6.1.2.1.1.1.0"));
}

TEST_F(MibModelTest, ResolveRejectsMalformedText) {
    EXPECT_THROW(m.resolve("nope.0"), Error);
    EXPECT_THROW(m.resolve("swtLimit.0."), Error);
    EXPECT_THROW(m.resolve("swtLimit.x"), Error);
    EXPECT_THROW(m.resolve("swtConnState.'web"), Error);
    EXPECT_THROW(m.resolve(""), Error);
}

TEST_F(MibModelTest, NodeForAndFindByOid) {
    EXPECT_EQ(m.nodeFor(kRoot + Oid({4, 1, 4, 5}))->name, "swtRowStatus");
    EXPECT_EQ(m.findByOid(kRoot + Oid({4, 1, 4}))->name, "swtRowStatus");
    EXPECT_EQ(m.findByOid(kRoot + Oid({4, 1, 4, 5})), nullptr);
}

TEST_F(MibModelTest, FormatUsesEnumsTimeTicksAndUnits) {
    EXPECT_EQ(m.format({kRoot + Oid({4, 1, 4, 5}), Value::integer(1)}), "swtRowStatus.5 = active(1)");
    EXPECT_EQ(m.format({kRoot + Oid({1, 1, 0}), Value::string("hi")}), "swtName.0 = \"hi\"");
    EXPECT_EQ(m.format({kRoot + Oid({1, 4, 0}), Value::integer(7)}), "swtLimit.0 = 7");
    EXPECT_EQ(m.format({kRoot + Oid({1, 5, 0}), Value::timeTicks(153)}), "swtUptime.0 = (153) 0:00:01.53");
    EXPECT_EQ(m.format({kRoot + Oid({1, 5, 0}), Value::timeTicks(8640000u + 360000u)}), "swtUptime.0 = (9000000) 1 day, 1:00:00.00");
    EXPECT_EQ(m.format({m.oid("swmTemperature") + SubId{0}, Value::integer(21)}), "swmTemperature.0 = 21 degrees Celsius");
    EXPECT_EQ(m.format({kRoot + Oid({4, 1, 2, 9}), Value::exception(Type::NoSuchInstance)}), "swtRowName.9 = NoSuchInstance");
    EXPECT_EQ(m.format({Oid({9, 9, 9}), Value::integer(1)}), "9.9.9 = 1");
}

TEST_F(MibModelTest, ParseValueNumbersEnumsAndStrings) {
    EXPECT_EQ(m.parseValue("swtLimit", "75"), Value::integer(75));
    EXPECT_EQ(m.parseValue("swtRowStatus.5", "createAndGo"), Value::integer(4));
    EXPECT_EQ(m.parseValue("swtRowStatus.5", "destroy(6)"), Value::integer(6));
    EXPECT_EQ(m.parseValue("swtRowStatus.5", "4"), Value::integer(4));
    EXPECT_EQ(m.parseValue("swtName", "\"quoted\""), Value::string("quoted"));
    EXPECT_EQ(m.parseValue("swmTemperature", "-12"), Value::integer(-12));
    EXPECT_EQ(m.parseValue("swmPort", "4294967295"), Value::gauge(4294967295u));
    EXPECT_EQ(m.parseValue("swtBigCounter", "18446744073709551615"), Value::counter64(18446744073709551615ULL));
}

TEST_F(MibModelTest, ParseValueSmiHexAndBinaryNotation) {  // as used in DEFVAL
    EXPECT_EQ(m.parseValue("swtName", "'4142'H"), Value::string("AB"));
    EXPECT_EQ(m.parseValue("swtName", "'00ff'h"), Value::string(std::string("\x00\xff", 2)));
    EXPECT_EQ(m.parseValue("swtName", "''H"), Value::string(""));
    EXPECT_EQ(m.parseValue("swtName", "'0100000101000010'B"), Value::string("AB"));
    EXPECT_THROW(m.parseValue("swtName", "'414'H"), Error);   // half octet
    EXPECT_THROW(m.parseValue("swtName", "'41G2'H"), Error);  // not hex
}

TEST_F(MibModelTest, ParseValueRejectsInvalidText) {
    EXPECT_THROW(m.parseValue("swtLimit", "500"), SetError);  // range
    EXPECT_THROW(m.parseValue("swtLimit", "abc"), Error);     // no number
    EXPECT_THROW(m.parseValue("swtLimit", "99999999999"), Error);
    EXPECT_THROW(m.parseValue("swtRowStatus", "sleeping"), Error);
    EXPECT_THROW(m.parseValue("swmPort", "-1"), Error);
    EXPECT_THROW(m.parseValue("swtTable", "1"), Error);  // not a scalar / column
}

// --- broken MIBs (no shared model) --------------------------------------------------------------

TEST(MibModelLoad, SyntaxErrorNamesTheFile) {
    try {
        MibModel::load({kTestMibs + "/broken/BROKEN-SYNTAX-MIB.txt"});
        FAIL() << "syntax error not reported";
    } catch (const Error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("loading MIB failed"), std::string::npos) << what;
        EXPECT_NE(what.find("BROKEN-SYNTAX-MIB.txt"), std::string::npos) << what;  // Net-SNMP names the file and line
    }
}

TEST(MibModelLoad, MissingImportIsReported) {
    try {
        MibModel::load({kTestMibs + "/broken/BROKEN-IMPORT-MIB.txt"});
        FAIL() << "missing import not reported";
    } catch (const Error& e) {
        EXPECT_NE(std::string(e.what()).find("NO-SUCH-MODULE-MIB"), std::string::npos) << e.what();
    }
}

TEST(MibModelLoad, MissingFileThrows) { EXPECT_THROW(MibModel::load({kTestMibs + "/does-not-exist.txt"}), Error); }

}  // namespace
