// gmock mock of IMibBuilder (IMibBuilder.hpp).
#pragma once

#include <gmock/gmock.h>

#include <functional>

#include "snmpwrap/IMibBuilder.hpp"

namespace snmpwrap::test {

class MockMibBuilder : public IMibBuilder {
public:
    MOCK_METHOD(const Oid&, root, (), (const, noexcept, override));
    MOCK_METHOD(IMibBuilder&, scalar, (const Oid&, ScalarDef), (override));
    MOCK_METHOD(IMibBuilder&, table, (const Oid&, TableDef), (override));
    MOCK_METHOD(void, onRequestEnd, (std::function<void(bool)>), (override));
    MOCK_METHOD(void, onUndo, (std::function<void()>), (override));
    using IMibBuilder::scalar;
    using IMibBuilder::table;
};

}  // namespace snmpwrap::test
