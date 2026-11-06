// Unit tests of Error.hpp: the exception classes and the ErrorStatus numbers of RFC 3416.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "snmpwrap/Error.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;

namespace {

// --- ErrorStatus: the numbers on the wire ---------------------------------------------------------

struct Numbered {
    ErrorStatus status;
    int number;
};

class ErrorStatusNumber : public ::testing::TestWithParam<Numbered> {};

TEST_P(ErrorStatusNumber, IsTheRfc3416Value) { EXPECT_EQ(static_cast<int>(GetParam().status), GetParam().number); }

INSTANTIATE_TEST_SUITE_P(
    Error, ErrorStatusNumber,
    ::testing::Values(Numbered{ErrorStatus::NoError, 0}, Numbered{ErrorStatus::TooBig, 1}, Numbered{ErrorStatus::NoSuchName, 2},
                      Numbered{ErrorStatus::BadValue, 3}, Numbered{ErrorStatus::ReadOnly, 4}, Numbered{ErrorStatus::GenErr, 5},
                      Numbered{ErrorStatus::NoAccess, 6}, Numbered{ErrorStatus::WrongType, 7},
                      Numbered{ErrorStatus::WrongLength, 8}, Numbered{ErrorStatus::WrongEncoding, 9},
                      Numbered{ErrorStatus::WrongValue, 10}, Numbered{ErrorStatus::NoCreation, 11},
                      Numbered{ErrorStatus::InconsistentValue, 12}, Numbered{ErrorStatus::ResourceUnavailable, 13},
                      Numbered{ErrorStatus::CommitFailed, 14}, Numbered{ErrorStatus::UndoFailed, 15},
                      Numbered{ErrorStatus::AuthorizationError, 16}, Numbered{ErrorStatus::NotWritable, 17},
                      Numbered{ErrorStatus::InconsistentName, 18}));

// --- Error ----------------------------------------------------------------------------------------

TEST(Error, IsARuntimeErrorWithItsText) {
    const Error e("broken");
    EXPECT_STREQ(e.what(), "broken");
    EXPECT_THROW(throw Error("x"), std::runtime_error);
}

// --- SetError -------------------------------------------------------------------------------------

TEST(SetError, DefaultsToAnUnknownIndexAndAGeneralText) {
    const SetError e(ErrorStatus::WrongValue);
    EXPECT_EQ(e.status(), ErrorStatus::WrongValue);
    EXPECT_EQ(e.index(), SetError::kUnknownIndex);
    EXPECT_STREQ(e.what(), "SNMP SET rejected");
}

TEST(SetError, KeepsTextAndIndex) {
    const SetError e(ErrorStatus::NotWritable, "read-only", 2);
    EXPECT_EQ(e.status(), ErrorStatus::NotWritable);
    EXPECT_EQ(e.index(), 2u);
    EXPECT_STREQ(e.what(), "read-only");
}

TEST(SetError, IsAnError) { EXPECT_THROW(throw SetError(ErrorStatus::GenErr), Error); }

// --- ResponseError --------------------------------------------------------------------------------

TEST(ResponseError, KeepsStatusIndexAndText) {
    const ResponseError e(ErrorStatus::NoSuchName, 3, "agent returned error");
    EXPECT_EQ(e.status(), ErrorStatus::NoSuchName);
    EXPECT_EQ(e.index(), 3);
    EXPECT_STREQ(e.what(), "agent returned error");
}

TEST(ResponseError, IsAnError) { EXPECT_THROW(throw ResponseError(ErrorStatus::GenErr, 0, "x"), Error); }

// --- TransportError -------------------------------------------------------------------------------

TEST(TransportError, IsAnErrorWithItsText) {
    EXPECT_THROW(throw TransportError("timeout"), Error);
    EXPECT_STREQ(TransportError("timeout").what(), "timeout");
}

TEST(TransportError, IsNoResponseOrSetError) {  // callers tell "no answer" from "refused" by the type
    try {
        throw TransportError("timeout");
    } catch (const ResponseError&) {
        FAIL() << "caught as ResponseError";
    } catch (const SetError&) {
        FAIL() << "caught as SetError";
    } catch (const TransportError&) {
        SUCCEED();
    }
}

}  // namespace
