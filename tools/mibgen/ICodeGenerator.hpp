#pragma once

// Interface ICodeGenerator of snmpwrap-mibgen and what it takes and gives: Options, Output.

#include <string>

#include "snmpwrap/IMibInfo.hpp"

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

/// Turns a MIB module into C++ code - implemented by Generator; mock it in tests of what uses it (Cli).
class ICodeGenerator {
public:
    virtual ~ICodeGenerator() = default;
    /// @brief Generates the code of @p options.module. @throws snmpwrap::Error if the module cannot be generated.
    virtual Output generate(const IMibInfo& model, const Options& options) const = 0;

protected:
    ICodeGenerator() = default;
    ICodeGenerator(const ICodeGenerator&) = default;
    ICodeGenerator& operator=(const ICodeGenerator&) = default;
};

}  // namespace snmpwrap::mibgen
