// Unit tests of Cli.hpp / Cli.cpp: Cli::parse(), Cli::run() isolated with a mocked IMibLoader, ICodeGenerator and
// IFileWriter, and the real parts ChangedFileWriter (in a temporary directory) and MibModelLoader.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Cli.hpp"
#include "mibgen/IFileWriterMock.hpp"
#include "mibgen/IMibLoaderMock.hpp"
#include "mibgen/ICodeGeneratorMock.hpp"
#include "snmpwrap/Error.hpp"
#include "snmpwrap/IMibInfoMock.hpp"

using namespace snmpwrap;
using namespace snmpwrap::mibgen;
using namespace snmpwrap::mibgen::test;
using snmpwrap::test::MockMibInfo;
using ::testing::_;
using ::testing::ByMove;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::Throw;

namespace {

const std::string kTestMibs = SNMPWRAP_SOURCE_DIR "/test/mibs";

using Args = std::vector<std::string>;

// =================================================================================================
// Cli::parse
// =================================================================================================

TEST(CliParse, ReadsEveryOption) {
    const auto a = Cli::parse({"--module", "M-MIB", "--out", "gen", "--name", "base", "--root", "node", "--mib-dir", "d1",
                               "--mib-dir", "d2", "a/M-MIB.txt", "b/X-MIB.txt"});
    ASSERT_TRUE(a);
    EXPECT_EQ(a->options.module, "M-MIB");
    EXPECT_EQ(a->outDir, "gen");
    EXPECT_EQ(a->options.baseName, "base");
    EXPECT_EQ(a->options.rootName, "node");
    EXPECT_EQ(a->mibDirs, (Args{"d1", "d2"}));
    EXPECT_EQ(a->files, (Args{"a/M-MIB.txt", "b/X-MIB.txt"}));
}

TEST(CliParse, SourceInfoNamesTheFilesWithoutTheirDirectory) {
    const auto a = Cli::parse({"--module", "M-MIB", "--out", "gen", "a/M-MIB.txt", "b/X-MIB.txt"});
    ASSERT_TRUE(a);
    EXPECT_EQ(a->options.sourceInfo, "M-MIB.txt, X-MIB.txt");
}

TEST(CliParse, OptionalOptionsMayBeMissing) {
    const auto a = Cli::parse({"--module", "M-MIB", "--out", "gen", "M-MIB.txt"});
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->options.baseName.empty());
    EXPECT_TRUE(a->options.rootName.empty());
    EXPECT_TRUE(a->mibDirs.empty());
}

TEST(CliParse, OrderDoesNotMatter) {
    const auto a = Cli::parse({"M-MIB.txt", "--out", "gen", "--module", "M-MIB"});
    ASSERT_TRUE(a);
    EXPECT_EQ(a->options.module, "M-MIB");
}

class CliParseUsageError : public ::testing::TestWithParam<Args> {};

TEST_P(CliParseUsageError, IsRefused) { EXPECT_FALSE(Cli::parse(GetParam())); }

INSTANTIATE_TEST_SUITE_P(Cli, CliParseUsageError,
                         ::testing::Values(Args{},                                                    // nothing
                                           Args{"--out", "gen", "M-MIB.txt"},                         // no --module
                                           Args{"--module", "M-MIB", "M-MIB.txt"},                    // no --out
                                           Args{"--module", "M-MIB", "--out", "gen"},                 // no file
                                           Args{"--module", "M-MIB", "--out", "gen", "--bogus", "f"},  // unknown option
                                           Args{"--module", "M-MIB", "--out"},                        // value missing
                                           Args{"-x", "--module", "M-MIB", "--out", "gen", "f"}));

// =================================================================================================
// Cli::run with mocked parts
// =================================================================================================

class CliRunTest : public ::testing::Test {
protected:
    CliRunTest() {
        auto l = std::make_unique<StrictMock<MockMibLoader>>();
        auto g = std::make_unique<StrictMock<MockCodeGenerator>>();
        auto w = std::make_unique<StrictMock<MockFileWriter>>();
        loader = l.get();
        generator = g.get();
        writer = w.get();
        cli = std::make_unique<Cli>(err, std::move(l), std::move(g), std::move(w));
    }

    /// The loader returns a (mocked) model once.
    void expectLoad(const Args& files, const Args& dirs) {
        EXPECT_CALL(*loader, load(files, dirs)).WillOnce(Return(ByMove(std::unique_ptr<IMibInfo>(std::make_unique<MockMibInfo>()))));
    }

    std::ostringstream err;
    StrictMock<MockMibLoader>* loader = nullptr;
    StrictMock<MockCodeGenerator>* generator = nullptr;
    StrictMock<MockFileWriter>* writer = nullptr;
    std::unique_ptr<Cli> cli;
};

TEST_F(CliRunTest, LoadsGeneratesAndWritesHeaderAndSource) {
    expectLoad({"x/MY-MIB.txt"}, {"dir"});
    EXPECT_CALL(*generator, generate(_, ::testing::AllOf(Field(&Options::module, "MY-MIB"), Field(&Options::sourceInfo, "MY-MIB.txt"))))
        .WillOnce(Return(Output{"HEADER", "SOURCE"}));
    EXPECT_CALL(*writer, write(std::filesystem::path("out") / "my_mib.hpp", "HEADER")).WillOnce(Return(true));
    EXPECT_CALL(*writer, write(std::filesystem::path("out") / "my_mib.cpp", "SOURCE")).WillOnce(Return(true));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--out", "out", "--mib-dir", "dir", "x/MY-MIB.txt"}), 0);
    EXPECT_TRUE(err.str().empty());
}

TEST_F(CliRunTest, NameSetsTheFileNames) {
    expectLoad({"MY-MIB.txt"}, {});
    EXPECT_CALL(*generator, generate(_, Field(&Options::baseName, "other"))).WillOnce(Return(Output{"H", "S"}));
    EXPECT_CALL(*writer, write(std::filesystem::path("out") / "other.hpp", "H")).WillOnce(Return(true));
    EXPECT_CALL(*writer, write(std::filesystem::path("out") / "other.cpp", "S")).WillOnce(Return(true));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--name", "other", "--out", "out", "MY-MIB.txt"}), 0);
}

TEST_F(CliRunTest, RootIsPassedToTheGenerator) {
    expectLoad({"MY-MIB.txt"}, {});
    EXPECT_CALL(*generator, generate(_, Field(&Options::rootName, "enterprises"))).WillOnce(Return(Output{}));
    EXPECT_CALL(*writer, write(_, _)).Times(2).WillRepeatedly(Return(true));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--root", "enterprises", "--out", "out", "MY-MIB.txt"}), 0);
}

TEST_F(CliRunTest, UsageErrorIsExitCode2AndTouchesNothing) {
    EXPECT_EQ(cli->run({"--module", "MY-MIB"}), 2);  // StrictMocks: no call
    EXPECT_THAT(err.str(), HasSubstr("usage: snmpwrap-mibgen"));
}

TEST_F(CliRunTest, LoadErrorIsExitCode1WithTheMessage) {
    EXPECT_CALL(*loader, load(_, _)).WillOnce(Throw(Error("loading MIB failed: syntax")));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--out", "out", "MY-MIB.txt"}), 1);
    EXPECT_THAT(err.str(), HasSubstr("snmpwrap-mibgen: loading MIB failed: syntax"));
}

TEST_F(CliRunTest, GeneratorRefusalIsExitCode1AndWritesNothing) {
    expectLoad({"MY-MIB.txt"}, {});
    EXPECT_CALL(*generator, generate(_, _)).WillOnce(Throw(Error("both map to the C++ member 'x'")));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--out", "out", "MY-MIB.txt"}), 1);
    EXPECT_THAT(err.str(), HasSubstr("both map to the C++ member"));
}

TEST_F(CliRunTest, WriteFailureIsExitCode1) {
    expectLoad({"MY-MIB.txt"}, {});
    EXPECT_CALL(*generator, generate(_, _)).WillOnce(Return(Output{"H", "S"}));
    EXPECT_CALL(*writer, write(_, "H")).WillOnce(Return(false));
    EXPECT_EQ(cli->run({"--module", "MY-MIB", "--out", "out", "MY-MIB.txt"}), 1);
    EXPECT_THAT(err.str(), HasSubstr("cannot write to out"));
}

TEST(Cli, NeedsAllItsParts) {
    std::ostringstream err;
    EXPECT_THROW(Cli(err, nullptr, std::make_unique<MockCodeGenerator>(), std::make_unique<MockFileWriter>()), Error);
    EXPECT_THROW(Cli(err, std::make_unique<MockMibLoader>(), nullptr, std::make_unique<MockFileWriter>()), Error);
    EXPECT_THROW(Cli(err, std::make_unique<MockMibLoader>(), std::make_unique<MockCodeGenerator>(), nullptr), Error);
}

// =================================================================================================
// ChangedFileWriter (real files, temporary directory)
// =================================================================================================

class ChangedFileWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = std::filesystem::path(::testing::TempDir()) /
              ("snmpwrap_cli_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir);
    }
    void TearDown() override { std::filesystem::remove_all(dir); }
    static std::string read(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    std::filesystem::path dir;
    ChangedFileWriter writer;
};

TEST_F(ChangedFileWriterTest, CreatesTheDirectoryAndTheFile) {
    ASSERT_TRUE(writer.write(dir / "sub" / "a.hpp", "content"));
    EXPECT_EQ(read(dir / "sub" / "a.hpp"), "content");
}

TEST_F(ChangedFileWriterTest, UnchangedContentIsNotRewritten) {  // keeps builds incremental
    ASSERT_TRUE(writer.write(dir / "a.hpp", "content"));
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::filesystem::last_write_time(dir / "a.hpp", old);
    ASSERT_TRUE(writer.write(dir / "a.hpp", "content"));
    EXPECT_EQ(std::filesystem::last_write_time(dir / "a.hpp"), old);
}

TEST_F(ChangedFileWriterTest, ChangedContentIsRewritten) {
    ASSERT_TRUE(writer.write(dir / "a.hpp", "old"));
    ASSERT_TRUE(writer.write(dir / "a.hpp", "new"));
    EXPECT_EQ(read(dir / "a.hpp"), "new");
}

TEST_F(ChangedFileWriterTest, BinaryContentIsKept) {
    const std::string bytes("a\0b\r\n", 5);
    ASSERT_TRUE(writer.write(dir / "a.bin", bytes));
    EXPECT_EQ(read(dir / "a.bin"), bytes);
}

TEST_F(ChangedFileWriterTest, AnUnwritablePathIsFalse) {
    ASSERT_TRUE(writer.write(dir / "file", "x"));
    EXPECT_FALSE(writer.write(dir / "file" / "below-a-file.hpp", "x"));  // "file" is no directory
}

// =================================================================================================
// MibModelLoader (Net-SNMP's parser)
// =================================================================================================

TEST(MibModelLoader, LoadsTheFilesAndTheirImports) {
    const auto model = MibModelLoader().load({kTestMibs + "/NESTED-GROUP-MIB.txt"}, {kTestMibs});
    ASSERT_TRUE(model);
    EXPECT_EQ(model->node("boatRoll").module, "NESTED-GROUP-MIB");
}

TEST(MibModelLoader, BrokenFileIsAnError) {
    EXPECT_THROW(MibModelLoader().load({kTestMibs + "/broken/BROKEN-LOADER-MIB.txt"}, {}), Error);
}

}  // namespace
