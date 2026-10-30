// Shared by the tests of the code generated from NESTED-TEST-MIB.
#pragma once

#include <string>
#include <vector>

#include "nested_test_mib.hpp"  // generated from test/mibs/NESTED-TEST-MIB.txt

namespace snmpwrap::test {

/// A fully valid nested_test_mib::Data: every value inside the ranges of the MIB.
inline nested_test_mib::Data validData() {
    nested_test_mib::Data d;
    d.boatName = "aurora";
    d.navigation.navigationMode = nested_test_mib::NavigationMode::manual;
    d.attitude.extendedRollTable[1] = {124, "gyro-1", 7, 1000};
    d.attitude.extendedRollTable[2] = {-30, "gyro-2", 0, 0};
    d.navigation.extendedCourseTable[1].extendedCourseValue = 900;
    return d;
}

/// True if one of the messages contains @p what.
inline bool mentions(const std::vector<std::string>& messages, const std::string& what) {
    for (const std::string& s : messages)
        if (s.find(what) != std::string::npos) return true;
    return false;
}

}  // namespace snmpwrap::test
