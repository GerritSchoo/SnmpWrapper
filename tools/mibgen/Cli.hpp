#pragma once

// The command line of snmpwrap-mibgen, separate from main() so that it can be tested:
//   snmpwrap-mibgen --module <MODULE> --out <dir> [--name <base>] [--root <node>] [--mib-dir <dir>]... <mib-file>...

#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "Generator.hpp"
#include "IFileWriter.hpp"
#include "IMibLoader.hpp"

namespace snmpwrap::mibgen {

/// What the command line asks for.
struct CliArgs {
    Options options;                   ///< module, baseName (--name), rootName (--root); sourceInfo from the file names
    std::string outDir;                ///< --out
    std::vector<std::string> mibDirs;  ///< --mib-dir, in order
    std::vector<std::string> files;    ///< the MIB files, in order
};

/// Loads MIB files with Net-SNMP's parser (MibModel::load()).
class MibModelLoader final : public IMibLoader {
public:
    std::unique_ptr<IMibInfo> load(const std::vector<std::string>& files, const std::vector<std::string>& mibDirs) const override;
};

/// Writes a file only when its content changes, so builds stay incremental.
class ChangedFileWriter final : public IFileWriter {
public:
    bool write(const std::filesystem::path& path, const std::string& content) override;
};

/// The snmpwrap-mibgen command.
class Cli {
public:
    /// @brief With the real parts: Net-SNMP's MIB parser, the Generator, files on disk. @param[in] err Messages.
    explicit Cli(std::ostream& err);
    /// @brief With given parts (tests). @param[in] err Messages. @param[in] loader Reads the MIBs.
    /// @param[in] generator Generates the code. @param[in] writer Writes the files. None may be null.
    Cli(std::ostream& err, std::unique_ptr<IMibLoader> loader, std::unique_ptr<ICodeGenerator> generator,
        std::unique_ptr<IFileWriter> writer);

    /// @brief Parses the arguments (without the program name).
    /// @return The arguments, or std::nullopt for a usage error (unknown option, --module / --out / file missing).
    static std::optional<CliArgs> parse(const std::vector<std::string>& args);

    /// @brief The whole command: parse, load the MIBs, generate, write <out>/<base>.hpp and .cpp.
    /// @return The exit code: 0 success, 1 error (message on the error stream), 2 usage error (usage printed).
    int run(const std::vector<std::string>& args);

private:
    int usage();

    std::ostream& err_;
    std::unique_ptr<IMibLoader> loader_;
    std::unique_ptr<ICodeGenerator> generator_;
    std::unique_ptr<IFileWriter> writer_;
};

}  // namespace snmpwrap::mibgen
