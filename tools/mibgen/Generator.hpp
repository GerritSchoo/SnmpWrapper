#pragma once

// snmpwrap-mibgen: generates typed C++17 code for one MIB module from what is known about the loaded MIBs.

#include <string>

#include "ICodeGenerator.hpp"
#include "snmpwrap/IMibInfo.hpp"

namespace snmpwrap::mibgen {

/// The generator of snmpwrap-mibgen: Data, DataAgent, IInstrumentation, Remote, Notifications, send<Name>() ...
class Generator final : public ICodeGenerator {
public:
    Output generate(const IMibInfo& model, const Options& options) const override;

    /// @brief File and namespace name of a module: "SNMPWRAPPER-TEST-MIB" -> "snmpwrapper_test_mib".
    static std::string snakeCase(const std::string& module);
};

}  // namespace snmpwrap::mibgen
