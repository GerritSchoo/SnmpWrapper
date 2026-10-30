// The code generator itself (tools/mibgen/generator.hpp): snakeCase(), the API generate() emits, the
// options, determinism, keyword escaping and every refusal. Whether the generated code compiles and works
// is tested by test/generated/ and the integration tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "generator.hpp"
#include "snmpwrap/mib_model.hpp"

using namespace snmpwrap;
using snmpwrap::mibgen::generate;
using snmpwrap::mibgen::Options;
using snmpwrap::mibgen::snakeCase;
using ::testing::HasSubstr;
using ::testing::Not;

namespace {

const std::string kTestMibs = SNMPWRAP_SOURCE_DIR "/test/mibs";

TEST(SnakeCase, ModuleNameToIdentifier) {
    EXPECT_EQ(snakeCase("SNMPWRAPPER-TEST-MIB"), "snmpwrapper_test_mib");
    EXPECT_EQ(snakeCase("My-App-MIB"), "my_app_mib");
    EXPECT_EQ(snakeCase("RFC1213-MIB"), "rfc1213_mib");
    EXPECT_EQ(snakeCase("1ST-MIB"), "mib_1st_mib");  // must not start with a digit
    EXPECT_EQ(snakeCase(""), "mib_");
}

/// All test MIBs in one model, loaded once.
class GeneratorTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        model_ = std::make_unique<MibModel>(MibModel::load(
            {kTestMibs + "/SNMPWRAPPER-TEST-MIB.txt", kTestMibs + "/NESTED-TEST-MIB.txt", kTestMibs + "/NESTED-GROUP-MIB.txt",
             kTestMibs + "/mibgen/KEYWORD-MIB.txt", kTestMibs + "/mibgen/MEMBER-CLASH-MIB.txt", kTestMibs + "/mibgen/SPLIT-MIB.txt"},
            {kTestMibs}));
    }
    static void TearDownTestSuite() { model_.reset(); }

    static mibgen::Output gen(const std::string& module, const std::string& baseName = "", const std::string& rootName = "") {
        Options o;
        o.module = module;
        o.baseName = baseName;
        o.rootName = rootName;
        o.sourceInfo = "test.txt";
        return generate(*model_, o);
    }
    /// The message of the Error generate() throws, or "" if it does not throw.
    static std::string refusal(const std::string& module, const std::string& rootName = "") {
        try {
            gen(module, "", rootName);
        } catch (const Error& e) {
            return e.what();
        }
        return "";
    }

    static std::unique_ptr<MibModel> model_;
};

std::unique_ptr<MibModel> GeneratorTest::model_;

// --- what is generated --------------------------------------------------------------------------

TEST_F(GeneratorTest, HeaderDeclaresTheWholeApi) {
    const std::string h = gen("SNMPWRAPPER-TEST-MIB").header;
    EXPECT_THAT(h, HasSubstr("namespace snmpwrapper_test_mib {"));
    EXPECT_THAT(h, HasSubstr("inline const snmpwrap::Oid root{1, 3, 6, 1, 4, 1, 99999};"));
    EXPECT_THAT(h, HasSubstr("struct Data {"));
    EXPECT_THAT(h, HasSubstr("class DataAgent {"));
    EXPECT_THAT(h, HasSubstr("class Remote {"));
    EXPECT_THAT(h, HasSubstr("class Instrumentation {"));
    EXPECT_THAT(h, HasSubstr("enum class Object {"));
}

TEST_F(GeneratorTest, EnumsIndexesAndRowsPerTable) {
    const std::string h = gen("SNMPWRAPPER-TEST-MIB").header;
    EXPECT_THAT(h, HasSubstr("enum class SwtEntryStatus : std::int32_t {"));
    EXPECT_THAT(h, HasSubstr("enum class SwtConnState : std::int32_t {"));
    for (const char* table : {"SwtEntry", "SwtRowEntry", "SwtConnEntry", "SwtBigEntry"}) {
        EXPECT_THAT(h, HasSubstr(std::string("struct ") + table + "Index {")) << table;
        EXPECT_THAT(h, HasSubstr(std::string("struct ") + table + " {")) << table;
        EXPECT_THAT(h, HasSubstr(std::string("class ") + table + "RemoteTable {")) << table;
    }
}

TEST_F(GeneratorTest, RowStatusTablesGetValuesForCreation) {
    const std::string h = gen("SNMPWRAPPER-TEST-MIB").header;
    EXPECT_THAT(h, HasSubstr("struct SwtRowEntryValues {"));      // swtRowTable has a RowStatus
    EXPECT_THAT(h, Not(HasSubstr("struct SwtEntryValues {")));    // swtTable has none
}

TEST_F(GeneratorTest, NotificationsGetASendFunctionAndAType) {
    const std::string h = gen("SNMPWRAPPER-TEST-MIB").header;
    EXPECT_THAT(h, HasSubstr("void sendSwtAlarm(snmpwrap::NotificationSender& agent, const std::string& swtName, std::int32_t swtLimit);"));
    EXPECT_THAT(h, HasSubstr("struct SwtAlarm {"));
}

TEST_F(GeneratorTest, NestedGroupsBecomeNestedStructs) {
    const std::string h = gen("NESTED-TEST-MIB").header;
    EXPECT_THAT(h, HasSubstr("namespace nested_test_mib {"));
    EXPECT_THAT(h, HasSubstr("struct AttitudeGroup {"));
    EXPECT_THAT(h, HasSubstr("struct NavigationGroup {"));
}

TEST_F(GeneratorTest, SourceImplementsTheHeader) {
    const std::string c = gen("SNMPWRAPPER-TEST-MIB").source;
    EXPECT_THAT(c, HasSubstr("#include \"snmpwrapper_test_mib.hpp\""));
    EXPECT_THAT(c, HasSubstr("snmpwrap::Mib& registerMib(snmpwrap::Agent& agent, Instrumentation& impl) {"));
    EXPECT_THAT(c, HasSubstr("void sendSwtAlarm(snmpwrap::NotificationSender& agent"));
}

// --- options ------------------------------------------------------------------------------------

TEST_F(GeneratorTest, BaseNameIsNamespaceAndIncludeName) {
    const auto out = gen("SNMPWRAPPER-TEST-MIB", "custom_name");
    EXPECT_THAT(out.header, HasSubstr("namespace custom_name {"));
    EXPECT_THAT(out.header, Not(HasSubstr("namespace snmpwrapper_test_mib {")));
    EXPECT_THAT(out.source, HasSubstr("#include \"custom_name.hpp\""));
}

TEST_F(GeneratorTest, RootNameSetsTheRegistrationRoot) {
    EXPECT_THAT(gen("SNMPWRAPPER-TEST-MIB", "", "enterprises").header, HasSubstr("inline const snmpwrap::Oid root{1, 3, 6, 1, 4, 1};"));
}

TEST_F(GeneratorTest, SourceInfoIsNamedInTheFirstLine) {
    const std::string h = gen("SNMPWRAPPER-TEST-MIB").header;
    EXPECT_EQ(h.substr(0, h.find('\n')), "// Generated by snmpwrap-mibgen from test.txt (module SNMPWRAPPER-TEST-MIB).");
}

TEST_F(GeneratorTest, OutputIsDeterministic) {  // the build only rewrites files whose content changed
    for (const char* module : {"SNMPWRAPPER-TEST-MIB", "NESTED-TEST-MIB", "NESTED-GROUP-MIB", "KEYWORD-MIB"}) {
        const auto a = gen(module), b = gen(module);
        EXPECT_EQ(a.header, b.header) << module;
        EXPECT_EQ(a.source, b.source) << module;
    }
}

// --- names that are no valid C++ identifiers ----------------------------------------------------

TEST_F(GeneratorTest, KeywordsGetATrailingUnderscore) {
    const std::string h = gen("KEYWORD-MIB").header;
    EXPECT_THAT(h, HasSubstr("inline const snmpwrap::Oid default_{"));
    EXPECT_THAT(h, HasSubstr("inline const snmpwrap::Oid class_{"));
    EXPECT_THAT(h, HasSubstr("inline const snmpwrap::Oid register_{"));
    EXPECT_THAT(h, HasSubstr("inline const snmpwrap::Oid status_{"));  // a name the generated code uses itself
    EXPECT_THAT(h, HasSubstr("delete_ = 1,"));                         // enum label
    EXPECT_THAT(h, HasSubstr("true_ = 3,"));
    EXPECT_THAT(h, HasSubstr("struct NamespaceGroup {"));               // type names start upper case: no clash
    EXPECT_THAT(h, Not(HasSubstr(" default{")));
}

TEST_F(GeneratorTest, ValidateMemberDoesNotHideDataValidate) {
    const std::string h = gen("KEYWORD-MIB").header;
    EXPECT_THAT(h, HasSubstr("std::int32_t validate_{};"));
}

// --- refusals -----------------------------------------------------------------------------------

TEST_F(GeneratorTest, UnknownModuleIsRefused) {
    EXPECT_THAT(refusal("NO-SUCH-MIB"), HasSubstr("module NO-SUCH-MIB is not loaded or has no objects"));
}

TEST_F(GeneratorTest, TwoNamesForTheSameMemberAreRefused) {
    EXPECT_THAT(refusal("MEMBER-CLASH-MIB"), HasSubstr("both map to the C++ member 'node2'"));
}

TEST_F(GeneratorTest, ObjectsWithoutCommonPrefixNeedARoot) {
    EXPECT_THAT(refusal("SPLIT-MIB"), HasSubstr("objects of SPLIT-MIB have no common OID prefix; use --root"));
}

TEST_F(GeneratorTest, ObjectsOutsideTheRootAreRefused) {
    EXPECT_THAT(refusal("SPLIT-MIB", "sysDescr"), HasSubstr("object 'splitIso' is not below 1.3.6.1.2.1.1.1"));
}

TEST_F(GeneratorTest, UnknownRootNameIsRefused) {
    EXPECT_THAT(refusal("SNMPWRAPPER-TEST-MIB", "noSuchNode"), HasSubstr("noSuchNode"));
}

}  // namespace
