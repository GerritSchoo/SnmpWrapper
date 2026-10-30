// Helpers shared by the GoogleTest suites: run a SET like the agent does, check its error, and print
// snmpwrap types in assertion messages.
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "snmpwrap/mib.hpp"

namespace snmpwrap {

// Found by GoogleTest through ADL: failed EXPECT_EQ(...) show "1.3.6.1" / "Integer: 5" instead of bytes.
inline void PrintTo(const Oid& oid, std::ostream* os) { *os << oid.str(); }
inline void PrintTo(const Value& value, std::ostream* os) { *os << value.str(); }
inline void PrintTo(const VarBind& vb, std::ostream* os) { *os << vb.oid.str() << " = " << vb.value.str(); }
inline void PrintTo(ErrorStatus status, std::ostream* os) { *os << "ErrorStatus(" << static_cast<int>(status) << ")"; }

}  // namespace snmpwrap

namespace snmpwrap::test {

/// Runs a whole SET like the agent does: prepare -> apply -> commit, or undo after a failed apply.
/// @return The SetError of whichever phase failed, std::nullopt on success.
inline std::optional<SetError> trySet(Handler& h, const std::vector<VarBind>& sets) {
    std::unique_ptr<SetTransaction> t;
    try {
        t = h.prepare(sets);
    } catch (const SetError& e) {
        return e;
    }
    try {
        t->apply();
    } catch (const SetError& e) {
        t->undo();
        return e;
    }
    t->commit();
    return std::nullopt;
}

/// SET succeeded. Use as EXPECT_TRUE(Succeeds(trySet(...))) - a failure prints the error.
inline ::testing::AssertionResult Succeeds(const std::optional<SetError>& e) {
    if (!e) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "SET failed with status " << static_cast<int>(e->status()) << " at varbind "
                                         << e->index() << ": " << e->what();
}

/// SET failed with exactly this status at this varbind (0-based). Use as EXPECT_TRUE(FailsWith(...)).
inline ::testing::AssertionResult FailsWith(const std::optional<SetError>& e, ErrorStatus status, std::size_t index) {
    if (!e) return ::testing::AssertionFailure() << "SET succeeded, expected status " << static_cast<int>(status);
    if (e->status() == status && e->index() == index) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "SET failed with status " << static_cast<int>(e->status()) << " at varbind "
                                         << e->index() << " (" << e->what() << "), expected status "
                                         << static_cast<int>(status) << " at varbind " << index;
}

/// Instance OID of a scalar.
inline Oid scalar(const Oid& object) { return object + SubId{0}; }
/// OID of a table cell: column OID + row index.
inline Oid cell(const Oid& column, const Oid& index) { return column + index; }

}  // namespace snmpwrap::test
