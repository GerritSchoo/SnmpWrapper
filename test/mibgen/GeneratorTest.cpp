// Unit tests of Generator.hpp / Generator.cpp: Generator::snakeCase(), the API Generator::generate() emits, the
// options, determinism, keyword escaping and every refusal - with MIBs read by MibModel, and isolated with a mocked
// IMibInfo. Whether the generated code compiles and works is tested by test/generated/ and the integration tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Generator.hpp"
#include "snmpwrap/MibModel.hpp"
#include "snmpwrap/IMibInfoMock.hpp"

using namespace snmpwrap;
using snmpwrap::mibgen::Generator;
using snmpwrap::mibgen::Options;
using snmpwrap::test::MockMibInfo;
using ::testing::_;
using ::testing::Return;
using ::testing::HasSubstr;
using ::testing::Not;

namespace {

const std::string kTestMibs = SNMPWRAP_SOURCE_DIR "/test/mibs";

struct SnakeCaseCase {
    const char* module;
    const char* name;
};

class GeneratorSnakeCase : public ::testing::TestWithParam<SnakeCaseCase> {};

TEST_P(GeneratorSnakeCase, ModuleNameToIdentifier) { EXPECT_EQ(Generator::snakeCase(GetParam().module), GetParam().name); }

INSTANTIATE_TEST_SUITE_P(Generator, GeneratorSnakeCase,
                         ::testing::Values(SnakeCaseCase{"SNMPWRAPPER-TEST-MIB", "snmpwrapper_test_mib"},
                                           SnakeCaseCase{"My-App-MIB", "my_app_mib"},
                                           SnakeCaseCase{"RFC1213-MIB", "rfc1213_mib"},
                                           SnakeCaseCase{"1ST-MIB", "mib_1st_mib"},  // must not start with a digit
                                           SnakeCaseCase{"A.B C", "a_b_c"},
                                           SnakeCaseCase{"", "mib_"}));

// --- isolated: a mocked IMibInfo -------------------------------------------------------------------

TEST(GeneratorWithMockedMib, AModuleWithoutObjectsIsRefused) {
    MockMibInfo mib;
    EXPECT_CALL(mib, objects(std::string_view("EMPTY-MIB"))).WillOnce(Return(std::vector<const snmpwrap::MibNode*>{}));
    Options o;
    o.module = "EMPTY-MIB";
    try {
        Generator().generate(mib, o);
        FAIL() << "accepted";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), HasSubstr("module EMPTY-MIB is not loaded or has no objects"));
    }
}

TEST(GeneratorWithMockedMib, AModuleWithOnlyANotificationNeedsNoRoot) {
    snmpwrap::MibNode identity, trap;
    identity.name = "onlyTrapMIB";
    identity.module = "ONLY-TRAP-MIB";
    identity.oid = Oid({1, 3, 6, 1, 4, 1, 99999, 330});
    trap.name = "somethingHappened";
    trap.module = "ONLY-TRAP-MIB";
    trap.kind = snmpwrap::MibNodeKind::Notification;
    trap.oid = identity.oid + Oid({0, 1});
    ::testing::NiceMock<MockMibInfo> mib;
    ON_CALL(mib, objects(_)).WillByDefault(Return(std::vector<const snmpwrap::MibNode*>{&identity, &trap}));
    Options o;
    o.module = "ONLY-TRAP-MIB";
    const auto out = Generator().generate(mib, o);
    EXPECT_THAT(out.header, HasSubstr("void sendSomethingHappened(snmpwrap::INotificationSender& agent);"));
    EXPECT_THAT(out.header, Not(HasSubstr("class DataAgent")));  // nothing to serve
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
        return Generator().generate(*model_, o);
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
    EXPECT_THAT(h, HasSubstr("class IInstrumentation {"));
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
    EXPECT_THAT(h, HasSubstr("void sendSwtAlarm(snmpwrap::INotificationSender& agent, const std::string& swtName, std::int32_t swtLimit);"));
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
    EXPECT_THAT(c, HasSubstr("std::shared_ptr<snmpwrap::Mib> registerMib(snmpwrap::IRegistry& agent, std::shared_ptr<IInstrumentation> impl) {"));
    EXPECT_THAT(c, HasSubstr("void sendSwtAlarm(snmpwrap::INotificationSender& agent"));
}

// --- options ------------------------------------------------------------------------------------

TEST_F(GeneratorTest, BaseNameIsNamespaceAndIncludeName) {
    const auto out = gen("SNMPWRAPPER-TEST-MIB", "custom_name");
    EXPECT_THAT(out.header, HasSubstr("namespace custom_name {"));
    EXPECT_THAT(out.header, Not(HasSubstr("namespace snmpwrapper_test_mib {")));
    EXPECT_THAT(out.source, HasSubstr("#include \"custom_name.hpp\""));
}

TEST_F(GeneratorTest, RootNameSetsTheRegistrationRoot) {
    EXPECT_THAT(gen("SNMPWRAPPER-TEST-MIB", "", "enterprises").header,
                HasSubstr("inline const snmpwrap::Oid root{1, 3, 6, 1, 4, 1};  ///< registration root (enterprises)"));
    EXPECT_THAT(gen("SNMPWRAPPER-TEST-MIB").header, HasSubstr("///< registration root (common prefix of all objects)"));
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
    EXPECT_THAT(refusal("SPLIT-MIB", "sysDescr"),
                HasSubstr("object 'splitIso' (1.3.6.1.4.1.99999.322.1) is not below the root sysDescr (1.3.6.1.2.1.1.1)"));
}

TEST_F(GeneratorTest, RootInsideTheModuleIsRefused) {
    // swtScalars (99999.1) holds the scalars, but the tables are at 99999.2 and up: the agent could not serve them
    // and Remote::read() would silently miss them
    EXPECT_THAT(refusal("SNMPWRAPPER-TEST-MIB", "swtScalars"),
                HasSubstr("object 'swtTable' (1.3.6.1.4.1.99999.2) is not below the root swtScalars (1.3.6.1.4.1.99999.1)"));
}

TEST_F(GeneratorTest, RootThatIsAnObjectItselfIsRefused) {  // the root must lie strictly above every object
    EXPECT_THAT(refusal("SNMPWRAPPER-TEST-MIB", "swtLimit"), HasSubstr("is not below the root swtLimit"));
}

TEST_F(GeneratorTest, UnknownRootNameIsRefused) {
    EXPECT_THAT(refusal("SNMPWRAPPER-TEST-MIB", "noSuchNode"), HasSubstr("noSuchNode"));
}

}  // namespace
