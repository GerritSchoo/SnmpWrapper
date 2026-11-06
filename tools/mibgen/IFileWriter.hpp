#pragma once

// Interface IFileWriter of snmpwrap-mibgen: writing the generated files.

#include <filesystem>
#include <string>

namespace snmpwrap::mibgen {

/// Writes generated files - implemented by ChangedFileWriter; mock it in tests of Cli.
class IFileWriter {
public:
    virtual ~IFileWriter() = default;
    /// @brief Writes @p content to @p path (creating its directory). @return False if it cannot be written.
    virtual bool write(const std::filesystem::path& path, const std::string& content) = 0;

protected:
    IFileWriter() = default;
};

}  // namespace snmpwrap::mibgen
