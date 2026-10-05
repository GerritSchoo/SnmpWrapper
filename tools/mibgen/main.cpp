// snmpwrap-mibgen – generates typed C++17 code (agent interface, registration, notifications,
// typed client) for one MIB module.
//
//   snmpwrap-mibgen --module <MODULE> --out <dir> [--name <base>] [--root <node>] [--mib-dir <dir>]... <mib-file>...
//
// Writes <dir>/<base>.hpp and <dir>/<base>.cpp (base defaults to the module name in snake_case).
// Files are only rewritten when their content changes, so builds stay incremental.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "generator.hpp"

namespace {

int usage() {
    std::cerr << "usage: snmpwrap-mibgen --module <MODULE> --out <dir> [--name <base>] [--root <node>]\n"
                 "                       [--mib-dir <dir>]... <mib-file>...\n";
    return 2;
}

bool writeIfChanged(const std::filesystem::path& path, const std::string& content) {
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

}  // namespace

int main(int argc, char** argv) {
    snmpwrap::mibgen::Options opt;
    std::string outDir;
    std::vector<std::string> files, dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--module") opt.module = next();
        else if (a == "--out") outDir = next();
        else if (a == "--name") opt.baseName = next();
        else if (a == "--root") opt.rootName = next();
        else if (a == "--mib-dir") dirs.push_back(next());
        else if (!a.empty() && a[0] == '-') return usage();
        else files.push_back(a);
    }
    if (opt.module.empty() || outDir.empty() || files.empty()) return usage();

    try {
        for (const auto& f : files) {
            if (!opt.sourceInfo.empty()) opt.sourceInfo += ", ";
            opt.sourceInfo += std::filesystem::path(f).filename().string();
        }
        const snmpwrap::MibModel model = snmpwrap::MibModel::load(files, dirs);
        const auto out = snmpwrap::mibgen::generate(model, opt);
        const std::string base = opt.baseName.empty() ? snmpwrap::mibgen::snakeCase(opt.module) : opt.baseName;
        std::filesystem::create_directories(outDir);
        const auto dir = std::filesystem::path(outDir);
        if (!writeIfChanged(dir / (base + ".hpp"), out.header) || !writeIfChanged(dir / (base + ".cpp"), out.source)) {
            std::cerr << "snmpwrap-mibgen: cannot write to " << outDir << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "snmpwrap-mibgen: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
