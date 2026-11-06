#pragma once

// Interface IMibLoader of snmpwrap-mibgen: reading MIB files.

#include <memory>
#include <string>
#include <vector>

#include "snmpwrap/IMibInfo.hpp"

namespace snmpwrap::mibgen {

/// Reads MIB files - implemented by MibModelLoader; mock it in tests of Cli.
class IMibLoader {
public:
    virtual ~IMibLoader() = default;
    /// @brief Loads @p files, imports searched in @p mibDirs. @return What is known about them. @throws snmpwrap::Error
    virtual std::unique_ptr<IMibInfo> load(const std::vector<std::string>& files, const std::vector<std::string>& mibDirs) const = 0;

protected:
    IMibLoader() = default;
};

}  // namespace snmpwrap::mibgen
