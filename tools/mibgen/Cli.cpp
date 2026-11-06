#include "Cli.hpp"

#include <fstream>
#include <sstream>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/MibModel.hpp"

namespace snmpwrap::mibgen {

// --- MibModelLoader / ChangedFileWriter ----------------------------------------------------------

std::unique_ptr<IMibInfo> MibModelLoader::load(const std::vector<std::string>& files, const std::vector<std::string>& mibDirs) const {
    return std::make_unique<MibModel>(MibModel::load(files, mibDirs));
}

bool ChangedFileWriter::write(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream old;
        old << in.rdbuf();
        if (old.str() == content) return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return static_cast<bool>(out);
}

// --- Cli ------------------------------------------------------------------------------------------

Cli::Cli(std::ostream& err)
    : Cli(err, std::make_unique<MibModelLoader>(), std::make_unique<Generator>(), std::make_unique<ChangedFileWriter>()) {}

Cli::Cli(std::ostream& err, std::unique_ptr<IMibLoader> loader, std::unique_ptr<ICodeGenerator> generator,
         std::unique_ptr<IFileWriter> writer)
    : err_(err), loader_(std::move(loader)), generator_(std::move(generator)), writer_(std::move(writer)) {
    if (!loader_ || !generator_ || !writer_) throw Error("Cli: loader, generator and writer are required");
}

int Cli::usage() {
    err_ << "usage: snmpwrap-mibgen --module <MODULE> --out <dir> [--name <base>] [--root <node>]\n"
            "                       [--mib-dir <dir>]... <mib-file>...\n";
    return 2;
}

std::optional<CliArgs> Cli::parse(const std::vector<std::string>& args) {
    CliArgs a;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
        if (arg == "--module") a.options.module = next();
        else if (arg == "--out") a.outDir = next();
        else if (arg == "--name") a.options.baseName = next();
        else if (arg == "--root") a.options.rootName = next();
        else if (arg == "--mib-dir") a.mibDirs.push_back(next());
        else if (!arg.empty() && arg[0] == '-') return std::nullopt;
        else a.files.push_back(arg);
    }
    if (a.options.module.empty() || a.outDir.empty() || a.files.empty()) return std::nullopt;
    for (const auto& f : a.files) {
        if (!a.options.sourceInfo.empty()) a.options.sourceInfo += ", ";
        a.options.sourceInfo += std::filesystem::path(f).filename().string();
    }
    return a;
}

int Cli::run(const std::vector<std::string>& args) {
    const auto a = parse(args);
    if (!a) return usage();
    try {
        const std::unique_ptr<IMibInfo> model = loader_->load(a->files, a->mibDirs);
        const Output out = generator_->generate(*model, a->options);
        const std::string base = a->options.baseName.empty() ? Generator::snakeCase(a->options.module) : a->options.baseName;
        const std::filesystem::path dir(a->outDir);
        if (!writer_->write(dir / (base + ".hpp"), out.header) || !writer_->write(dir / (base + ".cpp"), out.source)) {
            err_ << "snmpwrap-mibgen: cannot write to " << a->outDir << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        err_ << "snmpwrap-mibgen: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace snmpwrap::mibgen
