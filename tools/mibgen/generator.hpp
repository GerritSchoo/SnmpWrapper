#pragma once

// snmpwrap-mibgen: generates typed C++17 code for one MIB module from a MibModel.

#include <string>
#include <vector>

#include "snmpwrap/mib_model.hpp"

namespace snmpwrap::mibgen {

struct Options {
    std::string module;      ///< MIB module to generate, e.g. "SNMPWRAPPER-TEST-MIB".
    std::string baseName;    ///< Output file base name and C++ namespace; default: module in snake_case.
    std::string rootName;    ///< Optional MIB node used as registration root; default: common prefix of all objects.
    std::string sourceInfo;  ///< Text for the "generated from" header line (e.g. the MIB file names).
};

struct Output {
    std::string header;  ///< contents of <baseName>.hpp
    std::string source;  ///< contents of <baseName>.cpp
};

/// Generates the code. Throws snmpwrap::Error if the module cannot be generated.
Output generate(const MibModel& model, const Options& options);

/// "SNMPWRAPPER-TEST-MIB" -> "snmpwrapper_test_mib"
std::string snakeCase(const std::string& module);

}  // namespace snmpwrap::mibgen
