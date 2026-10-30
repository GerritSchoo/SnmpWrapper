// Generated operator<< for Data and its parts (NESTED-TEST-MIB, SNMPWRAPPER-TEST-MIB): MIB order, nesting,
// enums by label, escaped text, raw bytes in hex, BITS by name.

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "generated/nested_data.hpp"
#include "nested_test_mib.hpp"
#include "snmpwrapper_test_mib.hpp"

namespace m = nested_test_mib;
using namespace snmpwrap;
using snmpwrap::test::validData;

namespace {

template <typename T>
std::string text(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

class TextOutputTest : public ::testing::Test {
protected:
    TextOutputTest() { d.attitude.extendedPitchTable[{3, "port"}] = {-5, RowStatus::Active}; }
    m::Data d = validData();
};

TEST_F(TextOutputTest, WholeDataInMibOrderNestedAndIndented) {
    EXPECT_EQ(text(d),
              "boatName = \"aurora\"\n"
              "attitude\n"
              "  extendedRollTable (2 rows)\n"
              "    [1]\n"
              "      extendedRollValue = 124\n"
              "      extendedRollSensor = \"gyro-1\"\n"
              "      extendedRollCounter = 7\n"
              "      extendedRollTimepoint = 1000\n"
              "    [2]\n"
              "      extendedRollValue = -30\n"
              "      extendedRollSensor = \"gyro-2\"\n"
              "      extendedRollCounter = 0\n"
              "      extendedRollTimepoint = 0\n"
              "  extendedPitchTable (1 row)\n"
              "    [3, \"port\"]\n"
              "      extendedPitchValue = -5\n"
              "      extendedPitchStatus = active(1)\n"
              "navigation\n"
              "  navigationMode = manual(1)\n"
              "  extendedCourseTable (1 row)\n"
              "    [1]\n"
              "      extendedCourseValue = 900\n");
}

TEST_F(TextOutputTest, GroupTableAndRow) {
    EXPECT_EQ(text(d.navigation),
              "navigationMode = manual(1)\n"
              "extendedCourseTable (1 row)\n"
              "  [1]\n"
              "    extendedCourseValue = 900\n");
    EXPECT_EQ(text(d.attitude.extendedPitchTable),
              "[3, \"port\"]\n"
              "  extendedPitchValue = -5\n"
              "  extendedPitchStatus = active(1)\n");
    EXPECT_EQ(text(d.attitude.extendedRollTable[2]),
              "extendedRollValue = -30\n"
              "extendedRollSensor = \"gyro-2\"\n"
              "extendedRollCounter = 0\n"
              "extendedRollTimepoint = 0\n");
    EXPECT_TRUE(text(m::Data{}.navigation.extendedCourseTable).empty());
}

TEST(TextOutput, IndexAndEnum) {
    EXPECT_EQ(text(m::ExtendedPitchEntryIndex{3, "port"}), "3, \"port\"");
    EXPECT_EQ(text(m::NavigationMode::automatic), "automatic(2)");
    EXPECT_EQ(text(static_cast<m::NavigationMode>(7)), "?(7)");
}

TEST(TextOutput, ValueKinds) {  // escaped text, bytes that are no text, BITS by name, Counter64, IpAddress in an index
    namespace t = snmpwrapper_test_mib;
    t::Data s;
    s.swtScalars.swtName = "say \"hi\"\\ \xc3\xa4";      // UTF-8 stays text
    s.swtScalars.swtFlags = std::string("\xa0\x01", 2);  // bits 0, 2 and the unnamed bit 15
    s.swtScalars.swtBigCounter = 4294967297u;
    s.swtTable[1] = {"bin\x01", 5, t::SwtEntryStatus::up};
    s.swtConnTable[{{10, 0, 0, 1}, 80, "web"}].swtConnState = t::SwtConnState::listen;
    EXPECT_EQ(text(s),
              "swtScalars\n"
              "  swtName = \"say \\\"hi\\\"\\\\ \xc3\xa4\"\n"
              "  swtCounter = 0\n"
              "  swtGauge = 0\n"
              "  swtLimit = 0\n"
              "  swtUptime = 0\n"
              "  swtBigCounter = 4294967297\n"
              "  swtFlags = {red, blue, 15}\n"
              "swtTable (1 row)\n"
              "  [1]\n"
              "    swtEntryName = 0x62696e01\n"
              "    swtEntryValue = 5\n"
              "    swtEntryStatus = up(2)\n"
              "swtRowTable (0 rows)\n"
              "swtConnTable (1 row)\n"
              "  [10.0.0.1, 80, \"web\"]\n"
              "    swtConnState = listen(2)\n"
              "swtBigTable (0 rows)\n");
}

}  // namespace
